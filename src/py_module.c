/* The `hlmod` built-in Python module's manifest: every native function it
 * exposes to Python mods, plus its two extension types. The functions
 * themselves are implemented across hlmod.c, hlmod_python.c, native_hook.c,
 * and hook_registry.c; this file only wires them together. */

#include "py_module.h"

#include <hl.h>
#include <hlmod.h>
#include <hlmod_python.h>
#include "native_hook.h"

static PyMethodDef HlmodMethods[] = {
    {"register_hook", hlmod_py_register_hook, METH_VARARGS, "Hooks a function by its findex."},
    {"unregister_hook", hlmod_py_unregister_hook, METH_VARARGS, "Remove a hook, optionally only if its callback is identical."},
    {"register_hlobj", hlmod_py_register_hlobj, METH_VARARGS, "Registers a Python class for a given Haxe type index."},
    {"create_subclass", hlmod_py_create_subclass, METH_VARARGS, "Register a native HL subtype backed by a Python class."},
    {"alloc_obj", hlmod_py_alloc_obj, METH_VARARGS, "Allocate an instance of an HL object type."},
    {"init_obj", hlmod_py_init_obj, METH_VARARGS, "Initialize a preallocated instance with its native constructor."},
    {"bind_instance", hlmod_py_bind_instance, METH_VARARGS, "Bind native object identity to its Python instance."},
    {"make_callback", hlmod_py_make_callback, METH_VARARGS, "Create an HL closure from a Python callable and signature."},
    {"array_new", hlmod_py_array_new, METH_VARARGS, "Create a typed HL native array from element type index and iterable."},
    {"array_length", hlmod_py_array_length, METH_VARARGS, "Return a native array's length."},
    {"array_get", hlmod_py_array_get, METH_VARARGS, "Read a native array element."},
    {"array_set", hlmod_py_array_set, METH_VARARGS, "Write a native array element."},
    {"array_element_type", hlmod_py_array_element_type, METH_VARARGS, "Return the element type index or None."},
    {"enum_info", hlmod_py_enum_info, METH_VARARGS, "Inspect a native enum constructor and parameters."},
    {"enum_new", hlmod_py_enum_new, METH_VARARGS, "Construct a typed native enum value."},
    {"dynobj_new", hlmod_py_dynobj_new, METH_VARARGS, "Allocate a native dynamic object."},
    {"dynobj_keys", hlmod_py_dynobj_keys, METH_VARARGS, "List native dynamic object fields."},
    {"dynobj_get", hlmod_py_dynobj_get, METH_VARARGS, "Read a native dynamic object field."},
    {"dynobj_set", hlmod_py_dynobj_set, METH_VARARGS, "Write a native dynamic object field."},
    {"dynobj_delete", hlmod_py_dynobj_delete, METH_VARARGS, "Delete a native dynamic object field."},
    {"ref_new", hlmod_py_ref_new, METH_VARARGS, "Allocate a typed native reference."},
    {"ref_get", hlmod_py_ref_get, METH_VARARGS, "Read a native reference."},
    {"ref_set", hlmod_py_ref_set, METH_VARARGS, "Write a native reference."},
    {"inspect_native", hlmod_py_inspect_native, METH_VARARGS, "Inspect native type fields and methods."},
    {"bytes_new", hlmod_py_bytes_new, METH_VARARGS, "Allocate zeroed native bytes."},
    {"bytes_from", hlmod_py_bytes_from, METH_VARARGS, "Copy a Python buffer into native bytes."},
    {"bytes_capacity", hlmod_py_bytes_capacity, METH_VARARGS, "Allocation size of native bytes, or None."},
    {"bytes_read", hlmod_py_bytes_read, METH_VARARGS, "Read a bounded range of native bytes."},
    {"bytes_write", hlmod_py_bytes_write, METH_VARARGS, "Write a bounded range of native bytes."},
    {"get_obj_field", hlmod_py_get_obj_field, METH_VARARGS, "Gets a field value from a Haxe object."},
    {"set_obj_field", hlmod_py_set_obj_field, METH_VARARGS, "Sets a field value on a Haxe object."},
    {"get_virtual_field", hlmod_py_get_virtual_field, METH_VARARGS, "Gets a field value from a Haxe virtual object."},
    {"set_virtual_field", hlmod_py_set_virtual_field, METH_VARARGS, "Sets a field value on a Haxe virtual object."},
    {"get_virtual_field_count", hlmod_py_get_virtual_field_count, METH_VARARGS, "Gets the field count for a Haxe virtual object."},
    {"get_virtual_field_name", hlmod_py_get_virtual_field_name, METH_VARARGS, "Gets the field name for a Haxe virtual object field index."},
    {"set_fixed_prng", hlmod_py_set_fixed_prng, METH_VARARGS, "Sets the PRNG to a fixed or random state."},
    {"get_fixed_prng", hlmod_py_get_fixed_prng, METH_NOARGS, "Gets whether the PRNG is in a fixed state."},
    {"assert_code_sha", hlmod_py_assert_code_sha, METH_VARARGS, "Asserts the bytecode SHA256, exiting if it mismatches."},
    {"get_global", hlmod_py_get_global, METH_VARARGS, "Gets the global instance of a type by index. Useful for static types."},
    {"ensure_global", hlmod_py_ensure_global, METH_VARARGS, "Ensures the global instance of a type by index is allocated, returning it."},
    {"call", hlmod_py_call, METH_VARARGS, "Calls an HL function by findex."},
    {"call_closure", hlmod_py_call_closure, METH_VARARGS, "Calls an HL closure by pointer."},
    {"dump_stack", hlmod_py_dump_stack, METH_NOARGS, "Dumps the current HL stack."},
    {"findex_for_name", hlmod_py_findex_for_name, METH_VARARGS, "Gets the findex for a specific function by its name"},
    {"type_index_for_name", hlmod_py_type_index_for_name, METH_VARARGS, "Gets the bytecode type index of an obj/struct/enum by its full name."},
    {"native_findex", hlmod_py_native_findex, METH_VARARGS, "Gets the findex of a @:hlNative function by its (lib, name)."},
    {"native_hook_test_prologue", hlmod_py_native_hook_test_prologue, METH_VARARGS,
        "Testing hook: decodes a raw prologue buffer with the native-hook engine's own instruction decoder."},
    {"profile_start", hlmod_py_profile_start, METH_VARARGS, "Starts the HL sampling profiler at the given samples/sec (default 1000)."},
    {"profile_end", hlmod_py_profile_end, METH_NOARGS, "Stops the profiler and writes hlprofile.dump."},
    {"gc_major", hlmod_py_gc_major, METH_NOARGS, "Forces a full GC collection cycle."},
    {"gc_stats", hlmod_py_gc_stats, METH_NOARGS, "Returns a dict of total_allocated/allocation_count/current_memory."},
    {"gc_enable", hlmod_py_gc_enable, METH_VARARGS, "Enables or disables the GC."},
    {"is_gc_ptr", hlmod_py_is_gc_ptr, METH_VARARGS, "Returns whether an HlPtr points into GC-managed memory."},
    {"gc_memsize", hlmod_py_gc_memsize, METH_VARARGS, "Returns the GC allocation size of an HlPtr, or None if not GC-managed."},
    {NULL, NULL, 0, NULL}
};
static struct PyModuleDef hlmod_module_def = {
    PyModuleDef_HEAD_INIT,
    "hlmod",                        // The name of the module in Python
    "Low-level hlmod framework API.", // Module's docstring
    -1,
    HlmodMethods                    // Link to the method table
};
PyMODINIT_FUNC PyInit_hlmod(void) {
    if (PyType_Ready(&HlPtrType) < 0)
        return NULL;
    if (PyType_Ready(&HlHookType) < 0)
        return NULL;

    PyObject* m = PyModule_Create(&hlmod_module_def);
    if (m == NULL)
        return NULL;

    if (PyModule_AddObjectRef(m, "HlPtr", (PyObject *)&HlPtrType) < 0 ||
        PyModule_AddObjectRef(m, "Hook", (PyObject *)&HlHookType) < 0 ||
        PyModule_AddStringConstant(m, "version", HLMOD_VERSION) < 0) {
        Py_DECREF(m);
        return NULL;
    }

    return m;
}
