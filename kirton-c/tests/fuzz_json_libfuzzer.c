/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/native.h"
#include "kirton/value.h"
#include "kirton/number.h"
#include <limits.h>
#include <string.h>

void json_init(void);

int LLVMFuzzerInitialize(int* argc,char*** argv){
    (void)argc; (void)argv;
    json_init();
    return 0;
}

int LLVMFuzzerTestOneInput(const unsigned char* data,size_t size){
    if (size>(size_t)INT_MAX) return 0;
    KirValueHeap* values=v_value_heap_new();
    KirBigIntHeap* bigints=kir_bigint_heap_new();
    KirValueHeap* old_values=v_value_heap_set_current(values);
    KirBigIntHeap* old_bigints=v_bigint_heap_set_current(bigints);
    Value input=v_nil(),valid=v_nil(),parsed=v_nil(),serialized=v_nil();
    input.kind=VAL_STR;
    input.as.str.s=(char*)data;
    input.as.str.n=(int)size;
    (void)kir_native_call("json_валиден",strlen("json_валиден"),1,&input,&valid);
    (void)kir_native_call("json_прочитать",strlen("json_прочитать"),1,&input,&parsed);
    (void)kir_native_call("json_написать",strlen("json_написать"),1,&parsed,&serialized);
    v_bigint_heap_set_current(old_bigints);
    v_value_heap_set_current(old_values);
    v_value_heap_destroy(values);
    kir_bigint_heap_destroy(bigints);
    return 0;
}
