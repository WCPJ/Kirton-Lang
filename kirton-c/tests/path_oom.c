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

void path_init(void);

int main(void){
    KirValueHeap* heap=v_value_heap_new();
    CHECK(heap!=NULL);
    KirValueHeap* previous=v_value_heap_set_current(heap);
    path_init();

    Value arguments[]={v_str_copy("alpha",-1),v_str_copy("beta",-1)};
    Value result=v_nil();
    for (size_t fail_after=0;fail_after<5;++fail_after){
        kir_allocator_test_fail_after(fail_after);
        CHECK(kir_native_call("путь_соединить",strlen("путь_соединить"),
                              2,arguments,&result));
        CHECK(result.kind==VAL_STR && result.as.str.n==0);
    }

    CHECK(kir_native_call("путь_соединить",strlen("путь_соединить"),
                          2,arguments,&result));
#ifdef _WIN32
    static const char expected[]="alpha\\beta";
#else
    static const char expected[]="alpha/beta";
#endif
    CHECK(result.kind==VAL_STR && result.as.str.n==strlen(expected) &&
          memcmp(result.as.str.s,expected,sizeof(expected)-1)==0);

    v_value_heap_set_current(previous);
    v_value_heap_destroy(heap);
    return 0;
}
