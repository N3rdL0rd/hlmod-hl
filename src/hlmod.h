#ifndef HLMOD_H
#define HLMOD_H

#include <Python.h>
#include "uthash.h"
#include <hl.h>
#include <hlmodule.h>

#define HL_MAX_ARGS 9

typedef struct {
    PyObject_HEAD
    void* ptr;
    int kind;
} HlPtr;

extern PyTypeObject HlPtrType;

typedef struct {
    PyObject_HEAD
    int findex;
} HlHook;

extern PyTypeObject HlHookType;

extern THREAD_LOCAL int64 g_return_value_int;
extern THREAD_LOCAL double g_return_value_double;
extern THREAD_LOCAL bool g_is_passthrough_call;

int jit_dispatch_hook(int findex, int nargs, void** args);
void* hlmod_cast_to_hl(PyObject* obj, hl_type* type);
PyObject* hlmod_cast_to_py(hl_type* type, void* ptr);

typedef struct HookRegistryEntry {
    int findex;
    PyObject* callback;
    UT_hash_handle hh;
} HookRegistryEntry;

extern HookRegistryEntry* g_hook_registry;
extern hl_module *g_runtime_module;

void hlmod_register_hook(int findex, PyObject* callback);

#endif // HLMOD_H