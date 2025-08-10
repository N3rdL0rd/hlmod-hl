#include <stdio.h>
#include <hlmod.h>
#include <hlmod_hooks.h>

/**
 * The C hook that will be called from the JIT-compiled code.
 *
 * @param findex The function index.
 * @param nargs  The number of arguments being passed.
 * @param args   An array of pointers, where each element points to an argument's
 *               location on the stack. For Haxe values (Int, Float), it's a pointer
 *               to the value. For Haxe pointers (String, Object), it's a pointer
 *               to the pointer.
 */
void hlmod_dispatch_hook(int findex, int nargs, void** args) {
    hl_blocking(true);

    hl_function* f = g_runtime_module->code->functions + g_runtime_module->functions_indexes[findex];
    hl_type_fun* fun_type = f->type->fun;

    //printf("[hlmod] Intercepted call to function f@%d with %d args\n",
    //    findex, nargs);

    for (int i = 0; i < nargs; i++) {
        hl_type* arg_type = fun_type->args[i];
        void* p_arg_value = args[i];

        switch (arg_type->kind) {
            default:
                printf("[hlmod] [WARN] Unknown arg %d [type %d]: %p\n", i, arg_type->kind, *(void**)p_arg_value);
                break;
        }
    }
    
    HookRegistryEntry* entry;
    HASH_FIND_INT(g_hook_registry, &findex, entry);

    if (entry != NULL) {
        if (!PyCallable_Check(entry->callback)) {
            fprintf(stderr, "[hlmod] [ERROR] Hook object is not callable!\n");
            return;
        }


        PyObject* pResult = PyObject_CallFunctionObjArgs(entry->callback, NULL);

		if (pResult == NULL) {
			PyErr_Print();
		}
        
        Py_DECREF(pResult);
    }

    hl_blocking(false);
}