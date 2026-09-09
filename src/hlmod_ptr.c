#include <hlmod_python.h>
#include "hlmod_internal.h"


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

PyObject *hlmod_py_make_hlcallable(vclosure *cl);
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


PyObject *hlmod_py_make_hlcallable(vclosure *cl)
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

PyObject *hlmod_py_make_hlvirtual(void *ptr, hl_type *type)
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

PyObject *hlmod_invoke(vclosure *closure, PyObject *arguments, int original, int direct)
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

