/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/eval.h"
#include <string.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)

int main(int argc, char** argv){
    CHECK(argc==2);
    VM* vm=vm_new();
    CHECK(vm!=NULL);
    CHECK(vm_run_root(vm,argv[1])!=NULL);
    CHECK(vm_gc_collections(vm)>10);
    CHECK(vm_gc_tracked(vm)<1024);
    Value result=v_nil();
    CHECK(vm_call_by_name(vm,"получить_сохраненное",0,NULL,&result));
    KirPinnedValue* pin=vm_value_pin(vm,result);
    CHECK(pin!=NULL);
    (void)vm_gc_collect(vm);
    Value preserved=vm_value_pinned(pin);
    size_t n=0;
    char* text=v_num_to_string(preserved,&n);
    CHECK(n==39);
    CHECK(memcmp(text,"123456789012345678901234567890123456789",39)==0);
    /* String result memory is allocated by v_num_to_string, not by the VM. */
    free(text);
    vm_value_unpin(vm,pin);
    Value temporary=v_nil();
    CHECK(vm_call_by_name(vm,"дать_временное",0,NULL,&temporary));
    CHECK(temporary.kind==VAL_LIST && temporary.as.list->len==2);
    KirPinnedValue* temporary_pin=vm_value_pin(vm,temporary);
    CHECK(temporary_pin!=NULL);
    (void)vm_gc_collect(vm);
    Value alive=vm_value_pinned(temporary_pin);
    CHECK(alive.kind==VAL_LIST && alive.as.list->len==2);
    char* tmp=v_num_to_string(alive.as.list->data[0],&n);
    CHECK(n==39 && memcmp(tmp,"123456789012345678901234567890123456790",39)==0);
    free(tmp);
    Value text_value=v_nil();
    CHECK(v_dict_get(&alive.as.list->data[1],"текст",strlen("текст"),&text_value));
    CHECK(text_value.kind==VAL_STR && text_value.as.str.n==strlen("живое"));
    CHECK(memcmp(text_value.as.str.s,"живое",text_value.as.str.n)==0);
    size_t before=vm_gc_tracked(vm);
    vm_value_unpin(vm,temporary_pin);
    CHECK(vm_gc_collect(vm)>=3);
    CHECK(vm_gc_tracked(vm)<before);
    vm_free(vm);
    return 0;
}
