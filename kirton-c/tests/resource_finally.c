/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/eval.h"
#include "kirton/native.h"
#include "kirton/diag.h"
#include "kirton/bytecode.h"
#include "kirton/parser.h"
#include "../src/native_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)

void native_init(void);
void math_init(void);
void runtime_init_mod(void);

static VM* active_vm;
static int destructor_calls;
static int assertion_calls;
static const char* ffi_plugin_path;

static int set_test_environment(const char* name,const char* value){
#ifdef _WIN32
    return _putenv_s(name,value?value:"");
#else
    return value&&*value ? setenv(name,value,1) : unsetenv(name);
#endif
}

static void destroy_test_handle(void* handle,void* user_data){
    (void)user_data;
    ++destructor_calls;
    free(handle);
}

static Value create_test_resource(int argc,const Value* argv){
    (void)argc; (void)argv;
    int* handle=(int*)malloc(sizeof(*handle));
    if (!handle) return v_nil();
    *handle=10;
    Value value=active_vm
        ? vm_native_resource_adopt(active_vm,handle,"test-counter",destroy_test_handle,NULL)
        : v_native_resource_adopt_current(handle,"test-counter",destroy_test_handle,NULL);
    if (value.kind!=VAL_NATIVE_RESOURCE) free(handle);
    return value;
}

static Value assert_true(int argc,const Value* argv){
    if (argc!=1 || argv[0].kind!=VAL_BOOL || !argv[0].as.boolean){
        kir_diag_error("<test_resource>",1,1,"native resource lifecycle assertion failed");
        return v_bool(false);
    }
    ++assertion_calls;
    return v_bool(true);
}

static Value plugin_path(int argc,const Value* argv){
    (void)argc;(void)argv;
    return v_str_copy(ffi_plugin_path?ffi_plugin_path:"",-1);
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

int main(int argc,char** argv){
    CHECK(argc==2 || argc==4);
    native_init();
    math_init();
    runtime_init_mod();
    static const KirNativeDef functions[]={
        {"создать",create_test_resource},
        {"проверить",assert_true},
        {"путь_плагина",plugin_path}
    };
    kir_native_register_module("test_resource",functions,3);
    kir_native_mark_bytecode_module_safe("test_resource");
    VM* vm=vm_new();
    CHECK(vm!=NULL);
    active_vm=vm;
    kir_diag_reset_errors();
    KirEnv* root=vm_run_root(vm,argv[1]);
    CHECK(root!=NULL);
    CHECK(destructor_calls==1);
    CHECK(assertion_calls==5);
    CHECK(vm_try_free(vm));
    CHECK(destructor_calls==1);
    active_vm=NULL;

    AstProgram* program=read_program(argv[1]);
    CHECK(program!=NULL && program->parse_errors==0);
    kir_diag_reset_errors();
    CHECK(kirton_bytecode_run(program,NULL)==0);
    CHECK(assertion_calls==10);
    CHECK(destructor_calls==2);
    ast_program_destroy(program);

    if (argc==4){
        ffi_plugin_path=argv[2];
        CHECK(set_test_environment("KIRTON_TEST_FFI_V3",ffi_plugin_path)==0);
        AstProgram* ffi_program=read_program(argv[3]);
        CHECK(ffi_program!=NULL && ffi_program->parse_errors==0);
        int assertions_before=assertion_calls;
        int destructors_before=destructor_calls;
        eval_set_entry("main");
        eval_set_ffi_enabled(1);
        kir_diag_reset_errors();
        CHECK(eval_program(ffi_program)==0);
        eval_set_ffi_enabled(0);
        int ast_assertions=assertion_calls-assertions_before;
        CHECK(ast_assertions>0 && destructor_calls==destructors_before+1);
        kir_diag_reset_errors();
        CHECK(kirton_bytecode_run_with_ffi(ffi_program,"main",1)==0);
        CHECK(assertion_calls-assertions_before==2*ast_assertions);
        CHECK(destructor_calls==destructors_before+2);
        ast_program_destroy(ffi_program);
        CHECK(set_test_environment("KIRTON_TEST_FFI_V3",NULL)==0);
    }
    return 0;
}
