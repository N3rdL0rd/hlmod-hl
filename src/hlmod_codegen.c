#include "hlmod_codegen.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <hl.h>

/*=================================================================================================
 *
 *                             STUB GENERATOR IMPLEMENTATION
 *
 *=================================================================================================*/


#ifdef HL_WIN
#	include <direct.h>
#	define MKDIR(path) _mkdir(path)
#else
#	include <sys/stat.h>
#	define MKDIR(path) mkdir(path, 0755)
#endif

typedef struct hl_type_set {
    hl_type** items;
    int count;
    int capacity;
} hl_type_set;

const uchar* python_type_str(hl_type *t);
static void mkdir_p(const char *path);
static hl_function* find_function_by_findex(hl_code* code, int findex);
static void to_python_safe_name(const uchar* u_name, char* buffer, size_t buffer_size);
static void print_method_stub_from_func(FILE *f, const char* name, hl_function *func, int findex, hl_code *code);
static void print_method_stub_from_type(FILE *f, const char* name, hl_type_fun* fun_type);
static void type_set_init(hl_type_set* set);
static void type_set_add(hl_type_set* set, hl_type* t);
static void type_set_free(hl_type_set* set);
static void collect_type_dependencies(hl_type* t, hl_type_set* deps);
static void get_python_path_for_type(hl_type* t, const char* base_dir, char* dir_path_out, char* class_name_out, size_t out_size);
static void print_absolute_import_for_type(FILE* f, hl_type* importer_type, hl_type* dependency_type, const char* base_dir, bool is_indented);
static void _python_type_str_rec(hl_type *t, uchar* buf, int* pos, int buf_size);

static void _str_append(uchar* buf, int* pos, int buf_size, const uchar* str) {
    if (!str) return;
    int len = ustrlen(str);
    if (*pos + len < buf_size) {
        memcpy(buf + *pos, str, len * sizeof(uchar));
        *pos += len;
    }
}

static void _python_type_str_rec(hl_type *t, uchar* buf, int* pos, int buf_size) {
    if (t == NULL) { _str_append(buf, pos, buf_size, USTR("Any")); return; }
    switch(t->kind) {
        case HVOID: _str_append(buf, pos, buf_size, USTR("None")); break;
        case HUI8: case HUI16: case HI32: case HI64: _str_append(buf, pos, buf_size, USTR("int")); break;
        case HF32: case HF64: _str_append(buf, pos, buf_size, USTR("float")); break;
        case HBOOL: _str_append(buf, pos, buf_size, USTR("bool")); break;
        case HBYTES: _str_append(buf, pos, buf_size, USTR("bytes")); break;
        case HDYN: _str_append(buf, pos, buf_size, USTR("Any")); break;
        case HTYPE: _str_append(buf, pos, buf_size, USTR("type")); break;
        case HREF: _python_type_str_rec(t->tparam, buf, pos, buf_size); break;
        case HDYNOBJ: _str_append(buf, pos, buf_size, USTR("dict[str, Any]")); break;
        case HARRAY:
            _str_append(buf, pos, buf_size, USTR("list["));
            _python_type_str_rec(t->tparam, buf, pos, buf_size);
            _str_append(buf, pos, buf_size, USTR("]"));
            break;
        case HNULL:
            _str_append(buf, pos, buf_size, USTR("Optional["));
            _python_type_str_rec(t->tparam, buf, pos, buf_size);
            _str_append(buf, pos, buf_size, USTR("]"));
            break;
        case HABSTRACT:
            if (t->abs_name && ucmp(t->abs_name, USTR("String")) == 0) {
                 _str_append(buf, pos, buf_size, USTR("str"));
            } else {
                 _str_append(buf, pos, buf_size, t->abs_name ? t->abs_name : USTR("Any"));
            }
            break;
        case HOBJ: case HSTRUCT: case HENUM:
            _str_append(buf, pos, buf_size, USTR("\""));
            if (t->obj && t->obj->name) {
                char safe_name_utf8[1024];
                to_python_safe_name(t->obj->name, safe_name_utf8, sizeof(safe_name_utf8));
                char* last_dot = strrchr(safe_name_utf8, '.');
                const char* name_to_append = last_dot ? last_dot + 1 : safe_name_utf8;
                uchar u_name_to_append[512];
                hl_from_utf8(u_name_to_append, sizeof(u_name_to_append)/sizeof(uchar), name_to_append);
                _str_append(buf, pos, buf_size, u_name_to_append);
            } else { _str_append(buf, pos, buf_size, USTR("Any")); }
            _str_append(buf, pos, buf_size, USTR("\""));
            break;
        case HFUN: case HMETHOD:
            _str_append(buf, pos, buf_size, USTR("Callable[["));
            for(int i = 0; i < t->fun->nargs; i++) {
                if(i > 0) _str_append(buf, pos, buf_size, USTR(", "));
                _python_type_str_rec(t->fun->args[i], buf, pos, buf_size);
            }
            _str_append(buf, pos, buf_size, USTR("], "));
            _python_type_str_rec(t->fun->ret, buf, pos, buf_size);
            _str_append(buf, pos, buf_size, USTR("]"));
            break;
        default: _str_append(buf, pos, buf_size, USTR("Any")); break;
    }
}

const uchar* python_type_str(hl_type *t) {
    static uchar buffer[2048];
    int pos = 0;
    memset(buffer, 0, sizeof(buffer));
    _python_type_str_rec(t, buffer, &pos, 2048);
    return buffer;
}

static hl_function* find_function_by_findex(hl_code* code, int findex) {
    for (int i = 0; i < code->nfunctions; i++) {
        if (code->functions[i].findex == findex) return &code->functions[i];
    }
    return NULL;
}

static void get_argument_names(hl_function *func, hl_code *code, const uchar **names_out, int max_names) {
    if (!func->assigns) return;
    int arg_idx = 0;
    // The assigns with op_index 0 are the argument names, in order, *excluding this*.
    for (int i = 0; i < func->nassigns && arg_idx < max_names; i++) {
        if (func->assigns[i].op_index == 0) {
            names_out[arg_idx++] = hl_get_ustring(code, func->assigns[i].str_index);
        }
    }
}

static void print_method_stub_from_func(FILE *f, const char* name, hl_function *func, int findex, hl_code *code) {
    if (!func || !func->type || (func->type->kind != HFUN && func->type->kind != HMETHOD)) {
        fprintf(f, "    def %s(self, *args, **kwargs) -> Any: ... # findex: %d\n", name, findex);
        return;
    }

    int nargs = func->type->fun->nargs;
    int real_nargs = nargs > 0 ? nargs - 1 : 0;
    const uchar* arg_names[real_nargs];
    for(int i = 0; i < real_nargs; i++) arg_names[i] = NULL;
    get_argument_names(func, code, arg_names, real_nargs);

    fprintf(f, "    def %s(self", name);
    for (int k = 1; k < nargs; k++) { // Start at 1 to skip 'this'
        const char* arg_name_utf8;
        char fallback_name[16];
        
        // ** FIX HERE **: Use k-1 to index the names array, as it does not include 'this'.
        if (arg_names[k-1]) {
            arg_name_utf8 = (char*)hl_to_utf8(arg_names[k-1]);
        } else {
            snprintf(fallback_name, sizeof(fallback_name), "arg%d", k - 1);
            arg_name_utf8 = fallback_name;
        }
        fprintf(f, ", %s: %s", arg_name_utf8, (char*)hl_to_utf8(python_type_str(func->type->fun->args[k])));
    }
    fprintf(f, ") -> %s: ... # findex: %d\n", (char*)hl_to_utf8(python_type_str(func->type->fun->ret)), findex);
}

static void print_method_stub_from_type(FILE *f, const char* name, hl_type_fun* fun_type) {
    fprintf(f, "    def %s(self", name);
    for (int k = 1; k < fun_type->nargs; k++) { // Start at 1 to skip 'this'
        fprintf(f, ", arg%d: %s", k - 1, (char*)hl_to_utf8(python_type_str(fun_type->args[k])));
    }
    fprintf(f, ") -> %s: ...\n", (char*)hl_to_utf8(python_type_str(fun_type->ret)));
}

static void mkdir_p(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len > 0 && (tmp[len - 1] == '/' || tmp[len - 1] == '\\')) tmp[len - 1] = 0;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = 0;
            if (MKDIR(tmp) != 0 && errno != EEXIST) {
                 fprintf(stderr, "[hlmod] Error creating directory %s: %s\n", tmp, strerror(errno));
                 return;
            }
            *p = '/';
        }
    }
    if (MKDIR(tmp) != 0 && errno != EEXIST) {
        fprintf(stderr, "[hlmod] Error creating directory %s: %s\n", tmp, strerror(errno));
    }
}

static void to_python_safe_name(const uchar* u_name, char* buffer, size_t buffer_size) {
    if (!u_name || buffer_size == 0) {
        if (buffer_size > 0) buffer[0] = '\0';
        return;
    }
    char* utf8_name = (char*)hl_to_utf8(u_name);
    size_t out_pos = 0;
    for (size_t i = 0; utf8_name[i] != '\0' && out_pos < buffer_size - 1; ++i) {
        if (utf8_name[i] == '$') {
            if (out_pos < buffer_size - 2) {
                buffer[out_pos++] = 'S';
                buffer[out_pos++] = '_';
            }
        } else {
            buffer[out_pos++] = utf8_name[i];
        }
    }
    buffer[out_pos] = '\0';
}

static void type_set_init(hl_type_set* set) {
    set->items = NULL;
    set->count = 0;
    set->capacity = 0;
}

static void type_set_free(hl_type_set* set) {
    if (set->items) free(set->items);
    set->items = NULL;
    set->count = 0;
    set->capacity = 0;
}

static void type_set_add(hl_type_set* set, hl_type* t) {
    if (!t || (t->kind != HOBJ && t->kind != HSTRUCT && t->kind != HENUM)) return;
    for (int i = 0; i < set->count; i++) {
        if (set->items[i] == t) return;
    }
    if (set->count >= set->capacity) {
        set->capacity = set->capacity == 0 ? 16 : set->capacity * 2;
        set->items = realloc(set->items, set->capacity * sizeof(hl_type*));
        if (!set->items) { fprintf(stderr, "[hlmod] FATAL: realloc failed.\n"); exit(1); }
    }
    set->items[set->count++] = t;
}

static void collect_type_dependencies(hl_type* t, hl_type_set* deps) {
    if (!t) return;
    switch (t->kind) {
        case HOBJ: case HSTRUCT: case HENUM: type_set_add(deps, t); break;
        case HARRAY: case HNULL: case HREF: collect_type_dependencies(t->tparam, deps); break;
        case HFUN: case HMETHOD:
            if(t->fun) {
                for (int i = 0; i < t->fun->nargs; i++) collect_type_dependencies(t->fun->args[i], deps);
                collect_type_dependencies(t->fun->ret, deps);
            }
            break;
        default: break;
    }
}

static void get_python_path_for_type(hl_type* t, const char* base_dir, char* dir_path_out, char* class_name_out, size_t out_size) {
    char class_path_full_safe[1024];
    to_python_safe_name(t->obj->name, class_path_full_safe, sizeof(class_path_full_safe));
    char* last_dot = strrchr(class_path_full_safe, '.');
    if (last_dot) {
        strncpy(class_name_out, last_dot + 1, out_size - 1);
        class_name_out[out_size - 1] = '\0';
        char package_path[1024];
        size_t package_len = last_dot - class_path_full_safe;
        strncpy(package_path, class_path_full_safe, package_len);
        package_path[package_len] = '\0';
        for (char* p = package_path; *p; ++p) if (*p == '.') *p = '/';
        snprintf(dir_path_out, out_size, "%s/%s", base_dir, package_path);
    } else {
        strncpy(class_name_out, class_path_full_safe, out_size - 1);
        class_name_out[out_size - 1] = '\0';
        snprintf(dir_path_out, out_size, "%s", base_dir);
    }
}

static void print_absolute_import_for_type(FILE* f, hl_type* importer_type, hl_type* dependency_type, const char* base_dir, bool is_indented) {
    if (importer_type == dependency_type) {
        return;
    }

    char dep_module_dir[1024], dep_class_name[512];
    get_python_path_for_type(dependency_type, base_dir, dep_module_dir, dep_class_name, sizeof(dep_module_dir));

    const char *top_level_pkg = strrchr(base_dir, '/');
    if (top_level_pkg) {
        top_level_pkg++;
    } else {
        const char *alt_top_level_pkg = strrchr(base_dir, '\\');
        top_level_pkg = alt_top_level_pkg ? alt_top_level_pkg + 1 : base_dir;
    }

    const char* module_path_rel = dep_module_dir + strlen(base_dir);
    if (*module_path_rel == '/' || *module_path_rel == '\\') {
        module_path_rel++;
    }

    char full_module_path[2048] = {0};
    strcpy(full_module_path, top_level_pkg);

    if (strlen(module_path_rel) > 0) {
        strcat(full_module_path, ".");
        strcat(full_module_path, module_path_rel);
    }
    
    strcat(full_module_path, ".");
    strcat(full_module_path, dep_class_name);
    
    for (char* p = full_module_path; *p; ++p) {
        if (*p == '/' || *p == '\\') {
            *p = '.';
        }
    }
    
    const char* indent = is_indented ? "    " : "";
    fprintf(f, "%sfrom %s import %s\n", indent, full_module_path, dep_class_name);
}

void hlmod_generate_stubs(hl_code *code) {
    const char* base_dir = "./mods/stubs";
    clock_t start_time = clock();
    printf("[hlmod] Generating class stubs...\n");
    mkdir_p(base_dir);

    for (int i = 0; i < code->ntypes; i++) {
        hl_type *t = &code->types[i];
        if (t->kind != HOBJ && t->kind != HSTRUCT) continue;
        char dir_path[1024];
        get_python_path_for_type(t, base_dir, dir_path, (char[1]){0}, sizeof(dir_path));
        char pkg_init_path[1024];
        snprintf(pkg_init_path, sizeof(pkg_init_path), "%s/__init__.py", dir_path);
        remove(pkg_init_path);
    }
    char root_init_path[1024];
    snprintf(root_init_path, sizeof(root_init_path), "%s/__init__.py", base_dir);
    remove(root_init_path);

    for (int i = 0; i < code->ntypes; i++) {
        hl_type *t = &code->types[i];
        if (t->kind != HOBJ && t->kind != HSTRUCT) continue;

        char dir_path[1024], class_name_only[512];
        get_python_path_for_type(t, base_dir, dir_path, class_name_only, sizeof(class_name_only));
        
        char file_path[1024];
        snprintf(file_path, sizeof(file_path), "%s/%s.py", dir_path, class_name_only);

        mkdir_p(dir_path);
        char pkg_init_path[1024];
        snprintf(pkg_init_path, sizeof(pkg_init_path), "%s/__init__.py", dir_path);
        FILE *pkg_init_f = fopen(pkg_init_path, "a");
        if (pkg_init_f) {
            fprintf(pkg_init_f, "from .%s import %s\n", class_name_only, class_name_only);
            fclose(pkg_init_f);
        }

        FILE *f = fopen(file_path, "w");
        if (!f) {
            fprintf(stderr, "[hlmod] Error: Could not open for writing: %s\n", file_path);
            continue;
        }

        fprintf(f, "# This file is automatically generated by hlmod. Do not edit.\n");
        fprintf(f, "from __future__ import annotations\n");
        fprintf(f, "from typing import Any, Callable, Optional, List, TYPE_CHECKING\n");
        fprintf(f, "from hlobj import HlObject, hltype\n\n");

        if (t->obj->super) {
            print_absolute_import_for_type(f, t, t->obj->super, base_dir, false);
        }

        hl_type_set deps;
        type_set_init(&deps);
        for (int j = 0; j < t->obj->nfields; j++) collect_type_dependencies(t->obj->fields[j].t, &deps);
        for (int j = 0; j < t->obj->nproto; j++) {
            hl_function* func = find_function_by_findex(code, t->obj->proto[j].findex);
            if (func) collect_type_dependencies(func->type, &deps);
        }

        bool has_type_deps = false;
        for (int j = 0; j < deps.count; j++) {
            if (deps.items[j] != t->obj->super && deps.items[j] != t) {
                has_type_deps = true;
                break;
            }
        }
        
        if (has_type_deps) {
            fprintf(f, "if TYPE_CHECKING:\n");
            for (int j = 0; j < deps.count; j++) {
                if (deps.items[j] != t->obj->super) {
                    print_absolute_import_for_type(f, t, deps.items[j], base_dir, true);
                }
            }
        }
        type_set_free(&deps);
        fprintf(f, "\n");

        char parent_class_arg[512] = "HlObject";
        if (t->obj->super) {
            get_python_path_for_type(t->obj->super, base_dir, (char[1]){0}, parent_class_arg, sizeof(parent_class_arg));
        }
        fprintf(f, "@hltype(%i)\nclass %s(%s):\n", i, class_name_only, parent_class_arg);

        int* binding_map = NULL;
        if (t->obj->nfields > 0) {
            binding_map = calloc(t->obj->nfields, sizeof(int));
            for(int j = 0; j < t->obj->nfields; j++) binding_map[j] = -1;
            for (int j = 0; j < t->obj->nbindings; j++) {
                int ffield = t->obj->bindings[j * 2 + 1];
                int findex = t->obj->bindings[j * 2];
                if (ffield >= 0 && ffield < t->obj->nfields) binding_map[ffield] = findex;
            }
        }

        bool has_content = false;
        if (t->obj->nfields > 0) {
            fprintf(f, "\n    # --- Fields ---\n");
            for (int j = 0; j < t->obj->nfields; j++) {
                hl_obj_field *field = &t->obj->fields[j];
                char safe_field_name[512];
                to_python_safe_name(field->name, safe_field_name, sizeof(safe_field_name));
                if (binding_map && binding_map[j] != -1) {
                    hl_function* func = find_function_by_findex(code, binding_map[j]);
                    if (func) print_method_stub_from_func(f, safe_field_name, func, binding_map[j], code);
                } else if (field->t->kind == HFUN || field->t->kind == HMETHOD) {
                    print_method_stub_from_type(f, safe_field_name, field->t->fun);
                } else {
                    fprintf(f, "    %s: %s\n", safe_field_name, (char*)hl_to_utf8(python_type_str(field->t)));
                }
                has_content = true;
            }
        }
        if (binding_map) free(binding_map);

        if (t->obj->nproto > 0) {
            fprintf(f, "\n    # --- Methods ---\n");
            for (int j = 0; j < t->obj->nproto; j++) {
                hl_obj_proto *proto = &t->obj->proto[j];
                char safe_proto_name[512];
                to_python_safe_name(proto->name, safe_proto_name, sizeof(safe_proto_name));
                hl_function* func = find_function_by_findex(code, proto->findex);
                if (func) print_method_stub_from_func(f, safe_proto_name, func, proto->findex, code);
                has_content = true;
            }
        }

        if (!has_content) fprintf(f, "    pass\n");
        fclose(f);
    }

    clock_t end_time = clock();
    double elapsed_ms = ((double)(end_time - start_time) / CLOCKS_PER_SEC) * 1000.0;
    printf("[hlmod] Finished generating class stubs in %.2fms.\n", elapsed_ms);
}