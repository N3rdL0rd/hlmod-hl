/*
 * Copyright (C)2015-2016 Haxe Foundation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#define USE_HLMOD_CRASH // TODO: some arg for this
// #define NO_STUBGEN

#include <hl.h>
#include <hlmodule.h>
#include "hlsystem.h"

#include <Python.h>
#include <hlmod.h>
#include <hlmod_codegen.h>
#include <hlmod_embedded.h>
#include <hlmod_python.h>
#include "native_hook.h"

#ifndef HL_WIN
#   include <unistd.h>
#   include <libgen.h>
#   include <string.h>
#endif

#include "sha256.h"
char g_code_sha256[65] = {0};

#ifdef USE_HLMOD_CRASH
#include <hlmod_crash.h>
#endif

hl_code *g_code = NULL;

#ifdef HL_WIN
#	include <locale.h>
#	include <direct.h>
#	define MKDIR(path) _mkdir(path)
#   define pprintf(str,file)	uprintf(USTR(str),file)
#   define pfopen(file,ext) _wfopen(file,USTR(ext))
#   define pcompare wcscmp
#   define ptoi(s)	wcstol(s,NULL,10)
#   define PSTR(x) USTR(x)
#   include <windows.h>
#   include <fcntl.h>
#   include <stdio.h>
#   include <io.h>
typedef uchar pchar;
#else
#	include <sys/stat.h>
#	include <errno.h>
#	define MKDIR(path) mkdir(path, 0755)
#   define pprintf printf
#   define pfopen fopen
#   define pcompare strcmp
#   define ptoi atoi
#   define PSTR(x) x
typedef char pchar;
#endif

typedef struct {
	pchar *file;
	hl_code *code;
	hl_module *m;
	vdynamic *ret;
	int file_time;
} main_context;

static int pfiletime( pchar *file )	{
#ifdef HL_WIN
	struct _stat32 st;
	_wstat32(file,&st);
	return (int)st.st_mtime;
#else
	struct stat st;
	stat(file,&st);
	return (int)st.st_mtime;
#endif
}

static hl_code *load_code( const pchar *file, char **error_msg, bool print_errors ) {
	hl_code *code;
	FILE *f = pfopen(file,"rb");
	int pos, size;
	char *fdata;
	if( f == NULL ) {
		if( print_errors ) pprintf("File not found '%s'\n",file);
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	size = (int)ftell(f);
	fseek(f, 0, SEEK_SET);
	fdata = (char*)malloc(size);
	pos = 0;
	while( pos < size ) {
		int r = (int)fread(fdata + pos, 1, size-pos, f);
		if( r <= 0 ) {
			if( print_errors ) pprintf("Failed to read '%s'\n",file);
			return NULL;
		}
		pos += r;
	}
	fclose(f);
    SHA256_CTX ctx;
    SHA256_BYTE hash[SHA256_BLOCK_SIZE];

    sha256_init(&ctx);
    sha256_update(&ctx, (SHA256_BYTE*)fdata, size);
    sha256_final(&ctx, hash);

    for(int i = 0; i < SHA256_BLOCK_SIZE; i++)
    	sprintf(&g_code_sha256[i * 2], "%02x", hash[i]);

    printf("[hlmod] Bytecode SHA256: %s\n", g_code_sha256);

	code = hl_code_read((unsigned char*)fdata, size, error_msg);
	free(fdata);
	return code;
}

static void get_exe_dir(pchar* buffer, int buffer_size) {
#ifdef HL_WIN
    if (GetModuleFileNameW(NULL, buffer, buffer_size) == 0) {
        buffer[0] = L'\0';
        return;
    }
    pchar* last_slash = wcsrchr(buffer, L'\\');
    if (last_slash) {
        *(last_slash + 1) = L'\0';
    }
#else
    char exe_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len == -1) {
        buffer[0] = '\0';
        return;
    }
    exe_path[len] = '\0';

    char* dir = dirname(exe_path);
    strncpy(buffer, dir, buffer_size - 2);
    buffer[buffer_size - 2] = '\0';

    strcat(buffer, "/");
#endif
}

static void check_deadcells() {
    const char* deadcells_hashes[] = {
        "696aaec83db7a21e76c828449167880f001ef056c230350dca6fddccc34cc9c7",
        "376564ab2173ddcbadf53d73baf2fc335793e4d14a637fc1829569c314f39667",
        "d5d17575f4bec6ab674a9cac56fba5fd696576f23fc1b22e32629bcafba92ad3",
        "45ebaecbedeff7c9b4b7d8b2faaf91ecd47c1f74dd7642938444ec9c83b488f5",
        NULL
    };

    for (int i = 0; deadcells_hashes[i] != NULL; i++) {
        if (strcmp(g_code_sha256, deadcells_hashes[i]) == 0) {
            printf("[hlmod] Dead Cells detected...\n");
#ifdef HL_WIN
            pchar exe_dir[MAX_PATH];
            get_exe_dir(exe_dir, MAX_PATH);

            pchar appid_path[MAX_PATH];
            swprintf(appid_path, MAX_PATH, L"%ssteam_appid.txt", exe_dir);

            if (_waccess(appid_path, 0) == -1) {
                printf("[hlmod] steam_appid.txt not found. Creating it for Dead Cells (588650).\n");
                FILE* f = _wfopen(appid_path, L"w");
                if (f) {
                    fprintf(f, "588650");
                    fclose(f);
                }
            }
#endif
            return;
        }
    }
}

static bool check_reload( main_context *m ) {
	int time = pfiletime(m->file);
	bool changed;
	if( time == m->file_time )
		return false;
	char *error_msg = NULL;
	hl_code *code = load_code(m->file, &error_msg, false);
	if( code == NULL )
		return false;
	changed = hl_module_patch(m->m, code);
	m->file_time = time;
	hl_code_free(code);
	return changed;
}

typedef struct {
    int registered;
    PyObject *callback; /* GIL protected; dispatch takes its own reference. */
} HookSlot;

static HookSlot *hook_slots;
static int hook_slot_count;

/* Init/dispose run outside the lifetime of native callers. Slots never move. */
int hlmod_hook_registry_init(int count) {
    if(hook_slots || count < 0) {
        PyErr_SetString(PyExc_RuntimeError, "Invalid hook registry initialization");
        return -1;
    }
    hook_slots = calloc(count ? count : 1, sizeof(*hook_slots));
    if(!hook_slots) {
        PyErr_NoMemory();
        return -1;
    }
    hook_slot_count = count;
    return 0;
}

bool hlmod_hook_registered(int findex) {
    return findex >= 0 && findex < hook_slot_count &&
        hlmod_atomic_load_int(&hook_slots[findex].registered) != 0;
}

PyObject *hlmod_hook_callback(int findex) {
    if(findex < 0 || findex >= hook_slot_count) return NULL;
    return Py_XNewRef(hook_slots[findex].callback);
}

void hlmod_hook_registry_shutdown(void) {
    /* Unpublish all callbacks before decrefs can reenter the registry. */
    int count = hook_slot_count;
    hook_slot_count = 0;
    for(int i = 0; i < count; i++) Py_CLEAR(hook_slots[i].callback);
    free(hook_slots);
    hook_slots = NULL;
}

void hlmod_register_hook(int findex, PyObject* callback) {
    if(g_module == NULL || !hook_slots) {
        PyErr_SetString(PyExc_RuntimeError, "HL hook registry is not initialized");
        return;
    }
    if(findex < 0 || findex >= hook_slot_count) {
        PyErr_SetString(PyExc_IndexError, "Function index is out of bounds");
        return;
    }
    int index = g_module->functions_indexes[findex];
    bool is_native = index >= g_module->code->nfunctions;
    if(index < 0 || index >= g_module->code->nfunctions + g_module->code->nnatives) {
        PyErr_SetString(PyExc_ValueError, "Unknown function index");
        return;
    }
    if(!PyCallable_Check(callback)) {
        PyErr_SetString(PyExc_TypeError, "Hook callback must be callable");
        return;
    }
    HookSlot *slot = &hook_slots[findex];
    if(slot->callback) {
        if(slot->callback != callback)
            PyErr_SetString(PyExc_ValueError, "Function already has a different hook callback");
        return;
    }
    if(is_native) {
        hl_native *native = &g_module->code->natives[index - g_module->code->nfunctions];
        if(hlmod_native_hook_ensure_installed(findex, native->t) != 0) return;
    }
    slot->callback = Py_NewRef(callback);
    hlmod_atomic_store_int(&slot->registered, 1);
}

static PyObject *hlmod_py_unregister_hook(PyObject *self, PyObject *args) {
    int findex;
    PyObject *callback = Py_None;
    if(!PyArg_ParseTuple(args, "i|O:unregister_hook", &findex, &callback)) return NULL;
    if(findex < 0 || findex >= hook_slot_count) Py_RETURN_FALSE;
    HookSlot *slot = &hook_slots[findex];
    if(!slot->callback || (callback != Py_None && callback != slot->callback)) Py_RETURN_FALSE;
    /* Readers that already saw true recheck under the GIL, never dereference
       a callback from the no-GIL path. In-flight calls own their callback. */
    hlmod_atomic_store_int(&slot->registered, 0);
    Py_CLEAR(slot->callback);
    Py_RETURN_TRUE;
}

static PyObject* hlmod_py_register_hook(PyObject *self, PyObject *args) {
    int findex;
    PyObject* callback;

	// int, PyObject*
    if (!PyArg_ParseTuple(args, "iO", &findex, &callback)) {
        return NULL;
    }

    if (!PyCallable_Check(callback)) {
        PyErr_SetString(PyExc_TypeError, "Second argument must be a Callable!");
        return NULL;
    }

    hlmod_register_hook(findex, callback);
    if (PyErr_Occurred()) return NULL;

    Py_RETURN_NONE;
}

static PyMethodDef HlmodMethods[] = {
    {"register_hook", hlmod_py_register_hook, METH_VARARGS, "Hooks a function by its findex."},
    {"unregister_hook", hlmod_py_unregister_hook, METH_VARARGS, "Remove a hook, optionally only if its callback is identical."},
    {"register_hlobj", hlmod_py_register_hlobj, METH_VARARGS, "Registers a Python class for a given Haxe type index."},
    {"create_subclass", hlmod_py_create_subclass, METH_VARARGS, "Register a native HL subtype backed by a Python class."},
    {"alloc_obj", hlmod_py_alloc_obj, METH_VARARGS, "Allocate an instance of an HL object type."},
    {"init_obj", hlmod_py_init_obj, METH_VARARGS, "Initialize a preallocated instance with its native constructor."},
    {"bind_instance", hlmod_py_bind_instance, METH_VARARGS, "Bind native object identity to its Python instance."},
    {"make_callback", hlmod_py_make_callback, METH_VARARGS, "Create an HL closure from a Python callable and signature."},
    {"array_new", hlmod_py_array_new, METH_VARARGS, "Create a typed HL native array from element type index and iterable."},
    {"array_length", hlmod_py_array_length, METH_VARARGS, "Return a native array's length."},
    {"array_get", hlmod_py_array_get, METH_VARARGS, "Read a native array element."},
    {"array_set", hlmod_py_array_set, METH_VARARGS, "Write a native array element."},
    {"array_element_type", hlmod_py_array_element_type, METH_VARARGS, "Return the element type index or None."},
    {"enum_info", hlmod_py_enum_info, METH_VARARGS, "Inspect a native enum constructor and parameters."},
    {"enum_new", hlmod_py_enum_new, METH_VARARGS, "Construct a typed native enum value."},
    {"dynobj_new", hlmod_py_dynobj_new, METH_VARARGS, "Allocate a native dynamic object."},
    {"dynobj_keys", hlmod_py_dynobj_keys, METH_VARARGS, "List native dynamic object fields."},
    {"dynobj_get", hlmod_py_dynobj_get, METH_VARARGS, "Read a native dynamic object field."},
    {"dynobj_set", hlmod_py_dynobj_set, METH_VARARGS, "Write a native dynamic object field."},
    {"dynobj_delete", hlmod_py_dynobj_delete, METH_VARARGS, "Delete a native dynamic object field."},
    {"ref_new", hlmod_py_ref_new, METH_VARARGS, "Allocate a typed native reference."},
    {"ref_get", hlmod_py_ref_get, METH_VARARGS, "Read a native reference."},
    {"ref_set", hlmod_py_ref_set, METH_VARARGS, "Write a native reference."},
    {"inspect_native", hlmod_py_inspect_native, METH_VARARGS, "Inspect native type fields and methods."},
    {"bytes_new", hlmod_py_bytes_new, METH_VARARGS, "Allocate zeroed native bytes."},
    {"bytes_from", hlmod_py_bytes_from, METH_VARARGS, "Copy a Python buffer into native bytes."},
    {"bytes_capacity", hlmod_py_bytes_capacity, METH_VARARGS, "Allocation size of native bytes, or None."},
    {"bytes_read", hlmod_py_bytes_read, METH_VARARGS, "Read a bounded range of native bytes."},
    {"bytes_write", hlmod_py_bytes_write, METH_VARARGS, "Write a bounded range of native bytes."},
    {"get_obj_field", hlmod_py_get_obj_field, METH_VARARGS, "Gets a field value from a Haxe object."},
    {"set_obj_field", hlmod_py_set_obj_field, METH_VARARGS, "Sets a field value on a Haxe object."},
    {"get_virtual_field", hlmod_py_get_virtual_field, METH_VARARGS, "Gets a field value from a Haxe virtual object."},
    {"set_virtual_field", hlmod_py_set_virtual_field, METH_VARARGS, "Sets a field value on a Haxe virtual object."},
    {"get_virtual_field_count", hlmod_py_get_virtual_field_count, METH_VARARGS, "Gets the field count for a Haxe virtual object."},
    {"get_virtual_field_name", hlmod_py_get_virtual_field_name, METH_VARARGS, "Gets the field name for a Haxe virtual object field index."},
    {"set_fixed_prng", hlmod_py_set_fixed_prng, METH_VARARGS, "Sets the PRNG to a fixed or random state."},
    {"get_fixed_prng", hlmod_py_get_fixed_prng, METH_NOARGS, "Gets whether the PRNG is in a fixed state."},
    {"assert_code_sha", hlmod_py_assert_code_sha, METH_VARARGS, "Asserts the bytecode SHA256, exiting if it mismatches."},
    {"get_global", hlmod_py_get_global, METH_VARARGS, "Gets the global instance of a type by index. Useful for static types."},
    {"ensure_global", hlmod_py_ensure_global, METH_VARARGS, "Ensures the global instance of a type by index is allocated, returning it."},
    {"call", hlmod_py_call, METH_VARARGS, "Calls an HL function by findex."},
    {"call_closure", hlmod_py_call_closure, METH_VARARGS, "Calls an HL closure by pointer."},
    {"dump_stack", hlmod_py_dump_stack, METH_NOARGS, "Dumps the current HL stack."},
    {"findex_for_name", hlmod_py_findex_for_name, METH_VARARGS, "Gets the findex for a specific function by its name"},
    {"native_findex", hlmod_py_native_findex, METH_VARARGS, "Gets the findex of a @:hlNative function by its (lib, name)."},
    {"profile_start", hlmod_py_profile_start, METH_VARARGS, "Starts the HL sampling profiler at the given samples/sec (default 1000)."},
    {"profile_end", hlmod_py_profile_end, METH_NOARGS, "Stops the profiler and writes hlprofile.dump."},
    {NULL, NULL, 0, NULL}
};
static struct PyModuleDef hlmod_module_def = {
    PyModuleDef_HEAD_INIT,
    "hlmod",                        // The name of the module in Python
    "Low-level hlmod framework API.", // Module's docstring
    -1,
    HlmodMethods                    // Link to the method table
};
PyMODINIT_FUNC PyInit_hlmod(void) {
    if (PyType_Ready(&HlPtrType) < 0)
        return NULL;
    if (PyType_Ready(&HlHookType) < 0)
        return NULL;

    PyObject* m = PyModule_Create(&hlmod_module_def);
    if (m == NULL)
        return NULL;
    
    if (PyModule_AddObjectRef(m, "HlPtr", (PyObject *)&HlPtrType) < 0 ||
        PyModule_AddObjectRef(m, "Hook", (PyObject *)&HlHookType) < 0 ||
        PyModule_AddStringConstant(m, "version", HLMOD_VERSION) < 0) {
        Py_DECREF(m);
        return NULL;
    }

    return m;
}




/**
 * @brief Calls a Python function from a script string to determine the mod load order.
 *
 * @param mods_dir The directory where the .py mod files are located.
 * @param load_order_list A pointer to a PyObject* that will receive the list of mods to load.
 * @return 1 on success (and load_order_list is populated), 0 on failure.
 * The caller is responsible for DECREF'ing the returned list.
 */
int get_mod_load_order(const char *mods_dir, PyObject **load_order_list) {
    *load_order_list = NULL;
    PyObject *module = PyImport_AddModule("_hlmod_mod_sorter");
    if (module == NULL) goto error;
    PyObject *compiled = Py_CompileString(hlmod_mod_sorter_source, "<hlmod>/mod_sorter.py", Py_file_input);
    if (compiled == NULL) goto error;
    PyObject *globals = PyModule_GetDict(module);
    PyObject *result = PyEval_EvalCode(compiled, globals, globals);
    Py_DECREF(compiled);
    if (result == NULL) goto error;
    Py_DECREF(result);
    result = PyObject_CallMethod(module, "resolve_mod_order", "s", mods_dir);
    if (result == NULL) goto error;
    if (!PyDict_Check(result)) {
        Py_DECREF(result);
        PyErr_SetString(PyExc_TypeError, "Mod resolver did not return a dictionary");
        goto error;
    }
    PyObject *status = PyDict_GetItemString(result, "status");
    if (status != NULL && PyUnicode_Check(status) && PyUnicode_CompareWithASCIIString(status, "ok") == 0) {
        PyObject *order = PyDict_GetItemString(result, "order");
        if (order != NULL && PyList_Check(order)) {
            *load_order_list = Py_NewRef(order);
            Py_DECREF(result);
            return 1;
        }
        PyErr_SetString(PyExc_TypeError, "Mod resolver did not return a load-order list");
    } else {
        PyObject *message = PyDict_GetItemString(result, "message");
        PyErr_SetObject(PyExc_RuntimeError, message != NULL ? message : Py_None);
    }
    Py_DECREF(result);
error:
    PyErr_Print();
    return 0;
}

/**
 * @brief Loads and initializes a Python mod within its ownership scope.
 */
static bool load_mod(PyObject *framework, PyObject *info) {
    PyObject *id = PyDict_GetItemString(info, "id");
    PyObject *name = PyDict_GetItemString(info, "name");
    PyObject *dependencies = PyDict_GetItemString(info, "dependencies");
    if (id == NULL || name == NULL || dependencies == NULL) {
        PyErr_SetString(PyExc_ValueError, "Incomplete mod resolver entry");
        PyErr_Print();
        return false;
    }
    const char *module_name = PyUnicode_AsUTF8(name);
    if (module_name == NULL) { PyErr_Print(); return false; }
    printf("    -> Loading `%s`\n", module_name);
    PyObject *mod = PyObject_CallMethod(framework, "load_mod", "OOO", id, name, dependencies);
    if (mod == NULL) {
        PyErr_Print();
        fprintf(stderr, "      [!] Error: Failed to load mod '%s'\n", module_name);
        return false;
    }
    Py_DECREF(mod);
    return true;
}

// Helper to get the base filename without extension from a path
void get_module_name_from_path(const char* filepath, char* module_name, size_t buffer_size) {
    const char* last_slash = strrchr(filepath, '/');
    const char* last_backslash = strrchr(filepath, '\\');
    const char* start_of_filename = filepath;

    if (last_slash && last_slash > start_of_filename) start_of_filename = last_slash + 1;
    if (last_backslash && last_backslash > start_of_filename) start_of_filename = last_backslash + 1;

    const char* dot = strrchr(start_of_filename, '.');
    if (dot) {
        size_t len = dot - start_of_filename;
        if (len < buffer_size) {
            strncpy(module_name, start_of_filename, len);
            module_name[len] = '\0';
        }
    } else {
        strncpy(module_name, start_of_filename, buffer_size - 1);
        module_name[buffer_size - 1] = '\0';
    }
}

#ifdef HL_VCC
// this allows some runtime detection to switch to high performance mode
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
#endif

bool fileExists(const char *path)
{
    FILE *fptr = fopen(path, "r");

    if (fptr == NULL)
        return false;

    fclose(fptr);

    return true;
}

typedef struct {
    PyObject_HEAD
    // No instance-specific data is needed for this simple proxy.
} ConsoleProxyObject;

// C implementation of the "write" method for our object.
static PyObject* ConsoleProxy_write(ConsoleProxyObject *self, PyObject *args) {
    PyObject* p_string;
    if (!PyArg_ParseTuple(args, "O", &p_string)) {
        return NULL;
    }

    if (!PyUnicode_Check(p_string)) {
        PyErr_SetString(PyExc_TypeError, "write() argument must be a string");
        return NULL;
    }

    Py_ssize_t size;
    const char *c_str = PyUnicode_AsUTF8AndSize(p_string, &size);
    if (c_str == NULL) {
        return NULL;
    }

    fwrite(c_str, 1, size, stdout);
    fflush(stdout);

    Py_ssize_t char_count = PyUnicode_GET_LENGTH(p_string);
    return PyLong_FromSsize_t(char_count);
}

static PyObject* ConsoleProxy_flush(ConsoleProxyObject *self, PyObject *args) {
    fflush(stdout);
    Py_RETURN_NONE;
}

static PyObject* ConsoleProxy_isatty(ConsoleProxyObject *self, PyObject *args) {
#ifdef HL_WIN
    if (_isatty(_fileno(stdout))) {
        Py_RETURN_TRUE;
    }
#else
    if (isatty(fileno(stdout))) {
        Py_RETURN_TRUE;
    }
#endif
    Py_RETURN_FALSE;
}

// Table of methods that our object will have.
static PyMethodDef ConsoleProxy_methods[] = {
    {"write",  (PyCFunction)ConsoleProxy_write,  METH_VARARGS, "Writes text to the C stdout."},
    {"flush",  (PyCFunction)ConsoleProxy_flush,  METH_NOARGS,  "Flushes the C stdout buffer."},
    {"isatty", (PyCFunction)ConsoleProxy_isatty, METH_NOARGS,  "Returns True if this is a TTY."},
    {NULL, NULL, 0, NULL} // Sentinel value
};

// The main type definition struct for our ConsoleProxyType.
static PyTypeObject ConsoleProxyType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "hlmod._ConsoleProxy",
    .tp_basicsize = sizeof(ConsoleProxyObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = PyType_GenericNew,
    .tp_doc = "A proxy object to forward Python I/O to the C stdout.",
    .tp_methods = ConsoleProxy_methods,
};

void hlmod_setup_pyio() {
#ifdef HL_WIN
    if (PyType_Ready(&ConsoleProxyType) < 0) {
        fprintf(stderr, "[hlmod] FATAL: Could not ready ConsoleProxyType.\n");
        PyErr_Print();
        return;
    }

    PyObject* proxy_instance = PyObject_CallObject((PyObject *)&ConsoleProxyType, NULL);
    if (proxy_instance == NULL) {
        fprintf(stderr, "[hlmod] FATAL: Could not create ConsoleProxy instance.\n");
        PyErr_Print();
        return;
    }

    PyObject* sys_module = PyImport_ImportModule("sys");
    if (sys_module) {
        PyObject_SetAttrString(sys_module, "stdout", proxy_instance);
        PyObject_SetAttrString(sys_module, "stderr", proxy_instance);
        Py_DECREF(sys_module);
    } else {
        fprintf(stderr, "[hlmod] FATAL: Could not import sys module.\n");
        PyErr_Print();
    }

    Py_DECREF(proxy_instance);
#endif
}

#ifdef HL_WIN
#if defined(HL_WIN_DESKTOP) && defined(HL_MINGW)
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PSTR lpCmdLine, INT nCmdShow) {
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
#else
int wmain(int argc, pchar *argv[]) {
#endif
#else
int main(int argc, pchar *argv[]) {
#endif
	if (PyImport_AppendInittab("hlmod", PyInit_hlmod) == -1) {
        fprintf(stderr, "Fatal Error: Could not add 'hlmod' to the built-in module table\n");
        return 1;
    }
	Py_InitializeEx(1);
    if (!Py_IsInitialized()) {
        fprintf(stderr, "Error: Could not initialize Python interpreter\n");
        return 1;
    }



// #   define HLMOD_STDOUT_HACK
#   ifdef HLMOD_STDOUT_HACK
    FILE* stderr_log_file = fopen("hlmod_pyerr.log", "w");
    FILE* stdout_log_file = fopen("hlmod_pyout.log", "w");
    if (stderr_log_file && stdout_log_file) {
        printf("[hlmod DEBUG] Python stderr/stdout redirecting to disk\n");
        PyObject* sys_module = PyImport_ImportModule("sys");
        if (sys_module) {
            PyObject* py_stderr_file = PyFile_FromFd(fileno(stderr_log_file), "hlmod_pyerr.log", "w", -1, NULL, NULL, NULL, 0);
            PyObject* py_stdout_file = PyFile_FromFd(fileno(stdout_log_file), "hlmod_pyout.log", "w", -1, NULL, NULL, NULL, 0);
            if (py_stderr_file && py_stdout_file) {
                PyObject_SetAttrString(sys_module, "stderr", py_stderr_file);
                PyObject_SetAttrString(sys_module, "stdout", py_stdout_file);
                Py_DECREF(py_stderr_file);
            } else {
                printf("[hlmod DEBUG] Something got messed up!\n");
            }
            Py_DECREF(sys_module);
        }
    }
#   else
    hlmod_setup_pyio();
#   endif


	static vclosure cl;
	pchar *file = NULL;
	char *error_msg = NULL;
	int debug_port = -1;
	bool debug_wait = false;
	bool hot_reload = false;
	int profile_count = -1;
	bool vtune_later = false;
	main_context ctx;
	bool isExc = false;
    bool sdk_only = false;
	int first_boot_arg = -1;
	argv++;
	argc--;

	while( argc ) {
		pchar *arg = *argv++;
		argc--;
		if( pcompare(arg,PSTR("--version")) == 0 ) {
			printf("%d.%d.%d (hlmod)\n",HL_VERSION>>16,(HL_VERSION>>8)&0xFF,HL_VERSION&0xFF);
			return 0;
		}
        if (pcompare(arg, PSTR("--generate-stubs")) == 0) {
            sdk_only = true;
            continue;
        }
		if( *arg == '-' || *arg == '+' ) {
			if( first_boot_arg < 0 ) first_boot_arg = argc + 1;
			// skip value
			if( argc && **argv != '+' && **argv != '-' ) {
				argc--;
				argv++;
			}
			continue;
		}
		file = arg;
		break;
	}
#define COPOUT printf("HL/JIT %d.%d.%d (c)2015-2025 Haxe Foundation. hlmod (c)2025 N3rdL0rd\n  Usage: hl [--generate-stubs] <file>\n",HL_VERSION>>16,(HL_VERSION>>8)&0xFF,HL_VERSION&0xFF);return 1;
	if( file == NULL ) {
        if (sdk_only) { COPOUT }
		FILE *fchk;
        if (fileExists("hlboot.dat")) {
		    file = PSTR("hlboot.dat");
        } else if (fileExists("deadcells_gl.exe")) {
            file = PSTR("deadcells_gl.exe"); // deadcells (and ONLY deadcells) bundles hlboot.dat with the main executable since it's MT's inhouse fork
        } else if (fileExists("deadcells.exe")) {
            file = PSTR("deadcells.exe"); // deadcells_gl.exe is the platform-neutral version of the game that hlmod has an easier time loading (directx is still fucked, somehow)
        } else {
            COPOUT
        }
#       ifdef HL_WIN
        pprintf("[hlmod] Defaulting to %s\n", file);
#       else
        printf("[hlmod] Defaulting to %s\n", file);
#       endif
		fchk = pfopen(file,"rb");
		if( fchk == NULL ) {
            COPOUT
		}
#undef COPOUT
		fclose(fchk);
		if( first_boot_arg >= 0 ) {
			argv -= first_boot_arg;
			argc = first_boot_arg;
		}
	}
    printf("[hlmod] HL init...\n");
	hl_global_init();
	hl_sys_init((void**)argv,argc,file);
	hl_register_thread(&ctx);
	ctx.file = file;
	ctx.code = load_code(file, &error_msg, true);
	if( ctx.code == NULL ) {
		if( error_msg ) printf("%s\n", error_msg);
		return 1;
	}

    check_deadcells();

#ifdef USE_HLMOD_CRASH
	hlmod_setup_handler();
#endif


    printf("[hlmod] Initializing HL module...\n");

	ctx.m = hl_module_alloc(ctx.code);
	if( ctx.m == NULL )
		return 2;
	if( !hl_module_init(ctx.m,hot_reload,vtune_later) )
		return 3;

    g_module = ctx.m;
    g_code = ctx.code;
    int exit_code = 1;
    PyObject *framework = NULL;
    if (hlmod_hook_registry_init(ctx.code->nfunctions + ctx.code->nnatives) < 0) {
        PyErr_Print();
        goto shutdown;
    }
#ifndef NO_STUBGEN
    if (!hlmod_generate_stubs(ctx.code)) goto shutdown;
#endif
    if (sdk_only) { exit_code = 0; goto shutdown; }

	printf("[hlmod] Finding mods...\n");
    const char* mods_directory = "./mods";

    PyObject* sys_path = PySys_GetObject("path");
    PyObject* mods_path_obj = PyUnicode_FromString(mods_directory);
    if (mods_path_obj == NULL || PyList_Append(sys_path, mods_path_obj) < 0) {
        Py_XDECREF(mods_path_obj);
        PyErr_Print();
        goto shutdown;
    }
    Py_DECREF(mods_path_obj);
    if (hlmod_python_init() < 0) {
        PyErr_Print();
        goto shutdown;
    }
    framework = PyImport_ImportModule("modcore");
    if (framework == NULL) {
        PyErr_Print();
        goto shutdown;
    }

    PyObject* load_order_list = NULL;
    if (get_mod_load_order(mods_directory, &load_order_list)) {
        Py_ssize_t mod_count = PyList_Size(load_order_list);
        printf("[hlmod] Found %zd mods.\n", mod_count);

        printf("[hlmod] Loading mods:\n");
        for (Py_ssize_t i = 0; i < mod_count; i++) {
            PyObject* mod_info_dict = PyList_GetItem(load_order_list, i);
            if (!load_mod(framework, mod_info_dict)) {
                Py_DECREF(load_order_list);
                goto shutdown;
            }
        }
        Py_DECREF(load_order_list);
    } else {
        fprintf(stderr, "[hlmod] Could not resolve mod load order. Halting.\n");
        goto shutdown;
    }
    PyObject *loaded = PyObject_CallMethod(framework, "finish_loading", NULL);
    if (loaded == NULL) { PyErr_Print(); goto shutdown; }
    Py_DECREF(loaded);
    printf("[hlmod] All mods initialized.\n\n");

	cl.t = ctx.code->functions[ctx.m->functions_indexes[ctx.m->code->entrypoint]].type;
	cl.fun = ctx.m->functions_ptrs[ctx.m->code->entrypoint];
	cl.hasValue = 0;
	hl_profile_setup(profile_count);

    // Release the GIL before calling into Haxe code, which might be blocking.
    PyThreadState* _save = PyEval_SaveThread();
	ctx.ret = hl_dyn_call_safe(&cl,NULL,0,&isExc);

	hl_profile_end();
    if (isExc) hl_print_uncaught_exception(ctx.ret);
    exit_code = isExc ? 1 : 0;

    // Re-acquire the GIL before finalizing Python.
    PyEval_RestoreThread(_save);
shutdown:
    if (framework != NULL) {
        PyObject *stopped = PyObject_CallMethod(framework, "shutdown", NULL);
        if (stopped == NULL) { PyErr_Print(); exit_code = 1; }
        Py_XDECREF(stopped);
        Py_CLEAR(framework);
    }
    hlmod_python_shutdown();
    hlmod_shutdown();
    if (Py_FinalizeEx() < 0) exit_code = 1;
    hlmod_python_dispose();
	hl_module_free(ctx.m);
    hl_code_free(ctx.code);
	hl_free(&ctx.code->alloc);
	// do not call hl_unregister_thread() or hl_global_free will display error
	// on global_lock if there are threads that are still running (such as debugger)
	hl_global_free();
    printf("[hlmod] Bye!\n");
    return exit_code;
}
