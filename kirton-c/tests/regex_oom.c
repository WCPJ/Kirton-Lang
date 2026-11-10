/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/native.h"
#include "kirton/value.h"
#include "kirton/common.h"
#include <string.h>

static KirValueHeap* test_heap;
static KirValueHeap* previous_heap;

static void cleanup_values(void){
    v_value_heap_set_current(previous_heap);
    v_value_heap_destroy(test_heap);
    test_heap=NULL;
}

#define CHECK(x) do { if (!(x)) { int line=__LINE__; cleanup_values(); return line; } } while (0)

void regex_init(void);

int main(void){
    test_heap=v_value_heap_new();
    CHECK(test_heap!=NULL);
    previous_heap=v_value_heap_set_current(test_heap);
    regex_init();
    Value args[]={v_str_copy("a",1),v_str_copy("banana",6),
                  v_str_copy("x",1),v_str_copy("g",1)};
    Value result=v_nil();
    kir_allocator_test_fail_after(0);
    CHECK(kir_native_call("регвыр_заменить",strlen("регвыр_заменить"),
                          4,args,&result));
    CHECK(result.kind==VAL_NIL);
    v_value_heap_mark(test_heap,NULL,args[0]);
    v_value_heap_mark(test_heap,NULL,args[1]);
    v_value_heap_mark(test_heap,NULL,args[2]);
    v_value_heap_mark(test_heap,NULL,args[3]);
    (void)v_value_heap_sweep(test_heap);
    char long_text[2049];
    memset(long_text,'a',sizeof(long_text)-1);
    long_text[sizeof(long_text)-1]='\0';
    args[0]=v_str_copy(".",1);
    args[1]=v_str_copy(long_text,(int)sizeof(long_text)-1);
    args[2]=v_str_copy("x",1);
    kir_allocator_test_fail_after(1);
    CHECK(kir_native_call("регвыр_заменить",strlen("регвыр_заменить"),
                          4,args,&result));
    CHECK(result.kind==VAL_NIL);
    v_value_heap_mark(test_heap,NULL,args[0]);
    v_value_heap_mark(test_heap,NULL,args[1]);
    v_value_heap_mark(test_heap,NULL,args[2]);
    v_value_heap_mark(test_heap,NULL,args[3]);
    v_value_heap_mark(test_heap,NULL,result);
    (void)v_value_heap_sweep(test_heap);
    CHECK(v_value_heap_count(test_heap)==4);
    cleanup_values();
    return 0;
}
