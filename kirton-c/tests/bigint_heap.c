/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/number.h"
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) return __LINE__; } while (0)

int main(void){
    const char digits[] = "123456789012345678901234567890";
    KirBigIntHeap* first = kir_bigint_heap_new();
    KirBigIntHeap* second = kir_bigint_heap_new();
    KirBigInt* retained = kir_bigint_parse(digits, strlen(digits), 10);
    KirBigInt* discarded = kir_bigint_from_i64(7);
    KirBigInt* other = kir_bigint_from_i64(11);
    KirBigInt* unmanaged = kir_bigint_from_i64(13);
    CHECK(first && second && retained && discarded && other && unmanaged);
    CHECK(kir_bigint_heap_manage(first, retained));
    CHECK(kir_bigint_heap_manage(first, retained));
    CHECK(!kir_bigint_heap_manage(second, retained));
    CHECK(!kir_bigint_heap_mark(second, retained));
    CHECK(!kir_bigint_heap_mark(first, unmanaged));
    CHECK(kir_bigint_heap_manage(first, discarded));
    CHECK(kir_bigint_heap_manage(second, other));
    CHECK(kir_bigint_heap_count(first)==2 && kir_bigint_heap_count(second)==1);
    CHECK(kir_bigint_heap_mark(first, retained));
    CHECK(kir_bigint_heap_sweep(first)==1 && kir_bigint_heap_count(first)==1);
    CHECK(kir_bigint_heap_count(second)==1);
    size_t n=0;
    char* text=kir_bigint_to_string(retained, &n);
    CHECK(text && n==strlen(digits) && memcmp(text,digits,n)==0);
    free(text);
    CHECK(kir_bigint_heap_sweep(first)==1 && kir_bigint_heap_count(first)==0);
    KirBigInt* explicitly_freed=kir_bigint_from_i64(99);
    CHECK(kir_bigint_heap_manage(first, explicitly_freed));
    kir_bigint_free(explicitly_freed);
    CHECK(kir_bigint_heap_count(first)==0 && kir_bigint_heap_sweep(first)==0);
    KirBigInt* detached = kir_bigint_parse(digits, strlen(digits), 10);
    KirBigInt* detached_small = kir_bigint_from_i64(123);
    CHECK(detached && detached_small);
    CHECK(kir_bigint_heap_manage(first, detached));
    CHECK(kir_bigint_heap_manage(first, detached_small));
    CHECK(kir_bigint_heap_mark(first, detached));
    CHECK(kir_bigint_heap_release_all(first)==2 && kir_bigint_heap_count(first)==0);
    CHECK(kir_bigint_heap_release_all(first)==0 && kir_bigint_heap_sweep(first)==0);
    kir_bigint_free(unmanaged);
    kir_bigint_heap_destroy(first);
    n=0;
    text=kir_bigint_to_string(detached, &n);
    CHECK(text && n==strlen(digits) && memcmp(text,digits,n)==0);
    free(text);
    int64_t small=0;
    CHECK(kir_bigint_fits_i64(detached_small, &small) && small==123);
    kir_bigint_free(detached);
    kir_bigint_free(detached_small);
    kir_bigint_heap_destroy(second);
    return 0;
}
