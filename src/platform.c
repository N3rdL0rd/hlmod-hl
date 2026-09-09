#include "platform.h"

#ifndef HL_WIN
#   include <unistd.h>
#   include <libgen.h>
#   include <string.h>
#   include <limits.h>
#endif

#include <stdio.h>

int pfiletime( pchar *file ) {
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

void get_exe_dir(pchar* buffer, int buffer_size) {
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

bool fileExists(const char *path)
{
    FILE *fptr = fopen(path, "r");

    if (fptr == NULL)
        return false;

    fclose(fptr);

    return true;
}
