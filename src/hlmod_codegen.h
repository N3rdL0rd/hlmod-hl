
#ifndef HLMOD_CODEGEN_H
#define HLMOD_CODEGEN_H

#include <hl.h>
#include <hlmodule.h>

/**
 * @brief Extracts native metadata and renders Python proxies; false on failure.
 */
bool hlmod_generate_stubs(hl_code *code);

#endif // HLMOD_CODEGEN_H

  