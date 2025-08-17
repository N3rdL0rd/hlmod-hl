#ifndef STD_GLOBALS_H
#define STD_GLOBALS_H

#include <hl.h>

#ifdef _WIN32
    #ifdef LIBHL_EXPORTS
        #define STD_API __declspec(dllexport)
    #else
        #define STD_API __declspec(dllimport)
    #endif
#else
    #define STD_API
#endif

extern STD_API bool g_fixed_prng;

#endif // STD_GLOBALS_H