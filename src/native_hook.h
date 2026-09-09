#ifndef HLMOD_NATIVE_HOOK_H
#define HLMOD_NATIVE_HOOK_H

#include "hlmod.h"

/* Lazily installs an inline x86-64 detour redirecting every existing and
 * future caller of the native function at findex through hlmod's ordinary
 * findex hook dispatch (jit_dispatch_hook), so `register_hook`/`hook()` work
 * identically for `@:hlNative` functions and for JIT-compiled bytecode
 * functions. Idempotent per findex. Only x86-64 targets are supported; other
 * architectures fail with a clear Python exception rather than guessing at a
 * prologue layout. On failure, a Python exception is set and -1 is returned.
 */
int hlmod_native_hook_ensure_installed(int findex, hl_type *signature);

PyObject *hlmod_py_native_findex(PyObject *self, PyObject *args);
PyObject *hlmod_py_native_hook_test_prologue(PyObject *self, PyObject *args);

#endif // HLMOD_NATIVE_HOOK_H
