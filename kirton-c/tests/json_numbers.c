/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/native.h"
#include "kirton/value.h"
#include "kirton/diag.h"
#include "kirton/common.h"
#include <math.h>
#include <string.h>

static KirValueHeap* test_heap;
static KirValueHeap* previous_heap;

static void cleanup_values(void){
    v_value_heap_set_current(previous_heap);
    v_value_heap_destroy(test_heap);
    test_heap=NULL;
}

#define CHECK(x) do { if (!(x)) { int line=__LINE__; cleanup_values(); return line; } } while (0)

void json_init(void);

int main(void){
    test_heap=v_value_heap_new();
    CHECK(test_heap!=NULL);
    previous_heap=v_value_heap_set_current(test_heap);
    json_init();
    Value overflow=v_str_copy("1e999",5),result=v_nil(),valid=v_nil();
    CHECK(kir_native_call("json_валиден",strlen("json_валиден"),1,&overflow,&result));
    CHECK(result.kind==VAL_BOOL && !result.as.boolean);

    unsigned int before=kir_diag_error_count();
    Value infinity=v_num(INFINITY);
    CHECK(kir_native_call("json_написать",strlen("json_написать"),1,&infinity,&result));
    CHECK(result.kind==VAL_STR && result.as.str.n==0);
    CHECK(kir_diag_error_count()==before+1);

    before=kir_diag_error_count();
    Value nan=v_num(NAN);
    CHECK(kir_native_call("json_написать",strlen("json_написать"),1,&nan,&result));
    CHECK(result.kind==VAL_STR && result.as.str.n==0);
    CHECK(kir_diag_error_count()==before+1);

    Value source=v_str_copy("oom",3);
    kir_allocator_test_fail_after(0);
    CHECK(kir_native_call("json_написать",strlen("json_написать"),1,&source,&result));
    CHECK(result.kind==VAL_STR && result.as.str.n==0);

    Value number=v_str_copy("123",3);
    kir_allocator_test_fail_after(0);
    CHECK(kir_native_call("json_валиден",strlen("json_валиден"),1,&number,&valid));
    CHECK(valid.kind==VAL_BOOL && !valid.as.boolean);
    cleanup_values();
    return 0;
}
