/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/ffi.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#ifdef _WIN32
#  define FFI_EXPORT __declspec(dllexport)
#else
#  define FFI_EXPORT __attribute__((visibility("default")))
#endif

typedef struct { int32_t health; int64_t identifier; double speed; } Actor;
static const KirFfiFieldV2 actor_fields[] = {
    {"здоровье", KIR_FFI_FIELD_I32, offsetof(Actor,health)},
    {"идентификатор", KIR_FFI_FIELD_I64, offsetof(Actor,identifier)},
    {"скорость", KIR_FFI_FIELD_F64, offsetof(Actor,speed)}
};
static const KirFfiRecordSchemaV2 actor_schema = {
    sizeof(Actor),_Alignof(Actor),3,actor_fields
};
/* Structurally valid, intentionally different schema for negative tests. */
typedef struct {int64_t health;} WrongActor;
static const KirFfiFieldV2 wrong_fields[] = {
    {"здоровье",KIR_FFI_FIELD_I64,offsetof(WrongActor,health)}
};
static const KirFfiRecordSchemaV2 wrong_schema = {
    sizeof(WrongActor),_Alignof(WrongActor),1,wrong_fields
};
static const KirFfiParamV2 actor_arg[]={
    {KIR_FFI_V2_RECORD,&actor_schema,NULL}
};
static const KirFfiParamV2 wrong_arg[]={
    {KIR_FFI_V2_RECORD,&wrong_schema,NULL}
};
static const KirFfiParamV2 resource_arg[]={
    {KIR_FFI_V2_RESOURCE,NULL,"test-counter"}
};
static const KirFfiParamV2 f64_arg[]={
    {KIR_FFI_V2_F64,NULL,NULL}
};
static Actor returned_actor;

static int checksum(uint32_t argc, const KirFfiValueV2* args, KirFfiValueV2* result){
    if(argc!=1 || args[0].type!=KIR_FFI_V2_RECORD ||
       args[0].data.record.length!=sizeof(Actor)) return 0;
    Actor actor;
    memcpy(&actor,args[0].data.record.bytes,sizeof(actor));
    if (actor.health!=42 || actor.identifier!=INT64_C(9007199254740993) ||
        actor.speed!=2.5) return 0;
    /* The argument is an isolated snapshot, not a pointer into the arena. */
    Actor overwritten=actor;
    overwritten.health=999;
    memcpy((void*)args[0].data.record.bytes,&overwritten,sizeof(overwritten));
    result->type=KIR_FFI_V2_I64;
    result->data.i64=actor.identifier;
    return 1;
}
static int sum_actor(const KirFfiHostV2* host,uint32_t argc,
                     const KirFfiValueV2* args, KirFfiValueV2* result){
    (void)host;
    return checksum(argc,args,result);
}
static int clone_actor(const KirFfiHostV2* host,uint32_t argc,
                       const KirFfiValueV2* args,KirFfiValueV2* result){
    (void)host;
    if(argc!=1 || args[0].type!=KIR_FFI_V2_RECORD ||
       args[0].data.record.length!=sizeof(Actor)) return 0;
    memcpy(&returned_actor,args[0].data.record.bytes,sizeof(returned_actor));
    result->type=KIR_FFI_V2_RECORD;
    result->data.record=(KirFfiRecordV2){(const unsigned char*)&returned_actor,
                                         sizeof(returned_actor)};
    return 1;
}
static int nonfinite_record(const KirFfiHostV2* host,uint32_t argc,
                            const KirFfiValueV2* args,KirFfiValueV2* result){
    if(!clone_actor(host,argc,args,result)) return 0;
    returned_actor.speed=NAN;
    return 1;
}
static int counter_visit(void* handle,void* user_data){
    if(!handle || !user_data) return 0;
    *(int*)handle+=7;
    *(int*)user_data=*(int*)handle;
    return 1;
}
static int increment(const KirFfiHostV2* host,uint32_t argc,
                     const KirFfiValueV2* args,KirFfiValueV2* result){
    if(argc!=1 || args[0].type!=KIR_FFI_V2_RESOURCE) return 0;
    int updated=0;
    if(!host || !host->visit_resource(host,args[0].data.resource_token,
                                      "test-counter",counter_visit,&updated) ||
        updated!=17) return 0;
    result->type=KIR_FFI_V2_I64;
    result->data.i64=updated;
    return 1;
}
static int handle_pass_through(const KirFfiHostV2* host,uint32_t argc,
                               const KirFfiValueV2* args,KirFfiValueV2* result){
    (void)host;
    if(argc!=1 || args[0].type!=KIR_FFI_V2_RESOURCE) return 0;
    result->type=KIR_FFI_V2_RESOURCE;
    result->data.resource_token=args[0].data.resource_token;
    return 1;
}
static int forged_token(const KirFfiHostV2* host,uint32_t argc,
                        const KirFfiValueV2* args,KirFfiValueV2* result){
    (void)host;(void)argc;(void)args;
    result->type=KIR_FFI_V2_RESOURCE;
    result->data.resource_token=987654;
    return 1;
}
static int classify_f64(const KirFfiHostV2* host,uint32_t argc,
                        const KirFfiValueV2* args,KirFfiValueV2* result){
    (void)host;
    if(argc!=1 || args[0].type!=KIR_FFI_V2_F64) return 0;
    result->type=KIR_FFI_V2_I64;
    result->data.i64=isnan(args[0].data.f64) ? 1 :
                     isinf(args[0].data.f64) ? 2 : 0;
    return 1;
}
static int return_nan(const KirFfiHostV2* host,uint32_t argc,
                      const KirFfiValueV2* args,KirFfiValueV2* result){
    (void)host;(void)argc;(void)args;
    result->type=KIR_FFI_V2_F64;
    result->data.f64=NAN;
    return 1;
}
static const KirFfiFunctionV2 functions[]={
    {"проверить",1,actor_arg,{KIR_FFI_V2_I64,NULL,NULL},sum_actor},
    {"копировать",1,actor_arg,{KIR_FFI_V2_RECORD,&actor_schema,NULL},clone_actor},
    {"плохая_запись",1,actor_arg,{KIR_FFI_V2_RECORD,&actor_schema,NULL},nonfinite_record},
    {"неверная_схема",1,wrong_arg,{KIR_FFI_V2_I64,NULL,NULL},sum_actor},
    {"увеличить",1,resource_arg,{KIR_FFI_V2_I64,NULL,NULL},increment},
    {"вернуть_ресурс",1,resource_arg,{KIR_FFI_V2_RESOURCE,NULL,"test-counter"},handle_pass_through},
    {"подделать_ресурс",1,resource_arg,{KIR_FFI_V2_RESOURCE,NULL,"test-counter"},forged_token},
    {"классифицировать_f64",1,f64_arg,{KIR_FFI_V2_I64,NULL,NULL},classify_f64},
    {"вернуть_nan",0,NULL,{KIR_FFI_V2_F64,NULL,NULL},return_nan}
};
static const KirFfiModuleV2 module={
    KIR_FFI_ABI_V2,sizeof(KirFfiModuleV2),"структуры",9,functions
};
FFI_EXPORT const KirFfiModuleV2* kirton_ffi_module_v2(void){return &module;}
