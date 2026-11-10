/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/eval.h"

#define CHECK(c) do { if (!(c)) return __LINE__; } while (0)

static int call_is(VM* vm, const char* name, int64_t expected){
    Value result = v_nil();
    int64_t actual = 0;
    return vm_call_by_name(vm, name, 0, NULL, &result) &&
           v_num_fits_i64(result, &actual) && actual == expected;
}

int main(int argc, char** argv){
    CHECK(argc == 2);
    VM* first = vm_new();
    VM* second = vm_new();
    CHECK(first && second);
    CHECK(vm_run_root(first, argv[1]) != NULL);
    CHECK(call_is(first, "next", 1));
    CHECK(call_is(first, "next", 2));
    CHECK(vm_run_root(second, argv[1]) != NULL);
    CHECK(call_is(second, "read", 0));
    CHECK(call_is(second, "next", 1));
    CHECK(call_is(first, "read", 2));
    CHECK(call_is(first, "next", 3));
    CHECK(call_is(second, "read", 1));
    vm_free(second);
    vm_free(first);
    return 0;
}
