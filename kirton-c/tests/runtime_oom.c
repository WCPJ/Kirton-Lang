/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/common.h"
#include "kirton/native.h"
#include "kirton/value.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

void runtime_init_mod(void);

static int read_environment(Value* key,Value* result){
    return kir_native_call("переменная",strlen("переменная"),1,key,result);
}

int main(void){
    CHECK(SetEnvironmentVariableW(L"KIRTON_RUNTIME_OOM_TEST",L"test-value"));
    KirValueHeap* heap=v_value_heap_new();
    CHECK(heap!=NULL);
    KirValueHeap* previous=v_value_heap_set_current(heap);
    runtime_init_mod();

    Value key=v_str_copy("KIRTON_RUNTIME_OOM_TEST",-1),result=v_nil();
    const size_t failures[]={0,1,2};
    for (size_t i=0;i<sizeof(failures)/sizeof(failures[0]);++i){
        kir_allocator_test_fail_after(failures[i]);
        CHECK(read_environment(&key,&result));
        CHECK(result.kind==VAL_STR && result.as.str.n==0);
    }
    CHECK(read_environment(&key,&result));
    CHECK(result.kind==VAL_STR && result.as.str.n==strlen("test-value") &&
          memcmp(result.as.str.s,"test-value",result.as.str.n)==0);

    CHECK(SetEnvironmentVariableW(L"KIRTON_RUNTIME_OOM_TEST",NULL));
    v_value_heap_set_current(previous);
    v_value_heap_destroy(heap);
    return 0;
}
