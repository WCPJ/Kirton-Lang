/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/common.h"
#include <string.h>

int main(int argc,char** argv){
    if (argc!=2) return 2;
    if (!strcmp(argv[1],"malloc")){
        kir_allocator_test_fail_after(0);
        (void)kir_malloc(16);
    } else if (!strcmp(argv[1],"realloc")){
        void* memory=kir_malloc(8);
        kir_allocator_test_fail_after(0);
        (void)kir_realloc(memory,32);
    } else if (!strcmp(argv[1],"calloc")){
        kir_allocator_test_fail_after(0);
        (void)kir_callocn(4,8);
    } else return 2;
    return 0;
}
