/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/common.h"
#include "kirton/native.h"
#include "kirton/value.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

void fs_init(void);

int main(void){
    static const char path[]="kirton_fs_oom_input.txt";
    FILE* file=fopen(path,"wb");
    CHECK(file!=NULL);
    CHECK(fwrite("fs input",1,8,file)==8);
    CHECK(fclose(file)==0);

    KirValueHeap* heap=v_value_heap_new();
    CHECK(heap!=NULL);
    KirValueHeap* previous=v_value_heap_set_current(heap);
    fs_init();
    Value filename=v_str_copy(path,-1),result=v_nil();
    kir_allocator_test_fail_after(0);
    CHECK(kir_native_call("фс_прочитать_текст",strlen("фс_прочитать_текст"),
                          1,&filename,&result));
    CHECK(result.kind==VAL_STR && result.as.str.n==0);

    CHECK(kir_native_call("фс_прочитать_текст",strlen("фс_прочитать_текст"),
                          1,&filename,&result));
    CHECK(result.kind==VAL_STR && result.as.str.n==8 &&
          memcmp(result.as.str.s,"fs input",8)==0);

    v_value_heap_set_current(previous);
    v_value_heap_destroy(heap);
    CHECK(remove(path)==0);
    return 0;
}
