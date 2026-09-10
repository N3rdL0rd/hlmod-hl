#include "hlmod_internal.h"
#include <hlmod_python.h>
#include <std_globals.h>
#include <limits.h>
#include <string.h>

#pragma region other python-side utils

PyObject *hlmod_py_get_fixed_prng(PyObject *self, PyObject *args)
{
    if (g_fixed_prng)
    {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

PyObject *hlmod_py_set_fixed_prng(PyObject *self, PyObject *args)
{
    PyObject *py_val;

    if (!PyArg_ParseTuple(args, "O", &py_val))
    {
        return NULL;
    }

    if (!PyBool_Check(py_val))
    {
        PyErr_SetString(PyExc_TypeError, "Argument must be a boolean (True or False).");
        return NULL;
    }

    if (py_val == Py_True)
    {
        g_fixed_prng = true;
    }
    else
    {
        g_fixed_prng = false;
    }

    Py_RETURN_NONE;
}

PyObject *hlmod_py_assert_code_sha(PyObject* self, PyObject* args)
{
    const char* expected_sha;

    if (!PyArg_ParseTuple(args, "s", &expected_sha)) {
        return NULL;
    }

    printf("[hlmod] Expecting SHA256: %s\n", expected_sha);

    if (strcmp(g_code_sha256, expected_sha) != 0) {
        fprintf(stderr, "\n[hlmod] FATAL ERROR: Bytecode SHA256 mismatch!\n");
        fprintf(stderr, "  Expected: %s\n", expected_sha);
        fprintf(stderr, "  Actual:   %s\n", g_code_sha256);
        fprintf(stderr, "  This mod is not compatible with this version of the game. Halting.\n");
        fflush(stderr);
        exit(1);
    }

    Py_RETURN_NONE;
}

PyObject *hlmod_py_get_global(PyObject* self, PyObject* args)
{
    int type_index;

    if (!PyArg_ParseTuple(args, "i", &type_index))
    {
        return NULL;
    }

    if (g_module == NULL || g_module->code == NULL)
    {
        PyErr_SetString(PyExc_RuntimeError, "hlmod is not initialized.");
        return NULL;
    }

    if (type_index < 0 || type_index >= g_module->code->ntypes)
    {
        PyErr_Format(PyExc_IndexError, "Type index %d is out of bounds.", type_index);
        return NULL;
    }

    hl_type *target_type = &g_module->code->types[type_index];
    if ((target_type->kind == HOBJ || target_type->kind == HSTRUCT) &&
        target_type->obj != NULL && target_type->obj->global_value != NULL)
    {
        return hlmod_cast_to_py(target_type, target_type->obj->global_value);
    }

    for (int i = 0; i < g_module->code->nglobals; i++)
    {
        hl_type *current_global_type = g_module->code->globals[i];

        if (current_global_type == target_type || hl_same_type(current_global_type, target_type))
        {
            // printf("Found global g@%i\n", i);
            void *addr = g_module->globals_data + g_module->globals_indexes[i];
            // printf("addr: %p\n", addr);
            return hlmod_cast_to_py(target_type, addr);
        }
    }

    Py_RETURN_NONE;
}

PyObject *hlmod_py_ensure_global(PyObject* self, PyObject* args)
{
    int type_index;

    if (!PyArg_ParseTuple(args, "i", &type_index))
    {
        return NULL;
    }

    if (g_module == NULL || g_module->code == NULL)
    {
        PyErr_SetString(PyExc_RuntimeError, "hlmod is not initialized.");
        return NULL;
    }

    if (type_index < 0 || type_index >= g_module->code->ntypes)
    {
        PyErr_Format(PyExc_IndexError, "Type index %d is out of bounds.", type_index);
        return NULL;
    }

    hl_type *target_type = &g_module->code->types[type_index];
    if ((target_type->kind == HOBJ || target_type->kind == HSTRUCT) &&
        target_type->obj != NULL && target_type->obj->global_value != NULL &&
        *(void **)target_type->obj->global_value != NULL)
    {
        return hlmod_cast_to_py(target_type, target_type->obj->global_value);
    }

    for (int i = 0; i < g_module->code->nglobals; i++)
    {
        hl_type *current_global_type = g_module->code->globals[i];

        if (current_global_type == target_type || hl_same_type(current_global_type, target_type))
        {
            void *addr = g_module->globals_data + g_module->globals_indexes[i];
            if ((target_type->kind == HOBJ || target_type->kind == HSTRUCT) &&
                target_type->obj != NULL && *(void **)addr != NULL)
            {
                return hlmod_cast_to_py(target_type, addr);
            }
        }
    }

    Py_RETURN_NONE;
}

PyObject *hlmod_py_dump_stack(PyObject *self, PyObject *args)
{
    if( hl_get_thread() != NULL ) {
		hl_dump_stack();
	} else {
        printf("[hlmod] No active HL thread!\n");
    }
    Py_RETURN_NONE;
}
/* Exported by gc.c but, like the reflection primitives above, never
 * declared in any shared header. */
HL_API void hl_gc_stats( double *total_allocated, double *allocation_count, double *current_memory );
HL_API void hl_gc_enable( bool b );

PyObject *hlmod_py_gc_major(PyObject *self, PyObject *args)
{
    Py_BEGIN_ALLOW_THREADS
    hl_gc_major();
    Py_END_ALLOW_THREADS
    Py_RETURN_NONE;
}

PyObject *hlmod_py_gc_stats(PyObject *self, PyObject *args)
{
    double total_allocated, allocation_count, current_memory;
    hl_gc_stats(&total_allocated, &allocation_count, &current_memory);
    return Py_BuildValue("{s:d,s:d,s:d}",
        "total_allocated", total_allocated,
        "allocation_count", allocation_count,
        "current_memory", current_memory);
}

PyObject *hlmod_py_gc_enable(PyObject *self, PyObject *args)
{
    int enabled;
    if (!PyArg_ParseTuple(args, "p", &enabled)) return NULL;
    hl_gc_enable(enabled != 0);
    Py_RETURN_NONE;
}

PyObject *hlmod_py_is_gc_ptr(PyObject *self, PyObject *args)
{
    HlPtr *pointer;
    if (!PyArg_ParseTuple(args, "O!", &HlPtrType, &pointer)) return NULL;
    if (hl_is_gc_ptr(pointer->ptr)) Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}

PyObject *hlmod_py_gc_memsize(PyObject *self, PyObject *args)
{
    HlPtr *pointer;
    if (!PyArg_ParseTuple(args, "O!", &HlPtrType, &pointer)) return NULL;
    if (!hl_is_gc_ptr(pointer->ptr)) Py_RETURN_NONE;
    return PyLong_FromLong(hl_gc_get_memsize(pointer->ptr));
}


PyObject *hlmod_py_profile_start(PyObject *self, PyObject *args)
{
    int sample_count = 1000;
    if (!PyArg_ParseTuple(args, "|i", &sample_count))
        return NULL;
    if (sample_count <= 0) {
        PyErr_SetString(PyExc_ValueError, "sample_count must be positive");
        return NULL;
    }
    Py_BEGIN_ALLOW_THREADS
    hl_profile_setup(sample_count);
    Py_END_ALLOW_THREADS
    Py_RETURN_NONE;
}

PyObject *hlmod_py_profile_end(PyObject *self, PyObject *args)
{
    const char *path = NULL;
    if (!PyArg_ParseTuple(args, "|z", &path))
        return NULL;
    Py_BEGIN_ALLOW_THREADS
    hl_profile_end();
    Py_END_ALLOW_THREADS
    Py_RETURN_NONE;
}

PyObject *hlmod_py_findex_for_name(PyObject *self, PyObject *args)
{
    const char *name;
    
    if (!PyArg_ParseTuple(args, "s", &name)) {
        return NULL;
    }
    if (!g_code) {
        PyErr_SetString(PyExc_RuntimeError, "hlmod is not initialized.");
        return NULL;
    }

    for (int i = 0; i < g_code->nfunctions; i++) {
        hl_function *f = &g_code->functions[i];

        if (f->obj == NULL || f->obj->name == NULL || f->field.name == NULL) {
            continue;
        }

        const char *class_u8 = (char*)hl_to_utf8(f->obj->name);
        const char *method_u8 = (char*)hl_to_utf8(f->field.name);

        char res[1024];
        snprintf(res, sizeof(res), "%s.%s", class_u8, method_u8);

        if (strcmp(res, name) == 0) {
            return PyLong_FromLong(f->findex);
        }
    }
    PyErr_SetString(PyExc_NameError, "No such function!");
    return NULL;
}

#pragma endregion
#pragma region Call

PyObject *hlmod_py_call(PyObject *self, PyObject *args)
{
    int findex;
    PyObject *arguments;
    if (!PyArg_ParseTuple(args, "iO!", &findex, &PyTuple_Type, &arguments)) return NULL;
    hl_type *type = hlmod_function_type(findex);
    if (!type) return NULL;
    vclosure closure = {0};
    closure.t = type;
    closure.fun = g_module->functions_ptrs[findex];
    return hlmod_invoke(&closure, arguments, -1, findex);
}

#pragma endregion
#pragma region HL-side closures

PyObject *hlmod_py_call_closure(PyObject *self, PyObject *args)
{
    HlPtr *pointer;
    PyObject *arguments;
    if (!PyArg_ParseTuple(args, "O!O!", &HlPtrType, &pointer, &PyTuple_Type, &arguments)) return NULL;
    vclosure *closure = hlmod_require_pointer(pointer, HFUN);
    if (!closure) return NULL;
    return hlmod_invoke(closure, arguments, -1, -1);
}


#pragma endregion
const char *kind2str(hl_type_kind kind) {
    switch(kind) {
        case HVOID:
            return "void";
        case HUI8:
            return "u8";
        case HUI16:
            return "u16";
        case HI32:
            return "i32";
        case HI64:
            return "i64";
        case HF32:
            return "f32";
        case HF64:
            return "f64";
        case HBOOL:
            return "bool";
        case HBYTES:
            return "bytes";
        case HDYN:
            return "dyn";
        case HFUN:
            return "fun";
        case HOBJ:
            return "obj";
        case HARRAY:
            return "array";
        case HTYPE:
            return "type";
        case HREF:
            return "ref";
        case HVIRTUAL:
            return "virtual";
        case HDYNOBJ:
            return "dynobj";
        case HABSTRACT:
            return "abstract";
        case HENUM:
            return "enum";
        case HNULL:
            return "null";
        case HMETHOD:
            return "method";
        case HSTRUCT:
            return "struct";
        case HPACKED:
            return "packed";
        case HGUID:
            return "guid";
    }
    return "unknown";
}

/* Reflection primitives exported by std/obj.c, but not declared in hl.h. */
HL_API vdynamic *hl_obj_get_field(vdynamic *obj, int hfield);
HL_API void hl_obj_set_field(vdynamic *obj, int hfield, vdynamic *value);
HL_API bool hl_obj_delete_field(vdynamic *obj, int hfield);

static hl_type *hlmod_indexed_type(int index)
{
    if (!g_code || index < 0 || index >= g_code->ntypes) {
        PyErr_SetString(PyExc_IndexError, "Native type index is out of bounds.");
        return NULL;
    }
    return &g_code->types[index];
}

static bool hlmod_value_layout(hl_type *type)
{
    if (!type || type->kind == HVOID || type->kind == HSTRUCT ||
        type->kind == HPACKED || type->kind == HGUID || type->kind == HMETHOD ||
        hl_type_size(type) <= 0) {
        PyErr_SetString(PyExc_TypeError, "Native value layout is unavailable or unsupported.");
        return false;
    }
    return true;
}

static PyObject *hlmod_unicode(const uchar *value)
{
    if (!value) Py_RETURN_NONE;
    int byteorder = -1;
    return PyUnicode_DecodeUTF16((const char *)value, ustrlen(value) * sizeof(uchar), "strict", &byteorder);
}

/* Set a newly allocated value and release it on both success and failure. */
static int hlmod_metadata_set(PyObject *dict, const char *key, PyObject *value)
{
    if (!value) return -1;
    int result = PyDict_SetItemString(dict, key, value);
    Py_DECREF(value);
    return result;
}

static PyObject *hlmod_type_descriptor(hl_type *type)
{
    PyObject *result = PyDict_New();
    if (!result) return NULL;
    int index = hlmod_type_index(type);
    if (hlmod_metadata_set(result, "type_index", index < 0 ? Py_NewRef(Py_None) : PyLong_FromLong(index)) < 0 ||
        hlmod_metadata_set(result, "kind", PyUnicode_FromString(kind2str(type->kind))) < 0 ||
        hlmod_metadata_set(result, "name", hlmod_unicode(hl_type_str(type))) < 0) {
        Py_DECREF(result);
        return NULL;
    }
    return result;
}

static hl_enum_construct *hlmod_enum_constructor(hl_type *type, int index)
{
    if (!type || type->kind != HENUM || !type->tenum) {
        PyErr_SetString(PyExc_TypeError, "Expected an enum type with constructor metadata.");
        return NULL;
    }
    if (index < 0 || index >= type->tenum->nconstructs) {
        PyErr_SetString(PyExc_IndexError, "Enum constructor index is out of bounds.");
        return NULL;
    }
    hl_enum_construct *constructor = &type->tenum->constructs[index];
    if (constructor->size < (int)(sizeof(void *) + sizeof(int)) ||
        (constructor->nparams && (!constructor->params || !constructor->offsets))) {
        PyErr_SetString(PyExc_TypeError, "Enum constructor layout is unavailable.");
        return NULL;
    }
    for (int i = 0; i < constructor->nparams; i++) {
        if (!hlmod_value_layout(constructor->params[i])) return NULL;
        int offset = constructor->offsets[i], size = hl_type_size(constructor->params[i]);
        if (offset < (int)(sizeof(void *) + sizeof(int)) || offset > constructor->size - size) {
            PyErr_SetString(PyExc_TypeError, "Enum parameter layout is invalid.");
            return NULL;
        }
    }
    return constructor;
}

PyObject *hlmod_py_enum_info(PyObject *self, PyObject *args)
{
    HlPtr *pointer;
    if (!PyArg_ParseTuple(args, "O!", &HlPtrType, &pointer)) return NULL;
    venum *value = hlmod_require_pointer(pointer, HENUM);
    if (!value) return NULL;
    hl_enum_construct *constructor = hlmod_enum_constructor(pointer->type, value->index);
    if (!constructor) return NULL;
    PyObject *parameters = PyTuple_New(constructor->nparams);
    if (!parameters) return NULL;
    for (int i = 0; i < constructor->nparams; i++) {
        PyObject *parameter = hlmod_cast_to_py(constructor->params[i], (char *)value + constructor->offsets[i]);
        if (!parameter) { Py_DECREF(parameters); return NULL; }
        PyTuple_SET_ITEM(parameters, i, parameter);
    }
    PyObject *name = hlmod_unicode(constructor->name);
    PyObject *result = name ? Py_BuildValue("{s:i,s:O,s:O}", "constructor_index", value->index,
        "constructor_name", name, "parameters", parameters) : NULL;
    Py_XDECREF(name);
    Py_DECREF(parameters);
    return result;
}

PyObject *hlmod_py_enum_new(PyObject *self, PyObject *args)
{
    int type_index, constructor_index;
    PyObject *parameters;
    if (!PyArg_ParseTuple(args, "iiO", &type_index, &constructor_index, &parameters)) return NULL;
    hl_type *type = hlmod_indexed_type(type_index);
    if (!type) return NULL;
    hl_enum_construct *constructor = hlmod_enum_constructor(type, constructor_index);
    if (!constructor) return NULL;
    PyObject *sequence = PySequence_Fast(parameters, "Enum parameters must be iterable.");
    if (!sequence) return NULL;
    if (PySequence_Fast_GET_SIZE(sequence) != constructor->nparams) {
        Py_DECREF(sequence);
        PyErr_Format(PyExc_TypeError, "Enum constructor expects %d parameters.", constructor->nparams);
        return NULL;
    }
    venum *value = hl_alloc_enum(type, constructor_index);
    PyObject *pointer = hlmod_ptr_new(value, type);
    if (!pointer) { Py_DECREF(sequence); return NULL; }
    for (int i = 0; i < constructor->nparams; i++) {
        PyObject *item = PySequence_GetItem(sequence, i);
        void *slot = item ? hlmod_cast_to_hl(item, constructor->params[i]) : NULL;
        Py_XDECREF(item);
        if (!slot) { Py_DECREF(pointer); Py_DECREF(sequence); return NULL; }
        memcpy((char *)value + constructor->offsets[i], slot, hl_type_size(constructor->params[i]));
    }
    Py_DECREF(sequence);
    return pointer;
}

/* HL bytes carry no logical length. The GC block size is the only real bound,
   so foreign (non-GC) buffers stay unreadable rather than guessed. */
static vbyte *hlmod_bytes(HlPtr *pointer, int *capacity)
{
    vbyte *bytes = hlmod_require_pointer(pointer, HBYTES);
    if (!bytes) return NULL;
    int size = hl_is_gc_ptr(bytes) ? hl_gc_get_memsize(bytes) : -1;
    if (size < 0) {
        PyErr_SetString(PyExc_TypeError, "This bytes pointer has no known allocation size; "
            "copy it with an explicit length from native code instead.");
        return NULL;
    }
    *capacity = size;
    return bytes;
}

static int hlmod_bytes_range(int capacity, Py_ssize_t offset, Py_ssize_t length)
{
    if (offset < 0 || length < 0 || offset > capacity || length > capacity - offset) {
        PyErr_Format(PyExc_IndexError, "Bytes range [%zd, %zd) is outside the %d byte allocation.",
            offset, offset + length, capacity);
        return -1;
    }
    return 0;
}

PyObject *hlmod_py_bytes_new(PyObject *self, PyObject *args)
{
    Py_ssize_t size;
    if (!PyArg_ParseTuple(args, "n:bytes_new", &size)) return NULL;
    if (size < 0 || size > INT_MAX) {
        PyErr_SetString(PyExc_ValueError, "Bytes size must fit in a positive HL allocation.");
        return NULL;
    }
    vbyte *bytes = hl_gc_alloc_noptr((int)size ? (int)size : 1);
    if (!bytes) return PyErr_NoMemory();
    memset(bytes, 0, (size_t)((int)size ? size : 1));
    return hlmod_ptr_new(bytes, &hlt_bytes);
}

PyObject *hlmod_py_bytes_from(PyObject *self, PyObject *args)
{
    Py_buffer view;
    if (!PyArg_ParseTuple(args, "y*:bytes_from", &view)) return NULL;
    if (view.len > INT_MAX) {
        PyBuffer_Release(&view);
        PyErr_SetString(PyExc_OverflowError, "Buffer exceeds the HL allocation limit.");
        return NULL;
    }
    vbyte *bytes = hl_gc_alloc_noptr(view.len ? (int)view.len : 1);
    PyObject *pointer = bytes ? hlmod_ptr_new(bytes, &hlt_bytes) : PyErr_NoMemory();
    if (pointer) memcpy(bytes, view.buf, (size_t)view.len);
    PyBuffer_Release(&view);
    return pointer;
}

PyObject *hlmod_py_bytes_capacity(PyObject *self, PyObject *args)
{
    HlPtr *pointer;
    if (!PyArg_ParseTuple(args, "O!:bytes_capacity", &HlPtrType, &pointer)) return NULL;
    vbyte *bytes = hlmod_require_pointer(pointer, HBYTES);
    if (!bytes) return NULL;
    int size = hl_is_gc_ptr(bytes) ? hl_gc_get_memsize(bytes) : -1;
    if (size < 0) Py_RETURN_NONE;
    return PyLong_FromLong(size);
}

PyObject *hlmod_py_bytes_read(PyObject *self, PyObject *args)
{
    HlPtr *pointer;
    Py_ssize_t offset, length;
    if (!PyArg_ParseTuple(args, "O!nn:bytes_read", &HlPtrType, &pointer, &offset, &length)) return NULL;
    int capacity;
    vbyte *bytes = hlmod_bytes(pointer, &capacity);
    if (!bytes || hlmod_bytes_range(capacity, offset, length) < 0) return NULL;
    return PyBytes_FromStringAndSize((const char *)bytes + offset, length);
}

PyObject *hlmod_py_bytes_write(PyObject *self, PyObject *args)
{
    HlPtr *pointer;
    Py_ssize_t offset;
    Py_buffer view;
    if (!PyArg_ParseTuple(args, "O!ny*:bytes_write", &HlPtrType, &pointer, &offset, &view)) return NULL;
    int capacity;
    vbyte *bytes = hlmod_bytes(pointer, &capacity);
    if (!bytes || hlmod_bytes_range(capacity, offset, view.len) < 0) {
        PyBuffer_Release(&view);
        return NULL;
    }
    memcpy(bytes + offset, view.buf, (size_t)view.len);
    PyBuffer_Release(&view);
    Py_RETURN_NONE;
}

static int hlmod_field_hash(PyObject *key, int *hash)
{
    if (!PyUnicode_Check(key)) {
        PyErr_SetString(PyExc_TypeError, "Dynamic field names must be strings.");
        return -1;
    }
    if (PyUnicode_FindChar(key, 0, 0, PyUnicode_GET_LENGTH(key), 1) >= 0) {
        PyErr_SetString(PyExc_ValueError, "Dynamic field names cannot contain NUL.");
        return -1;
    }
    PyObject *encoded = PyUnicode_AsEncodedString(key, "utf-16-le", "strict");
    if (!encoded) return -1;
    Py_ssize_t size = PyBytes_GET_SIZE(encoded);
    uchar *name = PyMem_Malloc(size + sizeof(uchar));
    if (!name) { Py_DECREF(encoded); PyErr_NoMemory(); return -1; }
    memcpy(name, PyBytes_AS_STRING(encoded), size);
    name[size / sizeof(uchar)] = 0;
    *hash = hl_hash_gen(name, true);
    PyMem_Free(name);
    Py_DECREF(encoded);
    return 0;
}

PyObject *hlmod_py_dynobj_new(PyObject *self, PyObject *args)
{
    if (!PyArg_ParseTuple(args, "")) return NULL;
    vdynobj *value = hl_alloc_dynobj();
    return hlmod_ptr_new(value, value->t);
}

PyObject *hlmod_py_dynobj_keys(PyObject *self, PyObject *args)
{
    HlPtr *pointer;
    if (!PyArg_ParseTuple(args, "O!", &HlPtrType, &pointer)) return NULL;
    vdynobj *value = hlmod_require_pointer(pointer, HDYNOBJ);
    if (!value) return NULL;
    PyObject *keys = PyTuple_New(value->nfields);
    if (!keys) return NULL;
    for (int i = 0; i < value->nfields; i++) {
        PyObject *name = hlmod_unicode((const uchar *)hl_field_name(value->lookup[i].hashed_name));
        if (!name) { Py_DECREF(keys); return NULL; }
        PyTuple_SET_ITEM(keys, i, name);
    }
    return keys;
}

PyObject *hlmod_py_dynobj_get(PyObject *self, PyObject *args)
{
    HlPtr *pointer;
    PyObject *key;
    int hash;
    if (!PyArg_ParseTuple(args, "O!O", &HlPtrType, &pointer, &key)) return NULL;
    vdynobj *value = hlmod_require_pointer(pointer, HDYNOBJ);
    if (!value || hlmod_field_hash(key, &hash) < 0) return NULL;
    if (!hl_lookup_find(value->lookup, value->nfields, hash)) {
        PyErr_SetObject(PyExc_KeyError, key);
        return NULL;
    }
    hl_trap_ctx trap;
    vdynamic *exception;
    hl_trap(trap, exception, failed);
    vdynamic *result = hl_obj_get_field((vdynamic *)value, hash);
    hl_endtrap(trap);
    return hlmod_cast_to_py(&hlt_dyn, &result);
failed:
    hl_endtrap(trap);
    PyErr_Format(PyExc_TypeError, "Cannot read dynamic field: %s", hl_to_utf8(hl_to_string(exception)));
    return NULL;
}

PyObject *hlmod_py_dynobj_set(PyObject *self, PyObject *args)
{
    HlPtr *pointer;
    PyObject *key, *item;
    int hash;
    if (!PyArg_ParseTuple(args, "O!OO", &HlPtrType, &pointer, &key, &item)) return NULL;
    vdynobj *value = hlmod_require_pointer(pointer, HDYNOBJ);
    if (!value || hlmod_field_hash(key, &hash) < 0) return NULL;
    /* Dynamic deliberately does not guess callable signatures. */
    void *slot = hlmod_cast_to_hl(item, &hlt_dyn);
    if (!slot) return NULL;
    hl_trap_ctx trap;
    vdynamic *exception;
    hl_trap(trap, exception, failed);
    hl_obj_set_field((vdynamic *)value, hash, *(vdynamic **)slot);
    hl_endtrap(trap);
    Py_RETURN_NONE;
failed:
    hl_endtrap(trap);
    PyErr_Format(PyExc_TypeError, "Cannot write dynamic field: %s", hl_to_utf8(hl_to_string(exception)));
    return NULL;
}

PyObject *hlmod_py_dynobj_delete(PyObject *self, PyObject *args)
{
    HlPtr *pointer;
    PyObject *key;
    int hash;
    if (!PyArg_ParseTuple(args, "O!O", &HlPtrType, &pointer, &key)) return NULL;
    vdynobj *value = hlmod_require_pointer(pointer, HDYNOBJ);
    if (!value || hlmod_field_hash(key, &hash) < 0) return NULL;
    if (!hl_obj_delete_field((vdynamic *)value, hash)) {
        PyErr_SetObject(PyExc_KeyError, key);
        return NULL;
    }
    Py_RETURN_NONE;
}

static void *hlmod_ref(HlPtr *pointer)
{
    void *value = hlmod_require_pointer(pointer, HREF);
    if (!value || !hlmod_value_layout(pointer->type->tparam)) return NULL;
    if (!pointer->root) {
        PyErr_SetString(PyExc_TypeError, "Stack-backed HL references have no safe escaping lifetime.");
        return NULL;
    }
    return value;
}

PyObject *hlmod_py_ref_new(PyObject *self, PyObject *args)
{
    int index;
    PyObject *value;
    if (!PyArg_ParseTuple(args, "iO", &index, &value)) return NULL;
    hl_type *type = hlmod_indexed_type(index);
    if (!type) return NULL;
    if (type->kind != HREF) {
        PyErr_SetString(PyExc_TypeError, "Reference creation requires an explicit HREF type index.");
        return NULL;
    }
    if (!hlmod_value_layout(type->tparam)) return NULL;
    void *slot = hlmod_cast_to_hl(value, type->tparam);
    return slot ? hlmod_ptr_new(slot, type) : NULL;
}

PyObject *hlmod_py_ref_get(PyObject *self, PyObject *args)
{
    HlPtr *pointer;
    if (!PyArg_ParseTuple(args, "O!", &HlPtrType, &pointer)) return NULL;
    void *value = hlmod_ref(pointer);
    return value ? hlmod_cast_to_py(pointer->type->tparam, value) : NULL;
}

PyObject *hlmod_py_ref_set(PyObject *self, PyObject *args)
{
    HlPtr *pointer;
    PyObject *item;
    if (!PyArg_ParseTuple(args, "O!O", &HlPtrType, &pointer, &item)) return NULL;
    void *value = hlmod_ref(pointer);
    if (!value) return NULL;
    void *slot = hlmod_cast_to_hl(item, pointer->type->tparam);
    if (!slot) return NULL;
    memcpy(value, slot, hl_type_size(pointer->type->tparam));
    Py_RETURN_NONE;
}

static PyObject *hlmod_type_descriptors(hl_type **types, int count)
{
    PyObject *result = PyTuple_New(count);
    if (!result) return NULL;
    for (int i = 0; i < count; i++) {
        PyObject *item = hlmod_type_descriptor(types[i]);
        if (!item) { Py_DECREF(result); return NULL; }
        PyTuple_SET_ITEM(result, i, item);
    }
    return result;
}

static int hlmod_inspect_field(PyObject *fields, const uchar *name, int index, hl_type *type, hl_type *owner)
{
    PyObject *field = PyDict_New();
    if (!field) return -1;
    int result = -1;
    if (hlmod_metadata_set(field, "name", hlmod_unicode(name)) < 0 ||
        hlmod_metadata_set(field, "index", PyLong_FromLong(index)) < 0 ||
        hlmod_metadata_set(field, "type", hlmod_type_descriptor(type)) < 0 ||
        hlmod_metadata_set(field, "declaring_type_index", hlmod_type_index(owner) < 0 ? Py_NewRef(Py_None) : PyLong_FromLong(hlmod_type_index(owner))) < 0)
        goto done;
    result = PyList_Append(fields, field);
done:
    Py_DECREF(field);
    return result;
}

PyObject *hlmod_py_inspect_native(PyObject *self, PyObject *args)
{
    PyObject *value;
    if (!PyArg_ParseTuple(args, "O", &value)) return NULL;
    HlPtr *pointer = NULL;
    hl_type *type;
    if (PyLong_Check(value) && !PyBool_Check(value)) {
        long index = PyLong_AsLong(value);
        if (PyErr_Occurred()) return NULL;
        if (index < 0 || index > INT_MAX) {
            PyErr_SetString(PyExc_IndexError, "Native type index is out of bounds.");
            return NULL;
        }
        type = hlmod_indexed_type((int)index);
        if (!type) return NULL;
    } else {
        pointer = hlmod_extract_pointer(value);
        if (!pointer) return NULL;
        type = pointer->type;
        if (pointer->ptr && hl_is_dynamic(type)) type = ((vdynamic *)pointer->ptr)->t;
    }
    PyObject *result = hlmod_type_descriptor(type);
    PyObject *fields = PyList_New(0), *methods = PyList_New(0);
    if (!result || !fields || !methods) goto failed;
    if (type->kind == HOBJ || type->kind == HSTRUCT) {
        hl_type *current = type;
        int depth = 0;
        while (current && depth++ < HLMOD_MAX_INHERITANCE) {
            hl_type_obj *obj = current->obj;
            hl_runtime_obj *rt = hl_get_obj_rt(current);
            for (int i = 0; i < obj->nfields; i++) {
                hl_obj_field *field = &obj->fields[i];
                if (hlmod_inspect_field(fields, field->name, rt->nfields - obj->nfields + i, field->t, current) < 0) goto failed;
            }
            for (int i = 0; i < obj->nproto; i++) {
                hl_obj_proto *proto = &obj->proto[i];
                hl_type *signature = hlmod_function_type(proto->findex);
                if (!signature) goto failed;
                PyObject *method = PyDict_New();
                if (!method) goto failed;
                int status = hlmod_metadata_set(method, "name", hlmod_unicode(proto->name));
                if (!status) status = hlmod_metadata_set(method, "findex", PyLong_FromLong(proto->findex));
                if (!status) status = hlmod_metadata_set(method, "declaring_type_index", hlmod_type_index(current) < 0 ? Py_NewRef(Py_None) : PyLong_FromLong(hlmod_type_index(current)));
                if (!status) status = hlmod_metadata_set(method, "type", hlmod_type_descriptor(signature));
                if (!status) status = hlmod_metadata_set(method, "arguments", hlmod_type_descriptors(signature->fun->args, signature->fun->nargs));
                if (!status) status = hlmod_metadata_set(method, "return_type", hlmod_type_descriptor(signature->fun->ret));
                if (!status) status = PyList_Append(methods, method);
                Py_DECREF(method);
                if (status < 0) goto failed;
            }
            current = obj->super;
        }
        if (current) {
            PyErr_SetString(PyExc_TypeError, "Native inheritance exceeds the inspection depth limit.");
            goto failed;
        }
    } else if (type->kind == HVIRTUAL) {
        for (int i = 0; i < type->virt->nfields; i++) {
            hl_obj_field *field = &type->virt->fields[i];
            if (hlmod_inspect_field(fields, field->name, i, field->t, type) < 0) goto failed;
        }
    } else if (type->kind == HDYNOBJ && pointer && pointer->ptr) {
        vdynobj *obj = pointer->ptr;
        for (int i = 0; i < obj->nfields; i++) {
            hl_field_lookup *field = &obj->lookup[i];
            if (hlmod_inspect_field(fields, (const uchar *)hl_field_name(field->hashed_name), i, field->t, type) < 0) goto failed;
        }
    } else if (type->kind == HENUM) {
        PyObject *constructors = PyList_New(0);
        if (!constructors) goto failed;
        for (int i = 0; i < type->tenum->nconstructs; i++) {
            hl_enum_construct *constructor = &type->tenum->constructs[i];
            PyObject *entry = PyDict_New();
            if (!entry) { Py_DECREF(constructors); goto failed; }
            int status = hlmod_metadata_set(entry, "name", hlmod_unicode(constructor->name));
            if (!status) status = hlmod_metadata_set(entry, "index", PyLong_FromLong(i));
            if (!status) status = hlmod_metadata_set(entry, "parameters", hlmod_type_descriptors(constructor->params, constructor->nparams));
            if (!status) status = PyList_Append(constructors, entry);
            Py_DECREF(entry);
            if (status < 0) { Py_DECREF(constructors); goto failed; }
        }
        if (hlmod_metadata_set(result, "constructors", constructors) < 0) goto failed;
    }
    if (type->kind == HREF || type->kind == HNULL || type->kind == HPACKED) {
        if (hlmod_metadata_set(result, "element_type", hlmod_type_descriptor(type->tparam)) < 0) goto failed;
    } else if (type->kind == HARRAY && pointer && pointer->ptr) {
        if (hlmod_metadata_set(result, "element_type", hlmod_type_descriptor(((varray *)pointer->ptr)->at)) < 0) goto failed;
    } else if (type->kind == HFUN || type->kind == HMETHOD) {
        if (hlmod_metadata_set(result, "arguments", hlmod_type_descriptors(type->fun->args, type->fun->nargs)) < 0 ||
            hlmod_metadata_set(result, "return_type", hlmod_type_descriptor(type->fun->ret)) < 0) goto failed;
    }
    if (PyDict_SetItemString(result, "fields", fields) < 0 || PyDict_SetItemString(result, "methods", methods) < 0) goto failed;
    Py_DECREF(fields);
    Py_DECREF(methods);
    Py_XDECREF(pointer);
    return result;
failed:
    Py_XDECREF(result);
    Py_XDECREF(fields);
    Py_XDECREF(methods);
    Py_XDECREF(pointer);
    return NULL;
}
