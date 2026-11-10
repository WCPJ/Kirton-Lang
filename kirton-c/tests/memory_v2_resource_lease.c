/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/eval.h"
#include "kirton/native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if (!(x)) {fprintf(stderr,"LEASE test line %d: %s\n",__LINE__,#x);return __LINE__;}}while(0)
void native_init(void);
typedef struct {
    VM* vm;
    KirNativeResourceLease* lease;
    Value resource;
    int calls;
    int destructor_calls;
    int close_blocked;
    int shutdown_blocked;
    int early_release_blocked;
} Context;
static void release_counter(void* handle,void* user_data){
    Context* ctx=(Context*)user_data;
    ++ctx->destructor_calls;
    free(handle);
}
static int submit_async_like(void* handle,void* user_data){
    int* value=(int*)handle;
    Context* ctx=(Context*)user_data;
    ++ctx->calls;
    *value+=1;
    ctx->close_blocked=!vm_native_resource_close(ctx->vm,ctx->resource);
    ctx->shutdown_blocked=!vm_try_free(ctx->vm);
    ctx->early_release_blocked=!vm_native_resource_lease_release(ctx->vm,ctx->lease);
    (void)vm_gc_collect(ctx->vm);
    return 1;
}
int main(void){
    native_init();
    VM* vm=vm_new();
    VM* foreign=vm_new();
    CHECK(vm && foreign);
    Context ctx={0};
    ctx.vm=vm;
    int* handle=(int*)malloc(sizeof(*handle));
    CHECK(handle);*handle=100;
    ctx.resource=vm_native_resource_adopt(vm,handle,"gpu-buffer",release_counter,&ctx);
    CHECK(ctx.resource.kind==VAL_NATIVE_RESOURCE);
    CHECK(vm_native_resource_lease_begin(foreign,ctx.resource,"gpu-buffer")==NULL);
    CHECK(vm_native_resource_lease_begin(vm,ctx.resource,"wrong")==NULL);
    ctx.lease=vm_native_resource_lease_begin(vm,ctx.resource,"gpu-buffer");
    CHECK(ctx.lease && vm_native_resource_lease_count(vm)==1);
    KirNativeResourceLease* second=vm_native_resource_lease_begin(vm,ctx.resource,"gpu-buffer");
    CHECK(second && vm_native_resource_lease_count(vm)==2);
    CHECK(!vm_native_resource_close(vm,ctx.resource));
    CHECK(!vm_try_free(vm));
    (void)vm_gc_collect(vm);
    CHECK(ctx.destructor_calls==0 && *handle==100);
    CHECK(!vm_native_resource_lease_visit(foreign,ctx.lease,submit_async_like,&ctx));
    CHECK(vm_native_resource_lease_visit(vm,ctx.lease,submit_async_like,&ctx));
    CHECK(ctx.calls==1 && *handle==101);
    CHECK(ctx.close_blocked && ctx.shutdown_blocked && ctx.early_release_blocked);
    CHECK(vm_native_resource_lease_release(vm,ctx.lease));
    CHECK(!vm_native_resource_lease_release(vm,ctx.lease));
    CHECK(vm_native_resource_lease_count(vm)==1);
    (void)vm_gc_collect(vm);
    CHECK(ctx.destructor_calls==0);
    CHECK(vm_native_resource_lease_release(vm,second));
    CHECK(vm_native_resource_lease_count(vm)==0);
    CHECK(vm_native_resource_close(vm,ctx.resource));
    CHECK(ctx.destructor_calls==1);
    CHECK(vm_native_resource_lease_begin(vm,ctx.resource,"gpu-buffer")==NULL);
    (void)vm_gc_collect(vm);
    CHECK(ctx.destructor_calls==1);
    /* A lease keeps an otherwise unreachable resource alive until release. */
    Context retained={0};
    retained.vm=vm;
    int* other=(int*)malloc(sizeof(*other));
    CHECK(other);*other=17;
    Value orphan=vm_native_resource_adopt(vm,other,"gpu-buffer",release_counter,&retained);
    CHECK(orphan.kind==VAL_NATIVE_RESOURCE);
    KirNativeResourceLease* retained_lease=vm_native_resource_lease_begin(vm,orphan,"gpu-buffer");
    CHECK(retained_lease);
    (void)vm_gc_collect(vm);
    CHECK(retained.destructor_calls==0);
    CHECK(vm_native_resource_lease_release(vm,retained_lease));
    (void)vm_gc_collect(vm);
    CHECK(retained.destructor_calls==1);
    /* A resource not manually closed is released by VM shutdown. */
    Context shutdown={0};
    int* last=(int*)malloc(sizeof(*last));
    CHECK(last);*last=1;
    Value shutdown_resource=vm_native_resource_adopt(vm,last,"gpu-buffer",release_counter,&shutdown);
    CHECK(shutdown_resource.kind==VAL_NATIVE_RESOURCE);
    CHECK(vm_try_free(foreign));
    CHECK(vm_try_free(vm));
    CHECK(shutdown.destructor_calls==1);
    puts("NATIVE_RESOURCE_LEASE_OK");
    return 0;
}
