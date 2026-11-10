/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/native.h"
#include "kirton/value.h"
#include <stdlib.h>
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
    const size_t depth=257;
    char* json=(char*)malloc(depth*2+2);
    CHECK(json!=NULL);
    memset(json,'[',depth);
    json[depth]='0';
    memset(json+depth+1,']',depth);
    json[depth*2+1]='\0';
    Value input=v_str_copy(json,(int)(depth*2+1));
    Value valid=v_nil();
    int called=kir_native_call("json_валиден",strlen("json_валиден"),1,&input,&valid);
    free(json);
    CHECK(called && valid.kind==VAL_BOOL && !valid.as.boolean);
    v_value_heap_mark(test_heap,NULL,input);
    (void)v_value_heap_sweep(test_heap);
    CHECK(v_value_heap_count(test_heap)==1);
    cleanup_values();
    return 0;
}
