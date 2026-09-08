#ifndef HLMOD_PYTHON_H
#define HLMOD_PYTHON_H
#include "hlmod.h"

PyObject *hlmod_python_proxy(void *ptr);
PyObject *hlmod_python_type(hl_type *type);
PyObject *hlmod_py_create_subclass(PyObject *, PyObject *);
PyObject *hlmod_py_alloc_obj(PyObject *, PyObject *);
PyObject *hlmod_py_bind_instance(PyObject *, PyObject *);
PyObject *hlmod_py_init_obj(PyObject *, PyObject *);
vdynamic *hlmod_python_alloc_obj(hl_type *);
extern THREAD_LOCAL int hlmod_python_bypass;
int hlmod_python_traverse(void **slot, visitproc visit, void *arg);
void hlmod_python_clear(void **slot);
void hlmod_python_dispose(void);
PyObject *hlmod_py_make_callback(PyObject *, PyObject *);
void *hlmod_python_callback(PyObject *callable, hl_type *signature);
void hlmod_python_root(void **slot, bool add);
void hlmod_python_retain(void);
int hlmod_python_dispatch(int findex, int nargs, void **args);
int hlmod_python_init(void);
void hlmod_python_shutdown(void);
/* JIT adapter entry: all arguments are value slots, including pointer arguments. */
void hlmod_python_invoke(void *context, void **slots, vdynamic *result);
#endif
