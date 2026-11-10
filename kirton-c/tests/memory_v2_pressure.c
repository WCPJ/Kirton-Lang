/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/eval.h"
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
void native_init(void);
int main(int argc, char** argv){
    CHECK(argc==2);
    native_init();
    VM* vm=vm_new();
    CHECK(vm);
    CHECK(vm_run_root(vm,argv[1]));
    CHECK(vm_gc_collections(vm)>=4);
    CHECK(vm_memory_arena_bytes(vm)<17u*1024u*1024u);
    (void)vm_gc_collect(vm);
    CHECK(vm_memory_arena_bytes(vm)==0);
    vm_free(vm);
    return 0;
}
