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

int main(void){
    KirValueHeap* heap=v_value_heap_new();
    CHECK(heap);
    KirValueHeap* old=v_value_heap_set_current(heap);
    KirRecordFieldDef desc[]={
        {"a",1,KIR_ELEM_I64},{"b",1,KIR_ELEM_I64},
        {"c",1,KIR_ELEM_F64},{"d",1,KIR_ELEM_U8}
    };
    Value schema=v_record_layout_new(desc,4);
    CHECK(schema.kind==VAL_RECORD_LAYOUT);
    size_t record_size=0;
    CHECK(v_record_layout_info(schema,&record_size,NULL,NULL));
    Value arena=v_arena_new(640*1024);
    CHECK(arena.kind==VAL_ARENA);
    Value first=v_nil();
    Value last=v_nil();
    for (size_t round=0;round<120;++round){
        for (size_t i=0;i<15000;++i){
            Value record=v_record_new(arena,schema);
            CHECK(record.kind==VAL_RECORD);
            if (i==0) first=record;
            if (i==14999) last=record;
            CHECK(v_record_set(record,"a",1,v_int((int64_t)(round*15000+i))));
        }
        Value got=v_nil();
        CHECK(v_record_get(first,"a",1,&got));
        CHECK(v_num_equal(got,v_int((int64_t)(round*15000))));
        CHECK(v_record_get(last,"a",1,&got));
        CHECK(v_num_equal(got,v_int((int64_t)(round*15000+14999))));
        CHECK(v_arena_reset(arena));
        CHECK(!v_record_valid(first) && !v_record_valid(last));
        /* The first records are stale; the heap still owns the descriptors.
           Sweep them, retaining just the schema and arena. */
        v_value_heap_mark(heap,NULL,schema);
        v_value_heap_mark(heap,NULL,arena);
        (void)v_value_heap_sweep(heap);
        CHECK(v_value_heap_count(heap)==2);
    }
    /* Ensure the records are really released, not only their backing bytes. */
    v_value_heap_mark(heap,NULL,schema);
    (void)v_value_heap_sweep(heap);
    CHECK(v_value_heap_arena_bytes(heap)==0);
    CHECK(v_value_heap_count(heap)==1);
    v_value_heap_set_current(old);
    v_value_heap_destroy(heap);
    return 0;
}
