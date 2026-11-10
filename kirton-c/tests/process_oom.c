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

void process_init(void);

static int run(const Value* command,Value* result){
    return kir_native_call("процесс_выполнить",strlen("процесс_выполнить"),
                           1,command,result);
}

static int has_error(Value result){
    Value error=v_nil();
    return result.kind==VAL_DICT &&
           v_dict_get(&result,"ошибка",strlen("ошибка"),&error) &&
           error.kind==VAL_STR && error.as.str.n>0;
}

int main(void){
    KirValueHeap* heap=v_value_heap_new();
    CHECK(heap!=NULL);
    KirValueHeap* previous=v_value_heap_set_current(heap);
    process_init();
#ifdef _WIN32
    Value command=v_str_copy("cmd.exe /C \"for /L %i in (1,1,10000) do @echo capture\"",-1);
    Value quick_command=v_str_copy("cmd.exe /C echo quick_capture",-1);
#else
    Value command=v_str_copy("yes capture | head -n 10000",-1);
    Value quick_command=v_str_copy("printf quick_capture",-1);
#endif
    Value result=v_nil(),code_value=v_nil();
    int64_t code=0;

    for (int i=0;i<5;++i){
        CHECK(run(&quick_command,&result));
        CHECK(v_dict_get(&result,"код",strlen("код"),&code_value));
        CHECK(v_num_fits_i64(code_value,&code) && code==0);
        Value quick_output=v_nil();
        CHECK(v_dict_get(&result,"stdout",strlen("stdout"),&quick_output));
        CHECK(quick_output.kind==VAL_STR &&
              strstr(quick_output.as.str.s,"quick_capture")!=NULL);
    }

    kir_allocator_test_fail_after(0);
    CHECK(run(&command,&result));
    CHECK(has_error(result));
    CHECK(v_dict_get(&result,"код",strlen("код"),&code_value));
    CHECK(v_num_fits_i64(code_value,&code) && code==-1);

    kir_allocator_test_fail_after(1);
    CHECK(run(&command,&result));
    CHECK(has_error(result));

    CHECK(run(&command,&result));
    CHECK(v_dict_get(&result,"код",strlen("код"),&code_value));
    CHECK(v_num_fits_i64(code_value,&code) && code==0);
    Value output=v_nil();
    CHECK(v_dict_get(&result,"stdout",strlen("stdout"),&output));
    CHECK(output.kind==VAL_STR && strstr(output.as.str.s,"capture")!=NULL);

    v_value_heap_set_current(previous);
    v_value_heap_destroy(heap);
    return 0;
}
