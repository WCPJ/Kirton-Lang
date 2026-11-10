/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/eval.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

static int call_value(VM* vm,const char* name,Value* out){
    return vm_call_by_name(vm,name,0,NULL,out);
}

int main(int argc,char** argv){
    CHECK(argc==2);
    VM* owner=vm_new();
    VM* other=vm_new();
    CHECK(owner && other);
    CHECK(vm_run_root(owner,argv[1])!=NULL);
    CHECK(vm_run_root(other,argv[1])!=NULL);

    Value value=v_nil();
    CHECK(call_value(owner,"создать_структуру",&value));
    KirPinnedValue* structure_pin=vm_value_pin(owner,value);
    CHECK(structure_pin && !vm_value_pin(other,value));
    value=v_nil();

    CHECK(call_value(owner,"создать_перечисление",&value));
    KirPinnedValue* enum_pin=vm_value_pin(owner,value);
    CHECK(enum_pin && !vm_value_pin(other,value));
    value=v_nil();

    CHECK(call_value(owner,"создать_замыкание",&value));
    KirPinnedValue* closure_pin=vm_value_pin(owner,value);
    CHECK(closure_pin && !vm_value_pin(other,value));
    value=v_nil();

    static const char* foreign_names[]={"создать_строку","создать_список",
                                        "создать_большое_число"};
    for (size_t i=0;i<sizeof(foreign_names)/sizeof(foreign_names[0]);++i){
        CHECK(call_value(owner,foreign_names[i],&value));
        if (vm_value_pin(other,value)){
            fprintf(stderr,"Foreign VM accepted %s (kind %d)\n",foreign_names[i],value.kind);
            return __LINE__;
        }
        Value result=v_nil();
        CHECK(!vm_call_by_name(other,"тождество",1,&value,&result));
        value=v_nil();
    }

    size_t collections_before=vm_gc_collections(owner);
    for (int i=0;i<100;++i) (void)vm_gc_collect(owner);
    CHECK(vm_gc_collections(owner)>=collections_before+100);

    Value structure=vm_value_pinned(structure_pin);
    CHECK(structure.kind==VAL_USER_STRUCT);
    Value fields=v_user_struct_fields(structure);
    Value name=v_nil(),number=v_nil();
    CHECK(v_dict_get(&fields,"имя",strlen("имя"),&name));
    CHECK(name.kind==VAL_STR && name.as.str.n==strlen("закреплено") &&
          memcmp(name.as.str.s,"закреплено",name.as.str.n)==0);
    CHECK(v_dict_get(&fields,"число",strlen("число"),&number));
    int64_t exact=0;
    CHECK(v_num_fits_i64(number,&exact) && exact==INT64_C(9007199254740993));

    const char* variant=NULL;
    size_t variant_len=0;
    Value enum_value=vm_value_pinned(enum_pin);
    CHECK(enum_value.kind==VAL_ENUM_VALUE &&
          v_enum_value_variant(enum_value,&variant,&variant_len) &&
          variant_len==strlen("активно") &&
          memcmp(variant,"активно",variant_len)==0);

    Value callable=vm_value_pinned(closure_pin),result=v_nil();
    CHECK(callable.kind==VAL_FUNCTION);
    CHECK(vm_call_by_name(owner,"вызвать",1,&callable,&result));
    CHECK(v_num_fits_i64(result,&exact) && exact==INT64_C(9007199254741001));

    vm_value_unpin(owner,closure_pin);
    vm_value_unpin(owner,enum_pin);
    vm_value_unpin(owner,structure_pin);
    CHECK(vm_try_free(other));
    CHECK(vm_try_free(owner));
    return 0;
}
