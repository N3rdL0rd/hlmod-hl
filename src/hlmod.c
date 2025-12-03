#include <stdio.h>
#include <hl.h>
#include <hlmod.h>
#include <Python.h>
#include <structmember.h>
#include <std_globals.h>

bool uchar_eq(const uchar *s1, const uchar *s2)
{
    while (*s1 != u'\0' && *s2 != u'\0')
    {
        if (*s1 != *s2)
        {
            return false;
        }
        s1++;
        s2++;
    }
    return *s1 == *s2;
}

THREAD_LOCAL int64_t g_return_value_int = 0;
THREAD_LOCAL double g_return_value_double = 0.0;
static THREAD_LOCAL int* g_passthrough_stack = NULL;
static THREAD_LOCAL int g_passthrough_stack_size = 0;
static THREAD_LOCAL int g_passthrough_stack_capacity = 0;

static void push_passthrough(int findex) {
    if (g_passthrough_stack_size >= g_passthrough_stack_capacity) {
        int new_capacity = g_passthrough_stack_capacity == 0 ? 8 : g_passthrough_stack_capacity * 2;
        g_passthrough_stack = (int*)realloc(g_passthrough_stack, new_capacity * sizeof(int));
        g_passthrough_stack_capacity = new_capacity;
    }
    g_passthrough_stack[g_passthrough_stack_size++] = findex;
}

static void pop_passthrough() {
    if (g_passthrough_stack_size > 0) {
        g_passthrough_stack_size--;
    }
}

static bool is_passthrough(int findex) {
    for (int i = 0; i < g_passthrough_stack_size; i++) {
        if (g_passthrough_stack[i] == findex) {
            return true;
        }
    }
    return false;
}

EXPORT int64_t hlmod_get_return_int() {
    return g_return_value_int;
}

EXPORT double hlmod_get_return_double() {
    return g_return_value_double;
}

static PyObject **g_hlobjs = NULL;
static int g_hlobjs_l = 0;

#pragma region HlPtr
static PyObject *HlPtr_New(void *ptr, int kind);
static PyObject *HlPtr_get_ptr(HlPtr *self, void *closure);
static PyObject *HlPtr_get_kind(HlPtr *self, void *closure);
static int HlPtr_init(HlPtr *self, PyObject *args, PyObject *kwds);
static void HlPtr_dealloc(HlPtr *self);


static PyGetSetDef HlPtr_getsetters[] = {
    {"ptr", (getter)HlPtr_get_ptr, NULL, "The raw pointer value", NULL},
    {"kind", (getter)HlPtr_get_kind, NULL, "The HL type kind enum", NULL},
    {NULL}};

PyTypeObject HlPtrType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "hlmod.HlPtr",
    .tp_doc = "A wrapper for a Haxe pointer and its type kind.",
    .tp_basicsize = sizeof(HlPtr),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = PyType_GenericNew,
    .tp_getset = HlPtr_getsetters,
    .tp_init = (initproc)HlPtr_init,
    .tp_dealloc = (destructor)HlPtr_dealloc,
};

static PyObject *HlPtr_New(void *ptr, int kind)
{
    PyObject *args = Py_BuildValue("(Ki)", (unsigned long long)ptr, kind);
    if (args == NULL) {
        return NULL;
    }

    PyObject *self = PyObject_CallObject((PyObject *)&HlPtrType, args);
    
    Py_DECREF(args);

    return self;
}

static void HlPtr_dealloc(HlPtr *self)
{
    if (self->root != NULL)
    {
        hl_remove_root(self->root);
        self->root = NULL; // Prevent double-free
    }
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int HlPtr_init(HlPtr *self, PyObject *args, PyObject *kwds)
{
    unsigned long long ptr_val;
    int kind = 0;
    static char *kwlist[] = {"ptr", "kind", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "K|i", kwlist, &ptr_val, &kind))
    {
        return -1;
    }

    self->ptr = (void *)ptr_val;
    self->kind = kind;
    self->root = NULL;

    if (self->ptr != NULL)
    {
        self->root = (void **)hl_gc_alloc_raw(sizeof(void *));
        if (self->root == NULL) {
            PyErr_SetString(PyExc_MemoryError, "Failed to allocate memory for HL GC root.");
            return -1;
        }

        *(self->root) = self->ptr;

        hl_add_root(self->root);
    }

    return 0;
}

static PyObject *HlPtr_get_ptr(HlPtr *self, void *closure)
{
    return PyLong_FromVoidPtr(self->ptr);
}
static PyObject *HlPtr_get_kind(HlPtr *self, void *closure)
{
    return PyLong_FromLong(self->kind);
}

#pragma region HlHook
static PyObject *HlHook_call_original(HlHook *self, PyObject *py_args)
{
    hl_function *f = g_module->code->functions + g_module->functions_indexes[self->findex];
    hl_type_fun *fun_type = f->type->fun;
    int nargs = fun_type->nargs;

    if (PyTuple_Size(py_args) != nargs)
    {
        PyErr_Format(PyExc_TypeError, "call_original() expected %d arguments, but got %zd", nargs, PyTuple_Size(py_args));
        return NULL;
    }

    vdynamic *vargs[HL_MAX_ARGS];
    if (nargs > HL_MAX_ARGS)
    {
        PyErr_SetString(PyExc_ValueError, "Too many arguments for call_original");
        return NULL;
    }

    for (int i = 0; i < nargs; i++)
    {
        PyObject *py_arg = PyTuple_GetItem(py_args, i);
        hl_type *hl_arg_type = fun_type->args[i];

        void *hl_val_ptr = hlmod_cast_to_hl(py_arg, hl_arg_type);
        if (hl_val_ptr == NULL)
        {
            return NULL;
        }

        vargs[i] = hl_make_dyn(hl_val_ptr, hl_arg_type);
    }

    vclosure cl;
    cl.t = f->type;
    cl.fun = g_module->functions_ptrs[self->findex];
    cl.hasValue = 0;

    push_passthrough(self->findex);
    bool is_exc;
    vdynamic *hl_result = hl_dyn_call_safe(&cl, nargs > 0 ? vargs : NULL, nargs, &is_exc);
    pop_passthrough();

    if (is_exc)
    {
        uchar *u_exc_str = hl_to_string(hl_result);
        char *exc_str_utf8 = hl_to_utf8(u_exc_str);
        PyErr_Format(PyExc_RuntimeError, "An exception occurred in the original Haxe function: %s", exc_str_utf8);
        return NULL;
    }

    if (fun_type->ret->kind == HVOID)
    {
        Py_RETURN_NONE;
    }

    PyObject *py_result = NULL;

    if (!hl_is_dynamic(fun_type->ret)) {
        py_result = hlmod_cast_to_py(fun_type->ret, &hl_result->v);
    } else {
        py_result = hlmod_cast_to_py(fun_type->ret, &hl_result);
    }

    if (py_result == NULL)
    {
        return NULL;
    }

    return py_result;
}

static PyMethodDef HlHook_methods[] = {
    {"call_original", (PyCFunction)HlHook_call_original, METH_VARARGS, "Calls the original Haxe function."},
    {NULL}};

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

PyObject *hlmod_py_register_hlobj(PyObject *self, PyObject *args)
{
    int type_idx;
    PyObject *py_class;

    if (!PyArg_ParseTuple(args, "iO", &type_idx, &py_class))
    {
        return NULL;
    }

    // printf("[hlmod] Registering HlObject for t@%i\n", type_idx);

    if (!PyType_Check(py_class))
    {
        PyErr_SetString(PyExc_TypeError, "Second argument must be a class.");
        return NULL;
    }

    if (type_idx >= g_hlobjs_l)
    {
        int new_len = type_idx + 1;
        g_hlobjs = realloc(g_hlobjs, sizeof(PyObject *) * new_len);
        memset(g_hlobjs + g_hlobjs_l, 0, sizeof(PyObject *) * (new_len - g_hlobjs_l));
        g_hlobjs_l = new_len;
    }

    if (g_hlobjs[type_idx] != NULL)
    {
        Py_DECREF(g_hlobjs[type_idx]);
    }

    Py_INCREF(py_class);
    g_hlobjs[type_idx] = py_class;

    Py_RETURN_NONE;
}

/**
 * @brief Performs a reverse lookup to find the Haxe type for a given Python object.
 * 
 * This function iterates through all registered Python classes (subclasses of HlObject)
 * and checks if the given 'obj' is an instance of any of them.
 * 
 * @param obj The Python object instance to look up.
 * @return The corresponding hl_type* if a match is found, otherwise NULL.
 */
static hl_type* hlmod_py_find_hlobject(PyObject *obj)
{
    for (int i = 0; i < g_hlobjs_l; i++)
    {
        PyObject *registered_class = g_hlobjs[i];

        if (registered_class == NULL) {
            continue;
        }

        int is_instance = PyObject_IsInstance(obj, registered_class);

        if (is_instance == -1) {
            fprintf(stderr, "[hlmod] [WARN] PyObject_IsInstance failed during reverse lookup.\n");
            PyErr_Clear();
            return NULL;
        }

        if (is_instance) {
            return &g_code->types[i];
        }
    }

    return NULL;
}

static PyObject *g_hlobj_module = NULL;
static PyObject *g_hlobj_base_class = NULL;
static PyObject *g_hlcallable_class = NULL;

/**
 * @brief Checks if a Python object is an instance of the HlObject base class
 *        from the 'hlobj' Python module. Caches the module and class for efficiency.
 *
 * @param obj The Python object to check.
 * @return true if it's an instance of hlobj.HlObject, false otherwise.
 */
bool hlmod_py_is_hlobject(PyObject *obj)
{
    if (g_hlobj_base_class == NULL)
    {
        g_hlobj_module = PyImport_ImportModule("hlobj");
        if (g_hlobj_module == NULL)
        {
            PyErr_Print();
            return false;
        }
        g_hlobj_base_class = PyObject_GetAttrString(g_hlobj_module, "HlObject");
        if (g_hlobj_base_class == NULL)
        {
            Py_DECREF(g_hlobj_module);
            g_hlobj_module = NULL;
            PyErr_Clear();
            return false;
        }
    }

    int is_instance = PyObject_IsInstance(obj, g_hlobj_base_class);

    if (is_instance == -1) {
        PyErr_Clear();
        return false;
    }

    return is_instance == 1;
}

#pragma region Casting

/**
 * @brief Cast an HL type to Python.
 * 
 * @param type The type of the HL value
 * @param ptr A pointer to the HL value
 * @returns a PyObject* that is a casted version of the HL value
 */
PyObject *hlmod_cast_to_py(hl_type *type, void *ptr)
{
    if (type == NULL)
    {
        fprintf(stderr, "[hlmod] [ERROR] [hl->py] Received NULL type with pointer %p.\n", ptr);
        Py_RETURN_NONE;
    }

    switch (type->kind)
    {
    case HF64:
        return PyFloat_FromDouble(*(double *)ptr);
    case HF32:
        return PyFloat_FromDouble((double)(*(float *)ptr));
    case HI32:
        return PyLong_FromLong(*(int *)ptr);
    case HBOOL:
        return PyBool_FromLong(*(bool *)ptr);
    case HUI16:
        return PyLong_FromLong(*(unsigned short *)ptr);
    case HUI8:
        return PyLong_FromLong(*(unsigned char *)ptr);
    case HOBJ:
    {
        void *obj_ptr = *(void **)ptr;
        if (obj_ptr == NULL)
        {
            Py_RETURN_NONE;
        }
        if (type->obj != NULL && type->obj->name != NULL && uchar_eq(type->obj->name, u"String"))
        {
            // printf("%s: ", type->obj->name);
            vstring *s = (vstring *)obj_ptr;
            // printf("s: %p s->bytes: %p s->length: %p\n", s, s->bytes, s->length);
            return PyUnicode_DecodeUTF16((const char *)s->bytes, s->length * sizeof(uchar), "strict", NULL);
        }
        if (g_code == NULL)
            break;

        int type_idx = type - g_code->types;

        if (type_idx >= 0 && type_idx < g_hlobjs_l)
        {
            PyObject *py_class = g_hlobjs[type_idx];
            if (py_class != NULL)
            {
                PyObject *py_arg_ptr = HlPtr_New(obj_ptr, HOBJ);
                if (py_arg_ptr == NULL)
                    return NULL;

                PyObject *py_args = PyTuple_Pack(1, py_arg_ptr);
                Py_DECREF(py_arg_ptr);
                if (py_args == NULL)
                    return NULL;

                PyObject *py_instance = PyObject_CallObject(py_class, py_args);
                Py_DECREF(py_args);

                return py_instance;
            }
        }
        break;
    }
    case HNULL:
    {
        void *nullable_ptr = *(void **)ptr;
        if (nullable_ptr == NULL)
        {
            Py_RETURN_NONE;
        }
        if (hl_is_ptr(type->tparam))
        {
            return hlmod_cast_to_py(type->tparam, &nullable_ptr);
        }
        else
        {
            vdynamic *dyn = (vdynamic *)nullable_ptr;
            return hlmod_cast_to_py(type->tparam, &dyn->v);
        }
    }
    case HARRAY:
    {
        varray *arr = *(varray **)ptr;
        if (arr == NULL)
        {
            Py_RETURN_NONE;
        }

        PyObject *py_list = PyList_New(arr->size);
        if (!py_list)
            return NULL;

        hl_type *element_type = arr->at;
        void *data_ptr = hl_aptr(arr, void);
        int element_size = hl_type_size(element_type);

        for (int i = 0; i < arr->size; i++)
        {
            void *element_ptr = (char *)data_ptr + i * element_size;
            PyObject *py_item = hlmod_cast_to_py(element_type, element_ptr);
            if (!py_item)
            {
                Py_DECREF(py_list);
                return NULL;
            }
            if (PyList_SetItem(py_list, i, py_item) != 0)
            {
                Py_DECREF(py_item);
                Py_DECREF(py_list);
                return NULL;
            }
        }
        return py_list;
    }
    case HDYN:
    {
        vdynamic *dyn = *(vdynamic **)ptr;
        // printf("unwrapping dyn with type ");
        // printf("%s...\n", kind2str(dyn->t->kind));
        return hlmod_cast_to_py(dyn->t, &dyn->v);
    }
    case HFUN:
    {
        vclosure *cl = (vclosure *)ptr;
        if (cl == NULL)
        {
            Py_RETURN_NONE;
        }

        if (g_hlcallable_class == NULL)
        {
            if (g_hlobj_module == NULL) {
                g_hlobj_module = PyImport_ImportModule("hlobj");
                if (g_hlobj_module == NULL) {
                    PyErr_SetString(PyExc_ImportError, "Failed to import the 'hlobj' module. Is it in `mods/`?");
                    return NULL;
                }
            }
            g_hlcallable_class = PyObject_GetAttrString(g_hlobj_module, "HlCallable");
            if (g_hlcallable_class == NULL) {
                PyErr_SetString(PyExc_AttributeError, "Could not find 'HlCallable' class in 'hlobj' module.");
                return NULL;
            }
        }
        
        PyObject *py_ptr = HlPtr_New(cl, HFUN);
        if (py_ptr == NULL) {
            return NULL;
        }

        PyObject *py_args = PyTuple_Pack(1, py_ptr);
        Py_DECREF(py_ptr);
        if (py_args == NULL) {
            return NULL;
        }

        PyObject *py_instance = PyObject_CallObject(g_hlcallable_class, py_args);
        Py_DECREF(py_args);

        return py_instance;
    }
    default:
#       ifdef HLMOD_DEBUG
        printf("[hlmod] [DEBUG] Falling through with %s\n", kind2str(type->kind));
#       endif
        break;
    }

    if (hl_is_ptr(type))
    {
        void *obj_ptr = *(void **)ptr;
        if (obj_ptr == NULL)
        {
            Py_RETURN_NONE;
        }
        return HlPtr_New(obj_ptr, type->kind);
    }

    fprintf(stderr, "[hlmod] [ERROR] [hl->py] Something goofed!\n");
    Py_RETURN_NONE;
}

void *hlmod_cast_to_hl(PyObject *obj, hl_type *type)
{
    if (type == NULL)
    {
        fprintf(stderr, "[hlmod] [ERROR] [py->hl] Received NULL type.\n");
        return NULL;
    }

    if (type->kind == HDYN)
    {
        if (obj == Py_None) {
            void **ret_ptr = (void **)hl_gc_alloc_raw(sizeof(void *));
            *ret_ptr = NULL;
            return ret_ptr;
        }

        hl_type *inner_type = NULL;
        if (PyBool_Check(obj)) {
            inner_type = &hlt_bool;
        } else if (PyLong_Check(obj)) {
            inner_type = &hlt_i32; // HACK: safe guess for types with overlap is to just take the most common. if someone decides to use a `hl.I64`, then this will die!
        } else if (PyFloat_Check(obj)) {
            inner_type = &hlt_f64;
        } else if (PyUnicode_Check(obj)) {
            // HACK: is this fucked? should this be different? or even cached? yes! does it work? also yes!
            for (int i = 0; i < g_code->ntypes; i++) {
                hl_type *t = &g_code->types[i];
                if (t->kind == HOBJ && t->obj->name && uchar_eq(t->obj->name, u"String")) {
                    inner_type = t;
                    break;
                }
            }
        } else if (hlmod_py_is_hlobject(obj)) {
            inner_type = hlmod_py_find_hlobject(obj);
        } else if (g_hlcallable_class != NULL && PyObject_IsInstance(obj, g_hlcallable_class))
        {
            PyObject *py_hlptr = PyObject_GetAttrString(obj, "_hlmod_ptr");
            if (py_hlptr == NULL || !Py_IS_TYPE(py_hlptr, &HlPtrType)) {
                PyErr_SetString(PyExc_TypeError, "HlCallable must have a valid _hlmod_ptr attribute.");
                if (py_hlptr) Py_DECREF(py_hlptr);
                return NULL;
            }

            vclosure* cl = (vclosure*)((HlPtr*)py_hlptr)->ptr;
            Py_DECREF(py_hlptr);

            if (cl == NULL) {
                PyErr_SetString(PyExc_ValueError, "HlCallable's _hlmod_ptr contains a null Haxe closure.");
                return NULL;
            }

            inner_type = cl->t;
        } else if (PyObject_IsInstance(obj, (PyObject *)&HlPtrType)) {
            PyErr_Format(PyExc_TypeError, "HlPtr is an ambiguous type and cannot be directly cast back to a Dynamic, which is what you're trying to do. Try wrapping this HlPtr in another type from `modcore.hlobj` to get it to cast cleanly. If you're confused as to why you're getting a HlPtr where you definitely shouldn't, then you should open an issue on Github.");
            return NULL;
        }
        // TODO: more types back and forth in a HDYN
            
        if (inner_type == NULL) {
            PyErr_Format(PyExc_TypeError, "Cannot wrap ambiguous type '%s' back into a Dynamic. Whoops!", Py_TYPE(obj)->tp_name);
            return NULL;
        }

        vdynamic *dyn_box = hl_alloc_dynamic(inner_type);
        if (dyn_box == NULL) {
            PyErr_SetString(PyExc_MemoryError, "Failed to allocate vdynamic for re-wrapping.");
            return NULL;
        }

        void *inner_hl_val_ptr = hlmod_cast_to_hl(obj, inner_type);
        if (inner_hl_val_ptr == NULL) {
            return NULL;
        }

        memcpy(&dyn_box->v, inner_hl_val_ptr, hl_type_size(inner_type));

        void **ret_ptr = (void **)hl_gc_alloc_raw(sizeof(void *));
        *ret_ptr = dyn_box;
        return ret_ptr;
    }

    if (type->kind == HNULL)
    {
        if (obj == Py_None)
        {
            void **ret_ptr = (void **)hl_gc_alloc_raw(sizeof(void *));
            *ret_ptr = NULL;
            return ret_ptr;
        }

        hl_type* inner_type = type->tparam;
        if (hl_is_ptr(inner_type)) {
            return hlmod_cast_to_hl(obj, inner_type);
        } else {
            vdynamic* box = hl_alloc_dynamic(inner_type);
            void* inner_val_ptr = hlmod_cast_to_hl(obj, inner_type);
            if (!inner_val_ptr) return NULL;

            memcpy(&box->v, inner_val_ptr, hl_type_size(inner_type));

            void** ret_ptr = (void**)hl_gc_alloc_raw(sizeof(void*));
            *ret_ptr = box;
            return ret_ptr;
        }
    }

    if (type->kind == HARRAY)
    {
        if (!PyList_Check(obj))
        {
            PyErr_Format(PyExc_TypeError, "Expected a list for HARRAY, but got %s", Py_TYPE(obj)->tp_name);
            return NULL;
        }

        Py_ssize_t size = PyList_Size(obj);
        hl_type *element_type = type->tparam;
        varray *arr = hl_alloc_array(element_type, (int)size);

        void *data_ptr = hl_aptr(arr, void);
        int element_size = hl_type_size(element_type);

        for (Py_ssize_t i = 0; i < size; i++)
        {
            PyObject *py_item = PyList_GetItem(obj, i);
            void *hl_item_ptr = hlmod_cast_to_hl(py_item, element_type);
            if (!hl_item_ptr)
            {
                return NULL;
            }

            void *dest_ptr = (char *)data_ptr + i * element_size;
            if (hl_is_ptr(element_type))
            {
                *(void **)dest_ptr = *(void **)hl_item_ptr;
            }
            else
            {
                memcpy(dest_ptr, hl_item_ptr, element_size);
            }
        }

        void **ret_ptr = (void **)hl_gc_alloc_raw(sizeof(void *));
        *ret_ptr = arr;
        return ret_ptr;
    }

    if (obj == Py_None)
    {
        hl_null_access();
        return NULL;
    }

    if (type->kind == HOBJ && type->obj != NULL && type->obj->name != NULL && uchar_eq(type->obj->name, u"String"))
    {
        if (!PyUnicode_Check(obj))
        {
            PyErr_Format(PyExc_TypeError, "Expected a string for Haxe type String, but got %s", Py_TYPE(obj)->tp_name);
            return NULL;
        }

        PyObject *utf16_bytes = PyUnicode_AsUTF16String(obj);
        if (utf16_bytes == NULL)
            return NULL;

        const char *buffer = PyBytes_AsString(utf16_bytes);
        Py_ssize_t size_in_bytes = PyBytes_Size(utf16_bytes);

        const char *string_start = buffer;
        Py_ssize_t string_size = size_in_bytes;

        if (size_in_bytes >= 2 && (unsigned char)buffer[0] == 0xFF && (unsigned char)buffer[1] == 0xFE)
        {
            string_start += 2;
            string_size -= 2;
        }

        if (string_size % 2 != 0)
        {
            fprintf(stderr, "[hlmod] [WARN] UTF-16 string conversion resulted in an odd number of bytes.\n");
            string_size--;
        }

        vstring *s_data = (vstring *)hl_gc_alloc_raw(sizeof(vstring));
        uchar *s_val = (uchar *)hl_gc_alloc_raw(string_size);
        memcpy(s_val, string_start, string_size);

        s_data->t = type;
        s_data->bytes = s_val;
        s_data->length = string_size / 2;

        Py_DECREF(utf16_bytes);

        void **ret_ptr = (void **)hl_gc_alloc_raw(sizeof(void *));
        *ret_ptr = s_data;
        return ret_ptr;
    }

    void *ptr = NULL;

    switch (type->kind)
    {
    case HF64:
    {
        double val = PyFloat_AsDouble(obj);
        if (PyErr_Occurred())
            return NULL;
        double *mem = (double *)hl_gc_alloc_noptr(sizeof(double));
        *mem = val;
        ptr = mem;
        break;
    }
    case HF32:
    {
        double val = PyFloat_AsDouble(obj);
        if (PyErr_Occurred())
            return NULL;
        float *mem = (float *)hl_gc_alloc_noptr(sizeof(float));
        *mem = (float)val;
        ptr = mem;
        break;
    }
    case HI32:
    {
        long val = PyLong_AsLong(obj);
        if (PyErr_Occurred())
            return NULL;
        int *mem = (int *)hl_gc_alloc_noptr(sizeof(int));
        *mem = (int)val;
        ptr = mem;
        break;
    }
    case HBOOL:
    {
        int val = PyObject_IsTrue(obj);
        if (val == -1)
            return NULL;
        bool *mem = (bool *)hl_gc_alloc_noptr(sizeof(bool));
        *mem = (bool)val;
        ptr = mem;
        break;
    }
    case HUI16:
    {
        unsigned long val = PyLong_AsUnsignedLong(obj);
        if (PyErr_Occurred())
            return NULL;
        unsigned short *mem = (unsigned short *)hl_gc_alloc_noptr(sizeof(unsigned short));
        *mem = (unsigned short)val;
        ptr = mem;
        break;
    }
    case HUI8:
    {
        unsigned long val = PyLong_AsUnsignedLong(obj);
        if (PyErr_Occurred())
            return NULL;
        unsigned char *mem = (unsigned char *)hl_gc_alloc_noptr(sizeof(unsigned char));
        *mem = (unsigned char)val;
        ptr = mem;
        break;
    }
    default:
    {
        void *inner_ptr = NULL;

        if (PyObject_HasAttrString(obj, "_hlmod_ptr"))
        {
            PyObject *py_hlptr = PyObject_GetAttrString(obj, "_hlmod_ptr");
            if (py_hlptr == NULL)
                return NULL;

            if (Py_IS_TYPE(py_hlptr, &HlPtrType))
            {
                inner_ptr = ((HlPtr *)py_hlptr)->ptr;
            }
            else
            {
                PyErr_SetString(PyExc_TypeError, "Attribute '_hlmod_ptr' was not of type hlmod.HlPtr.");
            }
            Py_DECREF(py_hlptr);
        }
        else if (Py_IS_TYPE(obj, &HlPtrType))
        {
            inner_ptr = ((HlPtr *)obj)->ptr;
        }
        else if (PyLong_Check(obj))
        {
            inner_ptr = PyLong_AsVoidPtr(obj);
        }

        if (PyErr_Occurred())
            return NULL;

        if (inner_ptr == NULL)
        {
            PyErr_Format(PyExc_TypeError, "Expected a subclass of HlObject, an hlmod.HlPtr, or an int pointer, but got %s", Py_TYPE(obj)->tp_name);
            return NULL;
        }

        void **outer_ptr = (void **)hl_gc_alloc_raw(sizeof(void *));
        *outer_ptr = inner_ptr;
        ptr = outer_ptr;
        break;
    }
    }

    return ptr;
}

#pragma region Field Access

PyObject *hlmod_py_get_obj_field(PyObject *self, PyObject *args)
{
    PyObject *hlobj_ptr;
    int field_index;

    if (!PyArg_ParseTuple(args, "O!i", &HlPtrType, &hlobj_ptr, &field_index))
    {
        return NULL;
    }

    vobj *obj = (vobj *)((HlPtr *)hlobj_ptr)->ptr;
    if (obj == NULL)
    {
        PyErr_SetString(PyExc_ValueError, "Cannot get field from a null HlPtr.");
        return NULL;
    }

    hl_runtime_obj *rt = hl_get_obj_rt(obj->t);
    // printf("rt points to %p\n", rt);
    // printf("t is %p\n", rt->t);
    // printf("field indexes at %p\n", rt->fields_indexes);

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

    vobj *obj = (vobj *)((HlPtr *)hlobj_ptr)->ptr;
    if (obj == NULL)
    {
        PyErr_SetString(PyExc_ValueError, "Cannot set field on a null HlPtr.");
        return NULL;
    }

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
    if (hl_value_ptr == NULL && PyErr_Occurred())
    {
        return NULL;
    }

    switch (field_type->kind)
    {
    case HI32:
    case HUI16:
    case HUI8:
    case HBOOL:
        *(int *)field_ptr = hl_value_ptr ? *(int *)hl_value_ptr : 0;
        break;
    case HF64:
        *(double *)field_ptr = hl_value_ptr ? *(double *)hl_value_ptr : 0.0;
        break;
    case HF32:
        *(float *)field_ptr = hl_value_ptr ? *(float *)hl_value_ptr : 0.0f;
        break;
    default:
        *(void **)field_ptr = hl_value_ptr ? *(void **)hl_value_ptr : NULL;
        break;
    }

    Py_RETURN_NONE;
}
#pragma endregion

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

    for (int i = 0; i < g_module->code->nglobals; i++)
    {
        hl_type *current_global_type = g_module->code->globals[i];

        if (current_global_type == target_type)
        {
            // printf("Found global g@%i\n", i);
            void *addr = g_module->globals_data + g_module->globals_indexes[i];
            // printf("addr: %p\n", addr);
            return hlmod_cast_to_py(target_type, addr);
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

PyObject *hlmod_py_findex_for_name(PyObject *self, PyObject *args)
{
    const char *name;
    
    if (!PyArg_ParseTuple(args, "s", &name)) {
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
    PyObject *py_args_tuple;

    if (!PyArg_ParseTuple(args, "iO!", &findex, &PyTuple_Type, &py_args_tuple))
    {
        PyErr_SetString(PyExc_TypeError, "Usage: call(findex: int, args: tuple)");
        return NULL;
    }

    if (findex < 0 || findex >= g_module->code->nfunctions) {
        PyErr_Format(PyExc_IndexError, "Function index %d is out of bounds.", findex);
        return NULL;
    }
    hl_function *f = g_module->code->functions + g_module->functions_indexes[findex];
    hl_type_fun *fun_type = f->type->fun;
    int nargs = fun_type->nargs;

    if (PyTuple_Size(py_args_tuple) != nargs)
    {
        PyErr_Format(PyExc_TypeError, "Haxe function f@%d expected %d arguments, but got %zd", findex, nargs, PyTuple_Size(py_args_tuple));
        return NULL;
    }

    vdynamic *vargs[HL_MAX_ARGS];
    if (nargs > HL_MAX_ARGS)
    {
        PyErr_SetString(PyExc_ValueError, "Cannot call Haxe function with more than HL_MAX_ARGS arguments.");
        return NULL;
    }

    for (int i = 0; i < nargs; i++)
    {
        PyObject *py_arg = PyTuple_GetItem(py_args_tuple, i);
        hl_type *hl_arg_type = fun_type->args[i];

        void *hl_val_ptr = hlmod_cast_to_hl(py_arg, hl_arg_type);
        if (hl_val_ptr == NULL)
        {
            return NULL;
        }

        vargs[i] = hl_make_dyn(hl_val_ptr, hl_arg_type);
    }

    vclosure cl;
    cl.t = f->type;
    cl.fun = g_module->functions_ptrs[findex];
    cl.hasValue = 0;

    bool is_exc;
    vdynamic *hl_result = hl_dyn_call_safe(&cl, nargs > 0 ? vargs : NULL, nargs, &is_exc);

    if (is_exc)
    {
        uchar *u_exc_str = hl_to_string(hl_result);
        char *exc_str_utf8 = hl_to_utf8(u_exc_str);
        PyErr_Format(PyExc_RuntimeError, "An exception occurred in the Haxe function f@%d: %s", findex, exc_str_utf8);
        return NULL;
    }

    if (fun_type->ret->kind == HVOID)
    {
        Py_RETURN_NONE;
    }

    PyObject *py_result = NULL;
    if (!hl_is_dynamic(fun_type->ret)) {
        py_result = hlmod_cast_to_py(fun_type->ret, &hl_result->v);
    } else {
        py_result = hlmod_cast_to_py(fun_type->ret, &hl_result);
    }
    
    if (py_result == NULL)
    {
        return NULL;
    }

    return py_result;
}

#pragma endregion

#pragma region HL-side closures



#pragma endregion

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

int jit_dispatch_hook(int findex, int nargs, void **args)
{
    if (is_passthrough(findex)) {
        return 0;
    }

    HookRegistryEntry *entry;
    HASH_FIND_INT(g_hook_registry, &findex, entry);

    if (entry == NULL)
    {
        return 0;
    }

    void** safe_args = NULL;
    if (nargs > 0) {
        safe_args = (void**)malloc(nargs * sizeof(void*));
        if (safe_args == NULL) { PyErr_NoMemory(); PyErr_Print(); return 0; }
        memcpy(safe_args, args, nargs * sizeof(void*));
    }

    g_return_value_int = 0;
    g_return_value_double = 0.0;

    hl_blocking(true);
    PyGILState_STATE gstate;
    gstate = PyGILState_Ensure();

    hl_function *f = g_module->code->functions + g_module->functions_indexes[findex];
    hl_type_fun *fun_type = f->type->fun;

    PyObject *pArgs = PyTuple_New(nargs + 1); // for hook
    if (!pArgs)
    {
        PyErr_Print();
        PyGILState_Release(gstate);
        hl_blocking(false);
        return 0;
    }

    HlHook *hook_obj = (HlHook *)HlHookType.tp_new(&HlHookType, NULL, NULL);
    if (!hook_obj)
    {
        PyErr_Print();
        Py_DECREF(pArgs);
        PyGILState_Release(gstate);
        hl_blocking(false);
        return 0;
    }
    hook_obj->findex = findex;
    PyTuple_SetItem(pArgs, 0, (PyObject *)hook_obj);

    for (int i = 0; i < nargs; i++)
    {
        PyObject *pValue = hlmod_cast_to_py(fun_type->args[i], safe_args[i]);
        if (pValue == NULL)
        {
            PyErr_Print();
            Py_DECREF(pArgs);
            PyGILState_Release(gstate);
            hl_blocking(false);
            return 0;
        }

        PyTuple_SetItem(pArgs, i + 1, pValue);
    }

    PyObject *pResult = PyObject_CallObject(entry->callback, pArgs);
    
    if (safe_args != NULL) {
        free(safe_args);
    }

    Py_DECREF(pArgs);

    if (pResult == NULL)
    {
        PyErr_Print();

        PyGILState_Release(gstate);
        hl_blocking(false);
        return 0;
    }
    else
    {
        int res = 1;
        if (Py_IsNone(pResult) != 1)
        {
            if (f->type->fun->ret->kind == HF32 || f->type->fun->ret->kind == HF64)
            {
                g_return_value_double = PyFloat_AsDouble(pResult);
                if (PyErr_Occurred())
                {
                    PyErr_Print();
                }
            }
            else
            {
                void *hl_val = hlmod_cast_to_hl(pResult, fun_type->ret);
                if (hl_val != NULL)
                {
                    if (hl_is_ptr(fun_type->ret))
                    {
                        // For pointer types, hl_val is a pointer to the pointer (e.g., vobj**)
                        g_return_value_int = (int64_t)(*(void **)hl_val);
                    }
                    else
                    {
                        // For primitive types, hl_val is a pointer to the value itself (e.g., int*)
                        g_return_value_int = (int64_t)hl_val;
                    }
                }
            }
            res = 1;
        }
        Py_DECREF(pResult);
        PyGILState_Release(gstate);
        hl_blocking(false);
        return res;
    }
}

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