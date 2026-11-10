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

void math_init(void);

int main(void){
    math_init();
    KirValueHeap* heap=v_value_heap_new();
    CHECK(heap);
    KirValueHeap* previous=v_value_heap_set_current(heap);
    Value input=v_str_copy("123456789.25",-1),result=v_nil();
    kir_allocator_test_fail_after(0);
    CHECK(kir_native_call("число_из_строки",strlen("число_из_строки"),
                          1,&input,&result));
    CHECK(result.kind==VAL_NUM && v_num_equal(result,v_int(0)));
    CHECK(kir_native_call("число_из_строки",strlen("число_из_строки"),
                          1,&input,&result));
    CHECK(result.kind==VAL_NUM && v_num_to_double(result)==123456789.25);
    v_value_heap_set_current(previous);
    v_value_heap_destroy(heap);
    return 0;
}
