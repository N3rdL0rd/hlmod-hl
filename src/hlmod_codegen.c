#include <hlmod_codegen.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <hl.h>
#include <hlmod.h>
#include <cJSON.h>

/*=================================================================================================
 *
 *                             STUB GENERATOR IMPLEMENTATION
 *
 *=================================================================================================*/

#ifdef HL_WIN
#include <direct.h>
#define MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>
#define MKDIR(path) mkdir(path, 0755)
#endif

typedef struct hl_type_set
{
    hl_type **items;
    int count;
    int capacity;
} hl_type_set;

static cJSON *g_docs_root = NULL;

const uchar *python_type_str(hl_type *t);
static void mkdir_p(const char *path);
static hl_function *find_function_by_findex(hl_code *code, int findex);
static hl_function *find_function_by_obj_and_name(hl_code *code, hl_type_obj *obj, const uchar *name);
static void sanitize_name_for_python(const char *input_name, char *buffer, size_t buffer_size);
static void to_python_safe_name(const uchar *u_name, char *buffer, size_t buffer_size);
static void print_method_stub_from_func(FILE *f, const char *name, hl_function *func, int findex, hl_code *code, const char *docstring);
static void print_method_stub_from_type(FILE *f, const char *name, hl_type_fun *fun_type, const char *docstring);
static void type_set_init(hl_type_set *set);
static void type_set_add(hl_type_set *set, hl_type *t);
static void type_set_free(hl_type_set *set);
static void collect_type_dependencies(hl_type *t, hl_type_set *deps);
static void get_python_path_for_type(hl_type *t, const char *base_dir, char *dir_path_out, size_t dir_path_size, char *class_name_out, size_t class_name_size);
static void print_absolute_import_for_type(FILE *f, hl_type *importer_type, hl_type *dependency_type, const char *base_dir, bool is_indented);
static void print_absolute_import_for_named_class(FILE *f, hl_type *dependency_type, const char *base_dir, const char *class_name, bool is_indented);
static void write_class_stub(FILE *f, hl_code *code, hl_type *t, int type_index, const char *class_name, const char *parent_class_arg, int static_tindex);
static void write_static_annotations(FILE *f, hl_code *code, hl_type *static_type, const char *static_class_name);
static void write_static_method_map(FILE *f, hl_code *code, hl_type *static_type);
static void _python_type_str_rec(hl_type *t, uchar *buf, int *pos, int buf_size);
static void load_documentation();
static void free_documentation();
static const char *get_doc_for_type(hl_type *t);
static const char *get_doc_for_member(const char *type_name_full, const char *member_type, const char *member_name);
static void print_docstring(FILE *f, const char *doc, const char *indent);
static bool is_python_keyword(const char *s);

static const char *PYTHON_KEYWORDS[] = {
    "False", "None", "True", "and", "as", "assert", "async", "await",
    "break", "class", "continue", "def", "del", "elif", "else",
    "except", "finally", "for", "from", "global", "if", "import",
    "in", "is", "lambda", "nonlocal", "not", "or", "pass", "raise",
    "return", "try", "while", "with", "yield", NULL};

static bool is_python_keyword(const char *s)
{
    if (!s)
        return false;
    for (int i = 0; PYTHON_KEYWORDS[i] != NULL; i++)
    {
        if (strcmp(s, PYTHON_KEYWORDS[i]) == 0)
        {
            return true;
        }
    }
    return false;
}

static void _str_append(uchar *buf, int *pos, int buf_size, const uchar *str)
{
    if (!str)
        return;
    size_t len = ustrlen(str);
    if (*pos + len < buf_size)
    {
        memcpy(buf + *pos, str, len * sizeof(uchar));
        *pos += (int)len;
    }
}

static void _python_type_str_rec(hl_type *t, uchar *buf, int *pos, int buf_size)
{
    if (t == NULL)
    {
        _str_append(buf, pos, buf_size, USTR("Any"));
        return;
    }
    switch (t->kind)
    {
    case HVOID:
        _str_append(buf, pos, buf_size, USTR("None"));
        break;
    case HUI8:
    case HUI16:
    case HI32:
    case HI64:
        _str_append(buf, pos, buf_size, USTR("int"));
        break;
    case HF32:
    case HF64:
        _str_append(buf, pos, buf_size, USTR("float"));
        break;
    case HBOOL:
        _str_append(buf, pos, buf_size, USTR("bool"));
        break;
    case HBYTES:
        _str_append(buf, pos, buf_size, USTR("bytes"));
        break;
    case HDYN:
        _str_append(buf, pos, buf_size, USTR("Any"));
        break;
    case HTYPE:
        _str_append(buf, pos, buf_size, USTR("type"));
        break;
    case HREF:
        _python_type_str_rec(t->tparam, buf, pos, buf_size);
        break;
    case HDYNOBJ:
        _str_append(buf, pos, buf_size, USTR("dict[str, Any]"));
        break;
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
        if (t->abs_name && ucmp(t->abs_name, USTR("String")) == 0)
        {
            _str_append(buf, pos, buf_size, USTR("str"));
        }
        else
        {
            _str_append(buf, pos, buf_size, t->abs_name ? t->abs_name : USTR("Any"));
        }
        break;
    case HOBJ:
    case HSTRUCT:
        if (t->kind == HOBJ && t->obj && t->obj->name && ucmp(t->obj->name, USTR("String")) == 0)
        {
            _str_append(buf, pos, buf_size, USTR("str"));
            break;
        }
        _str_append(buf, pos, buf_size, USTR("\""));
        const uchar *name_to_convert = NULL;
        if ((t->kind == HOBJ || t->kind == HSTRUCT) && t->obj)
        {
            name_to_convert = t->obj->name;
        }

        if (name_to_convert)
        {
            char safe_name_utf8[1024];
            to_python_safe_name(name_to_convert, safe_name_utf8, sizeof(safe_name_utf8));
            char *last_dot = strrchr(safe_name_utf8, '.');
            const char *name_to_append = last_dot ? last_dot + 1 : safe_name_utf8;
            uchar u_name_to_append[512];
            hl_from_utf8(u_name_to_append, sizeof(u_name_to_append) / sizeof(uchar), name_to_append);
            _str_append(buf, pos, buf_size, u_name_to_append);
        }
        else
        {
            _str_append(buf, pos, buf_size, USTR("Any"));
        }
        _str_append(buf, pos, buf_size, USTR("\""));
        break;
    case HENUM: // TODO: Add proper HENUM support
        _str_append(buf, pos, buf_size, USTR("Any"));
        break;
    case HFUN:
    case HMETHOD:
        _str_append(buf, pos, buf_size, USTR("Callable[["));
        for (int i = 0; i < t->fun->nargs; i++)
        {
            if (i > 0)
                _str_append(buf, pos, buf_size, USTR(", "));
            _python_type_str_rec(t->fun->args[i], buf, pos, buf_size);
        }
        _str_append(buf, pos, buf_size, USTR("], "));
        _python_type_str_rec(t->fun->ret, buf, pos, buf_size);
        _str_append(buf, pos, buf_size, USTR("]"));
        break;
    default:
        _str_append(buf, pos, buf_size, USTR("Any"));
        break;
    }
}

const uchar *python_type_str(hl_type *t)
{
    static uchar buffer[2048];
    int pos = 0;
    memset(buffer, 0, sizeof(buffer));
    _python_type_str_rec(t, buffer, &pos, 2048);
    return buffer;
}

static hl_function *find_function_by_findex(hl_code *code, int findex)
{
    for (int i = 0; i < code->nfunctions; i++)
    {
        if (code->functions[i].findex == findex)
            return &code->functions[i];
    }
    return NULL;
}

static hl_function *find_function_by_obj_and_name(hl_code *code, hl_type_obj *obj, const uchar *name)
{
    if (!code || !obj || !name)
        return NULL;

    for (int i = 0; i < code->nfunctions; i++)
    {
        hl_function *func = &code->functions[i];
        hl_type_obj *func_obj = fun_obj(func);
        const uchar *func_name = fun_field_name(func);
        if (func_obj == obj && func_name != NULL && ucmp(func_name, name) == 0)
            return func;
    }
    return NULL;
}

static void get_argument_names(hl_function *func, hl_code *code, const uchar **names_out, int max_names)
{
    if (!func->assigns)
        return;
    int arg_idx = 0;
    // The assigns with op_index 0 are the argument names, in order, *excluding this*.
    for (int i = 0; i < func->nassigns && arg_idx < max_names; i++)
    {
        if (func->assigns[i].op_index == 0)
        {
            names_out[arg_idx++] = hl_get_ustring(code, func->assigns[i].str_index);
        }
    }
}

static void print_method_stub_from_func(FILE *f, const char *name, hl_function *func, int findex, hl_code *code, const char *docstring)
{
    if (!func || !func->type || (func->type->kind != HFUN && func->type->kind != HMETHOD))
    {
        fprintf(f, "    def %s(self, *args) -> Any:\n", name);
        print_docstring(f, docstring, "        ");
        fprintf(f, "        return self._hlmod_call_findex(%d, *args) # FALLBACK! f@%d\n", findex, findex);
        return;
    }

    int nargs = func->type->fun->nargs;
    int real_nargs = nargs > 0 ? nargs - 1 : 0;
    if (real_nargs > HL_MAX_ARGS)
    {
        fprintf(stderr, "[hlmod] FATAL: Function %s has %d args, exceeding max of %d. Increase HL_MAX_ARGS to prevent this issue!\n", name, real_nargs, HL_MAX_ARGS);
        exit(65);
    }
    const uchar *arg_names[HL_MAX_ARGS];
    char safe_arg_names[HL_MAX_ARGS][512];

    for (int i = 0; i < real_nargs; i++)
        arg_names[i] = NULL;
    get_argument_names(func, code, arg_names, real_nargs);

    for (int i = 0; i < real_nargs; i++)
    {
        const char *arg_name_utf8;
        char fallback_name[16];
        if (arg_names[i])
        {
            arg_name_utf8 = (char *)hl_to_utf8(arg_names[i]);
        }
        else
        {
            snprintf(fallback_name, sizeof(fallback_name), "arg%d", i);
            arg_name_utf8 = fallback_name;
        }
        sanitize_name_for_python(arg_name_utf8, safe_arg_names[i], 512);
    }

    fprintf(f, "    def %s(self", name);
    for (int k = 1; k < nargs; k++)
    { // Start at 1 to skip 'this'
        fprintf(f, ", %s: %s", safe_arg_names[k - 1], (char *)hl_to_utf8(python_type_str(func->type->fun->args[k])));
    }
    fprintf(f, ") -> %s:\n", (char *)hl_to_utf8(python_type_str(func->type->fun->ret)));
    print_docstring(f, docstring, "        ");
    fprintf(f, "        return self._hlmod_call_findex(%d", findex);
    for (int k = 1; k < nargs; k++)
    {
        fprintf(f, ", %s", safe_arg_names[k - 1]);
    }
    fprintf(f, ") # findex: %d\n", findex);
}

static void print_method_stub_from_type(FILE *f, const char *name, hl_type_fun *fun_type, const char *docstring)
{
    fprintf(f, "    def %s(self", name);
    for (int k = 1; k < fun_type->nargs; k++)
    { // Start at 1 to skip 'this'
        fprintf(f, ", arg%d: %s", k - 1, (char *)hl_to_utf8(python_type_str(fun_type->args[k])));
    }
    fprintf(f, ") -> %s:\n", (char *)hl_to_utf8(python_type_str(fun_type->ret)));
    print_docstring(f, docstring, "        ");
    fprintf(f, "        ...\n");
}

static char *read_file_to_buffer(const char *filename)
{
    FILE *f = fopen(filename, "rb");
    if (!f)
        return NULL;

    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buffer = (char *)malloc(length + 1);
    if (!buffer)
    {
        fclose(f);
        return NULL;
    }

    fread(buffer, 1, length, f);
    buffer[length] = '\0';
    fclose(f);
    return buffer;
}

static void load_documentation()
{
    if (g_docs_root)
        return;

    const char *paths_to_try[] = {
        "haxe_docs.json",
        "./mods/haxe_docs.json",
        "./std_doc/haxe_docs.json",
        NULL};

    char *json_string = NULL;
    for (int i = 0; paths_to_try[i] != NULL; i++)
    {
        json_string = read_file_to_buffer(paths_to_try[i]);
        if (json_string)
        {
            printf("[hlmod] Found documentation at: %s\n", paths_to_try[i]);
            break;
        }
    }

    if (!json_string)
    {
        printf("[hlmod] Warning: 'haxe_docs.json' not found. Stubs will be generated without docstrings.\n");
        return;
    }

    g_docs_root = cJSON_Parse(json_string);
    free(json_string);

    if (!g_docs_root)
    {
        printf("[hlmod] Error: Failed to parse JSON from documentation file.\n");
    }
}

static void free_documentation()
{
    if (g_docs_root)
    {
        cJSON_Delete(g_docs_root);
        g_docs_root = NULL;
    }
}

static void print_docstring(FILE *f, const char *doc, const char *indent)
{
    if (!doc || doc[0] == '\0')
    {
        return;
    }

    const char *p = doc;
    bool is_multiline = strchr(doc, '\n') != NULL;

    fprintf(f, "%s\"\"\"", indent);

    if (is_multiline)
    {
        fprintf(f, "\n%s", indent);
    }

    while (*p)
    {
        switch (*p)
        {
        case '\"':
            fputc('\\', f);
            fputc('"', f);
            break;
        case '\\':
            fputc('\\', f);
            fputc('\\', f);
            break;
        case '\n':
            fputc('\n', f);
            fputs(indent, f);
            break;
        default:
            fputc(*p, f);
            break;
        }
        p++;
    }

    if (is_multiline)
    {
        fprintf(f, "\n%s\"\"\"\n", indent);
    }
    else
    {
        fprintf(f, "\"\"\"\n");
    }
}

static const char *get_full_type_name(hl_type *t, char *buffer, size_t buffer_size)
{
    const uchar *type_name_u = NULL;
    if ((t->kind == HOBJ || t->kind == HSTRUCT) && t->obj)
    {
        type_name_u = t->obj->name;
    }
    // TODO: Add proper HENUM support

    if (type_name_u)
    {
        to_python_safe_name(type_name_u, buffer, buffer_size);
        return buffer;
    }
    return NULL;
}

static const char *get_doc_for_type(hl_type *t)
{
    if (!g_docs_root)
        return NULL;

    char type_name_full[1024];
    if (!get_full_type_name(t, type_name_full, sizeof(type_name_full)))
        return NULL;

    cJSON *type_obj = cJSON_GetObjectItemCaseSensitive(g_docs_root, type_name_full);
    if (!type_obj)
        return NULL;

    cJSON *doc_node = cJSON_GetObjectItemCaseSensitive(type_obj, "doc");
    if (cJSON_IsString(doc_node) && (doc_node->valuestring != NULL))
    {
        return doc_node->valuestring;
    }

    return NULL;
}

static const char *get_doc_for_member(const char *type_name_full, const char *member_type, const char *member_name)
{
    if (!g_docs_root || !type_name_full)
        return NULL;

    cJSON *type_obj = cJSON_GetObjectItemCaseSensitive(g_docs_root, type_name_full);
    if (!type_obj)
        return NULL;

    cJSON *members_obj = cJSON_GetObjectItemCaseSensitive(type_obj, member_type); // "functions" or "fields"
    if (!members_obj)
        return NULL;

    cJSON *doc_node = cJSON_GetObjectItemCaseSensitive(members_obj, member_name);
    if (cJSON_IsString(doc_node) && (doc_node->valuestring != NULL))
    {
        return doc_node->valuestring;
    }

    return NULL;
}

static void mkdir_p(const char *path)
{
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len > 0 && (tmp[len - 1] == '/' || tmp[len - 1] == '\\'))
        tmp[len - 1] = 0;
    for (char *p = tmp + 1; *p; p++)
    {
        if (*p == '/' || *p == '\\')
        {
            *p = 0;
            if (MKDIR(tmp) != 0 && errno != EEXIST)
            {
                fprintf(stderr, "[hlmod] Error creating directory %s: %s\n", tmp, strerror(errno));
                return;
            }
            *p = '/';
        }
    }
    if (MKDIR(tmp) != 0 && errno != EEXIST)
    {
        fprintf(stderr, "[hlmod] Error creating directory %s: %s\n", tmp, strerror(errno));
    }
}

static void sanitize_name_for_python(const char *input_name, char *buffer, size_t buffer_size)
{
    char temp_buffer[1024];
    size_t out_pos = 0;

    // Step 1: Handle `$` characters
    for (size_t i = 0; input_name[i] != '\0' && out_pos < sizeof(temp_buffer) - 1; ++i)
    {
        if (input_name[i] == '$')
        {
            if (out_pos < sizeof(temp_buffer) - 2)
            {
                temp_buffer[out_pos++] = 'S';
                temp_buffer[out_pos++] = '_';
            }
        }
        else
        {
            temp_buffer[out_pos++] = input_name[i];
        }
    }
    temp_buffer[out_pos] = '\0';

    // Step 2: Handle Python keywords
    if (is_python_keyword(temp_buffer))
    {
        snprintf(buffer, buffer_size, "%s_", temp_buffer);
    }
    else
    {
        strncpy(buffer, temp_buffer, buffer_size);
        if (buffer_size > 0)
            buffer[buffer_size - 1] = '\0';
    }
}

static void to_python_safe_name(const uchar *u_name, char *buffer, size_t buffer_size)
{
    if (!u_name || buffer_size == 0)
    {
        if (buffer_size > 0)
            buffer[0] = '\0';
        return;
    }
    char *utf8_name = (char *)hl_to_utf8(u_name);
    sanitize_name_for_python(utf8_name, buffer, buffer_size);
}

static void type_set_init(hl_type_set *set)
{
    set->items = NULL;
    set->count = 0;
    set->capacity = 0;
}

static void type_set_free(hl_type_set *set)
{
    if (set->items)
        free(set->items);
    set->items = NULL;
    set->count = 0;
    set->capacity = 0;
}

static void type_set_add(hl_type_set *set, hl_type *t)
{
    if (!t || (t->kind != HOBJ && t->kind != HSTRUCT)) // TODO: Add proper HENUM support
        return;
    for (int i = 0; i < set->count; i++)
    {
        if (set->items[i] == t)
            return;
    }
    if (set->count >= set->capacity)
    {
        set->capacity = set->capacity == 0 ? 16 : set->capacity * 2;
        set->items = realloc(set->items, set->capacity * sizeof(hl_type *));
        if (!set->items)
        {
            fprintf(stderr, "[hlmod] FATAL: realloc failed.\n");
            fflush(stderr);
            exit(1);
        }
    }
    set->items[set->count++] = t;
}

static void collect_type_dependencies(hl_type *t, hl_type_set *deps)
{
    if (!t)
        return;
    switch (t->kind)
    {
    case HOBJ:
    case HSTRUCT:
        type_set_add(deps, t);
        break;
    case HARRAY:
    case HNULL:
    case HREF:
        collect_type_dependencies(t->tparam, deps);
        break;
    case HFUN:
    case HMETHOD:
        if (t->fun)
        {
            for (int i = 0; i < t->fun->nargs; i++)
                collect_type_dependencies(t->fun->args[i], deps);
            collect_type_dependencies(t->fun->ret, deps);
        }
        break;
    default:
        break;
    }
}

static void get_python_path_for_type(hl_type *t, const char *base_dir, char *dir_path_out, size_t dir_path_size, char *class_name_out, size_t class_name_size)
{
    const uchar *type_name = NULL;
    if ((t->kind == HOBJ || t->kind == HSTRUCT) && t->obj)
    {
        type_name = t->obj->name;
    }
    else
    {
        if (dir_path_out) dir_path_out[0] = '\0';
        if (class_name_out) class_name_out[0] = '\0';
        return;
    }

    // TODO: Add proper HENUM support

    if (type_name == NULL)
    {
        if (dir_path_out) dir_path_out[0] = '\0';
        if (class_name_out) class_name_out[0] = '\0';
        return;
    }

    char class_path_full[1024];
    char* utf8_name = (char*)hl_to_utf8(type_name);
    strncpy(class_path_full, utf8_name, sizeof(class_path_full) - 1);
    class_path_full[sizeof(class_path_full) - 1] = '\0';

    char *last_dot = strrchr(class_path_full, '.');
    if (last_dot)
    {
        if (class_name_out && class_name_size > 0)
        {
            sanitize_name_for_python(last_dot + 1, class_name_out, class_name_size);
        }

        if (dir_path_out && dir_path_size > 0)
        {
            *last_dot = '\0';
            char *package_path = class_path_full;

            snprintf(dir_path_out, dir_path_size, "%s", base_dir);
            size_t current_len = strlen(dir_path_out);

            char *component = strtok(package_path, ".");
            while (component != NULL)
            {
                char sanitized_component[512];
                sanitize_name_for_python(component, sanitized_component, sizeof(sanitized_component));

                int written = snprintf(dir_path_out + current_len, dir_path_size - current_len, "/%s", sanitized_component);
                if (written > 0)
                {
                    current_len += written;
                }
                component = strtok(NULL, ".");
            }
        }
    }
    else
    {
        if (class_name_out && class_name_size > 0)
        {
            sanitize_name_for_python(class_path_full, class_name_out, class_name_size);
        }
        if (dir_path_out && dir_path_size > 0)
        {
            snprintf(dir_path_out, dir_path_size, "%s", base_dir);
        }
    }
}

static void print_absolute_import_for_type(FILE *f, hl_type *importer_type, hl_type *dependency_type, const char *base_dir, bool is_indented)
{
    if (importer_type == dependency_type)
    {
        return;
    }

    char dep_module_dir[1024], dep_class_name[512];
    get_python_path_for_type(dependency_type, base_dir, dep_module_dir, sizeof(dep_module_dir), dep_class_name, sizeof(dep_class_name));
    if (dep_class_name[0] == '\0')
        return; // Skip if path could not be determined

    const char *top_level_pkg = strrchr(base_dir, '/');
    if (top_level_pkg)
    {
        top_level_pkg++;
    }
    else
    {
        const char *alt_top_level_pkg = strrchr(base_dir, '\\');
        top_level_pkg = alt_top_level_pkg ? alt_top_level_pkg + 1 : base_dir;
    }

    const char *module_path_rel = dep_module_dir + strlen(base_dir);
    if (*module_path_rel == '/' || *module_path_rel == '\\')
    {
        module_path_rel++;
    }

    char full_module_path[2048];
    int pos = 0;
    int remaining_size = sizeof(full_module_path);
    int written_chars = 0;

    written_chars = snprintf(full_module_path + pos, remaining_size, "%s", top_level_pkg);
    if (written_chars < 0 || written_chars >= remaining_size)
    {
        fprintf(stderr, "[hlmod] Error: Module path construction failed (1).\n");
        return;
    }
    pos += written_chars;
    remaining_size -= written_chars;

    if (strlen(module_path_rel) > 0)
    {
        written_chars = snprintf(full_module_path + pos, remaining_size, ".%s", module_path_rel);
        if (written_chars < 0 || written_chars >= remaining_size)
        {
            fprintf(stderr, "[hlmod] Error: Module path construction failed (2).\n");
            return;
        }
        pos += written_chars;
        remaining_size -= written_chars;
    }

    written_chars = snprintf(full_module_path + pos, remaining_size, ".%s", dep_class_name);
    if (written_chars < 0 || written_chars >= remaining_size)
    {
        fprintf(stderr, "[hlmod] Error: Module path construction failed (3).\n");
        return;
    }
    for (char *p = full_module_path; *p; ++p)
    {
        if (*p == '/' || *p == '\\')
        {
            *p = '.';
        }
    }

    const char *indent = is_indented ? "    " : "";
    fprintf(f, "%sfrom %s import %s\n", indent, full_module_path, dep_class_name);
}

static void print_absolute_import_for_named_class(FILE *f, hl_type *dependency_type, const char *base_dir, const char *class_name, bool is_indented)
{
    if (!class_name || class_name[0] == '\0')
    {
        return;
    }

    char dep_module_dir[1024], dep_module_name[512];
    get_python_path_for_type(dependency_type, base_dir, dep_module_dir, sizeof(dep_module_dir), dep_module_name, sizeof(dep_module_name));
    if (dep_module_dir[0] == '\0' || dep_module_name[0] == '\0')
    {
        return;
    }

    const char *top_level_pkg = strrchr(base_dir, '/');
    if (top_level_pkg)
    {
        top_level_pkg++;
    }
    else
    {
        const char *alt_top_level_pkg = strrchr(base_dir, '\\');
        top_level_pkg = alt_top_level_pkg ? alt_top_level_pkg + 1 : base_dir;
    }

    const char *module_path_rel = dep_module_dir + strlen(base_dir);
    if (*module_path_rel == '/' || *module_path_rel == '\\')
    {
        module_path_rel++;
    }

    char full_module_path[2048];
    int pos = 0;
    int remaining_size = sizeof(full_module_path);
    int written_chars = snprintf(full_module_path + pos, remaining_size, "%s", top_level_pkg);
    if (written_chars < 0 || written_chars >= remaining_size)
    {
        return;
    }
    pos += written_chars;
    remaining_size -= written_chars;

    if (strlen(module_path_rel) > 0)
    {
        written_chars = snprintf(full_module_path + pos, remaining_size, ".%s", module_path_rel);
        if (written_chars < 0 || written_chars >= remaining_size)
        {
            return;
        }
        pos += written_chars;
        remaining_size -= written_chars;
    }

    written_chars = snprintf(full_module_path + pos, remaining_size, ".%s", dep_module_name);
    if (written_chars < 0 || written_chars >= remaining_size)
    {
        return;
    }

    for (char *p = full_module_path; *p; ++p)
    {
        if (*p == '/' || *p == '\\')
        {
            *p = '.';
        }
    }

    const char *indent = is_indented ? "    " : "";
    fprintf(f, "%sfrom %s import %s\n", indent, full_module_path, class_name);
}

static bool is_static_type(hl_type *t)
{
    if (!t || (t->kind != HOBJ && t->kind != HSTRUCT))
        return false;

    if (!t->obj || !t->obj->name || t->obj->name[0] == 0)
        return false;

    char *name_utf8 = (char *)hl_to_utf8(t->obj->name);
    if (!name_utf8)
        return false;

    if (name_utf8[0] == '$')
        return true;

    char *last_dot = strrchr(name_utf8, '.');
    if (last_dot && *(last_dot + 1) == '$')
        return true;

    return false;
}

static hl_type *find_static_pair(hl_code *code, hl_type *instance_type)
{
    if (!instance_type || !instance_type->obj || !instance_type->obj->name || instance_type->obj->name[0] == 0)
        return NULL;

    char *instance_name_utf8 = (char *)hl_to_utf8(instance_type->obj->name);
    char static_name_buffer[1024];

    char *last_dot = strrchr(instance_name_utf8, '.');
    if (last_dot) {
        int package_len = last_dot - instance_name_utf8 + 1;
        snprintf(static_name_buffer, sizeof(static_name_buffer), "%.*s$%s",
                 package_len, instance_name_utf8, last_dot + 1);
    } else {
        snprintf(static_name_buffer, sizeof(static_name_buffer), "$%s", instance_name_utf8);
    }

    uchar static_name_u[1024];
    hl_from_utf8(static_name_u, sizeof(static_name_u) / sizeof(uchar), static_name_buffer);

    for (int i = 0; i < code->ntypes; i++) {
        hl_type *iter_type = &code->types[i];
        
        if (is_static_type(iter_type) && ucmp(iter_type->obj->name, static_name_u) == 0) {
            return iter_type;
        }
    }

    return NULL;
}

static hl_type *find_instance_pair(hl_code *code, hl_type *static_type)
{
    char *static_name_utf8 = (char *)hl_to_utf8(static_type->obj->name);
    char instance_name_buffer[1024];

    char *last_dot = strrchr(static_name_utf8, '.');
    if (last_dot && *(last_dot + 1) == '$') {
        int package_len = last_dot - static_name_utf8 + 1;
        snprintf(instance_name_buffer, sizeof(instance_name_buffer), "%.*s%s",
                 package_len, static_name_utf8, last_dot + 2);
    } else if (static_name_utf8[0] == '$') {
        snprintf(instance_name_buffer, sizeof(instance_name_buffer), "%s", static_name_utf8 + 1);
    } else {
        return NULL;
    }

    uchar instance_name_u[1024];
    hl_from_utf8(instance_name_u, sizeof(instance_name_u) / sizeof(uchar), instance_name_buffer);

    for (int i = 0; i < code->ntypes; i++) {
        hl_type *iter_type = &code->types[i];

        if ((iter_type->kind == HOBJ || iter_type->kind == HSTRUCT) && !is_static_type(iter_type)) {
            if (iter_type->obj && iter_type->obj->name && ucmp(iter_type->obj->name, instance_name_u) == 0) {
                return iter_type;
            }
        }
    }

    return NULL;
}

static void write_class_stub(FILE *f, hl_code *code, hl_type *t, int type_index, const char *class_name, const char *parent_class_arg, int static_tindex)
{
    char type_name_full[1024];
    get_full_type_name(t, type_name_full, sizeof(type_name_full));

    fprintf(f, "\n@hltype(%i)\nclass %s(%s):\n", type_index, class_name, parent_class_arg);
    const char *class_doc = get_doc_for_type(t);
    print_docstring(f, class_doc, "    ");
    if (class_doc && class_doc[0] != '\0')
    {
        fprintf(f, "\n");
    }

    if (static_tindex >= 0)
    {
        fprintf(f, "    _hl_static_tindex = %d\n\n", static_tindex);
    }

    fprintf(f, "    _hl_fields = {\n");

    hl_type *inheritance_chain[HLMOD_MAX_INHERITANCE];
    int chain_depth = 0;
    hl_type *current_type = t;
    while (current_type && chain_depth < HLMOD_MAX_INHERITANCE)
    {
        inheritance_chain[chain_depth++] = current_type;
        current_type = current_type->obj ? current_type->obj->super : NULL;
    }

    int absolute_field_index = 0;
    for (int depth = chain_depth - 1; depth >= 0; depth--)
    {
        hl_type *type_in_chain = inheritance_chain[depth];
        if (type_in_chain->obj)
        {
            for (int j = 0; j < type_in_chain->obj->nfields; j++)
            {
                hl_obj_field *field = &type_in_chain->obj->fields[j];
                char safe_name[512];
                to_python_safe_name(field->name, safe_name, sizeof(safe_name));
                if (safe_name[0] != '\0')
                {
                    fprintf(f, "        \"%s\": %d,\n", safe_name, absolute_field_index);
                }
                absolute_field_index++;
            }
        }
    }
    fprintf(f, "    }\n\n");

    int *binding_map = NULL;
    if (t->obj->nfields > 0)
    {
        binding_map = calloc(t->obj->nfields, sizeof(int));
        for (int j = 0; j < t->obj->nfields; j++)
            binding_map[j] = -1;
        for (int j = 0; j < t->obj->nbindings; j++)
        {
            int ffield = t->obj->bindings[j * 2 + 1];
            int findex = t->obj->bindings[j * 2];
            if (ffield >= 0 && ffield < t->obj->nfields)
                binding_map[ffield] = findex;
        }
    }

    bool has_content = false;
    if (t->obj->nfields > 0)
    {
        fprintf(f, "    # --- Fields ---\n");
        for (int j = 0; j < t->obj->nfields; j++)
        {
            hl_obj_field *field = &t->obj->fields[j];
            char safe_field_name[512];
            to_python_safe_name(field->name, safe_field_name, sizeof(safe_field_name));
            if (safe_field_name[0] == '\0')
            {
                continue;
            }

            const char *field_doc = get_doc_for_member(type_name_full, "fields", safe_field_name);

            if (binding_map && binding_map[j] != -1)
            {
                hl_function *func = find_function_by_findex(code, binding_map[j]);
                if (func)
                    print_method_stub_from_func(f, safe_field_name, func, binding_map[j], code, field_doc);
            }
            else if (field->t->kind == HFUN || field->t->kind == HMETHOD)
            {
                fprintf(f, "    %s: %s\n", safe_field_name, (char *)hl_to_utf8(python_type_str(field->t)));
                print_docstring(f, field_doc, "    ");
            }
            else
            {
                fprintf(f, "    %s: %s\n", safe_field_name, (char *)hl_to_utf8(python_type_str(field->t)));
                print_docstring(f, field_doc, "    ");
            }
            fprintf(f, "\n");
            has_content = true;
        }
    }
    if (binding_map)
        free(binding_map);

    if (t->obj->nproto > 0)
    {
        fprintf(f, "    # --- Methods ---\n");
        for (int j = 0; j < t->obj->nproto; j++)
        {
            hl_obj_proto *proto = &t->obj->proto[j];
            char safe_proto_name[512];
            to_python_safe_name(proto->name, safe_proto_name, sizeof(safe_proto_name));
            const char *method_doc = get_doc_for_member(type_name_full, "functions", safe_proto_name);
            hl_function *func = find_function_by_findex(code, proto->findex);
            if (func)
                print_method_stub_from_func(f, safe_proto_name, func, proto->findex, code, method_doc);
            fprintf(f, "\n");
            has_content = true;
        }
    }

    if (!has_content)
        fprintf(f, "    pass\n");
}

static void write_static_annotations(FILE *f, hl_code *code, hl_type *static_type, const char *static_class_name)
{
    if (!static_type || !static_type->obj)
    {
        return;
    }

    fprintf(f, "    # --- Static ---\n");
    fprintf(f, "    STATIC: \"%s\"\n", static_class_name);

    for (int j = 0; j < static_type->obj->nfields; j++)
    {
        hl_obj_field *field = &static_type->obj->fields[j];
        char safe_field_name[512];
        to_python_safe_name(field->name, safe_field_name, sizeof(safe_field_name));
        if (safe_field_name[0] == '\0')
        {
            continue;
        }
        fprintf(f, "    %s: %s\n", safe_field_name, (char *)hl_to_utf8(python_type_str(field->t)));
    }

    for (int j = 0; j < static_type->obj->nproto; j++)
    {
        hl_obj_proto *proto = &static_type->obj->proto[j];
        char safe_proto_name[512];
        to_python_safe_name(proto->name, safe_proto_name, sizeof(safe_proto_name));
        if (safe_proto_name[0] == '\0')
        {
            continue;
        }
        hl_function *func = find_function_by_findex(code, proto->findex);
        if (func && func->type)
        {
            fprintf(f, "    %s: %s\n", safe_proto_name, (char *)hl_to_utf8(python_type_str(func->type)));
        }
    }
    fprintf(f, "\n");
}

static void write_static_method_map(FILE *f, hl_code *code, hl_type *static_type)
{
    fprintf(f, "    _hl_static_methods = {\n");
    if (!static_type || !static_type->obj)
    {
        fprintf(f, "    }\n\n");
        return;
    }

    const char *type_name_utf8 = (const char *)hl_to_utf8(static_type->obj->name);

    for (int j = 0; j < static_type->obj->nfields; j++)
    {
        hl_obj_field *field = &static_type->obj->fields[j];
        if (field->t->kind != HFUN && field->t->kind != HMETHOD)
        {
            continue;
        }

        char safe_field_name[512];
        char raw_field_name[512];
        to_python_safe_name(field->name, safe_field_name, sizeof(safe_field_name));
        snprintf(raw_field_name, sizeof(raw_field_name), "%s", (const char *)hl_to_utf8(field->name));
        if (safe_field_name[0] != '\0')
        {
            fprintf(f, "        \"%s\": \"%s.%s\",\n", safe_field_name, type_name_utf8, raw_field_name);
        }
    }

    for (int j = 0; j < static_type->obj->nproto; j++)
    {
        hl_obj_proto *proto = &static_type->obj->proto[j];
        char safe_proto_name[512];
        char raw_proto_name[512];
        to_python_safe_name(proto->name, safe_proto_name, sizeof(safe_proto_name));
        snprintf(raw_proto_name, sizeof(raw_proto_name), "%s", (const char *)hl_to_utf8(proto->name));
        if (safe_proto_name[0] != '\0')
        {
            fprintf(f, "        \"%s\": \"%s.%s\",\n", safe_proto_name, type_name_utf8, raw_proto_name);
        }
    }

    fprintf(f, "    }\n\n");
}

void hlmod_do_generate_stubs(hl_code *code)
{
    const char *base_dir = "./mods/stubs";
    clock_t start_time = clock();
    printf("[hlmod] Generating class stubs... (this may take up to 10 seconds)\n");

    load_documentation();

    mkdir_p(base_dir);

    for (int i = 0; i < code->ntypes; i++)
    {
        hl_type *t = &code->types[i];
        if (t->kind != HOBJ && t->kind != HSTRUCT) // TODO: Add proper HENUM support
            continue;
        char dir_path[1024];
        get_python_path_for_type(t, base_dir, dir_path, sizeof(dir_path), NULL, 0);
        if (dir_path[0] == '\0')
            continue;
        char pkg_init_path[1024];
        snprintf(pkg_init_path, sizeof(pkg_init_path), "%s/__init__.py", dir_path);
        remove(pkg_init_path);
    }
    char root_init_path[1024];
    snprintf(root_init_path, sizeof(root_init_path), "%s/__init__.py", base_dir);
    remove(root_init_path);

    for (int i = 0; i < code->ntypes; i++)
    {
        hl_type *t = &code->types[i];
        if (t->kind != HOBJ && t->kind != HSTRUCT) // TODO: Add proper HENUM support
            continue;
        if (t->kind == HOBJ && t->obj && t->obj->name && ucmp(t->obj->name, USTR("String")) == 0)
            continue;

        hl_type *static_pair = NULL;

        if (is_static_type(t))
        {
            hl_type *instance_pair = find_instance_pair(code, t);
            if (instance_pair != NULL)
            {
                printf("[hlmod] Deferring static type '%s' (will be handled by '%s')\n",
                       (char *)hl_to_utf8(t->obj->name),
                       (char *)hl_to_utf8(instance_pair->obj->name));
                continue;
            }
            else
            {
                printf("[hlmod] Processing standalone static type '%s'\n", (char *)hl_to_utf8(t->obj->name));
            }
        }
        else
        {
            static_pair = find_static_pair(code, t);
            if (static_pair)
            {
                printf("[hlmod] Paired instance type '%s' with static type '%s'\n",
                       (char *)hl_to_utf8(t->obj->name),
                       (char *)hl_to_utf8(static_pair->obj->name));
            }
        }

        char dir_path[1024], class_name_only[512];
        get_python_path_for_type(t, base_dir, dir_path, sizeof(dir_path), class_name_only, sizeof(class_name_only));
        if (class_name_only[0] == '\0')
            continue;

        char file_path[1024];
        snprintf(file_path, sizeof(file_path), "%s/%s.py", dir_path, class_name_only);

        mkdir_p(dir_path);
        char pkg_init_path[1024];
        snprintf(pkg_init_path, sizeof(pkg_init_path), "%s/__init__.py", dir_path);
        FILE *pkg_init_f = fopen(pkg_init_path, "a");
        if (pkg_init_f)
        {
            fprintf(pkg_init_f, "from .%s import %s\n", class_name_only, class_name_only);
            fclose(pkg_init_f);
        }

        FILE *f = fopen(file_path, "w");
        if (!f)
        {
            fprintf(stderr, "[hlmod] Error: Could not open for writing: %s\n", file_path);
            continue;
        }

        fprintf(f, "# This file is automatically generated by hlmod. Do not edit.\n");
        fprintf(f, "from __future__ import annotations\n");
        fprintf(f, "from typing import Any, Callable, Optional, List, TYPE_CHECKING\n");

        // TODO: Add proper HENUM support
        fprintf(f, "from hlobj import HlObject, hltype\n\n");

        hl_type_set deps;
        type_set_init(&deps);

        if (t->obj->super)
        {
            print_absolute_import_for_type(f, t, t->obj->super, base_dir, false);
        }
        for (int j = 0; j < t->obj->nfields; j++)
            collect_type_dependencies(t->obj->fields[j].t, &deps);
        for (int j = 0; j < t->obj->nproto; j++)
        {
            hl_function *func = find_function_by_findex(code, t->obj->proto[j].findex);
            if (func)
                collect_type_dependencies(func->type, &deps);
        }
        if (static_pair)
        {
            hl_type *static_super = t->obj->super ? find_static_pair(code, t->obj->super) : NULL;
            if (static_super)
            {
                char static_super_class_name[512];
                char super_class_name[512];
                get_python_path_for_type(t->obj->super, base_dir, NULL, 0, super_class_name, sizeof(super_class_name));
                snprintf(static_super_class_name, sizeof(static_super_class_name), "_%sStatic", super_class_name);
                print_absolute_import_for_named_class(f, t->obj->super, base_dir, static_super_class_name, false);
            }
            for (int j = 0; j < static_pair->obj->nfields; j++)
                collect_type_dependencies(static_pair->obj->fields[j].t, &deps);
            for (int j = 0; j < static_pair->obj->nproto; j++)
            {
                hl_function *func = find_function_by_findex(code, static_pair->obj->proto[j].findex);
                if (func)
                    collect_type_dependencies(func->type, &deps);
            }
        }

        bool has_type_deps = false;
        for (int j = 0; j < deps.count; j++)
        {
            if (deps.items[j] == t)
                continue;
            if ((t->kind == HOBJ || t->kind == HSTRUCT) && deps.items[j] == t->obj->super)
                continue;
            if (static_pair && deps.items[j] == static_pair)
                continue;
            has_type_deps = true;
            break;
        }

        if (has_type_deps)
        {
            fprintf(f, "\nif TYPE_CHECKING:\n");
            for (int j = 0; j < deps.count; j++)
            {
                if (deps.items[j] == t)
                    continue;
                if ((t->kind == HOBJ || t->kind == HSTRUCT) && deps.items[j] == t->obj->super)
                    continue;
                if (static_pair && deps.items[j] == static_pair)
                    continue;
                print_absolute_import_for_type(f, t, deps.items[j], base_dir, true);
            }
        }

        type_set_free(&deps);

        char parent_class_arg[512] = "HlObject";
        if (t->obj->super)
        {
            get_python_path_for_type(t->obj->super, base_dir, NULL, 0, parent_class_arg, sizeof(parent_class_arg));
        }
        if (static_pair)
        {
            char static_class_name[512];
            snprintf(static_class_name, sizeof(static_class_name), "_%sStatic", class_name_only);

            char static_parent_class_arg[512] = "HlObject";
            if (t->obj->super)
            {
                hl_type *static_super = find_static_pair(code, t->obj->super);
                if (static_super)
                {
                    char super_class_name[512];
                    get_python_path_for_type(t->obj->super, base_dir, NULL, 0, super_class_name, sizeof(super_class_name));
                    snprintf(static_parent_class_arg, sizeof(static_parent_class_arg), "_%sStatic", super_class_name);
                }
            }

            write_class_stub(f, code, static_pair, (int)(static_pair - code->types), static_class_name, static_parent_class_arg, -1);
            fprintf(f, "\n");
        }

        write_class_stub(f, code, t, i, class_name_only, parent_class_arg, static_pair ? (int)(static_pair - code->types) : -1);
        if (static_pair)
        {
            char static_class_name[512];
            snprintf(static_class_name, sizeof(static_class_name), "_%sStatic", class_name_only);
            write_static_method_map(f, code, static_pair);
            write_static_annotations(f, code, static_pair, static_class_name);
        }
        fclose(f);
    }

    free_documentation();

    clock_t end_time = clock();
    double elapsed_ms = ((double)(end_time - start_time) / CLOCKS_PER_SEC) * 1000.0;
    printf("[hlmod] Finished generating class stubs in %.2fms.\n", elapsed_ms);
}

static void write_signature_file(const char *path, const char *hash)
{
    mkdir_p("./mods/stubs");

    FILE *f = fopen(path, "w");
    if (!f)
    {
        fprintf(stderr, "[hlmod] Error: Could not open signature file for writing: %s\n", path);
        return;
    }
    fprintf(f, "%s", hash);
    fclose(f);
}

void hlmod_generate_stubs(hl_code *code) {
#   ifndef SOURCE_FILE_SHA256_HASH
    printf("[hlmod] WARNING: CMake seems to be configured incorrectly! Generating stubs anyway...\n");
    hlmod_do_generate_stubs(code);
#   else
    const char *base_dir = "./mods/stubs";
    char signature_path[1024];
    snprintf(signature_path, sizeof(signature_path), "%s/.source_hash", base_dir);

    const char *current_hash = SOURCE_FILE_SHA256_HASH;
    char *saved_hash = read_file_to_buffer(signature_path);

    bool should_regenerate = false;
    if (saved_hash == NULL)
    {
        printf("[hlmod] Signature file not found. Stubs will be regenerated.\n");
        should_regenerate = true;
    }
    else
    {
        if (strcmp(saved_hash, current_hash) != 0)
        {
            printf("[hlmod] Source signature has changed. Stubs will be regenerated.\n");
            should_regenerate = true;
        }
        else
        {
            printf("[hlmod] Source signature matches. Skipping stub generation.\n");
        }
        free(saved_hash);
    }

    if (should_regenerate)
    {
        hlmod_do_generate_stubs(code);
        write_signature_file(signature_path, current_hash);
    }
#   endif
}
