#include "hlmod_internal.h"
#include <hlmod_python.h>

/**
 * @brief The C hook that will be called from the JIT-compiled code.
 *
 * @param findex The function index.
 * @param nargs  The number of arguments being passed.
 * @param args   An array of pointers, where each element points to an argument's
 *               location on the stack. For Haxe values (Int, Float), it's a pointer
 *               to the value. For Haxe pointers (String, Object), it's a pointer
 *               to the pointer.
 * @return 1 if the function should return early with a new value.
 * @return 0 if the original function logic should continue.
 */

int jit_dispatch_hook(int findex, int nargs, void **args)
{
    if (is_passthrough(findex)) return 0;
    if (hlmod_python_dispatch(findex, nargs, args)) return 1;
    if (!hlmod_hook_registered(findex)) return 0;

    /* Only the wait for the GIL is blocking: conversions allocate in the HL GC. */
    hl_blocking(true);
    PyGILState_STATE gstate = PyGILState_Ensure();
    hl_blocking(false);
    PyObject *callback = hlmod_hook_callback(findex);
    if (callback == NULL) {
        PyGILState_Release(gstate);
        return 0;
    }
    int64_t saved_int = g_return_value_int;
    double saved_double = g_return_value_double;
    int64_t result_int = 0;
    double result_double = 0;
    void *result_pointer = NULL;
    bool pointer_rooted = false;
    int handled = 0;
    bool failed = false;
    char *error = NULL;
    PyObject *arguments = NULL, *result = NULL;
    hl_type *type = hlmod_function_type(findex);
    if (!type) goto done;
    hl_type_fun *fun = type->fun;
    if (nargs < 0 || nargs > HL_MAX_ARGS || nargs != fun->nargs || (nargs && !args)) {
        PyErr_SetString(PyExc_ValueError, "Invalid JIT hook argument metadata.");
        goto done;
    }
    arguments = PyTuple_New(nargs + 1);
    if (!arguments) goto done;
    HlHook *hook = (HlHook *)HlHookType.tp_alloc(&HlHookType, 0);
    if (!hook) goto done;
    hook->findex = findex;
    PyTuple_SET_ITEM(arguments, 0, (PyObject *)hook);
    for (int i = 0; i < nargs; i++) {
        PyObject *value = hlmod_cast_to_py(fun->args[i], args[i]);
        if (!value) goto done;
        PyTuple_SET_ITEM(arguments, i + 1, value);
    }
    result = PyObject_CallObject(callback, arguments);
    if (!result) goto done;
    if (fun->ret->kind != HVOID) {
        void *slot = hlmod_cast_to_hl(result, fun->ret);
        if (!slot) goto done;
        switch (fun->ret->kind) {
        case HF64: result_double = *(double *)slot; break;
        case HF32: result_double = *(float *)slot; break;
        case HI64: result_int = *(int64 *)slot; break;
        case HI32: result_int = *(int *)slot; break;
        case HUI16: result_int = *(unsigned short *)slot; break;
        case HUI8: result_int = *(unsigned char *)slot; break;
        case HBOOL: result_int = *(bool *)slot; break;
        default:
            result_pointer = *(void **)slot;
            result_int = (int64_t)(intptr_t)result_pointer;
            hl_add_root(&result_pointer);
            pointer_rooted = true;
            break;
        }
    }
    handled = 1;
done:
    if (PyErr_Occurred() || !handled) {
        failed = true;
        error = hlmod_python_take_error();
    }
    Py_XDECREF(result);
    Py_XDECREF(arguments);
    Py_DECREF(callback);
    g_return_value_int = failed ? saved_int : result_int;
    g_return_value_double = failed ? saved_double : result_double;
    hl_blocking(true);
    PyGILState_Release(gstate);
    hl_blocking(false);
    if (pointer_rooted) hl_remove_root(&result_pointer);
    if (failed) hlmod_python_throw_error(error);
    return handled;
}
