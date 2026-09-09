#ifndef HLMOD_PLATFORM_H
#define HLMOD_PLATFORM_H

/* Cross-platform shims for the narrow set of OS-level operations hlmod's
 * entrypoint needs: wide vs. narrow path/argv handling, file mtimes, and
 * locating the running executable's directory. */

#include <stdbool.h>

#ifdef HL_WIN
#   include <locale.h>
#   include <direct.h>
#   define MKDIR(path) _mkdir(path)
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
