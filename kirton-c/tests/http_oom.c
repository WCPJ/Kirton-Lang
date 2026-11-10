/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/common.h"
#include "kirton/native.h"
#include "kirton/value.h"

#include <stdint.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

void http_init(void);
int kir_http_test_reserve_body(char** body,size_t* capacity,size_t used,size_t added);

int main(void){
    char* body=NULL;
    size_t capacity=0;
    CHECK(kir_http_test_reserve_body(&body,&capacity,SIZE_MAX,1)==0);
    CHECK(body==NULL && capacity==0);

    kir_allocator_test_fail_after(0);
    CHECK(kir_http_test_reserve_body(&body,&capacity,0,3)==-1);
    CHECK(body==NULL && capacity==0);

    CHECK(kir_http_test_reserve_body(&body,&capacity,0,3)==1);
    memcpy(body,"abc",3);
    char* original=body;
    size_t original_capacity=capacity;
    kir_allocator_test_fail_after(0);
    CHECK(kir_http_test_reserve_body(&body,&capacity,3,5000)==-1);
    CHECK(body==original && capacity==original_capacity && memcmp(body,"abc",3)==0);
    kir_free(body);

    KirValueHeap* heap=v_value_heap_new();
    CHECK(heap!=NULL);
    KirValueHeap* previous=v_value_heap_set_current(heap);
    http_init();
    Value url=v_str_copy("https://example.invalid/",-1),result=v_nil(),error=v_nil();

    Value method=v_str_copy("POST",-1);
    Value oversized_body=v_nil();
    oversized_body.kind=VAL_STR;
    oversized_body.as.str.s="";
    oversized_body.as.str.n=(size_t)UINT32_MAX+1u;
    Value request[]={method,url,v_nil(),oversized_body};
    CHECK(kir_native_call("http_запрос",strlen("http_запрос"),4,request,&result));
    CHECK(result.kind==VAL_DICT && v_dict_get(&result,"ошибка",strlen("ошибка"),&error));
    CHECK(error.kind==VAL_STR && error.as.str.n>0);

    kir_allocator_test_fail_after(0);
    CHECK(kir_native_call("http_получить",strlen("http_получить"),1,&url,&result));
    error=v_nil();
    CHECK(result.kind==VAL_DICT && v_dict_get(&result,"ошибка",strlen("ошибка"),&error));
    CHECK(error.kind==VAL_STR && error.as.str.n>0);
    v_value_heap_set_current(previous);
    v_value_heap_destroy(heap);
    return 0;
}
