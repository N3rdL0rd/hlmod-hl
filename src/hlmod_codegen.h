
#ifndef HLMOD_CODEGEN_H
#define HLMOD_CODEGEN_H

#include <hl.h>
#include <hlmodule.h>

/**
 * @brief Extracts native metadata and renders Python proxies into
 * `<mods_dir>/stubs`; false on failure.
 */
bool hlmod_generate_stubs(hl_code *code, const char *mods_dir);

#endif // HLMOD_CODEGEN_H

  