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

void timers_init(void);

int main(void){
    timers_init();
    Value delay=v_int(100000),result=v_nil();
    kir_allocator_test_fail_after(0);
    CHECK(kir_native_call("таймер_после",strlen("таймер_после"),1,&delay,&result));
    int64_t id=0;
    CHECK(v_num_fits_i64(result,&id) && id==0);

    CHECK(kir_native_call("таймер_после",strlen("таймер_после"),1,&delay,&result));
    CHECK(v_num_fits_i64(result,&id) && id>0);
    Value timer_id=v_int(id);
    CHECK(kir_native_call("таймер_отменить",strlen("таймер_отменить"),1,
                          &timer_id,&result));
    CHECK(result.kind==VAL_BOOL && result.as.boolean);
    return 0;
}
