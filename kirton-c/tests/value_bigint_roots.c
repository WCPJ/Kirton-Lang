/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/value.h"
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) return __LINE__; } while (0)

int main(void){
    const char digits[] = "123456789012345678901234567890";
    KirBigIntHeap* heap = kir_bigint_heap_new();
    CHECK(heap != NULL);
    KirBigIntHeap* previous = v_bigint_heap_set_current(heap);
    Value shared = v_int_parse(digits, strlen(digits), 10);
    Value unreachable = v_int_parse("987654321098765432109876543210", 30, 10);
    CHECK(shared.integer_big && unreachable.integer_big);
    CHECK(kir_bigint_heap_count(heap) == 2);

    KirValueHeap* values=v_value_heap_new();
    KirValueHeap* previous_values=v_value_heap_set_current(values);
    Value list = v_list_new();
    Value dict = v_dict_new();
    v_list_push(&list, shared);
    v_dict_set(&dict, "number", 6, shared);
    v_list_push(&list, dict);
    v_dict_set(&dict, "back", 4, list);
    v_mark_bigints(heap, list);
    v_mark_bigints(heap, list);
    CHECK(kir_bigint_heap_sweep(heap) == 1);
    CHECK(kir_bigint_heap_count(heap) == 1);
    size_t n = 0;
    char* text = v_num_to_string(shared, &n);
    CHECK(text && n == strlen(digits) && memcmp(text, digits, n) == 0);
    free(text);
    v_mark_bigints(heap, dict);
    CHECK(kir_bigint_heap_sweep(heap) == 0);
    CHECK(kir_bigint_heap_sweep(heap) == 1);
    CHECK(kir_bigint_heap_count(heap) == 0);
    v_bigint_heap_set_current(previous);
    v_value_heap_set_current(previous_values);
    v_value_heap_destroy(values);
    kir_bigint_heap_destroy(heap);
    return 0;
}
