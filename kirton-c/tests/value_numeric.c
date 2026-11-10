/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/value.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

int main(void){
    int out = 0;
    CHECK(v_num_to_int_trunc(v_int(42), &out) && out == 42);
    CHECK(v_num_to_int_trunc(v_num(-12.9), &out) && out == -12);
    CHECK(!v_num_to_int_trunc(v_num((double)INT_MAX + 1.0), &out));

    Value huge_integer = v_int_parse("9007199254740993", 16, 10);
    CHECK(!v_num_to_int_trunc(huge_integer, &out));

    Value negative_zero = v_num_mod(v_num(6.0), v_num(-3.0));
    size_t zero_len = 0;
    char* zero_text = v_num_to_string(negative_zero, &zero_len);
    CHECK(zero_text != NULL && zero_len == 4 && memcmp(zero_text, "-0.0", 4) == 0);
    free(zero_text);

    Value max_size = v_int_from_size(SIZE_MAX);
    size_t actual_len = 0;
    char* actual = v_num_to_string(max_size, &actual_len);
    char expected[3*sizeof(size_t) + 1];
    int expected_len = snprintf(expected, sizeof(expected), "%zu", SIZE_MAX);
    CHECK(actual != NULL && expected_len > 0);
    CHECK(actual_len == (size_t)expected_len && memcmp(actual, expected, actual_len) == 0);
    free(actual);
    /* Outside a VM heap, the C caller owns parsed BigInt values. */
    if (huge_integer.integer_big) kir_bigint_free(huge_integer.integer_big);
    if (max_size.integer_big) kir_bigint_free(max_size.integer_big);
    return 0;
}
