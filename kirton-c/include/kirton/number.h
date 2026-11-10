/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct KirBigInt KirBigInt;
typedef struct KirBigIntHeap KirBigIntHeap;

/* Optional ownership for runtime integers. Fresh BigInts are unmanaged.
   manage returns 0 if the value already belongs to another heap.
   mark accepts only values owned by this heap. sweep frees unmarked values
   and clears the marks of survivors. All live aliases must be marked first.
   destroy invalidates every value still owned by the heap. */
KirBigIntHeap* kir_bigint_heap_new(void);
void           kir_bigint_heap_destroy(KirBigIntHeap* heap);
int            kir_bigint_heap_manage(KirBigIntHeap* heap, KirBigInt* value);
/* Accepts unmanaged caller-owned integers and integers owned by this heap only. */
int            kir_bigint_heap_accepts_value(const KirBigIntHeap* heap,
                                              const KirBigInt* value);
int            kir_bigint_heap_mark(KirBigIntHeap* heap, KirBigInt* value);
size_t         kir_bigint_heap_sweep(KirBigIntHeap* heap);
size_t         kir_bigint_heap_count(const KirBigIntHeap* heap);
/* Отсоединить выжившие числа от кучи, сохранив их для внешних владельцев. */
size_t         kir_bigint_heap_release_all(KirBigIntHeap* heap);

/* Immutable arbitrary-precision signed integers. */
KirBigInt* kir_bigint_parse(const char* text, size_t len, unsigned int base);
KirBigInt* kir_bigint_from_i64(int64_t value);
KirBigInt* kir_bigint_add(const KirBigInt* a, const KirBigInt* b);
KirBigInt* kir_bigint_sub(const KirBigInt* a, const KirBigInt* b);
KirBigInt* kir_bigint_mul(const KirBigInt* a, const KirBigInt* b);
/* Floor division and modulo, with modulo carrying the divisor's sign. */
int        kir_bigint_divmod_floor(const KirBigInt* a, const KirBigInt* b,
                                   KirBigInt** quotient, KirBigInt** remainder);
KirBigInt* kir_bigint_bitwise(const KirBigInt* a, const KirBigInt* b, char op);
KirBigInt* kir_bigint_bitnot(const KirBigInt* value);
KirBigInt* kir_bigint_shift_left(const KirBigInt* value, size_t bits);
KirBigInt* kir_bigint_shift_right(const KirBigInt* value, size_t bits);
void       kir_bigint_free(KirBigInt* value);
int        kir_bigint_compare(const KirBigInt* a, const KirBigInt* b);
int        kir_bigint_fits_i64(const KirBigInt* value, int64_t* out);
double     kir_bigint_to_double(const KirBigInt* value);
char*      kir_bigint_to_string(const KirBigInt* value, size_t* out_len);

/* Compare an exact integer with an IEEE-754 double; returns 0 for NaN. */
int kir_bigint_compare_double(const KirBigInt* integer, double real, int* out_compare);
