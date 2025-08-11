#ifndef HLMOD_H
#define HLMOD_H

#include <Python.h>
#include "uthash.h"
#include <hl.h>
#include <hlmodule.h>

void jit_dispatch_hook(int findex, int nargs, void** args);

typedef struct HookRegistryEntry {
    int findex;         // fIndex to hook
    PyObject* callback; // The Python function to call
    UT_hash_handle hh;  // Required by uthash
} HookRegistryEntry;

extern HookRegistryEntry* g_hook_registry;
extern hl_module *g_runtime_module;

void hlmod_register_hook(int findex, PyObject* callback);

#endif // HLMOD_H