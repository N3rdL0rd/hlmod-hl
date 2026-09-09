#include <stdio.h>
#include <hl.h>
#include <hlmod.h>
#include <Python.h>
#include <structmember.h>
#include <std_globals.h>
#include <hlmod_python.h>
#include <limits.h>
#include <float.h>
#include <math.h>

static int hlmod_type_index(hl_type *type)
{
    if (!g_code || !type) return -1;
    uintptr_t address = (uintptr_t)type, base = (uintptr_t)g_code->types;
    if (address < base || (address - base) / sizeof(hl_type) >= (size_t)g_code->ntypes ||
        (address - base) % sizeof(hl_type)) return -1;
    return (int)((address - base) / sizeof(hl_type));
}

static hl_type *hlmod_function_type(int findex)
{
    if (!g_module || !g_module->code) {
        PyErr_SetString(PyExc_RuntimeError, "hlmod is not initialized.");
        return NULL;
    }
    if (findex < 0 || findex >= g_module->code->nfunctions + g_module->code->nnatives) {
        PyErr_Format(PyExc_IndexError, "Function index %d is out of bounds.", findex);
        return NULL;
    }
    hl_type *type = g_module->ctx.functions_types[findex];
    if (!type || type->kind != HFUN || !type->fun || !g_module->functions_ptrs[findex]) {
        PyErr_SetString(PyExc_TypeError, "Function metadata is unavailable.");
        return NULL;
    }
    return type;
}

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

static int push_passthrough(int findex) {
    if (g_passthrough_stack_size >= g_passthrough_stack_capacity) {
        int new_capacity = g_passthrough_stack_capacity == 0 ? 8 : g_passthrough_stack_capacity * 2;
        int *stack = realloc(g_passthrough_stack, new_capacity * sizeof(int));
        if (!stack) { PyErr_NoMemory(); return -1; }
        g_passthrough_stack = stack;
        g_passthrough_stack_capacity = new_capacity;
    }
    g_passthrough_stack[g_passthrough_stack_size++] = findex;
    return 0;
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
static PyObject *g_hlobj_module = NULL;
static PyObject *g_hlcallable_class = NULL;
static PyObject *g_hlvirtual_class = NULL;
typedef struct HlMethodSignature {
    hl_type *method;
    hl_type callable;
    struct HlMethodSignature *next;
} HlMethodSignature;
static HlMethodSignature *g_method_signatures = NULL;

#pragma region HlPtr
static PyObject *HlPtr_get_type_index(HlPtr *self, void *closure);
static PyObject *HlPtr_get_trusted(HlPtr *self, void *closure);
static PyObject *HlPtr_get_ptr(HlPtr *self, void *closure);
static PyObject *HlPtr_get_kind(HlPtr *self, void *closure);
static int HlPtr_init(HlPtr *self, PyObject *args, PyObject *kwds);
static void HlPtr_dealloc(HlPtr *self);
static int HlPtr_traverse(HlPtr *self, visitproc visit, void *arg)
{
    return self->root ? hlmod_python_traverse(self->root, visit, arg) : 0;
}

static int HlPtr_clear(HlPtr *self)
{
    if (self->root) hlmod_python_clear(self->root);
    return 0;
}

static PyObject *hlmod_py_make_hlcallable(vclosure *cl);
static PyObject *HlHook_get_findex(HlHook *self, void *closure);
static PyObject *HlHook_as_closure(HlHook *self, PyObject *Py_UNUSED(ignored));


static PyGetSetDef HlPtr_getsetters[] = {
    {"ptr", (getter)HlPtr_get_ptr, NULL, "The raw pointer value", NULL},
    {"kind", (getter)HlPtr_get_kind, NULL, "The HL type kind enum", NULL},
    {"type_index", (getter)HlPtr_get_type_index, NULL, "Bytecode type index, or None", NULL},
    {"trusted", (getter)HlPtr_get_trusted, NULL, "Whether native type provenance is available", NULL},
    {NULL}};

PyTypeObject HlPtrType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "hlmod.HlPtr",
    .tp_doc = "Opaque raw address wrapper; only native-created pointers have trusted type provenance.",
    .tp_basicsize = sizeof(HlPtr),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_new = PyType_GenericNew,
    .tp_getset = HlPtr_getsetters,
    .tp_init = (initproc)HlPtr_init,
    .tp_dealloc = (destructor)HlPtr_dealloc,
    .tp_traverse = (traverseproc)HlPtr_traverse,
    .tp_clear = (inquiry)HlPtr_clear,
};

PyObject *hlmod_ptr_new(void *ptr, hl_type *type)
{
    if (!type || !hl_is_ptr(type) || type->kind == HPACKED || type->kind == HGUID) {
        PyErr_SetString(PyExc_TypeError, "A pointer requires a supported native pointer type.");
        return NULL;
    }
    HlPtr *self = (HlPtr *)HlPtrType.tp_alloc(&HlPtrType, 0);
    if (!self) return NULL;
    self->ptr = ptr;
    self->kind = type->kind;
    self->type = type;
    if (ptr && hl_is_gc_ptr(ptr)) {
        self->root = &self->ptr;
        hl_add_root(self->root);
        hlmod_python_root(self->root, true);
    }
    return (PyObject *)self;
}

static void HlPtr_dealloc(HlPtr *self)
{
    PyObject_GC_UnTrack(self);
    if (self->root) {
        hlmod_python_root(self->root, false);
        hl_remove_root(self->root);
    }
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int HlPtr_init(HlPtr *self, PyObject *args, PyObject *kwds)
{
    PyObject *address;
    int kind = HVOID;
    static char *kwlist[] = {"ptr", "kind", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|i", kwlist, &address, &kind)) return -1;
    void *ptr = PyLong_AsVoidPtr(address);
    if (PyErr_Occurred()) return -1;
    if (kind < HVOID || kind >= HLAST) {
        PyErr_SetString(PyExc_ValueError, "Invalid HL type kind.");
        return -1;
    }
    if (self->type) {
        PyErr_SetString(PyExc_TypeError, "Cannot reinitialize a trusted native pointer.");
        return -1;
    }
    if (self->root) hl_remove_root(self->root);
    self->root = NULL;
    self->ptr = ptr;
    self->kind = kind;
    /* Caller-supplied addresses are never dereferenced, rooted, or trusted. */
    return 0;
}

static PyObject *HlPtr_get_type_index(HlPtr *self, void *closure)
{
    int index = hlmod_type_index(self->type);
    if (index < 0) Py_RETURN_NONE;
    return PyLong_FromLong(index);
}

static PyObject *HlPtr_get_trusted(HlPtr *self, void *closure)
{
    return PyBool_FromLong(self->type != NULL);
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
static PyObject *hlmod_py_make_hlcallable(vclosure *cl)
{
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

    PyObject *py_ptr = hlmod_ptr_new(cl, cl->t);
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

static PyObject *hlmod_py_make_hlvirtual(void *ptr, hl_type *type)
{
    if (ptr == NULL)
    {
        Py_RETURN_NONE;
    }

    if (g_hlobj_module == NULL) {
        g_hlobj_module = PyImport_ImportModule("hlobj");
        if (g_hlobj_module == NULL) {
            PyErr_SetString(PyExc_ImportError, "Failed to import the 'hlobj' module. Is it in `mods/`?");
            return NULL;
        }
    }

    if (g_hlvirtual_class == NULL)
    {
        g_hlvirtual_class = PyObject_GetAttrString(g_hlobj_module, "HlVirtual");
        if (g_hlvirtual_class == NULL) {
            PyErr_SetString(PyExc_AttributeError, "Could not find 'HlVirtual' class in 'hlobj' module.");
            return NULL;
        }
    }

    if (g_code == NULL)
    {
        PyErr_SetString(PyExc_RuntimeError, "hlmod code metadata is unavailable.");
        return NULL;
    }

    int type_idx = hlmod_type_index(type);
    PyObject *py_class = NULL;
    if (type_idx >= 0 && type_idx < g_hlobjs_l)
    {
        py_class = g_hlobjs[type_idx];
    }
    if (py_class == NULL)
    {
        py_class = g_hlvirtual_class;
    }

    PyObject *py_arg_ptr = hlmod_ptr_new(ptr, type);
    if (py_arg_ptr == NULL)
    {
        return NULL;
    }

    PyObject *wrap = PyObject_GetAttrString(py_class, "_hlmod_wrap");
    PyObject *py_instance = wrap ? PyObject_CallOneArg(wrap, py_arg_ptr) : NULL;
    Py_XDECREF(wrap);
    Py_DECREF(py_arg_ptr);
    return py_instance;
}

static PyObject *HlHook_get_findex(HlHook *self, void *closure)
{
    return PyLong_FromLong(self->findex);
}

static PyObject *hlmod_invoke(vclosure *closure, PyObject *arguments, int original, int direct)
{
    hl_type_fun *fun = closure->t->fun;
    int nargs = fun->nargs;
    /* std/fun.c's dynamic dispatcher has nine machine argument slots. */
    int bound = closure->hasValue && fun->parent != NULL;
    if (nargs < 0 || nargs + bound > 9) {
        PyErr_SetString(PyExc_ValueError, "HL dynamic calls support at most nine arguments including a bound receiver.");
        return NULL;
    }
    if (PyTuple_GET_SIZE(arguments) != nargs) {
        PyErr_Format(PyExc_TypeError, "Haxe call expected %d arguments, got %zd.", nargs, PyTuple_GET_SIZE(arguments));
        return NULL;
    }
    vdynamic *values[9] = {0};
    for (int i = 0; i < nargs; i++) {
        if (fun->args[i]->kind == HSTRUCT) {
            PyErr_SetString(PyExc_TypeError, "Struct arguments are unsupported by the HL dynamic dispatcher.");
            return NULL;
        }
        void *slot = hlmod_cast_to_hl(PyTuple_GET_ITEM(arguments, i), fun->args[i]);
        if (!slot) return NULL;
        values[i] = hl_make_dyn(slot, fun->args[i]);
    }
    if (original >= 0 && push_passthrough(original) < 0) return NULL;
    int saved_bypass = hlmod_python_bypass;
    if (direct >= 0) hlmod_python_bypass = direct;
    int64_t saved_int = g_return_value_int;
    double saved_double = g_return_value_double;
    bool exception;
    /* Keep bridge-owned inputs alive independently of Python GC while native
       code runs, including workers that call back into Python before joining. */
    for (int i = 0; i < nargs; i++) hl_add_root(&values[i]);
    hl_add_root(&closure);
    PyThreadState *python_state = PyEval_SaveThread();
    vdynamic *result = hl_dyn_call_safe(closure, nargs ? values : NULL, nargs, &exception);
    hl_add_root(&result);
    /* Waiting for the GIL is a GC-safe blocking region, not HL execution. */
    hl_blocking(true);
    PyEval_RestoreThread(python_state);
    hl_blocking(false);
    hl_remove_root(&result);
    hl_remove_root(&closure);
    for (int i = 0; i < nargs; i++) hl_remove_root(&values[i]);
    g_return_value_int = saved_int;
    g_return_value_double = saved_double;
    hlmod_python_bypass = saved_bypass;
    if (original >= 0) pop_passthrough();
    if (exception) {
        PyErr_Format(PyExc_RuntimeError, "Haxe call raised: %s", hl_to_utf8(hl_to_string(result)));
        return NULL;
    }
    if (fun->ret->kind == HVOID || !result) Py_RETURN_NONE;
    return hlmod_cast_to_py(fun->ret, hl_is_dynamic(fun->ret) ? (void *)&result : (void *)&result->v);
}

static PyObject *HlHook_call_original(HlHook *self, PyObject *py_args)
{
    hl_type *type = hlmod_function_type(self->findex);
    if (!type) return NULL;
    vclosure closure = {0};
    closure.t = type;
    closure.fun = g_module->functions_ptrs[self->findex];
    return hlmod_invoke(&closure, py_args, self->findex, self->findex);
}

static PyObject *HlHook_as_closure(HlHook *self, PyObject *Py_UNUSED(ignored))
{
    hl_type *type = hlmod_function_type(self->findex);
    if (!type) return NULL;
    vclosure *cl = hl_alloc_closure_void(type, g_module->functions_ptrs[self->findex]);
    if (cl == NULL)
    {
        PyErr_SetString(PyExc_MemoryError, "Failed to allocate closure wrapper.");
        return NULL;
    }

    return hlmod_py_make_hlcallable(cl);
}

static PyGetSetDef HlHook_getsetters[] = {
    {"findex", (getter)HlHook_get_findex, NULL, "The function index this hook was invoked for.", NULL},
    {NULL}};

static PyMethodDef HlHook_methods[] = {
    {"call_original", (PyCFunction)HlHook_call_original, METH_VARARGS, "Calls the original Haxe function."},
    {"as_closure", (PyCFunction)HlHook_as_closure, METH_NOARGS, "Returns the original hooked function as an HlCallable."},
    {NULL}};

PyTypeObject HlHookType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "hlmod.Hook",
    .tp_doc = "Hook context object",
    .tp_basicsize = sizeof(HlHook),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = PyType_GenericNew,
    .tp_getset = HlHook_getsetters,
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

    if (!g_code || type_idx < 0 || type_idx >= g_code->ntypes) {
        PyErr_Format(PyExc_IndexError, "Type index %d is out of bounds.", type_idx);
        return NULL;
    }

    if (!PyType_Check(py_class))
    {
        PyErr_SetString(PyExc_TypeError, "Second argument must be a class.");
        return NULL;
    }

    if (type_idx >= g_hlobjs_l)
    {
        int new_len = type_idx + 1;
        PyObject **registry = realloc(g_hlobjs, sizeof(PyObject *) * new_len);
        if (!registry) return PyErr_NoMemory();
        g_hlobjs = registry;
        memset(g_hlobjs + g_hlobjs_l, 0, sizeof(PyObject *) * (new_len - g_hlobjs_l));
        g_hlobjs_l = new_len;
    }

    PyObject *previous = g_hlobjs[type_idx];
    g_hlobjs[type_idx] = Py_NewRef(py_class);
    Py_XDECREF(previous);

    Py_RETURN_NONE;
}

void hlmod_shutdown(void)
{
    PyObject **classes = g_hlobjs;
    int count = g_hlobjs_l;
    g_hlobjs = NULL;
    g_hlobjs_l = 0;
    g_method_signatures = NULL; /* Storage belongs to the bytecode allocator. */
    for (int i = 0; i < count; i++) Py_XDECREF(classes[i]);
    free(classes);
    Py_CLEAR(g_hlcallable_class);
    Py_CLEAR(g_hlvirtual_class);
    Py_CLEAR(g_hlobj_module);
    hlmod_hook_registry_shutdown();
    free(g_passthrough_stack);
    g_passthrough_stack = NULL;
    g_passthrough_stack_size = 0;
    g_passthrough_stack_capacity = 0;
    g_return_value_int = 0;
    g_return_value_double = 0;
}

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
static HlPtr *hlmod_extract_pointer(PyObject *obj)
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

static void *hlmod_require_pointer(HlPtr *pointer, hl_type_kind kind)
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
