#include <hlmod_codegen.h>
#include <hlmod.h>
#include <hlmod_embedded.h>
#include <stdint.h>
#include <stdio.h>

/* Native metadata only. Python owns naming, annotations, docs and source output. */
typedef struct {
    hl_code *code;
    hl_type **extra;
    int count;
    int capacity;
    bool failed;
} metadata_context;

/* Both helpers consume the value, including on failure. */
static void set_item(metadata_context *ctx, PyObject *dict, const char *key, PyObject *value) {
    if (!value || !dict) ctx->failed = true;
    if (!ctx->failed && PyDict_SetItemString(dict, key, value) < 0) ctx->failed = true;
    Py_XDECREF(value);
}

static void append_item(metadata_context *ctx, PyObject *list, PyObject *value) {
    if (!value || !list) ctx->failed = true;
    if (!ctx->failed && PyList_Append(list, value) < 0) ctx->failed = true;
    Py_XDECREF(value);
}

static int type_index(metadata_context *ctx, hl_type *type) {
    if (!type) return -1;
    uintptr_t address = (uintptr_t)type;
    uintptr_t start = (uintptr_t)ctx->code->types;
    uintptr_t end = start + sizeof(hl_type) * ctx->code->ntypes;
    if (address >= start && address < end && (address - start) % sizeof(hl_type) == 0)
        return (int)((address - start) / sizeof(hl_type));
    for (int i = 0; i < ctx->count; i++)
        if (ctx->extra[i] == type) return ctx->code->ntypes + i;
    if (ctx->count == ctx->capacity) {
        int capacity = ctx->capacity ? ctx->capacity * 2 : 16;
        hl_type **extra = PyMem_Realloc(ctx->extra, capacity * sizeof(*extra));
        if (!extra) {
            ctx->failed = true;
            PyErr_NoMemory();
            return -1;
        }
        ctx->extra = extra;
        ctx->capacity = capacity;
    }
    ctx->extra[ctx->count] = type;
    return ctx->code->ntypes + ctx->count++;
}

static void set_name(metadata_context *ctx, PyObject *node, const char *key, const uchar *name) {
    set_item(ctx, node, key, PyUnicode_FromString(name ? (const char *)hl_to_utf8(name) : ""));
}

static PyObject *field_metadata(metadata_context *ctx, hl_obj_field *field) {
    PyObject *node = Py_BuildValue("{s:i}", "type", type_index(ctx, field->t));
    set_name(ctx, node, "name", field->name);
    return node;
}

static PyObject *type_metadata(metadata_context *ctx, hl_type *type, int index) {
    PyObject *node = Py_BuildValue("{s:i,s:i}", "index", index, "kind", type->kind);
    if (!node) { ctx->failed = true; return NULL; }
    if (type->kind == HOBJ || type->kind == HSTRUCT) {
        hl_type_obj *obj = type->obj;
        set_name(ctx, node, "name", obj->name);
        set_item(ctx, node, "super", PyLong_FromLong(type_index(ctx, obj->super)));
        PyObject *fields = PyList_New(0);
        for (int j = 0; j < obj->nfields && !ctx->failed; j++)
            append_item(ctx, fields, field_metadata(ctx, &obj->fields[j]));
        set_item(ctx, node, "fields", fields);
        PyObject *methods = PyList_New(0);
        for (int j = 0; j < obj->nproto && !ctx->failed; j++) {
            hl_obj_proto *proto = &obj->proto[j];
            PyObject *method = Py_BuildValue("{s:i,s:i}", "findex", proto->findex, "pindex", proto->pindex);
            set_name(ctx, method, "name", proto->name);
            append_item(ctx, methods, method);
        }
        set_item(ctx, node, "methods", methods);
        PyObject *bindings = PyList_New(0);
        for (int j = 0; j < obj->nbindings && !ctx->failed; j++)
            append_item(ctx, bindings, Py_BuildValue("{s:i,s:i}",
                "field", obj->bindings[j * 2], "findex", obj->bindings[j * 2 + 1]));
        set_item(ctx, node, "bindings", bindings);
    } else if (type->kind == HVIRTUAL) {
        PyObject *fields = PyList_New(0);
        for (int j = 0; j < type->virt->nfields && !ctx->failed; j++)
            append_item(ctx, fields, field_metadata(ctx, &type->virt->fields[j]));
        set_item(ctx, node, "fields", fields);
    } else if (type->kind == HFUN || type->kind == HMETHOD) {
        PyObject *args = PyList_New(0);
        for (int j = 0; j < type->fun->nargs && !ctx->failed; j++)
            append_item(ctx, args, PyLong_FromLong(type_index(ctx, type->fun->args[j])));
        set_item(ctx, node, "args", args);
        set_item(ctx, node, "return", PyLong_FromLong(type_index(ctx, type->fun->ret)));
    } else if (type->kind == HREF || type->kind == HNULL || type->kind == HPACKED) {
        set_item(ctx, node, "param", PyLong_FromLong(type_index(ctx, type->tparam)));
    } else if (type->kind == HABSTRACT) {
        set_name(ctx, node, "name", type->abs_name);
    } else if (type->kind == HENUM) {
        set_name(ctx, node, "name", type->tenum->name);
    }
    return node;
}

static PyObject *constructor_metadata(metadata_context *ctx) {
    PyObject *constructors = PyList_New(0);
    for (int i = 0; i < ctx->code->ntypes && !ctx->failed; i++) {
        hl_type *type = &ctx->code->types[i];
        if (type->kind != HOBJ) continue;
        for (int j = 0; j < type->obj->nbindings && !ctx->failed; j++) {
            hl_obj_field *field = hl_obj_field_fetch(type, type->obj->bindings[j * 2]);
            if (ucmp(field->name, USTR("__constructor__")) != 0) continue;
            append_item(ctx, constructors, Py_BuildValue("{s:i,s:i}",
                "static_type", i, "findex", type->obj->bindings[j * 2 + 1]));
        }
    }
    return constructors;
}

/* Best-effort source location for a function's first opcode, when the bytecode
 * was compiled with -debug. Absent on release builds; callers must tolerate
 * the metadata keys being omitted. */
static void add_debug_location(metadata_context *ctx, PyObject *node, hl_function *func) {
    if (!ctx->code->hasdebug || !func->debug || func->nops <= 0) return;
    int file = func->debug[0];
    int line = func->debug[1];
    if (file < 0 || file >= ctx->code->ndebugfiles) return;
    set_item(ctx, node, "file", PyUnicode_FromStringAndSize(
        ctx->code->debugfiles[file], ctx->code->debugfiles_lens[file]));
    set_item(ctx, node, "line", PyLong_FromLong(line));
}

static PyObject *extract_metadata(hl_code *code) {
    metadata_context ctx = {.code = code};
    PyObject *root = Py_BuildValue("{s:s,s:i}",
        "code_hash", g_code_sha256, "native_type_count", code->ntypes);
    if (!root) return NULL;
    PyObject *functions = PyList_New(0);
    for (int i = 0; i < code->nfunctions && !ctx.failed; i++) {
        hl_function *func = &code->functions[i];
        PyObject *node = Py_BuildValue("{s:i,s:i}",
            "findex", func->findex, "type", type_index(&ctx, func->type));
        hl_type_obj *obj = fun_obj(func);
        set_name(&ctx, node, "owner", obj ? obj->name : NULL);
        set_name(&ctx, node, "name", fun_field_name(func));
        PyObject *names = PyList_New(0);
        for (int j = 0; func->assigns && j < func->nassigns && !ctx.failed; j++) {
            if (func->assigns[j].op_index == 0)
                append_item(&ctx, names, PyUnicode_FromString((const char *)hl_to_utf8(
                    hl_get_ustring(code, func->assigns[j].str_index))));
        }
        add_debug_location(&ctx, node, func);
        set_item(&ctx, node, "arg_names", names);
        append_item(&ctx, functions, node);
    }
    for (int i = 0; i < code->nnatives && !ctx.failed; i++) {
        hl_native *native = &code->natives[i];
        append_item(&ctx, functions, Py_BuildValue("{s:i,s:i}",
            "findex", native->findex, "type", type_index(&ctx, native->t)));
    }
    set_item(&ctx, root, "functions", functions);
    set_item(&ctx, root, "constructors", constructor_metadata(&ctx));
    PyObject *types = PyList_New(0);
    for (int i = 0; i < code->ntypes + ctx.count && !ctx.failed; i++) {
        hl_type *type = i < code->ntypes ? &code->types[i] : ctx.extra[i - code->ntypes];
        append_item(&ctx, types, type_metadata(&ctx, type, i));
    }
    set_item(&ctx, root, "types", types);
    PyMem_Free(ctx.extra);
    if (ctx.failed) { Py_DECREF(root); return NULL; }
    return root;
}

bool hlmod_generate_stubs(hl_code *code) {
    PyObject *metadata = extract_metadata(code);
    if (!metadata) goto error;
    PyObject *compiled = Py_CompileString(hlmod_stub_renderer_source,
        "<hlmod>/stub_renderer.py", Py_file_input);
    PyObject *module = compiled ? PyImport_ExecCodeModule("_hlmod_codegen", compiled) : NULL;
    Py_XDECREF(compiled);
#ifndef SOURCE_FILE_SHA256_HASH
#define SOURCE_FILE_SHA256_HASH ""
#endif
    PyObject *result = module ? PyObject_CallMethod(module, "generate", "Oss",
        metadata, "./mods/stubs", SOURCE_FILE_SHA256_HASH) : NULL;
    Py_XDECREF(module);
    Py_DECREF(metadata);
    if (!result) goto error;
    Py_DECREF(result);
    return true;
error:
    fprintf(stderr, "[hlmod] Python proxy generation failed.\n");
    PyErr_Print();
    return false;
}
