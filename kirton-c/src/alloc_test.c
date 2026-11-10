/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include <stddef.h>

#if defined(KIRTON_ALLOC_TESTING)
static size_t allocations_before_failure;
static int failure_armed;

void kir_allocator_test_fail_after(size_t successful_allocations){
    allocations_before_failure=successful_allocations;
    failure_armed=1;
}

int kir_allocator_test_should_fail(void){
    if (!failure_armed) return 0;
    if (allocations_before_failure){
        --allocations_before_failure;
        return 0;
    }
    failure_armed=0;
    return 1;
}
#else
typedef int kir_alloc_test_disabled_translation_unit;
#endif
