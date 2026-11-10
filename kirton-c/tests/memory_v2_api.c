/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/value.h"
#include "kirton/eval.h"
#include <stdint.h>

void native_init(void);
#define CHECK(test) do { if (!(test)) return __LINE__; } while (0)

int main(int argc, char** argv){
    CHECK(argc==2);
    KirValueHeap* heap=v_value_heap_new();
    CHECK(heap);
    KirValueHeap* previous=v_value_heap_set_current(heap);
    Value arena=v_arena_new(128);
    CHECK(arena.kind==VAL_ARENA);
    CHECK(v_value_heap_arena_bytes(heap)==128);
    CHECK(v_value_heap_set_arena_limit(heap,128));
    CHECK(v_arena_new(1).kind==VAL_NIL);
    Value buffer=v_arena_allocate(arena,8,16);
    CHECK(buffer.kind==VAL_BUFFER);
    CHECK(v_buffer_is_aligned(buffer,16));
    CHECK(v_buffer_is_aligned(buffer,64));
    CHECK(v_buffer_set_u8(buffer,7,216));
    uint8_t data=0;
    CHECK(v_buffer_get_u8(buffer,7,&data) && data==216);
    Value unrelated=v_arena_allocate(arena,8,1);
    CHECK(unrelated.kind==VAL_BUFFER);
    CHECK(v_value_heap_count(heap)==3);
    /* Mark ONLY the view. The containing arena must remain alive. */
    v_value_heap_mark(heap,NULL,buffer);
    CHECK(v_value_heap_sweep(heap)==1);
    CHECK(v_value_heap_count(heap)==2);
    CHECK(v_buffer_get_u8(buffer,7,&data) && data==216);
    CHECK(v_arena_reset(arena));
    CHECK(!v_buffer_get_u8(buffer,7,&data));
    Value latest=v_arena_allocate(arena,16,8);
    CHECK(latest.kind==VAL_BUFFER);
    CHECK(v_buffer_get_u8(latest,7,&data) && data==0);
    Value massive_alignment=v_arena_allocate(arena,0,8192);
    CHECK(massive_alignment.kind==VAL_NIL);
    CHECK(v_arena_close(arena));
    CHECK(v_value_heap_arena_bytes(heap)==0);
    CHECK(v_arena_new(129).kind==VAL_NIL);
    Value page_arena=v_arena_new_aligned(32,4096);
    CHECK(page_arena.kind==VAL_ARENA);
    Value page_buffer=v_arena_allocate(page_arena,8,4096);
    CHECK(page_buffer.kind==VAL_BUFFER);
    CHECK(v_buffer_is_aligned(page_buffer,4096));
    CHECK(v_arena_close(page_arena));
    for (size_t alignment=1;alignment<=16384;alignment*=2){
        Value aligned_arena=v_arena_new_aligned(32,alignment);
        CHECK(aligned_arena.kind==VAL_ARENA);
        Value aligned_buffer=v_arena_allocate(aligned_arena,8,alignment);
        CHECK(aligned_buffer.kind==VAL_BUFFER &&
              v_buffer_is_aligned(aligned_buffer,alignment));
        CHECK(v_arena_close(aligned_arena));
    }
    CHECK(!v_buffer_set_u8(latest,0,5));
    CHECK(!v_arena_reset(arena));
    v_value_heap_set_current(previous);
    v_value_heap_destroy(heap);

    /* Check integration with the real VM, including pinning of a buffer
       whose parent arena only survives by the buffer -> arena GC edge. */
    native_init();
    VM* vm=vm_new();
    CHECK(vm);
    CHECK(vm_memory_set_arena_limit(vm,100));
    CHECK(vm_run_root(vm,argv[1]));
    Value returned=v_nil();
    CHECK(vm_call_by_name(vm,"создать_буфер",0,NULL,&returned));
    CHECK(returned.kind==VAL_BUFFER);
    CHECK(vm_memory_arena_bytes(vm)==64);
    VM* foreign=vm_new();
    CHECK(foreign && vm_run_root(foreign,argv[1]));
    CHECK(!vm_value_pin(foreign,returned));
    Value rejected=v_nil();
    CHECK(!vm_call_by_name(foreign,"принять_буфер",1,&returned,&rejected));
    vm_free(foreign);
    CHECK(!vm_memory_set_arena_limit(vm,32));
    KirPinnedValue* pin=vm_value_pin(vm,returned);
    CHECK(pin);
    CHECK(vm_gc_collect(vm)>=0); /* explicit full collection */
    CHECK(v_buffer_get_u8(vm_value_pinned(pin),2,&data) && data==173);
    vm_value_unpin(vm,pin);
    CHECK(vm_gc_collect(vm)>=2);
    CHECK(vm_memory_arena_bytes(vm)==0);
    CHECK(vm_memory_set_arena_limit(vm,32)); /* unreachable arena and view */
    vm_free(vm);
    return 0;
}
