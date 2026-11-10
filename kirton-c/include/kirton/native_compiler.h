/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include <stddef.h>
#include "kirton/ast.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Emit LLVM IR for the deliberately small Native 0.1 subset.
   On success, *out_ir is a malloc-owned UTF-8 string. */
int kirton_llvm_emit(AstProgram* program, char** out_ir,
                     char* error, size_t error_capacity);
int kirton_llvm_emit_ex(AstProgram* program, char** out_ir,
                        int* out_requires_runtime,
                        char* error, size_t error_capacity);
/* Opt-in FFI is scoped to the compiled program and disabled by default. */
int kirton_llvm_emit_options(AstProgram* program, char** out_ir,
                             int* out_requires_runtime, int allow_ffi,
                             char* error, size_t error_capacity);

/* Invoke the configured Clang driver on an LLVM IR file. */
int kirton_native_compile(const char* llvm_path, const char* output_path,
                          char* error, size_t error_capacity);
int kirton_native_compile_ex(const char* llvm_path, const char* output_path,
                             int link_runtime,
                             char* error, size_t error_capacity);

#ifdef __cplusplus
}
#endif
