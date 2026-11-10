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

void str_init(void);

static int call_string(const char* name,int argc,const Value* arguments,Value* result){
    return kir_native_call(name,strlen(name),argc,arguments,result);
}

int main(void){
    KirValueHeap* heap=v_value_heap_new();
    CHECK(heap!=NULL);
    KirValueHeap* previous=v_value_heap_set_current(heap);
    str_init();

    Value replace_args[]={v_str_copy("aaaa",-1),v_str_copy("aa",-1),v_str_copy("b",-1)};
    Value result=v_nil();
    kir_allocator_test_fail_after(0);
    CHECK(call_string("строка_заменить",3,replace_args,&result));
    CHECK(result.kind==VAL_STR && result.as.str.n==0);
    CHECK(call_string("строка_заменить",3,replace_args,&result));
    CHECK(result.kind==VAL_STR && result.as.str.n==2 &&
          memcmp(result.as.str.s,"bb",2)==0);

    Value join_items=v_list_new();
    v_list_push(&join_items,v_str_copy("a",-1));
    v_list_push(&join_items,v_str_copy("b",-1));
    Value join_args[]={join_items,v_str_copy("-",-1)};
    kir_allocator_test_fail_after(0);
    CHECK(call_string("строка_склеить",2,join_args,&result));
    CHECK(result.kind==VAL_STR && result.as.str.n==0);
    CHECK(call_string("строка_склеить",2,join_args,&result));
    CHECK(result.kind==VAL_STR && result.as.str.n==3 &&
          memcmp(result.as.str.s,"a-b",3)==0);

    v_value_heap_set_current(previous);
    v_value_heap_destroy(heap);
    return 0;
}
