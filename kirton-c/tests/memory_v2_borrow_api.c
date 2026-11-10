/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/eval.h"
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include <stdio.h>
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "borrow test failed line %d: %s\n",__LINE__,#expr); return __LINE__; } } while (0)
void native_init(void);

int main(int argc, char** argv){
    CHECK(argc==2);
    native_init();
    VM* vm=vm_new();
    VM* other=vm_new();
    CHECK(vm && other);
    CHECK(vm_run_root(vm,argv[1]) && vm_run_root(other,argv[1]));
    Value list=v_nil();
    CHECK(vm_call_by_name(vm,"создать_набор",0,NULL,&list));
    CHECK(list.kind==VAL_LIST && list.as.list && list.as.list->len==4);
    KirPinnedValue* root=vm_value_pin(vm,list);
    CHECK(root);
    Value arena=list.as.list->data[0];
    Value buffer=list.as.list->data[1];
    Value array=list.as.list->data[2];
    Value record=list.as.list->data[3];
    CHECK(arena.kind==VAL_ARENA && buffer.kind==VAL_BUFFER &&
          array.kind==VAL_TYPED_ARRAY && record.kind==VAL_RECORD);
    CHECK(vm_memory_borrow_count(vm)==0);
    CHECK(vm_memory_borrow(vm,buffer,(KirBorrowMode)0)==NULL);
    CHECK(vm_memory_borrow(vm,buffer,(KirBorrowMode)3)==NULL);
    CHECK(vm_memory_borrow(other,buffer,KIR_BORROW_READ)==NULL);
    CHECK(vm_memory_borrow(vm,arena,KIR_BORROW_READ)==NULL);
    CHECK(vm_memory_borrow(vm,list,KIR_BORROW_READ)==NULL);
    CHECK(vm_memory_borrow_release(NULL,NULL)==0);
    KirMemoryBorrow* first=vm_memory_borrow(vm,buffer,KIR_BORROW_READ);
    KirMemoryBorrow* second=vm_memory_borrow(vm,array,KIR_BORROW_READ);
    CHECK(first && second && vm_memory_borrow_count(vm)==2);
    CHECK(vm_memory_borrow_data(first) && vm_memory_borrow_size(first)==32);
    CHECK(vm_memory_borrow_size(second)==8*sizeof(int64_t));
    CHECK(vm_memory_borrow_mutable_data(first)==NULL);
    CHECK(vm_memory_borrow(vm,record,KIR_BORROW_WRITE)==NULL);
    CHECK(!v_arena_reset(arena) && !v_arena_close(arena));
    CHECK(!vm_try_free(vm));
    CHECK(vm_memory_borrow_release(other,first)==0);
    uint8_t val=42;
    CHECK(!vm_memory_buffer_write(vm,buffer,0,&val,1));
    CHECK(vm_memory_buffer_read(vm,buffer,0,&val,1));
    CHECK(!vm_memory_array_set(vm,array,0,v_int(10)));
    Value out=v_nil();
    CHECK(vm_memory_array_get(vm,array,0,&out));
    CHECK(!vm_memory_record_set(vm,record,"номер",strlen("номер"),v_int(5)));
    CHECK(vm_memory_record_get(vm,record,"номер",strlen("номер"),&out));
    (void)vm_gc_collect(vm);
    CHECK(vm_memory_borrow_data(first) && vm_memory_borrow_data(second));
    CHECK(vm_memory_borrow_release(vm,first));
    CHECK(!v_arena_reset(arena));
    CHECK(vm_memory_borrow_count(vm)==1);
    CHECK(vm_memory_borrow_release(vm,second));
    CHECK(vm_memory_borrow_count(vm)==0);

    KirMemoryBorrow* writer=vm_memory_borrow(vm,record,KIR_BORROW_WRITE);
    CHECK(writer);
    CHECK(vm_memory_borrow_mutable_data(writer));
    CHECK(vm_memory_borrow_size(writer)>=sizeof(int64_t));
    CHECK(vm_memory_borrow(vm,buffer,KIR_BORROW_READ)==NULL);
    CHECK(vm_memory_borrow(vm,array,KIR_BORROW_WRITE)==NULL);
    CHECK(!v_arena_reset(arena) && !v_arena_close(arena));
    CHECK(!vm_memory_buffer_read(vm,buffer,0,&val,1));
    CHECK(!vm_memory_array_get(vm,array,0,&out));
    CHECK(!vm_memory_record_get(vm,record,"номер",strlen("номер"),&out));
    int64_t exact=INT64_MAX;
    memcpy(vm_memory_borrow_mutable_data(writer),&exact,sizeof(exact));
    vm_value_unpin(vm,root); /* Only borrow keeps the arena reachable now. */
    (void)vm_gc_collect(vm);
    CHECK(vm_memory_arena_bytes(vm)==4096);
    CHECK(!vm_try_free(vm));
    CHECK(vm_memory_borrow_release(vm,writer));
    CHECK(vm_memory_borrow_count(vm)==0);
    /* This Value remains live only up to the next GC (no C root). */
    CHECK(vm_memory_record_get(vm,record,"номер",strlen("номер"),&out));
    CHECK(v_num_equal(out,v_int(INT64_MAX)));
    (void)vm_gc_collect(vm);
    CHECK(vm_memory_arena_bytes(vm)==0);

    /* A second arena tests direct typed-array and raw-buffer writes. */
    Value next=v_nil();
    CHECK(vm_call_by_name(vm,"создать_набор",0,NULL,&next));
    CHECK(next.kind==VAL_LIST && next.as.list->len==4);
    KirPinnedValue* next_pin=vm_value_pin(vm,next);
    CHECK(next_pin);
    Value next_arena=next.as.list->data[0];
    Value next_buffer=next.as.list->data[1];
    Value next_array=next.as.list->data[2];
    KirMemoryBorrow* array_writer=vm_memory_borrow(vm,next_array,KIR_BORROW_WRITE);
    CHECK(array_writer && vm_memory_borrow_size(array_writer)==8*sizeof(int64_t));
    int64_t number=INT64_MIN+17;
    memcpy(vm_memory_borrow_mutable_data(array_writer),&number,sizeof(number));
    CHECK(!vm_memory_array_get(vm,next_array,0,&out));
    CHECK(vm_memory_borrow_release(vm,array_writer));
    CHECK(vm_memory_array_get(vm,next_array,0,&out));
    CHECK(v_num_equal(out,v_int(INT64_MIN+17)));
    KirMemoryBorrow* buf_writer=vm_memory_borrow(vm,next_buffer,KIR_BORROW_WRITE);
    CHECK(buf_writer && vm_memory_borrow_size(buf_writer)==32);
    uint8_t* direct=(uint8_t*)vm_memory_borrow_mutable_data(buf_writer);
    CHECK(direct);
    direct[0]=255;
    direct[31]=123;
    CHECK(!vm_memory_buffer_read(vm,next_buffer,0,&val,1));
    CHECK(vm_memory_borrow_release(vm,buf_writer));
    CHECK(vm_memory_buffer_read(vm,next_buffer,0,&val,1) && val==255);
    CHECK(vm_memory_buffer_read(vm,next_buffer,31,&val,1) && val==123);
    for (size_t i=0;i<25000;++i){
        KirMemoryBorrow* r=vm_memory_borrow(vm,next_buffer,KIR_BORROW_READ);
        CHECK(r && vm_memory_borrow_size(r)==32);
        if (i%1000==0) (void)vm_gc_collect(vm);
        CHECK(vm_memory_borrow_release(vm,r));
    }
    CHECK(vm_memory_borrow_count(vm)==0);
    CHECK(v_arena_reset(next_arena));
    CHECK(vm_memory_borrow(vm,next_buffer,KIR_BORROW_READ)==NULL);
    CHECK(vm_memory_borrow(vm,next_array,KIR_BORROW_READ)==NULL);
    CHECK(v_arena_close(next_arena));
    vm_value_unpin(vm,next_pin);

    Value empty=v_nil();
    CHECK(vm_call_by_name(vm,"создать_пустой",0,NULL,&empty));
    CHECK(empty.kind==VAL_LIST && empty.as.list->len==2);
    KirPinnedValue* empty_pin=vm_value_pin(vm,empty);
    CHECK(empty_pin);
    Value empty_arena=empty.as.list->data[0];
    Value empty_buffer=empty.as.list->data[1];
    KirMemoryBorrow* e=vm_memory_borrow(vm,empty_buffer,KIR_BORROW_READ);
    CHECK(e && vm_memory_borrow_size(e)==0 && vm_memory_borrow_data(e));
    CHECK(!v_arena_reset(empty_arena));
    CHECK(vm_memory_borrow_release(vm,e));
    CHECK(v_arena_reset(empty_arena));
    CHECK(vm_memory_borrow(vm,empty_buffer,KIR_BORROW_READ)==NULL);
    vm_value_unpin(vm,empty_pin);
    (void)vm_gc_collect(vm);
    CHECK(vm_memory_arena_bytes(vm)==0);
    CHECK(vm_try_free(vm));
    CHECK(vm_try_free(other));
    return 0;
}
