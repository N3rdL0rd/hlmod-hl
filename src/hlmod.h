#ifndef HLMOD_H
#define HLMOD_H

#include <Python.h>
#include "uthash.h"
#include <hl.h>
#include <hlmodule.h>

#define HL_MAX_ARGS 64
#define HLMOD_MAX_INHERITANCE 128
#define HLMOD_VERSION "0.0.1a"
#define HLMOD_DEBUG

typedef struct {
    PyObject_HEAD
    void* ptr;
    int kind;
    hl_type *type; /* NULL for explicitly constructed, opaque raw pointers. */
    void **root;
} HlPtr;

extern PyTypeObject HlPtrType;

typedef struct {
    PyObject_HEAD
    int findex;
} HlHook;

EXPORT int64_t hlmod_get_return_int();
EXPORT double hlmod_get_return_double();

extern PyTypeObject HlHookType;

PyObject* hlmod_py_register_hlobj(PyObject *self, PyObject *args);
PyObject* hlmod_py_get_obj_field(PyObject *self, PyObject *args);
PyObject* hlmod_py_set_obj_field(PyObject *self, PyObject *args);
PyObject* hlmod_py_get_virtual_field(PyObject *self, PyObject *args);
PyObject* hlmod_py_set_virtual_field(PyObject *self, PyObject *args);
PyObject* hlmod_py_get_virtual_field_count(PyObject *self, PyObject *args);
PyObject* hlmod_py_get_virtual_field_name(PyObject *self, PyObject *args);

PyObject* hlmod_py_set_fixed_prng(PyObject *self, PyObject *args);
PyObject* hlmod_py_get_fixed_prng(PyObject *self, PyObject *args);

PyObject *hlmod_py_assert_code_sha(PyObject* self, PyObject* args);

PyObject *hlmod_py_call(PyObject *self, PyObject *args);
PyObject *hlmod_py_call_closure(PyObject *self, PyObject *args);
PyObject *hlmod_py_get_global(PyObject* self, PyObject* args);
PyObject *hlmod_py_ensure_global(PyObject* self, PyObject* args);
PyObject *hlmod_py_dump_stack(PyObject *self, PyObject *args);
PyObject *hlmod_py_findex_for_name(PyObject *self, PyObject *args);
PyObject *hlmod_py_profile_start(PyObject *self, PyObject *args);
PyObject *hlmod_py_profile_end(PyObject *self, PyObject *args);

extern THREAD_LOCAL int64_t g_return_value_int;
extern THREAD_LOCAL double g_return_value_double;

int jit_dispatch_hook(int findex, int nargs, void** args);
void* hlmod_cast_to_hl(PyObject* obj, hl_type* type);
PyObject* hlmod_cast_to_py(hl_type* type, void* ptr);
PyObject *hlmod_ptr_new(void *ptr, hl_type *type);
void hlmod_shutdown(void);
PyObject *hlmod_py_array_new(PyObject *self, PyObject *args);
PyObject *hlmod_py_array_length(PyObject *self, PyObject *args);
PyObject *hlmod_py_array_get(PyObject *self, PyObject *args);
PyObject *hlmod_py_array_set(PyObject *self, PyObject *args);
PyObject *hlmod_py_array_element_type(PyObject *self, PyObject *args);
PyObject *hlmod_py_enum_info(PyObject *self, PyObject *args);
PyObject *hlmod_py_enum_new(PyObject *self, PyObject *args);
PyObject *hlmod_py_dynobj_new(PyObject *self, PyObject *args);
PyObject *hlmod_py_dynobj_keys(PyObject *self, PyObject *args);
PyObject *hlmod_py_dynobj_get(PyObject *self, PyObject *args);
PyObject *hlmod_py_dynobj_set(PyObject *self, PyObject *args);
PyObject *hlmod_py_dynobj_delete(PyObject *self, PyObject *args);
PyObject *hlmod_py_ref_new(PyObject *self, PyObject *args);
PyObject *hlmod_py_ref_get(PyObject *self, PyObject *args);
PyObject *hlmod_py_ref_set(PyObject *self, PyObject *args);
PyObject *hlmod_py_inspect_native(PyObject *self, PyObject *args);

extern hl_module *g_module;
extern hl_code *g_code;

extern char g_code_sha256[65];

void hlmod_register_hook(int findex, PyObject* callback);
const char *kind2str(hl_type_kind kind);

#endif // HLMOD_H
