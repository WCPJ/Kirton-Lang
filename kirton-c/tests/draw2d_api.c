/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/native.h"

#include <stdint.h>
#include <string.h>

void draw2d_init(void);

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

static int call(const char* name, int argc, const Value* args, Value* out){
    return kir_native_call(name, strlen(name), argc, args, out);
}

int main(void){
    draw2d_init();

    Value dimensions[] = {v_int(2), v_int(2)};
    Value image = v_nil();
    CHECK(call("img_создать", 2, dimensions, &image));
    int64_t image_id = -1;
    CHECK(v_num_fits_i64(image, &image_id) && image_id >= 0);

    Value rect[] = {image, v_int(0), v_int(0), v_int(2), v_int(2),
                    v_int(255), v_int(0), v_int(0), v_int(255), v_int(1)};
    Value result = v_nil();
    CHECK(call("прямоугольник", 9, rect, &result));
    CHECK(result.kind == VAL_BOOL && result.as.boolean);
    CHECK(call("прямоугольник", 10, rect, &result));
    CHECK(result.kind == VAL_BOOL && result.as.boolean);

    Value circle[] = {image, v_int(1), v_int(1), v_int(1),
                      v_int(0), v_int(255), v_int(0), v_int(255), v_int(1)};
    CHECK(call("окружность", 8, circle, &result));
    CHECK(result.kind == VAL_BOOL && result.as.boolean);
    CHECK(call("окружность", 9, circle, &result));
    CHECK(result.kind == VAL_BOOL && result.as.boolean);

    Value invalid_dimensions[] = {v_int_parse("2147483648", 10, 10), v_int(2)};
    CHECK(call("img_создать", 2, invalid_dimensions, &result));
    int64_t invalid_id = 0;
    CHECK(v_num_fits_i64(result, &invalid_id) && invalid_id == -1);
    return 0;
}
