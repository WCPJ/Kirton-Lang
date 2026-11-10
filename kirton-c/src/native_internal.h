/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "kirton/native.h"

void kir_native_mark_bytecode_safe(const KirNativeDef* defs,size_t count);
void kir_native_mark_bytecode_module_safe(const char* module_name);
int kir_native_bytecode_module_is_safe(const char* module_name);
int kir_native_bytecode_slot(const char* name,size_t name_len);
int kir_native_call_bytecode_slot(int slot,int argc,const Value* argv,Value* out);
int kir_native_has_function(const char* name,size_t name_len);
Value v_native_resource_adopt_current(void* handle,const char* type_tag,
                                     KirNativeResourceDestroy destroy,
                                     void* user_data);
