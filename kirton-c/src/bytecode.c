/* SPDX-License-Identifier: MPL-2.0 */
#include "kirton/bytecode.h"
#include "kirton/ffi_internal.h"
#include "eval_internal.h"
#include "kirton/value.h"
#include "kirton/diag.h"
#include "kirton/utf8.h"
#include "kirton/common.h"
#include "kirton/native.h"
#include "kirton/source_cache.h"
#include "native_internal.h"
#include "kirton/parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include <math.h>
#ifdef _WIN32
#include <windows.h>
#endif

/* Prototype v1: fixed-width logical instruction records, name-addressed slots,
   one operand stack per recursive call frame. Offsets are instruction indices. */
typedef enum { BC_CONST, BC_MATH_NAN, BC_MATH_INF, BC_LOAD, BC_LET, BC_SET, BC_POP, BC_PRINT, BC_NEG, BC_NOT,
 BC_BITNOT, BC_BOOLIFY, BC_ADD, BC_SUB, BC_MUL, BC_DIV, BC_FLOOR_DIV, BC_MOD,
 BC_BIT_AND, BC_BIT_OR, BC_BIT_XOR, BC_SHL, BC_SHR, BC_BIND_FUNC, BC_PREP_CALL, BC_ARG_CHECK, BC_ARG_STORE, BC_CALL_VALUE,
 BC_GET_ATTR, BC_ATTR_NAME, BC_CHECK_SET_ATTR, BC_SET_ATTR, BC_IMPORT, BC_INCLUDE, BC_TRY_EXEC, BC_FLOW_BREAK, BC_FLOW_CONTINUE, BC_EQ, BC_NE, BC_LT, BC_LE,
 BC_GT, BC_GE, BC_MAKE_LIST, BC_MAKE_DICT, BC_INDEX, BC_SETINDEX, BC_ITER_NEXT,
 BC_ITER_END, BC_JUMP, BC_JFALSE, BC_JTRUE, BC_RET, BC_END } Op;
typedef struct { Op op; int a, b; AstPos pos; } Ins;
typedef struct { const char** keys; size_t* lens; size_t n; } DictKeys;
typedef struct BModule BModule;
typedef struct BModuleBinding BModuleBinding;
typedef struct BEnv BEnv;
typedef struct { const char* name; size_t name_len; const char** arg_names; size_t* arg_name_lens; size_t argc; int fast_args, function_hint, module_attr, value_callee, native_slot; const char* module_name; size_t module_name_len; const char* member; size_t member_len; } CallDesc;
typedef struct { const char* object_name; size_t object_name_len; const char* member; size_t member_len; } AttrDesc;
typedef struct { const char* name; size_t name_len; const char* alias; size_t alias_len; const char* from_dir; size_t from_dir_len; int include; } ImportDesc;
typedef struct Chunk Chunk;
typedef struct { Chunk* body; Chunk* catch_body; Chunk* finally_body; const char* catch_name; size_t catch_name_len; int has_catch,has_finally; size_t break_target,continue_target; } TryDesc;
struct Chunk { Ins* code; size_t n, cap; Value* constants; size_t cn, cc; DictKeys* dicts; size_t dn, dc; CallDesc* calls; size_t call_n, call_c; AttrDesc* attrs; size_t attr_n, attr_c; ImportDesc* imports; size_t import_n,import_c; TryDesc* tries; size_t try_n,try_c; };
typedef struct { char* name; size_t len; AstFunc* ast; Chunk chunk; int module_scope; } BFunc;
typedef struct { AstUserType* type; BEnv* env; } BType;
struct BModuleBinding { char* alias; size_t alias_len; BModule* module; char* native_module; BModuleBinding* next; };
struct BEnv { VDict storage; Value dict; BModuleBinding* modules; BEnv* parent; BEnv* owned_next; size_t closure_refs, child_refs, active_refs, owner_refs; int global; };
struct BModule { char* key; char* module_name; AstProgram* program; BEnv* env; Value exports; int initialized,loading; };
typedef struct BFrame { BEnv* env; BFunc* function; Chunk* chunk; AstPos call_pos; int internal; struct BFrame* caller; } BFrame;
typedef struct BLoop { Chunk* chunk; size_t* breaks; size_t break_n, break_c; size_t* continues; size_t continue_n, continue_c; size_t continue_ip; } BLoop;
typedef struct { BFunc** funcs; size_t fn, fc; BType* types; size_t type_n,type_c; BModule** modules; size_t module_n,module_c; AstProgram** owned_programs; size_t owned_program_n,owned_program_c; Value* stack; size_t sn, sc; BEnv global_env; BEnv* owned_envs; BFrame* frames; Chunk* init; KirValueHeap* heap; KirBigIntHeap* bigs; KirFfiState* ffi; int ffi_enabled; size_t next_gc, collections, reclaimed, peak, live_end; double compile_ms, execute_ms; KirDiagnostic error_diag; char error_code[32]; Value error_stack; Value error_roots[256]; size_t error_root_n; int has_error; unsigned try_depth; AstPos pos; int failed; } BVM;
typedef enum { B_FLOW_NONE, B_FLOW_RETURN, B_FLOW_BREAK, B_FLOW_CONTINUE } BFlow;
static int run_chunk(BVM *vm,Chunk *chunk,BEnv *scope,Value *out,BFlow *flow,unsigned depth,BFunc *function,AstPos call_pos,int internal);
static double now_ms(void){
#ifdef _WIN32
    LARGE_INTEGER counter,frequency;QueryPerformanceCounter(&counter);QueryPerformanceFrequency(&frequency);
    return (double)counter.QuadPart*1000.0/(double)frequency.QuadPart;
#else
    struct timespec ts;timespec_get(&ts,TIME_UTC);return (double)ts.tv_sec*1000.0+(double)ts.tv_nsec/1000000.0;
#endif
}

static void* grow(void* p,size_t* cap,size_t need,size_t unit){ if(*cap>=need)return p; size_t n=*cap?*cap:16; while(n<need){if(n>SIZE_MAX/2)return NULL;n*=2;}if(unit&&n>SIZE_MAX/unit)return NULL;void*q=kir_oom_realloc(p,n*unit);if(!q)return NULL;*cap=n;return q; }
static int nameeq(const char*,size_t,const char*,size_t);
static int emit2(Chunk*c,Op op,int a,int b,AstPos p){ Ins*code=(Ins*)grow(c->code,&c->cap,c->n+1,sizeof(Ins));if(!code)return 0;c->code=code;c->code[c->n++]=(Ins){op,a,b,p};return 1; }
static int emit(Chunk*c,Op op,int a,AstPos p){return emit2(c,op,a,0,p);}
static int constant(Chunk*c,Value v){Value*constants=(Value*)grow(c->constants,&c->cc,c->cn+1,sizeof(Value));if(!constants)return -1;c->constants=constants;c->constants[c->cn]=v;return (int)c->cn++;}
static int dict_keys(Chunk*c,AstDict*d){DictKeys*dicts=(DictKeys*)grow(c->dicts,&c->dc,c->dn+1,sizeof(DictKeys));if(!dicts)return -1;c->dicts=dicts;c->dicts[c->dn]=(DictKeys){d->keys_s,d->keys_n,d->len};return (int)c->dn++;}
static int try_desc_add(Chunk*c,AstTry*ast){
    TryDesc*tries=(TryDesc*)grow(c->tries,&c->try_c,c->try_n+1,sizeof(TryDesc));if(!tries)return -1;c->tries=tries;
    TryDesc*t=&c->tries[c->try_n];memset(t,0,sizeof(*t));
    t->body=(Chunk*)kir_oom_calloc(1,sizeof(Chunk));t->catch_body=ast->has_catch?(Chunk*)kir_oom_calloc(1,sizeof(Chunk)):NULL;
    t->finally_body=ast->has_finally?(Chunk*)kir_oom_calloc(1,sizeof(Chunk)):NULL;
    if(!t->body||(ast->has_catch&&!t->catch_body)||(ast->has_finally&&!t->finally_body)){
        free(t->body);free(t->catch_body);free(t->finally_body);memset(t,0,sizeof(*t));return -1;
    }
    t->catch_name=ast->catch_name;t->catch_name_len=ast->catch_name_len;t->has_catch=ast->has_catch;t->has_finally=ast->has_finally;
    t->break_target=t->continue_target=SIZE_MAX;
    return (int)c->try_n++;
}
static void chunk_free(Chunk*c){
    if(!c)return;
    for(size_t i=0;i<c->try_n;i++){
        TryDesc*t=&c->tries[i];chunk_free(t->body);chunk_free(t->catch_body);chunk_free(t->finally_body);
        free(t->body);free(t->catch_body);free(t->finally_body);
    }
    free(c->code);free(c->constants);free(c->dicts);free(c->calls);free(c->attrs);free(c->imports);free(c->tries);
    memset(c,0,sizeof(*c));
}
static int call_desc(BVM*vm,Chunk*c,AstCall*call,AstExpr*callee){
    CallDesc*calls=(CallDesc*)grow(c->calls,&c->call_c,c->call_n+1,sizeof(CallDesc));if(!calls)return -1;c->calls=calls;
    CallDesc*d=&c->calls[c->call_n];
    memset(d,0,sizeof(*d));d->function_hint=-1;d->native_slot=-1;
    if(callee->kind==AST_VAR){d->name=callee->as.var.name_start;d->name_len=callee->as.var.name_len;}
    else if(callee->kind==AST_ATTR&&callee->as.attr.obj->kind==AST_VAR){d->module_attr=1;d->module_name=callee->as.attr.obj->as.var.name_start;d->module_name_len=callee->as.attr.obj->as.var.name_len;d->member=callee->as.attr.name;d->member_len=callee->as.attr.n;}
    else d->value_callee=1;
    d->arg_names=call->arg_names;d->arg_name_lens=call->arg_name_lens;d->argc=call->argc;d->fast_args=1;d->function_hint=-1;
    for(size_t i=0;i<vm->fn&&!d->module_attr&&!d->value_callee;i++)if(vm->funcs[i]->module_scope&&nameeq(vm->funcs[i]->name,vm->funcs[i]->len,d->name,d->name_len)){d->function_hint=(int)i;break;}
    for(size_t i=0;i<call->argc;i++)if(call->arg_names&&call->arg_names[i])d->fast_args=0;
    for(size_t i=0;i<vm->type_n&&!d->module_attr;i++)if(vm->types[i].type->kind==AST_TYPE_STRUCT&&vm->types[i].type->name_len==d->name_len&&!memcmp(vm->types[i].type->name,d->name,d->name_len))d->fast_args=0;
    return (int)c->call_n++;
}
static int attr_desc(Chunk*c,AstExpr*object,AstAttr*attr){
    AttrDesc*attrs=(AttrDesc*)grow(c->attrs,&c->attr_c,c->attr_n+1,sizeof(AttrDesc));if(!attrs)return -1;c->attrs=attrs;
    AttrDesc*d=&c->attrs[c->attr_n];
    d->object_name=object->as.var.name_start;d->object_name_len=object->as.var.name_len;
    d->member=attr->name;d->member_len=attr->n;
    return (int)c->attr_n++;
}
static int import_desc(Chunk*c,AstStmt*s,int include){
    ImportDesc*imports=(ImportDesc*)grow(c->imports,&c->import_c,c->import_n+1,sizeof(ImportDesc));if(!imports)return -1;c->imports=imports;
    ImportDesc*d=&c->imports[c->import_n];memset(d,0,sizeof(*d));d->include=include;
    if(include){d->name=s->as.inc.path;d->name_len=s->as.inc.pn;}
    else{d->name=s->as.imp.name;d->name_len=s->as.imp.nn;d->alias=s->as.imp.alias;d->alias_len=s->as.imp.an;
#if KIRTON_AST_IMPORT_HAS_FROM
        d->from_dir=s->as.imp.from_dir;d->from_dir_len=s->as.imp.from_dir_len;
#endif
    }
    return (int)c->import_n++;
}
static int function_index(BVM*vm,AstFunc*ast){for(size_t i=0;i<vm->fn;i++)if(vm->funcs[i]->ast==ast)return (int)i;return -1;}
static int add_function(BVM*vm,AstFunc*ast){
    int existing=function_index(vm,ast);if(existing>=0)return existing;
    BFunc**funcs=grow(vm->funcs,&vm->fc,vm->fn+1,sizeof(BFunc*));if(!funcs)return -1;
    vm->funcs=funcs;BFunc*entry=(BFunc*)kir_oom_calloc(1,sizeof(*entry));if(!entry)return -1;
    entry->name=(char*)kir_oom_malloc(ast->name_len+1);if(!entry->name){free(entry);return -1;}
    memcpy(entry->name,ast->name_start,ast->name_len);entry->name[ast->name_len]=0;
    entry->len=ast->name_len;entry->ast=ast;vm->funcs[vm->fn]=entry;return (int)vm->fn++;
}
static AstTypeField* type_field(AstUserType*type,const char*name,size_t len){
    if(!type||type->kind!=AST_TYPE_STRUCT)return NULL;
    for(size_t i=0;i<type->field_count;i++)if(type->fields[i].name_len==len&&!memcmp(type->fields[i].name,name,len))return &type->fields[i];
    return NULL;
}
static int type_index(BVM*vm,BEnv*scope,const char*name,size_t len,AstUserTypeKind kind){
    for(BEnv*e=scope;e;e=e->parent)for(size_t i=vm->type_n;i>0;i--){BType*t=&vm->types[i-1];if(t->env==e&&t->type->kind==kind&&t->type->name_len==len&&!memcmp(t->type->name,name,len))return (int)(i-1);}
    return -1;
}
static int add_types(BVM*vm,AstUserType**types,size_t count,BEnv*env){
    for(size_t i=0;i<count;i++){
        int present=0;for(size_t j=0;j<vm->type_n;j++)if(vm->types[j].type==types[i]&&vm->types[j].env==env){present=1;break;}
        if(present)continue;
        BType*types_array=(BType*)grow(vm->types,&vm->type_c,vm->type_n+1,sizeof(BType));if(!types_array)return 0;vm->types=types_array;
        vm->types[vm->type_n++]=(BType){types[i],env};
    }
    return 1;
}
static int nameeq(const char*a,size_t an,const char*b,size_t bn){return an==bn&&!memcmp(a,b,an);}
static void unsupported(BVM*v,AstPos p,const char*what){ if(v->failed)return; kir_diag_error(p.file?p.file:"<скрипт>",p.line,p.col,"Байткод: неподдерживаемая конструкция: %s",what);v->failed=1; }
static int compile_expr(BVM*,Chunk*,AstExpr*);
static int compile_stmts(BVM*v,Chunk*c,AstStmt**ss,size_t n,int function,BLoop*loop);
static int compile_lazy(BVM*v,Chunk*c,AstExpr*e,int is_and){
    if(!compile_expr(v,c,e->as.bin.a))return 0;
    size_t short_jump;
    if(is_and){
        short_jump=c->n;
        if(!emit(c,BC_JFALSE,0,e->pos)||!compile_expr(v,c,e->as.bin.b)||
           !emit(c,BC_BOOLIFY,0,e->pos))return 0;
    }else{
        if(!emit(c,BC_NOT,0,e->pos))return 0;
        short_jump=c->n;
        if(!emit(c,BC_JFALSE,0,e->pos)||
           !compile_expr(v,c,e->as.bin.b)||!emit(c,BC_BOOLIFY,0,e->pos))return 0;
    }
    size_t done=c->n;if(!emit(c,BC_JUMP,0,e->pos))return 0;
    c->code[short_jump].a=(int)c->n;
    int k=constant(c,v_bool(!is_and));
    if(k<0||!emit(c,BC_CONST,k,e->pos))return 0;
    c->code[done].a=(int)c->n;
    return 1;
}

static int compile_expr(BVM*v,Chunk*c,AstExpr*e){
    if(!e)return 0;
    int k;
    switch(e->kind){
    case AST_NUM:case AST_BOOL:case AST_NIL:
        k=constant(c,e->kind==AST_NUM?e->as.num.value:e->kind==AST_BOOL?v_bool(e->as.boolean.value):v_nil());
        return k>=0&&emit(c,BC_CONST,k,e->pos);
    case AST_STR:{
        if(e->as.str.len>INT_MAX){unsupported(v,e->pos,"строковый литерал слишком велик");return 0;}
        k=constant(c,v_str_copy(e->as.str.start,(int)e->as.str.len));
        return k>=0&&emit(c,BC_CONST,k,e->pos);
    }
    case AST_VAR:
        k=constant(c,v_str_copy(e->as.var.name_start,(int)e->as.var.name_len));
        return k>=0&&emit(c,BC_LOAD,k,e->pos);
    case AST_UNARY:{
        const char*op=e->as.unary.op;
        Op byteop=!strcmp(op,"-")?BC_NEG:!strcmp(op,"!")?BC_NOT:!strcmp(op,"~")?BC_BITNOT:(Op)-1;
        if((int)byteop<0){unsupported(v,e->pos,"унарный оператор");return 0;}
        return compile_expr(v,c,e->as.unary.e)&&emit(c,byteop,0,e->pos);
    }
    case AST_LIST:
        for(size_t i=0;i<e->as.list.len;i++)if(!compile_expr(v,c,e->as.list.items[i]))return 0;
        return emit(c,BC_MAKE_LIST,(int)e->as.list.len,e->pos);
    case AST_DICT:
        for(size_t i=0;i<e->as.dict.len;i++)if(!compile_expr(v,c,e->as.dict.vals[i]))return 0;
        k=dict_keys(c,&e->as.dict);return k>=0&&emit(c,BC_MAKE_DICT,k,e->pos);
    case AST_INDEX:
        return compile_expr(v,c,e->as.index.base)&&compile_expr(v,c,e->as.index.index)&&emit(c,BC_INDEX,0,e->pos);
    case AST_ATTR:
        if(e->as.attr.obj->kind==AST_VAR){
            k=attr_desc(c,e->as.attr.obj,&e->as.attr);return k>=0&&emit(c,BC_ATTR_NAME,k,e->pos);
        }
        k=constant(c,v_str_copy(e->as.attr.name,(int)e->as.attr.n));
        return k>=0&&compile_expr(v,c,e->as.attr.obj)&&emit(c,BC_GET_ATTR,k,e->pos);
    case AST_BIN:{
        const char*op=e->as.bin.op;
        if(!strcmp(op,"&&"))return compile_lazy(v,c,e,1);
        if(!strcmp(op,"||"))return compile_lazy(v,c,e,0);
        Op byteop=!strcmp(op,"+")?BC_ADD:!strcmp(op,"-")?BC_SUB:
            !strcmp(op,"*")?BC_MUL:!strcmp(op,"/")?BC_DIV:
            !strcmp(op,"нацело")?BC_FLOOR_DIV:!strcmp(op,"%")?BC_MOD:
            !strcmp(op,"&")?BC_BIT_AND:!strcmp(op,"|")?BC_BIT_OR:
            !strcmp(op,"^")?BC_BIT_XOR:!strcmp(op,"<<")?BC_SHL:
            !strcmp(op,">>")?BC_SHR:!strcmp(op,"==")?BC_EQ:
            !strcmp(op,"!=")?BC_NE:!strcmp(op,"<")?BC_LT:
            !strcmp(op,"<=")?BC_LE:!strcmp(op,">")?BC_GT:
            !strcmp(op,">=")?BC_GE:(Op)-1;
        if((int)byteop<0){unsupported(v,e->pos,"бинарный оператор");return 0;}
        return compile_expr(v,c,e->as.bin.a)&&compile_expr(v,c,e->as.bin.b)&&emit(c,byteop,0,e->pos);
    }
    case AST_CALL:{
        AstExpr*cal=e->as.call.callee;
        int value_callee=cal->kind!=AST_VAR&&!(cal->kind==AST_ATTR&&cal->as.attr.obj->kind==AST_VAR);
        if(cal->kind==AST_VAR){
            const char*name=cal->as.var.name_start;size_t len=cal->as.var.name_len;
            int declared=0;for(size_t i=0;i<v->fn;i++)if(nameeq(v->funcs[i]->name,v->funcs[i]->len,name,len)){declared=1;break;}
            if(!declared&&e->as.call.argc==0&&nameeq(name,len,"нан",strlen("нан"))){k=constant(c,v_str_copy(name,(int)len));return k>=0&&emit(c,BC_MATH_NAN,k,e->pos);}
            if(!declared&&e->as.call.argc==0&&nameeq(name,len,"беск",strlen("беск"))){k=constant(c,v_str_copy(name,(int)len));return k>=0&&emit(c,BC_MATH_INF,k,e->pos);}
        }
        if(value_callee&&!compile_expr(v,c,cal))return 0;
        k=call_desc(v,c,&e->as.call,cal);if(k<0||!emit(c,BC_PREP_CALL,k,e->pos))return 0;
        for(size_t i=0;i<e->as.call.argc;i++){
            if(!c->calls[k].fast_args&&!emit2(c,BC_ARG_CHECK,k,(int)i,e->pos))return 0;
            if(!compile_expr(v,c,e->as.call.args[i]))return 0;
            if(!c->calls[k].fast_args&&!emit2(c,BC_ARG_STORE,k,(int)i,e->pos))return 0;
        }
        return emit(c,BC_CALL_VALUE,k,e->pos);
    }
    default:unsupported(v,e->pos,"атрибуты и прочие выражения");return 0;
    }
}
static void fail(BVM*,AstPos,const char*);
static void error_root_push(BVM*vm,Value value){if(vm->error_root_n<sizeof(vm->error_roots)/sizeof(vm->error_roots[0]))vm->error_roots[vm->error_root_n++]=value;}
static void error_root_pop(BVM*vm){if(vm->error_root_n)vm->error_roots[--vm->error_root_n]=v_nil();}
static Value capture_error_stack(BVM*vm){
    Value stack=v_list_new();vm->error_stack=stack;
    for(BFrame*fr=vm->frames;fr;fr=fr->caller){
        if(fr->internal||!fr->function)continue;
        Value frame=v_dict_new();
        v_dict_set(&frame,"функция",strlen("функция"),v_str_copy(fr->function->name,-1));
        v_dict_set(&frame,"файл",strlen("файл"),v_str_copy(fr->call_pos.file?fr->call_pos.file:"<скрипт>",-1));
        v_dict_set(&frame,"строка",strlen("строка"),v_int(fr->call_pos.line>0?fr->call_pos.line:1));
        v_dict_set(&frame,"столбец",strlen("столбец"),v_int(fr->call_pos.col>0?fr->call_pos.col:1));
        v_list_push(&stack,frame);
    }
    vm->error_stack=stack;return stack;
}
static void env_init(BEnv*env,BEnv*parent,int global){memset(env,0,sizeof(*env));env->dict.kind=VAL_DICT;env->dict.as.dict=&env->storage;env->parent=parent;env->global=global!=0;}
static void env_release_storage(BEnv*env){for(size_t i=0;i<env->storage.len;i++)kir_free(env->storage.keys[i]);kir_free(env->storage.keys);kir_free(env->storage.kn);kir_free(env->storage.vals);memset(&env->storage,0,sizeof(env->storage));BModuleBinding*m=env->modules;while(m){BModuleBinding*n=m->next;free(m->alias);free(m->native_module);free(m);m=n;}env->modules=NULL;}
static BModule* module_by_key(BVM*vm,const char*key){for(size_t i=0;i<vm->module_n;i++)if(!strcmp(vm->modules[i]->key,key))return vm->modules[i];return NULL;}
static BModule* module_new(BVM*vm,const char*key){
    BModule**modules=grow(vm->modules,&vm->module_c,vm->module_n+1,sizeof(BModule*));if(!modules)return NULL;vm->modules=modules;
    BModule*m=(BModule*)kir_oom_calloc(1,sizeof(*m));if(!m)return NULL;m->key=(char*)kir_oom_malloc(strlen(key)+1);if(!m->key){free(m);return NULL;}strcpy(m->key,key);vm->modules[vm->module_n++]=m;return m;
}
static void env_try_free(BVM*vm,BEnv*env){
    if(!env||env->global||env->closure_refs||env->child_refs||env->active_refs||env->owner_refs)return;
    BEnv**link=&vm->owned_envs;while(*link&&*link!=env)link=&(*link)->owned_next;
    if(*link)*link=env->owned_next;
    BEnv*parent=env->parent;env_release_storage(env);free(env);
    if(parent&&parent->child_refs){--parent->child_refs;env_try_free(vm,parent);}
}
static BEnv* env_new(BVM*vm,BEnv*parent){
    BEnv*env=(BEnv*)kir_oom_calloc(1,sizeof(*env));if(!env)return NULL;
    env_init(env,parent,0);env->owner_refs=1;
    env->owned_next=vm->owned_envs;vm->owned_envs=env;
    if(parent)++parent->child_refs;
    return env;
}
static void env_owner_release(BVM*vm,BEnv*env){if(env&&env->owner_refs)--env->owner_refs;env_try_free(vm,env);}
static void env_active_enter(BEnv*env){if(env)++env->active_refs;}
static void env_active_leave(BVM*vm,BEnv*env){if(env&&env->active_refs)--env->active_refs;env_try_free(vm,env);}
static void gc_mark_env(BVM*vm,BEnv*env){for(BEnv*e=env;e;e=e->parent)v_value_heap_mark(vm->heap,vm->bigs,e->dict);}
static void gc_trace_bytecode_closure(void*ctx,void*closure){gc_mark_env((BVM*)ctx,(BEnv*)closure);}
static void gc_release_bytecode_closure(void*ctx,void*closure){BVM*vm=(BVM*)ctx;BEnv*env=(BEnv*)closure;if(env&&env->closure_refs)--env->closure_refs;env_try_free(vm,env);}
static Value function_value_new(BVM*vm,AstFunc*ast,BEnv*closure){
    KirValueHeap*old=v_value_heap_set_current(vm->heap);
    Value value=v_function_new(ast,closure,gc_trace_bytecode_closure,gc_release_bytecode_closure,vm);
    v_value_heap_set_current(old);
    if(value.kind==VAL_FUNCTION&&closure)++closure->closure_refs;
    return value;
}
static void env_free_all(BVM*vm){while(vm->owned_envs){BEnv*e=vm->owned_envs;vm->owned_envs=e->owned_next;env_release_storage(e);free(e);}}
static int value_matches_type(BVM*vm,BEnv*scope,const char*name,size_t len,Value value){
    if(nameeq(name,len,"пусто",strlen("пусто")))return value.kind==VAL_NIL;
    if(nameeq(name,len,"булево",strlen("булево")))return value.kind==VAL_BOOL;
    if(nameeq(name,len,"целое",strlen("целое")))return value.kind==VAL_NUM&&v_num_is_integer(value);
    if(nameeq(name,len,"число",strlen("число")))return value.kind==VAL_NUM&&!v_num_is_integer(value);
    if(nameeq(name,len,"строка",strlen("строка")))return value.kind==VAL_STR;
    if(nameeq(name,len,"список",strlen("список")))return value.kind==VAL_LIST;
    if(nameeq(name,len,"словарь",strlen("словарь")))return value.kind==VAL_DICT;
    int id=type_index(vm,scope,name,len,AST_TYPE_STRUCT);if(id<0)id=type_index(vm,scope,name,len,AST_TYPE_ENUM);
    if(id<0)return 0;AstUserType*t=vm->types[id].type;
    return t->kind==AST_TYPE_STRUCT?v_user_struct_type(value)==t:v_enum_value_type(value)==t;
}
static int callable_function_index(BVM*vm,Value value){
    if(!v_function_is_owned(vm->heap,value))return -1;
    AstFunc*ast=v_function_ast(value);return function_index(vm,ast);
}
static int call_target_tag(BVM*vm,Value value){
    size_t max_tag=vm->type_n+1+vm->fn+4096;
    if(value.kind!=VAL_NIL||value.integer_small>INT64_MIN+(int64_t)max_tag)return -1;
    return (int)(value.integer_small-INT64_MIN);
}
static int call_target_type(BVM*vm,Value value){int tag=call_target_tag(vm,value);return tag>=0&&(size_t)tag<vm->type_n?tag:-1;}
static Value call_target_marker(size_t tag){Value marker=v_nil();marker.integer_small=INT64_MIN+(int64_t)tag;return marker;}
static int user_type_index(BVM*vm,BEnv*scope,const char*name,size_t len){return type_index(vm,scope,name,len,AST_TYPE_STRUCT);}
static int enum_type_index(BVM*vm,BEnv*scope,const char*name,size_t len){return type_index(vm,scope,name,len,AST_TYPE_ENUM);}
static int loop_add(BVM*vm,BLoop*loop,size_t patch,int is_break,AstPos pos){
    size_t**items=is_break?&loop->breaks:&loop->continues;
    size_t*len=is_break?&loop->break_n:&loop->continue_n;
    size_t*cap=is_break?&loop->break_c:&loop->continue_c;
    size_t*grown=(size_t*)grow(*items,cap,*len+1,sizeof(size_t));
    if(!grown){fail(vm,pos,"нехватка памяти списка переходов цикла");return 0;}
    *items=grown;(*items)[(*len)++]=patch;return 1;
}
static void loop_apply(Chunk*c,BLoop*loop,size_t break_target){
    for(size_t i=0;i<loop->break_n;i++){Ins*in=&c->code[loop->breaks[i]];if(in->op==BC_TRY_EXEC)c->tries[in->a].break_target=break_target;else in->a=(int)break_target;}
    for(size_t i=0;i<loop->continue_n;i++){Ins*in=&c->code[loop->continues[i]];if(in->op==BC_TRY_EXEC)c->tries[in->a].continue_target=loop->continue_ip;else in->a=(int)loop->continue_ip;}
    free(loop->breaks);free(loop->continues);
}
static int compile_stmts(BVM*vm,Chunk*c,AstStmt**ss,size_t n,int function,BLoop*loop){
    for(size_t i=0;i<n&&!vm->failed;i++){
        AstStmt*s=ss[i];int operand;
        switch(s->kind){
        case STMT_LET:case STMT_SET:
            if(!compile_expr(vm,c,s->kind==STMT_LET?s->as.let_.value:s->as.set.value))return 0;
            operand=constant(c,v_str_copy(s->kind==STMT_LET?s->as.let_.ns:s->as.set.ns,(int)(s->kind==STMT_LET?s->as.let_.nn:s->as.set.nn)));
            if(operand<0||!emit(c,s->kind==STMT_LET?BC_LET:BC_SET,operand,s->pos))return 0;
            break;
        case STMT_EXPR:
            if(!compile_expr(vm,c,s->as.expr)||!emit(c,BC_POP,0,s->pos))return 0;
            break;
        case STMT_RETURN:
            /* AST permits return in module init: it ends the current init block;
               entry dispatch still happens afterward, matching eval_program(). */
            if(s->as.ret.value){if(!compile_expr(vm,c,s->as.ret.value))return 0;}
            else{operand=constant(c,v_nil());if(operand<0||!emit(c,BC_CONST,operand,s->pos))return 0;}
            if(!emit(c,BC_RET,0,s->pos))return 0;
            break;
        case STMT_BREAK:case STMT_CONTINUE:{
            if(!loop){unsupported(vm,s->pos,s->kind==STMT_BREAK?"прервать вне цикла":"продолжить вне цикла");return 0;}
            if(loop->chunk!=c){if(!emit(c,s->kind==STMT_BREAK?BC_FLOW_BREAK:BC_FLOW_CONTINUE,0,s->pos))return 0;break;}
            size_t at=c->n;if(!emit(c,BC_JUMP,0,s->pos)||!loop_add(vm,loop,at,s->kind==STMT_BREAK,s->pos))return 0;
            break;
        }
        case STMT_IF:{
            if(!compile_expr(vm,c,s->as.if_.cond))return 0;
            size_t jf=c->n;if(!emit(c,BC_JFALSE,0,s->pos)||!compile_stmts(vm,c,s->as.if_.then_b,s->as.if_.then_len,function,loop))return 0;
            size_t je=c->n;if(!emit(c,BC_JUMP,0,s->pos))return 0;c->code[jf].a=(int)c->n;
            if(s->as.if_.has_else&&!compile_stmts(vm,c,s->as.if_.else_b,s->as.if_.else_len,function,loop))return 0;
            c->code[je].a=(int)c->n;break;
        }
        case STMT_WHILE:{
            size_t top=c->n;if(!compile_expr(vm,c,s->as.wh.cond))return 0;
            size_t jf=c->n;if(!emit(c,BC_JFALSE,0,s->pos))return 0;
            BLoop inner={0};inner.chunk=c;inner.continue_ip=top;
            if(!compile_stmts(vm,c,s->as.wh.body,s->as.wh.body_len,function,&inner)){loop_apply(c,&inner,c->n);return 0;}
            if(!emit(c,BC_JUMP,(int)top,s->pos)){loop_apply(c,&inner,c->n);return 0;}
            size_t end=c->n;c->code[jf].a=(int)end;loop_apply(c,&inner,end);break;
        }
        case STMT_FOREACH:{
            if(!compile_expr(vm,c,s->as.fe.seq))return 0;
            operand=constant(c,v_int(0));if(operand<0||!emit(c,BC_CONST,operand,s->pos))return 0;
            int name=constant(c,v_str_copy(s->as.fe.it,(int)s->as.fe.in));if(name<0)return 0;
            size_t next=c->n;if(!emit2(c,BC_ITER_NEXT,name,0,s->pos))return 0;
            BLoop inner={0};inner.chunk=c;inner.continue_ip=next;
            if(!compile_stmts(vm,c,s->as.fe.body,s->as.fe.body_len,function,&inner)){loop_apply(c,&inner,c->n);return 0;}
            if(!emit(c,BC_JUMP,(int)next,s->pos)){loop_apply(c,&inner,c->n);return 0;}
            size_t cleanup=c->n;if(inner.break_n&&!emit(c,BC_ITER_END,0,s->pos)){loop_apply(c,&inner,c->n);return 0;}
            size_t end=c->n;c->code[next].b=(int)end;loop_apply(c,&inner,inner.break_n?cleanup:end);break;
        }
        case STMT_TRY:{
            int id=try_desc_add(c,&s->as.try_);if(id<0)return 0;TryDesc*t=&c->tries[id];
            if(!compile_stmts(vm,t->body,s->as.try_.body,s->as.try_.body_len,function,loop))return 0;
            if(t->has_catch&&!compile_stmts(vm,t->catch_body,s->as.try_.catch_body,s->as.try_.catch_body_len,function,loop))return 0;
            if(t->has_finally&&!compile_stmts(vm,t->finally_body,s->as.try_.finally_body,s->as.try_.finally_body_len,function,loop))return 0;
            size_t at=c->n;if(!emit(c,BC_TRY_EXEC,id,s->pos))return 0;
            if(loop&&loop->chunk==c){if(!loop_add(vm,loop,at,1,s->pos)||!loop_add(vm,loop,at,0,s->pos))return 0;}
            break;
        }
        case STMT_SETINDEX:
            if(!compile_expr(vm,c,s->as.sidx.base)||!compile_expr(vm,c,s->as.sidx.idx)||!compile_expr(vm,c,s->as.sidx.value)||!emit(c,BC_SETINDEX,0,s->pos))return 0;
            break;
        case STMT_SETATTR:
            if(!compile_expr(vm,c,s->as.sattr.obj))return 0;
            operand=constant(c,v_str_copy(s->as.sattr.name,(int)s->as.sattr.n));
            if(operand<0||!emit(c,BC_CHECK_SET_ATTR,operand,s->pos)||
               !compile_expr(vm,c,s->as.sattr.value)||!emit(c,BC_SET_ATTR,operand,s->pos))return 0;
            break;
        case STMT_PRINT:
            for(size_t z=0;z<s->as.print.argc;z++)if(!compile_expr(vm,c,s->as.print.args[z]))return 0;
            if(s->as.print.argc>INT_MAX||!emit(c,BC_PRINT,(int)s->as.print.argc,s->pos))return 0;
            break;
        case STMT_FUNCTION:
            break; /* Nested function values are hoisted in a prologue. */
        case STMT_IMPORT:{
            operand=import_desc(c,s,0);if(operand<0)return 0;
            ImportDesc*d=&c->imports[operand];char*name=(char*)kir_oom_malloc(d->name_len+1);if(!name){fail(vm,s->pos,"Нехватка памяти имени модуля");return 0;}
            memcpy(name,d->name,d->name_len);name[d->name_len]=0;char*from=NULL;
            if(d->from_dir&&d->from_dir_len){from=(char*)kir_oom_malloc(d->from_dir_len+1);if(!from){free(name);fail(vm,s->pos,"Нехватка памяти пути модуля");return 0;}memcpy(from,d->from_dir,d->from_dir_len);from[d->from_dir_len]=0;}
            char resolved[1024];int has_file=kir_resolve_module_path(name,from,resolved,sizeof(resolved));
            int rejected=!has_file&&kir_native_has_module(name)&&!kir_native_bytecode_module_is_safe(name);free(from);free(name);
            if(rejected){unsupported(vm,s->pos,"импорт нативного модуля с системными ресурсами");return 0;}
            if(!emit(c,BC_IMPORT,operand,s->pos))return 0;break;
        }
        case STMT_INCLUDE:operand=import_desc(c,s,1);if(operand<0||!emit(c,BC_INCLUDE,operand,s->pos))return 0;break;
        default:unsupported(vm,s->pos,"оператор (обработка ошибок)");return 0;
        }
    }
    return !vm->failed;
}

static int collect_nested_functions(BVM*vm,AstStmt**body,size_t len){
    for(size_t i=0;i<len;i++){
        AstStmt*s=body[i];if(!s)continue;
        switch(s->kind){
        case STMT_FUNCTION:if(add_function(vm,s->as.function)<0)return 0;if(!collect_nested_functions(vm,s->as.function->body,s->as.function->body_len))return 0;break;
        case STMT_IF:if(!collect_nested_functions(vm,s->as.if_.then_b,s->as.if_.then_len)||!collect_nested_functions(vm,s->as.if_.else_b,s->as.if_.else_len))return 0;break;
        case STMT_WHILE:if(!collect_nested_functions(vm,s->as.wh.body,s->as.wh.body_len))return 0;break;
        case STMT_FOREACH:if(!collect_nested_functions(vm,s->as.fe.body,s->as.fe.body_len))return 0;break;
        default:break;
        }
    }
    return 1;
}
static int compile_hoisted_functions(BVM*vm,Chunk*c,AstStmt**body,size_t len){
    for(size_t i=0;i<len;i++){
        AstStmt*s=body[i];if(!s)continue;
        if(s->kind==STMT_FUNCTION){
            int id=function_index(vm,s->as.function);
            int name=constant(c,v_str_copy(s->as.function->name_start,(int)s->as.function->name_len));
            if(id<0||name<0||!emit2(c,BC_BIND_FUNC,id,name,s->pos))return 0;
        }else if(s->kind==STMT_IF){
            if(!compile_hoisted_functions(vm,c,s->as.if_.then_b,s->as.if_.then_len)||!compile_hoisted_functions(vm,c,s->as.if_.else_b,s->as.if_.else_len))return 0;
        }else if(s->kind==STMT_WHILE){if(!compile_hoisted_functions(vm,c,s->as.wh.body,s->as.wh.body_len))return 0;}
        else if(s->kind==STMT_FOREACH){if(!compile_hoisted_functions(vm,c,s->as.fe.body,s->as.fe.body_len))return 0;}
    }
    return 1;
}

static void fail(BVM *vm, AstPos pos, const char *message) {
    if (vm->failed) return;
    kir_diag_error(pos.file ? pos.file : "<скрипт>", pos.line, pos.col, "%s", message);
    kir_diag_last_error(&vm->error_diag);
    snprintf(vm->error_code,sizeof(vm->error_code),"runtime");vm->has_error=1;capture_error_stack(vm);
    vm->failed = 1;
}
static void set_error_from_last_diag(BVM*vm,const char*code){kir_diag_last_error(&vm->error_diag);snprintf(vm->error_code,sizeof(vm->error_code),"%s",code?code:"runtime");vm->has_error=1;capture_error_stack(vm);vm->failed=1;}
static Value make_error_value(const KirDiagnostic*diag,const char*code,Value stack){
    Value error=v_dict_new();
    Value text=v_str_copy(code?code:"runtime",-1);v_dict_set(&error,"код",strlen("код"),text);
    text=v_str_copy(diag->message,-1);v_dict_set(&error,"сообщение",strlen("сообщение"),text);
    text=v_str_copy(diag->file,-1);v_dict_set(&error,"файл",strlen("файл"),text);
    v_dict_set(&error,"строка",strlen("строка"),v_int(diag->line));
    v_dict_set(&error,"столбец",strlen("столбец"),v_int(diag->col));
    v_dict_set(&error,"стек",strlen("стек"),stack);
    return error;
}
static int push(BVM *vm, Value value) {
    Value*stack=(Value*)grow(vm->stack,&vm->sc,vm->sn+1,sizeof(Value));
    if (!stack) { fail(vm, vm->pos, "нехватка памяти стека"); return 0; }
    vm->stack=stack;
    vm->stack[vm->sn++] = value; return 1;
}
static Value pop(BVM *vm) { return vm->sn ? vm->stack[--vm->sn] : v_nil(); }
static int env_get(BEnv *env, const char *name, size_t len, Value *out) {
    for (BEnv *e = env; e; e = e->parent) if (v_dict_get(&e->dict, name, len, out)) return 1;
    return 0;
}
static BEnv *env_find(BEnv *env, const char *name, size_t len) {
    for (BEnv *e = env; e; e = e->parent) if (v_dict_find(&e->dict, name, len) >= 0) return e;
    return NULL;
}
static void module_unbind(BEnv*,const char*,size_t);
static void env_assign_or_declare(BEnv*env,const char*name,size_t len,Value value){
    BEnv*target=env_find(env,name,len);if(!target)target=env;
    module_unbind(target,name,len);v_dict_set(&target->dict,name,len,value);
}
static void module_unbind(BEnv*env,const char*alias,size_t len){
    BModuleBinding**link=&env->modules;while(*link){BModuleBinding*entry=*link;
        if(entry->alias_len==len&&!memcmp(entry->alias,alias,len)){*link=entry->next;free(entry->alias);free(entry->native_module);free(entry);return;}
        link=&entry->next;
    }
}
static int module_bind(BEnv*env,const char*alias,size_t len,BModule*module,const char*native){
    module_unbind(env,alias,len);BModuleBinding*entry=(BModuleBinding*)kir_oom_calloc(1,sizeof(*entry));if(!entry)return 0;
    entry->alias=(char*)kir_oom_malloc(len+1);if(!entry->alias){free(entry);return 0;}memcpy(entry->alias,alias,len);entry->alias[len]=0;entry->alias_len=len;
    entry->module=module;entry->native_module=native?(char*)kir_oom_malloc(strlen(native)+1):NULL;
    if(native&&!entry->native_module){free(entry->alias);free(entry);return 0;}
    if(native)strcpy(entry->native_module,native);
    entry->next=env->modules;env->modules=entry;return 1;
}
static BModuleBinding* module_find(BEnv*env,const char*alias,size_t len){
    for(BEnv*scope=env;scope;scope=scope->parent){
        for(BModuleBinding*m=scope->modules;m;m=m->next)if(m->alias_len==len&&!memcmp(m->alias,alias,len))return m;
        if(v_dict_find(&scope->dict,alias,len)>=0)return NULL;
    }
    return NULL;
}
static int own_program(BVM*vm,AstProgram*p){
    if(!p)return 0;
    AstProgram**items=grow(vm->owned_programs,&vm->owned_program_c,vm->owned_program_n+1,sizeof(AstProgram*));
    if(!items)return 0;vm->owned_programs=items;vm->owned_programs[vm->owned_program_n++]=p;return 1;
}
static int compile_program(BVM*vm,AstProgram*p,BEnv*scope,Chunk*init,AstPos pos){
    size_t first=vm->fn;
    if(!add_types(vm,p->types,p->type_count,scope))return 0;
    for(size_t i=0;i<p->func_count;i++){
        int id=add_function(vm,p->funcs[i]);if(id<0)return 0;
        vm->funcs[id]->module_scope=1;
    }
    for(size_t i=0;i<p->func_count;i++)if(!collect_nested_functions(vm,p->funcs[i]->body,p->funcs[i]->body_len))return 0;
    for(size_t i=first;i<vm->fn;i++){
        BFunc*f=vm->funcs[i];
        if(!compile_hoisted_functions(vm,&f->chunk,f->ast->body,f->ast->body_len)||
           !compile_stmts(vm,&f->chunk,f->ast->body,f->ast->body_len,1,NULL)||
           !emit(&f->chunk,BC_END,0,f->ast->pos))return 0;
    }
    for(size_t i=0;i<p->func_count;i++){
        int id=function_index(vm,p->funcs[i]);int name=constant(init,v_str_copy(p->funcs[i]->name_start,(int)p->funcs[i]->name_len));
        if(id<0||name<0||!emit2(init,BC_BIND_FUNC,id,name,p->funcs[i]->pos))return 0;
    }
    if(!compile_hoisted_functions(vm,init,p->init,p->init_len)||!compile_stmts(vm,init,p->init,p->init_len,0,NULL)||!emit(init,BC_END,0,pos))return 0;
    return 1;
}
static int module_import(BVM*vm,Chunk*chunk,ImportDesc*d,BEnv*scope,AstPos pos){
    char*name=(char*)kir_oom_malloc(d->name_len+1);if(!name){fail(vm,pos,"Нехватка памяти имени модуля");return 0;}
    memcpy(name,d->name,d->name_len);name[d->name_len]=0;
    char*from=NULL;if(d->from_dir&&d->from_dir_len){from=(char*)kir_oom_malloc(d->from_dir_len+1);if(from){memcpy(from,d->from_dir,d->from_dir_len);from[d->from_dir_len]=0;}}
    if(d->from_dir&&d->from_dir_len&&!from){free(name);fail(vm,pos,"Нехватка памяти пути модуля");return 0;}
    char path[1024];
        if(!kir_resolve_module_path(name,from,path,sizeof(path))){
        if(kir_native_has_module(name)){
            if(!kir_native_bytecode_module_is_safe(name)){free(from);free(name);fail(vm,pos,"Нативный модуль не разрешён в режиме байткода");return 0;}
            const char*alias=d->alias&&d->alias_len?d->alias:name;size_t an=d->alias&&d->alias_len?d->alias_len:strlen(name);
            Value dummy=v_dict_new();v_dict_set(&scope->dict,alias,an,dummy);
            if(!module_bind(scope,alias,an,NULL,name)){free(from);free(name);fail(vm,pos,"Не удалось сохранить импорт модуля");return 0;}
            free(from);free(name);return 1;
        }
        if(from&&*from)kir_diag_error("<импорт>",1,1,"Не найден модуль '%s' (из \"%s\")",name,from);
        else kir_diag_error("<импорт>",1,1,"Не найден модуль '%s' (попробуйте из \"...\" или KIRTON_PATH)",name);
        set_error_from_last_diag(vm,"runtime");free(from);free(name);return 0;
    }
    free(from);char key[1024],default_name[256];kir_normalize_module_key(path,key,sizeof(key));kir_module_name_from_path(path,default_name,sizeof(default_name));
    BModule*m=module_by_key(vm,key);
    if(m&&m->loading){kir_diag_error("<импорт>",1,1,"Циклический импорт: %s",key);set_error_from_last_diag(vm,"runtime");free(name);return 0;}
    if(!m){m=module_new(vm,key);if(!m){free(name);fail(vm,pos,"Нехватка памяти кэша модулей");return 0;}}
    if(!m->initialized){
        size_t source_len=0;char*source=kir_read_text_file(path,&source_len);
        if(!source){kir_diag_error("<импорт>",1,1,"Не удалось открыть модуль: %s",path);set_error_from_last_diag(vm,"runtime");free(name);return 0;}
        double module_compile_start=now_ms();unsigned before=kir_diag_error_count();sc_cache_file(path,source,(int)source_len);KirBigIntHeap*old_big=v_bigint_heap_set_current(NULL);AstProgram*p=parse_program(source,source_len,path);v_bigint_heap_set_current(old_big);kir_free(source);
        if(kir_diag_error_count()!=before||!p){if(p)ast_program_destroy(p);free(name);if(kir_diag_error_count()!=before)set_error_from_last_diag(vm,"runtime");else fail(vm,pos,"Не удалось разобрать импортируемый модуль");return 0;}
        if(!own_program(vm,p)){ast_program_destroy(p);free(name);fail(vm,pos,"Не удалось сохранить AST модуля");return 0;}
        m->program=p;m->env=env_new(vm,NULL);if(!m->env){free(name);fail(vm,pos,"Не удалось создать окружение модуля");return 0;}
        m->module_name=(char*)kir_oom_malloc(p->module_name&&p->module_name_len?p->module_name_len+1:strlen(default_name)+1);
        if(!m->module_name){free(name);fail(vm,pos,"Не удалось сохранить имя модуля");return 0;}
        if(p->module_name&&p->module_name_len){memcpy(m->module_name,p->module_name,p->module_name_len);m->module_name[p->module_name_len]=0;}else strcpy(m->module_name,default_name);
        m->loading=1;Chunk init={0};AstPos module_pos={p->source_file,1,1};
        int ok=compile_program(vm,p,m->env,&init,module_pos);vm->compile_ms+=now_ms()-module_compile_start;
        if(!ok&&!vm->failed)fail(vm,module_pos,"Не удалось скомпилировать импортируемый модуль");
        Value ignored=v_nil();BFlow flow=B_FLOW_NONE;if(ok&&!vm->failed)run_chunk(vm,&init,m->env,&ignored,&flow,0,NULL,module_pos,0);
        chunk_free(&init);m->loading=0;
        if(vm->failed){free(name);return 0;}
        m->exports=v_dict_new();
        for(size_t i=0;i<p->func_count;i++){Value value;if(v_dict_get(&m->env->dict,p->funcs[i]->name_start,p->funcs[i]->name_len,&value))v_dict_set(&m->exports,p->funcs[i]->name_start,p->funcs[i]->name_len,value);}
        m->initialized=1;
    }
    const char*alias=d->alias&&d->alias_len?d->alias:m->module_name;size_t an=d->alias&&d->alias_len?d->alias_len:strlen(m->module_name);
    v_dict_set(&scope->dict,alias,an,m->exports);
    if(!module_bind(scope,alias,an,m,NULL)){free(name);fail(vm,pos,"Не удалось сохранить импорт модуля");return 0;}
    if(strcmp(alias,m->module_name)&&!module_bind(scope,m->module_name,strlen(m->module_name),m,NULL)){free(name);fail(vm,pos,"Не удалось сохранить имя модуля");return 0;}
    (void)chunk;free(name);return 1;
}
static int include_file(BVM*vm,ImportDesc*d,BEnv*scope,AstPos pos){
    char*path=(char*)kir_oom_malloc(d->name_len+1);if(!path){fail(vm,pos,"Нехватка памяти пути включения");return 0;}
    memcpy(path,d->name,d->name_len);path[d->name_len]=0;size_t len=0;char*source=kir_read_text_file(path,&len);
    if(!source){kir_diag_error("<включить>",1,1,"Не удалось открыть для включения");set_error_from_last_diag(vm,"runtime");free(path);return 0;}
    double include_compile_start=now_ms();unsigned before=kir_diag_error_count();sc_cache_file(path,source,(int)len);KirBigIntHeap*old_big=v_bigint_heap_set_current(NULL);AstProgram*p=parse_program(source,len,path);v_bigint_heap_set_current(old_big);kir_free(source);
    if(kir_diag_error_count()!=before||!p){if(p)ast_program_destroy(p);free(path);if(kir_diag_error_count()!=before)set_error_from_last_diag(vm,"runtime");else fail(vm,pos,"Не удалось разобрать включаемый файл");return 0;}
    if(!own_program(vm,p)){ast_program_destroy(p);free(path);fail(vm,pos,"Не удалось сохранить AST включаемого файла");return 0;}
    Chunk init={0};int ok=compile_program(vm,p,scope,&init,pos);vm->compile_ms+=now_ms()-include_compile_start;if(!ok&&!vm->failed)fail(vm,pos,"Не удалось скомпилировать включаемый файл");
    Value ignored=v_nil();BFlow flow=B_FLOW_NONE;if(ok&&!vm->failed)run_chunk(vm,&init,scope,&ignored,&flow,0,NULL,pos,0);
    chunk_free(&init);free(path);return !vm->failed;
}
static void gc_mark_chunk(BVM*vm,Chunk*c){
    if(!c)return;
    for(size_t i=0;i<c->cn;i++)v_value_heap_mark(vm->heap,vm->bigs,c->constants[i]);
    for(size_t i=0;i<c->try_n;i++){gc_mark_chunk(vm,c->tries[i].body);gc_mark_chunk(vm,c->tries[i].catch_body);gc_mark_chunk(vm,c->tries[i].finally_body);}
}
static void gc_mark(BVM *vm) {
    v_value_heap_mark(vm->heap, vm->bigs, vm->global_env.dict);
    for (size_t i = 0; i < vm->sn; ++i) v_value_heap_mark(vm->heap, vm->bigs, vm->stack[i]);
    v_value_heap_mark(vm->heap,vm->bigs,vm->error_stack);
    for(size_t i=0;i<vm->error_root_n;i++)v_value_heap_mark(vm->heap,vm->bigs,vm->error_roots[i]);
    gc_mark_chunk(vm,vm->init);
    for (size_t f = 0; f < vm->fn; ++f) gc_mark_chunk(vm,&vm->funcs[f]->chunk);
    for(size_t i=0;i<vm->module_n;i++)if(vm->modules[i]){
        BModule*m=vm->modules[i];if(m->env)v_value_heap_mark(vm->heap,vm->bigs,m->env->dict);
        v_value_heap_mark(vm->heap,vm->bigs,m->exports);
    }
    /* Imported-module initialization executes a temporary chunk outside vm->init and BFunc. */
    for (BFrame *fr = vm->frames; fr; fr = fr->caller) {
        for (BEnv *e = fr->env; e; e = e->parent) v_value_heap_mark(vm->heap, vm->bigs, e->dict);
        gc_mark_chunk(vm,fr->chunk);
    }
}
static size_t heap_count(BVM *vm) {
    size_t values=v_value_heap_count(vm->heap), bigs=kir_bigint_heap_count(vm->bigs);
    return values>SIZE_MAX-bigs?SIZE_MAX:values+bigs;
}
static void gc_collect(BVM *vm) {
    gc_mark(vm);vm->reclaimed+=v_value_heap_sweep(vm->heap);vm->reclaimed+=kir_bigint_heap_sweep(vm->bigs);++vm->collections;
}
static void gc_safepoint(BVM *vm) {
    size_t count = heap_count(vm);
    if (count > vm->peak) vm->peak = count;
    if (count < vm->next_gc) return;
    gc_collect(vm);
    count = heap_count(vm);
    if (count > vm->peak) vm->peak = count;
    vm->next_gc = count > (SIZE_MAX - 128) / 2 ? SIZE_MAX : count * 2 + 128;
}
static int index_get(BVM *vm, Value base, Value idx, Value *out, AstPos pos) {
    if (base.kind == VAL_LIST && idx.kind == VAL_NUM) {
        int64_t i; if (!v_num_fits_i64(idx, &i)) { fail(vm, pos, "Индекс списка должен быть целым в диапазоне int64"); return 0; }
        if (i < 0 || (uint64_t)i >= (uint64_t)base.as.list->len) { fail(vm, pos, "Индекс списка вне диапазона"); return 0; }
        *out = base.as.list->data[(size_t)i]; return 1;
    }
    if (base.kind == VAL_STR && idx.kind == VAL_NUM) {
        int64_t index;
        if (!v_num_fits_i64(idx, &index) || index < 0) { fail(vm, pos, "Индекс строки должен быть неотрицательным целым"); return 0; }
        const char *p=base.as.str.s,*end=p+base.as.str.n;int64_t at=0;
        while(p<end){uint32_t cp=0;int width=kir_utf8_decode(p,end,&cp);if(width<=0){fail(vm,pos,"Строка содержит недопустимый UTF-8");return 0;}if(at==index){*out=v_str_copy(p,width);return 1;}p+=width;++at;}
        fail(vm,pos,"Индекс строки вне диапазона");return 0;
    }
    if (base.kind == VAL_DICT && idx.kind == VAL_STR) { if (!v_dict_get(&base, idx.as.str.s, idx.as.str.n, out)) *out = v_nil(); return 1; }
    *out = v_nil(); return 1;
}
static AstTypeField* field_named(AstUserType*type,const char*name,size_t len){
    return type_field(type,name,len);
}
static int attr_get(BVM*vm,Value object,const char*name,size_t len,Value*out,AstPos pos){
    if(object.kind==VAL_USER_STRUCT){
        Value fields=v_user_struct_fields(object);
        if(!v_dict_get(&fields,name,len,out)){fail(vm,pos,"Неизвестное поле структуры");return 0;}
        return 1;
    }
    if(object.kind==VAL_DICT){if(!v_dict_get(&object,name,len,out))*out=v_nil();return 1;}
    if(object.kind==VAL_RECORD){if(!v_record_get(object,name,len,out)){fail(vm,pos,"Неизвестное поле или недействительная запись");return 0;}return 1;}
    *out=v_nil();return 1;
}
static int attr_set(BVM*vm,BEnv*scope,Value object,const char*name,size_t len,Value value,AstPos pos){
    if(object.kind==VAL_DICT){v_dict_set(&object,name,len,value);return 1;}
    if(object.kind==VAL_RECORD){if(!v_record_set(object,name,len,value)){fail(vm,pos,"Неверный тип, поле или недействительная запись");return 0;}return 1;}
    if(object.kind==VAL_USER_STRUCT){
        AstUserType*type=v_user_struct_type(object);AstTypeField*field=field_named(type,name,len);
        if(!field||!value_matches_type(vm,scope,field->type_name,field->type_name_len,value)){fail(vm,pos,"Неверный тип или поле пользовательской структуры");return 0;}
        Value fields=v_user_struct_fields(object);v_dict_set(&fields,name,len,value);return 1;
    }
    fail(vm,pos,"Присваивание полей возможно только для словаря или записи");return 0;
}
static BFunc* function_by_ast(BVM*vm,AstFunc*ast){int i=function_index(vm,ast);return i<0?NULL:vm->funcs[i];}
static int is_constructor_target(BVM*vm,Value value){return call_target_type(vm,value)>=0;}
static Value direct_function_marker(BVM*vm,size_t id){return call_target_marker(vm->type_n+2+id);}
static int prepare_call(BVM*vm,Chunk*chunk,int descriptor,AstPos pos){
    CallDesc*d=&chunk->calls[descriptor];Value target;
    BEnv*scope=vm->frames?vm->frames->env:&vm->global_env;
    if(d->value_callee){
        if(!vm->sn){fail(vm,pos,"Повреждён стек вызываемого значения");return 0;}
        target=vm->stack[vm->sn-1];
        if(target.kind!=VAL_FUNCTION||!v_function_is_owned(vm->heap,target)||!v_function_ast(target)){
            fail(vm,pos,"Значение выражения не является функцией");return 0;
        }
        for(size_t i=0;i<d->argc;i++)if(d->arg_names&&d->arg_names[i]){
            fail(vm,pos,"Именованные аргументы не поддерживаются для этой функции");return 0;
        }
        return 1;
    }
    if(d->module_attr){
        BModuleBinding*binding=module_find(scope,d->module_name,d->module_name_len);
        if(binding){
            if(binding->module){
                Value exports=binding->module->exports;
                if(!v_dict_get(&exports,d->member,d->member_len,&target)){fail(vm,pos,"Неизвестный экспорт модуля");return 0;}
                if(target.kind!=VAL_FUNCTION||!v_function_is_owned(vm->heap,target)){fail(vm,pos,"Экспорт модуля не является функцией");return 0;}
                for(size_t i=0;i<d->argc;i++)if(d->arg_names&&d->arg_names[i]){fail(vm,pos,"Именованные аргументы не поддерживаются для этой функции");return 0;}
                return push(vm,target);
            }
            if(binding->native_module){
                if(strcmp(binding->native_module,"внешний")==0)
                    return push(vm,call_target_marker(vm->type_n+2+vm->fn+(size_t)descriptor));
                char full[512];int n=snprintf(full,sizeof(full),"%s.%.*s",binding->native_module,(int)d->member_len,d->member);
                if(n<0||(size_t)n>=sizeof(full)||(d->native_slot=kir_native_bytecode_slot(full,(size_t)n))<0){fail(vm,pos,"Нативная функция не поддерживается в режиме байткода");return 0;}
                for(size_t i=0;i<d->argc;i++)if(d->arg_names&&d->arg_names[i]){fail(vm,pos,"Именованные аргументы не поддерживаются для этой функции");return 0;}
                return push(vm,call_target_marker(vm->type_n+2+vm->fn+(size_t)descriptor));
            }
        }
        if(env_get(scope,d->module_name,d->module_name_len,&target)){
            if(attr_get(vm,target,d->member,d->member_len,&target,pos)){
                if(target.kind!=VAL_FUNCTION||!v_function_is_owned(vm->heap,target)){fail(vm,pos,"Значение атрибута не является функцией");return 0;}
                for(size_t i=0;i<d->argc;i++)if(d->arg_names&&d->arg_names[i]){fail(vm,pos,"Именованные аргументы не поддерживаются для этой функции");return 0;}
                return push(vm,target);
            }
            return 0;
        }
        if(kir_native_has_module(d->module_name)){
            if(nameeq(d->module_name,d->module_name_len,"внешний",strlen("внешний"))){fail(vm,pos,"Неизвестный модуль или переменная");return 0;}
            char full[512];int n=snprintf(full,sizeof(full),"%.*s.%.*s",(int)d->module_name_len,d->module_name,(int)d->member_len,d->member);
            if(n>=0&&(size_t)n<sizeof(full)&&(d->native_slot=kir_native_bytecode_slot(full,(size_t)n))>=0&&kir_native_bytecode_module_is_safe(d->module_name)){
                for(size_t i=0;i<d->argc;i++)if(d->arg_names&&d->arg_names[i]){fail(vm,pos,"Именованные аргументы не поддерживаются для этой функции");return 0;}
                return push(vm,call_target_marker(vm->type_n+2+vm->fn+(size_t)descriptor));
            }
            fail(vm,pos,"Нативная функция или модуль не поддерживается в режиме байткода");return 0;
        }
        fail(vm,pos,"Неизвестный модуль или переменная");return 0;
    }
    /* AST resolves top-level registered native functions before the function
       table, but an explicit local Value binding shadows the native. Bytecode
       binds top-level functions into the global environment, so restore the
       same precedence before consulting that environment. */
    if(d->name && d->name_len){
        int is_top_level_func=0;
        for(size_t i=0;i<vm->fn;++i)
            if(vm->funcs[i]->module_scope &&
               nameeq(vm->funcs[i]->name,vm->funcs[i]->len,d->name,d->name_len)){
                is_top_level_func=1;break;
            }
        Value local=v_nil();
        int has_local=scope!=&vm->global_env &&
            v_dict_get(&scope->dict,d->name,d->name_len,&local);
        if(is_top_level_func && !has_local &&
           (d->native_slot=kir_native_bytecode_slot(d->name,d->name_len))>=0){
            for(size_t i=0;i<d->argc;++i)if(d->arg_names&&d->arg_names[i]){
                fail(vm,pos,"Именованные аргументы не поддерживаются для этой функции");return 0;
            }
            return push(vm,call_target_marker(vm->type_n+2+vm->fn+(size_t)descriptor));
        }
    }
    if(env_get(vm->frames?vm->frames->env:&vm->global_env,d->name,d->name_len,&target)){
        AstFunc*ast=v_function_ast(target);
        if(target.kind!=VAL_FUNCTION||!v_function_is_owned(vm->heap,target)||!ast){
            fail(vm,pos,"Значение не является функцией");return 0;
        }
        for(size_t i=0;i<d->argc;i++)if(d->arg_names&&d->arg_names[i]){fail(vm,pos,"Именованные аргументы не поддерживаются для этой функции");return 0;}
        if(d->function_hint>=0&&(size_t)d->function_hint<vm->fn&&vm->funcs[d->function_hint]->module_scope&&
           vm->funcs[d->function_hint]->ast==ast&&v_function_closure(target)==&vm->global_env)
            return push(vm,direct_function_marker(vm,(size_t)d->function_hint));
        return push(vm,target);
    }
    int type_id=user_type_index(vm,scope,d->name,d->name_len);
    if(type_id>=0){
        AstUserType*type=vm->types[type_id].type;
        if(d->argc!=type->field_count){fail(vm,pos,"Неверное число полей структуры");return 0;}
        if(!push(vm,call_target_marker((size_t)type_id)))return 0;
        return push(vm,v_dict_new());
    }
    if(d->argc==0&&nameeq(d->name,d->name_len,"нан",strlen("нан")))return push(vm,call_target_marker(vm->type_n));
    if(d->argc==0&&nameeq(d->name,d->name_len,"беск",strlen("беск")))return push(vm,call_target_marker(vm->type_n+1));
    if((d->native_slot=kir_native_bytecode_slot(d->name,d->name_len))>=0)return push(vm,call_target_marker(vm->type_n+2+vm->fn+(size_t)descriptor));
    fail(vm,pos,"Неизвестная функция или переменная");return 0;
}

static int bytecode_ffi_invoke(BVM*vm,CallDesc*d,const Value*args,int argc,
                               Value*out,AstPos pos){
    if(!vm->ffi_enabled||!vm->ffi){
        fail(vm,pos,"Ошибка вызова внешней библиотеки: Внешние библиотеки отключены (нужно --allow-ffi)");
        return 0;
    }
    if(nameeq(d->member,d->member_len,"загрузить",strlen("загрузить"))){
        if(argc!=1||args[0].kind!=VAL_STR||
           memchr(args[0].as.str.s,0,args[0].as.str.n)){
            fail(vm,pos,"Неверные аргументы внешнего вызова: ожидаются имена-строки без NUL");
            return 0;
        }
        char*path=(char*)kir_oom_malloc(args[0].as.str.n+1);
        if(!path){fail(vm,pos,"Нехватка памяти имени библиотеки FFI");return 0;}
        memcpy(path,args[0].as.str.s,args[0].as.str.n);path[args[0].as.str.n]=0;
        int ok=kir_ffi_state_load(vm->ffi,path);
        free(path);
        if(!ok){
            char message[512];snprintf(message,sizeof(message),"Ошибка вызова внешней библиотеки: %s",kir_ffi_state_error(vm->ffi));
            fail(vm,pos,message);return 0;
        }
        *out=v_bool(true);return 1;
    }
    if(nameeq(d->member,d->member_len,"вызвать",strlen("вызвать"))){
        if(argc<2||args[0].kind!=VAL_STR||args[1].kind!=VAL_STR||
           memchr(args[0].as.str.s,0,args[0].as.str.n)||
           memchr(args[1].as.str.s,0,args[1].as.str.n)){
            fail(vm,pos,"Неверные аргументы внешнего вызова: ожидаются имена-строки без NUL");
            return 0;
        }
        char*module=(char*)kir_oom_malloc(args[0].as.str.n+1);
        char*function=(char*)kir_oom_malloc(args[1].as.str.n+1);
        if(!module||!function){free(module);free(function);fail(vm,pos,"Нехватка памяти имени функции FFI");return 0;}
        memcpy(module,args[0].as.str.s,args[0].as.str.n);module[args[0].as.str.n]=0;
        memcpy(function,args[1].as.str.s,args[1].as.str.n);function[args[1].as.str.n]=0;
        int ok=kir_ffi_state_call(vm->ffi,vm->heap,module,function,argc-2,args+2,out);
        free(module);free(function);
        if(!ok){
            char message[512];snprintf(message,sizeof(message),"Ошибка вызова внешней библиотеки: %s",kir_ffi_state_error(vm->ffi));
            fail(vm,pos,message);return 0;
        }
        return 1;
    }
    fail(vm,pos,"В модуле внешний нет такой функции");
    return 0;
}
static int invoke_code(BVM*vm,BFunc*func,BEnv*closure,size_t target_at,CallDesc*d,Value*out,unsigned depth,AstPos pos){
    if(!func){fail(vm,pos,"Скомпилированный код функции не найден");return 0;}
    if(func->ast->param_count!=d->argc){fail(vm,pos,"Неверное число аргументов функции");return 0;}
    if(!closure)closure=&vm->global_env;
    BEnv*call_env=env_new(vm,closure);if(!call_env){fail(vm,pos,"Не удалось создать окружение вызова");return 0;}
    for(size_t i=0;i<d->argc;i++)v_dict_set(&call_env->dict,func->ast->params_start[i],func->ast->params_len[i],vm->stack[target_at+1+i]);
    vm->sn=target_at;Value result=v_nil();BFlow flow=B_FLOW_NONE;
    run_chunk(vm,&func->chunk,call_env,&result,&flow,depth+1,func,pos,0);
    env_owner_release(vm,call_env);
    if(flow==B_FLOW_BREAK||flow==B_FLOW_CONTINUE){fail(vm,pos,"Управление циклом не может выходить из функции");}
    if(!vm->failed){*out=result;return 1;}
    return 0;
}
static int invoke_function(BVM*vm,Value callable,size_t target_at,CallDesc*d,Value*out,unsigned depth,AstPos pos){
    AstFunc*ast=v_function_ast(callable);
    BFunc*func=(d->function_hint>=0&&(size_t)d->function_hint<vm->fn&&vm->funcs[d->function_hint]->ast==ast)?
        vm->funcs[d->function_hint]:function_by_ast(vm,ast);
    return invoke_code(vm,func,(BEnv*)v_function_closure(callable),target_at,d,out,depth,pos);
}
static int run_chunk(BVM *vm,Chunk *chunk,BEnv *scope,Value *out,BFlow *out_flow,unsigned depth,BFunc *function,AstPos call_pos,int internal);
static int execute_try(BVM*vm,TryDesc*t,BEnv*scope,BFunc*function,AstPos call_pos,unsigned depth,Value*out,BFlow*out_flow,AstPos pos){
    kir_diag_silence_push();++vm->try_depth;
    BFlow flow=B_FLOW_NONE;Value result=v_nil();
    run_chunk(vm,t->body,scope,&result,&flow,depth,function,call_pos,1);
    int pending=vm->failed;KirDiagnostic saved=vm->error_diag;char saved_code[sizeof(vm->error_code)];snprintf(saved_code,sizeof(saved_code),"%s",vm->error_code);Value saved_stack=vm->error_stack;
    if(pending&&t->has_catch){
        vm->failed=0;vm->has_error=0;
        BEnv*catch_env=env_new(vm,scope);
        if(!catch_env)fail(vm,pos,"Не удалось создать окружение перехвата");
        else{
            Value error=make_error_value(&saved,saved_code,saved_stack);v_dict_set(&catch_env->dict,t->catch_name,t->catch_name_len,error);vm->error_stack=v_nil();
            BFlow catch_flow=B_FLOW_NONE;Value catch_result=v_nil();
            run_chunk(vm,t->catch_body,catch_env,&catch_result,&catch_flow,depth,function,call_pos,1);
            env_owner_release(vm,catch_env);result=catch_result;flow=catch_flow;
            pending=vm->failed;if(pending){saved=vm->error_diag;snprintf(saved_code,sizeof(saved_code),"%s",vm->error_code);saved_stack=vm->error_stack;}
        }
    }
    if(t->has_finally){
        int prior_error=vm->failed;KirDiagnostic prior_diag=vm->error_diag;char prior_code[sizeof(vm->error_code)];snprintf(prior_code,sizeof(prior_code),"%s",vm->error_code);Value prior_stack=vm->error_stack;if(prior_error)error_root_push(vm,prior_stack);
        vm->failed=0;vm->has_error=0;
        BFlow final_flow=B_FLOW_NONE;Value final_result=v_nil();
        /* The pending return is outside the bytecode operand stack while
           finally executes; retain it as a GC root until flow resolution. */
        error_root_push(vm,result);
        run_chunk(vm,t->finally_body,scope,&final_result,&final_flow,depth,function,call_pos,1);
        error_root_pop(vm);
        if(vm->failed){pending=1;saved=vm->error_diag;snprintf(saved_code,sizeof(saved_code),"%s",vm->error_code);saved_stack=vm->error_stack;flow=B_FLOW_NONE;}
        else if(final_flow!=B_FLOW_NONE){pending=0;flow=final_flow;result=final_result;vm->error_stack=v_nil();}
        else if(prior_error){pending=1;saved=prior_diag;snprintf(saved_code,sizeof(saved_code),"%s",prior_code);saved_stack=prior_stack;vm->failed=1;vm->has_error=1;vm->error_diag=prior_diag;vm->error_stack=prior_stack;snprintf(vm->error_code,sizeof(vm->error_code),"%s",prior_code);}
        else pending=0;
        if(prior_error)error_root_pop(vm);
    }
    if(!vm->failed&&pending){vm->failed=1;vm->has_error=1;vm->error_diag=saved;snprintf(vm->error_code,sizeof(vm->error_code),"%s",saved_code);}
    kir_diag_silence_pop();if(vm->try_depth)--vm->try_depth;
    if(vm->failed&&vm->try_depth==0)kir_diag_replay_last_error();
    *out=result;*out_flow=flow;return !vm->failed;
}
static int run_chunk(BVM *vm,Chunk *chunk,BEnv *scope,Value *out,BFlow *out_flow,unsigned depth,BFunc *function,AstPos call_pos,int internal){
    if(depth>256){fail(vm,vm->pos,"Превышена глубина вызовов");return 0;}
    env_active_enter(scope);BFrame frame={scope,function,chunk,call_pos,internal,vm->frames};vm->frames=&frame;
    size_t base=vm->sn,ip=0;int returned=0;Value result=v_nil();BFlow flow=B_FLOW_NONE;
    while(ip<chunk->n&&!vm->failed&&!returned){
        Ins in=chunk->code[ip++];vm->pos=in.pos;Value a,b;
        switch(in.op){
        case BC_CONST:push(vm,chunk->constants[in.a]);break;
        case BC_MATH_NAN:push(vm,v_num(NAN));break;
        case BC_MATH_INF:push(vm,v_num(INFINITY));break;
        case BC_LOAD:{Value key=chunk->constants[in.a];if(!env_get(scope,key.as.str.s,key.as.str.n,&a))fail(vm,in.pos,"Неизвестная переменная");else push(vm,a);break;}
        case BC_LET:case BC_SET:{a=pop(vm);Value key=chunk->constants[in.a];BEnv*target=in.op==BC_LET?scope:env_find(scope,key.as.str.s,key.as.str.n);if(!target){fail(vm,in.pos,"Присваивание неизвестной переменной");break;}module_unbind(target,key.as.str.s,key.as.str.n);v_dict_set(&target->dict,key.as.str.s,key.as.str.n,a);break;}
        case BC_BIND_FUNC:{BFunc*func=vm->funcs[in.a];Value name=chunk->constants[in.b];Value f=function_value_new(vm,func->ast,scope);if(f.kind!=VAL_FUNCTION)fail(vm,in.pos,"Не удалось создать значение функции");else v_dict_set(&scope->dict,name.as.str.s,name.as.str.n,f);break;}
        case BC_POP:(void)pop(vm);break;
        case BC_PRINT:{size_t n=(size_t)in.a;Value*items=(Value*)kir_oom_malloc((n?n:1)*sizeof(Value));if(!items){fail(vm,in.pos,"Нехватка памяти печати");break;}for(size_t z=n;z>0;--z)items[z-1]=pop(vm);for(size_t z=0;z<n;++z){if(z)printf(" ");v_print(items[z]);}putchar('\n');free(items);break;}
        case BC_NEG:a=pop(vm);push(vm,a.kind==VAL_NUM?v_num_neg(a):v_nil());break;
        case BC_NOT:a=pop(vm);push(vm,v_bool(!v_truthy(a)));break;
        case BC_BITNOT:a=pop(vm);if(!v_num_is_integer(a))fail(vm,in.pos,"Побитовое отрицание '~' требует целое число");else push(vm,v_num_bitnot(a));break;
        case BC_BOOLIFY:a=pop(vm);push(vm,v_bool(v_truthy(a)));break;
        case BC_MAKE_LIST:{size_t n=(size_t)in.a;if(vm->sn<base+n){fail(vm,in.pos,"Повреждён стек операндов");break;}size_t at=vm->sn-n;Value x=v_list_new();if(!push(vm,x))break;for(size_t z=0;z<n;++z)v_list_push(&x,vm->stack[at+z]);vm->stack[at]=x;vm->sn=at+1;break;}
        case BC_MAKE_DICT:{DictKeys*k=&chunk->dicts[in.a];size_t n=k->n;if(vm->sn<base+n){fail(vm,in.pos,"Повреждён стек операндов");break;}size_t at=vm->sn-n;Value x=v_dict_new();if(!push(vm,x))break;for(size_t z=0;z<n;++z)v_dict_set(&x,k->keys[z],k->lens[z],vm->stack[at+z]);vm->stack[at]=x;vm->sn=at+1;break;}
        case BC_INDEX:b=pop(vm);a=pop(vm);if(index_get(vm,a,b,&result,in.pos))push(vm,result);break;
        case BC_SETINDEX:{Value val=pop(vm),idx=pop(vm),basev=pop(vm);if(basev.kind==VAL_LIST&&idx.kind==VAL_NUM){int64_t i;if(!v_num_fits_i64(idx,&i))fail(vm,in.pos,"Индекс списка должен быть целым в диапазоне int64");else if(i<0||(uint64_t)i>=basev.as.list->len)fail(vm,in.pos,"Индекс списка вне диапазона");else basev.as.list->data[(size_t)i]=val;}else if(basev.kind==VAL_DICT&&idx.kind==VAL_STR)v_dict_set(&basev,idx.as.str.s,idx.as.str.n,val);else fail(vm,in.pos,"Неверные типы для присваивания по индексу");break;}
        case BC_GET_ATTR:{a=pop(vm);Value name=chunk->constants[in.a];if(attr_get(vm,a,name.as.str.s,name.as.str.n,&result,in.pos))push(vm,result);break;}
        case BC_ATTR_NAME:{AttrDesc*d=&chunk->attrs[in.a];if(env_get(scope,d->object_name,d->object_name_len,&a)){if(attr_get(vm,a,d->member,d->member_len,&result,in.pos))push(vm,result);}else{int tid=enum_type_index(vm,scope,d->object_name,d->object_name_len);if(tid<0){fail(vm,in.pos,"Неизвестная переменная перечисления");break;}AstUserType*t=vm->types[tid].type;int found=0;for(size_t i=0;i<t->variant_count;i++)if(t->variant_lens[i]==d->member_len&&!memcmp(t->variants[i],d->member,d->member_len)){push(vm,v_enum_value_new(t,t->variants[i],t->variant_lens[i]));found=1;break;}if(!found)fail(vm,in.pos,"Неизвестный вариант перечисления");}break;}
        case BC_CHECK_SET_ATTR:{a=vm->sn>base?vm->stack[vm->sn-1]:v_nil();if(a.kind!=VAL_DICT&&a.kind!=VAL_USER_STRUCT&&a.kind!=VAL_RECORD)fail(vm,in.pos,"Присваивание полей возможно только для словаря или записи");break;}
        case BC_SET_ATTR:{Value val=pop(vm),obj=pop(vm),name=chunk->constants[in.a];attr_set(vm,scope,obj,name.as.str.s,name.as.str.n,val,in.pos);break;}
        case BC_JUMP:ip=(size_t)in.a;break;
        case BC_JFALSE:a=pop(vm);if(!v_truthy(a))ip=(size_t)in.a;break;
        case BC_JTRUE:a=pop(vm);if(v_truthy(a))ip=(size_t)in.a;break;
        case BC_TRY_EXEC:{
            TryDesc*t=&chunk->tries[in.a];BFlow try_flow=B_FLOW_NONE;Value try_result=v_nil();
            execute_try(vm,t,scope,function,call_pos,depth,&try_result,&try_flow,in.pos);
            if(vm->failed)break;
            if(try_flow==B_FLOW_RETURN){result=try_result;flow=B_FLOW_RETURN;returned=1;}
            else if(try_flow==B_FLOW_BREAK){if(t->break_target!=SIZE_MAX)ip=t->break_target;else{flow=try_flow;returned=1;}}
            else if(try_flow==B_FLOW_CONTINUE){if(t->continue_target!=SIZE_MAX)ip=t->continue_target;else{flow=try_flow;returned=1;}}
            break;
        }
        case BC_IMPORT:module_import(vm,chunk,&chunk->imports[in.a],scope,in.pos);break;
        case BC_INCLUDE:include_file(vm,&chunk->imports[in.a],scope,in.pos);break;
        case BC_PREP_CALL:prepare_call(vm,chunk,in.a,in.pos);break;
        case BC_ARG_CHECK:{CallDesc*d=&chunk->calls[in.a];if(vm->sn<1){fail(vm,in.pos,"Повреждён стек аргументов");break;}size_t target_at;Value target;if(vm->sn>=2&&is_constructor_target(vm,vm->stack[vm->sn-2])){target_at=vm->sn-2;target=vm->stack[target_at];}else{target_at=vm->sn-(size_t)in.b-1;target=vm->stack[target_at];}int tid=call_target_type(vm,target);if(tid>=0){const char*field=d->arg_names?d->arg_names[in.b]:NULL;size_t fl=d->arg_name_lens?d->arg_name_lens[in.b]:0;if(!field){fail(vm,in.pos,"Неизвестное поле структуры");break;}AstTypeField*f=field_named(vm->types[tid].type,field,fl);if(!f){fail(vm,in.pos,"Неизвестное поле структуры");break;}Value fields=vm->stack[target_at+1];if(v_dict_find(&fields,field,fl)>=0)fail(vm,in.pos,"Повторное поле структуры");}break;}
        case BC_ARG_STORE:{CallDesc*d=&chunk->calls[in.a];size_t target_at;Value target;if(vm->sn>=3&&is_constructor_target(vm,vm->stack[vm->sn-3])){target_at=vm->sn-3;target=vm->stack[target_at];}else{target_at=vm->sn-(size_t)in.b-2;target=vm->stack[target_at];}int tid=call_target_type(vm,target);if(tid>=0){Value val=pop(vm);const char*field=d->arg_names[in.b];size_t fl=d->arg_name_lens[in.b];AstTypeField*f=field_named(vm->types[tid].type,field,fl);if(!f||!value_matches_type(vm,scope,f->type_name,f->type_name_len,val)){fail(vm,in.pos,"Неверный тип поля пользовательской структуры");break;}Value fields=vm->stack[target_at+1];v_dict_set(&fields,field,fl,val);}break;}
        case BC_CALL_VALUE:{
            CallDesc*d=&chunk->calls[in.a];
            int struct_id=vm->sn>=2?call_target_type(vm,vm->stack[vm->sn-2]):-1;
            if(struct_id>=0){size_t at=vm->sn-2;Value fields=vm->stack[at+1];Value instance=v_user_struct_new(vm->types[struct_id].type,fields);if(instance.kind!=VAL_USER_STRUCT){fail(vm,in.pos,"Не удалось создать экземпляр структуры");break;}vm->sn=at;push(vm,instance);}
            else{
                size_t at=vm->sn-d->argc-1;Value callable=vm->stack[at];int tag=call_target_tag(vm,callable);
                if(tag==(int)vm->type_n||tag==(int)vm->type_n+1){vm->sn=at;push(vm,v_num(tag==(int)vm->type_n?NAN:INFINITY));}
                else if(tag==(int)(vm->type_n+2+vm->fn+(size_t)in.a)){
                    if(d->module_attr){
                        BModuleBinding*binding=module_find(scope,d->module_name,d->module_name_len);
                        if(binding&&binding->native_module&&strcmp(binding->native_module,"внешний")==0){
                            Value ffi_result=v_nil();
                            if(bytecode_ffi_invoke(vm,d,vm->stack+at+1,(int)d->argc,&ffi_result,in.pos)){
                                vm->sn=at;push(vm,ffi_result);
                            }
                            break;
                        }
                    }
                    Value native_result=v_nil();unsigned errors_before=kir_diag_error_count();
                    if(!kir_native_call_bytecode_slot(d->native_slot,(int)d->argc,vm->stack+at+1,&native_result))fail(vm,in.pos,"Нативная функция не поддерживается в режиме байткода");
                    else if(kir_diag_error_count()!=errors_before)set_error_from_last_diag(vm,"runtime");
                    else{vm->sn=at;push(vm,native_result);}
                }
                else if(tag>=(int)vm->type_n+2&&(size_t)(tag-(int)vm->type_n-2)<vm->fn){
                    BFunc*f=vm->funcs[tag-(int)vm->type_n-2];invoke_code(vm,f,&vm->global_env,at,d,&result,depth,in.pos);if(!vm->failed)push(vm,result);
                }else{invoke_function(vm,callable,at,d,&result,depth,in.pos);if(!vm->failed)push(vm,result);}
            }
            break;
        }
        case BC_FLOW_BREAK:flow=B_FLOW_BREAK;returned=1;break;
        case BC_FLOW_CONTINUE:flow=B_FLOW_CONTINUE;returned=1;break;
        case BC_ITER_NEXT:{
            if(vm->sn<base+2){fail(vm,in.pos,"Повреждён стек итератора");break;}
            size_t seq_at=vm->sn-2,idx_at=vm->sn-1;Value seq=vm->stack[seq_at],idxv=vm->stack[idx_at],item=v_nil();
            int exhausted=0;int64_t index=0;if(!v_num_fits_i64(idxv,&index)||index<0){fail(vm,in.pos,"Недопустимый индекс итератора");break;}
            if(seq.kind==VAL_LIST){if((uint64_t)index>=seq.as.list->len)exhausted=1;else item=seq.as.list->data[(size_t)index];}
            else if(seq.kind==VAL_STR){if((uint64_t)index>=seq.as.str.n)exhausted=1;else{const char*p=seq.as.str.s+(size_t)index,*end=seq.as.str.s+seq.as.str.n;uint32_t cp=0;int width=kir_utf8_decode(p,end,&cp);if(width<=0){fail(vm,in.pos,"Строка содержит недопустимый UTF-8");break;}item=v_str_copy(p,width);index+=width;}}
            else if(seq.kind==VAL_TYPED_ARRAY){size_t count=0;if(!v_typed_array_length(seq,&count)){fail(vm,in.pos,"Нельзя перебрать недействительный типизированный массив");break;}if((uint64_t)index>=count)exhausted=1;else if(!v_typed_array_get(seq,(size_t)index,&item)){fail(vm,in.pos,"Типизированный массив был закрыт или сброшен во время перебора");break;}}
            else exhausted=1;
            if(exhausted){vm->sn-=2;ip=(size_t)in.b;break;}
            Value key=chunk->constants[in.a];env_assign_or_declare(scope,key.as.str.s,key.as.str.n,item);if(seq.kind!=VAL_STR)++index;vm->stack[idx_at]=v_int(index);break;
        }
        case BC_ITER_END:if(vm->sn<base+2)fail(vm,in.pos,"Повреждён стек итератора");else vm->sn-=2;break;
        case BC_RET:result=pop(vm);flow=B_FLOW_RETURN;returned=1;break;
        case BC_END:result=v_nil();flow=B_FLOW_NONE;returned=1;break;
        default:b=pop(vm);a=pop(vm);
            if(in.op==BC_ADD&&a.kind==VAL_STR&&b.kind==VAL_STR){if(a.as.str.n>SIZE_MAX-b.as.str.n||a.as.str.n+b.as.str.n>INT_MAX){fail(vm,in.pos,"Строка слишком велика");break;}size_t n=a.as.str.n+b.as.str.n;char*s=(char*)kir_oom_malloc(n?n:1);if(!s){fail(vm,in.pos,"Нехватка памяти строки");break;}memcpy(s,a.as.str.s,a.as.str.n);memcpy(s+a.as.str.n,b.as.str.s,b.as.str.n);Value x=v_str_copy(s,(int)n);free(s);push(vm,x);break;}
            if(in.op==BC_EQ||in.op==BC_NE){bool eq=eval_values_equal(a,b);push(vm,v_bool(in.op==BC_EQ?eq:!eq));break;}
            if(in.op==BC_BIT_AND||in.op==BC_BIT_OR||in.op==BC_BIT_XOR){if(!v_num_is_integer(a)||!v_num_is_integer(b))fail(vm,in.pos,"Побитовая операция требует два целых числа");else push(vm,v_num_bitwise(a,b,in.op==BC_BIT_AND?'&':in.op==BC_BIT_OR?'|':'^'));break;}
            if(in.op==BC_SHL||in.op==BC_SHR){int64_t bits=0;if(!v_num_is_integer(a)||!v_num_is_integer(b)||!v_num_fits_i64(b,&bits)||bits<0||(uint64_t)bits>(uint64_t)SIZE_MAX)fail(vm,in.pos,"Сдвиг требует целое число и неотрицательную целую величину сдвига в диапазоне size_t");else push(vm,v_num_shift(a,(size_t)bits,in.op==BC_SHL));break;}
            if(a.kind!=VAL_NUM||b.kind!=VAL_NUM){if(in.op==BC_FLOOR_DIV||in.op==BC_MOD)fail(vm,in.pos,"Операция деления требует два числа");else push(vm,v_nil());break;}
            if(in.op==BC_ADD)push(vm,v_num_add(a,b));else if(in.op==BC_SUB)push(vm,v_num_sub(a,b));else if(in.op==BC_MUL)push(vm,v_num_mul(a,b));else if(in.op==BC_DIV){if(v_num_to_double(b)==0)fail(vm,in.pos,"Деление на ноль");else push(vm,v_num(v_num_to_double(a)/v_num_to_double(b)));}else if(in.op==BC_FLOOR_DIV||in.op==BC_MOD){int cmp=1;if(v_num_compare(b,v_int(0),&cmp)&&cmp==0)fail(vm,in.pos,"Деление на ноль в операции деления/остатка");else push(vm,in.op==BC_FLOOR_DIV?v_num_floor_div(a,b):v_num_mod(a,b));}else if(in.op==BC_LT||in.op==BC_LE||in.op==BC_GT||in.op==BC_GE){int cmp=0;int comparable=v_num_compare(a,b,&cmp);push(vm,v_bool(comparable&&(in.op==BC_LT?cmp<0:in.op==BC_LE?cmp<=0:in.op==BC_GT?cmp>0:cmp>=0)));}else push(vm,v_nil());break;
        }
        if(!vm->failed&&!returned)gc_safepoint(vm);
    }
    *out=result;if(out_flow)*out_flow=flow;vm->sn=base;env_active_leave(vm,scope);vm->frames=frame.caller;return !vm->failed;
}
static int bytecode_run_internal(AstProgram *program,const char *entry,
                                 KirtonBytecodeStats *stats,int allow_ffi){
    if(stats)memset(stats,0,sizeof(*stats));if(!program)return 1;
    BVM vm={0};vm.heap=v_value_heap_new();vm.bigs=kir_bigint_heap_new();
    vm.ffi_enabled=allow_ffi!=0;
    KirValueHeap*old_heap=v_value_heap_set_current(vm.heap);KirBigIntHeap*old_bigs=v_bigint_heap_set_current(vm.bigs);
    if(vm.ffi_enabled){
        vm.ffi=kir_ffi_state_create();
        if(!vm.ffi){fail(&vm,(AstPos){program->source_file,0,0},"Недостаточно памяти для FFI Runtime");goto cleanup;}
    }
    vm.next_gc=256;env_init(&vm.global_env,NULL,1);
    if(!add_types(&vm,program->types,program->type_count,&vm.global_env)){fail(&vm,(AstPos){program->source_file,0,0},"Не удалось собрать таблицу типов");goto cleanup;}
    for(size_t i=0;i<program->func_count;i++){
        int id=add_function(&vm,program->funcs[i]);
        if(id<0){fail(&vm,program->funcs[i]->pos,"Не удалось собрать таблицу функций");goto cleanup;}
        vm.funcs[id]->module_scope=1;
    }
    for(size_t i=0;i<program->func_count;i++)if(!collect_nested_functions(&vm,program->funcs[i]->body,program->funcs[i]->body_len)){fail(&vm,program->funcs[i]->pos,"Не удалось собрать вложенные функции");goto cleanup;}
    double compile_start=now_ms(),execute_start=0.0;Chunk init={0};vm.init=&init;
    int compile_ok=1;
    for(size_t i=0;i<program->func_count;i++){
        int id=function_index(&vm,program->funcs[i]);int name=constant(&init,v_str_copy(program->funcs[i]->name_start,(int)program->funcs[i]->name_len));
        if(id<0||name<0||!emit2(&init,BC_BIND_FUNC,id,name,program->funcs[i]->pos)){compile_ok=0;break;}
    }
    if(compile_ok)compile_ok=compile_hoisted_functions(&vm,&init,program->init,program->init_len);
    if(compile_ok)compile_ok=compile_stmts(&vm,&init,program->init,program->init_len,0,NULL);
    if(!compile_ok&&!vm.failed)fail(&vm,(AstPos){program->source_file,0,0},"Нехватка памяти компилятора");
    for(size_t i=0;i<vm.fn&&!vm.failed;i++){
        BFunc*f=vm.funcs[i];
        if(!compile_hoisted_functions(&vm,&f->chunk,f->ast->body,f->ast->body_len)||
           !compile_stmts(&vm,&f->chunk,f->ast->body,f->ast->body_len,1,NULL)){
            if(!vm.failed)fail(&vm,f->ast->pos,"Нехватка памяти компилятора");break;
        }
        if(!emit(&f->chunk,BC_END,0,f->ast->pos))fail(&vm,f->ast->pos,"Нехватка памяти байткода");
    }
    if(!emit(&init,BC_END,0,(AstPos){program->source_file,0,0}))fail(&vm,(AstPos){program->source_file,0,0},"Нехватка памяти байткода");
    vm.compile_ms=now_ms()-compile_start;double root_compile_ms=vm.compile_ms;execute_start=now_ms();
    Value result=v_nil();BFlow init_flow=B_FLOW_NONE;if(!vm.failed)run_chunk(&vm,&init,&vm.global_env,&result,&init_flow,0,NULL,(AstPos){program->source_file,0,0},0);
    if(!vm.failed&&entry&&entry[0]){
        Value callable=v_nil();
        if(!env_get(&vm.global_env,entry,strlen(entry),&callable))fail(&vm,(AstPos){program->source_file,0,0},"Неизвестная функция входа");
        else if(callable.kind!=VAL_FUNCTION||!v_function_is_owned(vm.heap,callable))fail(&vm,(AstPos){program->source_file,0,0},"Значение входа не является функцией");
        else {CallDesc zero={0};zero.function_hint=-1;if(!push(&vm,callable))fail(&vm,(AstPos){program->source_file,0,0},"Нехватка памяти стека");else invoke_function(&vm,callable,vm.sn-1,&zero,&result,0,(AstPos){program->source_file,0,0});}
    }
    vm.execute_ms=now_ms()-execute_start;
    if(vm.compile_ms>root_compile_ms)vm.execute_ms-=vm.compile_ms-root_compile_ms;
    gc_collect(&vm);vm.live_end=heap_count(&vm);
    if(stats){stats->collections=vm.collections;stats->reclaimed_objects=vm.reclaimed;stats->peak_objects=vm.peak;stats->live_objects_at_end=vm.live_end;stats->compile_ms=vm.compile_ms;stats->execute_ms=vm.execute_ms;}
    for(size_t i=0;i<vm.fn;i++){free(vm.funcs[i]->name);chunk_free(&vm.funcs[i]->chunk);free(vm.funcs[i]);}
    free(vm.funcs);vm.funcs=NULL;chunk_free(&init);free(vm.stack);
    v_value_heap_set_current(old_heap);v_bigint_heap_set_current(old_bigs);
    /* Destroy managed Values while closure environments are still valid:
       function finalizers release their environment references. The VM owns
       this heap, so release_all would orphan ordinary managed allocations. */
    v_value_heap_destroy(vm.heap);env_free_all(&vm);env_release_storage(&vm.global_env);free(vm.types);
    kir_bigint_heap_destroy(vm.bigs);
    kir_ffi_state_destroy(vm.ffi);
    for(size_t i=0;i<vm.module_n;i++)if(vm.modules[i]){free(vm.modules[i]->key);free(vm.modules[i]->module_name);free(vm.modules[i]);}
    free(vm.modules);for(size_t i=0;i<vm.owned_program_n;i++)ast_program_destroy(vm.owned_programs[i]);free(vm.owned_programs);
    return vm.failed?1:0;
cleanup:
    for(size_t i=0;i<vm.fn;i++){free(vm.funcs[i]->name);chunk_free(&vm.funcs[i]->chunk);free(vm.funcs[i]);}
    free(vm.funcs);free(vm.stack);
    v_value_heap_set_current(old_heap);v_bigint_heap_set_current(old_bigs);
    v_value_heap_destroy(vm.heap);env_free_all(&vm);env_release_storage(&vm.global_env);free(vm.types);
    kir_bigint_heap_destroy(vm.bigs);
    kir_ffi_state_destroy(vm.ffi);
    for(size_t i=0;i<vm.module_n;i++)if(vm.modules[i]){free(vm.modules[i]->key);free(vm.modules[i]->module_name);free(vm.modules[i]);}
    free(vm.modules);for(size_t i=0;i<vm.owned_program_n;i++)ast_program_destroy(vm.owned_programs[i]);free(vm.owned_programs);
    return vm.failed?1:0;
}
int kirton_bytecode_run_with_stats(AstProgram *program,const char *entry,KirtonBytecodeStats *stats){
    return bytecode_run_internal(program,entry,stats,0);
}
int kirton_bytecode_run(AstProgram *program, const char *entry) {
    return bytecode_run_internal(program,entry,NULL,0);
}
int kirton_bytecode_run_with_ffi(AstProgram *program,const char *entry,int allow_ffi){
    return bytecode_run_internal(program,entry,NULL,allow_ffi);
}
