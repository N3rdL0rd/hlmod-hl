
#ifndef HLMOD_CODEGEN_H
#define HLMOD_CODEGEN_H

#include <hl.h>
#include <hlmodule.h>

/**
 * @brief Generates Python class stubs for all Objs in the bytecode.
 */
void hlmod_generate_stubs(hl_code *code);

#endif // HLMOD_CODEGEN_H

  