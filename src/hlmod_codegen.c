#include "hlmod_codegen.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#ifdef HL_WIN
#	include <direct.h>
#	define MKDIR(path) _mkdir(path)
#else
#	include <sys/stat.h>
#	define MKDIR(path) mkdir(path, 0755)
#endif

// --- Forward declaration for recursive type conversion ---
const uchar* python_type_str(hl_type *t);

/**
 * @brief Appends a uchar string to a buffer, managing position and bounds.
 */
static void _str_append(uchar* buf, int* pos, int buf_size, const uchar* str) {
    if (!str) return;
    int len = ustrlen(str);
    if (*pos + len < buf_size) {
        memcpy(buf + *pos, str, len * sizeof(uchar));
        *pos += len;
    }
}

/**
 * @brief Recursively builds a Python type string from an hl_type.
 */
static void _python_type_str_rec(hl_type *t, uchar* buf, int* pos, int buf_size) {
    if (t == NULL) {
        _str_append(buf, pos, buf_size, USTR("Any"));
        return;
    }

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
            _str_append(buf, pos, buf_size, USTR("List["));
            _python_type_str_rec(t->tparam, buf, pos, buf_size);
            _str_append(buf, pos, buf_size, USTR("]"));
            break;
        case HNULL:
            _str_append(buf, pos, buf_size, USTR("Optional["));
            _python_type_str_rec(t->tparam, buf, pos, buf_size);
            _str_append(buf, pos, buf_size, USTR("]"));
            break;
        case HABSTRACT:
            if (ucmp(t->abs_name, USTR("String")) == 0) {
                 _str_append(buf, pos, buf_size, USTR("str"));
            } else {
                 _str_append(buf, pos, buf_size, t->abs_name);
            }
            break;
        case HOBJ: case HSTRUCT: case HENUM:
            _str_append(buf, pos, buf_size, USTR("\""));
            _str_append(buf, pos, buf_size, t->obj->name);
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
        default:
            _str_append(buf, pos, buf_size, USTR("Any")); // Fallback
            break;
    }
}

/**
 * @brief Converts an hl_type to its Python equivalent as a string.
 *        The returned pointer is to a static buffer and should be used immediately.
 */
const uchar* python_type_str(hl_type *t) {
    static uchar buffer[2048];
    int pos = 0;
    _python_type_str_rec(t, buffer, &pos, 2048);
    buffer[pos] = 0;
    return buffer;
}


/**
 * @brief Finds a function definition in the bytecode by its unique function index (findex).
 */
static hl_function* find_function_by_findex(hl_code* code, int findex) {
    for (int i = 0; i < code->nfunctions; i++) {
        if (code->functions[i].findex == findex) {
            return &code->functions[i];
        }
    }
    return NULL;
}



/**
 * @brief Recursively creates directories for a given path.
 * @param path The full directory path to create.
 */
static void mkdir_p(const char *path) {
    char tmp[1024];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);

    // Remove trailing slash if it exists
    if (len > 0 && (tmp[len - 1] == '/' || tmp[len - 1] == '\\')) {
        tmp[len - 1] = 0;
    }

    // Iterate through the path and create each directory component
    for (p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = 0;
            // Create the directory, ignore error if it already exists
            if (MKDIR(tmp) != 0 && errno != EEXIST) {
                 fprintf(stderr, "[hlmod] Error creating directory %s: %s\n", tmp, strerror(errno));
                 return;
            }
            *p = '/'; // Use a consistent separator
        }
    }
    // Create the final directory in the path
    if (MKDIR(tmp) != 0 && errno != EEXIST) {
        fprintf(stderr, "[hlmod] Error creating directory %s: %s\n", tmp, strerror(errno));
    }
}


/**
 * @brief Converts a Haxe uchar* name to a Python-safe UTF-8 string, writing it into a buffer.
 *        Specifically, it replaces a leading '$' with 'S_'.
 * @param u_name The Haxe uchar string.
 * @param buffer The output character buffer.
 * @param buffer_size The size of the output buffer.
 */
static void to_python_safe_name(const uchar* u_name, char* buffer, size_t buffer_size) {
    if (!u_name || buffer_size == 0) {
        if(buffer_size > 0) buffer[0] = '\0';
        return;
    }

    char* utf8_name = (char*)hl_to_utf8(u_name);
    if (utf8_name[0] == '$') {
        snprintf(buffer, buffer_size, "S_%s", utf8_name + 1);
    } else {
        strncpy(buffer, utf8_name, buffer_size - 1);
        buffer[buffer_size - 1] = '\0';
    }
}

/**
 * @brief Generates a Python method stub line from a full hl_function definition.
 */
static void print_method_stub_from_func(FILE *f, const char* name, hl_function *func, int findex) {
    if (!func || !func->type || (func->type->kind != HFUN && func->type->kind != HMETHOD)) {
        fprintf(f, "    def %s(self, *args, **kwargs): ... # findex: %d\n", name, findex);
        return;
    }

    fprintf(f, "    def %s(self", name);
    for (int k = 0; k < func->type->fun->nargs; k++) {
        fprintf(f, ", arg%d: %s", k, (char*)hl_to_utf8(python_type_str(func->type->fun->args[k])));
    }
    fprintf(f, ") -> %s: ... # findex: %d\n", (char*)hl_to_utf8(python_type_str(func->type->fun->ret)), findex);
}

/**
 * @brief Generates a Python method stub line from just a function type.
 */
static void print_method_stub_from_type(FILE *f, const char* name, hl_type_fun* fun_type) {
    fprintf(f, "    def %s(self", name);
    for (int k = 0; k < fun_type->nargs; k++) {
        fprintf(f, ", arg%d: %s", k, (char*)hl_to_utf8(python_type_str(fun_type->args[k])));
    }
    fprintf(f, ") -> %s: ...\n", (char*)hl_to_utf8(python_type_str(fun_type->ret)));
}


/**
 * @brief Generates Python class stubs for all Objs in the bytecode.
 * @param code A pointer to the loaded HashLink code.
 */
 void hlmod_generate_stubs(hl_code *code) {
    const char* base_dir = "./mods/hl";
    clock_t start_time = clock();
    printf("[hlmod] Generating class stubs...\n");
    mkdir_p(base_dir);

    char init_path[1024];
    snprintf(init_path, sizeof(init_path), "%s/__init__.py", base_dir);
    FILE *init_f = fopen(init_path, "w");
    if (init_f) {
        fprintf(init_f, "# HLMOD Root Package\n");
        fclose(init_f);
    }

    for (int i = 0; i < code->ntypes; i++) {
        hl_type *t = &code->types[i];
        if (t->kind == HOBJ || t->kind == HSTRUCT) {
            char class_path_full_safe[1024];
            to_python_safe_name(t->obj->name, class_path_full_safe, sizeof(class_path_full_safe));

            char class_path_copy[1024];
            strncpy(class_path_copy, class_path_full_safe, sizeof(class_path_copy));
            class_path_copy[sizeof(class_path_copy) - 1] = '\0';


            char file_path[1024];
            char dir_path[1024];
            char class_name_only[512] = {0};

            char* last_dot = strrchr(class_path_copy, '.');
            if (last_dot) {
                *last_dot = '\0';
                char* package_path = class_path_copy;
                strncpy(class_name_only, last_dot + 1, sizeof(class_name_only) - 1);
                for (char* p = package_path; *p; ++p) {
                    if (*p == '.') *p = '/';
                }
                snprintf(dir_path, sizeof(dir_path), "%s/%s", base_dir, package_path);
            } else {
                strncpy(class_name_only, class_path_copy, sizeof(class_name_only) - 1);
                snprintf(dir_path, sizeof(dir_path), "%s", base_dir);
            }

            mkdir_p(dir_path);
            snprintf(file_path, sizeof(file_path), "%s/%s.py", dir_path, class_name_only);

            char pkg_init_path[1024];
            snprintf(pkg_init_path, sizeof(pkg_init_path), "%s/__init__.py", dir_path);
            // Use "a" to create if not exists, without truncating if it does.
            FILE *pkg_init_f = fopen(pkg_init_path, "a");
            if(pkg_init_f) fclose(pkg_init_f);

            FILE *f = fopen(file_path, "w");
            if (f == NULL) {
                fprintf(stderr, "Error opening file: %s\n", file_path);
                continue;
            }

            fprintf(f, "# This file is automatically generated by hlmod. Do not edit.\n");
            fprintf(f, "from typing import Any, Callable, Optional, List\n\n");

            char parent_class_arg[512] = {0};
            if (t->obj->super) {
                char parent_full_name_safe[1024];
                to_python_safe_name(t->obj->super->obj->name, parent_full_name_safe, sizeof(parent_full_name_safe));

                char parent_full_name_copy[1024];
                strncpy(parent_full_name_copy, parent_full_name_safe, sizeof(parent_full_name_copy));

                char* parent_last_dot = strrchr(parent_full_name_copy, '.');
                if (parent_last_dot) {
                    *parent_last_dot = '\0';
                    fprintf(f, "from hl.%s import %s\n\n", parent_full_name_copy, parent_last_dot + 1);
                    strncpy(parent_class_arg, parent_last_dot + 1, sizeof(parent_class_arg) - 1);
                } else {
                    fprintf(f, "from hl import %s\n\n", parent_full_name_copy);
                    strncpy(parent_class_arg, parent_full_name_copy, sizeof(parent_class_arg) - 1);
                }
            }

            fprintf(f, "class %s(%s):\n", class_name_only, parent_class_arg[0] ? parent_class_arg : "object");

            int* binding_map = malloc(sizeof(int) * t->obj->nfields);
            for(int j = 0; j < t->obj->nfields; j++) binding_map[j] = -1;
            for (int j = 0; j < t->obj->nbindings; j++) {
                int findex = t->obj->bindings[j * 2];
                int ffield = t->obj->bindings[j * 2 + 1];
                if (ffield < t->obj->nfields) binding_map[ffield] = findex;
            }

            bool has_content = false;
            if (t->obj->nfields > 0) fprintf(f, "\n    # --- Fields defined in this class ---\n");
            for (int j = 0; j < t->obj->nfields; j++) {
                hl_obj_field *field = &t->obj->fields[j];
                char safe_field_name[512];
                to_python_safe_name(field->name, safe_field_name, sizeof(safe_field_name));

                int bound_findex = binding_map[j];
                if (bound_findex != -1) {
                    hl_function* func = find_function_by_findex(code, bound_findex);
                    if(func) print_method_stub_from_func(f, safe_field_name, func, bound_findex);
                } else if (field->t->kind == HFUN || field->t->kind == HMETHOD) {
                    print_method_stub_from_type(f, safe_field_name, field->t->fun);
                } else {
                    fprintf(f, "    %s: %s\n", safe_field_name, (char*)hl_to_utf8(python_type_str(field->t)));
                }
                has_content = true;
            }
            free(binding_map);

            if (t->obj->nproto > 0) {
                fprintf(f, "\n    # --- Methods defined in this class ---\n");
                for (int j = 0; j < t->obj->nproto; j++) {
                    hl_obj_proto *proto = &t->obj->proto[j];
                    char safe_proto_name[512];
                    to_python_safe_name(proto->name, safe_proto_name, sizeof(safe_proto_name));

                    hl_function* func = find_function_by_findex(code, proto->findex);
                    if(func) print_method_stub_from_func(f, safe_proto_name, func, proto->findex);
                    has_content = true;
                }
            }

            if (!has_content) fprintf(f, "    pass\n");

            fclose(f);
        }
    }
    clock_t end_time = clock();
    double elapsed_ms = ((double)(end_time - start_time) / CLOCKS_PER_SEC) * 1000.0;
    printf("[hlmod] Finished generating class stubs in %fms.\n", elapsed_ms);
}