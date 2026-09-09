#ifndef HLMOD_PLATFORM_H
#define HLMOD_PLATFORM_H

/* Cross-platform shims for the narrow set of OS-level operations hlmod's
 * entrypoint needs: wide vs. narrow path/argv handling, file mtimes, and
 * locating the running executable's directory. */

#include <stdbool.h>

/* Defines HL_WIN (and `uchar`) via platform detection - must come before any
 * `#ifdef HL_WIN` below. Without this, a translation unit that includes
 * platform.h before hl.h would see HL_WIN as undefined and silently take the
 * non-Windows branch even when actually compiling for Windows: invisible on
 * Linux (the non-Windows branch is correct there anyway) but a hard failure
 * on Windows (missing unistd.h, wrong path/string macros). */
#include <hl.h>

#ifdef HL_WIN
#   include <locale.h>
#   include <direct.h>
#   define MKDIR(path) _mkdir(path)
#   define pprintf(str,file)	uprintf(USTR(str),file)
#   define pfopen(file,ext) _wfopen(file,USTR(ext))
#   define pcompare wcscmp
#   define ptoi(s)	wcstol(s,NULL,10)
#   define PSTR(x) USTR(x)
/* hl.h's own _GUID macro (a native type-signature string constant) collides
 * textually with windows.h's `typedef struct _GUID { ... } GUID;` - the same
 * workaround already used in gc.c/module.c/random.c/sys.c/native_hook.c. */
#   undef _GUID
#   include <windows.h>
#   include <fcntl.h>
#   include <stdio.h>
#   include <io.h>
typedef uchar pchar;
#else
#   include <sys/stat.h>
#   include <errno.h>
#   define MKDIR(path) mkdir(path, 0755)
#   define pprintf printf
#   define pfopen fopen
#   define pcompare strcmp
#   define ptoi atoi
#   define PSTR(x) x
typedef char pchar;
#endif

/* File modification time, in seconds since the epoch. */
int pfiletime(pchar *file);

/* Writes the directory containing the running executable (trailing slash
 * included) into `buffer`, or an empty string on failure. */
void get_exe_dir(pchar *buffer, int buffer_size);

bool fileExists(const char *path);

#endif // HLMOD_PLATFORM_H
