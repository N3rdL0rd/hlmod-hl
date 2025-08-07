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
#include <hl.h>
#include <hlmodule.h>
#include "hlsystem.h"

#include <Python.h>
#include <hlmod_hooks.h>

#ifdef HL_WIN
#	include <locale.h>
typedef uchar pchar;
#define pprintf(str,file)	uprintf(USTR(str),file)
#define pfopen(file,ext) _wfopen(file,USTR(ext))
#define pcompare wcscmp
#define ptoi(s)	wcstol(s,NULL,10)
#define PSTR(x) USTR(x)
#else
#	include <sys/stat.h>
typedef char pchar;
#define pprintf printf
#define pfopen fopen
#define pcompare strcmp
#define ptoi atoi
#define PSTR(x) x
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
	code = hl_code_read((unsigned char*)fdata, size, error_msg);
	free(fdata);
	return code;
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

HookRegistryEntry* g_hook_registry = NULL;

void hlmod_register_hook(int findex, PyObject* callback) {
    HookRegistryEntry* entry;
    HASH_FIND_INT(g_hook_registry, &findex, entry);
    if (entry == NULL) {
        entry = (HookRegistryEntry*)malloc(sizeof(HookRegistryEntry));
        entry->findex = findex;
        HASH_ADD_INT(g_hook_registry, findex, entry);
    } else {
        Py_DECREF(entry->callback);
    }
    Py_INCREF(callback);
    entry->callback = callback;
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

    Py_RETURN_NONE;
}

static PyMethodDef HlmodMethods[] = {
    // { Python-visible Name, C Function Pointer, Argument Type, Docstring }
    {"register_hook", hlmod_py_register_hook, METH_VARARGS, "Hooks a function by its findex."},
    
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
    return PyModule_Create(&hlmod_module_def);
}


const char *g_sorter_script =
    "import os\n"
    "import ast\n"
    "from collections import deque\n"
    "\n"
    "def get_mod_info(filepath):\n"
    "    '''Safely parses a Python file to get its MOD_INFO dict without executing it.'''\n"
    "    with open(filepath, 'r', encoding='utf-8') as f:\n"
    "        tree = ast.parse(f.read(), filename=filepath)\n"
    "    for node in tree.body:\n"
    "        if isinstance(node, ast.Assign):\n"
    "            if len(node.targets) == 1 and isinstance(node.targets[0], ast.Name) and node.targets[0].id == 'MOD_INFO':\n"
    "                return ast.literal_eval(node.value)\n"
    "    return None\n"
    "\n"
    "def resolve_mod_order(mods_dir):\n"
    "    '''Discovers mods, builds a dependency graph, and performs a topological sort.'''\n"
    "    mods = {}\n"
    "    for filename in os.listdir(mods_dir):\n"
    "        if filename.endswith('.py') and not filename.startswith('__'):\n"
    "            filepath = os.path.join(mods_dir, filename)\n"
    "            info = get_mod_info(filepath)\n"
    "            if info and 'id' in info and 'dependencies' in info:\n"
    "                mods[info['id']] = {\n"
    "                    'info': info,\n"
    "                    'filepath': filepath,\n"
    "                    'dependencies': set(info['dependencies'])\n"
    "                }\n"
    "\n"
    "    # --- TOPOLOGICAL SORT (Kahn's Algorithm) --- \n"
    "    # Create a reverse graph to find who depends on whom\n"
    "    reverse_graph = {mod_id: set() for mod_id in mods}\n"
    "    in_degree = {}\n"
    "\n"
    "    for mod_id, data in mods.items():\n"
    "        # The in-degree is simply the number of dependencies a mod has.\n"
    "        in_degree[mod_id] = len(data['dependencies'])\n"
    "        for dep_id in data['dependencies']:\n"
    "            if dep_id not in mods:\n"
    "                return {'status': 'error', 'message': f'Mod \\'{mod_id}\\' has an unmet dependency: \\'{dep_id}\\' '}\n"
    "            # Add an edge from the dependency to the current mod\n"
    "            reverse_graph[dep_id].add(mod_id)\n"
    "\n"
    "    # Queue of all nodes with no dependencies (in-degree of 0)\n"
    "    queue = deque([mod_id for mod_id, degree in in_degree.items() if degree == 0])\n"
    "    \n"
    "    sorted_order = []\n"
    "    while queue:\n"
    "        mod_id = queue.popleft()\n"
    "        sorted_order.append({'id': mod_id, 'filepath': mods[mod_id]['filepath']})\n"
    "        \n"
    "        # For each mod that depended on the one we just processed...\n"
    "        for dependent_mod_id in reverse_graph[mod_id]:\n"
    "            # ...decrement its in-degree.\n"
    "            in_degree[dependent_mod_id] -= 1\n"
    "            if in_degree[dependent_mod_id] == 0:\n"
    "                queue.append(dependent_mod_id)\n"
    "\n"
    "    if len(sorted_order) == len(mods):\n"
    "        return {'status': 'ok', 'order': sorted_order}\n"
    "    else:\n"
    "        cycle_nodes = set(mods.keys()) - {item['id'] for item in sorted_order}\n"
    "        return {'status': 'error', 'message': f'Circular dependency detected among mods: {list(cycle_nodes)}'}\n";


/**
 * @brief Calls a Python function from a script string to determine the mod load order.
 *
 * @param mods_dir The directory where the .py mod files are located.
 * @param load_order_list A pointer to a PyObject* that will receive the list of mods to load.
 * @return 1 on success (and load_order_list is populated), 0 on failure.
 * The caller is responsible for DECREF'ing the returned list.
 */
int get_mod_load_order(const char *mods_dir, PyObject **load_order_list) {
    PyObject *pName, *pModule, *pModuleDict, *pFunc, *pArgs, *pValue, *pResultObj, *pStatus, *pOrder;
    int success = 0;

    // 1. Create a new, temporary module to hold our sorter code.
    pName = PyUnicode_FromString("mod_sorter_module");
    pModule = PyImport_AddModuleObject(pName);
    Py_DECREF(pName);
    if (pModule == NULL) {
        fprintf(stderr, "[hlmod] Error: Failed to create a temporary Python module.\n");
        PyErr_Print();
        return 0;
    }
    pModuleDict = PyModule_GetDict(pModule);

    // 2. Execute our script string within the new module's namespace to define the functions.
    pValue = PyRun_String(g_sorter_script, Py_file_input, pModuleDict, pModuleDict);
    if (pValue == NULL) {
        fprintf(stderr, "Error: An exception occurred while defining Python mod resolver functions.\n");
        PyErr_Print();
        Py_DECREF(pModule);
        return 0;
    }
    Py_DECREF(pValue);

    // 3. Get a handle to the specific function we want to call.
    pFunc = PyObject_GetAttrString(pModule, "resolve_mod_order");
    if (pFunc == NULL || !PyCallable_Check(pFunc)) {
        if (PyErr_Occurred()) PyErr_Print();
        fprintf(stderr, "[hlmod] Error: Cannot find callable function 'resolve_mod_order'.\n");
        Py_DECREF(pModule);
        return 0;
    }

    // 4. Build the arguments tuple for the Python function call. It takes one argument.
    pArgs = PyTuple_New(1);
    pValue = PyUnicode_FromString(mods_dir);
    if (!pValue) {
        PyErr_Print();
        Py_DECREF(pArgs);
        Py_DECREF(pFunc);
        Py_DECREF(pModule);
        return 0;
    }
    PyTuple_SetItem(pArgs, 0, pValue); // PyTuple_SetItem steals a reference to pValue

    // 5. Call the function and get the result.
    pResultObj = PyObject_CallObject(pFunc, pArgs);
    Py_DECREF(pArgs); // We are done with the arguments tuple.

    // Cleanup the function and module references now that the call is made.
    Py_DECREF(pFunc);
    Py_DECREF(pModule);

    // 6. Process the dictionary that was returned from the Python function.
    if (pResultObj != NULL) {
        pStatus = PyDict_GetItemString(pResultObj, "status");
        const char* status_str = PyUnicode_AsUTF8(pStatus);
        
        if (strcmp(status_str, "ok") == 0) {
            pOrder = PyDict_GetItemString(pResultObj, "order");
            if (pOrder && PyList_Check(pOrder)) {
                Py_INCREF(pOrder); // Pass ownership of this new reference to the caller
                *load_order_list = pOrder;
                success = 1;
            }
        } else {
            PyObject *pMessage = PyDict_GetItemString(pResultObj, "message");
            fprintf(stderr, "[hlmod] Error: %s\n", PyUnicode_AsUTF8(pMessage));
        }
        Py_DECREF(pResultObj); // We are done with the result dictionary.
    } else {
        fprintf(stderr, "[hlmod] Error: Python function call failed.\n");
        PyErr_Print();
    }

    return success;
}

/**
 * @brief Imports a Python module by name and calls its initialize() function.
 * @param module_name The name of the module to import (e.g., "core_api").
 */
void load_mod(const char* module_name) {
    PyObject *pName, *pModule, *pFunc, *pValue;
    printf("    -> Loading mod: %s\n", module_name);

    pName = PyUnicode_FromString(module_name);
    pModule = PyImport_Import(pName);
    Py_DECREF(pName);

    if (pModule != NULL) {
        pFunc = PyObject_GetAttrString(pModule, "initialize");
        if (pFunc && PyCallable_Check(pFunc)) {
            pValue = PyObject_CallObject(pFunc, NULL);
            if (pValue == NULL) {
                fprintf(stderr, "      [!] Error calling initialize() in mod '%s'\n", module_name);
                PyErr_Print();
            }
            Py_XDECREF(pValue);
        } else {
            if (PyErr_Occurred()) PyErr_Print();
            fprintf(stderr, "      [!] Error: Cannot find function 'initialize' in mod '%s'\n", module_name);
        }
        Py_XDECREF(pFunc);
        Py_DECREF(pModule);
    } else {
        PyErr_Print();
        fprintf(stderr, "      [!] Error: Failed to load mod '%s'\n", module_name);
    }
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

#if defined(HL_LINUX) || defined(HL_MAC)
#include <signal.h>
static void handle_signal( int signum ) {
	signal(signum, SIG_DFL);
	printf("SIGNAL %d\n",signum);
	if( hl_get_thread() != NULL ) {
		hl_dump_stack();
	}
	fflush(stdout);
	raise(signum);
}
static void setup_handler() {
	struct sigaction act;
	act.sa_sigaction = NULL;
	act.sa_handler = handle_signal;
	act.sa_flags = 0;
	sigemptyset(&act.sa_mask);
	signal(SIGPIPE, SIG_IGN);
	sigaction(SIGSEGV,&act,NULL);
	sigaction(SIGTERM,&act,NULL);
}
#else
static void setup_handler() {
}
#endif

#ifdef HL_WIN
int wmain(int argc, pchar *argv[]) {
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
	int first_boot_arg = -1;
	argv++;
	argc--;

	while( argc ) {
		pchar *arg = *argv++;
		argc--;
		if( pcompare(arg,PSTR("--version")) == 0 ) {
			printf("%d.%d.%d (hlmod)",HL_VERSION>>16,(HL_VERSION>>8)&0xFF,HL_VERSION&0xFF);
			return 0;
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
	if( file == NULL ) {
		FILE *fchk;
		file = PSTR("hlboot.dat");
		fchk = pfopen(file,"rb");
		if( fchk == NULL ) {
			printf("HL/JIT %d.%d.%d (c)2015-2025 Haxe Foundation. hlmod (c)2025 N3rdL0rd\n  Usage: hl <file>\n",HL_VERSION>>16,(HL_VERSION>>8)&0xFF,HL_VERSION&0xFF);
			return 1;
		}
		fclose(fchk);
		if( first_boot_arg >= 0 ) {
			argv -= first_boot_arg;
			argc = first_boot_arg;
		}
	}
	hl_global_init();
	hl_sys_init((void**)argv,argc,file);
	hl_register_thread(&ctx);
	ctx.file = file;
	ctx.code = load_code(file, &error_msg, true);
	if( ctx.code == NULL ) {
		if( error_msg ) printf("%s\n", error_msg);
		return 1;
	}
	ctx.m = hl_module_alloc(ctx.code);
	if( ctx.m == NULL )
		return 2;
	if( !hl_module_init(ctx.m,hot_reload,vtune_later) )
		return 3;
	if( hot_reload ) {
		ctx.file_time = pfiletime(ctx.file);
		hl_setup_reload_check(check_reload,&ctx);
	}
	hl_code_free(ctx.code);
	if( debug_port > 0 && !hl_module_debug(ctx.m,debug_port,debug_wait) ) {
		fprintf(stderr,"Could not start debugger on port %d\n",debug_port);
		return 4;
	}

	printf("[hlmod] Initializing mods...\n");
    const char* mods_directory = "./mods";

    PyObject* sys_path = PySys_GetObject("path");
    PyObject* mods_path_obj = PyUnicode_FromString(mods_directory);
    PyList_Append(sys_path, mods_path_obj);
    Py_DECREF(mods_path_obj);

    PyObject* load_order_list = NULL;
    if (get_mod_load_order(mods_directory, &load_order_list)) {
        printf("[hlmod] Determined load order:\n");
        Py_ssize_t mod_count = PyList_Size(load_order_list);
        for (Py_ssize_t i = 0; i < mod_count; i++) {
            PyObject* mod_info_dict = PyList_GetItem(load_order_list, i);
            PyObject* mod_id_obj = PyDict_GetItemString(mod_info_dict, "id");
            printf("  %ld. %s\n", (long)i + 1, PyUnicode_AsUTF8(mod_id_obj));
        }

        printf("[hlmod] Executing mod initializers...\n");
        for (Py_ssize_t i = 0; i < mod_count; i++) {
            PyObject* mod_info_dict = PyList_GetItem(load_order_list, i);
            PyObject* filepath_obj = PyDict_GetItemString(mod_info_dict, "filepath");
            const char* filepath_str = PyUnicode_AsUTF8(filepath_obj);
            
            char module_name[256];
            get_module_name_from_path(filepath_str, module_name, sizeof(module_name));
            
            load_mod(module_name);
        }
        Py_DECREF(load_order_list);
    } else {
        fprintf(stderr, "[hlmod] Could not resolve mod load order. Halting.\n");
        Py_FinalizeEx();
        return 1;
    }
    printf("[hlmod] All mods initialized.\n\n");

	cl.t = ctx.code->functions[ctx.m->functions_indexes[ctx.m->code->entrypoint]].type;
	cl.fun = ctx.m->functions_ptrs[ctx.m->code->entrypoint];
	cl.hasValue = 0;
	setup_handler();
	hl_profile_setup(profile_count);
	ctx.ret = hl_dyn_call_safe(&cl,NULL,0,&isExc);

	hl_profile_end();
	if( isExc ) {
		hl_print_uncaught_exception(ctx.ret);
		hl_debug_break();
		hl_global_free();
		return 1;
	}
	hl_module_free(ctx.m);
	hl_free(&ctx.code->alloc);
	// do not call hl_unregister_thread() or hl_global_free will display error
	// on global_lock if there are threads that are still running (such as debugger)
	hl_global_free();
	return 0;
}

