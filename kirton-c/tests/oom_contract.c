/* SPDX-License-Identifier: MPL-2.0 */
#include "kirton/bytecode.h"
#include "kirton/common.h"
#include "kirton/diag.h"
#include "kirton/eval.h"
#include "kirton/ffi_internal.h"
#include "kirton/lexer.h"
#include "kirton/native.h"
#include "kirton/number.h"
#include "kirton/parser.h"
#include "kirton/value.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(test) do { if (!(test)) { fprintf(stderr,"OOM contract line %d: %s\n",__LINE__,#test); return __LINE__; } } while (0)

void native_init(void);

static AstProgram* parse_inline(const char* source){
    return parse_program(source,strlen(source),"oom_contract.kir");
}

static int test_lexer(void){
    static const char source[]="имя = 1";
    KirLexer lexer;
    kir_allocator_test_fail_after(0);
    kir_lexer_init(&lexer,source,sizeof(source)-1);
    KirToken token=kir_lexer_next(&lexer);
    CHECK(token.kind==TK_ID);
    /* Lexer made no managed allocation; the pending injection is consumed here. */
    CHECK(kir_oom_malloc(1)==NULL);
    return 0;
}

static int test_arena(void){
    KirValueHeap* heap=v_value_heap_new();
    CHECK(heap);
    KirValueHeap* previous=v_value_heap_set_current(heap);
    kir_allocator_test_fail_after(1); /* descriptor succeeds, backing calloc fails */
    Value arena=v_arena_new(64);
    CHECK(arena.kind==VAL_NIL);
    CHECK(v_value_heap_count(heap)==0 && v_value_heap_arena_bytes(heap)==0);
    arena=v_arena_new(64);
    CHECK(arena.kind==VAL_ARENA);
    CHECK(v_arena_close(arena));
    v_value_heap_set_current(previous);
    v_value_heap_destroy(heap);
    return 0;
}

static int test_ffi_create(void){
    kir_allocator_test_fail_after(0);
    CHECK(kir_ffi_state_create()==NULL);
    KirFfiState* state=kir_ffi_state_create();
    CHECK(state);
    kir_ffi_state_destroy(state);
    return 0;
}

static int test_vm_init(void){
    /* VM, BigInt heap, and Value heap allocate first; FFI state is fallible. */
    kir_allocator_test_fail_after(3);
    CHECK(vm_new()==NULL);
    VM* vm=vm_new();
    CHECK(vm);
    CHECK(vm_try_free(vm));
    return 0;
}

static int test_bytecode_ffi_init(void){
    static const char source[]="функция main() { вернуть 1 }";
    AstProgram* program=parse_inline(source);
    CHECK(program && !program->parse_errors);
    kir_diag_reset_errors();
    kir_allocator_test_fail_after(2); /* Value heap + BigInt heap, then FFI calloc */
    CHECK(kirton_bytecode_run_with_ffi(program,"main",1)!=0);
    kir_diag_reset_errors();
    CHECK(kirton_bytecode_run(program,"main")==0);
    ast_program_destroy(program);
    return 0;
}

static int test_bytecode_sweep(size_t fail_after,const char* path){
    FILE* file=fopen(path,"rb");
    CHECK(file);
    CHECK(fseek(file,0,SEEK_END)==0);
    long length=ftell(file);
    CHECK(length>=0 && fseek(file,0,SEEK_SET)==0);
    char* source=(char*)malloc((size_t)length+1);
    CHECK(source);
    CHECK(fread(source,1,(size_t)length,file)==(size_t)length);
    fclose(file);
    AstProgram* program=parse_program(source,(size_t)length,path);
    free(source);
    CHECK(program && !program->parse_errors);
    kir_diag_reset_errors();
    kir_allocator_test_fail_after(fail_after);
    int rc=kirton_bytecode_run(program,NULL);
    if(rc){
        kir_diag_reset_errors();
        if(kirton_bytecode_run(program,NULL)!=0){
            ast_program_destroy(program);
            return __LINE__;
        }
        puts("BYTECODE_OOM_RECOVERED");
    }else puts("BYTECODE_OOM_NOT_HIT");
    ast_program_destroy(program);
    return 0;
}

static int append_source(char* source,size_t capacity,size_t* used,const char* fmt,size_t value){
    if(*used>=capacity)return 0;
    int wrote=snprintf(source+*used,capacity-*used,fmt,value);
    if(wrote<0||(size_t)wrote>=capacity-*used)return 0;
    *used+=(size_t)wrote;
    return 1;
}

static int test_ast_root_sweep(size_t fail_after){
    char source[8192];size_t used=0;
    int wrote=snprintf(source,sizeof(source),"функция f(");
    if(wrote<0||(size_t)wrote>=sizeof(source))return 3;
    used=(size_t)wrote;
    for(size_t i=0;i<80;i++){
        if(i){if(used+1>=sizeof(source))return 3;source[used++]=',';}
        if(!append_source(source,sizeof(source),&used,"a%zu",i))return 3;
    }
    if(used+32>=sizeof(source))return 3;
    memcpy(source+used,") { вернуть 1 }\nf(",strlen(") { вернуть 1 }\nf("));
    used+=strlen(") { вернуть 1 }\nf(");
    for(size_t i=0;i<80;i++){
        if(i){if(used+1>=sizeof(source))return 3;source[used++]=',';}
        if(!append_source(source,sizeof(source),&used,"%zu",i))return 3;
    }
    if(used+3>=sizeof(source))return 3;
    memcpy(source+used,")\n",3);
    used+=2;
    source[used]='\0';
    kir_diag_reset_errors();
    AstProgram* program=parse_program(source,used,"oom_ast_roots.kir");
    if(!program||program->parse_errors)return 4;
    kir_allocator_test_fail_after(fail_after);
    int rc=eval_program(program);
    if(rc){
        kir_diag_reset_errors();
        if(eval_program(program)!=0){ast_program_destroy(program);return __LINE__;}
        puts("AST_OOM_RECOVERED");
        ast_program_destroy(program);
        return 0;
    }
    ast_program_destroy(program);
    puts("AST_OOM_NOT_HIT");
    return 0;
}

int main(int argc,char** argv){
    if(argc<2)return 2;
    if(!strcmp(argv[1],"lexer"))return test_lexer();
    if(!strcmp(argv[1],"arena"))return test_arena();
    if(!strcmp(argv[1],"ffi_create"))return test_ffi_create();
    if(!strcmp(argv[1],"vm_init"))return test_vm_init();
    if(!strcmp(argv[1],"bytecode_ffi_init"))return test_bytecode_ffi_init();
    if(!strcmp(argv[1],"gc_mark")){
        KirValueHeap* heap=v_value_heap_new();
        if(!heap)return 3;
        (void)v_value_heap_set_current(heap);
        Value root=v_list_new();
        kir_allocator_test_fail_after(0);
        v_value_heap_mark(heap,NULL,root);
        return 4;
    }
    if(!strcmp(argv[1],"bigint")){
        KirBigIntHeap* heap=kir_bigint_heap_new();
        (void)v_bigint_heap_set_current(heap);
        kir_allocator_test_fail_after(0);
        (void)v_int_parse("123456789012345678901234567890",30,10);
        return 4;
    }
    if(!strcmp(argv[1],"vm_start")){
        kir_allocator_test_fail_after(0);
        (void)vm_new();
        return 4;
    }
    if(!strcmp(argv[1],"ast_runtime")){
        static const char source[]="пусть значение = [1, 2, 3]";
        AstProgram* program=parse_inline(source);
        if(!program || program->parse_errors)return 3;
        kir_allocator_test_fail_after(0);
        (void)eval_program(program);
        ast_program_destroy(program);
        return 4;
    }
    if(!strcmp(argv[1],"bytecode_sweep")){
        if(argc!=4)return 2;
        char* end=NULL;
        unsigned long long count=strtoull(argv[2],&end,10);
        if(!end||*end)return 2;
        native_init();
        return test_bytecode_sweep((size_t)count,argv[3]);
    }
    if(!strcmp(argv[1],"ast_root_sweep")){
        if(argc!=3)return 2;
        char* end=NULL;
        unsigned long long count=strtoull(argv[2],&end,10);
        if(!end||*end)return 2;
        return test_ast_root_sweep((size_t)count);
    }
    return 2;
}
