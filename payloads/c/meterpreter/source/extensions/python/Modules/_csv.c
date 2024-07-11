/* csv module */

/*

This module provides the low-level underpinnings of a CSV reading/writing
module.  Users should not use this module directly, but import the csv.py
module instead.

**** For people modifying this code, please note that as of this writing
**** (2003-03-23), it is intended that this code should work with Python
**** 2.2.

*/

#define MODULE_VERSION "1.0"

#include "Python.h"
#include "structmember.h"


/* begin 2.2 compatibility macros */
#ifndef PyDoc_STRVAR
/* Define macros for inline documentation. */
#define PyDoc_VAR(name) static char name[]
#define PyDoc_STRVAR(name,str) PyDoc_VAR(name) = PyDoc_STR(str)
#ifdef WITH_DOC_STRINGS
#define PyDoc_STR(str) str
#else
#define PyDoc_STR(str) ""
#endif
#endif /* ifndef PyDoc_STRVAR */

#ifndef PyMODINIT_FUNC
#       if defined(__cplusplus)
#               define PyMODINIT_FUNC extern "C" void
#       else /* __cplusplus */
#               define PyMODINIT_FUNC void
#       endif /* __cplusplus */
#endif

#ifndef Py_CLEAR
#define Py_CLEAR(op)                                            \
    do {                                                        \
        if (op) {                                               \
            PyObject *tmp = (PyObject *)(op);                   \
            (op) = NULL;                                        \
            Py_DECREF(tmp);                                     \
        }                                                       \
    } while (0)
#endif
#ifndef Py_VISIT
#define Py_VISIT(op)                                                    \
    do {                                                                \
        if (op) {                                                       \
            int vret = visit((PyObject *)(op), arg);                    \
            if (vret)                                                   \
                return vret;                                            \
        }                                                               \
    } while (0)
#endif

/* end 2.2 compatibility macros */

#define IS_BASESTRING(o) \
    PyObject_TypeCheck(o, &PyBaseString_Type)

static PyObject *error_obj;     /* CSV exception */
static PyObject *dialects;      /* Dialect registry */
static long field_limit = 128 * 1024;   /* max parsed field size */

typedef enum {
    START_RECORD, START_FIELD, ESCAPED_CHAR, IN_FIELD,
    IN_QUOTED_FIELD, ESCAPE_IN_QUOTED_FIELD, QUOTE_IN_QUOTED_FIELD,
    EAT_CRNL
} ParserState;

typedef enum {
    QUOTE_MINIMAL, QUOTE_ALL, QUOTE_NONNUMERIC, QUOTE_NONE
} QuoteStyle;

typedef struct {
    QuoteStyle style;
    char *name;
} StyleDesc;

static StyleDesc quote_styles[] = {
    { QUOTE_MINIMAL,    "QUOTE_MINIMAL" },
    { QUOTE_ALL,        "QUOTE_ALL" },
    { QUOTE_NONNUMERIC, "QUOTE_NONNUMERIC" },
    { QUOTE_NONE,       "QUOTE_NONE" },
    { 0 }
};

typedef struct {
    PyObject_HEAD

    int doublequote;            /* is " represented by ""? */
    char delimiter;             /* field separator */
    char quotechar;             /* quote character */
    char escapechar;            /* escape character */
    int skipinitialspace;       /* ignore spaces following delimiter? */
    PyObject *lineterminator; /* string to write between records */
    int quoting;                /* style of quoting to write */

    int strict;                 /* raise exception on bad CSV */
} DialectObj;

staticforward PyTypeObject Dialect_Type;

typedef struct {
    PyObject_HEAD

    PyObject *input_iter;   /* iterate over this for input lines */

    DialectObj *dialect;    /* parsing dialect */

    PyObject *fields;           /* field list for current record */
    ParserState state;          /* current CSV parse state */
    char *field;                /* build current field in here */
    int field_size;             /* size of allocated buffer */
    int field_len;              /* length of current field */
    int numeric_field;          /* treat field as numeric */
    unsigned long line_num;     /* Source-file line number */
} ReaderObj;

staticforward PyTypeObject Reader_Type;

#define ReaderObject_Check(v)   (Py_TYPE(v) == &Reader_Type)

typedef struct {
    PyObject_HEAD

    PyObject *writeline;    /* write output lines to this file */

    DialectObj *dialect;    /* parsing dialect */

    char *rec;                  /* buffer for parser.join */
    int rec_size;               /* size of allocated record */
    int rec_len;                /* length of record */
    int num_fields;             /* number of fields in record */
} WriterObj;

staticforward PyTypeObject Writer_Type;

/*
 * DIALECT class
 */

static PyObject *
get_dialect_from_registry(PyObject * name_obj)
{
    PyObject *dialect_obj;

    dialect_obj = PyDict_GetItem(dialects, name_obj);
    if (dialect_obj == NULL) {
        if (!PyErr_Occurred())
            PyErr_Format(error_obj, "unknown dialect");
    }
    else
        Py_INCREF(dialect_obj);
    return dialect_obj;
}

static PyObject *
get_string(PyObject *str)
{
    Py_XINCREF(str);
    return str;
}

static PyObject *
get_nullchar_as_None(char c)
{
    if (c == '\0') {
        Py_INCREF(Py_None);
        return Py_None;
    }
    else
        return PyString_FromStringAndSize((char*)&c, 1);
}

static PyObject *
Dialect_get_lineterminator(DialectObj *self)
{
    return get_string(self->lineterminator);
}

static PyObject *
Dialect_get_escapechar(DialectObj *self)
{
    return get_nullchar_as_None(self->escapechar);
}

static PyObject *
Dialect_get_quotechar(DialectObj *self)
{
    return get_nullchar_as_None(self->quotechar);
}

static PyObject *
Dialect_get_quoting(DialectObj *self)
{
    return PyInt_FromLong(self->quoting);
}

static int
_set_bool(const char *name, int *target, PyObject *src, int dflt)
{
    if (src == NULL)
        *target = dflt;
    else {
        int b = PyObject_IsTrue(src);
        if (b < 0)
            return -1;
        *target = b;
    }
    return 0;
}

static int
_set_int(const char *name, int *target, PyObject *src, int dflt)
{
    if (src == NULL)
        *target = dflt;
    else {
        if (!PyInt_Check(src)) {
            PyErr_Format(PyExc_TypeError,
                         "\"%s\" must be an integer", name);
            return -1;
        }
        *target = PyInt_AsLong(src);
    }
    return 0;
}

static int
_set_char(const char *name, char *target, PyObject *src, char dflt)
{
    if (src == NULL)
        *target = dflt;
    else {
        *target = '\0';
        if (src != Py_None) {
            Py_ssize_t len;
            if (!PyString_Check(src)) {
                PyErr_Format(PyExc_TypeError,
                    "\"%s\" must be string, not %.200s", name,
                    src->ob_type->tp_name);
                return -1;
            }
            len = PyString_GET_SIZE(src);
            if (len > 1) {
                PyErr_Format(PyExc_TypeError,
                    "\"%s\" must be an 1-character string",
                    name);
                return -1;
            }
            if (len > 0)
                *target = *PyString_AS_STRING(src);
        }
    }
    return 0;
}

static int
_set_str(const char *name, PyObject **target, PyObject *src, const char *dflt)
{
    if (src == NULL)
        *target = PyString_FromString(dflt);
    else {
        if (src == Py_None)
            *target = NULL;
        else if (!IS_BASESTRING(src)) {
            PyErr_Format(PyExc_TypeError,
                         "\"%s\" must be a string", name);
            return -1;
        }
        else {
            Py_XDECREF(*target);
            Py_INCREF(src);
            *target = src;
        }
    }
    return 0;
}

static int
dialect_check_quoting(int quoting)
{
    StyleDesc *qs = quote_styles;

    for (qs = quote_styles; qs->name; qs++) {
        if (qs->style == quoting)
            return 0;
    }
    PyErr_Format(PyExc_TypeError, "bad \"quoting\" value");
    return -1;
}

#define D_OFF(x) offsetof(DialectObj, x)

static struct PyMemberDef Dialect_memberlist[] = {
    { "delimiter",          T_CHAR, D_OFF(delimiter), READONLY },
    { "skipinitialspace",   T_INT, D_OFF(skipinitialspace), READONLY },
    { "doublequote",        T_INT, D_OFF(doublequote), READONLY },
    { "strict",             T_INT, D_OFF(strict), READONLY },
    { NULL }
};

static PyGetSetDef Dialect_getsetlist[] = {
    { "escapechar",             (getter)Dialect_get_escapechar},
    { "lineterminator",         (getter)Dialect_get_lineterminator},
    { "quotechar",              (getter)Dialect_get_quotechar},
    { "quoting",                (getter)Dialect_get_quoting},
    {NULL},
};

static void
Dialect_dealloc(DialectObj *self)
{
    Py_XDECREF(self->lineterminator);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static char *dialect_kws[] = {
    "dialect",
    "delimiter",
    "doublequote",
    "escapechar",
    "lineterminator",
    "quotechar",
    "quoting",
    "skipinitialspace",
    "strict",
    NULL
};

static PyObject *
dialect_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    DialectObj *self;
    PyObject *ret = NULL;
    PyObject *dialect = NULL;
    PyObject *delimiter = NULL;
    PyObject *doublequote = NULL;
    PyObject *escapechar = NULL;
    PyObject *lineterminator = NULL;
    PyObject *quotechar = NULL;
    PyObject *quoting = NULL;
    PyObject *skipinitialspace = NULL;
    PyObject *strict = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs,
                                     "|OOOOOOOOO", dialect_kws,
                                     &dialect,
                                     &delimiter,
                                     &doublequote,
                                     &escapechar,
                                     &lineterminator,
                                     &quotechar,
                                     &quoting,
                                     &skipinitialspace,
                                     &strict))
        return NULL;

    if (dialect != NULL) {
        if (IS_BASESTRING(dialect)) {
            dialect = get_dialect_from_registry(dialect);
            if (dialect == NULL)
                return NULL;
        }
        else
            Py_INCREF(dialect);
        /* Can we reuse this instance? */
        if (PyObject_TypeCheck(dialect, &Dialect_Type) &&
            delimiter == 0 &&
            doublequote == 0 &&
            escapechar == 0 &&
            lineterminator == 0 &&
            quotechar == 0 &&
            quoting == 0 &&
            skipinitialspace == 0 &&
            strict == 0)
            return dialect;
    }

    self = (DialectObj *)type->tp_alloc(type, 0);
    if (self == NULL) {
        Py_XDECREF(dialect);
        return NULL;
    }
    self->lineterminator = NULL;

    Py_XINCREF(delimiter);
    Py_XINCREF(doublequote);
    Py_XINCREF(escapechar);
    Py_XINCREF(lineterminator);
    Py_XINCREF(quotechar);
    Py_XINCREF(quoting);
    Py_XINCREF(skipinitialspace);
    Py_XINCREF(strict);
    if (dialect != NULL) {
#define DIALECT_GETATTR(v, n) \
        if (v == NULL) \
            v = PyObject_GetAttrString(dialect, n)
        DIALECT_GETATTR(delimiter, "delimiter");
        DIALECT_GETATTR(doublequote, "doublequote");
        DIALECT_GETATTR(escapechar, "escapechar");
        DIALECT_GETATTR(lineterminator, "lineterminator");
        DIALECT_GETATTR(quotechar, "quotechar");
        DIALECT_GETATTR(quoting, "quoting");
        DIALECT_GETATTR(skipinitialspace, "skipinitialspace");
        DIALECT_GETATTR(strict, "strict");
        PyErr_Clear();
    }

    /* check types and convert to C values */
#define DIASET(meth, name, target, src, dflt) \
    if (meth(name, target, src, dflt)) \
        goto err
    DIASET(_set_char, "delimiter", &self->delimiter, delimiter, ',');
    DIASET(_set_bool, "doublequote", &self->doublequote, doublequote, 1);
    DIASET(_set_char, "escapechar", &self->escapechar, escapechar, 0);
    DIASET(_set_str, "lineterminator", &self->lineterminator, lineterminator, "\r\n");
    DIASET(_set_char, "quotechar", &self->quotechar, quotechar, '"');
    DIASET(_set_int, "quoting", &self->quoting, quoting, QUOTE_MINIMAL);
    DIASET(_set_bool, "skipinitialspace", &self->skipinitialspace, skipinitialspace, 0);
    DIASET(_set_bool, "strict", &self->strict, strict, 0);

    /* validate options */
    if (dialect_check_quoting(self->quoting))
        goto err;
    if (self->delimiter == 0) {
        PyErr_SetString(PyExc_TypeError,
                        "\"delimiter\" must be an 1-character string");
        goto err;
    }
    if (quotechar == Py_None && quoting == NULL)
        self->quoting = QUOTE_NONE;
    if (self->quoting != QUOTE_NONE && self->quotechar == 0) {
        PyErr_SetString(PyExc_TypeError,
                        "quotechar must be set if quoting enabled");
        goto err;
    }
    if (self->lineterminator == 0) {
        PyErr_SetString(PyExc_TypeError, "lineterminator must be set");
        goto err;
    }

    ret = (PyObject *)self;
    Py_INCREF(self);
err:
    Py_XDECREF(self);
    Py_XDECREF(dialect);
    Py_XDECREF(delimiter);
    Py_XDECREF(doublequote);
    Py_XDECREF(escapechar);
    Py_XDECREF(lineterminator);
    Py_XDECREF(quotechar);
    Py_XDECREF(quoting);
    Py_XDECREF(skipinitialspace);
    Py_XDECREF(strict);
    return ret;
}


PyDoc_STRVAR(Dialect_Type_doc,
"CSV dialect\n"
"\n"
"The Dialect type records CSV parsing and generation options.\n");

static PyTypeObject Dialect_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "_csv.Dialect",                         /* tp_name */
    sizeof(DialectObj),                     /* tp_basicsize */
    0,                                      /* tp_itemsize */
    /*  methods  */
    (destructor)Dialect_dealloc,            /* tp_dealloc */
    (printfunc)0,                           /* tp_print */
    (getattrfunc)0,                         /* tp_getattr */
    (setattrfunc)0,                         /* tp_setattr */
    (cmpfunc)0,                             /* tp_compare */
    (reprfunc)0,                            /* tp_repr */
    0,                                      /* tp_as_number */
    0,                                      /* tp_as_sequence */
    0,                                      /* tp_as_mapping */
    (hashfunc)0,                            /* tp_hash */
    (ternaryfunc)0,                         /* tp_call */
    (reprfunc)0,                                /* tp_str */
    0,                                      /* tp_getattro */
    0,                                      /* tp_setattro */
    0,                                      /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /* tp_flags */
    Dialect_Type_doc,                       /* tp_doc */
    0,                                      /* tp_traverse */
    0,                                      /* tp_clear */
    0,                                      /* tp_richcompare */
    0,                                      /* tp_weaklistoffset */
    0,                                      /* tp_iter */
    0,                                      /* tp_iternext */
    0,                                          /* tp_methods */
    Dialect_memberlist,                     /* tp_members */
    Dialect_getsetlist,                     /* tp_getset */
    0,                                          /* tp_base */
    0,                                          /* tp_dict */
    0,                                          /* tp_descr_get */
    0,                                          /* tp_descr_set */
    0,                                          /* tp_dictoffset */
    0,                                          /* tp_init */
    0,                                          /* tp_alloc */
    dialect_new,                                /* tp_new */
    0,                                          /* tp_free */
};

/*
 * Return an instance of the dialect type, given a Python instance or kwarg
 * description of the dialect
 */
static PyObject *
_call_dialect(PyObject *dialect_inst, PyObject *kwargs)
{
    PyObject *ctor_args;
    PyObject *dialect;

    ctor_args = Py_BuildValue(dialect_inst ? "(O)" : "()", dialect_inst);
    if (ctor_args == NULL)
        return NULL;
    dialect = PyObject_Call((PyObject *)&Dialect_Type, ctor_args, kwargs);
    Py_DECREF(ctor_args);
    return dialect;
}

/*
 * READER
 */
static int
parse_save_field(ReaderObj *self)
{
    PyObject *field;

    field = PyString_FromStringAndSize(self->field, self->field_len);
    if (field == NULL)
        return -1;
    self->field_len = 0;
    if (self->numeric_field) {
        PyObject *tmp;

        self->numeric_field = 0;
        tmp = PyNumber_Float(field);
        if (tmp == NULL) {
            Py_DECREF(field);
            return -1;
        }
        Py_DECREF(field);
        field = tmp;
    }
    PyList_Append(self->fields, field);
    Py_DECREF(field);
    return 0;
}

static int
parse_grow_buff(ReaderObj *self)
{
    if (self->field_size == 0) {
        self->field_size = 4096;
        if (self->field != NULL)
            PyMem_Free(self->field);
        self->field = PyMem_Malloc(self->field_size);
    }
    else {
        if (self->field_size > INT_MAX / 2) {
            PyErr_NoMemory();
            return 0;
        }
        self->field_size *= 2;
        self->field = PyMem_Realloc(self->field, self->field_size);
    }
    if (self->field == NULL) {
        PyErr_NoMemory();
        return 0;
    }
    return 1;
}

static int
parse_add_char(ReaderObj *self, char c)
{
    if (self->field_len >= field_limit) {
        PyErr_Format(error_obj, "field larger than field limit (%ld)",
                     field_limit);
        return -1;
    }
    if (self->field_len == self->field_size && !parse_grow_buff(self))
        return -1;
    self->field[self->field_len++] = c;
    return 0;
}

static int
parse_process_char(ReaderObj *self, char c)
{
    DialectObj *dialect = self->dialect;

    switch (self->state) {
    case START_RECORD:
        /* start of record */
        if (c == '\0')
            /* empty line - return [] */
            break;
        else if (c == '\n' || c == '\r') {
            self->state = EAT_CRNL;
            break;
        }
        /* normal character - handle as START_FIELD */
        self->state = START_FIELD;
        /* fallthru */
    case START_FIELD:
        /* expecting field */
        if (c == '\n' || c == '\r' || c == '\0') {
            /* save empty field - return [fields] */
            if (parse_save_field(self) < 0)
                return -1;
            self->state = (c == '\0' ? START_RECORD : EAT_CRNL);
        }
        else if (c == dialect->quotechar &&
                 dialect->quoting != QUOTE_NONE) {
            /* start quoted field */
            self->state = IN_QUOTED_FIELD;
        }
        else if (c == dialect->escapechar) {
            /* possible escaped character */
            self->state = ESCAPED_CHAR;
        }
        else if (c == ' ' && dialect->skipinitialspace)
            /* ignore space at start of field */
            ;
        else if (c == dialect->delimiter) {
            /* save empty field */
            if (parse_save_field(self) < 0)
                return -1;
        }
        else {
            /* begin new unquoted field */
            if (dialect->quoting == QUOTE_NONNUMERIC)
                self->numeric_field = 1;
            if (parse_add_char(self, c) < 0)
                return -1;
            self->state = IN_FIELD;
        }
        break;

    case ESCAPED_CHAR:
        if (c == '\0')
            c = '\n';
        if (parse_add_char(self, c) < 0)
            return -1;
        self->state = IN_FIELD;
        break;

    case IN_FIELD:
        /* in unquoted field */
        if (c == '\n' || c == '\r' || c == '\0') {
            /* end of line - return [fields] */
            if (parse_save_field(self) < 0)
                return -1;
            self->state = (c == '\0' ? START_RECORD : EAT_CRNL);
        }
        else if (c == dialect->escapechar) {
            /* possible escaped character */
            self->state = ESCAPED_CHAR;
        }
        else if (c == dialect->delimiter) {
            /* save field - wait for new field */
            if (parse_save_field(self) < 0)
                return -1;
            self->state = START_FIELD;
        }
        else {
            /* normal character - save in field */
            if (parse_add_char(self, c) < 0)
                return -1;
        }
        break;

    case IN_QUOTED_FIELD:
        /* in quoted field */
        if (c == '\0')
            ;
        else if (c == dialect->escapechar) {
            /* Possible escape character */
            self->state = ESCAPE_IN_QUOTED_FIELD;
        }
        else if (c == dialect->quotechar &&
                 dialect->quoting != QUOTE_NONE) {
            if (dialect->doublequote) {
                /* doublequote; " represented by "" */
                self->state = QUOTE_IN_QUOTED_FIELD;
            }
            else {
                /* end of quote part of field */
                self->state = IN_FIELD;
            }
        }
        else {
            /* normal character - save in field */
            if (parse_add_char(self, c) < 0)
                return -1;
        }
        break;

    case ESCAPE_IN_QUOTED_FIELD:
        if (c == '\0')
            c = '\n';
        if (parse_add_char(self, c) < 0)
            return -1;
        self->state = IN_QUOTED_FIELD;
        break;

    case QUOTE_IN_QUOTED_FIELD:
        /* doublequote - seen a quote in an quoted field */
        if (dialect->quoting != QUOTE_NONE &&
            c == dialect->quotechar) {
            /* save "" as " */
            if (parse_add_char(self, c) < 0)
                return -1;
            self->state = IN_QUOTED_FIELD;
        }
        else if (c == dialect->delimiter) {
            /* save field - wait for new field */
            if (parse_save_field(self) < 0)
                return -1;
            self->state = START_FIELD;
        }
        else if (c == '\n' || c == '\r' || c == '\0') {
            /* end of line - return [fields] */
            if (parse_save_field(self) < 0)
                return -1;
            self->state = (c == '\0' ? START_RECORD : EAT_CRNL);
        }
        else if (!dialect->strict) {
            if (parse_add_char(self, c) < 0)
                return -1;
            self->state = IN_FIELD;
        }
        else {
            /* illegal */
            PyErr_Format(error_obj, "'%c' expected after '%c'",
                            dialect->delimiter,
                            dialect->quotechar);
            return -1;
        }
        break;

    case EAT_CRNL:
        if (c == '\n' || c == '\r')
            ;
        else if (c == '\0')
            self->state = START_RECORD;
        else {
            PyErr_Format(error_obj, "new-line character seen in unquoted field - do you need to open the file in universal-newline mode?");
            return -1;
        }
        break;

    }
    return 0;
}

static int
parse_reset(ReaderObj *self)
{
    Py_XDECREF(self->fields);
    self->fields = PyList_New(0);
    if (self->fields == NULL)
        return -1;
    self->field_len = 0;
    self->state = START_RECORD;
    self->numeric_field = 0;
    return 0;
}

static PyObject *
Reader_iternext(ReaderObj *self)
{
    PyObject *lineobj;
    PyObject *fields = NULL;
    char *line, c;
    int linelen;

    if (parse_reset(self) < 0)
        return NULL;
    do {
        lineobj = PyIter_Next(self->input_iter);
        if (lineobj == NULL) {
            /* End of input OR exception */
            if (!PyErr_Occurred() && (self->field_len != 0 ||
                                      self->state == IN_QUOTED_FIELD)) {
                if (self->dialect->strict)
                    PyErr_SetString(error_obj, "unexpected end of data");
                else if (parse_save_field(self) >= 0 )
                    break;
            }
            return NULL;
        }
        ++self->line_num;

        line = PyString_AsString(lineobj);
        linelen = (int)PyString_Size(lineobj);

        if (line == NULL || linelen < 0) {
            Py_DECREF(lineobj);
            return NULL;
        }
        while (linelen--) {
            c = *line++;
            if (c == '\0') {
                Py_DECREF(lineobj);
                PyErr_Format(error_obj,
                             "line contains NULL byte");
                goto err;
            }
            if (parse_process_char(self, c) < 0) {
                Py_DECREF(lineobj);
                goto err;
            }
        }
        Py_DECREF(lineobj);
        if (parse_process_char(self, 0) < 0)
            goto err;
    } while (self->state != START_RECORD);

    fields = self->fields;
    self->fields = NULL;
err:
    return fields;
}

static void
Reader_dealloc(ReaderObj *self)
{
    PyObject_GC_UnTrack(self);
    Py_XDECREF(self->dialect);
    Py_XDECREF(self->input_iter);
    Py_XDECREF(self->fields);
    if (self->field != NULL)
        PyMem_Free(self->field);
    PyObject_GC_Del(self);
}

static int
Reader_traverse(ReaderObj *self, visitproc visit, void *arg)
{
    Py_VISIT(self->dialect);
    Py_VISIT(self->input_iter);
    Py_VISIT(self->fields);
    return 0;
}

static int
Reader_clear(ReaderObj *self)
{
    Py_CLEAR(self->dialect);
    Py_CLEAR(self->input_iter);
    Py_CLEAR(self->fields);
    return 0;
}

PyDoc_STRVAR(Reader_Type_doc,
"CSV reader\n"
"\n"
"Reader objects are responsible for reading and parsing tabular data\n"
"in CSV format.\n"
);

static struct PyMethodDef Reader_methods[] = {
    { NULL, NULL }
};
#define R_OFF(x) offsetof(ReaderObj, x)

static struct PyMemberDef Reader_memberlist[] = {
    { "dialect", T_OBJECT, R_OFF(dialect), RO },
    { "line_num", T_ULONG, R_OFF(line_num), RO },
    { NULL }
};


static PyTypeObject Reader_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "_csv.reader",                          /*tp_name*/
    sizeof(ReaderObj),                      /*tp_basicsize*/
    0,                                      /*tp_itemsize*/
    /* methods */
    (destructor)Reader_dealloc,             /*tp_dealloc*/
    (printfunc)0,                           /*tp_print*/
    (getattrfunc)0,                         /*tp_getattr*/
    (setattrfunc)0,                         /*tp_setattr*/
    (cmpfunc)0,                             /*tp_compare*/
    (reprfunc)0,                            /*tp_repr*/
    0,                                      /*tp_as_number*/
    0,                                      /*tp_as_sequence*/
    0,                                      /*tp_as_mapping*/
    (hashfunc)0,                            /*tp_hash*/
    (ternaryfunc)0,                         /*tp_call*/
    (reprfunc)0,                                /*tp_str*/
    0,                                      /*tp_getattro*/
    0,                                      /*tp_setattro*/
    0,                                      /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE |
        Py_TPFLAGS_HAVE_GC,                     /*tp_flags*/
    Reader_Type_doc,                        /*tp_doc*/
    (traverseproc)Reader_traverse,          /*tp_traverse*/
    (inquiry)Reader_clear,                  /*tp_clear*/
    0,                                      /*tp_richcompare*/
    0,                                      /*tp_weaklistoffset*/
    PyObject_SelfIter,                          /*tp_iter*/
    (getiterfunc)Reader_iternext,           /*tp_iternext*/
    Reader_methods,                         /*tp_methods*/
    Reader_memberlist,                      /*tp_members*/
    0,                                      /*tp_getset*/

};

static PyObject *
csv_reader(PyObject *module, PyObject *args, PyObject *keyword_args)
{
    PyObject * iterator, * dialect = NULL;
    ReaderObj * self = PyObject_GC_New(ReaderObj, &Reader_Type);

    if (!self)
        return NULL;

    self->dialect = NULL;
    self->fields = NULL;
    self->input_iter = NULL;
    self->field = NULL;
    self->field_size = 0;
    self->line_num = 0;

    if (parse_reset(self) < 0) {
        Py_DECREF(self);
        return NULL;
    }

    if (!PyArg_UnpackTuple(args, "", 1, 2, &iterator, &dialect)) {
        Py_DECREF(self);
        return NULL;
    }
    self->input_iter = PyObject_GetIter(iterator);
    if (self->input_iter == NULL) {
        PyErr_SetString(PyExc_TypeError,
                        "argument 1 must be an iterator");
        Py_DECREF(self);
        return NULL;
    }
    self->dialect = (DialectObj *)_call_dialect(dialect, keyword_args);
    if (self->dialect == NULL) {
        Py_DECREF(self);
        return NULL;
    }

    PyObject_GC_Track(self);
    return (PyObject *)self;
}

/*
 * WRITER
 */
/* ---------------------------------------------------------------- */
static void
join_reset(WriterObj *self)
{
    self->rec_len = 0;
    self->num_fields = 0;
}

#define MEM_INCR 32768

/* Calculate new record length or append field to record.  Return new
 * record length.
 */
static int
join_append_data(WriterObj *self, char *field, int quote_empty,
                 int *quoted, int copy_phase)
{
    DialectObj *dialect = self->dialect;
    int i, rec_len;
    char *lineterm;

#define ADDCH(c) \
    do {\
        if (copy_phase) \
            self->rec[rec_len] = c;\
        rec_len++;\
    } while(0)

    lineterm = PyString_AsString(dialect->lineterminator);
    if (lineterm == NULL)
        return -1;

    rec_len = self->rec_len;

    /* If this is not the first field we need a field separator */
    if (self->num_fields > 0)
        ADDCH(dialect->delimiter);

    /* Handle preceding quote */
    if (copy_phase && *quoted)
        ADDCH(dialect->quotechar);

    /* Copy/count field data */
    for (i = 0;; i++) {
        char c = field[i];
        int want_escape = 0;

        if (c == '\0')
            break;

        if (c == dialect->delimiter ||
            c == dialect->escapechar ||
            c == dialect->quotechar ||
            strchr(lineterm, c)) {
            if (dialect->quoting == QUOTE_NONE)
                want_escape = 1;
            else {
                if (c == dialect->quotechar) {
                    if (dialect->doublequote)
                        ADDCH(dialect->quotechar);
                    else
                        want_escape = 1;
                }
                if (!want_escape)
                    *quoted = 1;
            }
            if (want_escape) {
                if (!dialect->escapechar) {
                    PyErr_Format(error_obj,
                                 "need to escape, but no escapechar set");
                    return -1;
                }
                ADDCH(dialect->escapechar);
            }
        }
        /* Copy field character into record buffer.
         */
        ADDCH(c);
    }

    /* If field is empty check if it needs to be quoted.
     */
    if (i == 0 && quote_empty) {
        if (dialect->quoting == QUOTE_NONE) {
            PyErr_Format(error_obj,
                         "single empty field record must be quoted");
            return -1;
        }
        else
            *quoted = 1;
    }

    if (*quoted) {
        if (copy_phase)
            ADDCH(dialect->quotechar);
        else
            rec_len += 2;
    }
    return rec_len;
#undef ADDCH
}

static int
join_check_rec_size(WriterObj *self, int rec_len)
{

    if (rec_len < 0 || rec_len > INT_MAX - MEM_INCR) {
        PyErr_NoMemory();
        return 0;
    }

    if (rec_len > self->rec_size) {
        if (self->rec_size == 0) {
            self->rec_size = (rec_len / MEM_INCR + 1) * MEM_INCR;
            if (self->rec != NULL)
                PyMem_Free(self->rec);
            self->rec = PyMem_Malloc(self->rec_size);
        }
        else {
            char *old_rec = self->rec;

            self->rec_size = (rec_len / MEM_INCR + 1) * MEM_INCR;
            self->rec = PyMem_Realloc(self->rec, self->rec_size);
            if (self->rec == NULL)
                PyMem_Free(old_rec);
        }
        if (self->rec == NULL) {
            PyErr_NoMemory();
            return 0;
        }
    }
    return 1;
}

static int
join_append(WriterObj *self, char *field, int *quoted, int quote_empty)
{
    int rec_len;

    rec_len = join_append_data(self, field, quote_empty, quoted, 0);
    if (rec_len < 0)
        return 0;

    /* grow record buffer if necessary */
    if (!join_check_rec_size(self, rec_len))
        return 0;

    self->rec_len = join_append_data(self, field, quote_empty, quoted, 1);
    self->num_fields++;

    return 1;
}

static int
join_append_lineterminator(WriterObj *self)
{
    int terminator_len;
    char *terminator;

    terminator_len = (int)PyString_Size(self->dialect->lineterminator);
    if (terminator_len == -1)
        return 0;

    /* grow record buffer if necessary */
    if (!join_check_rec_size(self, self->rec_len + terminator_len))
        return 0;

    terminator = PyString_AsString(self->dialect->lineterminator);
    if (terminator == NULL)
        return 0;
    memmove(self->rec + self->rec_len, terminator, terminator_len);
    self->rec_len += terminator_len;

    return 1;
}

PyDoc_STRVAR(csv_writerow_doc,
"writerow(sequence)\n"
"\n"
"Construct and write a CSV record from a sequence of fields.  Non-string\n"
"elements will be converted to string.");

static PyObject *
csv_writerow(WriterObj *self, PyObject *seq)
{
    DialectObj *dialect = self->dialect;
    int len, i;

    if (!PySequence_Check(seq))
        return PyErr_Format(error_obj, "sequence expected");

    len = (int)PySequence_Length(seq);
    if (len < 0)
        return NULL;

    /* Join all fields in internal buffer.
     */
    join_reset(self);
    for (i = 0; i < len; i++) {
        PyObject *field;
        int append_ok;
        int quoted;

        field = PySequence_GetItem(seq, i);
        if (field == NULL)
            return NULL;

        switch (dialect->quoting) {
        case QUOTE_NONNUMERIC:
            quoted = !PyNumber_Check(field);
            break;
        case QUOTE_ALL:
            quoted = 1;
            break;
        default:
            quoted = 0;
            break;
        }

        if (PyString_Check(field)) {
            append_ok = join_append(self,
                                    PyString_AS_STRING(field),
                                    &quoted, len == 1);
            Py_DECREF(field);
        }
        else if (field == Py_None) {
            append_ok = join_append(self, "", &quoted, len == 1);
            Py_DECREF(field);
        }
        else {
            PyObject *str;

            if (PyFloat_Check(field)) {
                str = PyObject_Repr(field);
            } else {
                str = PyObject_Str(field);
            }
            Py_DECREF(field);
            if (str == NULL)
                return NULL;

            append_ok = join_append(self, PyString_AS_STRING(str),
                                    &quoted, len == 1);
            Py_DECREF(str);
        }
        if (!append_ok)
            return NULL;
    }

    /* Add line terminator.
     */
    if (!join_append_lineterminator(self))
        return 0;

    return PyObject_CallFunction(self->writeline,
                                 "(s#)", self->rec, self->rec_len);
}

PyDoc_STRVAR(csv_writerows_doc,
"writerows(sequence of sequences)\n"
"\n"
"Construct and write a series of sequences to a csv file.  Non-string\n"
"elements will be converted to string.");

static PyObject *
csv_writerows(WriterObj *self, PyObject *seqseq)
{
    PyObject *row_iter, *row_obj, *result;

    row_iter = PyObject_GetIter(seqseq);
    if (row_iter == NULL) {
        PyErr_SetString(PyExc_TypeError,
                        "writerows() argument must be iterable");
        return NULL;
    }
    while ((row_obj = PyIter_Next(row_iter))) {
        result = csv_writerow(self, row_obj);
        Py_DECREF(row_obj);
        if (!result) {
            Py_DECREF(row_iter);
            return NULL;
        }
        else
             Py_DECREF(result);
    }
    Py_DECREF(row_iter);
    if (PyErr_Occurred())
        return NULL;
    Py_INCREF(Py_None);
    return Py_None;
}

static struct PyMethodDef Writer_methods[] = {
    { "writerow", (PyCFunction)csv_writerow, METH_O, csv_writerow_doc},
    { "writerows", (PyCFunction)csv_writerows, METH_O, csv_writerows_doc},
    { NULL, NULL }
};

#define W_OFF(x) offsetof(WriterObj, x)

static struct PyMemberDef Writer_memberlist[] = {
    { "dialect", T_OBJECT, W_OFF(dialect), RO },
    { NULL }
};

static void
Writer_dealloc(WriterObj *self)
{
    PyObject_GC_UnTrack(self);
    Py_XDECREF(self->dialect);
    Py_XDECREF(self->writeline);
    if (self->rec != NULL)
        PyMem_Free(self->rec);
    PyObject_GC_Del(self);
}

static int
Writer_traverse(WriterObj *self, visitproc visit, void *arg)
{
    Py_VISIT(self->dialect);
    Py_VISIT(self->writeline);
    return 0;
}

static int
Writer_clear(WriterObj *self)
{
    Py_CLEAR(self->dialect);
    Py_CLEAR(self->writeline);
    return 0;
}

PyDoc_STRVAR(Writer_Type_doc,
"CSV writer\n"
"\n"
"Writer objects are responsible for generating tabular data\n"
"in CSV format from sequence input.\n"
);

static PyTypeObject Writer_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "_csv.writer",                          /*tp_name*/
    sizeof(WriterObj),                      /*tp_basicsize*/
    0,                                      /*tp_itemsize*/
    /* methods */
    (destructor)Writer_dealloc,             /*tp_dealloc*/
    (printfunc)0,                           /*tp_print*/
    (getattrfunc)0,                         /*tp_getattr*/
    (setattrfunc)0,                         /*tp_setattr*/
    (cmpfunc)0,                             /*tp_compare*/
    (reprfunc)0,                            /*tp_repr*/
    0,                                      /*tp_as_number*/
    0,                                      /*tp_as_sequence*/
    0,                                      /*tp_as_mapping*/
    (hashfunc)0,                            /*tp_hash*/
    (ternaryfunc)0,                         /*tp_call*/
    (reprfunc)0,                            /*tp_str*/
    0,                                      /*tp_getattro*/
    0,                                      /*tp_setattro*/
    0,                                      /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE |
        Py_TPFLAGS_HAVE_GC,                     /*tp_flags*/
    Writer_Type_doc,
    (traverseproc)Writer_traverse,          /*tp_traverse*/
    (inquiry)Writer_clear,                  /*tp_clear*/
    0,                                      /*tp_richcompare*/
    0,                                      /*tp_weaklistoffset*/
    (getiterfunc)0,                         /*tp_iter*/
    (getiterfunc)0,                         /*tp_iternext*/
    Writer_methods,                         /*tp_methods*/
    Writer_memberlist,                      /*tp_members*/
    0,                                      /*tp_getset*/
};

static PyObject *
csv_writer(PyObject *module, PyObject *args, PyObject *keyword_args)
{
    PyObject * output_file, * dialect = NULL;
    WriterObj * self = PyObject_GC_New(WriterObj, &Writer_Type);

    if (!self)
        return NULL;

    self->dialect = NULL;
    self->writeline = NULL;

    self->rec = NULL;
    self->rec_size = 0;
    self->rec_len = 0;
    self->num_fields = 0;

    if (!PyArg_UnpackTuple(args, "", 1, 2, &output_file, &dialect)) {
        Py_DECREF(self);
        return NULL;
    }
    self->writeline = PyObject_GetAttrString(output_file, "write");
    if (self->writeline == NULL || !PyCallable_Check(self->writeline)) {
        PyErr_SetString(PyExc_TypeError,
                        "argument 1 must have a \"write\" method");
        Py_DECREF(self);
        return NULL;
    }
    self->dialect = (DialectObj *)_call_dialect(dialect, keyword_args);
    if (self->dialect == NULL) {
        Py_DECREF(self);
        return NULL;
    }
    PyObject_GC_Track(self);
    return (PyObject *)self;
}

/*
 * DIALECT REGISTRY
 */
static PyObject *
csv_list_dialects(PyObject *module, PyObject *args)
{
    return PyDict_Keys(dialects);
}

static PyObject *
csv_register_dialect(PyObject *module, PyObject *args, PyObject *kwargs)
{
    PyObject *name_obj, *dialect_obj = NULL;
    PyObject *dialect;

    if (!PyArg_UnpackTuple(args, "", 1, 2, &name_obj, &dialect_obj))
        return NULL;
    if (!IS_BASESTRING(name_obj)) {
        PyErr_SetString(PyExc_TypeError,
                        "dialect name must be a string or unicode");
        return NULL;
    }
    dialect = _call_dialect(dialect_obj, kwargs);
    if (dialect == NULL)
        return NULL;
    if (PyDict_SetItem(dialects, name_obj, dialect) < 0) {
        Py_DECREF(dialect);
        return NULL;
    }
    Py_DECREF(dialect);
    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject *
csv_unregister_dialect(PyObject *module, PyObject *name_obj)
{
    if (PyDict_DelItem(dialects, name_obj) < 0)
        return PyErr_Format(error_obj, "unknown dialect");
    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject *
csv_get_dialect(PyObject *module, PyObject *name_obj)
{
    return get_dialect_from_registry(name_obj);
}

static PyObject *
csv_field_size_limit(PyObject *module, PyObject *args)
{
    PyObject *new_limit = NULL;
    long old_limit = field_limit;

    if (!PyArg_UnpackTuple(args, "field_size_limit", 0, 1, &new_limit))
        return NULL;
    if (new_limit != NULL) {
        if (!PyInt_Check(new_limit)) {
            PyErr_Format(PyExc_TypeError,
                         "limit must be an integer");
            return NULL;
        }
        field_limit = PyInt_AsLong(new_limit);
    }
    return PyInt_FromLong(old_limit);
}

/*
 * MODULE
 */

PyDoc_STRVAR(csv_module_doc,
"CSV parsing and writing.\n"
"\n"
"This module provides classes that assist in the reading and writing\n"
"of Comma Separated Value (CSV) files, and implements the interface\n"
"described by PEP 305.  Although many CSV files are simple to parse,\n"
"the format is not formally defined by a stable specification and\n"
"is subtle enough that parsing lines of a CSV file with something\n"
"like line.split(\",\") is bound to fail.  The module supports three\n"
"basic APIs: reading, writing, and registration of dialects.\n"
"\n"
"\n"
"DIALECT REGISTRATION:\n"
"\n"
"Readers and writers support a dialect argument, which is a convenient\n"
"handle on a group of settings.  When the dialect argument is a string,\n"
"it identifies one of the dialects previously registered with the module.\n"
"If it is a class or instance, the attributes of the argument are used as\n"
"the settings for the reader or writer:\n"
"\n"
"    class excel:\n"
"        delimiter = ','\n"
"        quotechar = '\"'\n"
"        escapechar = None\n"
"        doublequote = True\n"
"        skipinitialspace = False\n"
"        lineterminator = '\\r\\n'\n"
"        quoting = QUOTE_MINIMAL\n"
"\n"
"SETTINGS:\n"
"\n"
"    * quotechar - specifies a one-character string to use as the \n"
"        quoting character.  It defaults to '\"'.\n"
"    * delimiter - specifies a one-character string to use as the \n"
"        field separator.  It defaults to ','.\n"
"    * skipinitialspace - specifies how to interpret whitespace which\n"
"        immediately follows a delimiter.  It defaults to False, which\n"
"        means that whitespace immediately following a delimiter is part\n"
"        of the following field.\n"
"    * lineterminator -  specifies the character sequence which should \n"
"        terminate rows.\n"
"    * quoting - controls when quotes should be generated by the writer.\n"
"        It can take on any of the following module constants:\n"
"\n"
"        csv.QUOTE_MINIMAL means only when required, for example, when a\n"
"            field contains either the quotechar or the delimiter\n"
"        csv.QUOTE_ALL means that quotes are always placed around fields.\n"
"        csv.QUOTE_NONNUMERIC means that quotes are always placed around\n"
"            fields which do not parse as integers or floating point\n"
"            numbers.\n"
"        csv.QUOTE_NONE means that quotes are never placed around fields.\n"
"    * escapechar - specifies a one-character string used to escape \n"
"        the delimiter when quoting is set to QUOTE_NONE.\n"
"    * doublequote - controls the handling of quotes inside fields.  When\n"
"        True, two consecutive quotes are interpreted as one during read,\n"
"        and when writing, each quote character embedded in the data is\n"
"        written as two quotes\n");

PyDoc_STRVAR(csv_reader_doc,
"    csv_reader = reader(iterable [, dialect='excel']\n"
"                        [optional keyword args])\n"
"    for row in csv_reader:\n"
"        process(row)\n"
"\n"
"The \"iterable\" argument can be any object that returns a line\n"
"of input for each iteration, such as a file object or a list.  The\n"
"optional \"dialect\" parameter is discussed below.  The function\n"
"also accepts optional keyword arguments which override settings\n"
"provided by the dialect.\n"
"\n"
"The returned object is an iterator.  Each iteration returns a row\n"
"of the CSV file (which can span multiple input lines).\n");

PyDoc_STRVAR(csv_writer_doc,
"    csv_writer = csv.writer(fileobj [, dialect='excel']\n"
"                            [optional keyword args])\n"
"    for row in sequence:\n"
"        csv_writer.writerow(row)\n"
"\n"
"    [or]\n"
"\n"
"    csv_writer = csv.writer(fileobj [, dialect='excel']\n"
"                            [optional keyword args])\n"
"    csv_writer.writerows(rows)\n"
"\n"
"The \"fileobj\" argument can be any object that supports the file API.\n");

PyDoc_STRVAR(csv_list_dialects_doc,
"Return a list of all know dialect names.\n"
"    names = csv.list_dialects()");

PyDoc_STRVAR(csv_get_dialect_doc,
"Return the dialect instance associated with name.\n"
"    dialect = csv.get_dialect(name)");

PyDoc_STRVAR(csv_register_dialect_doc,
"Create a mapping from a string name to a dialect class.\n"
"    dialect = csv.register_dialect(name, dialect)");

PyDoc_STRVAR(csv_unregister_dialect_doc,
"Delete the name/dialect mapping associated with a string name.\n"
"    csv.unregister_dialect(name)");

PyDoc_STRVAR(csv_field_size_limit_doc,
"Sets an upper limit on parsed fields.\n"
"    csv.field_size_limit([limit])\n"
"\n"
"Returns old limit. If limit is not given, no new limit is set and\n"
"the old limit is returned");

static struct PyMethodDef csv_methods[] = {
    { "reader", (PyCFunction)csv_reader,
        METH_VARARGS | METH_KEYWORDS, csv_reader_doc},
    { "writer", (PyCFunction)csv_writer,
        METH_VARARGS | METH_KEYWORDS, csv_writer_doc},
    { "list_dialects", (PyCFunction)csv_list_dialects,
        METH_NOARGS, csv_list_dialects_doc},
    { "register_dialect", (PyCFunction)csv_register_dialect,
        METH_VARARGS | METH_KEYWORDS, csv_register_dialect_doc},
    { "unregister_dialect", (PyCFunction)csv_unregister_dialect,
        METH_O, csv_unregister_dialect_doc},
    { "get_dialect", (PyCFunction)csv_get_dialect,
        METH_O, csv_get_dialect_doc},
    { "field_size_limit", (PyCFunction)csv_field_size_limit,
        METH_VARARGS, csv_field_size_limit_doc},
    { NULL, NULL }
};

PyMODINIT_FUNC
init_csv(void)
{
    PyObject *module;
    StyleDesc *style;

    if (PyType_Ready(&Dialect_Type) < 0)
        return;

    if (PyType_Ready(&Reader_Type) < 0)
        return;

    if (PyType_Ready(&Writer_Type) < 0)
        return;

    /* Create the module and add the functions */
    module = Py_InitModule3("_csv", csv_methods, csv_module_doc);
    if (module == NULL)
        return;

    /* Add version to the module. */
    if (PyModule_AddStringConstant(module, "__version__",
                                   MODULE_VERSION) == -1)
        return;

    /* Add _dialects dictionary */
    dialects = PyDict_New();
    if (dialects == NULL)
        return;
    if (PyModule_AddObject(module, "_dialects", dialects))
        return;

    /* Add quote styles into dictionary */
    for (style = quote_styles; style->name; style++) {
        if (PyModule_AddIntConstant(module, style->name,
                                    style->style) == -1)
            return;
    }

    /* Add the Dialect type */
    Py_INCREF(&Dialect_Type);
    if (PyModule_AddObject(module, "Dialect", (PyObject *)&Dialect_Type))
        return;

    /* Add the CSV exception object to the module. */
    error_obj = PyErr_NewException("_csv.Error", NULL, NULL);
    if (error_obj == NULL)
        return;
    PyModule_AddObject(module, "Error", error_obj);
}
