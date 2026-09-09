#include "hlmod_python.h"
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

static hl_type type_handle_type = { .kind = HTYPE };

/* Metadata and executable adapters have module lifetime. Instances and callback
   environments have GC lifetime; the registry never roots an HL allocation. */
typedef struct NativeCall NativeCall;
typedef struct PythonType PythonType;
typedef struct PythonPeer PythonPeer;
struct NativeCall {
    void (*finalize)(void *);
    hl_type *signature;
    PyObject *callable;
    PythonPeer *peer;
    void *code;
    int codesize;
    int findex;
    int hash;
    NativeCall *next;
};
struct PythonType {
    hl_type type;
    hl_type_obj object;
    hl_module_context context;
    hl_obj_field lifetime_field;
    PyObject *python;
    NativeCall *methods;
    int lifetime_offset;
    PythonType *next;
};
struct PythonPeer {
    void *ptr;
    PyObject *weak;
    PyObject *strong;
    NativeCall *callback;
    bool native_live;
    bool dead;
    UT_hash_handle hh;
};

/* Calling a weakref returns an owned reference on all supported Python versions. */
static PyObject *peer_reference(PythonPeer *peer) {
    if(peer->strong) return Py_NewRef(peer->strong);
    PyObject *object = PyObject_CallNoArgs(peer->weak);
    if(object == Py_None) Py_CLEAR(object);
    return object;
}

typedef struct PythonRoot {
    void **slot;
    PyObject *edges;
    size_t snapshot_index;
    UT_hash_handle hh;
} PythonRoot;
typedef struct {
    void (*finalize)(void *);
    PythonPeer *peer;
} PeerFinalizer;
typedef struct Adapter {
    hl_type *signature;
    hl_type full_type;
    hl_type_fun full_fun;
    void *code;
    int codesize;
    struct Adapter *next;
} Adapter;

static PythonType *python_types;

static PythonType *published_python_types(void) {
#if defined(HL_VCC)
    return _InterlockedCompareExchangePointer((void *volatile *)&python_types, NULL, NULL);
#else
    return __atomic_load_n(&python_types, __ATOMIC_ACQUIRE);
#endif
}

static void publish_python_type(PythonType *type) {
    type->next = published_python_types();
#if defined(HL_VCC)
    _InterlockedExchangePointer((void *volatile *)&python_types, type);
#else
    __atomic_store_n(&python_types, type, __ATOMIC_RELEASE);
#endif
}
static PythonPeer *python_peers;
static PythonRoot *python_roots;
static Adapter *adapters;
static PyObject *gc_callback;
static bool collecting;
static bool initialized;
THREAD_LOCAL int hlmod_python_bypass = -1;
static THREAD_LOCAL vdynamic *constructor_instance;
static THREAD_LOCAL hl_type *constructor_type;

typedef struct {
    PyObject_HEAD
    PyObject *callable;
    PyObject *weakrefs;
} CallbackOwner;

static int owner_traverse(CallbackOwner *self, visitproc visit, void *arg) {
    Py_VISIT(self->callable);
    return 0;
}

static int owner_clear(CallbackOwner *self) {
    Py_CLEAR(self->callable);
    return 0;
}

static void owner_dealloc(CallbackOwner *self) {
    PyObject_GC_UnTrack(self);
    if(self->weakrefs) PyObject_ClearWeakRefs((PyObject*)self);
    owner_clear(self);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyTypeObject CallbackOwnerType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "hlmod.CallbackOwner",
    .tp_basicsize = sizeof(CallbackOwner),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)owner_traverse,
    .tp_clear = (inquiry)owner_clear,
    .tp_dealloc = (destructor)owner_dealloc,
    .tp_weaklistoffset = offsetof(CallbackOwner, weakrefs),
};

typedef struct {
    PythonRoot *root;
    size_t offset, count;
} RootSnapshot;

typedef struct {
    void *ptr;
    PythonPeer *peer;
    UT_hash_handle hh;
} PeerSnapshot;

static RootSnapshot *snapshot_roots, *snapshot_current;
static PeerSnapshot *snapshot_peers, *snapshot_peer_index;
static PythonPeer **snapshot_edges;
static size_t snapshot_nroots, snapshot_npeers, snapshot_count, snapshot_capacity;
static bool snapshot_failed, snapshot_active;

/* GC calls this serially with the world stopped. Only libc scratch allocation
   is allowed here; Python references are materialized after the GC unlocks. */
static bool trace_foreign(void **slot, void *ptr) {
    if(!slot) return snapshot_active && snapshot_npeers != 0;
    if(!ptr) {
        PythonRoot *root;
        HASH_FIND_PTR(python_roots, &slot, root);
        snapshot_current = root ? &snapshot_roots[root->snapshot_index] : NULL;
        if(snapshot_current) snapshot_current->offset = snapshot_count;
        return true;
    }
    if(snapshot_failed || !snapshot_current) return false;
    PeerSnapshot *entry;
    HASH_FIND_PTR(snapshot_peer_index, &ptr, entry);
    if(!entry || entry->peer->dead) return true;
    if(snapshot_count == snapshot_capacity) {
        size_t capacity = snapshot_capacity ? snapshot_capacity * 2 : 64;
        if(capacity < snapshot_capacity || capacity > SIZE_MAX / sizeof(*snapshot_edges)) {
            snapshot_failed = true;
            return false;
        }
        PythonPeer **edges = realloc(snapshot_edges, capacity * sizeof(*edges));
        if(!edges) {
            snapshot_failed = true;
            return false;
        }
        snapshot_edges = edges;
        snapshot_capacity = capacity;
    }
    snapshot_edges[snapshot_count++] = entry->peer;
    snapshot_current->count++;
    return true;
}

static int refresh_foreign_edges(void) {
    PythonRoot *root, *rtmp;
    PythonPeer *peer, *ptmp;
    size_t i = 0, j;
    PyObject *holders = NULL, *new_edges = NULL;
    int result = -1;
    snapshot_nroots = HASH_COUNT(python_roots);
    snapshot_npeers = HASH_COUNT(python_peers);
    snapshot_count = snapshot_capacity = 0;
    snapshot_failed = false;
    snapshot_roots = calloc(snapshot_nroots + 1, sizeof(*snapshot_roots));
    snapshot_peers = calloc(snapshot_npeers + 1, sizeof(*snapshot_peers));
    holders = PyTuple_New(snapshot_nroots);
    new_edges = PyTuple_New(snapshot_nroots);
    if(!snapshot_roots || !snapshot_peers || !holders || !new_edges) {
        if(!PyErr_Occurred()) PyErr_NoMemory();
        goto done;
    }
    HASH_ITER(hh, python_roots, root, rtmp) {
        root->snapshot_index = i;
        snapshot_roots[i].root = root;
        PyObject *holder = (PyObject*)((char*)root->slot - offsetof(HlPtr, ptr));
        PyTuple_SET_ITEM(holders, i++, Py_NewRef(holder));
    }
    i = 0;
    HASH_ITER(hh, python_peers, peer, ptmp) {
        PeerSnapshot *entry = &snapshot_peers[i++];
        entry->ptr = peer->callback ? (void*)peer->callback : peer->ptr;
        entry->peer = peer;
        HASH_ADD_PTR(snapshot_peer_index, ptr, entry);
    }
    snapshot_active = true;
    hl_gc_major();
    snapshot_active = false;
    if(snapshot_failed) {
        PyErr_NoMemory();
        goto done;
    }
    for(i = 0; i < snapshot_nroots; i++) {
        PyObject *edges = PyList_New(0);
        if(!edges) goto done;
        PyTuple_SET_ITEM(new_edges, i, edges);
        RootSnapshot *snapshot = &snapshot_roots[i];
        for(j = 0; j < snapshot->count; j++) {
            peer = snapshot_edges[snapshot->offset + j];
            if(peer->dead) continue;
            PyObject *obj = peer_reference(peer);
            if(!obj) {
                if(PyErr_Occurred()) goto done;
                continue;
            }
            int appended = PyList_Append(edges, obj);
            Py_DECREF(obj);
            if(appended < 0) goto done;
        }
    }
    for(i = 0; i < snapshot_nroots; i++) {
        root = snapshot_roots[i].root;
        Py_XSETREF(root->edges, Py_NewRef(PyTuple_GET_ITEM(new_edges, i)));
    }
    result = 0;
done:
    HASH_CLEAR(hh, snapshot_peer_index);
    snapshot_current = NULL;
    free(snapshot_roots);
    free(snapshot_peers);
    free(snapshot_edges);
    snapshot_roots = NULL;
    snapshot_peers = NULL;
    snapshot_edges = NULL;
    Py_XDECREF(new_edges);
    Py_XDECREF(holders);
    return result;
}

static PythonType *find_type(hl_type *type) {
    PythonType *p;
    for(p = published_python_types(); p; p = p->next)
        if(&p->type == type) return p;
    return NULL;
}

PyObject *hlmod_python_type(hl_type *type) {
    PythonType *p = find_type(type);
    return p ? p->python : NULL;
}

PyObject *hlmod_python_proxy(void *ptr) {
    PythonPeer *p;
    HASH_FIND_PTR(python_peers, &ptr, p);
    if(!p || p->dead || p->callback) return NULL;
    return peer_reference(p);
}

void hlmod_python_root(void **slot, bool add) {
    PythonRoot *r;
    HASH_FIND_PTR(python_roots, &slot, r);
    if(add && !r) {
        r = malloc(sizeof(*r));
        if(!r) Py_FatalError("Unable to track HL root");
        r->slot = slot;
        r->edges = NULL;
        HASH_ADD_PTR(python_roots, slot, r);
    } else if(!add && r) {
        HASH_DEL(python_roots, r);
        Py_XDECREF(r->edges);
        free(r);
    }
}

static bool defer_root(void **slot) {
    PythonRoot *r;
    HASH_FIND_PTR(python_roots, &slot, r);
    return r != NULL;
}

/* Runs under the stopped HL world. Do not touch Python or acquire its GIL. */
static void observe_native(bool (*marked)(void *)) {
    PythonPeer *p, *tmp;
    HASH_ITER(hh, python_peers, p, tmp)
        p->native_live = !p->dead && marked(p->callback ? (void*)p->callback : p->ptr);
}

static void peer_finalize(void *value) {
    PeerFinalizer *f = value;
    if(f->peer) f->peer->dead = true;
}

static void callback_finalize(void *value) {
    NativeCall *call = value;
    if(call->peer) call->peer->dead = true;
}

static void release_peers(bool release_unretained) {
    PythonPeer *p, *tmp;
    HASH_ITER(hh, python_peers, p, tmp) {
        if(p->dead) {
            HASH_DEL(python_peers, p);
            Py_XDECREF(p->strong);
            Py_XDECREF(p->weak);
            free(p);
        } else if(release_unretained && !p->native_live) {
            Py_CLEAR(p->strong);
        }
    }
}


int hlmod_python_traverse(void **slot, visitproc visit, void *arg) {
    PythonRoot *r;
    HASH_FIND_PTR(python_roots, &slot, r);
    if(r) Py_VISIT(r->edges);
    return 0;
}

void hlmod_python_clear(void **slot) {
    PythonRoot *r;
    HASH_FIND_PTR(python_roots, &slot, r);
    if(r) Py_CLEAR(r->edges);
}

void hlmod_python_retain(void) {
    PythonPeer *p, *tmp;
    if(!initialized || collecting) return;
    release_peers(false);
    HASH_ITER(hh, python_peers, p, tmp) {
        if(!p->strong && p->weak) {
            p->strong = peer_reference(p);
        }
    }
}

static PyObject *python_gc_callback(PyObject *self, PyObject *args) {
    const char *phase;
    PyObject *info;
    if(!PyArg_ParseTuple(args, "sO", &phase, &info)) return NULL;
    if(strcmp(phase, "start") == 0 && !collecting) {
        collecting = true;
        if(refresh_foreign_edges() < 0) {
            collecting = false;
            return NULL;
        }
        release_peers(true);
    } else if(strcmp(phase, "stop") == 0) {
        collecting = false;
        hlmod_python_retain();
    }
    Py_RETURN_NONE;
}

static PyMethodDef gc_method = {"_hl_gc", python_gc_callback, METH_VARARGS, NULL};

int hlmod_python_init(void) {
    PyObject *gc, *callbacks;
    if(initialized) return 0;
    if(PyType_Ready(&CallbackOwnerType) < 0) return -1;
    gc = PyImport_ImportModule("gc");
    if(!gc) return -1;
    callbacks = PyObject_GetAttrString(gc, "callbacks");
    Py_DECREF(gc);
    if(!callbacks) return -1;
    gc_callback = PyCFunction_New(&gc_method, NULL);
    if(!gc_callback || PyList_Append(callbacks, gc_callback) < 0) {
        Py_DECREF(callbacks);
        Py_CLEAR(gc_callback);
        return -1;
    }
    Py_DECREF(callbacks);
    hl_gc_set_foreign_hooks(defer_root, observe_native, trace_foreign);
    initialized = true;
    return 0;
}

static hl_type *type_argument(PyObject *arg) {
    if(PyLong_Check(arg)) {
        long index = PyLong_AsLong(arg);
        if(PyErr_Occurred()) return NULL;
        if(!g_code || index < 0 || index >= g_code->ntypes) {
            PyErr_SetString(PyExc_IndexError, "Invalid HL type index");
            return NULL;
        }
        return g_code->types + index;
    }
    if(PyObject_TypeCheck(arg, &HlPtrType)) {
        HlPtr *p = (HlPtr*)arg;
        if(p->type && p->type->kind == HTYPE && p->ptr) return p->ptr;
    }
    PyErr_SetString(PyExc_TypeError, "Expected an HL type index or typed type handle");
    return NULL;
}

static bool supported_type(hl_type *t, bool result) {
    switch(t->kind) {
    case HVOID: return result;
    case HUI8: case HUI16: case HI32: case HI64: case HF32: case HF64:
    case HBOOL: case HBYTES: case HDYN: case HFUN: case HOBJ: case HARRAY:
    case HTYPE: case HREF: case HVIRTUAL: case HDYNOBJ: case HABSTRACT:
    case HENUM: case HNULL: return true;
    default: return false;
    }
}

static int check_signature(hl_type *t) {
    int i;
    if(!t || t->kind != HFUN || t->fun->nargs > HL_MAX_ARGS ||
       !supported_type(t->fun->ret, true)) goto unsupported;
    for(i = 0; i < t->fun->nargs; i++)
        if(!supported_type(t->fun->args[i], false)) goto unsupported;
    return 0;
unsupported:
    PyErr_SetString(PyExc_TypeError, "Unsupported Python callback signature");
    return -1;
}

static hl_function *find_method(hl_type *base, int hash) {
    hl_type *t;
    int i;
    for(t = base; t; t = t->obj->super) {
        for(i = 0; i < g_code->nfunctions; i++) {
            hl_function *f = g_code->functions + i;
            const uchar *name = fun_field_name(f);
            if(fun_obj(f) == t->obj && name && hl_hash_gen(name, false) == hash &&
               f->type->fun->nargs > 0 && f->type->fun->args[0]->kind == HOBJ)
                return f;
        }
    }
    return NULL;
}

static hl_field_lookup *method_lookup(hl_runtime_obj *rt, int hash) {
    for(; rt; rt = rt->parent) {
        int i;
        for(i = 0; i < rt->nlookup; i++)
            if(rt->lookup[i].hashed_name == hash) return rt->lookup + i;
    }
    return NULL;
}

static void free_python_type(PythonType *p) {
    NativeCall *c = p->methods;
    while(c) {
        NativeCall *next = c->next;
        Py_XDECREF(c->callable);
        if(c->code) hl_free_executable_memory(c->code, c->codesize);
        free(c);
        c = next;
    }
    Py_XDECREF(p->python);
    hl_free(&p->context.alloc);
    free((void*)p->object.name);
    free(p);
}

PyObject *hlmod_py_create_subclass(PyObject *self, PyObject *args) {
    PyObject *base_arg, *cls, *overrides, *key, *value, *name;
    Py_ssize_t pos = 0;
    hl_type *base;
    PythonType *p;
    if(!PyArg_ParseTuple(args, "OOO:create_subclass", &base_arg, &cls, &overrides)) return NULL;
    if(!PyType_Check(cls) || !PyDict_Check(overrides)) {
        PyErr_SetString(PyExc_TypeError, "Subclass registration requires a class and override dictionary");
        return NULL;
    }
    base = type_argument(base_arg);
    if(!base) return NULL;
    if(base->kind != HOBJ) {
        PyErr_SetString(PyExc_TypeError, "Only HL object classes support Python inheritance");
        return NULL;
    }
    if(hlmod_python_init() < 0) return NULL;
    p = calloc(1, sizeof(*p));
    if(!p) return PyErr_NoMemory();
    hl_alloc_init(&p->context.alloc);
    p->context.functions_ptrs = base->obj->m->functions_ptrs;
    p->context.functions_types = base->obj->m->functions_types;
    p->python = Py_NewRef(cls);
    p->type.kind = HOBJ;
    p->type.obj = &p->object;
    p->object.super = base;
    p->object.m = &p->context;
    p->object.nfields = 1;
    p->object.fields = &p->lifetime_field;
    p->lifetime_field.name = USTR("");
    p->lifetime_field.t = &hlt_bytes;
    p->lifetime_field.hashed_name = hl_hash_gen(USTR("__hlmod_python_peer"), false);
    name = PyObject_GetAttrString(cls, "__qualname__");
    if(!name) goto fail;
    const char *utf8 = PyUnicode_AsUTF8(name);
    if(!utf8) { Py_DECREF(name); goto fail; }
    size_t len = strlen(utf8) + 1;
    uchar *wide = malloc(len * sizeof(uchar));
    if(!wide) { Py_DECREF(name); PyErr_NoMemory(); goto fail; }
    hl_from_utf8(wide, (int)len, utf8);
    p->object.name = wide;
    Py_DECREF(name);
    hl_runtime_obj *rt = hl_get_obj_proto(&p->type);
    p->lifetime_offset = rt->fields_indexes[rt->nfields - 1];
    while(PyDict_Next(overrides, &pos, &key, &value)) {
        const char *method = PyUnicode_AsUTF8(key);
        if(!method) goto fail;
        int hash = hl_hash_utf8(method);
        hl_function *f = find_method(base, hash);
        if(!f || !PyCallable_Check(value)) {
            PyErr_Format(PyExc_TypeError, "Cannot override HL member %s", method);
            goto fail;
        }
        if(check_signature(f->type) < 0) goto fail;
        NativeCall *c = calloc(1, sizeof(*c));
        if(!c) { PyErr_NoMemory(); goto fail; }
        c->signature = f->type;
        c->callable = Py_NewRef(value);
        c->findex = f->findex;
        c->hash = hash;
        c->next = p->methods;
        p->methods = c;
        c->code = hl_jit_python_adapter(c->signature, c, false, &c->codesize);
        if(!c->code) { PyErr_NoMemory(); goto fail; }
        hl_field_lookup *lookup = method_lookup(rt, hash);
        if(lookup && lookup->field_index < 0)
            rt->methods[-lookup->field_index - 1] = c->code;
        hl_type *ancestor;
        for(ancestor = base; ancestor; ancestor = ancestor->obj->super) {
            int i;
            for(i = 0; i < ancestor->obj->nproto; i++) {
                hl_obj_proto *proto = ancestor->obj->proto + i;
                if(proto->hashed_name == hash && proto->pindex >= 0)
                    p->type.vobj_proto[proto->pindex] = c->code;
            }
        }
        int i;
        for(i = 0; i < rt->nbindings; i++) {
            hl_obj_field *field = hl_obj_field_fetch(base, rt->bindings[i].fid);
            if(field && field->hashed_name == hash) {
                if(!rt->bindings[i].closure) {
                    PyErr_Format(PyExc_TypeError, "Static binding %s cannot be overridden", method);
                    goto fail;
                }
                rt->bindings[i].ptr = c->code;
            }
        }
        if(hash == hl_hash_gen(USTR("__string"), false)) rt->toStringFun = c->code;
        if(hash == hl_hash_gen(USTR("__compare"), false)) rt->compareFun = c->code;
        if(hash == hl_hash_gen(USTR("__cast"), false)) rt->castFun = c->code;
        if(hash == hl_hash_gen(USTR("__get_field"), false)) rt->getFieldFun = c->code;
    }
    PyObject *handle = hlmod_ptr_new(&p->type, &type_handle_type);
    if(!handle) goto fail;
    publish_python_type(p);
    return handle;
fail:
    free_python_type(p);
    return NULL;
}

PyObject *hlmod_py_alloc_obj(PyObject *self, PyObject *args) {
    PyObject *arg;
    if(!PyArg_ParseTuple(args, "O:alloc_obj", &arg)) return NULL;
    hl_type *type = type_argument(arg);
    if(!type) return NULL;
    if(type->kind != HOBJ) {
        PyErr_SetString(PyExc_TypeError, "Allocation requires an HL object class");
        return NULL;
    }
    void *ptr = hl_alloc_obj(type);
    return hlmod_ptr_new(ptr, type);
}

PyObject *hlmod_py_bind_instance(PyObject *self, PyObject *args) {
    PyObject *arg, *instance;
    if(!PyArg_ParseTuple(args, "OO:bind_instance", &arg, &instance)) return NULL;
    if(!PyObject_TypeCheck(arg, &HlPtrType)) {
        PyErr_SetString(PyExc_TypeError, "Expected an HL object handle");
        return NULL;
    }
    HlPtr *ptr = (HlPtr*)arg;
    PythonType *type = ptr->type ? find_type(ptr->type) : NULL;
    if(!type || !ptr->ptr || !PyObject_TypeCheck(instance, (PyTypeObject*)type->python)) {
        PyErr_SetString(PyExc_TypeError, "Instance does not match its registered native subtype");
        return NULL;
    }
    PythonPeer *peer;
    HASH_FIND_PTR(python_peers, &ptr->ptr, peer);
    if(peer) {
        PyErr_SetString(PyExc_ValueError, "Native instance is already bound");
        return NULL;
    }
    peer = calloc(1, sizeof(*peer));
    if(!peer) return PyErr_NoMemory();
    peer->weak = PyWeakref_NewRef(instance, NULL);
    if(!peer->weak) { free(peer); return NULL; }
    peer->ptr = ptr->ptr;
    peer->strong = Py_NewRef(instance);
    PeerFinalizer *finalizer = hl_gc_alloc_finalizer(sizeof(*finalizer));
    finalizer->finalize = peer_finalize;
    finalizer->peer = peer;
    *(void**)((char*)ptr->ptr + type->lifetime_offset) = finalizer;
    HASH_ADD_PTR(python_peers, ptr, peer);
    Py_RETURN_NONE;
}

void *hlmod_python_callback(PyObject *callable, hl_type *signature) {
    Adapter *a;
    if(!PyCallable_Check(callable)) {
        PyErr_SetString(PyExc_TypeError, "Expected a Python callable");
        return NULL;
    }
    if(check_signature(signature) < 0 || hlmod_python_init() < 0) return NULL;
    for(a = adapters; a && a->signature != signature; a = a->next) {}
    if(!a) {
        a = calloc(1, sizeof(*a));
        if(!a) { PyErr_NoMemory(); return NULL; }
        a->signature = signature;
        a->full_type.kind = HFUN;
        a->full_type.fun = &a->full_fun;
        a->full_fun.nargs = signature->fun->nargs + 1;
        a->full_fun.ret = signature->fun->ret;
        a->full_fun.args = malloc(sizeof(hl_type*) * a->full_fun.nargs);
        if(!a->full_fun.args) { free(a); PyErr_NoMemory(); return NULL; }
        a->full_fun.args[0] = &hlt_bytes;
        memcpy(a->full_fun.args + 1, signature->fun->args, sizeof(hl_type*) * signature->fun->nargs);
        a->code = hl_jit_python_adapter(signature, NULL, true, &a->codesize);
        if(!a->code) { free(a->full_fun.args); free(a); PyErr_NoMemory(); return NULL; }
        a->next = adapters;
        adapters = a;
    }
    PythonPeer *peer = calloc(1, sizeof(*peer));
    if(!peer) { PyErr_NoMemory(); return NULL; }
    CallbackOwner *owner = (CallbackOwner*)CallbackOwnerType.tp_alloc(&CallbackOwnerType, 0);
    if(!owner) { free(peer); return NULL; }
    owner->callable = Py_NewRef(callable);
    peer->strong = (PyObject*)owner;
    peer->weak = PyWeakref_NewRef((PyObject*)owner, NULL);
    if(!peer->weak) { Py_DECREF(owner); free(peer); return NULL; }
    NativeCall *call = hl_gc_alloc_finalizer(sizeof(*call));
    memset(call, 0, sizeof(*call));
    call->finalize = callback_finalize;
    call->signature = signature;
    call->peer = peer;
    peer->callback = call;
    vclosure *closure = hl_alloc_closure_ptr(&a->full_type, a->code, call);
    peer->ptr = closure;
    HASH_ADD_PTR(python_peers, ptr, peer);
    return closure;
}

PyObject *hlmod_py_make_callback(PyObject *self, PyObject *args) {
    PyObject *callable, *arg;
    if(!PyArg_ParseTuple(args, "OO:make_callback", &callable, &arg)) return NULL;
    hl_type *signature = type_argument(arg);
    if(!signature) return NULL;
    vclosure *closure = hlmod_python_callback(callable, signature);
    return closure ? hlmod_ptr_new(closure, closure->t) : NULL;
}

vdynamic *hlmod_python_alloc_obj(hl_type *type) {
    if(constructor_instance && constructor_type == type) {
        vdynamic *instance = constructor_instance;
        constructor_instance = NULL;
        return instance;
    }
    return hl_alloc_obj(type);
}

PyObject *hlmod_py_init_obj(PyObject *self, PyObject *args) {
    PyObject *handle, *arguments;
    int findex;
    if(!PyArg_ParseTuple(args, "OiO!:init_obj", &handle, &findex, &PyTuple_Type, &arguments)) return NULL;
    if(!PyObject_TypeCheck(handle, &HlPtrType)) {
        PyErr_SetString(PyExc_TypeError, "Expected a typed HL object handle");
        return NULL;
    }
    HlPtr *ptr = (HlPtr*)handle;
    if(!ptr->ptr || !ptr->type || ptr->type->kind != HOBJ || findex < 0 ||
       findex >= g_code->nfunctions + g_code->nnatives) {
        PyErr_SetString(PyExc_TypeError, "Invalid constructor target");
        return NULL;
    }
    int index = g_module->functions_indexes[findex];
    if(index < 0 || index >= g_code->nfunctions) {
        PyErr_SetString(PyExc_TypeError, "Constructor must be an HL allocation wrapper");
        return NULL;
    }
    hl_function *f = g_code->functions + index;
    hl_type_fun *signature = f->type->fun;
    if(signature->ret->kind == HVOID && signature->nargs > 0) {
        hl_type *ancestor;
        for(ancestor = ptr->type; ancestor; ancestor = ancestor->obj->super)
            if(ancestor == signature->args[0]) break;
        if(ancestor) {
            Py_ssize_t count = PyTuple_GET_SIZE(arguments);
            PyObject *full_args = PyTuple_New(count + 1);
            if(!full_args) return NULL;
            PyTuple_SET_ITEM(full_args, 0, Py_NewRef(handle));
            for(Py_ssize_t n = 0; n < count; n++)
                PyTuple_SET_ITEM(full_args, n + 1, Py_NewRef(PyTuple_GET_ITEM(arguments, n)));
            PyObject *call_args = Py_BuildValue("(iO)", findex, full_args);
            Py_DECREF(full_args);
            if(!call_args) return NULL;
            PyObject *result = hlmod_py_call(NULL, call_args);
            Py_DECREF(call_args);
            if(!result) return NULL;
            Py_DECREF(result);
            Py_RETURN_NONE;
        }
    }
    hl_type *allocation = NULL;
    int i;
    for(i = 0; i < f->nops; i++) {
        if(f->ops[i].op != ONew) continue;
        hl_type *candidate = f->regs[f->ops[i].p1];
        hl_type *ancestor;
        for(ancestor = ptr->type; ancestor; ancestor = ancestor->obj->super)
            if(ancestor == candidate) break;
        if(!ancestor) continue;
        if(allocation) {
            PyErr_SetString(PyExc_TypeError, "Ambiguous constructor allocation wrapper");
            return NULL;
        }
        allocation = candidate;
    }
    if(!allocation) {
        PyErr_SetString(PyExc_TypeError, "Constructor wrapper has no matching object allocation");
        return NULL;
    }
    PyObject *call_args = Py_BuildValue("(iO)", findex, arguments);
    if(!call_args) return NULL;
    vdynamic *previous_instance = constructor_instance;
    hl_type *previous_type = constructor_type;
    constructor_instance = ptr->ptr;
    constructor_type = allocation;
    PyObject *result = hlmod_py_call(NULL, call_args);
    bool consumed = constructor_instance == NULL;
    constructor_instance = previous_instance;
    constructor_type = previous_type;
    Py_DECREF(call_args);
    if(!result) return NULL;
    Py_DECREF(result);
    if(!consumed) {
        PyErr_SetString(PyExc_RuntimeError, "Constructor did not initialize the supplied instance");
        return NULL;
    }
    Py_RETURN_NONE;
}

char *hlmod_python_take_error(void) {
    PyObject *exc_type, *exc_value, *traceback;
    PyErr_Fetch(&exc_type, &exc_value, &traceback);
    PyErr_NormalizeException(&exc_type, &exc_value, &traceback);
    char *error = NULL;
    if(exc_value) {
        /* Modders need the Python location, not just the exception message. */
        if(traceback) PyException_SetTraceback(exc_value, traceback);
        PyObject *module = PyImport_ImportModule("traceback");
        PyObject *lines = module ? PyObject_CallMethod(module, "format_exception", "O", exc_value) : NULL;
        PyObject *separator = lines ? PyUnicode_FromString("") : NULL;
        PyObject *text = separator ? PyUnicode_Join(separator, lines) : NULL;
        const char *message = text ? PyUnicode_AsUTF8(text) : NULL;
        if(message) error = strdup(message);
        Py_XDECREF(text);
        Py_XDECREF(separator);
        Py_XDECREF(lines);
        Py_XDECREF(module);
        if(!error) {
            PyErr_Clear();
            PyObject *fallback = PyObject_Repr(exc_value);
            const char *repr = fallback ? PyUnicode_AsUTF8(fallback) : NULL;
            if(repr) error = strdup(repr);
            Py_XDECREF(fallback);
        }
    }
    Py_XDECREF(exc_type);
    Py_XDECREF(exc_value);
    Py_XDECREF(traceback);
    PyErr_Clear();
    return error ? error : strdup("Python callback failed without an exception");
}

void hlmod_python_throw_error(char *error) {
    if(!error) hl_error("Python callback failed (unable to format exception)");
    vdynamic *exception = hl_alloc_strbytes(USTR("Python callback: %s"), hl_to_utf16(error));
    free(error);
    hl_throw(exception);
}

/* No Python references, GIL, or HL blocking scope may cross an HL longjmp. */
void hlmod_python_invoke(void *context, void **slots, vdynamic *result) {
    NativeCall *call = context;
    hl_type *signature = call->signature;
    hl_blocking(true);
    PyGILState_STATE gil = PyGILState_Ensure();
    hl_blocking(false);
    PyObject *args = PyTuple_New(signature->fun->nargs);
    PyObject *value = NULL;
    PyObject *owner = NULL;
    PyObject *callable = call->callable;
    if(call->peer) {
        owner = peer_reference(call->peer);
        if(owner) {
            callable = ((CallbackOwner*)owner)->callable;
        }
    }
    char *error = NULL;
    bool failed = false;
    int i;
    if(args) {
        for(i = 0; i < signature->fun->nargs; i++) {
            PyObject *arg = hlmod_cast_to_py(signature->fun->args[i], slots[i]);
            if(!arg) break;
            PyTuple_SET_ITEM(args, i, arg);
        }
        if(i == signature->fun->nargs) {
            if(callable) value = PyObject_CallObject(callable, args);
            else PyErr_SetString(PyExc_RuntimeError, "Python callback has been collected");
        }
    }
    if(value) {
        memset(result, 0, sizeof(*result));
        result->t = signature->fun->ret;
        if(result->t->kind != HVOID) {
            void *slot = hlmod_cast_to_hl(value, result->t);
            if(slot) memcpy(&result->v, slot, hl_type_size(result->t));
        }
    }
    if(!value || PyErr_Occurred()) {
        failed = true;
        error = hlmod_python_take_error();
    }
    Py_XDECREF(value);
    Py_XDECREF(args);
    Py_XDECREF(owner);
    hl_blocking(true);
    PyGILState_Release(gil);
    hl_blocking(false);
    if(failed) hlmod_python_throw_error(error);
}

int hlmod_python_dispatch(int findex, int nargs, void **args) {
    if(hlmod_python_bypass == findex) {
        hlmod_python_bypass = -1;
        return 0;
    }
    /* Type metadata is immutable after atomic publication and is reclaimed only
       after native callers stop. Never inspect the GIL-owned peer hash here. */
    if(nargs == 0 || !published_python_types()) return 0;
    hl_type *signature = g_module->ctx.functions_types[findex];
    if(signature->fun->args[0]->kind != HOBJ) return 0;
    void *ptr = *(void**)args[0];
    if(!ptr) return 0;
    hl_type *t = ((vobj*)ptr)->t;
    if(t->kind != HOBJ || t->obj->m == &g_module->ctx) return 0;
    for(; t && t->kind == HOBJ; t = t->obj->super) {
        PythonType *p = find_type(t);
        if(!p) continue;
        NativeCall *call;
        for(call = p->methods; call; call = call->next) {
            if(call->findex == findex && call->signature->fun->nargs == nargs) {
                vdynamic result = {0};
                hlmod_python_invoke(call, args, &result);
                g_return_value_int = result.v.i64;
                g_return_value_double = result.t->kind == HF32 ? result.v.f : result.v.d;
                return 1;
            }
        }
    }
    return 0;
}

void hlmod_python_shutdown(void) {
    hl_gc_set_foreign_hooks(NULL, NULL, NULL);
    initialized = false;
    if(gc_callback) {
        PyObject *gc = PyImport_ImportModule("gc");
        PyObject *callbacks = gc ? PyObject_GetAttrString(gc, "callbacks") : NULL;
        if(callbacks) {
            Py_ssize_t i = PySequence_Index(callbacks, gc_callback);
            if(i >= 0) PySequence_DelItem(callbacks, i);
            Py_DECREF(callbacks);
        }
        Py_XDECREF(gc);
        Py_CLEAR(gc_callback);
        PyErr_Clear();
    }
    PythonPeer *p, *tmp;
    HASH_ITER(hh, python_peers, p, tmp) {
        HASH_DEL(python_peers, p);
        if(!p->dead) {
            if(p->callback) p->callback->peer = NULL;
            else {
                PythonType *type = find_type(((vobj*)p->ptr)->t);
                PeerFinalizer *f = *(PeerFinalizer**)((char*)p->ptr + type->lifetime_offset);
                if(f) f->peer = NULL;
            }
        }
        Py_XDECREF(p->strong);
        Py_XDECREF(p->weak);
        free(p);
    }
    PythonType *type;
    for(type = python_types; type; type = type->next) {
        NativeCall *call;
        for(call = type->methods; call; call = call->next) Py_CLEAR(call->callable);
        Py_CLEAR(type->python);
    }
}

/* Called after Python finalization, before releasing the HL module. */
void hlmod_python_dispose(void) {
    while(python_types) {
        PythonType *next = python_types->next;
        free_python_type(python_types);
        python_types = next;
    }
    while(adapters) {
        Adapter *next = adapters->next;
        hl_free_executable_memory(adapters->code, adapters->codesize);
        free(adapters->full_fun.args);
        free(adapters);
        adapters = next;
    }
    PythonRoot *r, *tmp;
    HASH_ITER(hh, python_roots, r, tmp) {
        HASH_DEL(python_roots, r);
        free(r);
    }
}
