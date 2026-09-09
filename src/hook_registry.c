/* Findex-indexed hook publication registry: a slot per bytecode function or
 * native, tracking whether a Python hook is installed and its callback.
 * Reads from the JIT's no-GIL hot path (`hlmod_hook_registered`,
 * `hlmod_hook_callback`) use the atomic helpers in hlmod_python.h; writers
 * always hold the GIL. */

#include <Python.h>
#include <hl.h>
#include <hlmod.h>
#include <hlmod_python.h>
#include "native_hook.h"

typedef struct {
    int registered;
    PyObject *callback; /* GIL protected; dispatch takes its own reference. */
} HookSlot;

static HookSlot *hook_slots;
static int hook_slot_count;

/* Init/dispose run outside the lifetime of native callers. Slots never move. */
int hlmod_hook_registry_init(int count) {
    if(hook_slots || count < 0) {
        PyErr_SetString(PyExc_RuntimeError, "Invalid hook registry initialization");
        return -1;
    }
    hook_slots = calloc(count ? count : 1, sizeof(*hook_slots));
    if(!hook_slots) {
        PyErr_NoMemory();
        return -1;
    }
    hook_slot_count = count;
    return 0;
}

bool hlmod_hook_registered(int findex) {
    return findex >= 0 && findex < hook_slot_count &&
        hlmod_atomic_load_int(&hook_slots[findex].registered) != 0;
}

PyObject *hlmod_hook_callback(int findex) {
    if(findex < 0 || findex >= hook_slot_count) return NULL;
    return Py_XNewRef(hook_slots[findex].callback);
}

void hlmod_hook_registry_shutdown(void) {
    /* Unpublish all callbacks before decrefs can reenter the registry. */
    int count = hook_slot_count;
    hook_slot_count = 0;
    for(int i = 0; i < count; i++) Py_CLEAR(hook_slots[i].callback);
    free(hook_slots);
    hook_slots = NULL;
}

void hlmod_register_hook(int findex, PyObject* callback) {
    if(g_module == NULL || !hook_slots) {
        PyErr_SetString(PyExc_RuntimeError, "HL hook registry is not initialized");
        return;
    }
    if(findex < 0 || findex >= hook_slot_count) {
        PyErr_SetString(PyExc_IndexError, "Function index is out of bounds");
        return;
    }
    int index = g_module->functions_indexes[findex];
    bool is_native = index >= g_module->code->nfunctions;
    if(index < 0 || index >= g_module->code->nfunctions + g_module->code->nnatives) {
        PyErr_SetString(PyExc_ValueError, "Unknown function index");
        return;
    }
    if(!PyCallable_Check(callback)) {
        PyErr_SetString(PyExc_TypeError, "Hook callback must be callable");
        return;
    }
    HookSlot *slot = &hook_slots[findex];
    if(slot->callback) {
        if(slot->callback != callback)
            PyErr_SetString(PyExc_ValueError, "Function already has a different hook callback");
        return;
    }
    if(is_native) {
        hl_native *native = &g_module->code->natives[index - g_module->code->nfunctions];
        if(hlmod_native_hook_ensure_installed(findex, native->t) != 0) return;
    }
    slot->callback = Py_NewRef(callback);
    hlmod_atomic_store_int(&slot->registered, 1);
}

PyObject *hlmod_py_unregister_hook(PyObject *self, PyObject *args) {
    int findex;
    PyObject *callback = Py_None;
    if(!PyArg_ParseTuple(args, "i|O:unregister_hook", &findex, &callback)) return NULL;
    if(findex < 0 || findex >= hook_slot_count) Py_RETURN_FALSE;
    HookSlot *slot = &hook_slots[findex];
    if(!slot->callback || (callback != Py_None && callback != slot->callback)) Py_RETURN_FALSE;
    /* Readers that already saw true recheck under the GIL, never dereference
       a callback from the no-GIL path. In-flight calls own their callback. */
    hlmod_atomic_store_int(&slot->registered, 0);
    Py_CLEAR(slot->callback);
    Py_RETURN_TRUE;
}

PyObject *hlmod_py_register_hook(PyObject *self, PyObject *args) {
    int findex;
    PyObject* callback;

	// int, PyObject*
    if (!PyArg_ParseTuple(args, "iO", &findex, &callback)) {
        return NULL;
    }

    if (!PyCallable_Check(callback)) {
        PyErr_SetString(PyExc_TypeError, "Second argument must be a Callable!");
        return NULL;
    }

    hlmod_register_hook(findex, callback);
    if (PyErr_Occurred()) return NULL;

    Py_RETURN_NONE;
}
