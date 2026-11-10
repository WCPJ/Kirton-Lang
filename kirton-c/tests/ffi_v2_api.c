/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/eval.h"
#include "kirton/value.h"
#include "kirton/native.h"
#include "kirton/common.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FFI_V2 check line %d: %s\n",__LINE__,#x); return __LINE__; } } while(0)
void native_init(void);

int main(int argc,char** argv){
    CHECK(argc==4);
    native_init();
    VM* first=vm_new();
    VM* second=vm_new();
    CHECK(first && second);
    CHECK(!vm_ffi_load(first,argv[1])); /* default: forbidden */
    CHECK(strstr(vm_ffi_last_error(first),"отключены")!=NULL);
    CHECK(vm_ffi_set_enabled(first,1));
    CHECK(vm_ffi_set_enabled(second,1));
    CHECK(!vm_ffi_load(first,"/unavailable/library/ffi.so"));
    CHECK(!vm_ffi_load(first,argv[3]));
    CHECK(strstr(vm_ffi_last_error(first),"ABI")!=NULL);
    CHECK(vm_ffi_load(first,argv[1]));
    CHECK(!vm_ffi_load(first,argv[1])); /* duplicate module names */
    CHECK(vm_ffi_load(second,argv[1]));
    Value a[]={v_int(INT64_C(9007199254740993)),v_int(7)};
    Value result=v_nil();
    CHECK(vm_ffi_call(first,"арифметика","сложить",2,a,&result));
    int64_t exact=0;
    CHECK(v_num_fits_i64(result,&exact) && exact==INT64_C(9007199254741000));
    CHECK(!vm_ffi_call(first,"арифметика","сложить",1,a,&result));
    CHECK(!vm_ffi_call(first,"арифметика","сложить",2,(Value[]){v_bool(true),v_int(7)},&result));
    CHECK(!vm_ffi_call(first,"арифметика","неправильный_ответ",0,NULL,&result));
    CHECK(!vm_ffi_call(first,"арифметика","отказ",0,NULL,&result));
    CHECK(!vm_ffi_call(first,"арифметика","отсутствует",0,NULL,&result));
    Value decimals[]={v_num(1.5),v_num(4.0)};
    CHECK(vm_ffi_call(first,"арифметика","умножить",2,decimals,&result));
    CHECK(result.kind==VAL_NUM && v_num_to_double(result)==6.0);
    Value special[]={v_num(NAN),v_num(1.0)};
    CHECK(vm_ffi_call(first,"арифметика","умножить",2,special,&result));
    CHECK(result.kind==VAL_NUM && isnan(v_num_to_double(result)));
    special[0]=v_num(INFINITY);
    CHECK(vm_ffi_call(first,"арифметика","умножить",2,special,&result));
    CHECK(result.kind==VAL_NUM && isinf(v_num_to_double(result)) &&
          v_num_to_double(result)>0.0);
    Value logical=v_bool(false);
    CHECK(vm_ffi_call(first,"арифметика","инвертировать",1,&logical,&result));
    CHECK(result.kind==VAL_BOOL && result.as.boolean);
    CHECK(vm_ffi_call(first,"арифметика","пусто",0,NULL,&result));
    CHECK(result.kind==VAL_NIL);
    const char raw[]={ 'A',0,'B' };
    Value input=v_str_copy(raw,3);
    CHECK(vm_ffi_call(first,"арифметика","повторить",1,&input,&result));
    CHECK(result.kind==VAL_STR && result.as.str.n==3 &&
          !memcmp(result.as.str.s,raw,3));
    kir_free(input.as.str.s); /* created outside a VM heap */
    KirPinnedValue* root=vm_value_pin(first,result);
    CHECK(root);
    (void)vm_gc_collect(first);
    CHECK(vm_value_pinned(root).kind==VAL_STR &&
          !memcmp(vm_value_pinned(root).as.str.s,raw,3));
    vm_value_unpin(first,root);

    /* The Kirton syntax runs through the same VM-scoped FFI, including
       module loading and exact integer return conversion. */
    Value path=v_str_copy(argv[1],-1);
    /* second already loaded this plugin above; loading again is a failure.
       Script needs a separate, fresh VM for this end-to-end path. */
    VM* script=vm_new();
    CHECK(script && vm_ffi_set_enabled(script,1));
    CHECK(vm_run_root(script,argv[2])!=NULL);
    CHECK(vm_call_by_name(script,"проверить_внешний",1,&path,&result));
    CHECK(v_num_fits_i64(result,&exact) && exact==INT64_C(9007199254741000));
    kir_free(path.as.str.s);
    (void)vm_gc_collect(script);
    CHECK(vm_try_free(script));
    CHECK(vm_try_free(first));
    CHECK(vm_try_free(second));
    puts("FFI_V2_OK");
    return 0;
}
