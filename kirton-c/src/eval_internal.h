/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "kirton/value.h"

/* Shared AST/bytecode structural equality, including cyclic collections. */
bool eval_values_equal(Value left, Value right);
char* kir_read_text_file(const char* path, size_t* out_len);
int kir_resolve_module_path(const char* name, const char* from_dir,
                            char* out_path, size_t out_size);
void kir_normalize_module_key(const char* path, char* out, size_t out_size);
void kir_module_name_from_path(const char* path, char* out, size_t out_size);
