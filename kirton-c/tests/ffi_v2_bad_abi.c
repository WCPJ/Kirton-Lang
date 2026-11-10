/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/ffi.h"
#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
const KirFfiModuleV1* kirton_ffi_module_v1(void){
    static const KirFfiModuleV1 invalid={KIR_FFI_ABI_V1+1,sizeof(KirFfiModuleV1),"неверный",0,NULL};
    return &invalid;
}
