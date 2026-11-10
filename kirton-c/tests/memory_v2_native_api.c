/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/eval.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#define CHECK(expr) do { if (!(expr)) return __LINE__; } while(0)
void native_init(void);

int main(int argc, char** argv){
    CHECK(argc==2);
    native_init();
    VM* vm=vm_new();
    VM* other=vm_new();
    CHECK(vm && other && vm_run_root(vm,argv[1]) && vm_run_root(other,argv[1]));
    Value buffer=v_nil(), array=v_nil(), record=v_nil(), result=v_nil();
    CHECK(vm_call_by_name(vm,"создать_буфер",0,NULL,&buffer));
    CHECK(vm_call_by_name(vm,"создать_массив",0,NULL,&array));
    CHECK(vm_call_by_name(vm,"создать_запись",0,NULL,&record));
    KirPinnedValue* bp=vm_value_pin(vm,buffer);
    KirPinnedValue* ap=vm_value_pin(vm,array);
    KirPinnedValue* rp=vm_value_pin(vm,record);
    CHECK(bp && ap && rp);
    (void)vm_gc_collect(vm);
    buffer=vm_value_pinned(bp);
    array=vm_value_pinned(ap);
    record=vm_value_pinned(rp);
    uint8_t src[5]={10,20,30,40,50};
    uint8_t dst[8]={0};
    CHECK(vm_memory_buffer_write(vm,buffer,2,src,5));
    CHECK(vm_memory_buffer_read(vm,buffer,0,dst,8));
    CHECK(dst[0]==0 && dst[1]==0 && !memcmp(dst+2,src,5) && dst[7]==0);
    CHECK(!vm_memory_buffer_write(vm,buffer,4,src,5));
    CHECK(!vm_memory_buffer_read(vm,buffer,SIZE_MAX,dst,1));
    CHECK(!vm_memory_buffer_write(vm,buffer,0,NULL,1));
    CHECK(!vm_memory_buffer_read(vm,buffer,0,NULL,1));
    CHECK(vm_memory_buffer_read(vm,buffer,8,NULL,0));
    CHECK(!vm_memory_buffer_read(other,buffer,0,dst,8));
    CHECK(!vm_memory_buffer_write(other,buffer,0,src,5));
    CHECK(vm_memory_buffer_read(vm,buffer,0,dst,8) && dst[2]==10);
    CHECK(vm_memory_array_set(vm,array,1,v_int(INT64_MAX)));
    CHECK(vm_memory_array_get(vm,array,1,&result));
    CHECK(v_num_equal(result,v_int(INT64_MAX)));
    CHECK(!vm_memory_array_get(other,array,1,&result));
    CHECK(!vm_memory_array_set(other,array,1,v_int(1)));
    CHECK(!vm_memory_array_get(vm,array,99,&result));
    CHECK(vm_memory_record_set(vm,record,"номер",strlen("номер"),v_int(INT64_MIN)));
    CHECK(vm_memory_record_get(vm,record,"номер",strlen("номер"),&result));
    CHECK(v_num_equal(result,v_int(INT64_MIN)));
    CHECK(!vm_memory_record_get(other,record,"номер",strlen("номер"),&result));
    CHECK(!vm_memory_record_set(other,record,"номер",strlen("номер"),v_int(1)));
    CHECK(!vm_memory_record_get(vm,record,"нет",strlen("нет"),&result));
    CHECK(!vm_memory_record_get(vm,record,NULL,0,&result));
    CHECK(!vm_memory_buffer_read(NULL,buffer,0,dst,1));
    vm_value_unpin(vm,bp);
    vm_value_unpin(vm,ap);
    vm_value_unpin(vm,rp);
    (void)vm_gc_collect(vm);
    CHECK(vm_memory_arena_bytes(vm)==0);
    vm_free(other);
    vm_free(vm);
    return 0;
}
