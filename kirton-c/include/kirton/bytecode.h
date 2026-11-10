/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "kirton/ast.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Experimental in-memory compiler/VM. Does not serialize bytecode. */
typedef struct {
    size_t collections;
    size_t reclaimed_objects;
    size_t peak_objects;
    size_t live_objects_at_end;
    double compile_ms;
    double execute_ms;
} KirtonBytecodeStats;
int kirton_bytecode_run(AstProgram* program, const char* entry);
int kirton_bytecode_run_with_stats(AstProgram* program, const char* entry,
                                   KirtonBytecodeStats* stats);
/* Runs Bytecode with the same explicit trusted-library opt-in as AST. */
int kirton_bytecode_run_with_ffi(AstProgram* program, const char* entry,
                                 int allow_ffi);

#ifdef __cplusplus
}
#endif
