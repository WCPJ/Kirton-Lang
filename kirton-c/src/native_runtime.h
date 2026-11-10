/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct KirtonNativeRuntime KirtonNativeRuntime;
typedef struct KirtonNativeFrame KirtonNativeFrame;
typedef struct KirtonNativeValue KirtonNativeValue;

typedef KirtonNativeValue* (*KirtonNativeCompiledFn)(
    KirtonNativeRuntime* runtime, const KirtonNativeValue* function,
    const KirtonNativeValue* const* arguments);
/* Compiled callbacks are non-capturing functions. The function identity is
   owned by the Runtime; no source interpreter is linked into the call path. */
void kirton_native_function_register(KirtonNativeRuntime* runtime,size_t id,
    size_t argc,KirtonNativeCompiledFn compiled);
KirtonNativeValue* kirton_native_function_new(KirtonNativeRuntime* runtime,size_t id);
KirtonNativeValue* kirton_native_function_new_captured(KirtonNativeRuntime* runtime,
    size_t id, const KirtonNativeValue* const* cells,size_t count);
KirtonNativeValue* kirton_native_cell_new(KirtonNativeRuntime* runtime);
KirtonNativeValue* kirton_native_cell_get(KirtonNativeRuntime* runtime,
    const KirtonNativeValue* cell);
void kirton_native_cell_set(KirtonNativeRuntime* runtime,
    const KirtonNativeValue* cell,const KirtonNativeValue* value);
KirtonNativeValue* kirton_native_capture_cell(KirtonNativeRuntime* runtime,
    const KirtonNativeValue* function,size_t index);
KirtonNativeValue* kirton_native_function_call(KirtonNativeRuntime* runtime,
    const KirtonNativeValue* function,
    const KirtonNativeValue* const* arguments,size_t argc);
KirtonNativeRuntime* kirton_native_runtime_new(void);
void kirton_native_ffi_enable(KirtonNativeRuntime* runtime);
KirtonNativeValue* kirton_native_memory_call(KirtonNativeRuntime* runtime,
    const char* method,size_t method_length,
    const KirtonNativeValue* const* args,size_t argc);
KirtonNativeValue* kirton_native_ffi_invoke(KirtonNativeRuntime* runtime,
    int method,const KirtonNativeValue* const* args,size_t argc);
int kirton_native_runtime_destroy(KirtonNativeRuntime* runtime);
void kirton_native_runtime_abort(KirtonNativeRuntime* runtime, int status);
/* Structured native error handling; no AST execution in generated binaries. */
void kirton_native_try_enter(KirtonNativeRuntime* runtime);
void kirton_native_try_leave(KirtonNativeRuntime* runtime);
int kirton_native_error_pending(KirtonNativeRuntime* runtime);
int kirton_native_error_suspend(KirtonNativeRuntime* runtime);
void kirton_native_error_resume(KirtonNativeRuntime* runtime,int prior);
KirtonNativeValue* kirton_native_error_take(KirtonNativeRuntime* runtime);

/* Structured native error handling; no AST execution in generated binaries. */
void kirton_native_try_enter(KirtonNativeRuntime* runtime);
void kirton_native_try_leave(KirtonNativeRuntime* runtime);
int kirton_native_error_pending(KirtonNativeRuntime* runtime);
int kirton_native_error_suspend(KirtonNativeRuntime* runtime);
void kirton_native_error_resume(KirtonNativeRuntime* runtime,int prior);
KirtonNativeValue* kirton_native_error_take(KirtonNativeRuntime* runtime);

KirtonNativeFrame* kirton_native_frame_enter(KirtonNativeRuntime* runtime);
void kirton_native_frame_leave(KirtonNativeRuntime* runtime,
                               KirtonNativeFrame* frame);
KirtonNativeValue* kirton_native_frame_return(KirtonNativeRuntime* runtime,
                                              KirtonNativeFrame* frame,
                                              const KirtonNativeValue* value);
KirtonNativeValue* kirton_native_root_value(KirtonNativeRuntime* runtime,
                                            const KirtonNativeValue* value);
KirtonNativeValue* kirton_native_string(KirtonNativeRuntime* runtime,
                                        const char* utf8, size_t length);
KirtonNativeValue* kirton_native_number(KirtonNativeRuntime* runtime,
                                        const char* text, size_t length,
                                        int is_integer);
KirtonNativeValue* kirton_native_nil(KirtonNativeRuntime* runtime);
KirtonNativeValue* kirton_native_i64(KirtonNativeRuntime* runtime,int64_t value);
KirtonNativeValue* kirton_native_bool(KirtonNativeRuntime* runtime,int64_t value);
KirtonNativeValue* kirton_native_list_new(KirtonNativeRuntime* runtime);
KirtonNativeValue* kirton_native_dict_new(KirtonNativeRuntime* runtime);
/* AST user types are copied into runtime-owned descriptors, never shared with
   the compiler process. IDs are stable positions in the source type table. */
void kirton_native_type_define(KirtonNativeRuntime* runtime,size_t id,int kind,
                               const char* name,size_t name_len,size_t fields);
void kirton_native_type_field(KirtonNativeRuntime* runtime,size_t id,size_t index,
                              const char* name,size_t name_len,
                              const char* type,size_t type_len,
                              size_t type_id_plus_one);
KirtonNativeValue* kirton_native_struct_construct(KirtonNativeRuntime* runtime,
    size_t id,const char* const* field_names,const size_t* field_lens,
    const KirtonNativeValue* const* values,size_t count);
KirtonNativeValue* kirton_native_enum_constant(KirtonNativeRuntime* runtime,
    size_t id,const char* variant,size_t length);
KirtonNativeValue* kirton_native_attr_get(KirtonNativeRuntime* runtime,
    const KirtonNativeValue* object,const char* name,size_t length);
void kirton_native_attr_set(KirtonNativeRuntime* runtime,
    const KirtonNativeValue* object,const char* name,size_t length,
    const KirtonNativeValue* value);
void kirton_native_list_append(KirtonNativeRuntime* runtime,
                               const KirtonNativeValue* collection,
                               const KirtonNativeValue* item);
void kirton_native_dict_put_literal(KirtonNativeRuntime* runtime,
                                    const KirtonNativeValue* collection,
                                    const char* key, size_t length,
                                    const KirtonNativeValue* item);
/* Advances an iterator over the current list length or UTF-8 string.
   NULL denotes exhaustion; any non-NULL value is a GC-pinned item. */
KirtonNativeValue* kirton_native_collection_builtin(KirtonNativeRuntime* runtime,
                                                   int builtin_id,
                                                   const KirtonNativeValue* const* args,
                                                   size_t argc);
KirtonNativeValue* kirton_native_iter_next(KirtonNativeRuntime* runtime,
                                           const KirtonNativeValue* sequence,
                                           size_t* cursor);
KirtonNativeValue* kirton_native_collection_index(KirtonNativeRuntime* runtime,
                                                  const KirtonNativeValue* collection,
                                                  const KirtonNativeValue* index);
void kirton_native_collection_set(KirtonNativeRuntime* runtime,
                                  const KirtonNativeValue* collection,
                                  const KirtonNativeValue* index,
                                  const KirtonNativeValue* value);
KirtonNativeValue* kirton_native_dynamic_add(KirtonNativeRuntime* runtime,
                                             const KirtonNativeValue* left,
                                             const KirtonNativeValue* right);
KirtonNativeValue* kirton_native_dynamic_numeric(KirtonNativeRuntime* runtime,
                                                 int operation,
                                                 const KirtonNativeValue* left,
                                                 const KirtonNativeValue* right);
KirtonNativeValue* kirton_native_dynamic_compare(KirtonNativeRuntime* runtime,
                                                 int operation,
                                                 const KirtonNativeValue* left,
                                                 const KirtonNativeValue* right);
KirtonNativeValue* kirton_native_dynamic_neg(KirtonNativeRuntime* runtime,
                                             const KirtonNativeValue* value);
int kirton_native_equal(KirtonNativeRuntime* runtime,
                        const KirtonNativeValue* left,
                        const KirtonNativeValue* right);
KirtonNativeValue* kirton_native_string_concat(KirtonNativeRuntime* runtime,
                                               const KirtonNativeValue* left,
                                               const KirtonNativeValue* right);
int kirton_native_string_equal(KirtonNativeRuntime* runtime,
                               const KirtonNativeValue* left,
                               const KirtonNativeValue* right);
KirtonNativeValue* kirton_native_hold_value(KirtonNativeRuntime* runtime,
                                            const KirtonNativeValue* value);
KirtonNativeValue* kirton_native_numeric_binary(KirtonNativeRuntime* runtime,
                                                int operation,
                                                const KirtonNativeValue* left,
                                                const KirtonNativeValue* right);
KirtonNativeValue* kirton_native_numeric_neg(KirtonNativeRuntime* runtime,
                                             const KirtonNativeValue* value);
KirtonNativeValue* kirton_native_numeric_bitnot(KirtonNativeRuntime* runtime,
                                                const KirtonNativeValue* value);
int kirton_native_numeric_compare(KirtonNativeRuntime* runtime,int operation,
                                  const KirtonNativeValue* left,
                                  const KirtonNativeValue* right);
void kirton_native_print_value(KirtonNativeRuntime* runtime,
                               const KirtonNativeValue* value);
void kirton_native_print_i64(KirtonNativeRuntime* runtime, int64_t value);
void kirton_native_print_bool(KirtonNativeRuntime* runtime, int64_t value);
void kirton_native_print_space(KirtonNativeRuntime* runtime);
void kirton_native_print_end(KirtonNativeRuntime* runtime);
int kirton_native_truthy(KirtonNativeRuntime* runtime,
                         const KirtonNativeValue* value);
void kirton_native_safepoint(KirtonNativeRuntime* runtime,
                             const KirtonNativeValue* const* live_values,
                             size_t live_count);
