#include "hlmod_internal.h"

#include <string.h>

#pragma region Field Access

PyObject *hlmod_py_get_obj_field(PyObject *self, PyObject *args)
{
    PyObject *hlobj_ptr;
    int field_index;

    if (!PyArg_ParseTuple(args, "O!i", &HlPtrType, &hlobj_ptr, &field_index))
    {
        return NULL;
    }

    vobj *obj = hlmod_require_pointer((HlPtr *)hlobj_ptr, HOBJ);
    if (!obj) return NULL;

    hl_runtime_obj *rt = hl_get_obj_rt(obj->t);

    if (field_index < 0 || field_index >= rt->nfields)
    {
        PyErr_Format(PyExc_IndexError, "Field index %d is out of bounds (0 to %d).",
                     field_index, rt->nfields);
        return NULL;
    }

    hl_obj_field* field_info = hl_obj_field_fetch(obj->t, field_index);
    if (field_info == NULL) {
        PyErr_Format(PyExc_IndexError, "Could not fetch field info for index %d.", field_index);
        return NULL;
    }
    hl_type *field_type = field_info->t;
    int field_offset = rt->fields_indexes[field_index];

    void *field_ptr = (char *)obj + field_offset;

    return hlmod_cast_to_py(field_type, field_ptr);
}

PyObject *hlmod_py_set_obj_field(PyObject *self, PyObject *args)
{
    PyObject *hlobj_ptr;
    int field_index;
    PyObject *py_value;

    if (!PyArg_ParseTuple(args, "O!iO", &HlPtrType, &hlobj_ptr, &field_index, &py_value))
    {
        return NULL;
    }

    vobj *obj = hlmod_require_pointer((HlPtr *)hlobj_ptr, HOBJ);
    if (!obj) return NULL;

    hl_runtime_obj *rt = hl_get_obj_rt(obj->t);

    if (field_index < 0 || field_index >= rt->nfields)
    {
        PyErr_Format(PyExc_IndexError, "Field index %d is out of bounds for type '%s' (0-%d).",
                     field_index, (char *)hl_to_utf8(obj->t->obj->name), rt->nfields - 1);
        return NULL;
    }

    hl_obj_field* field_info = hl_obj_field_fetch(obj->t, field_index);
    if (field_info == NULL) {
        PyErr_Format(PyExc_IndexError, "Could not fetch field info for index %d.", field_index);
        return NULL;
    }
    hl_type *field_type = field_info->t;
    int field_offset = rt->fields_indexes[field_index];

    void *field_ptr = (char *)obj + field_offset;
    void *hl_value_ptr = hlmod_cast_to_hl(py_value, field_type);
    if (hl_value_ptr == NULL)
    {
        return NULL;
    }

    memcpy(field_ptr, hl_value_ptr, hl_type_size(field_type));

    Py_RETURN_NONE;
}

static hl_type *hlmod_virtual_callable_type(hl_type *type)
{
    if (type->kind == HFUN) return type;
    for (HlMethodSignature *item = g_method_signatures; item; item = item->next)
        if (item->method == type) return &item->callable;
    if (!g_code) {
        PyErr_SetString(PyExc_RuntimeError, "hlmod type metadata is unavailable.");
        return NULL;
    }
    /* HMETHOD already excludes its receiver, but is not a closure value type.
       Keep its HFUN view alive as long as bytecode, including escaped callbacks. */
    HlMethodSignature *item = hl_malloc(&g_code->alloc, sizeof(*item));
    item->method = type;
    item->callable = *type;
    item->callable.kind = HFUN;
    item->next = g_method_signatures;
    g_method_signatures = item;
    return &item->callable;
}

static PyObject *hlmod_virtual_get_callable(vvirtual *virt, hl_obj_field *field)
{
    hl_type *signature = hlmod_virtual_callable_type(field->t);
    if (!signature) return NULL;
    hl_trap_ctx trap;
    vdynamic *exception;
    hl_trap(trap, exception, failed);
    /* The runtime binds prototype code pointers to their backing receiver and
       also handles mutable closure slots on objects and dynamic records. */
    vdynamic *value = hl_dyn_getp((vdynamic *)virt, field->hashed_name, &hlt_dyn);
    hl_endtrap(trap);
    if (!value) Py_RETURN_NONE;
    if (value->t->kind != HFUN || !hl_safe_cast(value->t, signature)) {
        PyErr_SetString(PyExc_TypeError, "Virtual field does not contain a compatible callable.");
        return NULL;
    }
    return hlmod_py_make_hlcallable((vclosure *)value);
failed:
    hl_endtrap(trap);
    PyErr_Format(PyExc_TypeError, "Cannot read virtual callable: %s", hl_to_utf8(hl_to_string(exception)));
    return NULL;
}

static PyObject *hlmod_virtual_set_callable(vvirtual *virt, hl_obj_field *field, PyObject *value)
{
    hl_type *signature = hlmod_virtual_callable_type(field->t);
    if (!signature) return NULL;
    void *slot = hlmod_cast_to_hl(value, signature);
    if (!slot) return NULL;
    hl_trap_ctx trap;
    vdynamic *exception;
    hl_trap(trap, exception, failed);
    /* Never write to hl_vfields for methods: those entries can be code, not
       storage. The runtime setter updates mutable backing fields/remaps views,
       and rejects immutable prototype methods. */
    hl_dyn_setp((vdynamic *)virt, field->hashed_name, signature, *(void **)slot);
    hl_endtrap(trap);
    Py_RETURN_NONE;
failed:
    hl_endtrap(trap);
    PyErr_Format(PyExc_TypeError, "Cannot write virtual callable: %s", hl_to_utf8(hl_to_string(exception)));
    return NULL;
}

PyObject *hlmod_py_get_virtual_field(PyObject *self, PyObject *args)
{
    PyObject *hlvirt_ptr;
    int field_index;

    if (!PyArg_ParseTuple(args, "O!i", &HlPtrType, &hlvirt_ptr, &field_index))
    {
        return NULL;
    }

    vvirtual *virt = hlmod_require_pointer((HlPtr *)hlvirt_ptr, HVIRTUAL);
    if (!virt) return NULL;
    if (virt->t == NULL || virt->t->kind != HVIRTUAL || virt->t->virt == NULL)
    {
        PyErr_SetString(PyExc_TypeError, "HlPtr does not point to a valid Haxe virtual object.");
        return NULL;
    }
    if (field_index < 0 || field_index >= virt->t->virt->nfields)
    {
        PyErr_Format(PyExc_IndexError, "Virtual field index %d is out of bounds (0 to %d).",
                     field_index, virt->t->virt->nfields - 1);
        return NULL;
    }

    hl_obj_field *field_info = &virt->t->virt->fields[field_index];
    if (field_info->t->kind == HFUN || field_info->t->kind == HMETHOD)
    {
        return hlmod_virtual_get_callable(virt, field_info);
    }

    void *field_ptr = hl_vfields(virt)[field_index];
    if (field_ptr == NULL)
    {
        Py_RETURN_NONE;
    }

    return hlmod_cast_to_py(field_info->t, field_ptr);
}

PyObject *hlmod_py_set_virtual_field(PyObject *self, PyObject *args)
{
    PyObject *hlvirt_ptr;
    int field_index;
    PyObject *py_value;

    if (!PyArg_ParseTuple(args, "O!iO", &HlPtrType, &hlvirt_ptr, &field_index, &py_value))
    {
        return NULL;
    }

    vvirtual *virt = hlmod_require_pointer((HlPtr *)hlvirt_ptr, HVIRTUAL);
    if (!virt) return NULL;
    if (virt->t == NULL || virt->t->kind != HVIRTUAL || virt->t->virt == NULL)
    {
        PyErr_SetString(PyExc_TypeError, "HlPtr does not point to a valid Haxe virtual object.");
        return NULL;
    }
    if (field_index < 0 || field_index >= virt->t->virt->nfields)
    {
        PyErr_Format(PyExc_IndexError, "Virtual field index %d is out of bounds (0 to %d).",
                     field_index, virt->t->virt->nfields - 1);
        return NULL;
    }

    hl_obj_field *field_info = &virt->t->virt->fields[field_index];
    if (field_info->t->kind == HFUN || field_info->t->kind == HMETHOD)
    {
        return hlmod_virtual_set_callable(virt, field_info, py_value);
    }

    void *field_ptr = hl_vfields(virt)[field_index];
    if (field_ptr == NULL)
    {
        PyErr_SetString(PyExc_ValueError, "Virtual field is currently unavailable on this value.");
        return NULL;
    }

    void *hl_value_ptr = hlmod_cast_to_hl(py_value, field_info->t);
    if (hl_value_ptr == NULL)
    {
        return NULL;
    }

    memcpy(field_ptr, hl_value_ptr, hl_type_size(field_info->t));

    Py_RETURN_NONE;
}

PyObject *hlmod_py_get_virtual_field_count(PyObject *self, PyObject *args)
{
    PyObject *hlvirt_ptr;

    if (!PyArg_ParseTuple(args, "O!", &HlPtrType, &hlvirt_ptr))
    {
        return NULL;
    }

    vvirtual *virt = hlmod_require_pointer((HlPtr *)hlvirt_ptr, HVIRTUAL);
    if (!virt) return NULL;
    if (virt->t == NULL || virt->t->kind != HVIRTUAL || virt->t->virt == NULL)
    {
        PyErr_SetString(PyExc_TypeError, "HlPtr does not point to a valid Haxe virtual object.");
        return NULL;
    }

    return PyLong_FromLong(virt->t->virt->nfields);
}

PyObject *hlmod_py_get_virtual_field_name(PyObject *self, PyObject *args)
{
    PyObject *hlvirt_ptr;
    int field_index;

    if (!PyArg_ParseTuple(args, "O!i", &HlPtrType, &hlvirt_ptr, &field_index))
    {
        return NULL;
    }

    vvirtual *virt = hlmod_require_pointer((HlPtr *)hlvirt_ptr, HVIRTUAL);
    if (!virt) return NULL;
    if (virt->t == NULL || virt->t->kind != HVIRTUAL || virt->t->virt == NULL)
    {
        PyErr_SetString(PyExc_TypeError, "HlPtr does not point to a valid Haxe virtual object.");
        return NULL;
    }
    if (field_index < 0 || field_index >= virt->t->virt->nfields)
    {
        PyErr_Format(PyExc_IndexError, "Virtual field index %d is out of bounds (0 to %d).",
                     field_index, virt->t->virt->nfields - 1);
        return NULL;
    }

    return PyUnicode_FromString((const char *)hl_to_utf8(virt->t->virt->fields[field_index].name));
}
#pragma endregion
