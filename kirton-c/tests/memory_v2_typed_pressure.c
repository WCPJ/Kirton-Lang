/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/value.h"
#include <stdint.h>
#include <stddef.h>
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)

/* Reuse an 8-MiB arena hundreds of times with stable GC accounting.
   This checks that typed descriptors do not retain an old generation. */
int main(void){
    KirValueHeap* heap=v_value_heap_new();
    CHECK(heap);
    KirValueHeap* previous=v_value_heap_set_current(heap);
    const size_t count=1000000;
    Value arena=v_arena_new_aligned(count*8,64);
    CHECK(arena.kind==VAL_ARENA);
    CHECK(v_value_heap_arena_bytes(heap)==count*8);
    for (int iteration=0;iteration<250;++iteration){
        Value array=v_typed_array_new(arena,count,KIR_ELEM_I64);
        CHECK(array.kind==VAL_TYPED_ARRAY);
        CHECK(v_typed_array_set(array,0,v_int(iteration)));
        CHECK(v_typed_array_set(array,count/2,v_int(INT64_C(9007199254740993))));
        CHECK(v_typed_array_set(array,count-1,v_int(-iteration)));
        Value result=v_nil();
        CHECK(v_typed_array_get(array,count/2,&result) &&
              v_num_equal(result,v_int(INT64_C(9007199254740993))));
        v_value_heap_mark(heap,NULL,array);
        CHECK(v_value_heap_sweep(heap)==0);
        CHECK(v_arena_reset(arena));
        CHECK(!v_typed_array_valid(array));
        v_value_heap_mark(heap,NULL,arena);
        CHECK(v_value_heap_sweep(heap)==1);
        CHECK(v_value_heap_count(heap)==1);
    }
    CHECK(v_arena_close(arena));
    CHECK(v_value_heap_arena_bytes(heap)==0);
    CHECK(v_value_heap_sweep(heap)==1);
    v_value_heap_set_current(previous);
    v_value_heap_destroy(heap);
    return 0;
}
