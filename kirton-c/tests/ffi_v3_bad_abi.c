/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/ffi.h"
#ifdef _WIN32
# define FFI_EXPORT __declspec(dllexport)
#else
# define FFI_EXPORT __attribute__((visibility("default")))
#endif
static const KirFfiModuleV2 bad={999,sizeof(KirFfiModuleV2),"bad-v3",0,0};
FFI_EXPORT const KirFfiModuleV2* kirton_ffi_module_v2(void){return &bad;}
