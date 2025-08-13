#include <stdio.h>
#include <hl.h>
#include <hlmod.h>
#include <Python.h>
#include <structmember.h>

THREAD_LOCAL int64 g_return_value_int = 0;
THREAD_LOCAL double g_return_value_double = 0.0;
THREAD_LOCAL bool g_is_passthrough_call = false;

#pragma region HlPtr
static PyObject* HlPtr_New(void* ptr, int kind);
static PyObject* HlPtr_get_ptr(HlPtr* self, void* closure);
static PyObject* HlPtr_get_kind(HlPtr* self, void* closure);

static PyGetSetDef HlPtr_getsetters[] = {
    {"ptr", (getter)HlPtr_get_ptr, NULL, "The raw pointer value", NULL},
    {"kind", (getter)HlPtr_get_kind, NULL, "The HL type kind enum", NULL},
    {NULL}
};

PyTypeObject HlPtrType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "hlmod.HlPtr",
    .tp_doc = "A wrapper for a Haxe pointer and its type kind.",
    .tp_basicsize = sizeof(HlPtr),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = PyType_GenericNew,
    .tp_getset = HlPtr_getsetters,
};

static PyObject* HlPtr_New(void* ptr, int kind) {
    HlPtr* self = (HlPtr*)HlPtrType.tp_alloc(&HlPtrType, 0);
    if (self != NULL) {
        self->ptr = ptr;
        self->kind = kind;
    }
    return (PyObject*)self;
}

static PyObject* HlPtr_get_ptr(HlPtr* self, void* closure) {
    return PyLong_FromVoidPtr(self->ptr);
}
static PyObject* HlPtr_get_kind(HlPtr* self, void* closure) {
    return PyLong_FromLong(self->kind);
}

#pragma region HlHook
static PyObject* HlHook_call_original(HlHook* self, PyObject* py_args) {
    hl_function* f = g_runtime_module->code->functions + g_runtime_module->functions_indexes[self->findex];
    hl_type_fun* fun_type = f->type->fun;
    int nargs = fun_type->nargs;

    if (PyTuple_Size(py_args) != nargs) {
        PyErr_Format(PyExc_TypeError, "call_original() expected %d arguments, but got %zd", nargs, PyTuple_Size(py_args));
        return NULL;
    }

    vdynamic* vargs[HL_MAX_ARGS];
    if (nargs > HL_MAX_ARGS) {
        PyErr_SetString(PyExc_ValueError, "Too many arguments for call_original");
        return NULL;
    }

    for (int i = 0; i < nargs; i++) {
        PyObject* py_arg = PyTuple_GetItem(py_args, i);
        hl_type* hl_arg_type = fun_type->args[i];

        void* hl_val_ptr = hlmod_cast_to_hl(py_arg, hl_arg_type);
        if (hl_val_ptr == NULL) {
            return NULL;
        }

        vargs[i] = hl_make_dyn(hl_val_ptr, hl_arg_type);
    }
    
    vclosure cl;
    cl.t = f->type;
    cl.fun = g_runtime_module->functions_ptrs[self->findex];
    cl.hasValue = 0;

    g_is_passthrough_call = true;
    bool is_exc;
    vdynamic* hl_result = hl_dyn_call_safe(&cl, nargs > 0 ? vargs : NULL, nargs, &is_exc);
    g_is_passthrough_call = false;

    if (is_exc) {
        PyErr_SetString(PyExc_RuntimeError, "An exception occurred in the original Haxe function.");
        return NULL;
    }

    if (fun_type->ret->kind == HVOID) {
        Py_RETURN_NONE;
    }

    PyObject* py_result = hlmod_cast_to_py(fun_type->ret, &hl_result->v);
    
    return py_result;
}

static PyMethodDef HlHook_methods[] = {
    {"call_original", (PyCFunction)HlHook_call_original, METH_VARARGS, "Calls the original Haxe function."},
    {NULL}
};

PyTypeObject HlHookType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "hlmod.Hook",
    .tp_doc = "Hook context object",
    .tp_basicsize = sizeof(HlHook),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = PyType_GenericNew,
    .tp_methods = HlHook_methods,
};


#pragma region Casting

/**
 * @brief Cast an HL value to Python as a PyObject
 * 
 * @param type The type of the HL value
 * @param ptr A pointer to the HL value
 * @returns a PyObject* that is a casted version of the HL value
 */
PyObject* hlmod_cast_to_py(hl_type* type, void* ptr) {
    void* obj_ptr = *(void**)ptr;
    if (obj_ptr == NULL) {
        Py_RETURN_NONE;
    }
    if (type == NULL) {
        fprintf(stderr, "[hlmod] [ERROR] [hl->py] Received NULL type in cast function.\n");
        Py_RETURN_NONE;
    }

    switch (type->kind) {
        case HF64:
            return PyFloat_FromDouble( *(double*)ptr );
        case HF32:
            return PyFloat_FromDouble( (double)(*(float*)ptr) );
        case HI32:
            return PyLong_FromLong( *(int*)ptr );
        case HBOOL:
            return PyBool_FromLong( *(bool*)ptr );
        case HUI16:
            return PyLong_FromLong( *(unsigned short*)ptr );
        case HUI8:
            return PyLong_FromLong( *(unsigned char*)ptr );

        default:
            return HlPtr_New(obj_ptr, type->kind);
    }

    printf("[hlmod] [ERROR] [hl->py] Something goofed!\n");
    Py_RETURN_NONE;
}

void* hlmod_cast_to_hl(PyObject* obj, hl_type* type) {
    if (obj == Py_None) {
        hl_null_access();
        return NULL;
    }

    if (type == NULL) {
        fprintf(stderr, "[hlmod] [ERROR] [py->hl] Received NULL type in cast function.\n");
        return NULL;
    }

    void* ptr = NULL;

    switch (type->kind) {
        case HF64: {
            double val = PyFloat_AsDouble(obj);
            if (PyErr_Occurred()) return NULL;
            double* mem = (double*)hl_gc_alloc_noptr(sizeof(double));
            *mem = val;
            ptr = mem;
            break;
        }
        case HF32: {
            double val = PyFloat_AsDouble(obj);
            if (PyErr_Occurred()) return NULL;
            float* mem = (float*)hl_gc_alloc_noptr(sizeof(float));
            *mem = (float)val;
            ptr = mem;
            break;
        }
        case HI32: {
            long val = PyLong_AsLong(obj);
            if (PyErr_Occurred()) return NULL;
            int* mem = (int*)hl_gc_alloc_noptr(sizeof(int));
            *mem = (int)val;
            ptr = mem;
            break;
        }
        case HBOOL: {
            int val = PyObject_IsTrue(obj);
            if (val == -1) return NULL;
            bool* mem = (bool*)hl_gc_alloc_noptr(sizeof(bool));
            *mem = (bool)val;
            ptr = mem;
            break;
        }
        case HUI16: {
            unsigned long val = PyLong_AsUnsignedLong(obj);
            if (PyErr_Occurred()) return NULL;
            unsigned short* mem = (unsigned short*)hl_gc_alloc_noptr(sizeof(unsigned short));
            *mem = (unsigned short)val;
            ptr = mem;
            break;
        }
        case HUI8: {
            unsigned long val = PyLong_AsUnsignedLong(obj);
            if (PyErr_Occurred()) return NULL;
            unsigned char* mem = (unsigned char*)hl_gc_alloc_noptr(sizeof(unsigned char));
            *mem = (unsigned char)val;
            ptr = mem;
            break;
        }
        default: {
            void* inner_ptr;
            if (Py_IS_TYPE(obj, &HlPtrType)) {
                inner_ptr = ((HlPtr*)obj)->ptr;
            } else if (PyLong_Check(obj)) {
                inner_ptr = PyLong_AsVoidPtr(obj);
                if (PyErr_Occurred()) return NULL;
            } else {
                PyErr_Format(PyExc_TypeError, "Expected an hlmod.HlPtr or an int pointer, but got %s", Py_TYPE(obj)->tp_name);
                return NULL;
            }

            void** outer_ptr = (void**)hl_gc_alloc_raw(sizeof(void*));
            *outer_ptr = inner_ptr;
            ptr = outer_ptr;
            break;
        }
    }

    return ptr;
}

#pragma region JIT hook
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

int jit_dispatch_hook(int findex, int nargs, void** args) {
    if (g_is_passthrough_call) {
        g_is_passthrough_call = false;
        return 0;
    }

    HookRegistryEntry* entry;
    HASH_FIND_INT(g_hook_registry, &findex, entry);

    g_return_value_int = 0;
    g_return_value_double = 0.0;

    if (entry == NULL) {
        return 0;
    }

    hl_blocking(true);
    PyGILState_STATE gstate;
    gstate = PyGILState_Ensure();

    hl_function* f = g_runtime_module->code->functions + g_runtime_module->functions_indexes[findex];
    hl_type_fun* fun_type = f->type->fun;

    //printf("[hlmod] Intercepted call to function f@%d with %d args\n",
    //    findex, nargs);

    PyObject* pArgs = PyTuple_New(nargs + 1); // for hook
    if (!pArgs) {
        PyErr_Print();
        PyGILState_Release(gstate);
        hl_blocking(false);
        return 0;
    }

    HlHook* hook_obj = (HlHook*)HlHookType.tp_new(&HlHookType, NULL, NULL);
    if (!hook_obj) {
        PyErr_Print();
        Py_DECREF(pArgs);
        PyGILState_Release(gstate);
        hl_blocking(false);
        return 0;
    }
    hook_obj->findex = findex;
    PyTuple_SetItem(pArgs, 0, (PyObject*)hook_obj);

    for (int i = 0; i < nargs; i++) {
        PyObject* pValue = hlmod_cast_to_py(fun_type->args[i], args[i]);
        if (pValue == NULL) {
            PyErr_Print();
            Py_DECREF(pArgs);
            PyGILState_Release(gstate);
            hl_blocking(false);
            return 0;
        }

        PyTuple_SetItem(pArgs, i + 1, pValue);
    }

    PyObject* pResult = PyObject_CallObject(entry->callback, pArgs);
    Py_DECREF(pArgs);

    if (pResult == NULL) {
        PyErr_Print();

        PyGILState_Release(gstate);
        hl_blocking(false);
        return 0;
    } else {
        int res = 1;
        if (Py_IsNone(pResult) != 1) {
            if (f->type->fun->ret->kind == HF32 || f->type->fun->ret->kind == HF64) {
                g_return_value_double = PyFloat_AsDouble(pResult);
                if (PyErr_Occurred()) {
                    PyErr_Print();
                }
            } else {
                void** temp_ptr = (void**)hlmod_cast_to_hl(pResult, fun_type->ret);
                if (temp_ptr != NULL) {
                    g_return_value_int = (int64)(*temp_ptr);
                }
            }
            res = 1;
            // printf("[hlmod] Patching return!\n");
        }
        Py_DECREF(pResult);
        PyGILState_Release(gstate);
        hl_blocking(false);
        return res;
    }
}