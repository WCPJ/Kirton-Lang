/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/value.h"
#include "kirton/eval.h"
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <math.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
void native_init(void);

typedef struct {
    uint8_t tag;
    int64_t id;
    int32_t quantity;
    double speed;
} Mirror;

int main(int argc, char** argv){
    CHECK(argc==2);
    KirValueHeap* heap=v_value_heap_new();
    CHECK(heap);
    KirValueHeap* old=v_value_heap_set_current(heap);
    KirRecordFieldDef fields[]={
        {"tag",3,KIR_ELEM_U8}, {"id",2,KIR_ELEM_I64},
        {"quantity",8,KIR_ELEM_I32}, {"speed",5,KIR_ELEM_F64}
    };
    Value schema=v_record_layout_new(fields,4);
    CHECK(schema.kind==VAL_RECORD_LAYOUT);
    size_t size=0, alignment=0, field_count=0, offset=0;
    KirElementType type=0;
    CHECK(v_record_layout_info(schema,&size,&alignment,&field_count));
    CHECK(size==sizeof(Mirror) && alignment==_Alignof(Mirror) && field_count==4);
    CHECK(v_record_field_info(schema,"id",2,&offset,&type));
    CHECK(offset==offsetof(Mirror,id) && type==KIR_ELEM_I64);
    CHECK(v_record_field_info(schema,"quantity",8,&offset,&type));
    CHECK(offset==offsetof(Mirror,quantity) && type==KIR_ELEM_I32);
    CHECK(v_record_field_info(schema,"speed",5,&offset,&type));
    CHECK(offset==offsetof(Mirror,speed) && type==KIR_ELEM_F64);
    CHECK(!v_record_field_info(schema,"missing",7,NULL,NULL));
    CHECK(!v_record_layout_info(v_nil(),NULL,NULL,NULL));
    CHECK(v_record_layout_new(fields,0).kind==VAL_NIL);
    KirRecordFieldDef duplicates[]={{"id",2,KIR_ELEM_U8},{"id",2,KIR_ELEM_I64}};
    CHECK(v_record_layout_new(duplicates,2).kind==VAL_NIL);
    KirRecordFieldDef invalid_type[]={{"field",5,(KirElementType)200}};
    CHECK(v_record_layout_new(invalid_type,1).kind==VAL_NIL);
    KirRecordFieldDef invalid_name[]={{"",0,KIR_ELEM_U8}};
    CHECK(v_record_layout_new(invalid_name,1).kind==VAL_NIL);
    char with_null[3]={'a','\0','b'};
    KirRecordFieldDef embedded_null[]={{with_null,3,KIR_ELEM_U8}};
    CHECK(v_record_layout_new(embedded_null,1).kind==VAL_NIL);
    CHECK(v_record_new(v_arena_new(8),schema).kind==VAL_NIL);
    Value arena=v_arena_new_aligned(80,64);
    CHECK(arena.kind==VAL_ARENA);
    Value first=v_record_new(arena,schema);
    Value second=v_record_new(arena,schema);
    CHECK(first.kind==VAL_RECORD && second.kind==VAL_RECORD);
    CHECK(v_record_valid(first) && v_record_valid(second));
    CHECK(v_record_get(first,"id",2,&(Value){0}));
    Value got=v_nil();
    CHECK(v_record_get(first,"tag",3,&got) && v_num_equal(got,v_int(0)));
    CHECK(v_record_set(first,"tag",3,v_int(255)));
    CHECK(!v_record_set(first,"tag",3,v_int(256)));
    CHECK(!v_record_set(first,"tag",3,v_int(-1)));
    CHECK(!v_record_set(first,"tag",3,v_str_copy("x",1)));
    CHECK(v_record_set(first,"id",2,v_int(INT64_MIN)));
    CHECK(v_record_set(second,"id",2,v_int(INT64_MAX)));
    CHECK(v_record_set(first,"quantity",8,v_int(INT32_MIN)));
    CHECK(!v_record_set(first,"quantity",8,v_int(INT64_MAX)));
    CHECK(v_record_set(first,"speed",5,v_num(123.5)));
    CHECK(!v_record_set(first,"speed",5,v_num(INFINITY)));
    CHECK(!v_record_set(first,"missing",7,v_int(2)));
    CHECK(!v_record_get(first,"missing",7,&got));
    CHECK(v_record_get(first,"id",2,&got) && v_num_equal(got,v_int(INT64_MIN)));
    CHECK(v_record_get(second,"id",2,&got) && v_num_equal(got,v_int(INT64_MAX)));
    CHECK(v_record_get(first,"quantity",8,&got) && v_num_equal(got,v_int(INT32_MIN)));
    CHECK(v_record_get(first,"speed",5,&got) && got.kind==VAL_NUM && got.as.num==123.5);

    /* Record is the only root. It must retain both arena and schema. */
    Value roots=v_list_new();
    v_list_push(&roots,first);
    v_value_heap_mark(heap,NULL,roots);
    CHECK(v_value_heap_sweep(heap)>0);
    CHECK(v_record_valid(first));
    CHECK(v_record_get(first,"id",2,&got) && v_num_equal(got,v_int(INT64_MIN)));
    CHECK(v_record_layout_info(schema,&size,NULL,NULL) && size==sizeof(Mirror));
    CHECK(v_arena_reset(arena));
    CHECK(!v_record_valid(first));
    CHECK(!v_record_get(first,"id",2,&got));
    CHECK(!v_record_set(first,"id",2,v_int(1)));
    Value replacement=v_record_new(arena,schema);
    CHECK(replacement.kind==VAL_RECORD && v_record_valid(replacement));
    CHECK(v_record_get(replacement,"id",2,&got) && v_num_equal(got,v_int(0)));
    CHECK(v_arena_close(arena));
    CHECK(!v_record_valid(replacement));
    v_value_heap_set_current(old);
    v_value_heap_destroy(heap);

    /* Cross-VM pin/call should reject foreign records and schemas,
       even if hidden in nested cyclic containers. */
    native_init();
    VM* vm=vm_new();
    VM* other=vm_new();
    CHECK(vm && other && vm_run_root(vm,argv[1]) && vm_run_root(other,argv[1]));
    Value returned=v_nil();
    CHECK(vm_call_by_name(vm,"создать_запись",0,NULL,&returned));
    CHECK(returned.kind==VAL_RECORD);
    CHECK(!vm_value_pin(other,returned));
    Value ignored=v_nil();
    CHECK(!vm_call_by_name(other,"использовать_запись",1,&returned,&ignored));
    KirValueHeap* scratch=v_value_heap_new();
    KirValueHeap* prior=v_value_heap_set_current(scratch);
    Value cycle=v_list_new();
    v_list_push(&cycle,returned);
    v_list_push(&cycle,cycle);
    Value wrapper=v_dict_new();
    v_dict_set(&wrapper,"nested",6,cycle);
    CHECK(!vm_value_pin(other,wrapper));
    v_value_heap_set_current(prior);
    v_value_heap_destroy(scratch);
    KirPinnedValue* pin=vm_value_pin(vm,returned);
    CHECK(pin);
    (void)vm_gc_collect(vm);
    CHECK(v_record_get(vm_value_pinned(pin),"номер",strlen("номер"),&got));
    CHECK(v_num_equal(got,v_int(INT64_C(9007199254740993))));
    vm_value_unpin(vm,pin);
    (void)vm_gc_collect(vm);
    CHECK(vm_memory_arena_bytes(vm)==0);
    vm_free(other);
    vm_free(vm);
    return 0;
}
