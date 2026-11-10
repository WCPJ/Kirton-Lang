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

void list_init(void);

int main(void){
    KirValueHeap* heap=v_value_heap_new();
    CHECK(heap!=NULL);
    KirValueHeap* previous=v_value_heap_set_current(heap);
    list_init();

    Value list=v_list_new();
    Value arguments[]={list,v_int(42)};
    Value result=v_nil();
    kir_allocator_test_fail_after(0);
    CHECK(kir_native_call("список_добавить",strlen("список_добавить"),
                          2,arguments,&result));
    CHECK(result.kind==VAL_BOOL && !result.as.boolean);
    CHECK(list.as.list->len==0 && list.as.list->data==NULL);

    CHECK(kir_native_call("список_добавить",strlen("список_добавить"),
                          2,arguments,&result));
    CHECK(result.kind==VAL_BOOL && result.as.boolean);
    CHECK(list.as.list->len==1 && list.as.list->data[0].as.num==42.0);

    VList huge={NULL,SIZE_MAX,0};
    VList one={NULL,1,0};
    Value malformed_args[2]={v_nil(),v_nil()};
    malformed_args[0].kind=VAL_LIST;
    malformed_args[0].as.list=&huge;
    malformed_args[1].kind=VAL_LIST;
    malformed_args[1].as.list=&one;
    CHECK(kir_native_call("список_соединить",strlen("список_соединить"),
                          2,malformed_args,&result));
    CHECK(result.kind==VAL_NIL);
    Value insert_args[]={malformed_args[0],v_int(0),v_int(7)};
    CHECK(kir_native_call("список_вставить",strlen("список_вставить"),
                          3,insert_args,&result));
    CHECK(result.kind==VAL_BOOL && !result.as.boolean);

    v_value_heap_set_current(previous);
    v_value_heap_destroy(heap);
    return 0;
}
