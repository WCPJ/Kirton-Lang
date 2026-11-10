/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#pragma once
#include "kirton/ffi.h"
#include "kirton/value.h"

typedef struct KirFfiState KirFfiState;
KirFfiState* kir_ffi_state_create(void);
void kir_ffi_state_destroy(KirFfiState* state);
int kir_ffi_state_load(KirFfiState* state, const char* path_utf8);
int kir_ffi_state_call(KirFfiState* state, KirValueHeap* heap,
                       const char* module, const char* name,
                       int argc, const Value* args, Value* out);
const char* kir_ffi_state_error(const KirFfiState* state);
