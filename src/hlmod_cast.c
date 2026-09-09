#include <hlmod_python.h>
#include "hlmod_internal.h"

#include <limits.h>
#include <float.h>
#include <string.h>

#pragma region Casting

static PyObject *hlmod_wrap_pointer(const char *name, void *ptr, hl_type *type, PyObject *py_class)
{
    PyObject *owned_class = NULL;
    if (!py_class) {
        if (!g_hlobj_module) g_hlobj_module = PyImport_ImportModule("hlobj");
        if (!g_hlobj_module) return NULL;
        owned_class = PyObject_GetAttrString(g_hlobj_module, name);
        if (!owned_class) return NULL;
        py_class = owned_class;
    }
    PyObject *wrap = name ? Py_NewRef(py_class) : PyObject_GetAttrString(py_class, "_hlmod_wrap");
    PyObject *pointer = wrap ? hlmod_ptr_new(ptr, type) : NULL;
    PyObject *result = pointer ? PyObject_CallOneArg(wrap, pointer) : NULL;
    Py_XDECREF(pointer);
    Py_XDECREF(wrap);
    Py_XDECREF(owned_class);
    return result;
}

/* A missing attribute is ordinary, but a descriptor's other errors propagate. */
HlPtr *hlmod_extract_pointer(PyObject *obj)
{
    PyObject *pointer;
    if (Py_IS_TYPE(obj, &HlPtrType)) pointer = Py_NewRef(obj);
    else {
        pointer = PyObject_GetAttrString(obj, "_hlmod_ptr");
        if (!pointer) return NULL;
    }
    if (!Py_IS_TYPE(pointer, &HlPtrType) || !((HlPtr *)pointer)->type) {
        Py_DECREF(pointer);
        PyErr_SetString(PyExc_TypeError, "Expected a native-created, typed HlPtr; raw addresses are opaque.");
        return NULL;
    }
    return (HlPtr *)pointer;
}

void *hlmod_require_pointer(HlPtr *pointer, hl_type_kind kind)
{
    if (!pointer->type || pointer->type->kind != kind) {
        PyErr_Format(PyExc_TypeError, "Expected a trusted %s pointer.", kind2str(kind));
        return NULL;
    }
    if (!pointer->ptr) {
        PyErr_SetString(PyExc_ValueError, "Cannot dereference a null HlPtr.");
        return NULL;
    }
    return pointer->ptr;
}

static void *hlmod_pointer_slot(void *pointer)
{
    void **slot = hl_gc_alloc_raw(sizeof(void *));
    *slot = pointer;
    return slot;
}

PyObject *hlmod_cast_to_py(hl_type *type, void *ptr)
{
    if (!type || !ptr) {
        PyErr_SetString(PyExc_ValueError, "Missing HL type or value slot.");
        return NULL;
    }
    switch (type->kind) {
    case HVOID: Py_RETURN_NONE;
    case HI64: return PyLong_FromLongLong(*(int64 *)ptr);
    case HI32: return PyLong_FromLong(*(int *)ptr);
    case HUI16: return PyLong_FromUnsignedLong(*(unsigned short *)ptr);
    case HUI8: return PyLong_FromUnsignedLong(*(unsigned char *)ptr);
    case HBOOL: return PyBool_FromLong(*(bool *)ptr);
    case HF32: return PyFloat_FromDouble(*(float *)ptr);
    case HF64: return PyFloat_FromDouble(*(double *)ptr);
    case HPACKED:
    case HGUID:
    case HMETHOD:
        PyErr_Format(PyExc_TypeError, "HL %s has no supported Python value representation.", kind2str(type->kind));
        return NULL;
    default: break;
    }
    if (!hl_is_ptr(type)) {
        PyErr_Format(PyExc_TypeError, "Unsupported HL type %s.", kind2str(type->kind));
        return NULL;
    }
    void *value = *(void **)ptr;
    if (!value) Py_RETURN_NONE;
    if (type->kind == HDYN || type->kind == HNULL) {
        if (type->kind == HNULL && (!type->tparam || type->tparam->kind == HSTRUCT)) {
            PyErr_SetString(PyExc_TypeError, "Nullable struct representation is unsupported.");
            return NULL;
        }
        vdynamic *dyn = value;
        if (!dyn->t || dyn->t->kind == HDYN || dyn->t->kind == HNULL) {
            PyErr_SetString(PyExc_TypeError, "Invalid Dynamic runtime type.");
            return NULL;
        }
        return hlmod_cast_to_py(dyn->t, hl_is_dynamic(dyn->t) ? (void *)&value : (void *)&dyn->v);
    }
    if (hl_is_dynamic(type)) type = ((vdynamic *)value)->t;
    if (type->kind == HOBJ) {
        PyObject *owned = hlmod_python_proxy(value);
        if (owned || PyErr_Occurred()) return owned;
        if (type->obj && type->obj->name && uchar_eq(type->obj->name, u"String")) {
            vstring *string = value;
            int byteorder = -1;
            return PyUnicode_DecodeUTF16((const char *)string->bytes, (Py_ssize_t)string->length * sizeof(uchar), "strict", &byteorder);
        }
        PyObject *py_class = hlmod_python_type(type);
        int index = hlmod_type_index(type);
        if (!py_class && index >= 0 && index < g_hlobjs_l) py_class = g_hlobjs[index];
        if (py_class) return hlmod_wrap_pointer(NULL, value, type, py_class);
    } else if (type->kind == HVIRTUAL) {
        return hlmod_py_make_hlvirtual(value, type);
    } else if (type->kind == HFUN) {
        return hlmod_py_make_hlcallable(value);
    } else if (type->kind == HARRAY) {
        return hlmod_wrap_pointer("HlArray", value, type, NULL);
    } else if (type->kind == HBYTES) {
        return hlmod_wrap_pointer("HlBytes", value, type, NULL);
    } else if (type->kind == HENUM) {
        return hlmod_wrap_pointer("HlEnum", value, type, NULL);
    } else if (type->kind == HDYNOBJ) {
        return hlmod_wrap_pointer("HlDynObject", value, type, NULL);
    } else if (type->kind == HREF) {
        if (!hl_is_gc_ptr(value)) {
            PyErr_SetString(PyExc_TypeError, "Stack-backed HL references cannot escape into Python; use HlRef.create with an explicit reference type.");
            return NULL;
        }
        return hlmod_wrap_pointer("HlRef", value, type, NULL);
    }
    return hlmod_ptr_new(value, type);
}

void *hlmod_cast_to_hl(PyObject *obj, hl_type *type)
{
    hlmod_python_retain();
    if (!type) {
        PyErr_SetString(PyExc_ValueError, "Missing HL type.");
        return NULL;
    }
    if (type->kind == HVOID || type->kind == HPACKED || type->kind == HGUID || type->kind == HMETHOD) {
        PyErr_Format(PyExc_TypeError, "HL %s has no supported Python value representation.", kind2str(type->kind));
        return NULL;
    }
    if (obj == Py_None) {
        if (hl_is_ptr(type)) return hlmod_pointer_slot(NULL);
        PyErr_SetString(PyExc_TypeError, "None cannot represent an HL scalar.");
        return NULL;
    }
    if (type->kind == HNULL) {
        if (!type->tparam || type->tparam->kind == HSTRUCT) {
            PyErr_SetString(PyExc_TypeError, "Nullable struct representation is unsupported.");
            return NULL;
        }
        void *inner = hlmod_cast_to_hl(obj, type->tparam);
        if (!inner) return NULL;
        return hlmod_pointer_slot(hl_make_dyn(inner, type->tparam));
    }
    if (type->kind == HDYN) {
        hl_type *inner = NULL;
        if (PyBool_Check(obj)) inner = &hlt_bool;
        else if (PyLong_Check(obj)) {
            long long value = PyLong_AsLongLong(obj);
            if (PyErr_Occurred()) return NULL;
            inner = value >= INT32_MIN && value <= INT32_MAX ? &hlt_i32 : &hlt_i64;
        } else if (PyFloat_Check(obj)) inner = &hlt_f64;
        else if (PyUnicode_Check(obj)) {
            if (g_code) for (int i = 0; i < g_code->ntypes; i++) {
                hl_type *candidate = &g_code->types[i];
                if (candidate->kind == HOBJ && candidate->obj && candidate->obj->name && uchar_eq(candidate->obj->name, u"String")) {
                    inner = candidate;
                    break;
                }
            }
            if (!inner) {
                PyErr_SetString(PyExc_TypeError, "The bytecode has no String type.");
                return NULL;
            }
        } else {
            HlPtr *pointer = hlmod_extract_pointer(obj);
            if (!pointer) {
                if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
                    PyErr_Clear();
                    PyErr_SetString(PyExc_TypeError, "Dynamic needs a scalar or typed native value; callbacks require an explicit signature.");
                }
                return NULL;
            }
            inner = pointer->type;
            Py_DECREF(pointer);
        }
        if (inner->kind == HSTRUCT || inner->kind == HPACKED || inner->kind == HGUID || inner->kind == HMETHOD) {
            PyErr_SetString(PyExc_TypeError, "This native type cannot safely be boxed as Dynamic.");
            return NULL;
        }
        void *slot = hlmod_cast_to_hl(obj, inner);
        if (!slot) return NULL;
        return hlmod_pointer_slot(hl_make_dyn(slot, inner));
    }
    if (type->kind == HOBJ && type->obj && type->obj->name && uchar_eq(type->obj->name, u"String") && PyUnicode_Check(obj)) {
        PyObject *encoded = PyUnicode_AsEncodedString(obj, "utf-16-le", "strict");
        if (!encoded) return NULL;
        Py_ssize_t size = PyBytes_GET_SIZE(encoded);
        if (size > INT_MAX - (int)sizeof(uchar)) {
            Py_DECREF(encoded);
            PyErr_SetString(PyExc_OverflowError, "String exceeds the HL allocation limit.");
            return NULL;
        }
        vstring *string = (vstring *)hl_alloc_obj(type);
        uchar *bytes = hl_gc_alloc_noptr((int)size + sizeof(uchar));
        memcpy(bytes, PyBytes_AS_STRING(encoded), size);
        bytes[size / sizeof(uchar)] = 0;
        string->bytes = bytes;
        string->length = (int)(size / sizeof(uchar));
        Py_DECREF(encoded);
        return hlmod_pointer_slot(string);
    }
    if (!hl_is_ptr(type)) {
        union { double d; float f; int64 i64; int i; unsigned short u16; unsigned char u8; bool b; } value;
        switch (type->kind) {
        case HF64:
        case HF32: {
            double number = PyFloat_AsDouble(obj);
            if (PyErr_Occurred()) return NULL;
            if (type->kind == HF32) {
                if (isfinite(number) && fabs(number) > FLT_MAX) {
                    PyErr_SetString(PyExc_OverflowError, "Value is outside the Float32 range.");
                    return NULL;
                }
                value.f = (float)number;
            } else value.d = number;
            break;
        }
        case HBOOL:
            if (!PyBool_Check(obj)) {
                PyErr_SetString(PyExc_TypeError, "HL Bool requires a Python bool.");
                return NULL;
            }
            value.b = obj == Py_True;
            break;
        case HI64:
        case HI32:
        case HUI16:
        case HUI8: {
            long long number = PyLong_AsLongLong(obj);
            if (PyErr_Occurred()) return NULL;
            long long low = type->kind == HI64 ? LLONG_MIN : type->kind == HI32 ? INT32_MIN : 0;
            long long high = type->kind == HI64 ? LLONG_MAX : type->kind == HI32 ? INT32_MAX : type->kind == HUI16 ? UINT16_MAX : UINT8_MAX;
            if (number < low || number > high) {
                PyErr_Format(PyExc_OverflowError, "Value is outside the HL %s range.", kind2str(type->kind));
                return NULL;
            }
            if (type->kind == HI64) value.i64 = number;
            else if (type->kind == HI32) value.i = (int)number;
            else if (type->kind == HUI16) value.u16 = (unsigned short)number;
            else value.u8 = (unsigned char)number;
            break;
        }
        default:
            PyErr_Format(PyExc_TypeError, "Unsupported HL scalar %s.", kind2str(type->kind));
            return NULL;
        }
        int size = hl_type_size(type);
        void *slot = hl_gc_alloc_noptr(size);
        memcpy(slot, &value, size);
        return slot;
    }
    HlPtr *pointer = hlmod_extract_pointer(obj);
    if (!pointer) {
        if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
            PyErr_Clear();
            if (type->kind == HBYTES && PyObject_CheckBuffer(obj)) {
                /* Bytes parameters receive a GC-owned copy: later Python edits
                   are not visible to HL. Pass HlBytes to share storage. */
                PyObject *arguments = Py_BuildValue("(O)", obj);
                PyObject *copy = arguments ? hlmod_py_bytes_from(NULL, arguments) : NULL;
                Py_XDECREF(arguments);
                if (!copy) return NULL;
                void *slot = hlmod_pointer_slot(((HlPtr *)copy)->ptr);
                Py_DECREF(copy);
                return slot;
            }
            if (type->kind == HFUN && PyCallable_Check(obj)) {
                void *callback = hlmod_python_callback(obj, type);
                return callback ? hlmod_pointer_slot(callback) : NULL;
            }
            PyErr_Format(PyExc_TypeError, "Expected a typed native %s value, not %s.", kind2str(type->kind), Py_TYPE(obj)->tp_name);
        }
        return NULL;
    }
    hl_type *actual = pointer->type;
    void *value = pointer->ptr;
    if (value && hl_is_dynamic(actual)) actual = ((vdynamic *)value)->t;
    if (!hl_safe_cast(actual, type)) {
        Py_DECREF(pointer);
        PyErr_Format(PyExc_TypeError, "Native %s value is not assignable to requested %s type.", kind2str(actual->kind), kind2str(type->kind));
        return NULL;
    }
    void *slot = hlmod_pointer_slot(value);
    Py_DECREF(pointer);
    return slot;
}

static varray *hlmod_array(HlPtr *pointer)
{
    varray *array = hlmod_require_pointer(pointer, HARRAY);
    if (!array) return NULL;
    if (!array->at || array->size < 0 || array->at->kind == HVOID ||
        array->at->kind == HPACKED || array->at->kind == HGUID || array->at->kind == HMETHOD) {
        PyErr_SetString(PyExc_TypeError, "Array element layout is unsupported.");
        return NULL;
    }
    return array;
}

PyObject *hlmod_py_array_new(PyObject *self, PyObject *args)
{
    int type_index;
    PyObject *values;
    if (!PyArg_ParseTuple(args, "iO", &type_index, &values)) return NULL;
    if (!g_code || type_index < 0 || type_index >= g_code->ntypes) {
        PyErr_SetString(PyExc_IndexError, "Array element type index is out of bounds.");
        return NULL;
    }
    hl_type *element = &g_code->types[type_index];
    if (element->kind == HVOID || element->kind == HPACKED || element->kind == HGUID || element->kind == HMETHOD) {
        PyErr_SetString(PyExc_TypeError, "Array element layout is unsupported.");
        return NULL;
    }
    PyObject *sequence = PySequence_Fast(values, "Array values must be iterable.");
    if (!sequence) return NULL;
    Py_ssize_t size = PySequence_Fast_GET_SIZE(sequence);
    int width = hl_type_size(element);
    if (width <= 0 || size > (INT_MAX - (int)sizeof(varray)) / width) {
        Py_DECREF(sequence);
        PyErr_SetString(PyExc_OverflowError, "Array exceeds the HL allocation limit.");
        return NULL;
    }
    varray *array = hl_alloc_array(element, (int)size);
    PyObject *pointer = hlmod_ptr_new(array, &hlt_array);
    if (!pointer) { Py_DECREF(sequence); return NULL; }
    for (Py_ssize_t i = 0; i < size; i++) {
        PyObject *item = PySequence_GetItem(sequence, i);
        void *slot = item ? hlmod_cast_to_hl(item, element) : NULL;
        Py_XDECREF(item);
        if (!slot) { Py_DECREF(pointer); Py_DECREF(sequence); return NULL; }
        memcpy(hl_aptr(array, char) + i * width, slot, width);
    }
    Py_DECREF(sequence);
    return pointer;
}

PyObject *hlmod_py_array_length(PyObject *self, PyObject *args)
{
    HlPtr *pointer;
    if (!PyArg_ParseTuple(args, "O!", &HlPtrType, &pointer)) return NULL;
    varray *array = hlmod_array(pointer);
    return array ? PyLong_FromLong(array->size) : NULL;
}

static void *hlmod_array_slot(varray *array, Py_ssize_t index)
{
    if (index < 0) index += array->size;
    if (index < 0 || index >= array->size) {
        PyErr_SetString(PyExc_IndexError, "Array index is out of bounds.");
        return NULL;
    }
    return hl_aptr(array, char) + index * hl_type_size(array->at);
}

PyObject *hlmod_py_array_get(PyObject *self, PyObject *args)
{
    HlPtr *pointer;
    Py_ssize_t index;
    if (!PyArg_ParseTuple(args, "O!n", &HlPtrType, &pointer, &index)) return NULL;
    varray *array = hlmod_array(pointer);
    if (!array) return NULL;
    void *slot = hlmod_array_slot(array, index);
    return slot ? hlmod_cast_to_py(array->at, slot) : NULL;
}

PyObject *hlmod_py_array_set(PyObject *self, PyObject *args)
{
    HlPtr *pointer;
    Py_ssize_t index;
    PyObject *value;
    if (!PyArg_ParseTuple(args, "O!nO", &HlPtrType, &pointer, &index, &value)) return NULL;
    varray *array = hlmod_array(pointer);
    if (!array) return NULL;
    void *slot = hlmod_array_slot(array, index);
    if (!slot) return NULL;
    void *converted = hlmod_cast_to_hl(value, array->at);
    if (!converted) return NULL;
    memcpy(slot, converted, hl_type_size(array->at));
    Py_RETURN_NONE;
}

PyObject *hlmod_py_array_element_type(PyObject *self, PyObject *args)
{
    HlPtr *pointer;
    if (!PyArg_ParseTuple(args, "O!", &HlPtrType, &pointer)) return NULL;
    varray *array = hlmod_array(pointer);
    if (!array) return NULL;
    int index = hlmod_type_index(array->at);
    if (index < 0) Py_RETURN_NONE;
    return PyLong_FromLong(index);
}

