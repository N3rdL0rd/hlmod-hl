#ifndef HLMOD_PYTHON_H
#define HLMOD_PYTHON_H
#include "hlmod.h"

#if defined(HL_VCC)
#include <intrin.h>
#endif

/* Publication helpers for the no-GIL JIT path; writers otherwise hold the GIL. */
static inline int hlmod_atomic_load_int(int *value) {
#if defined(HL_VCC)
    return (int)_InterlockedCompareExchange((long volatile *)value, 0, 0);
#else
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
#endif
}

static inline void hlmod_atomic_store_int(int *value, int next) {
#if defined(HL_VCC)
    _InterlockedExchange((long volatile *)value, next);
#else
    __atomic_store_n(value, next, __ATOMIC_RELEASE);
#endif
}

int hlmod_hook_registry_init(int count);
void hlmod_hook_registry_shutdown(void);
bool hlmod_hook_registered(int findex);
/* Returns an owned reference, or NULL without an exception. Requires the GIL. */
PyObject *hlmod_hook_callback(int findex);
/* Take/format Python error under GIL; throw only after cleanup and GIL release. */
char *hlmod_python_take_error(void);
void hlmod_python_throw_error(char *error);

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
