/*
 * c_node_nogc.c — C extension type WITHOUT GC tracking.
 *
 * Identical to c_node.c but without Py_TPFLAGS_HAVE_GC.
 * This makes each object 32 bytes (no PyGC_Head) instead of 48 bytes,
 * matching the RustNode object size.
 *
 * Used to isolate whether the C vs Rust performance difference is
 * due to cache effects (object size) rather than code quality.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <structmember.h>
#include <assert.h>

typedef struct {
    PyObject_HEAD
    long value;
    PyObject *next;
} NodeNoGCObject;

static int
NodeNoGC_init(NodeNoGCObject *self, PyObject *args, PyObject *kwds)
{
    Py_ssize_t nargs = PyTuple_GET_SIZE(args);
    Py_ssize_t nkw = (kwds != NULL) ? PyDict_GET_SIZE(kwds) : 0;

    PyObject *value_obj = NULL;
    PyObject *next = Py_None;

    if (nargs + nkw < 1 || nargs + nkw > 2) {
        PyErr_SetString(PyExc_TypeError,
                        "CNodeNoGC() requires 1 or 2 arguments");
        return -1;
    }

    if (nargs >= 1) value_obj = PyTuple_GET_ITEM(args, 0);
    if (nargs >= 2) next = PyTuple_GET_ITEM(args, 1);

    if (kwds != NULL) {
        static PyObject *str_value = NULL, *str_next = NULL;
        if (!str_value) {
            str_value = PyUnicode_InternFromString("value");
            str_next = PyUnicode_InternFromString("next");
        }
        PyObject *kw_val = PyDict_GetItem(kwds, str_value);
        if (kw_val) {
            if (value_obj) {
                PyErr_SetString(PyExc_TypeError,
                                "CNodeNoGC() got multiple values for 'value'");
                return -1;
            }
            value_obj = kw_val;
        }
        PyObject *kw_next = PyDict_GetItem(kwds, str_next);
        if (kw_next) {
            if (nargs >= 2) {
                PyErr_SetString(PyExc_TypeError,
                                "CNodeNoGC() got multiple values for 'next'");
                return -1;
            }
            next = kw_next;
        }
    }

    if (!value_obj) {
        PyErr_SetString(PyExc_TypeError,
                        "CNodeNoGC() missing required argument: 'value'");
        return -1;
    }

    long value = PyLong_AsLong(value_obj);
    if (value == -1 && PyErr_Occurred())
        return -1;

    self->value = value;
    Py_INCREF(next);
    Py_XDECREF(self->next);
    self->next = next;
    return 0;
}

static void
NodeNoGC_dealloc(NodeNoGCObject *self)
{
    Py_XDECREF(self->next);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyMemberDef NodeNoGC_members[] = {
    {"value", Py_T_LONG, offsetof(NodeNoGCObject, value), 0, "node value"},
    {"next", Py_T_OBJECT_EX, offsetof(NodeNoGCObject, next), 0, "next node"},
    {NULL}
};

static PyTypeObject NodeNoGCType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "c_node_nogc.CNodeNoGC",
    .tp_doc = "C extension node WITHOUT GC tracking (32 bytes per object)",
    .tp_basicsize = sizeof(NodeNoGCObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,  /* NO Py_TPFLAGS_HAVE_GC */
    .tp_new = PyType_GenericNew,
    .tp_init = (initproc)NodeNoGC_init,
    .tp_dealloc = (destructor)NodeNoGC_dealloc,
    .tp_members = NodeNoGC_members,
};

static PyObject *
c_sum_list_nogc(PyObject *self, PyObject *head)
{
    long total = 0;
    PyObject *current = head;

    if (current != Py_None && !PyObject_TypeCheck(current, &NodeNoGCType)) {
        PyErr_SetString(PyExc_TypeError,
                        "c_sum_list_nogc expects a CNodeNoGC linked list");
        return NULL;
    }

    while (current != Py_None) {
        assert(Py_IS_TYPE(current, &NodeNoGCType));
        total += ((NodeNoGCObject *)current)->value;
        current = ((NodeNoGCObject *)current)->next;
    }

    return PyLong_FromLong(total);
}

static PyMethodDef module_methods[] = {
    {"c_sum_list_nogc", c_sum_list_nogc, METH_O,
     "Sum all values in a CNodeNoGC linked list."},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef c_node_nogc_module = {
    PyModuleDef_HEAD_INIT,
    "c_node_nogc",
    "C extension node without GC tracking.",
    -1,
    module_methods
};

PyMODINIT_FUNC
PyInit_c_node_nogc(void)
{
    PyObject *m;

    if (PyType_Ready(&NodeNoGCType) < 0)
        return NULL;

    m = PyModule_Create(&c_node_nogc_module);
    if (m == NULL)
        return NULL;

    Py_INCREF(&NodeNoGCType);
    if (PyModule_AddObject(m, "CNodeNoGC", (PyObject *)&NodeNoGCType) < 0) {
        Py_DECREF(&NodeNoGCType);
        Py_DECREF(m);
        return NULL;
    }

    return m;
}
