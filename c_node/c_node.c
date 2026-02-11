/*
 * c_node.c — C extension type for linked list benchmark.
 *
 * NodeObject is a genuine CPython type: PyObject_HEAD + fields.
 * c_sum_list traverses via direct struct pointer dereference —
 * the same mechanism CPython's own built-in types use.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <structmember.h>
#include <assert.h>

typedef struct {
    PyObject_HEAD
    long value;
    PyObject *next;  /* NodeObject* or Py_None */
} NodeObject;

/* --- NodeObject type -------------------------------------------------- */

static int
Node_init(NodeObject *self, PyObject *args, PyObject *kwds)
{
    Py_ssize_t nargs = PyTuple_GET_SIZE(args);
    Py_ssize_t nkw = (kwds != NULL) ? PyDict_GET_SIZE(kwds) : 0;

    PyObject *value_obj = NULL;
    PyObject *next = Py_None;

    if (nargs + nkw < 1 || nargs + nkw > 2) {
        PyErr_SetString(PyExc_TypeError,
                        "CNode() requires 1 or 2 arguments (value, next)");
        return -1;
    }

    if (nargs >= 1) {
        value_obj = PyTuple_GET_ITEM(args, 0);
    }
    if (nargs >= 2) {
        next = PyTuple_GET_ITEM(args, 1);
    }

    /* Handle keyword arguments */
    if (kwds != NULL) {
        static PyObject *str_value = NULL, *str_next = NULL;
        if (!str_value) {
            str_value = PyUnicode_InternFromString("value");
            str_next = PyUnicode_InternFromString("next");
        }

        PyObject *kw_val = PyDict_GetItem(kwds, str_value);
        if (kw_val != NULL) {
            if (value_obj != NULL) {
                PyErr_SetString(PyExc_TypeError,
                                "CNode() got multiple values for 'value'");
                return -1;
            }
            value_obj = kw_val;
        }

        PyObject *kw_next = PyDict_GetItem(kwds, str_next);
        if (kw_next != NULL) {
            if (nargs >= 2) {
                PyErr_SetString(PyExc_TypeError,
                                "CNode() got multiple values for 'next'");
                return -1;
            }
            next = kw_next;
        }
    }

    if (value_obj == NULL) {
        PyErr_SetString(PyExc_TypeError,
                        "CNode() missing required argument: 'value'");
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

static int
Node_traverse(NodeObject *self, visitproc visit, void *arg)
{
    Py_VISIT(self->next);
    return 0;
}

static int
Node_clear(NodeObject *self)
{
    Py_CLEAR(self->next);
    return 0;
}

static void
Node_dealloc(NodeObject *self)
{
    PyObject_GC_UnTrack(self);
    Node_clear(self);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyMemberDef Node_members[] = {
    {"value", Py_T_LONG, offsetof(NodeObject, value), 0, "node value"},
    {"next", Py_T_OBJECT_EX, offsetof(NodeObject, next), 0, "next node"},
    {NULL}
};

static PyTypeObject NodeType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "c_node.CNode",
    .tp_doc = "C extension linked list node",
    .tp_basicsize = sizeof(NodeObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_new = PyType_GenericNew,
    .tp_init = (initproc)Node_init,
    .tp_dealloc = (destructor)Node_dealloc,
    .tp_traverse = (traverseproc)Node_traverse,
    .tp_clear = (inquiry)Node_clear,
    .tp_members = Node_members,
};

/* --- c_sum_list: direct struct access --------------------------------- */

static PyObject *
c_sum_list(PyObject *self, PyObject *head)
{
    long total = 0;
    PyObject *current = head;

    /* Validate head at entry — public API boundary */
    if (current != Py_None && !PyObject_TypeCheck(current, &NodeType)) {
        PyErr_SetString(PyExc_TypeError,
                        "c_sum_list expects a CNode linked list");
        return NULL;
    }

    while (current != Py_None) {
        assert(Py_IS_TYPE(current, &NodeType));
        total += ((NodeObject *)current)->value;
        current = ((NodeObject *)current)->next;
    }

    return PyLong_FromLong(total);
}

/* --- Module definition ------------------------------------------------ */

static PyMethodDef module_methods[] = {
    {"c_sum_list", c_sum_list, METH_O,
     "Sum all values in a CNode linked list (direct struct access)."},
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef c_node_module = {
    PyModuleDef_HEAD_INIT,
    "c_node",
    "C extension linked list node for boundary-crossing benchmark.",
    -1,
    module_methods
};

PyMODINIT_FUNC
PyInit_c_node(void)
{
    PyObject *m;

    if (PyType_Ready(&NodeType) < 0)
        return NULL;

    m = PyModule_Create(&c_node_module);
    if (m == NULL)
        return NULL;

    Py_INCREF(&NodeType);
    if (PyModule_AddObject(m, "CNode", (PyObject *)&NodeType) < 0) {
        Py_DECREF(&NodeType);
        Py_DECREF(m);
        return NULL;
    }

    return m;
}
