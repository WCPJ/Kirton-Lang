/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/eval.h"
#include "kirton/native.h"
#include "kirton/diag.h"

#include <stdlib.h>

#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)

static VM* active_vm;
static int destroy_count;

static void destroy_resource(void* handle, void* user_data){
    (void)user_data;
    ++destroy_count;
    free(handle);
}

static Value create_resource(int argc, const Value* argv){
    (void)argc;
    (void)argv;
    void* handle=malloc(1);
    if (!handle) return v_nil();
    Value resource=vm_native_resource_adopt(active_vm,handle,"truthiness-test",
                                            destroy_resource,NULL);
    if (resource.kind!=VAL_NATIVE_RESOURCE) free(handle);
    return resource;
}

void native_init(void);

int main(int argc, char** argv){
    CHECK(argc==2);
    native_init();
    static const KirNativeDef functions[]={{"создать",create_resource}};
    kir_native_register_module("test_resource",functions,1);

    VM* vm=vm_new();
    CHECK(vm!=NULL);
    active_vm=vm;
    kir_diag_reset_errors();
    CHECK(vm_run_root(vm,argv[1])!=NULL);
    CHECK(kir_diag_error_count()==0);
    CHECK(destroy_count==1);
    vm_free(vm);
    CHECK(destroy_count==1);
    active_vm=NULL;
    return 0;
}
