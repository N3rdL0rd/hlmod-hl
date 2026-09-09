#include "mod_loader.h"

#include <hlmod_embedded.h>
#include <stdio.h>

/**
 * @brief Calls a Python function from a script string to determine the mod load order.
 *
 * @param mods_dir The directory where the .py mod files are located.
 * @param load_order_list A pointer to a PyObject* that will receive the list of mods to load.
 * @return 1 on success (and load_order_list is populated), 0 on failure.
 * The caller is responsible for DECREF'ing the returned list.
 */
int get_mod_load_order(const char *mods_dir, PyObject **load_order_list) {
    *load_order_list = NULL;
    PyObject *module = PyImport_AddModule("_hlmod_mod_sorter");
    if (module == NULL) goto error;
    PyObject *compiled = Py_CompileString(hlmod_mod_sorter_source, "<hlmod>/mod_sorter.py", Py_file_input);
    if (compiled == NULL) goto error;
    PyObject *globals = PyModule_GetDict(module);
    PyObject *result = PyEval_EvalCode(compiled, globals, globals);
    Py_DECREF(compiled);
    if (result == NULL) goto error;
    Py_DECREF(result);
    result = PyObject_CallMethod(module, "resolve_mod_order", "s", mods_dir);
    if (result == NULL) goto error;
    if (!PyDict_Check(result)) {
        Py_DECREF(result);
        PyErr_SetString(PyExc_TypeError, "Mod resolver did not return a dictionary");
        goto error;
    }
    PyObject *status = PyDict_GetItemString(result, "status");
    if (status != NULL && PyUnicode_Check(status) && PyUnicode_CompareWithASCIIString(status, "ok") == 0) {
        PyObject *order = PyDict_GetItemString(result, "order");
        if (order != NULL && PyList_Check(order)) {
            *load_order_list = Py_NewRef(order);
            Py_DECREF(result);
            return 1;
        }
        PyErr_SetString(PyExc_TypeError, "Mod resolver did not return a load-order list");
    } else {
        PyObject *message = PyDict_GetItemString(result, "message");
        PyErr_SetObject(PyExc_RuntimeError, message != NULL ? message : Py_None);
    }
    Py_DECREF(result);
error:
    PyErr_Print();
    return 0;
}

/**
 * @brief Loads and initializes a Python mod within its ownership scope.
 */
bool load_mod(PyObject *framework, PyObject *info) {
    PyObject *id = PyDict_GetItemString(info, "id");
    PyObject *name = PyDict_GetItemString(info, "name");
    PyObject *dependencies = PyDict_GetItemString(info, "dependencies");
    if (id == NULL || name == NULL || dependencies == NULL) {
        PyErr_SetString(PyExc_ValueError, "Incomplete mod resolver entry");
        PyErr_Print();
        return false;
    }
    const char *module_name = PyUnicode_AsUTF8(name);
    if (module_name == NULL) { PyErr_Print(); return false; }
    printf("    -> Loading `%s`\n", module_name);
    PyObject *mod = PyObject_CallMethod(framework, "load_mod", "OOO", id, name, dependencies);
    if (mod == NULL) {
        PyErr_Print();
        fprintf(stderr, "      [!] Error: Failed to load mod '%s'\n", module_name);
        return false;
    }
    Py_DECREF(mod);
    return true;
}
