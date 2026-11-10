/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/* Kirton V2.5: script-side deterministic close of host-created resources.
   Scripts cannot forge native handles or acquire arbitrary C pointers. */
#include "kirton/native.h"
#include "kirton/value.h"
#include "kirton/diag.h"
#include "native_internal.h"

static Value close_native(int argc, const Value* arguments){
    if (argc!=1 || arguments[0].kind!=VAL_NATIVE_RESOURCE ||
        !v_native_resource_close_current(arguments[0])){
        kir_diag_error("<ресурсы>",1,1,
            "закрыть(ресурс): ресурс недействителен, уже закрыт или используется");
        return v_nil();
    }
    return v_bool(true);
}

static Value is_open_native(int argc, const Value* arguments){
    if (argc!=1 || arguments[0].kind!=VAL_NATIVE_RESOURCE){
        kir_diag_error("<ресурсы>",1,1,"открыт(ресурс): ожидается внешний ресурс");
        return v_nil();
    }
    return v_bool(v_native_resource_is_open_current(arguments[0])!=0);
}

void native_resource_init(void){
    static const KirNativeDef entries[]={
        {"закрыть",close_native},
        {"открыт",is_open_native}
    };
    kir_native_register_module("ресурсы",entries,sizeof(entries)/sizeof(entries[0]));
    kir_native_mark_bytecode_module_safe("ресурсы");
}
