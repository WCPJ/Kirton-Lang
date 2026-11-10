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
#include <stdint.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr,"native-resource test line %d: %s\n",__LINE__,#condition); \
    return __LINE__; } } while (0)
void native_init(void);
void runtime_init_mod(void);
void json_init(void);

typedef struct {
    int cleanup_count;
    int sum;
    VM* vm;
    Value resource;
    int observed_blocked_close;
    int observed_blocked_vm_destroy;
    int gc_survived;
} State;

static void destroy_resource(void* handle, void* user_data){
    State* state=(State*)user_data;
    state->sum += *(int*)handle;
    ++state->cleanup_count;
    free(handle);
}

static int visit_resource(void* handle, void* user_data){
    State* state=(State*)user_data;
    *(int*)handle=123;
    state->observed_blocked_close=!vm_native_resource_close(state->vm,state->resource);
    state->observed_blocked_vm_destroy=!vm_try_free(state->vm);
    (void)vm_gc_collect(state->vm);
    state->gc_survived=vm_native_resource_is_open(state->vm,state->resource,"file-handle");
    return 1;
}

static Value make_resource(VM* vm, int number, State* state){
    int* handle=(int*)malloc(sizeof(*handle));
    if (!handle) return v_nil();
    *handle=number;
    Value resource=vm_native_resource_adopt(vm,handle,"file-handle",destroy_resource,state);
    if (resource.kind!=VAL_NATIVE_RESOURCE) free(handle);
    return resource;
}

int main(int argc, char** argv){
    CHECK(argc==2);
    native_init();
    runtime_init_mod();
    json_init();
    VM* first=vm_new();
    VM* second=vm_new();
    CHECK(first && second);
    CHECK(vm_run_root(first,argv[1]) && vm_run_root(second,argv[1]));
    State state={0};
    state.vm=first;
    Value resource=make_resource(first,7,&state);
    CHECK(resource.kind==VAL_NATIVE_RESOURCE);
    state.resource=resource;
    /* A copied Value with a forged identity must not be accepted even when
       its descriptor address is otherwise a valid member of this VM. */
    Value forged=resource;
    ++forged.integer_small;
    CHECK(!vm_native_resource_is_open(first,forged,"file-handle"));
    CHECK(!vm_native_resource_close(first,forged));
    CHECK(vm_value_pin(first,forged)==NULL);
    CHECK(vm_native_resource_is_open(first,resource,"file-handle"));
    CHECK(!vm_native_resource_is_open(first,resource,"wrong-handle"));
    CHECK(!vm_native_resource_is_open(second,resource,"file-handle"));
    CHECK(!vm_native_resource_close(second,resource));
    CHECK(!vm_native_resource_visit(second,resource,"file-handle",visit_resource,&state));
    CHECK(!vm_native_resource_visit(first,resource,"wrong-handle",visit_resource,&state));
    CHECK(!vm_native_resource_visit(first,resource,"file-handle",NULL,&state));
    CHECK(!vm_native_resource_adopt(NULL,(void*)1,"tag",destroy_resource,&state).kind);
    CHECK(vm_native_resource_adopt(first,NULL,"tag",destroy_resource,&state).kind==VAL_NIL);
    CHECK(vm_native_resource_adopt(first,(void*)1,"",destroy_resource,&state).kind==VAL_NIL);
    CHECK(vm_native_resource_adopt(first,(void*)1,"tag",NULL,&state).kind==VAL_NIL);

    /* Kirton itself can inspect/close a host-provided handle, without
       obtaining a raw pointer from it. */
    Value out=v_nil();
    CHECK(vm_call_by_name(first,"проверить_тип",1,&resource,&out));
    CHECK(out.kind==VAL_STR && out.as.str.n==strlen("внешний_ресурс") &&
          !memcmp(out.as.str.s,"внешний_ресурс",out.as.str.n));
    CHECK(vm_call_by_name(first,"открыт",1,&resource,&out));
    CHECK(out.kind==VAL_BOOL && out.as.boolean);
    /* Foreign resources cannot cross a VM boundary, even if nested. */
    Value nested=v_nil();
    CHECK(vm_call_by_name(first,"обернуть",1,&resource,&nested));
    CHECK(nested.kind==VAL_LIST && nested.as.list->len==1);
    CHECK(!vm_call_by_name(second,"обернуть",1,&nested,&out));
    KirPinnedValue* pin=vm_value_pin(first,nested);
    CHECK(pin);
    (void)vm_gc_collect(first);
    CHECK(state.cleanup_count==0);
    CHECK(vm_native_resource_is_open(first,resource,"file-handle"));
    state.resource=resource;
    CHECK(vm_native_resource_visit(first,resource,"file-handle",visit_resource,&state));
    CHECK(state.observed_blocked_close && state.observed_blocked_vm_destroy &&
          state.gc_survived && state.cleanup_count==0);
    CHECK(vm_call_by_name(first,"закрыть",1,&resource,&out));
    CHECK(out.kind==VAL_BOOL && out.as.boolean);
    CHECK(state.cleanup_count==1 && state.sum==123);
    CHECK(!vm_native_resource_is_open(first,resource,NULL));
    CHECK(!vm_native_resource_close(first,resource));
    CHECK(!vm_native_resource_visit(first,resource,"file-handle",visit_resource,&state));
    vm_value_unpin(first,pin);
    (void)vm_gc_collect(first);
    CHECK(state.cleanup_count==1);

    /* A nested list pinned by the host keeps a resource live across GC. */
    Value collection_resource=make_resource(first,31,&state);
    CHECK(collection_resource.kind==VAL_NATIVE_RESOURCE);
    CHECK(vm_call_by_name(first,"обернуть",1,&collection_resource,&nested));
    pin=vm_value_pin(first,nested);
    CHECK(pin);
    (void)vm_gc_collect(first);
    CHECK(state.cleanup_count==1);
    vm_value_unpin(first,pin);
    (void)vm_gc_collect(first);
    CHECK(state.cleanup_count==2 && state.sum==154);

    /* Resource with no roots is finalized exactly once by GC. */
    Value unreachable=make_resource(first,9,&state);
    CHECK(unreachable.kind==VAL_NATIVE_RESOURCE);
    (void)vm_gc_collect(first);
    CHECK(state.cleanup_count==3 && state.sum==163);
    (void)vm_gc_collect(first);
    CHECK(state.cleanup_count==3);

    /* Even when the native descriptor is still pinned, VM shutdown must
       deterministically close the external handle. */
    VM* short_lived=vm_new();
    CHECK(short_lived);
    State shutdown={0};
    Value alive=make_resource(short_lived,19,&shutdown);
    CHECK(alive.kind==VAL_NATIVE_RESOURCE);
    KirPinnedValue* keep=vm_value_pin(short_lived,alive);
    CHECK(keep);
    CHECK(vm_try_free(short_lived));
    CHECK(shutdown.cleanup_count==1 && shutdown.sum==19);

    /* Several resources closed and collected under allocation pressure. */
    for (int i=0;i<1200;++i){
        Value temporary=make_resource(first,1,&state);
        CHECK(temporary.kind==VAL_NATIVE_RESOURCE);
        if (i%2==0) CHECK(vm_native_resource_close(first,temporary));
        if (i%13==0) (void)vm_gc_collect(first);
    }
    (void)vm_gc_collect(first);
    CHECK(state.cleanup_count==1203);
    CHECK(state.sum==1363);
    /* JSON must reject opaque resources rather than leaking raw handles. */
    Value unserializable=make_resource(first,5,&state);
    CHECK(unserializable.kind==VAL_NATIVE_RESOURCE);
    CHECK(!vm_call_by_name(first,"попытка_json",1,&unserializable,&out));
    CHECK(vm_native_resource_is_open(first,unserializable,"file-handle"));
    (void)vm_gc_collect(first);
    CHECK(state.cleanup_count==1204 && state.sum==1368);
    CHECK(vm_try_free(first));
    CHECK(vm_try_free(second));
    /* Relinquishing a raw heap must not orphan an adopted native handle. */
    KirValueHeap* raw=v_value_heap_new();
    CHECK(raw);
    int* raw_handle=(int*)malloc(sizeof(*raw_handle));
    CHECK(raw_handle);
    *raw_handle=4;
    State raw_state={0};
    Value raw_resource=v_native_resource_adopt(raw,raw_handle,"raw",destroy_resource,&raw_state);
    CHECK(raw_resource.kind==VAL_NATIVE_RESOURCE);
    v_value_heap_release_all(raw);
    CHECK(raw_state.cleanup_count==1 && raw_state.sum==4);
    v_value_heap_destroy(raw);
    CHECK(raw_state.cleanup_count==1);
    return 0;
}
