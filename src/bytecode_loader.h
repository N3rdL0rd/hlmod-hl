#ifndef HLMOD_BYTECODE_LOADER_H
#define HLMOD_BYTECODE_LOADER_H

#include <hl.h>
#include <hlmodule.h>
#include "platform.h"

/* Reads and hashes a HashLink bytecode file, then parses it. Returns NULL on
 * any I/O or parse failure, optionally printing a message when
 * `print_errors` is set. Populates `g_code_sha256` (declared in hlmod.h) as
 * a side effect. */
hl_code *load_code( const pchar *file, char **error_msg, bool print_errors );

#endif // HLMOD_BYTECODE_LOADER_H
