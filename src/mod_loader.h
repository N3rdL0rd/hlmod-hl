#ifndef HLMOD_MOD_LOADER_H
#define HLMOD_MOD_LOADER_H
#include <Python.h>
#include <stdbool.h>

/* Runs the embedded mod resolver script against `mods_dir`, populating
 * `*load_order_list` with a dependency-ordered list of mod info dicts on
 * success. Returns 1 on success, 0 on failure (with a Python exception
 * already printed). The caller owns the returned list reference. */
int get_mod_load_order(const char *mods_dir, PyObject **load_order_list);

/* Loads and initializes one Python mod, described by a `{"id", "name",
 * "dependencies"}` dict from `get_mod_load_order`, within its ownership
 * scope in `framework` (the imported `modcore` module). */
bool load_mod(PyObject *framework, PyObject *info);

#endif // HLMOD_MOD_LOADER_H
