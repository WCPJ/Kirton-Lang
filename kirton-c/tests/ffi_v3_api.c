/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/eval.h"
#include "kirton/value.h"
#include "kirton/common.h"
#include "kirton/native.h"
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(expr) do {if (!(expr)) {fprintf(stderr,"FFI_V3 line %d: %s\n",__LINE__,#expr);return __LINE__;}}while(0)
void native_init(void);
static int destroy_count=0;
static void destroy_counter(void* ptr,void* context){
    (void)context;
    ++destroy_count;
    free(ptr);
}
int main(int argc,char** argv){
    CHECK(argc==4);
    native_init();
    VM* vm=vm_new();
    VM* other=vm_new();
    CHECK(vm && other);
    Value out=v_nil();
    CHECK(vm_ffi_set_enabled(vm,1) && vm_ffi_set_enabled(other,1));
    CHECK(!vm_ffi_load(vm,argv[3]));
    CHECK(strstr(vm_ffi_last_error(vm),"ABI")!=NULL);
#ifdef _WIN32
    kir_allocator_test_fail_after(0);
    CHECK(!vm_ffi_load(vm,argv[1]));
    CHECK(strstr(vm_ffi_last_error(vm),"Не удалось загрузить")!=NULL);
#endif
    CHECK(vm_ffi_load(vm,argv[1]));
    CHECK(!vm_ffi_load(vm,argv[1]));
    CHECK(vm_ffi_load(other,argv[1]));
    Value special=v_num(NAN);
    CHECK(vm_ffi_call(vm,"структуры","классифицировать_f64",1,&special,&out));
    int64_t classification=0;
    CHECK(v_num_fits_i64(out,&classification) && classification==1);
    special=v_num(INFINITY);
    CHECK(vm_ffi_call(vm,"структуры","классифицировать_f64",1,&special,&out));
    CHECK(v_num_fits_i64(out,&classification) && classification==2);
    CHECK(!vm_ffi_call(vm,"структуры","вернуть_nan",0,NULL,&out));
    CHECK(vm_run_root(vm,argv[2])!=NULL);
    Value path=v_str_copy(argv[1],-1);
    /* Script uses the same dispatcher and tests C struct snapshots. */
    CHECK(vm_call_by_name(vm,"подготовить_запись",0,NULL,&out));
    CHECK(out.kind==VAL_RECORD);
    KirPinnedValue* pin=vm_value_pin(vm,out);
    CHECK(pin);
    Value actor=vm_value_pinned(pin);
    /* A failed FFI snapshot allocation must release its borrow and leave the
       VM usable for the next call. */
    kir_allocator_test_fail_after(2); /* both validation walks succeed; FFI record copy fails */
    CHECK(!vm_ffi_call(vm,"структуры","проверить",1,&actor,&out));
    CHECK(vm_ffi_call(vm,"структуры","проверить",1,&actor,&out));
    int64_t exact=0;
    CHECK(v_num_fits_i64(out,&exact) && exact==INT64_C(9007199254740993));
    /* Plugin modified its argument snapshot, NOT the Kirton arena. */
    Value health=v_nil();
    CHECK(vm_memory_record_get(vm,actor,"здоровье",strlen("здоровье"),&health));
    CHECK(v_num_fits_i64(health,&exact) && exact==42);
    CHECK(!vm_ffi_call(vm,"структуры","неверная_схема",1,&actor,&out));
    CHECK(!vm_ffi_call(vm,"структуры","плохая_запись",1,&actor,&out));
    CHECK(vm_ffi_call(vm,"структуры","копировать",1,&actor,&out));
    CHECK(out.kind==VAL_RECORD);
    KirPinnedValue* copied=vm_value_pin(vm,out);
    CHECK(copied);
    (void)vm_gc_collect(vm);
    CHECK(vm_memory_record_get(vm,vm_value_pinned(copied),
                               "идентификатор",strlen("идентификатор"),&health));
    CHECK(v_num_fits_i64(health,&exact) && exact==INT64_C(9007199254740993));
    for (int i=0;i<10000;++i){
        CHECK(vm_ffi_call(vm,"структуры","проверить",1,&actor,&out));
        CHECK(v_num_fits_i64(out,&exact) && exact==INT64_C(9007199254740993));
        if ((i%128)==127) (void)vm_gc_collect(vm);
    }
    CHECK(!vm_ffi_call(other,"структуры","проверить",1,&actor,&out));
    CHECK(!vm_ffi_call(vm,"структуры","увеличить",1,&actor,&out));
    int* counter=(int*)malloc(sizeof(*counter));
    CHECK(counter);
    *counter=10;
    Value resource=vm_native_resource_adopt(vm,counter,"test-counter",destroy_counter,NULL);
    CHECK(resource.kind==VAL_NATIVE_RESOURCE);
    KirPinnedValue* keep=vm_value_pin(vm,resource);
    CHECK(keep);
    CHECK(vm_ffi_call(vm,"структуры","увеличить",1,&resource,&out));
    CHECK(v_num_fits_i64(out,&exact) && exact==17 && *counter==17);
    CHECK(vm_ffi_call(vm,"структуры","вернуть_ресурс",1,&resource,&out));
    CHECK(out.kind==VAL_NATIVE_RESOURCE && out.as.native_resource==resource.as.native_resource);
    CHECK(!vm_ffi_call(vm,"структуры","подделать_ресурс",1,&resource,&out));
    CHECK(!vm_ffi_call(other,"структуры","увеличить",1,&resource,&out));
    CHECK(vm_native_resource_close(vm,resource));
    CHECK(destroy_count==1);
    CHECK(!vm_ffi_call(vm,"структуры","увеличить",1,&resource,&out));
    vm_value_unpin(vm,keep);
    (void)vm_gc_collect(vm);
    CHECK(destroy_count==1);
    CHECK(vm_ffi_call(vm,"структуры","проверить",1,&actor,&out));
    CHECK(vm_value_pinned(pin).kind==VAL_RECORD);
    vm_value_unpin(vm,copied);
    vm_value_unpin(vm,pin);
    /* Verify execution from Kirton script, not merely C embedding API. */
    VM* script=vm_new();
    CHECK(script && vm_ffi_set_enabled(script,1));
    CHECK(vm_run_root(script,argv[2])!=NULL);
    CHECK(vm_call_by_name(script,"проверить_копирование",1,&path,&out));
    CHECK(v_num_fits_i64(out,&exact) && exact==INT64_C(9007199254740993));
    CHECK(vm_try_free(script));
    kir_free(path.as.str.s);
    CHECK(vm_try_free(other));
    CHECK(vm_try_free(vm));
    CHECK(destroy_count==1);
    puts("FFI_V3_OK");
    return 0;
}
