#include "hlsystem.h"
#include <stdio.h>

#if defined(HL_LINUX) || defined(HL_MAC)

#include <execinfo.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <dlfcn.h>

static void hlmod_handler(int signum) {
    signal(signum, SIG_DFL);
    fprintf(stderr, "\n--------------------------- SIGNAL %d ---------------------------\n", signum);
    fprintf(stderr, "        This traceback brought to you by hlmod. Good luck!\n\n");

    void *buffer[100];
    int nptrs = backtrace(buffer, 100);

    fprintf(stderr, "Native stacktrace:\n\n");
    char **strings = backtrace_symbols(buffer, nptrs);
    if (strings == NULL) {
        fprintf(stderr, "Could not get backtrace symbols.\n");
    }

    for (int j = 0; j < nptrs; j++) {
        if (strings) {
            fprintf(stderr, "  %s\n", strings[j]);
        } else {
            fprintf(stderr, "  #%d %p\n", j, buffer[j]);
        }

        Dl_info info;
        if (dladdr(buffer[j], &info) && info.dli_fname) {
            const char *module_path = info.dli_fname;
            
            void* relative_addr = (void*)((char*)buffer[j] - (char*)info.dli_fbase);

            char command[2048];
            snprintf(command, sizeof(command), "addr2line -e %s -f -p -i %p", module_path, relative_addr);

            FILE* fp = popen(command, "r");
            if (fp) {
                char output_buffer[1024];
                while (fgets(output_buffer, sizeof(output_buffer), fp) != NULL) {
                    if (strstr(output_buffer, "??:0") == NULL && strstr(output_buffer, "?? ??:0") == NULL) {
                       fprintf(stderr, "    -> %s", output_buffer);
                    }
                }
                pclose(fp);
            }
        }
    }
    
    
    if (strings) {
        free(strings);
    }
    
	if( hl_get_thread() != NULL ) {
        fprintf(stderr, "\nHL stack:\n\n");
		hl_dump_stack();
	} else {
        fprintf(stderr, "No HL stack to dump. Wow, you must have really broken something!\n");
    }
	fflush(stderr);
    raise(signum);
}

void hlmod_setup_handler() {
    struct sigaction act;
    memset(&act, 0, sizeof(struct sigaction));
    act.sa_handler = hlmod_handler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_RESTART;

    signal(SIGPIPE, SIG_IGN);
    sigaction(SIGSEGV, &act, NULL);
    sigaction(SIGTERM, &act, NULL);
    sigaction(SIGABRT, &act, NULL);
}

#elif defined(HL_WIN)

#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

static LONG WINAPI unhandled_exception_filter(struct _EXCEPTION_POINTERS *ExceptionInfo) {
    HANDLE process = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();
    
    SymInitialize(process, NULL, TRUE);
    
    CONTEXT context = *ExceptionInfo->ContextRecord;
    
    STACKFRAME64 stack;
    memset(&stack, 0, sizeof(STACKFRAME64));
    
#ifdef _M_IX86
    DWORD machineType = IMAGE_FILE_MACHINE_I386;
    stack.AddrPC.Offset = context.Eip;
    stack.AddrFrame.Offset = context.Ebp;
    stack.AddrStack.Offset = context.Esp;
#elif _M_X64
    DWORD machineType = IMAGE_FILE_MACHINE_AMD64;
    stack.AddrPC.Offset = context.Rip;
    stack.AddrFrame.Offset = context.Rbp;
    stack.AddrStack.Offset = context.Rsp;
#else
    #error "Unsupported architecture"
#endif
    stack.AddrPC.Mode = AddrModeFlat;
    stack.AddrFrame.Mode = AddrModeFlat;
    stack.AddrStack.Mode = AddrModeFlat;

    fprintf(stderr, "Unhandled exception. Stack trace:\n");
    
    unsigned char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
    PSYMBOL_INFO symbol = (PSYMBOL_INFO)buffer;
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;

    IMAGEHLP_LINE64 line;
    line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
    
    while (StackWalk64(machineType, process, thread, &stack, &context, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL)) {
        DWORD64 displacement = 0;
        if (SymFromAddr(process, stack.AddrPC.Offset, &displacement, symbol)) {
            fprintf(stderr, "at %s ", symbol->Name);
            
            DWORD displacement_line = 0;
            if (SymGetLineFromAddr64(process, stack.AddrPC.Offset, &displacement_line, &line)) {
                fprintf(stderr, "in %s: line %lu\n", line.FileName, line.LineNumber);
            } else {
                fprintf(stderr, "(no line information)\n");
            }
        } else {
             fprintf(stderr, "at 0x%llX (unknown function)\n", stack.AddrPC.Offset);
        }
    }

    SymCleanup(process);

	if( hl_get_thread() != NULL ) {
		hl_dump_stack();
	}
	fflush(stderr);
    
    return EXCEPTION_EXECUTE_HANDLER;
}


void hlmod_setup_handler() {
    SetUnhandledExceptionFilter(unhandled_exception_filter);
}

#else

void hlmod_setup_handler() {
    // nop
}

#endif