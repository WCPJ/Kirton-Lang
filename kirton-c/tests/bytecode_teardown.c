/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/bytecode.h"
#include "kirton/diag.h"
#include "kirton/native.h"
#include "kirton/parser.h"
#include "kirton/value.h"
#include "../src/native_internal.h"
#include <stdio.h>
#include <stdlib.h>
#if defined(_DEBUG)
#include <crtdbg.h>
#endif

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"check failed at %d: %s\n",__LINE__,#x); return __LINE__; } } while (0)

static size_t destructor_calls;
void native_init(void);

static void destroy_handle(void* handle,void* user_data){
    (void)user_data;
    ++destructor_calls;
    free(handle);
}

static Value create_resource(int argc,const Value* argv){
    (void)argc;(void)argv;
    int* handle=(int*)malloc(sizeof(*handle));
    if (!handle) return v_nil();
    *handle=1;
    Value resource=v_native_resource_adopt_current(handle,"bytecode-teardown",destroy_handle,NULL);
    if (resource.kind!=VAL_NATIVE_RESOURCE) free(handle);
    return resource;
}

static AstProgram* read_program(const char* path){
    FILE* file=fopen(path,"rb");
    if (!file) return NULL;
    if (fseek(file,0,SEEK_END)!=0){fclose(file);return NULL;}
    long size=ftell(file);
    if (size<0 || fseek(file,0,SEEK_SET)!=0){fclose(file);return NULL;}
    char* source=(char*)malloc((size_t)size+1);
    if (!source){fclose(file);return NULL;}
    if (fread(source,1,(size_t)size,file)!=(size_t)size){free(source);fclose(file);return NULL;}
    fclose(file);
    AstProgram* program=parse_program(source,(size_t)size,path);
    free(source);
    return program;
}

#if defined(_DEBUG)
static int crt_live_delta(const _CrtMemState* before,const _CrtMemState* after){
    _CrtMemState difference={0};
    (void)_CrtMemDifference(&difference,before,after);
    long count=0;
    for(int kind=0;kind<_MAX_BLOCKS;++kind)count+=difference.lCounts[kind];
    return (int)count;
}
#endif

int main(int argc,char** argv){
    if (argc!=3) return 2;
    native_init();
    static const KirNativeDef functions[]={{"создать",create_resource}};
    kir_native_register_module("test_resource",functions,1);
    kir_native_mark_bytecode_module_safe("test_resource");
    AstProgram* success=read_program(argv[1]);
    AstProgram* failure=read_program(argv[2]);
    CHECK(success && !success->parse_errors && failure && !failure->parse_errors);
#if defined(_DEBUG)
    /* First stderr use creates a process-wide CRT buffer, outside VM ownership. */
    kir_diag_error("<bytecode_teardown>",1,1,"diagnostic stream warmup");
    kir_diag_reset_errors();
#endif
    for (int i=0;i<32;++i){
#if defined(_DEBUG)
        _CrtMemState before={0},after={0};
        _CrtMemCheckpoint(&before);
#endif
        CHECK(kirton_bytecode_run(success,"main")==0);
#if defined(_DEBUG)
        _CrtMemCheckpoint(&after);
        CHECK(crt_live_delta(&before,&after)==0);
        _CrtMemCheckpoint(&before);
#endif
        CHECK(kirton_bytecode_run(failure,NULL)!=0);
        kir_diag_reset_errors();
#if defined(_DEBUG)
        _CrtMemCheckpoint(&after);
        CHECK(crt_live_delta(&before,&after)==0);
#endif
        CHECK(destructor_calls==(size_t)(2*(i+1)));
    }
    ast_program_destroy(success);
    ast_program_destroy(failure);
    return 0;
}
