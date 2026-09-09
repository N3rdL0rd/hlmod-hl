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
#include <hlmod_python.h>

#include "platform.h"
#include "bytecode_loader.h"
#include "game_profiles.h"
#include "mod_loader.h"
#include "console_proxy.h"
#include "py_module.h"

#ifdef USE_HLMOD_CRASH
#include <hlmod_crash.h>
#endif

hl_code *g_code = NULL;

typedef struct {
	pchar *file;
	hl_code *code;
	hl_module *m;
	vdynamic *ret;
	int file_time;
} main_context;


#ifdef HL_VCC
// this allows some runtime detection to switch to high performance mode
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
#endif


#ifdef HL_WIN
#if defined(HL_WIN_DESKTOP) && defined(HL_MINGW)
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR lpCmdLine, INT nCmdShow) {
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
#else
int wmain(int argc, pchar *argv[]) {
#endif
#else
int main(int argc, pchar *argv[]) {
#endif
#ifdef HL_WIN_DESKTOP
	/* hl_sys_print's Windows path only transcodes UTF-16 -> console UTF-8
	 * when print_flags includes PR_WIN_UTF8 (bit 0), which upstream
	 * HashLink leaves off by default. Without it, every Sys.print/println
	 * writes raw UTF-16LE bytes into a byte-mode stream (each ASCII
	 * character followed by a stray NUL), which most terminals render as
	 * if nothing were wrong but corrupts anything that captures stdout
	 * verbatim (redirection, pipes, subprocess capture). Enable it here so
	 * hlmod's own PR_AUTO_FLUSH default is preserved alongside a working
	 * console encoding. */
	extern int hl_sys_set_flags(int flags);
	hl_sys_set_flags(1 /* PR_WIN_UTF8 */ | 2 /* PR_AUTO_FLUSH */);
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
        } else {
            const pchar *game_boot_file = hlmod_game_profile_find_boot_file();
            if (game_boot_file != NULL) {
                file = (pchar *)game_boot_file;
            } else {
                COPOUT
            }
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

    hlmod_game_profile_apply_compat(g_code_sha256);

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
