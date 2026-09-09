#include "console_proxy.h"

#include <Python.h>
#include "platform.h"

#ifdef HL_WIN

typedef struct {
    PyObject_HEAD
    // No instance-specific data is needed for this simple proxy.
} ConsoleProxyObject;

// C implementation of the "write" method for our object.
static PyObject* ConsoleProxy_write(ConsoleProxyObject *self, PyObject *args) {
    PyObject* p_string;
    if (!PyArg_ParseTuple(args, "O", &p_string)) {
        return NULL;
    }

    if (!PyUnicode_Check(p_string)) {
        PyErr_SetString(PyExc_TypeError, "write() argument must be a string");
        return NULL;
    }

    Py_ssize_t size;
    const char *c_str = PyUnicode_AsUTF8AndSize(p_string, &size);
    if (c_str == NULL) {
        return NULL;
    }

    fwrite(c_str, 1, size, stdout);
    fflush(stdout);

    Py_ssize_t char_count = PyUnicode_GET_LENGTH(p_string);
    return PyLong_FromSsize_t(char_count);
}

static PyObject* ConsoleProxy_flush(ConsoleProxyObject *self, PyObject *args) {
    fflush(stdout);
    Py_RETURN_NONE;
}

static PyObject* ConsoleProxy_isatty(ConsoleProxyObject *self, PyObject *args) {
    if (_isatty(_fileno(stdout))) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

// Table of methods that our object will have.
static PyMethodDef ConsoleProxy_methods[] = {
    {"write",  (PyCFunction)ConsoleProxy_write,  METH_VARARGS, "Writes text to the C stdout."},
    {"flush",  (PyCFunction)ConsoleProxy_flush,  METH_NOARGS,  "Flushes the C stdout buffer."},
    {"isatty", (PyCFunction)ConsoleProxy_isatty, METH_NOARGS,  "Returns True if this is a TTY."},
    {NULL, NULL, 0, NULL} // Sentinel value
};

// The main type definition struct for our ConsoleProxyType.
static PyTypeObject ConsoleProxyType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "hlmod._ConsoleProxy",
    .tp_basicsize = sizeof(ConsoleProxyObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = PyType_GenericNew,
    .tp_doc = "A proxy object to forward Python I/O to the C stdout.",
    .tp_methods = ConsoleProxy_methods,
};

void hlmod_setup_pyio(void) {
    if (PyType_Ready(&ConsoleProxyType) < 0) {
        fprintf(stderr, "[hlmod] FATAL: Could not ready ConsoleProxyType.\n");
        PyErr_Print();
        return;
    }

    PyObject* proxy_instance = PyObject_CallObject((PyObject *)&ConsoleProxyType, NULL);
    if (proxy_instance == NULL) {
        fprintf(stderr, "[hlmod] FATAL: Could not create ConsoleProxy instance.\n");
        PyErr_Print();
        return;
    }

    PyObject* sys_module = PyImport_ImportModule("sys");
    if (sys_module) {
        PyObject_SetAttrString(sys_module, "stdout", proxy_instance);
        PyObject_SetAttrString(sys_module, "stderr", proxy_instance);
        Py_DECREF(sys_module);
    } else {
        fprintf(stderr, "[hlmod] FATAL: Could not import sys module.\n");
        PyErr_Print();
    }

    Py_DECREF(proxy_instance);
}

#else

void hlmod_setup_pyio(void) {
    /* Python's own stdout/stderr already interleave correctly with C stdout
     * on non-Windows platforms. */
}

#endif
