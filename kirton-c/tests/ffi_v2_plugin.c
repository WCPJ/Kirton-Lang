/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/ffi.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef _WIN32
#  define FFI_EXPORT __declspec(dllexport)
#else
#  define FFI_EXPORT __attribute__((visibility("default")))
#endif

static int add_i64(uint32_t argc, const KirFfiValue* a, KirFfiValue* r){
    if (argc!=2 || a[1].data.i64>INT64_MAX-a[0].data.i64) return 0;
    r->type=KIR_FFI_I64;
    r->data.i64=a[0].data.i64+a[1].data.i64;
    return 1;
}
static int mul_f64(uint32_t argc, const KirFfiValue* a, KirFfiValue* r){
    if (argc!=2) return 0;
    r->type=KIR_FFI_F64;
    r->data.f64=a[0].data.f64*a[1].data.f64;
    return 1;
}
static int invert_bool(uint32_t argc, const KirFfiValue* a, KirFfiValue* r){
    if (argc!=1) return 0;
    r->type=KIR_FFI_BOOL;
    r->data.boolean=!a[0].data.boolean;
    return 1;
}
static int echo_text(uint32_t argc, const KirFfiValue* a, KirFfiValue* r){
    if (argc!=1) return 0;
    r->type=KIR_FFI_TEXT;
    r->data.text=a[0].data.text;
    return 1;
}
static int noop(uint32_t argc,const KirFfiValue* a,KirFfiValue* r){
    (void)argc; (void)a;
    r->type=KIR_FFI_VOID;
    return 1;
}
static int invalid_result(uint32_t argc,const KirFfiValue* a,KirFfiValue* r){
    (void)argc; (void)a;
    r->type=KIR_FFI_BOOL;
    r->data.boolean=1;
    return 1;
}
static int deliberate_failure(uint32_t argc,const KirFfiValue* a,KirFfiValue* r){
    (void)argc; (void)a; (void)r;
    return 0;
}
static const uint32_t i64_args[]={KIR_FFI_I64,KIR_FFI_I64};
static const uint32_t f64_args[]={KIR_FFI_F64,KIR_FFI_F64};
static const uint32_t bool_args[]={KIR_FFI_BOOL};
static const uint32_t text_args[]={KIR_FFI_TEXT};
static const KirFfiFunctionV1 functions[]={
    {"сложить",2,i64_args,KIR_FFI_I64,add_i64},
    {"умножить",2,f64_args,KIR_FFI_F64,mul_f64},
    {"инвертировать",1,bool_args,KIR_FFI_BOOL,invert_bool},
    {"повторить",1,text_args,KIR_FFI_TEXT,echo_text},
    {"пусто",0,NULL,KIR_FFI_VOID,noop},
    {"неправильный_ответ",0,NULL,KIR_FFI_I64,invalid_result},
    {"отказ",0,NULL,KIR_FFI_I64,deliberate_failure}
};
static const KirFfiModuleV1 module={
    KIR_FFI_ABI_V1,sizeof(KirFfiModuleV1),"арифметика",
    (uint32_t)(sizeof(functions)/sizeof(functions[0])),functions
};
FFI_EXPORT const KirFfiModuleV1* kirton_ffi_module_v1(void){return &module;}
