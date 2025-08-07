#ifndef HLMOD_HOOKS_H
#define HLMOD_HOOKS_H

#include <Python.h>
#include "uthash.h"

typedef struct HookRegistryEntry {
    int findex;         // fIndex to hook
    PyObject* callback; // The Python function to call
    UT_hash_handle hh;  // Required by uthash
} HookRegistryEntry;

extern HookRegistryEntry* g_hook_registry;

void hlmod_register_hook(int findex, PyObject* callback);

#endif // HLMOD_HOOKS_H