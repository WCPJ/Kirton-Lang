/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/value.h"
#include "kirton/eval.h"
#include <stdint.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>

#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
void native_init(void);

int main(int argc, char** argv){
    CHECK(argc==2);
    KirValueHeap* heap=v_value_heap_new();
    CHECK(heap);
    KirValueHeap* previous=v_value_heap_set_current(heap);
    Value arena=v_arena_new_aligned(256,64);
    CHECK(arena.kind==VAL_ARENA);
    Value ints=v_typed_array_new(arena,6,KIR_ELEM_I32);
    Value longs=v_typed_array_new(arena,3,KIR_ELEM_I64);
    Value floats=v_typed_array_new(arena,3,KIR_ELEM_F64);
    Value bytes=v_typed_array_new(arena,5,KIR_ELEM_U8);
    CHECK(ints.kind==VAL_TYPED_ARRAY && longs.kind==VAL_TYPED_ARRAY);
    CHECK(floats.kind==VAL_TYPED_ARRAY && bytes.kind==VAL_TYPED_ARRAY);
    CHECK(v_typed_array_is_aligned(ints) && v_typed_array_is_aligned(longs) && v_typed_array_is_aligned(floats));
    size_t n=0;
    KirElementType t=0;
    CHECK(v_typed_array_length(ints,&n) && n==6);
    CHECK(v_typed_array_type(longs,&t) && t==KIR_ELEM_I64);
    Value got=v_nil();
    CHECK(v_typed_array_get(ints,0,&got) && v_num_equal(got,v_int(0)));
    CHECK(v_typed_array_set(ints,0,v_int(INT32_MIN)));
    CHECK(v_typed_array_set(ints,5,v_int(INT32_MAX)));
    CHECK(v_typed_array_get(ints,0,&got) && v_num_equal(got,v_int(INT32_MIN)));
    CHECK(v_typed_array_get(ints,5,&got) && v_num_equal(got,v_int(INT32_MAX)));
    CHECK(!v_typed_array_set(ints,1,v_int((int64_t)INT32_MAX+1)));
    CHECK(!v_typed_array_set(ints,1,v_int((int64_t)INT32_MIN-1)));
    CHECK(!v_typed_array_get(ints,6,&got));
    CHECK(!v_typed_array_set(ints,SIZE_MAX,v_int(1)));
    CHECK(v_typed_array_set(longs,0,v_int(INT64_MIN)));
    CHECK(v_typed_array_set(longs,1,v_int(INT64_MAX)));
    CHECK(v_typed_array_get(longs,0,&got) && v_num_equal(got,v_int(INT64_MIN)));
    CHECK(v_typed_array_get(longs,1,&got) && v_num_equal(got,v_int(INT64_MAX)));
    CHECK(v_typed_array_set(floats,1,v_num(-2.75)));
    CHECK(v_typed_array_get(floats,1,&got) && got.kind==VAL_NUM && !got.is_integer && got.as.num==-2.75);
    CHECK(!v_typed_array_set(floats,1,v_num(INFINITY)));
    CHECK(v_typed_array_set(bytes,4,v_int(255)));
    CHECK(!v_typed_array_set(bytes,4,v_int(256)));
    CHECK(!v_typed_array_set(bytes,4,v_int(-1)));
    CHECK(v_typed_array_get(bytes,4,&got) && v_num_equal(got,v_int(255)));
    CHECK(v_typed_array_new(arena,SIZE_MAX,KIR_ELEM_I64).kind==VAL_NIL);
    CHECK(v_typed_array_new(arena,1,(KirElementType)127).kind==VAL_NIL);
    CHECK(v_typed_array_new(arena,500,KIR_ELEM_I32).kind==VAL_NIL);
    CHECK(!v_typed_array_set(bytes,1,v_str_copy("bad",-1)));

    /* A nested VM collection keeps a typed array, and thereby its arena,
       alive even when no direct arena root survives. */
    Value roots=v_list_new();
    v_list_push(&roots,longs);
    v_value_heap_mark(heap,NULL,roots);
    CHECK(v_value_heap_sweep(heap)>=3);
    CHECK(v_typed_array_get(longs,1,&got) && v_num_equal(got,v_int(INT64_MAX)));
    /* An explicit reset invalidates every old view. */
    CHECK(v_arena_reset(arena));
    CHECK(!v_typed_array_valid(longs));
    CHECK(!v_typed_array_get(longs,0,&got));
    CHECK(!v_typed_array_set(longs,0,v_int(2)));
    Value newer=v_typed_array_new(arena,2,KIR_ELEM_I64);
    CHECK(newer.kind==VAL_TYPED_ARRAY);
    CHECK(v_typed_array_get(newer,0,&got) && v_num_equal(got,v_int(0)));
    CHECK(v_arena_close(arena));
    CHECK(!v_typed_array_valid(newer));
    CHECK(!v_typed_array_get(newer,0,&got));
    v_value_heap_set_current(previous);
    v_value_heap_destroy(heap);

    /* The C API needs explicit pinning when returning heap values. */
    native_init();
    VM* vm=vm_new();
    CHECK(vm && vm_run_root(vm,argv[1]));
    Value returned=v_nil();
    CHECK(vm_call_by_name(vm,"создать_массив",0,NULL,&returned));
    CHECK(returned.kind==VAL_TYPED_ARRAY);
    VM* other=vm_new();
    CHECK(other && vm_run_root(other,argv[1]));
    CHECK(!vm_value_pin(other,returned));
    /* Nested and cyclic C containers must not smuggle a foreign handle. */
    KirValueHeap* c_heap=v_value_heap_new();
    KirValueHeap* old=v_value_heap_set_current(c_heap);
    Value wrapper=v_list_new();
    v_list_push(&wrapper,returned);
    v_list_push(&wrapper,wrapper); /* self-reference */
    CHECK(!vm_value_pin(other,wrapper));
    Value dictionary=v_dict_new();
    v_dict_set(&dictionary,"nested",6,wrapper);
    CHECK(!vm_value_pin(other,dictionary));
    v_value_heap_set_current(old);
    v_value_heap_destroy(c_heap);
    Value ignored=v_nil();
    CHECK(!vm_call_by_name(other,"прочитать_массив",1,&returned,&ignored));
    vm_free(other);
    KirPinnedValue* pin=vm_value_pin(vm,returned);
    CHECK(pin);
    (void)vm_gc_collect(vm);
    CHECK(v_typed_array_get(vm_value_pinned(pin),2,&got));
    CHECK(v_num_equal(got,v_int(INT64_C(9007199254740993))));
    vm_value_unpin(vm,pin);
    (void)vm_gc_collect(vm);
    CHECK(vm_memory_arena_bytes(vm)==0);
    vm_free(vm);
    return 0;
}
