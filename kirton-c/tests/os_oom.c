/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/common.h"
#include "kirton/native.h"
#include "kirton/value.h"

#include <string.h>

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

void os_init(void);

static int call(const char* name,int argc,const Value* args,Value* result){
    return kir_native_call(name,strlen(name),argc,args,result);
}

int main(void){
    KirValueHeap* heap=v_value_heap_new();
    CHECK(heap!=NULL);
    KirValueHeap* previous=v_value_heap_set_current(heap);
    os_init();

    Value result=v_nil();
    kir_allocator_test_fail_after(0);
    CHECK(call("текущая_папка",0,NULL,&result));
    CHECK(result.kind==VAL_STR && result.as.str.n==0);

    Value name=v_str_copy("KIRTON_OOM_TEST_VALUE",-1);
    kir_allocator_test_fail_after(0);
    CHECK(call("окружение",1,&name,&result));
    CHECK(result.kind==VAL_NIL);

    Value new_value=v_str_copy("new-value",-1);
    Value set_args[]={name,new_value};
    kir_allocator_test_fail_after(0);
    CHECK(call("установить_окружение",2,set_args,&result));
    CHECK(result.kind==VAL_BOOL && !result.as.boolean);

    v_value_heap_set_current(previous);
    v_value_heap_destroy(heap);
    return 0;
}
