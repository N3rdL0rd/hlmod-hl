#ifndef HLMOD_PY_MODULE_H
#define HLMOD_PY_MODULE_H

#include <Python.h>

/* Registers and initializes the `hlmod` built-in Python module: its method
 * table (the framework's entire Python-callable native API) plus the
 * `HlPtr`/`Hook` types. Passed to `PyImport_AppendInittab` before
 * `Py_InitializeEx`. */
PyMODINIT_FUNC PyInit_hlmod(void);

#endif // HLMOD_PY_MODULE_H
