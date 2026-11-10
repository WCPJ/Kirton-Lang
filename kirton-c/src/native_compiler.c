/* SPDX-License-Identifier: MPL-2.0 */
#include "kirton/native_compiler.h"
#include "kirton/eval.h"
#include "kirton/parser.h"
#include "kirton/diag.h"
#include "eval_internal.h"
#include "kirton/value.h"
#include "kirton/common.h"
#include "native_stdlib_whitelist.h"
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef KIRTON_CLANG_PATH
#define KIRTON_CLANG_PATH ""
#endif
#ifndef KIRTON_RUNTIME_LIBRARY_PATH
#define KIRTON_RUNTIME_LIBRARY_PATH ""
#endif
#ifndef KIRTON_RUNTIME_LINK_FLAGS
#define KIRTON_RUNTIME_LINK_FLAGS ""
#endif
#ifndef KIRTON_NATIVE_POSIX_SANITIZER_FLAGS
#define KIRTON_NATIVE_POSIX_SANITIZER_FLAGS ""
#endif
#ifndef KIRTON_NATIVE_ASAN_LINK
#define KIRTON_NATIVE_ASAN_LINK 0
#endif
#ifndef KIRTON_NATIVE_ASAN_LINKER_PATH
#define KIRTON_NATIVE_ASAN_LINKER_PATH ""
#endif
#ifndef KIRTON_NATIVE_ASAN_MSVCLIB_DIR
#define KIRTON_NATIVE_ASAN_MSVCLIB_DIR ""
#endif
#ifndef KIRTON_NATIVE_ASAN_UCRT_DIR
#define KIRTON_NATIVE_ASAN_UCRT_DIR ""
#endif
#ifndef KIRTON_NATIVE_ASAN_UM_DIR
#define KIRTON_NATIVE_ASAN_UM_DIR ""
#endif
#ifndef KIRTON_NATIVE_ASAN_DLL_DIR
#define KIRTON_NATIVE_ASAN_DLL_DIR ""
#endif
#ifndef KIRTON_NATIVE_ASAN_LINK_LIBS
#define KIRTON_NATIVE_ASAN_LINK_LIBS ""
#endif

typedef struct {
    char* data;
    size_t len, cap;
} Text;

typedef struct {
    const char* name;
    size_t name_len;
    unsigned slot_id;
    int type;
    int is_global;
    int is_parameter;
    AstExpr* initializer;
    int boxed;             /* shared mutable cell, owned by a Value closure */
    int is_capture;        /* reads and writes an enclosing closure cell */
    unsigned capture_index;
    int hidden;             /* catch-only variable, invisible outside catch */
    const AstStmt* catch_owner;  /* exact lexical handler, not just its name */
    int is_ffi_module;      /* 1: opt-in foreign FFI; 2: memory Value API */
} Binding;

enum { NATIVE_I64, NATIVE_BOOL, NATIVE_STR, NATIVE_NUM, NATIVE_NIL, NATIVE_ANY, NATIVE_VOID, NATIVE_UNKNOWN };

static int native_type_is_numeric(int type){
    return type==NATIVE_I64 || type==NATIVE_NUM;
}

static int native_type_is_managed(int type){
    return type==NATIVE_STR || type==NATIVE_NUM || type==NATIVE_NIL || type==NATIVE_ANY;
}

typedef struct {
    unsigned id;
    int type;
} NativeValue;

typedef struct {
    const char* name;
    size_t length;
} NativeCapture;

typedef struct {
    AstFunc* ast;
    AstFunc* parent;
    NativeCapture* captures;
    size_t capture_count, capture_capacity;
    size_t id;
    int* parameter_types;
    int return_type;
    int saw_value_return;
    int saw_void_return;
    int is_nested;
} NativeFunction;

typedef struct {
    AstProgram* program;
    NativeFunction* functions;
    size_t function_count, function_capacity;
    Binding* globals;
    size_t global_count, global_capacity;
    Text global_definitions;
    Text string_constants;
    size_t string_count;
    int needs_runtime;
    int allow_ffi;
    char* error;
    size_t error_capacity;
    int changed;
} NativeModule;

typedef struct NativeForeachRoot {
    unsigned value_id;
    struct NativeForeachRoot* previous;
} NativeForeachRoot;

typedef struct NativeTryScope {
    const AstStmt* statement;
    unsigned previous_error_target_plus_one;
    unsigned loop_depth;
    struct NativeTryScope* previous;
} NativeTryScope;

typedef struct {
    AstProgram* program;
    NativeModule* module;
    NativeFunction* function;
    int is_main;
    Text text;
    Text allocas;
    Text initializers;
    Binding* bindings;
    size_t binding_count, binding_capacity;
    unsigned next_value, next_block, next_slot;
    unsigned next_root_array;
    int block_terminated;
    /* Innermost loop target for lexical break/continue, restored on exit. */
    int loop_kind; /* 0=none, 1=while, 2=foreach */
    unsigned loop_block;
    unsigned try_error_target_plus_one;
    unsigned loop_depth;
    NativeTryScope* active_try;
    NativeForeachRoot* active_foreach_roots;
    char* error;
    size_t error_capacity;
} Compiler;

static int text_appendf(Text* text, const char* format, ...){
    va_list ap;
    va_start(ap, format);
    va_list copy;
    va_copy(copy, ap);
    int needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (needed < 0){ va_end(ap); return 0; }
    size_t required = text->len + (size_t)needed + 1;
    if (required > text->cap){
        size_t capacity = text->cap ? text->cap : 1024;
        while (capacity < required){
            if (capacity > SIZE_MAX / 2){ va_end(ap); return 0; }
            capacity *= 2;
        }
        char* grown = (char*)realloc(text->data, capacity);
        if (!grown){ va_end(ap); return 0; }
        text->data = grown;
        text->cap = capacity;
    }
    vsnprintf(text->data + text->len, text->cap - text->len, format, ap);
    va_end(ap);
    text->len += (size_t)needed;
    return 1;
}

static int fail_at(Compiler* c, AstPos pos, const char* format, ...){
    if (c->error && c->error_capacity){
        int used = 0;
        if (pos.line > 0)
            used = snprintf(c->error, c->error_capacity,
                            "native compiler: line %d: ", pos.line);
        else
            used = snprintf(c->error, c->error_capacity, "native compiler: ");
        if (used < 0) used = 0;
        size_t offset = (size_t)used < c->error_capacity
                      ? (size_t)used : c->error_capacity - 1;
        va_list ap;
        va_start(ap, format);
        vsnprintf(c->error + offset, c->error_capacity - offset, format, ap);
        va_end(ap);
    }
    return 0;
}

static Binding* find_binding(Compiler* c, const char* name, size_t name_len){
    for (size_t i = c->binding_count; i > 0; --i){
        Binding* b = &c->bindings[i - 1];
        if (!b->hidden && b->name_len == name_len && memcmp(b->name, name, name_len) == 0){
            return b;
        }
    }
    return NULL;
}

static Binding* find_catch_binding(Compiler* c,const AstStmt* owner){
    for(size_t i=c->binding_count;i>0;--i){
        Binding* binding=&c->bindings[i-1];
        if(binding->catch_owner==owner) return binding;
    }
    return NULL;
}

static Binding* find_global(NativeModule* module, const char* name,
                            size_t name_len){
    for (size_t i = module->global_count; i > 0; --i){
        Binding* b = &module->globals[i - 1];
        if (b->name_len == name_len && memcmp(b->name, name, name_len) == 0)
            return b;
    }
    return NULL;
}

static NativeFunction* find_function(NativeModule* module, const char* name,
                                     size_t name_len){
    for (size_t i = module->function_count; i > 0; --i){
        NativeFunction* f = &module->functions[i - 1];
        if (!f->is_nested && f->ast->name_len == name_len &&
            memcmp(f->ast->name_start, name, name_len) == 0) return f;
    }
    return NULL;
}

static NativeFunction* find_nested_function(NativeModule* module,AstFunc* ast){
    for (size_t i=0;i<module->function_count;++i)
        if(module->functions[i].ast==ast && module->functions[i].is_nested)
            return &module->functions[i];
    return NULL;
}

static NativeFunction* find_function_by_ast(NativeModule* module,AstFunc* ast){
    for(size_t i=0;i<module->function_count;++i)
        if(module->functions[i].ast==ast)return &module->functions[i];
    return NULL;
}

static int function_child_captures(NativeModule* module,AstFunc* parent,
                                   const char* name,size_t length){
    if(!parent)return 0;
    for(size_t i=0;i<module->function_count;++i){
        NativeFunction* f=&module->functions[i];
        if(f->parent!=parent)continue;
        for(size_t j=0;j<f->capture_count;++j)
            if(f->captures[j].length==length &&
               !memcmp(f->captures[j].name,name,length))return 1;
    }
    return 0;
}

/* Only small, pure-Value collection builtins are lowered here. Never
   replace user-defined functions or arbitrary native-module calls. */
static int native_builtin_id(const char* name,size_t len){
    static const char* names[]={
        "список_пустой", "список_длина", "список_добавить",
        "словарь_пустой", "словарь_длина", "словарь_есть",
        "словарь_установить", "список_получить", "список_удалить",
        "список_вставить", "словарь_получить", "словарь_удалить",
        "сумма", "произведение"
    };
    for (size_t i=0;i<sizeof(names)/sizeof(names[0]);++i)
        if (strlen(names[i])==len && memcmp(names[i],name,len)==0)
            return (int)i+1;
    for (size_t i=0; i<KIRTON_NATIVE_STDLIB_COUNT; ++i){
        const char* builtin=kirton_native_stdlib_names[i];
        if (strlen(builtin)==len && !memcmp(builtin,name,len))
            return (int)(KIRTON_NATIVE_STDLIB_FIRST_ID+i);
    }
    return 0;
}

static AstUserType* find_user_type(NativeModule* module,AstPos origin,
                                    const char* name,size_t length,
                                    AstUserTypeKind kind,size_t* id){
    /* AST and Bytecode resolve named user types in the current module's
       environment, not in a process-wide namespace. The parser preserves
       the source path for both declarations and expressions; use that same
       scope to disambiguate equal names in independent imported modules. */
    for (size_t i=module->program->type_count;i>0;--i){
        AstUserType* type=module->program->types[i-1];
        if (origin.file && (!type->pos.file ||
            strcmp(origin.file,type->pos.file)!=0)) continue;
        if (type->kind==kind && type->name_len==length &&
            !memcmp(type->name,name,length)){
            if(id) *id=i-1;
            return type;
        }
    }
    return NULL;
}

static int add_binding(Compiler* c, const char* name, size_t name_len,
                       unsigned slot_id, int type){
    if (c->binding_count == c->binding_capacity){
        size_t capacity = c->binding_capacity ? c->binding_capacity * 2 : 8;
        if (capacity < c->binding_capacity || capacity > SIZE_MAX / sizeof(Binding))
            return 0;
        Binding* grown = (Binding*)realloc(c->bindings, capacity * sizeof(Binding));
        if (!grown) return 0;
        c->bindings = grown;
        c->binding_capacity = capacity;
    }
    Binding* binding=&c->bindings[c->binding_count++];
    *binding=(Binding){.name=name,.name_len=name_len,.slot_id=slot_id,.type=type};
    if(c->function && function_child_captures(c->module,c->function->ast,
                                               name,name_len)){
        binding->type=NATIVE_ANY;
        binding->boxed=1;
        c->module->needs_runtime=1;
    }
    return 1;
}

static Binding* add_global_binding(NativeModule* module, const char* name,
                                   size_t name_len, int type){
    Binding* existing = find_global(module, name, name_len);
    if (existing){
        if (native_type_is_numeric(existing->type) &&
            native_type_is_numeric(type) &&
            (existing->type==NATIVE_NUM || type==NATIVE_NUM)){
            if (existing->type!=NATIVE_NUM){
                existing->type=NATIVE_NUM;
                module->changed=1;
            }
            module->needs_runtime=1;
            return existing;
        }
        if (existing->type != NATIVE_UNKNOWN && type != NATIVE_UNKNOWN &&
            existing->type != type){
            if (existing->type != NATIVE_ANY){
                existing->type = NATIVE_ANY;
                module->needs_runtime = 1;
                module->changed = 1;
            }
            return existing;
        }
        if (existing->type == NATIVE_UNKNOWN && type != NATIVE_UNKNOWN){
            existing->type = type;
            module->changed = 1;
        }
        return existing;
    }
    if (module->global_count == module->global_capacity){
        size_t capacity = module->global_capacity ? module->global_capacity * 2 : 8;
        if (capacity < module->global_capacity ||
            capacity > SIZE_MAX / sizeof(Binding)) return NULL;
        Binding* grown = (Binding*)realloc(module->globals,
                                           capacity * sizeof(Binding));
        if (!grown) return NULL;
        module->globals = grown;
        module->global_capacity = capacity;
    }
    if (module->global_count > UINT_MAX) return NULL;
    unsigned slot = (unsigned)module->global_count;
    Binding* binding = &module->globals[module->global_count++];
    *binding = (Binding){.name=name,.name_len=name_len,.slot_id=slot,.type=type,.is_global=1};
    module->changed = 1;
    return binding;
}

static int emit_uninitialized_check(Compiler* c, Binding* binding,
                                    AstPos pos){
    unsigned check_id = c->next_value++;
    unsigned block_id = c->next_block++;
    if (!text_appendf(&c->text, binding->is_global ?
        "  %%set%u = load i1, ptr @ginit%u\n"
        "  br i1 %%set%u, label %%var_ok%u, label %%var_bad%u\n"
        "var_bad%u:\n"
        "  %%uninit_print%u = call i32 @puts(ptr @.uninitialized_message)\n"
        "%s"
        "  call void @exit(i32 71)\n"
        "  unreachable\n"
        "var_ok%u:\n" :
        "  %%set%u = load i1, ptr %%init%u\n"
        "  br i1 %%set%u, label %%var_ok%u, label %%var_bad%u\n"
        "var_bad%u:\n"
        "  %%uninit_print%u = call i32 @puts(ptr @.uninitialized_message)\n"
        "%s"
        "  call void @exit(i32 71)\n"
        "  unreachable\n"
        "var_ok%u:\n",
        check_id, binding->slot_id, check_id, block_id, block_id,
        block_id, check_id,
        c->module->needs_runtime ?
            "  call void @kirton_native_runtime_abort(ptr %rt, i32 71)\n" : "",
        block_id))
        return fail_at(c, pos, "out of memory while emitting LLVM IR");
    c->block_terminated = 0;
    return 1;
}

static int emit_string_constant(NativeModule* module,const char* bytes,
                                size_t length,size_t* out_id){
    if (module->string_count==SIZE_MAX || length==SIZE_MAX) return 0;
    size_t id=module->string_count++;
    if (!text_appendf(&module->string_constants,
        "@.kirton.str.%zu = private unnamed_addr constant [%zu x i8] c\"",
        id,length+1)) return 0;
    for (size_t i=0;i<length;++i){
        unsigned char byte=(unsigned char)bytes[i];
        if (!text_appendf(&module->string_constants,"\\%02X",(unsigned)byte))
            return 0;
    }
    if (!text_appendf(&module->string_constants,"\\00\", align 1\n")) return 0;
    *out_id=id;
    return 1;
}

static const char* llvm_value_type(int type);

static int emit_box_i64(Compiler* c,NativeValue* value,AstPos pos){
    if (value->type!=NATIVE_I64) return 1;
    if (!c->module->needs_runtime)
        return fail_at(c,pos,"dynamic integer value requires Native Runtime ABI");
    unsigned id=c->next_value++;
    if (!text_appendf(&c->text,
        "  %%v%u = call ptr @kirton_native_i64(ptr %%rt, i64 %%v%u)\n",
        id,value->id))
        return fail_at(c,pos,"out of memory while boxing Native integer");
    value->id=id;
    value->type=NATIVE_NUM;
    return 1;
}

static int emit_coerce_type(Compiler* c,NativeValue* value,int target,AstPos pos){
    if (value->type==target) return 1;
    if (target==NATIVE_NUM && value->type==NATIVE_I64)
        return emit_box_i64(c,value,pos);
    if (target==NATIVE_ANY){
        if (!c->module->needs_runtime)
            return fail_at(c,pos,"dynamic Value requires Native Runtime ABI");
        if (value->type==NATIVE_I64 || value->type==NATIVE_BOOL){
            unsigned id=c->next_value++;
            if (value->type==NATIVE_I64){
                if (!text_appendf(&c->text,
                    "  %%v%u = call ptr @kirton_native_i64(ptr %%rt, i64 %%v%u)\n",
                    id,value->id))
                    return fail_at(c,pos,"out of memory while boxing integer");
            } else {
                unsigned widened=c->next_value++;
                if (!text_appendf(&c->text,
                    "  %%v%u = zext i1 %%v%u to i64\n"
                    "  %%v%u = call ptr @kirton_native_bool(ptr %%rt, i64 %%v%u)\n",
                    widened,value->id,id,widened))
                    return fail_at(c,pos,"out of memory while boxing boolean");
            }
            value->id=id;
        } else if (!native_type_is_managed(value->type)){
            return fail_at(c,pos,"unsupported dynamic Value kind");
        }
        value->type=NATIVE_ANY;
        return 1;
    }
    return fail_at(c,pos,"Native value conversion is not supported");
}

static int emit_big_number_literal(Compiler* c,AstExpr* expr,NativeValue* out){
    size_t length=0;
    char* text=v_num_to_string(expr->as.num.value,&length);
    if (!text) return fail_at(c,expr->pos,"could not format numeric literal");
    size_t string_id=0;
    int emitted=emit_string_constant(c->module,text,length,&string_id);
    kir_free(text);
    if (!emitted)
        return fail_at(c,expr->pos,"out of memory while emitting numeric literal");
    unsigned id=c->next_value++;
    if (!text_appendf(&c->text,
        "  %%v%u = call ptr @kirton_native_number(ptr %%rt, ptr @.kirton.str.%zu, i64 %zu, i32 %d)\n",
        id,string_id,length,expr->as.num.value.is_integer ? 1 : 0))
        return fail_at(c,expr->pos,"out of memory while emitting managed number");
    *out=(NativeValue){id,NATIVE_NUM};
    return 1;
}

static int emit_truthy(Compiler* c, NativeValue value, AstPos pos,
                       unsigned* out_id);

static int native_ffi_method(Compiler* c,AstExpr* callee);
static int emit_expr(Compiler* c, AstExpr* expr, NativeValue* out);

static int emit_cell_handle(Compiler* c,Binding* binding,AstPos pos,
                            unsigned* out_cell){
    unsigned cell=c->next_value++;
    if(binding->is_capture){
        if(!text_appendf(&c->text,
            "  %%v%u = call ptr @kirton_native_capture_cell(ptr %%rt, ptr %%closure, i64 %u)\n",
            cell,binding->capture_index))
            return fail_at(c,pos,"could not get captured cell");
    } else if(binding->boxed){
        if(!text_appendf(&c->text,
            "  %%v%u = load ptr, ptr %%slot%u\n",cell,binding->slot_id))
            return fail_at(c,pos,"could not load captured local cell");
    } else return fail_at(c,pos,"expected captured variable cell");
    *out_cell=cell;
    return 1;
}

static int emit_cell_store(Compiler* c,Binding* binding,NativeValue value,
                           AstPos pos,int initialize){
    if(!initialize && !binding->is_capture &&
       !emit_uninitialized_check(c,binding,pos))return 0;
    unsigned cell;
    if(!emit_cell_handle(c,binding,pos,&cell))return 0;
    if(!text_appendf(&c->text,
        "  call void @kirton_native_cell_set(ptr %%rt, ptr %%v%u, ptr %%v%u)\n",
        cell,value.id))return fail_at(c,pos,"could not update captured variable");
    if(initialize && !binding->is_capture &&
       !text_appendf(&c->text,
        "  store i1 true, ptr %%init%u\n",binding->slot_id))return 0;
    return 1;
}


static int emit_indirect_function_call(Compiler* c,AstExpr* expr,NativeValue* out){
    /* Evaluate the callee once, before evaluating arguments, as in the AST VM.
       The result and arguments remain rooted in the calling frame. */
    NativeValue callee;
    if (!emit_expr(c,expr->as.call.callee,&callee) ||
        !emit_coerce_type(c,&callee,NATIVE_ANY,expr->pos)) return 0;
    size_t argc=expr->as.call.argc;
    unsigned array=c->next_value++,pointer=c->next_value++,result=c->next_value++;
    if(argc && !text_appendf(&c->allocas,
        "  %%call_args%u = alloca [%zu x ptr]\n",array,argc))
        return fail_at(c,expr->pos,"could not allocate dynamic arguments");
    for(size_t i=0;i<argc;++i){
        NativeValue arg;
        if (!emit_expr(c,expr->as.call.args[i],&arg) ||
            !emit_coerce_type(c,&arg,NATIVE_ANY,expr->as.call.args[i]->pos))return 0;
        unsigned index=c->next_value++;
        if (!text_appendf(&c->text,
            "  %%v%u = getelementptr inbounds [%zu x ptr], ptr %%call_args%u, i64 0, i64 %zu\n"
            "  store ptr %%v%u, ptr %%v%u\n",
            index,argc,array,i,arg.id,index))
            return fail_at(c,expr->pos,"could not lower dynamic argument");
    }
    if(argc && !text_appendf(&c->text,
        "  %%v%u = getelementptr inbounds [%zu x ptr], ptr %%call_args%u, i64 0, i64 0\n",
        pointer,argc,array))return fail_at(c,expr->pos,"could not lower call array");
    if(!(argc ? text_appendf(&c->text,
        "  %%v%u = call ptr @kirton_native_function_call(ptr %%rt, ptr %%v%u, ptr %%v%u, i64 %zu)\n",
        result,callee.id,pointer,argc) : text_appendf(&c->text,
        "  %%v%u = call ptr @kirton_native_function_call(ptr %%rt, ptr %%v%u, ptr null, i64 0)\n",
        result,callee.id)))
        return fail_at(c,expr->pos,"could not lower indirect call");
    *out=(NativeValue){result,NATIVE_ANY};
    return 1;
}

static int emit_expr(Compiler* c, AstExpr* expr, NativeValue* out){
    if (!expr) return fail_at(c, (AstPos){0}, "missing expression");
    if (expr->kind == AST_NUM){
        int64_t number;
        if (!v_num_is_integer(expr->as.num.value) ||
            !v_num_fits_i64(expr->as.num.value,&number))
            return emit_big_number_literal(c,expr,out);
        unsigned id = c->next_value++;
        if (!text_appendf(&c->text, "  %%v%u = add i64 0, %" PRId64 "\n", id, number))
            return fail_at(c, expr->pos, "out of memory while emitting LLVM IR");
        *out = (NativeValue){id, NATIVE_I64};
        return 1;
    }
    if (expr->kind == AST_NIL){
        unsigned id=c->next_value++;
        if (!text_appendf(&c->text,
            "  %%v%u = call ptr @kirton_native_nil(ptr %%rt)\n",id))
            return fail_at(c,expr->pos,"out of memory while emitting nil");
        *out=(NativeValue){id,NATIVE_NIL};
        return 1;
    }
    if (expr->kind == AST_STR){
        size_t string_id;
        if (!emit_string_constant(c->module,expr->as.str.start,
                                  expr->as.str.len,&string_id))
            return fail_at(c,expr->pos,"out of memory while emitting string literal");
        unsigned id=c->next_value++;
        if (!text_appendf(&c->text,
            "  %%v%u = call ptr @kirton_native_string(ptr %%rt, ptr @.kirton.str.%zu, i64 %zu)\n",
            id,string_id,expr->as.str.len))
            return fail_at(c,expr->pos,"out of memory while emitting string value");
        *out=(NativeValue){id,NATIVE_STR};
        return 1;
    }
    if (expr->kind == AST_BOOL){
        unsigned id = c->next_value++;
        if (!text_appendf(&c->text, "  %%v%u = or i1 false, %s\n",
                          id, expr->as.boolean.value ? "true" : "false"))
            return fail_at(c, expr->pos, "out of memory while emitting LLVM IR");
        *out = (NativeValue){id, NATIVE_BOOL};
        return 1;
    }
    if (expr->kind == AST_LIST || expr->kind == AST_DICT){
        int list=expr->kind==AST_LIST;
        unsigned id=c->next_value++;
        if (!text_appendf(&c->text,
            "  %%v%u = call ptr @%s(ptr %%rt)\n",id,
            list?"kirton_native_list_new":"kirton_native_dict_new"))
            return fail_at(c,expr->pos,"out of memory creating collection");
        size_t count=list?expr->as.list.len:expr->as.dict.len;
        for (size_t i=0;i<count;++i){
            AstExpr* child=list?expr->as.list.items[i]:expr->as.dict.vals[i];
            NativeValue item;
            if (!emit_expr(c,child,&item) ||
                !emit_coerce_type(c,&item,NATIVE_ANY,child->pos)) return 0;
            if (list){
                if (!text_appendf(&c->text,
                    "  call void @kirton_native_list_append(ptr %%rt, ptr %%v%u, ptr %%v%u)\n",
                    id,item.id)) return fail_at(c,expr->pos,"out of memory appending list item");
            } else {
                size_t key_id;
                if (!emit_string_constant(c->module,expr->as.dict.keys_s[i],
                                          expr->as.dict.keys_n[i],&key_id) ||
                    !text_appendf(&c->text,
                    "  call void @kirton_native_dict_put_literal(ptr %%rt, ptr %%v%u, ptr @.kirton.str.%zu, i64 %zu, ptr %%v%u)\n",
                    id,key_id,expr->as.dict.keys_n[i],item.id))
                    return fail_at(c,expr->pos,"out of memory inserting dictionary item");
            }
        }
        *out=(NativeValue){id,NATIVE_ANY};
        return 1;
    }
    if (expr->kind == AST_ATTR){
        AstExpr* obj=expr->as.attr.obj;
        if(obj->kind==AST_VAR &&
           !find_binding(c,obj->as.var.name_start,obj->as.var.name_len) &&
           (c->is_main || !find_global(c->module,obj->as.var.name_start,
                                                   obj->as.var.name_len))){
            size_t type_id=0;
            AstUserType* enum_type=find_user_type(c->module,expr->pos,
                obj->as.var.name_start,obj->as.var.name_len,AST_TYPE_ENUM,&type_id);
            if(enum_type){
                size_t key=0;
                if(!emit_string_constant(c->module,expr->as.attr.name,
                                         expr->as.attr.n,&key)) return 0;
                unsigned result=c->next_value++;
                if(!text_appendf(&c->text,
                    "  %%v%u = call ptr @kirton_native_enum_constant(ptr %%rt, i64 %zu, ptr @.kirton.str.%zu, i64 %zu)\n",
                    result,type_id,key,expr->as.attr.n))
                    return fail_at(c,expr->pos,"out of memory lowering enum");
                *out=(NativeValue){result,NATIVE_ANY};
                return 1;
            }
        }
        NativeValue base;
        size_t key_id;
        if (!emit_expr(c,expr->as.attr.obj,&base) ||
            !emit_coerce_type(c,&base,NATIVE_ANY,expr->pos) ||
            !emit_string_constant(c->module,expr->as.attr.name,
                                  expr->as.attr.n,&key_id)) return 0;
        unsigned result=c->next_value++;
        if (!text_appendf(&c->text,
            "  %%v%u = call ptr @kirton_native_attr_get(ptr %%rt, ptr %%v%u, ptr @.kirton.str.%zu, i64 %zu)\n",
            result,base.id,key_id,expr->as.attr.n))
            return fail_at(c,expr->pos,"out of memory lowering dictionary attribute");
        *out=(NativeValue){result,NATIVE_ANY};
        return 1;
    }
    if (expr->kind == AST_INDEX){
        NativeValue base,idx;
        if (!emit_expr(c,expr->as.index.base,&base) ||
            !emit_expr(c,expr->as.index.index,&idx) ||
            !emit_coerce_type(c,&base,NATIVE_ANY,expr->pos) ||
            !emit_coerce_type(c,&idx,NATIVE_ANY,expr->pos)) return 0;
        unsigned id=c->next_value++;
        if (!text_appendf(&c->text,
            "  %%v%u = call ptr @kirton_native_collection_index(ptr %%rt, ptr %%v%u, ptr %%v%u)\n",
            id,base.id,idx.id)) return fail_at(c,expr->pos,"out of memory indexing collection");
        *out=(NativeValue){id,NATIVE_ANY};
        return 1;
    }
    if (expr->kind == AST_VAR){
        Binding* binding = find_binding(c, expr->as.var.name_start,
                                        expr->as.var.name_len);
        if (!binding && !c->is_main)
            binding = find_global(c->module, expr->as.var.name_start,
                                  expr->as.var.name_len);
        if (!binding)
            return fail_at(c, expr->pos, "unknown variable '%.*s'",
                           (int)expr->as.var.name_len, expr->as.var.name_start);
        if(binding->boxed){
            if(!binding->is_capture && !emit_uninitialized_check(c,binding,expr->pos))
                return 0;
            unsigned cell,id;
            if(!emit_cell_handle(c,binding,expr->pos,&cell))return 0;
            id=c->next_value++;
            if(!text_appendf(&c->text,
                "  %%v%u = call ptr @kirton_native_cell_get(ptr %%rt, ptr %%v%u)\n",
                id,cell))return fail_at(c,expr->pos,"could not read captured variable");
            *out=(NativeValue){id,NATIVE_ANY};
            return 1;
        }
        if (!emit_uninitialized_check(c, binding, expr->pos)) return 0;
        unsigned id = c->next_value++;
        const char* llvm_type = llvm_value_type(binding->type);
        if (!text_appendf(&c->text, binding->is_global ?
            "  %%v%u = load %s, ptr @gslot%u\n" :
            "  %%v%u = load %s, ptr %%slot%u\n",
            id, llvm_type, binding->slot_id))
            return fail_at(c, expr->pos, "out of memory while emitting LLVM IR");
        if (binding->is_global && native_type_is_managed(binding->type)){
            unsigned held=c->next_value++;
            if (!text_appendf(&c->text,
                "  %%v%u = call ptr @kirton_native_hold_value(ptr %%rt, ptr %%v%u)\n",
                held,id))
                return fail_at(c,expr->pos,"out of memory while holding global Value");
            id=held;
        }
        *out = (NativeValue){id, binding->type};
        return 1;
    }
    if (expr->kind == AST_CALL){
        AstExpr* callee = expr->as.call.callee;
        if (!callee) return fail_at(c,expr->pos,"call without callee");
        int ffi_method=native_ffi_method(c,callee);
        if(ffi_method){
            if(ffi_method<0)return fail_at(c,expr->pos,"unknown foreign module method");
            AstExpr* imported_alias=callee->as.attr.obj;
            Binding* imported=find_binding(c,imported_alias->as.var.name_start,
                                           imported_alias->as.var.name_len);
            if(!imported)imported=find_global(c->module,
                imported_alias->as.var.name_start,imported_alias->as.var.name_len);
            if(!imported || !emit_uninitialized_check(c,imported,expr->pos))return 0;
            size_t argc=expr->as.call.argc;
            if((ffi_method==1 && argc!=1) || (ffi_method==2 && argc<2))
                return fail_at(c,expr->pos,"wrong foreign function argument count");
            unsigned array_id=c->next_value++,base_id=c->next_value++;
            if(!text_appendf(&c->allocas,
                "  %%ffi_args%u = alloca [%zu x ptr]\n",array_id,argc?argc:1))
                return fail_at(c,expr->pos,"cannot allocate FFI argument array");
            for(size_t i=0;i<argc;++i){
                NativeValue arg;
                if(!emit_expr(c,expr->as.call.args[i],&arg) ||
                   !emit_coerce_type(c,&arg,NATIVE_ANY,expr->pos))return 0;
                unsigned slot=c->next_value++;
                if(!text_appendf(&c->text,
                    "  %%v%u = getelementptr inbounds [%zu x ptr], ptr %%ffi_args%u, i64 0, i64 %zu\n"
                    "  store ptr %%v%u, ptr %%v%u\n",
                    slot,argc,array_id,i,arg.id,slot))return 0;
            }
            unsigned result=c->next_value++;
            if(ffi_method==3){
                size_t member_id;
                if(!emit_string_constant(c->module,callee->as.attr.name,
                    callee->as.attr.n,&member_id))return 0;
                if(!text_appendf(&c->text,
                    "  %%v%u = getelementptr inbounds [%zu x ptr], ptr %%ffi_args%u, i64 0, i64 0\n"
                    "  %%v%u = call ptr @kirton_native_memory_call(ptr %%rt, ptr @.kirton.str.%zu, i64 %zu, ptr %%v%u, i64 %zu)\n",
                    base_id,argc?argc:1,array_id,result,member_id,callee->as.attr.n,base_id,argc))return 0;
            }else if(!text_appendf(&c->text,
                "  %%v%u = getelementptr inbounds [%zu x ptr], ptr %%ffi_args%u, i64 0, i64 0\n"
                "  %%v%u = call ptr @kirton_native_ffi_invoke(ptr %%rt, i32 %d, ptr %%v%u, i64 %zu)\n",
                base_id,argc?argc:1,array_id,result,ffi_method,base_id,argc))return 0;
            *out=(NativeValue){result,NATIVE_ANY};return 1;
        }
        if (callee->kind != AST_VAR ||
            find_binding(c,callee->as.var.name_start,callee->as.var.name_len) ||
            (!c->is_main && find_global(c->module,
                callee->as.var.name_start,callee->as.var.name_len)))
            return emit_indirect_function_call(c,expr,out);
        const char* name = callee->as.var.name_start;
        size_t name_len = callee->as.var.name_len;
        if (find_binding(c, name, name_len) ||
            (!c->is_main && find_global(c->module, name, name_len)))
            return fail_at(c, expr->pos,
                           "calling a variable as a function is not supported");
        size_t type_id=0;
        AstUserType* constructor=find_user_type(c->module,expr->pos,name,name_len,
                                                 AST_TYPE_STRUCT,&type_id);
        if(constructor){
            size_t argc=expr->as.call.argc;
            unsigned names_id=c->next_value++,lengths_id=c->next_value++,
                     values_id=c->next_value++;
            if(argc && !text_appendf(&c->allocas,
                "  %%struct_names%u = alloca [%zu x ptr]\n"
                "  %%struct_lengths%u = alloca [%zu x i64]\n"
                "  %%struct_values%u = alloca [%zu x ptr]\n",
                names_id,argc,lengths_id,argc,values_id,argc))
                return fail_at(c,expr->pos,"out of memory allocating struct arguments");
            for(size_t i=0;i<argc;++i){
                const char* name_i=expr->as.call.arg_names?expr->as.call.arg_names[i]:NULL;
                size_t length_i=expr->as.call.arg_name_lens?expr->as.call.arg_name_lens[i]:0;
                size_t constant_id;
                if(!name_i || !emit_string_constant(c->module,name_i,length_i,&constant_id))
                    return fail_at(c,expr->pos,"invalid structure field name");
                NativeValue argument;
                if(!emit_expr(c,expr->as.call.args[i],&argument) ||
                   !emit_coerce_type(c,&argument,NATIVE_ANY,expr->pos)) return 0;
                unsigned n=c->next_value++,l=c->next_value++,v=c->next_value++;
                if(!text_appendf(&c->text,
                    "  %%v%u = getelementptr inbounds [%zu x ptr], ptr %%struct_names%u, i64 0, i64 %zu\n"
                    "  store ptr @.kirton.str.%zu, ptr %%v%u\n"
                    "  %%v%u = getelementptr inbounds [%zu x i64], ptr %%struct_lengths%u, i64 0, i64 %zu\n"
                    "  store i64 %zu, ptr %%v%u\n"
                    "  %%v%u = getelementptr inbounds [%zu x ptr], ptr %%struct_values%u, i64 0, i64 %zu\n"
                    "  store ptr %%v%u, ptr %%v%u\n",
                    n,argc,names_id,i,constant_id,n,
                    l,argc,lengths_id,i,length_i,l,
                    v,argc,values_id,i,argument.id,v))
                    return fail_at(c,expr->pos,"out of memory lowering struct fields");
            }
            unsigned first_name=c->next_value++,first_length=c->next_value++,
                first_value=c->next_value++,result=c->next_value++;
            if (argc){
                if(!text_appendf(&c->text,
                    "  %%v%u = getelementptr inbounds [%zu x ptr], ptr %%struct_names%u, i64 0, i64 0\n"
                    "  %%v%u = getelementptr inbounds [%zu x i64], ptr %%struct_lengths%u, i64 0, i64 0\n"
                    "  %%v%u = getelementptr inbounds [%zu x ptr], ptr %%struct_values%u, i64 0, i64 0\n"
                    "  %%v%u = call ptr @kirton_native_struct_construct(ptr %%rt, i64 %zu, ptr %%v%u, ptr %%v%u, ptr %%v%u, i64 %zu)\n",
                    first_name,argc,names_id,first_length,argc,lengths_id,
                    first_value,argc,values_id,result,type_id,
                    first_name,first_length,first_value,argc))
                    return fail_at(c,expr->pos,"out of memory lowering struct constructor");
            } else if (!text_appendf(&c->text,
                "  %%v%u = call ptr @kirton_native_struct_construct(ptr %%rt, i64 %zu, ptr null, ptr null, ptr null, i64 0)\n",
                result,type_id)) return fail_at(c,expr->pos,"out of memory lowering empty struct");
            *out=(NativeValue){result,NATIVE_ANY};
            return 1;
        }
        NativeFunction* function = native_builtin_id(name,name_len) ? NULL :
            find_function(c->module, name, name_len);
        if (!function){
            int builtin=native_builtin_id(name,name_len);
            if (!builtin) return fail_at(c,expr->pos,"unknown function '%.*s'",
                                          (int)name_len,name);
            size_t argc=expr->as.call.argc;
            unsigned array=c->next_value++,pointer=c->next_value++,result=c->next_value++;
            if (argc && !text_appendf(&c->allocas,
                "  %%builtin_args%u = alloca [%zu x ptr]\n",array,argc))
                return fail_at(c,expr->pos,"out of memory allocating builtin arguments");
            for (size_t i=0;i<argc;++i){
                NativeValue argument;
                if (!emit_expr(c,expr->as.call.args[i],&argument) ||
                    !emit_coerce_type(c,&argument,NATIVE_ANY,expr->as.call.args[i]->pos))
                    return 0;
                unsigned slot=c->next_value++;
                if (!text_appendf(&c->text,
                    "  %%v%u = getelementptr inbounds [%zu x ptr], ptr %%builtin_args%u, i64 0, i64 %zu\n"
                    "  store ptr %%v%u, ptr %%v%u\n",
                    slot,argc,array,i,argument.id,slot))
                    return fail_at(c,expr->pos,"out of memory lowering builtin arguments");
            }
            if (argc && !text_appendf(&c->text,
                "  %%v%u = getelementptr inbounds [%zu x ptr], ptr %%builtin_args%u, i64 0, i64 0\n",
                pointer,argc,array))
                return fail_at(c,expr->pos,"out of memory lowering builtin call");
            if (argc ? !text_appendf(&c->text,
                "  %%v%u = call ptr @kirton_native_collection_builtin(ptr %%rt, i32 %d, ptr %%v%u, i64 %zu)\n",
                result,builtin,pointer,argc) : !text_appendf(&c->text,
                "  %%v%u = call ptr @kirton_native_collection_builtin(ptr %%rt, i32 %d, ptr null, i64 0)\n",
                result,builtin))
                return fail_at(c,expr->pos,"out of memory lowering builtin call");
            *out=(NativeValue){result,NATIVE_ANY};
            return 1;
        }
        if (expr->as.call.argc != function->ast->param_count)
            return fail_at(c, expr->pos,
                "function '%.*s' expects %zu arguments, got %zu",
                (int)name_len, name, function->ast->param_count,
                expr->as.call.argc);
        Text args = {0};
        int ok = 1;
        if (c->module->needs_runtime)
            ok=text_appendf(&args,"ptr %%rt");
        for (size_t i = 0; i < expr->as.call.argc && ok; ++i){
            NativeValue argument;
            if (!emit_expr(c, expr->as.call.args[i], &argument)){
                free(args.data);
                return 0;
            }
            int expected = function->parameter_types[i];
            if (expected==NATIVE_UNKNOWN ||
                (argument.type!=expected &&
                 !emit_coerce_type(c,&argument,expected,expr->as.call.args[i]->pos))){
                free(args.data);
                return fail_at(c, expr->as.call.args[i]->pos,
                    "argument %zu of '%.*s' has unsupported type (expected %s)",
                    i + 1, (int)name_len, name,
                    expected == NATIVE_BOOL ? "bool" :
                    expected == NATIVE_STR ? "string" :
                    expected == NATIVE_NUM ? "number" : "signed i64");
            }
            if ((i || c->module->needs_runtime) && !text_appendf(&args, ", ")) ok = 0;
            if (ok) ok = text_appendf(&args, "%s %%v%u",
                llvm_value_type(expected), argument.id);
        }
        if (!ok){
            free(args.data);
            return fail_at(c, expr->pos, "out of memory while emitting call arguments");
        }
        int ret_type = function->return_type;
        if (ret_type == NATIVE_UNKNOWN){
            free(args.data);
            return fail_at(c, expr->pos,
                           "could not infer native return type of '%.*s'",
                           (int)name_len, name);
        }
        if (ret_type == NATIVE_VOID){
            ok = text_appendf(&c->text, "  call void @kirton_fn_%zu(%s)\n",
                              function->id, args.data ? args.data : "");
            out->id = 0;
            out->type = NATIVE_VOID;
        } else {
            unsigned id = c->next_value++;
            ok = text_appendf(&c->text,
                "  %%v%u = call %s @kirton_fn_%zu(%s)\n",
                id, llvm_value_type(ret_type),
                function->id, args.data ? args.data : "");
            *out = (NativeValue){id, ret_type};
        }
        free(args.data);
        if (!ok) return fail_at(c, expr->pos, "out of memory while emitting function call");
        return 1;
    }
    if (expr->kind == AST_UNARY && !strcmp(expr->as.unary.op,"!")){
        NativeValue operand;
        unsigned truth=0;
        if (!emit_expr(c,expr->as.unary.e,&operand) ||
            !emit_truthy(c,operand,expr->pos,&truth)) return 0;
        unsigned result=c->next_value++;
        if (!text_appendf(&c->text,
            "  %%v%u = xor i1 %%v%u, true\n",result,truth))
            return fail_at(c,expr->pos,"out of memory while emitting logical NOT");
        *out=(NativeValue){result,NATIVE_BOOL};
        return 1;
    }
    if (expr->kind == AST_UNARY &&
        (!strcmp(expr->as.unary.op,"-") || !strcmp(expr->as.unary.op,"~"))){
        NativeValue operand;
        if (!emit_expr(c, expr->as.unary.e, &operand)) return 0;
        if (!native_type_is_numeric(operand.type) && operand.type!=NATIVE_ANY){
            if (!strcmp(expr->as.unary.op,"-")){
                unsigned id=c->next_value++;
                if (!text_appendf(&c->text,
                    "  %%v%u = call ptr @kirton_native_nil(ptr %%rt)\n",id))
                    return fail_at(c,expr->pos,"out of memory emitting nil unary result");
                *out=(NativeValue){id,NATIVE_NIL};
                return 1;
            }
            return fail_at(c,expr->pos,"bitwise NOT requires a number");
        }
        if (!emit_box_i64(c,&operand,expr->pos)) return 0;
        unsigned id=c->next_value++;
        if (!text_appendf(&c->text,
            "  %%v%u = call ptr @%s(ptr %%rt, ptr %%v%u)\n",
            id,!strcmp(expr->as.unary.op,"~") ?
                "kirton_native_numeric_bitnot" :
                operand.type==NATIVE_ANY ? "kirton_native_dynamic_neg" :
                "kirton_native_numeric_neg",
            operand.id))
            return fail_at(c,expr->pos,"out of memory while emitting numeric unary op");
        *out=(NativeValue){id,NATIVE_NUM};
        return 1;
    }
    if (expr->kind == AST_BIN){
        const char* op=expr->as.bin.op;
        if (!strcmp(op,"&&") || !strcmp(op,"||")){
            int is_and=!strcmp(op,"&&");
            unsigned block=c->next_block++,slot=c->next_root_array++;
            if (!text_appendf(&c->allocas,
                "  %%logic_slot%u = alloca i1\n",slot))
                return fail_at(c,expr->pos,"out of memory while emitting logical operator");
            NativeValue left;
            unsigned truth=0;
            if (!emit_expr(c,expr->as.bin.a,&left) ||
                !emit_truthy(c,left,expr->as.bin.a->pos,&truth)) return 0;
            if (!text_appendf(&c->text,
                "  br i1 %%v%u, label %%%s%u, label %%%s%u\n"
                "logic_short%u:\n"
                "  store i1 %s, ptr %%logic_slot%u\n"
                "  br label %%logic_end%u\n"
                "logic_rhs%u:\n",
                truth,is_and?"logic_rhs":"logic_short",block,
                is_and?"logic_short":"logic_rhs",block,
                block,is_and?"false":"true",slot,block,block))
                return fail_at(c,expr->pos,"out of memory while emitting short circuit");
            NativeValue right;
            unsigned right_truth=0;
            if (!emit_expr(c,expr->as.bin.b,&right) ||
                !emit_truthy(c,right,expr->as.bin.b->pos,&right_truth)) return 0;
            unsigned result=c->next_value++;
            if (!text_appendf(&c->text,
                "  store i1 %%v%u, ptr %%logic_slot%u\n"
                "  br label %%logic_end%u\n"
                "logic_end%u:\n"
                "  %%v%u = load i1, ptr %%logic_slot%u\n",
                right_truth,slot,block,block,result,slot))
                return fail_at(c,expr->pos,"out of memory while emitting logical result");
            *out=(NativeValue){result,NATIVE_BOOL};
            return 1;
        }
        NativeValue left, right;
        if (!emit_expr(c, expr->as.bin.a, &left) ||
            !emit_expr(c, expr->as.bin.b, &right)) return 0;
        int numeric_op=0;
        if (!strcmp(op,"+")) numeric_op=KIR_NUM_ADD;
        else if (!strcmp(op,"-")) numeric_op=KIR_NUM_SUB;
        else if (!strcmp(op,"*")) numeric_op=KIR_NUM_MUL;
        else if (!strcmp(op,"/")) numeric_op=KIR_NUM_DIV;
        else if (!strcmp(op,"нацело")) numeric_op=KIR_NUM_FLOOR_DIV;
        else if (!strcmp(op,"%")) numeric_op=KIR_NUM_MOD;
        else if (!strcmp(op,"&")) numeric_op=KIR_NUM_BITAND;
        else if (!strcmp(op,"|")) numeric_op=KIR_NUM_BITOR;
        else if (!strcmp(op,"^")) numeric_op=KIR_NUM_BITXOR;
        else if (!strcmp(op,"<<")) numeric_op=KIR_NUM_SHL;
        else if (!strcmp(op,">>")) numeric_op=KIR_NUM_SHR;
        if (numeric_op==KIR_NUM_ADD && left.type==NATIVE_STR && right.type==NATIVE_STR){
            unsigned id=c->next_value++;
            if (!text_appendf(&c->text,
                "  %%v%u = call ptr @kirton_native_string_concat(ptr %%rt, ptr %%v%u, ptr %%v%u)\n",
                id,left.id,right.id))
                return fail_at(c,expr->pos,"out of memory while emitting string concatenation");
            *out=(NativeValue){id,NATIVE_STR};
            return 1;
        }
        if (numeric_op){
            int polymorphic_add=numeric_op==KIR_NUM_ADD &&
                (left.type==NATIVE_ANY || right.type==NATIVE_ANY);
            if ((!native_type_is_numeric(left.type) && left.type!=NATIVE_ANY &&
                 !(polymorphic_add && left.type==NATIVE_STR)) ||
                (!native_type_is_numeric(right.type) && right.type!=NATIVE_ANY &&
                 !(polymorphic_add && right.type==NATIVE_STR))){
                /* AST yields nil for mismatched primitive + - * / (both
                   operands have already been evaluated for side effects). */
                if (numeric_op==KIR_NUM_ADD || numeric_op==KIR_NUM_SUB ||
                    numeric_op==KIR_NUM_MUL || numeric_op==KIR_NUM_DIV){
                    unsigned id=c->next_value++;
                    if (!text_appendf(&c->text,
                        "  %%v%u = call ptr @kirton_native_nil(ptr %%rt)\n",id))
                        return fail_at(c,expr->pos,"out of memory emitting nil arithmetic result");
                    *out=(NativeValue){id,NATIVE_NIL};
                    return 1;
                }
                return fail_at(c,expr->pos,"arithmetic requires two numbers");
            }
            if (!emit_box_i64(c,&left,expr->pos) ||
                !emit_box_i64(c,&right,expr->pos)) return 0;
            unsigned id=c->next_value++;
            int dynamic_op=left.type==NATIVE_ANY || right.type==NATIVE_ANY;
            if (polymorphic_add){
                if (!text_appendf(&c->text,
                    "  %%v%u = call ptr @kirton_native_dynamic_add(ptr %%rt, ptr %%v%u, ptr %%v%u)\n",
                    id,left.id,right.id))
                    return fail_at(c,expr->pos,"out of memory while emitting dynamic addition");
            } else if (!text_appendf(&c->text,
                "  %%v%u = call ptr @%s(ptr %%rt, i32 %d, ptr %%v%u, ptr %%v%u)\n",
                id,dynamic_op?"kirton_native_dynamic_numeric":"kirton_native_numeric_binary",
                numeric_op,left.id,right.id))
                return fail_at(c,expr->pos,"out of memory while emitting BigInt arithmetic");
            *out=(NativeValue){id,polymorphic_add?NATIVE_ANY:NATIVE_NUM};
            return 1;
        }
        const char* predicate = NULL;
        if ((!strcmp(op,"<") || !strcmp(op,"<=") ||
             !strcmp(op,">") || !strcmp(op,">=")) &&
            ((!native_type_is_numeric(left.type) && left.type!=NATIVE_ANY) ||
             (!native_type_is_numeric(right.type) && right.type!=NATIVE_ANY))){
            unsigned id=c->next_value++;
            if (!text_appendf(&c->text,
                "  %%v%u = call ptr @kirton_native_nil(ptr %%rt)\n",id))
                return fail_at(c,expr->pos,"out of memory emitting nil comparison");
            *out=(NativeValue){id,NATIVE_NIL};
            return 1;
        }
        if (strcmp(expr->as.bin.op, "==") == 0) predicate = "eq";
        else if (strcmp(expr->as.bin.op, "!=") == 0) predicate = "ne";
        else if (strcmp(expr->as.bin.op, "<") == 0) predicate = "slt";
        else if (strcmp(expr->as.bin.op, "<=") == 0) predicate = "sle";
        else if (strcmp(expr->as.bin.op, ">") == 0) predicate = "sgt";
        else if (strcmp(expr->as.bin.op, ">=") == 0) predicate = "sge";
        else return fail_at(c, expr->pos, "operator '%s' is not supported", expr->as.bin.op);
        if ((!strcmp(op,"==") || !strcmp(op,"!=")) &&
            (left.type==NATIVE_ANY || right.type==NATIVE_ANY)){
            if (!emit_coerce_type(c,&left,NATIVE_ANY,expr->pos) ||
                !emit_coerce_type(c,&right,NATIVE_ANY,expr->pos)) return 0;
            unsigned compared=c->next_value++,result=c->next_value++;
            if (!text_appendf(&c->text,
                "  %%v%u = call i32 @kirton_native_equal(ptr %%rt, ptr %%v%u, ptr %%v%u)\n"
                "  %%v%u = icmp %s i32 %%v%u, 0\n",
                compared,left.id,right.id,result,!strcmp(op,"==")?"ne":"eq",compared))
                return fail_at(c,expr->pos,"out of memory while emitting dynamic equality");
            *out=(NativeValue){result,NATIVE_BOOL};
            return 1;
        }
        int compare_op=!strcmp(op,"==")?KIR_NUM_EQ:!strcmp(op,"!=")?KIR_NUM_NE:
            !strcmp(op,"<")?KIR_NUM_LT:!strcmp(op,"<=")?KIR_NUM_LE:
            !strcmp(op,">")?KIR_NUM_GT:KIR_NUM_GE;
        if (left.type==NATIVE_STR && right.type==NATIVE_STR &&
            (!strcmp(op,"==") || !strcmp(op,"!="))){
            unsigned compared=c->next_value++,result=c->next_value++;
            if (!text_appendf(&c->text,
                "  %%v%u = call i32 @kirton_native_string_equal(ptr %%rt, ptr %%v%u, ptr %%v%u)\n"
                "  %%v%u = icmp %s i32 %%v%u, 0\n",
                compared,left.id,right.id,result,!strcmp(op,"==")?"ne":"eq",compared))
                return fail_at(c,expr->pos,"out of memory while emitting string equality");
            *out=(NativeValue){result,NATIVE_BOOL};
            return 1;
        }
        if ((native_type_is_numeric(left.type) || left.type==NATIVE_ANY) &&
            (native_type_is_numeric(right.type) || right.type==NATIVE_ANY) &&
            (left.type==NATIVE_NUM || right.type==NATIVE_NUM ||
             left.type==NATIVE_ANY || right.type==NATIVE_ANY)){
            if (!emit_box_i64(c,&left,expr->pos) ||
                !emit_box_i64(c,&right,expr->pos)) return 0;
            if (left.type==NATIVE_ANY || right.type==NATIVE_ANY){
                unsigned result=c->next_value++;
                if (!text_appendf(&c->text,
                    "  %%v%u = call ptr @kirton_native_dynamic_compare(ptr %%rt, i32 %d, ptr %%v%u, ptr %%v%u)\n",
                    result,compare_op,left.id,right.id))
                    return fail_at(c,expr->pos,"out of memory while emitting dynamic comparison");
                *out=(NativeValue){result,NATIVE_ANY};
            } else {
                unsigned compared=c->next_value++,result=c->next_value++;
                if (!text_appendf(&c->text,
                    "  %%v%u = call i32 @kirton_native_numeric_compare(ptr %%rt, i32 %d, ptr %%v%u, ptr %%v%u)\n"
                    "  %%v%u = icmp ne i32 %%v%u, 0\n",
                    compared,compare_op,left.id,right.id,result,compared))
                    return fail_at(c,expr->pos,"out of memory while emitting BigInt comparison");
                *out=(NativeValue){result,NATIVE_BOOL};
            }
            return 1;
        }
        /* Kirton equality compares value kinds first. A number and a boolean,
           for example, are never equal; both operands must still be evaluated
           because either expression may have side effects. */
        if ((!strcmp(op,"==") || !strcmp(op,"!=")) &&
            left.type==NATIVE_NIL && right.type==NATIVE_NIL){
            unsigned result=c->next_value++;
            if (!text_appendf(&c->text,"  %%v%u = or i1 false, %s\n",
                              result,!strcmp(op,"==")?"true":"false"))
                return fail_at(c,expr->pos,"out of memory while emitting nil equality");
            *out=(NativeValue){result,NATIVE_BOOL};
            return 1;
        }
        if ((!strcmp(op,"==") || !strcmp(op,"!=")) &&
            left.type!=right.type &&
            !(native_type_is_numeric(left.type) &&
              native_type_is_numeric(right.type))){
            unsigned result=c->next_value++;
            if (!text_appendf(&c->text,"  %%v%u = or i1 false, %s\n",
                              result,!strcmp(op,"!=")?"true":"false"))
                return fail_at(c,expr->pos,"out of memory while emitting mixed-kind equality");
            *out=(NativeValue){result,NATIVE_BOOL};
            return 1;
        }
        if (left.type != right.type ||
            ((strcmp(predicate, "slt") == 0 || strcmp(predicate, "sle") == 0 ||
              strcmp(predicate, "sgt") == 0 || strcmp(predicate, "sge") == 0) &&
             left.type != NATIVE_I64))
            return fail_at(c, expr->pos,
                           "comparison requires matching integer operands (equality also accepts booleans)");
        unsigned op_id = c->next_value++;
        if (!text_appendf(&c->text,
            "  %%v%u = icmp %s %s %%v%u, %%v%u\n",
            op_id, predicate, left.type == NATIVE_BOOL ? "i1" : "i64",
            left.id, right.id))
            return fail_at(c, expr->pos, "out of memory while emitting LLVM IR");
        *out = (NativeValue){op_id, NATIVE_BOOL};
        return 1;
    }
    return fail_at(c, expr->pos, "expression kind %d is not supported", (int)expr->kind);
}

static int emit_truthy(Compiler* c, NativeValue value, AstPos pos,
                       unsigned* out_id){
    if (value.type == NATIVE_BOOL){ *out_id = value.id; return 1; }
    if (native_type_is_managed(value.type) && c->module->needs_runtime){
        unsigned truth=c->next_value++,result=c->next_value++;
        if (!text_appendf(&c->text,
            "  %%v%u = call i32 @kirton_native_truthy(ptr %%rt, ptr %%v%u)\n"
            "  %%v%u = icmp ne i32 %%v%u, 0\n",truth,value.id,result,truth))
            return fail_at(c,pos,"out of memory while emitting string truthiness");
        *out_id=result;
        return 1;
    }
    unsigned id = c->next_value++;
    if (!text_appendf(&c->text, "  %%v%u = icmp ne i64 %%v%u, 0\n",
                      id, value.id))
        return fail_at(c, pos, "out of memory while emitting LLVM IR");
    *out_id = id;
    return 1;
}

static int emit_statements(Compiler* c, AstStmt** statements, size_t count);
static int emit_store(Compiler* c, Binding* binding, NativeValue value, AstPos pos);
static int emit_gc_safepoint(Compiler* c,AstPos pos);
static int emit_try_status_guard(Compiler* c,AstPos pos);

static int emit_if(Compiler* c, AstStmt* stmt){
    NativeValue condition;
    unsigned condition_id;
    if (!emit_expr(c, stmt->as.if_.cond, &condition) ||
        !emit_truthy(c, condition, stmt->pos, &condition_id)) return 0;
    if (!emit_gc_safepoint(c,stmt->pos) ||
        !emit_try_status_guard(c,stmt->pos)) return 0;
    unsigned block = c->next_block++;
    if (!text_appendf(&c->text,
        "  br i1 %%v%u, label %%if_then%u, label %%if_else%u\n"
        "if_then%u:\n",
        condition_id, block, block, block))
        return fail_at(c, stmt->pos, "out of memory while emitting LLVM IR");
    c->block_terminated = 0;
    if (!emit_statements(c, stmt->as.if_.then_b, stmt->as.if_.then_len)) return 0;
    int then_terminated = c->block_terminated;
    if (!then_terminated &&
        !text_appendf(&c->text, "  br label %%if_end%u\n", block))
        return fail_at(c, stmt->pos, "out of memory while emitting LLVM IR");
    if (!text_appendf(&c->text, "if_else%u:\n", block))
        return fail_at(c, stmt->pos, "out of memory while emitting LLVM IR");
    c->block_terminated = 0;
    if (stmt->as.if_.has_else &&
        !emit_statements(c, stmt->as.if_.else_b, stmt->as.if_.else_len)) return 0;
    int else_terminated = c->block_terminated;
    if (!else_terminated &&
        !text_appendf(&c->text, "  br label %%if_end%u\n", block))
        return fail_at(c, stmt->pos, "out of memory while emitting LLVM IR");
    if (!text_appendf(&c->text, "if_end%u:\n", block))
        return fail_at(c, stmt->pos, "out of memory while emitting LLVM IR");
    if (then_terminated && else_terminated){
        if (!text_appendf(&c->text, "  unreachable\n"))
            return fail_at(c, stmt->pos, "out of memory while emitting LLVM IR");
        c->block_terminated = 1;
    } else {
        c->block_terminated = 0;
    }
    return 1;
}

static int emit_while(Compiler* c, AstStmt* stmt){
    unsigned block = c->next_block++;
    int parent_kind=c->loop_kind;
    unsigned parent_block=c->loop_block;
    c->loop_kind=1;
    c->loop_block=block;
    ++c->loop_depth;
    if (!text_appendf(&c->text,
        "  br label %%while_cond%u\n"
        "while_cond%u:\n", block, block))
        return fail_at(c, stmt->pos, "out of memory while emitting LLVM IR");
    c->block_terminated = 0;
    NativeValue condition;
    unsigned condition_id;
    if (!emit_expr(c, stmt->as.wh.cond, &condition) ||
        !emit_truthy(c, condition, stmt->pos, &condition_id)) return 0;
    if (!emit_gc_safepoint(c,stmt->pos) ||
        !emit_try_status_guard(c,stmt->pos)) return 0;
    if (!text_appendf(&c->text,
        "  br i1 %%v%u, label %%while_body%u, label %%while_end%u\n"
        "while_body%u:\n",
        condition_id, block, block, block))
        return fail_at(c, stmt->pos, "out of memory while emitting LLVM IR");
    c->block_terminated = 0;
    if (!emit_statements(c, stmt->as.wh.body, stmt->as.wh.body_len)) return 0;
    if (!c->block_terminated &&
        !text_appendf(&c->text, "  br label %%while_cond%u\n", block))
        return fail_at(c, stmt->pos, "out of memory while emitting LLVM IR");
    if (!text_appendf(&c->text, "while_end%u:\n", block))
        return fail_at(c, stmt->pos, "out of memory while emitting LLVM IR");
    c->loop_kind=parent_kind;
    c->loop_block=parent_block;
    --c->loop_depth;
    c->block_terminated = 0;
    return 1;
}

/* The iterator is the original, pinned Value. An index local is kept in LLVM
   storage, while the VM decides whether the next element exists. This follows
   AST semantics for mutated lists and UTF-8 codepoint iteration. */
static int emit_foreach(Compiler* c,AstStmt* stmt){
    NativeValue seq;
    if (!emit_expr(c,stmt->as.fe.seq,&seq) ||
        !emit_coerce_type(c,&seq,NATIVE_ANY,stmt->pos)) return 0;
    Binding* binding=find_binding(c,stmt->as.fe.it,stmt->as.fe.in);
    if (!binding) binding=find_global(c->module,stmt->as.fe.it,stmt->as.fe.in);
    if (!binding) return fail_at(c,stmt->pos,"missing foreach iterator binding");
    unsigned block=c->next_block++;
    unsigned item_id=c->next_value++;
    unsigned done_id=c->next_value++;
    int parent_kind=c->loop_kind;
    unsigned parent_block=c->loop_block;
    c->loop_kind=2;
    c->loop_block=block;
    ++c->loop_depth;
    NativeForeachRoot iterator_root={seq.id,c->active_foreach_roots};
    c->active_foreach_roots=&iterator_root;
    if (!text_appendf(&c->allocas,
          "  %%iter%u = alloca i64\n",block) ||
        !text_appendf(&c->text,
          "  store i64 0, ptr %%iter%u\n"
          "  br label %%foreach_next%u\n"
          "foreach_next%u:\n"
          "  %%v%u = call ptr @kirton_native_iter_next(ptr %%rt, ptr %%v%u, ptr %%iter%u)\n"
          "  %%v%u = icmp eq ptr %%v%u, null\n"
          "  br i1 %%v%u, label %%foreach_end%u, label %%foreach_body%u\n"
          "foreach_body%u:\n",
          block,block,block,item_id,seq.id,block,
          done_id,item_id,done_id,block,block,block))
        return fail_at(c,stmt->pos,"out of memory lowering foreach loop");
    c->block_terminated=0;
    NativeValue item={item_id,NATIVE_ANY};
    /* foreach assigns-or-declares on its first iteration; the ordinary SET
       path insists the destination was initialized beforehand. */
    if (binding->is_global && !c->is_main){
        unsigned promoted=c->next_value++;
        if (!text_appendf(&c->text,
            "  %%v%u = call ptr @kirton_native_root_value(ptr %%rt, ptr %%v%u)\n",
            promoted,item.id))
            return fail_at(c,stmt->pos,"out of memory rooting foreach global");
        item.id=promoted;
    }
    if (binding->boxed){
        if(!emit_cell_store(c,binding,item,stmt->pos,1))return 0;
    } else if (!text_appendf(&c->text,
        binding->is_global ?
            "  store ptr %%v%u, ptr @gslot%u\n"
            "  store i1 true, ptr @ginit%u\n" :
            "  store ptr %%v%u, ptr %%slot%u\n"
            "  store i1 true, ptr %%init%u\n",
        item.id,binding->slot_id,binding->slot_id))return 0;
    if(!emit_statements(c,stmt->as.fe.body,stmt->as.fe.body_len))return 0;
    if (!c->block_terminated &&
        !text_appendf(&c->text,"  br label %%foreach_step%u\n",block))
        return fail_at(c,stmt->pos,"out of memory lowering foreach backedge");
    /* Continue joins at this point too, so even an empty or always-continue
       loop reaches a safepoint and drops obsolete element pins each pass. */
    if (!text_appendf(&c->text,"foreach_step%u:\n",block) ||
        !emit_gc_safepoint(c,stmt->pos) ||
        !text_appendf(&c->text,
            "  br label %%foreach_next%u\nforeach_end%u:\n",block,block))
        return fail_at(c,stmt->pos,"out of memory lowering foreach exit");
    c->loop_kind=parent_kind;
    c->loop_block=parent_block;
    --c->loop_depth;
    c->active_foreach_roots=iterator_root.previous;
    c->block_terminated=0;
    return 1;
}

static int constrain_type(Compiler* c, int* current, int expected,
                          AstPos pos, const char* what){
    if (expected == NATIVE_UNKNOWN) return 1;
    if (*current == NATIVE_UNKNOWN){
        *current = expected;
        c->module->changed = 1;
        if (expected==NATIVE_ANY) c->module->needs_runtime=1;
        return 1;
    }
    if (*current==NATIVE_ANY) return 1;
    if (expected==NATIVE_ANY){
        *current=NATIVE_ANY;
        c->module->needs_runtime=1;
        c->module->changed=1;
        return 1;
    }
    if (native_type_is_numeric(*current) && native_type_is_numeric(expected)){
        if (*current==NATIVE_I64 && expected==NATIVE_NUM){
            *current=NATIVE_NUM;
            c->module->changed=1;
        }
        return 1;
    }
    if (*current != expected){
        *current=NATIVE_ANY;
        c->module->needs_runtime=1;
        c->module->changed=1;
    }
    (void)pos; (void)what;
    return 1;
}

static int constrain_local_type(Compiler* c,int* current,int expected,
                                AstPos pos,const char* what){
    if (expected==NATIVE_UNKNOWN) return 1;
    if (*current==NATIVE_UNKNOWN){ *current=expected; return 1; }
    if (*current==NATIVE_ANY) return 1;
    if (expected==NATIVE_ANY){
        *current=NATIVE_ANY;
        c->module->needs_runtime=1;
        return 1;
    }
    if (native_type_is_numeric(*current) && native_type_is_numeric(expected)){
        if (*current==NATIVE_I64 && expected==NATIVE_NUM) *current=NATIVE_NUM;
        return 1;
    }
    if (*current!=expected){
        *current=NATIVE_ANY;
        c->module->needs_runtime=1;
    }
    (void)pos; (void)what;
    return 1;
}

static int infer_expr_type(Compiler* c, AstExpr* expr, int expected,
                           int* out_type);

static int statements_declare_name(AstStmt** statements,size_t count,
                                   const char* name,size_t length){
    for(size_t i=0;i<count;++i){
        AstStmt* s=statements[i];
        if(!s)continue;
        if(s->kind==STMT_LET && s->as.let_.nn==length &&
            !memcmp(s->as.let_.ns,name,length))return 1;
        if(s->kind==STMT_FOREACH && s->as.fe.in==length &&
            !memcmp(s->as.fe.it,name,length))return 1;
        if(s->kind==STMT_FUNCTION && s->as.function->name_len==length &&
            !memcmp(s->as.function->name_start,name,length))return 1;
        if(s->kind==STMT_IF &&
           (statements_declare_name(s->as.if_.then_b,s->as.if_.then_len,name,length)||
            statements_declare_name(s->as.if_.else_b,s->as.if_.else_len,name,length)))return 1;
        if(s->kind==STMT_WHILE &&
           statements_declare_name(s->as.wh.body,s->as.wh.body_len,name,length))return 1;
        if(s->kind==STMT_FOREACH &&
           statements_declare_name(s->as.fe.body,s->as.fe.body_len,name,length))return 1;
    }
    return 0;
}

static int function_declares_name(AstFunc* function,
                                   const char* name,size_t length){
    if(!function)return 0;
    for(size_t i=0;i<function->param_count;++i)
        if(function->params_len[i]==length &&
           !memcmp(function->params_start[i],name,length))return 1;
    return statements_declare_name(function->body,function->body_len,name,length);
}

static int setup_captures(Compiler* c){
    if(!c->function)return 1;
    NativeFunction* f=c->function;
    for(size_t i=0;i<f->capture_count;++i){
        NativeCapture* capture=&f->captures[i];
        Binding* binding=find_binding(c,capture->name,capture->length);
        if(!binding){
            if(c->binding_count>UINT_MAX ||
               !add_binding(c,capture->name,capture->length,
                            (unsigned)c->binding_count,NATIVE_ANY))
                return fail_at(c,f->ast->pos,"failed to bind captured variable");
            binding=find_binding(c,capture->name,capture->length);
        }
        binding->is_capture=1;
        binding->capture_index=(unsigned)i;
        binding->boxed=1;
        binding->type=NATIVE_ANY;
    }
    return 1;
}

/* Forward captures through intermediate lexical functions. A closure may
   retain a binding declared by any enclosing function, not only its parent.
   Every intermediate function receives the same GC-managed mutable cell. */
static int ensure_function_capture(NativeModule* module,NativeFunction* f,
                                   const char* name,size_t length){
    if(!f || !f->is_nested || !f->parent)return 0;
    for(size_t i=0;i<f->capture_count;++i)
        if(f->captures[i].length==length &&
           !memcmp(f->captures[i].name,name,length))return 1;
    if(!function_declares_name(f->parent,name,length)){
        NativeFunction* parent=find_function_by_ast(module,f->parent);
        if(!parent || !ensure_function_capture(module,parent,name,length))return 0;
    }
    if(f->capture_count==f->capture_capacity){
        size_t capacity=f->capture_capacity?f->capture_capacity*2:4;
        if(capacity<f->capture_capacity || capacity>SIZE_MAX/sizeof(NativeCapture))return 0;
        NativeCapture* grown=(NativeCapture*)realloc(f->captures,capacity*sizeof(*grown));
        if(!grown)return 0;
        f->captures=grown;
        f->capture_capacity=capacity;
    }
    f->captures[f->capture_count++]=(NativeCapture){name,length};
    module->changed=1;
    NativeFunction* parent=find_function_by_ast(module,f->parent);
    if(parent)for(size_t i=0;i<parent->ast->param_count;++i)
        if(parent->ast->params_len[i]==length &&
           !memcmp(parent->ast->params_start[i],name,length))
            parent->parameter_types[i]=NATIVE_ANY;
    return 1;
}

static Binding* infer_capture(Compiler* c,const char* name,size_t length){
    if(!c->function || !ensure_function_capture(c->module,c->function,name,length))
        return NULL;
    if(!setup_captures(c))return NULL;
    return find_binding(c,name,length);
}

static Binding* infer_binding(Compiler* c, const char* name, size_t name_len){
    Binding* binding = find_binding(c, name, name_len);
    if (binding) return binding;
    if(c->function && c->function->is_nested){
        Binding* captured=infer_capture(c,name,name_len);
        if(captured)return captured;
    }
    if (c->is_main) return NULL;
    return find_global(c->module, name, name_len);
}

static int infer_constrain_expr(Compiler* c, AstExpr* expr, int expected){
    if (!expr) return fail_at(c, (AstPos){0}, "missing expression");
    if (expected==NATIVE_ANY) return 1;
    int actual = NATIVE_UNKNOWN;
    if (expr->kind == AST_VAR){
        Binding* binding = infer_binding(c, expr->as.var.name_start,
                                         expr->as.var.name_len);
        if (!binding)
            return fail_at(c, expr->pos, "unknown variable '%.*s'",
                (int)expr->as.var.name_len, expr->as.var.name_start);
        if (!(binding->is_global || binding->is_parameter ?
              constrain_type(c,&binding->type,expected,expr->pos,"variable") :
              constrain_local_type(c,&binding->type,expected,expr->pos,"variable"))) return 0;
        if (binding->is_global){
            Binding* global = find_global(c->module, expr->as.var.name_start,
                                          expr->as.var.name_len);
            if (global && !constrain_type(c, &global->type, expected,
                                          expr->pos, "global variable")) return 0;
        }
        if (binding->is_parameter && c->function &&
            binding->slot_id < c->function->ast->param_count &&
            !constrain_type(c,
                &c->function->parameter_types[binding->slot_id], expected,
                expr->pos, "function parameter")) return 0;
        if (binding->initializer &&
            !infer_constrain_expr(c, binding->initializer, expected)) return 0;
        return 1;
    }
    if (expr->kind == AST_CALL &&
        expr->as.call.callee && expr->as.call.callee->kind == AST_VAR){
        if (!infer_expr_type(c, expr, NATIVE_UNKNOWN, &actual)) return 0;
        NativeFunction* function = native_builtin_id(
            expr->as.call.callee->as.var.name_start,
            expr->as.call.callee->as.var.name_len) ? NULL : find_function(c->module,
            expr->as.call.callee->as.var.name_start,
            expr->as.call.callee->as.var.name_len);
        if (!function){
            if (infer_binding(c,expr->as.call.callee->as.var.name_start,
                              expr->as.call.callee->as.var.name_len))return 1;
            return native_builtin_id(expr->as.call.callee->as.var.name_start,
                                     expr->as.call.callee->as.var.name_len)!=0;
        }
        if (actual != NATIVE_UNKNOWN)
            return constrain_type(c,&function->return_type,expected,
                                 expr->pos,"function return");
        return constrain_type(c, &function->return_type, expected,
                              expr->pos, "function return");
    }
    if (expr->kind==AST_NUM && expected==NATIVE_NUM){
        c->module->needs_runtime=1;
        return 1;
    }
    if (!infer_expr_type(c, expr, NATIVE_UNKNOWN, &actual)) return 0;
    if (native_type_is_numeric(actual) && native_type_is_numeric(expected))
        return 1;
    if (actual != NATIVE_UNKNOWN && actual != expected)
        return fail_at(c, expr->pos, "expression has the wrong native type");
    return 1;
}

static int infer_numeric_pair(Compiler* c,AstExpr* left_expr,AstExpr* right_expr,
                              AstPos pos,int* left,int* right){
    if (!infer_expr_type(c,left_expr,NATIVE_UNKNOWN,left) ||
        !infer_expr_type(c,right_expr,NATIVE_UNKNOWN,right)) return 0;
    if (*left==NATIVE_UNKNOWN && *right!=NATIVE_UNKNOWN &&
        !infer_constrain_expr(c,left_expr,*right)) return 0;
    if (*right==NATIVE_UNKNOWN && *left!=NATIVE_UNKNOWN &&
        !infer_constrain_expr(c,right_expr,*left)) return 0;
    if (*left==NATIVE_UNKNOWN && *right==NATIVE_UNKNOWN){
        if (!infer_constrain_expr(c,left_expr,NATIVE_I64) ||
            !infer_constrain_expr(c,right_expr,NATIVE_I64)) return 0;
    }
    if (!infer_expr_type(c,left_expr,NATIVE_UNKNOWN,left) ||
        !infer_expr_type(c,right_expr,NATIVE_UNKNOWN,right)) return 0;
    if ((!native_type_is_numeric(*left) && *left!=NATIVE_ANY) ||
        (!native_type_is_numeric(*right) && *right!=NATIVE_ANY))
        return fail_at(c,pos,"numeric operation requires two numbers");
    return 1;
}

static int infer_expr_type(Compiler* c, AstExpr* expr, int expected,
                           int* out_type){
    if (!expr) return fail_at(c, (AstPos){0}, "missing expression");
    int type = NATIVE_UNKNOWN;
    if (expr->kind == AST_NUM){
        int64_t number;
        if (v_num_is_integer(expr->as.num.value) &&
            v_num_fits_i64(expr->as.num.value,&number)) type=NATIVE_I64;
        else { c->module->needs_runtime=1; type=NATIVE_NUM; }
    } else if (expr->kind == AST_NIL){
        type = NATIVE_NIL;
        c->module->needs_runtime=1;
    } else if (expr->kind == AST_STR){
        c->module->needs_runtime=1;
        type = NATIVE_STR;
    } else if (expr->kind == AST_BOOL){
        type = NATIVE_BOOL;
    } else if (expr->kind == AST_LIST || expr->kind == AST_DICT){
        c->module->needs_runtime=1;
        size_t count=expr->kind==AST_LIST?expr->as.list.len:expr->as.dict.len;
        for (size_t i=0;i<count;++i){
            AstExpr* child=expr->kind==AST_LIST?expr->as.list.items[i]:expr->as.dict.vals[i];
            int ignored;
            if (!infer_expr_type(c,child,NATIVE_UNKNOWN,&ignored)) return 0;
            if (ignored==NATIVE_VOID) return fail_at(c,child->pos,"collection element needs a value");
        }
        type=NATIVE_ANY;
    } else if (expr->kind == AST_ATTR){
        AstExpr* obj=expr->as.attr.obj;
        if (obj->kind==AST_VAR &&
            !infer_binding(c,obj->as.var.name_start,obj->as.var.name_len)){
            AstUserType* enum_type=find_user_type(c->module,expr->pos,
                obj->as.var.name_start,obj->as.var.name_len,AST_TYPE_ENUM,NULL);
            if(enum_type){
                int found=0;
                for(size_t i=0;i<enum_type->variant_count;++i)
                    if(enum_type->variant_lens[i]==expr->as.attr.n &&
                       !memcmp(enum_type->variants[i],expr->as.attr.name,expr->as.attr.n))
                        found=1;
                if(!found) return fail_at(c,expr->pos,"unknown enum variant");
                c->module->needs_runtime=1;
                type=NATIVE_ANY;
                goto inference_done;
            }
        }
        int base=NATIVE_UNKNOWN;
        if (!infer_expr_type(c,expr->as.attr.obj,NATIVE_UNKNOWN,&base)) return 0;
        if (base==NATIVE_VOID) return fail_at(c,expr->pos,"attribute needs an object value");
        c->module->needs_runtime=1;
        type=NATIVE_ANY;
    } else if (expr->kind == AST_INDEX){
        int base=NATIVE_UNKNOWN,index=NATIVE_UNKNOWN;
        if (!infer_expr_type(c,expr->as.index.base,NATIVE_UNKNOWN,&base) ||
            !infer_expr_type(c,expr->as.index.index,NATIVE_UNKNOWN,&index)) return 0;
        if (base==NATIVE_VOID || index==NATIVE_VOID)
            return fail_at(c,expr->pos,"indexing requires value operands");
        c->module->needs_runtime=1;
        type=NATIVE_ANY;
    } else if (expr->kind == AST_VAR){
        Binding* binding = infer_binding(c, expr->as.var.name_start,
                                         expr->as.var.name_len);
        if (!binding)
            return fail_at(c, expr->pos, "unknown variable '%.*s'",
                           (int)expr->as.var.name_len, expr->as.var.name_start);
        type = binding->type;
    } else if (expr->kind == AST_CALL){
        AstExpr* callee = expr->as.call.callee;
        if (!callee) return fail_at(c,expr->pos,"call without callee");
        int ffi_method=native_ffi_method(c,callee);
        if(ffi_method){
            if(ffi_method<0)return fail_at(c,expr->pos,"unknown foreign module method");
            size_t argc=expr->as.call.argc;
            if((ffi_method==1 && argc!=1) || (ffi_method==2 && argc<2))
                return fail_at(c,expr->pos,"wrong foreign function argument count");
            for(size_t i=0;i<argc;++i){
                int arg_type=NATIVE_UNKNOWN;
                if(!infer_expr_type(c,expr->as.call.args[i],NATIVE_UNKNOWN,&arg_type))return 0;
                if(arg_type==NATIVE_VOID)return fail_at(c,expr->pos,"foreign call requires value arguments");
            }
            c->module->needs_runtime=1;
            type=NATIVE_ANY;
            goto inference_done;
        }
        if (callee->kind!=AST_VAR || infer_binding(c,callee->as.var.name_start,
                                                    callee->as.var.name_len)){
            int callee_type=NATIVE_UNKNOWN;
            if (!infer_expr_type(c,callee,NATIVE_UNKNOWN,&callee_type))return 0;
            if (callee->kind==AST_VAR && callee_type==NATIVE_UNKNOWN){
                Binding* binding=infer_binding(c,callee->as.var.name_start,
                                               callee->as.var.name_len);
                if(!constrain_type(c,&binding->type,NATIVE_ANY,callee->pos,
                                   "indirect function"))return 0;
                if(binding->is_parameter && c->function &&
                   binding->slot_id<c->function->ast->param_count)
                    if(!constrain_type(c,
                        &c->function->parameter_types[binding->slot_id],NATIVE_ANY,
                        callee->pos,"indirect function parameter"))return 0;
            }
            for(size_t i=0;i<expr->as.call.argc;++i){
                int arg=NATIVE_UNKNOWN;
                if(!infer_expr_type(c,expr->as.call.args[i],NATIVE_UNKNOWN,&arg))return 0;
                if(arg==NATIVE_VOID)return fail_at(c,expr->pos,"function argument has no value");
            }
            c->module->needs_runtime=1;
            type=NATIVE_ANY;
            goto inference_done;
        }
        AstUserType* constructor=find_user_type(c->module,expr->pos,
            callee->as.var.name_start,callee->as.var.name_len,AST_TYPE_STRUCT,NULL);
        if (constructor){
            if (expr->as.call.argc!=constructor->field_count)
                return fail_at(c,expr->pos,"wrong number of structure fields");
            for (size_t i=0;i<expr->as.call.argc;++i){
                const char* name=expr->as.call.arg_names?
                    expr->as.call.arg_names[i]:NULL;
                size_t length=expr->as.call.arg_name_lens?
                    expr->as.call.arg_name_lens[i]:0;
                if (!name) return fail_at(c,expr->pos,"structure requires named fields");
                int found=0;
                for(size_t j=0;j<constructor->field_count;++j)
                    if(constructor->fields[j].name_len==length &&
                        !memcmp(constructor->fields[j].name,name,length)) found=1;
                if(!found) return fail_at(c,expr->pos,"unknown structure field");
                int value_type;
                if(!infer_expr_type(c,expr->as.call.args[i],NATIVE_UNKNOWN,&value_type)) return 0;
                if(value_type==NATIVE_VOID) return fail_at(c,expr->pos,"structure field requires a value");
            }
            c->module->needs_runtime=1;
            type=NATIVE_ANY;
            goto inference_done;
        }
        NativeFunction* function = native_builtin_id(
            callee->as.var.name_start,callee->as.var.name_len) ? NULL :
            find_function(c->module,callee->as.var.name_start,callee->as.var.name_len);
        if (!function){
            int builtin=native_builtin_id(callee->as.var.name_start,
                                          callee->as.var.name_len);
            if (!builtin) return fail_at(c,expr->pos,"unknown function '%.*s'",
                (int)callee->as.var.name_len,callee->as.var.name_start);
            for (size_t i=0;i<expr->as.call.argc;++i){
                int argument_type=NATIVE_UNKNOWN;
                if (!infer_expr_type(c,expr->as.call.args[i],NATIVE_UNKNOWN,
                                     &argument_type)) return 0;
                if (argument_type==NATIVE_VOID)
                    return fail_at(c,expr->pos,"builtin arguments require values");
            }
            c->module->needs_runtime=1;
            type=NATIVE_ANY;
            goto inference_done;
        }
        if (expr->as.call.argc != function->ast->param_count)
            return fail_at(c, expr->pos,
                "function '%.*s' expects %zu arguments, got %zu",
                (int)callee->as.var.name_len, callee->as.var.name_start,
                function->ast->param_count, expr->as.call.argc);
        for (size_t i = 0; i < expr->as.call.argc; ++i){
            int argument_type = NATIVE_UNKNOWN;
            int param_type = function->parameter_types[i];
            if (!infer_expr_type(c, expr->as.call.args[i], NATIVE_UNKNOWN,
                                 &argument_type)) return 0;
            if (argument_type != NATIVE_UNKNOWN && param_type == NATIVE_UNKNOWN){
                if (!constrain_type(c, &function->parameter_types[i],
                                    argument_type, expr->as.call.args[i]->pos,
                                    "function parameter")) return 0;
                param_type = function->parameter_types[i];
            }
            if (param_type != NATIVE_UNKNOWN && argument_type == NATIVE_UNKNOWN &&
                param_type!=NATIVE_ANY &&
                !infer_constrain_expr(c, expr->as.call.args[i], param_type)) return 0;
            if (param_type != NATIVE_UNKNOWN && argument_type != NATIVE_UNKNOWN &&
                param_type != argument_type){
                if (native_type_is_numeric(param_type) &&
                    native_type_is_numeric(argument_type)){
                    if (!constrain_type(c,&function->parameter_types[i],
                        (param_type==NATIVE_NUM || argument_type==NATIVE_NUM) ?
                            NATIVE_NUM : NATIVE_I64,
                        expr->as.call.args[i]->pos,"function parameter")) return 0;
                } else if (!constrain_type(c,&function->parameter_types[i],
                                      NATIVE_ANY,expr->as.call.args[i]->pos,
                                      "function parameter")) return 0;
            }
        }
        type = function->return_type;
    } else if (expr->kind == AST_UNARY && !strcmp(expr->as.unary.op,"!")){
        int operand=NATIVE_UNKNOWN;
        if (!infer_expr_type(c,expr->as.unary.e,NATIVE_UNKNOWN,&operand)) return 0;
        if (operand==NATIVE_VOID)
            return fail_at(c,expr->pos,"logical NOT requires a value");
        type=NATIVE_BOOL;
    } else if (expr->kind == AST_UNARY &&
               (!strcmp(expr->as.unary.op,"-") ||
                !strcmp(expr->as.unary.op,"~"))){
        int operand=NATIVE_UNKNOWN;
        if (!infer_expr_type(c,expr->as.unary.e,NATIVE_UNKNOWN,&operand)) return 0;
        if (operand==NATIVE_UNKNOWN &&
            !infer_constrain_expr(c,expr->as.unary.e,NATIVE_I64)) return 0;
        if (!infer_expr_type(c,expr->as.unary.e,NATIVE_UNKNOWN,&operand)) return 0;
        c->module->needs_runtime=1;
        if (!native_type_is_numeric(operand) && operand!=NATIVE_ANY){
            if (!strcmp(expr->as.unary.op,"-")) type=NATIVE_NIL;
            else return fail_at(c,expr->pos,"bitwise NOT requires a number");
        } else type=NATIVE_NUM;
    } else if (expr->kind == AST_BIN){
        const char* op = expr->as.bin.op;
        if (!strcmp(op,"&&") || !strcmp(op,"||")){
            int left=NATIVE_UNKNOWN,right=NATIVE_UNKNOWN;
            if (!infer_expr_type(c,expr->as.bin.a,NATIVE_UNKNOWN,&left) ||
                !infer_expr_type(c,expr->as.bin.b,NATIVE_UNKNOWN,&right)) return 0;
            if (left==NATIVE_VOID || right==NATIVE_VOID)
                return fail_at(c,expr->pos,"logical expression needs two values");
            type=NATIVE_BOOL;
        } else if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0 ||
            strcmp(op, "*") == 0 || strcmp(op,"/")==0 ||
            strcmp(op,"нацело")==0 || strcmp(op,"%")==0 ||
            strcmp(op,"&")==0 || strcmp(op,"|")==0 ||
            strcmp(op,"^")==0 || strcmp(op,"<<")==0 ||
            strcmp(op,">>")==0){
            int left=NATIVE_UNKNOWN,right=NATIVE_UNKNOWN;
            if (!strcmp(op,"+")){
                if (!infer_expr_type(c,expr->as.bin.a,NATIVE_UNKNOWN,&left) ||
                    !infer_expr_type(c,expr->as.bin.b,NATIVE_UNKNOWN,&right)) return 0;
                if (left==NATIVE_ANY || right==NATIVE_ANY){
                    c->module->needs_runtime=1;
                    type=NATIVE_ANY;
                    goto inference_done;
                }
                if (left==NATIVE_STR || right==NATIVE_STR){
                    if (left==NATIVE_UNKNOWN &&
                        !infer_constrain_expr(c,expr->as.bin.a,NATIVE_STR)) return 0;
                    if (right==NATIVE_UNKNOWN &&
                        !infer_constrain_expr(c,expr->as.bin.b,NATIVE_STR)) return 0;
                    if (!infer_expr_type(c,expr->as.bin.a,NATIVE_UNKNOWN,&left) ||
                        !infer_expr_type(c,expr->as.bin.b,NATIVE_UNKNOWN,&right)) return 0;
                    if (left!=NATIVE_STR || right!=NATIVE_STR){
                        /* AST returns nil for dissimilar known '+' operands. */
                        c->module->needs_runtime=1;
                        type=NATIVE_NIL;
                        goto inference_done;
                    }
                    c->module->needs_runtime=1;
                    type=NATIVE_STR;
                    goto inference_done;
                }
            }
            if (strcmp(op,"нацело") && strcmp(op,"%") &&
                strcmp(op,"&") && strcmp(op,"|") && strcmp(op,"^") &&
                strcmp(op,"<<") && strcmp(op,">>")){
                if (!infer_expr_type(c,expr->as.bin.a,NATIVE_UNKNOWN,&left) ||
                    !infer_expr_type(c,expr->as.bin.b,NATIVE_UNKNOWN,&right)) return 0;
                if ((left!=NATIVE_UNKNOWN && left!=NATIVE_ANY &&
                     !native_type_is_numeric(left)) ||
                    (right!=NATIVE_UNKNOWN && right!=NATIVE_ANY &&
                     !native_type_is_numeric(right))){
                    c->module->needs_runtime=1;
                    type=NATIVE_NIL;
                    goto inference_done;
                }
            }
            if (!infer_numeric_pair(c,expr->as.bin.a,expr->as.bin.b,
                                    expr->pos,&left,&right)) return 0;
            c->module->needs_runtime=1;
            type=NATIVE_NUM;
        } else if (strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
                   strcmp(op, ">") == 0 || strcmp(op, ">=") == 0){
            int left=NATIVE_UNKNOWN,right=NATIVE_UNKNOWN;
            if (!infer_expr_type(c,expr->as.bin.a,NATIVE_UNKNOWN,&left) ||
                !infer_expr_type(c,expr->as.bin.b,NATIVE_UNKNOWN,&right)) return 0;
            if ((left!=NATIVE_UNKNOWN && left!=NATIVE_ANY &&
                 !native_type_is_numeric(left)) ||
                (right!=NATIVE_UNKNOWN && right!=NATIVE_ANY &&
                 !native_type_is_numeric(right))){
                c->module->needs_runtime=1;
                type=NATIVE_NIL;
                goto inference_done;
            }
            if (!infer_numeric_pair(c,expr->as.bin.a,expr->as.bin.b,
                                    expr->pos,&left,&right)) return 0;
            if (left==NATIVE_NUM || right==NATIVE_NUM ||
                left==NATIVE_ANY || right==NATIVE_ANY)
                c->module->needs_runtime=1;
            type = (left==NATIVE_ANY || right==NATIVE_ANY) ? NATIVE_ANY : NATIVE_BOOL;
        } else if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0){
            int left = NATIVE_UNKNOWN, right = NATIVE_UNKNOWN;
            if (!infer_expr_type(c, expr->as.bin.a, NATIVE_UNKNOWN, &left) ||
                !infer_expr_type(c, expr->as.bin.b, NATIVE_UNKNOWN, &right)) return 0;
            if (left==NATIVE_UNKNOWN && right!=NATIVE_UNKNOWN &&
                !native_type_is_numeric(right) &&
                !infer_constrain_expr(c,expr->as.bin.a,right)) return 0;
            if (right==NATIVE_UNKNOWN && left!=NATIVE_UNKNOWN &&
                !native_type_is_numeric(left) &&
                !infer_constrain_expr(c,expr->as.bin.b,left)) return 0;
            if (!infer_expr_type(c,expr->as.bin.a,NATIVE_UNKNOWN,&left) ||
                !infer_expr_type(c,expr->as.bin.b,NATIVE_UNKNOWN,&right)) return 0;
            /* Known mismatched value kinds are legal for == and !=. Their
               result is determined by the value kinds, not an inferred numeric
               conversion. Preserve numeric unification for unresolved calls. */
            if (left==NATIVE_VOID || right==NATIVE_VOID)
                return fail_at(c,expr->pos,"equality requires two values");
            if (left==NATIVE_UNKNOWN && right==NATIVE_UNKNOWN){
                /* Do not force two unresolved function results to integers:
                   one may resolve to a string and the other to a number. */
            } else if (left!=NATIVE_UNKNOWN && right!=NATIVE_UNKNOWN &&
                left!=right &&
                !(native_type_is_numeric(left) && native_type_is_numeric(right))){
                /* different kinds: equality=false, inequality=true */
            } else if (left==NATIVE_ANY || right==NATIVE_ANY){
                c->module->needs_runtime=1;
            } else if (native_type_is_numeric(left) || native_type_is_numeric(right) ||
                left==NATIVE_UNKNOWN || right==NATIVE_UNKNOWN){
                if (!infer_numeric_pair(c,expr->as.bin.a,expr->as.bin.b,
                                        expr->pos,&left,&right)) return 0;
                if (left==NATIVE_NUM || right==NATIVE_NUM)
                    c->module->needs_runtime=1;
            } else if (left!=right){
                return fail_at(c,expr->pos,
                               "equality requires operands with matching types");
            } else if (left==NATIVE_STR){
                c->module->needs_runtime=1;
            }
            type = NATIVE_BOOL;
        } else {
            return fail_at(c, expr->pos, "operator '%s' is not supported", op);
        }
    } else {
        return fail_at(c, expr->pos, "expression kind %d is not supported",
                       (int)expr->kind);
    }
inference_done:
    if (expected != NATIVE_UNKNOWN && expected!=NATIVE_ANY){
        if (type == NATIVE_UNKNOWN){
            if (!infer_constrain_expr(c, expr, expected)) return 0;
            type = expected;
        } else if (!constrain_local_type(c,&type,expected,expr->pos,"expression")) return 0;
    }
    *out_type = type;
    return 1;
}

static int native_ffi_method(Compiler* c,AstExpr* callee){
    if(!callee || callee->kind!=AST_ATTR || !callee->as.attr.obj ||
       callee->as.attr.obj->kind!=AST_VAR)return 0;
    AstExpr* obj=callee->as.attr.obj;
    Binding* binding=find_binding(c,obj->as.var.name_start,obj->as.var.name_len);
    if(!binding)binding=find_global(c->module,obj->as.var.name_start,obj->as.var.name_len);
    if(!binding || !binding->is_ffi_module)return 0;
    if(binding->is_ffi_module==2)return 3;
    if(callee->as.attr.n==strlen("загрузить") &&
       !memcmp(callee->as.attr.name,"загрузить",callee->as.attr.n))return 1;
    if(callee->as.attr.n==strlen("вызвать") &&
       !memcmp(callee->as.attr.name,"вызвать",callee->as.attr.n))return 2;
    return -1;
}

static int infer_statements(Compiler* c, AstStmt** statements, size_t count){
    for (size_t i = 0; i < count; ++i){
        AstStmt* stmt = statements[i];
        if (!stmt) return fail_at(c, (AstPos){0}, "missing statement");
        if(stmt->kind==STMT_IMPORT){
            const AstImport* imp=&stmt->as.imp;
            int module_kind=imp->nn==strlen("внешний") &&
                !memcmp(imp->name,"внешний",imp->nn)?1:
                imp->nn==strlen("память") && !memcmp(imp->name,"память",imp->nn)?2:0;
            if(module_kind && !imp->from_dir_len){
                if(module_kind==1 && !c->module->allow_ffi)
                    return fail_at(c,stmt->pos,"foreign module requires explicit --allow-ffi");
                const char* alias=imp->an?imp->alias:imp->name;
                size_t length=imp->an?imp->an:imp->nn;
                if(c->is_main){
                    Binding* global=add_global_binding(c->module,alias,length,NATIVE_ANY);
                    if(!global)return fail_at(c,stmt->pos,"could not bind native module import");
                    global->is_ffi_module=module_kind;
                    Binding* visible=find_binding(c,alias,length);
                    if(!visible){
                        if(!add_binding(c,alias,length,global->slot_id,NATIVE_ANY))return 0;
                        visible=find_binding(c,alias,length);
                        visible->is_global=1;
                    }
                    visible->is_ffi_module=module_kind;
                }else{
                    Binding* local=find_binding(c,alias,length);
                    if(!local){
                        if(c->binding_count>UINT_MAX ||
                            !add_binding(c,alias,length,(unsigned)c->binding_count,NATIVE_ANY))
                            return fail_at(c,stmt->pos,"cannot bind local native module import");
                        local=find_binding(c,alias,length);
                    }else if(local->is_parameter ||
                             (local->is_ffi_module && local->is_ffi_module!=module_kind)){
                        return fail_at(c,stmt->pos,"conflicting native module import alias");
                    }
                    local->is_ffi_module=module_kind;
                }
                c->module->needs_runtime=1;
            } else return fail_at(c,stmt->pos,"unexpected unexpanded module import");
        } else if (stmt->kind == STMT_LET){
            Binding* binding = c->is_main ?
                find_binding(c, stmt->as.let_.ns, stmt->as.let_.nn) :
                find_binding(c, stmt->as.let_.ns, stmt->as.let_.nn);
            if (!binding && c->function)
                binding = find_global(c->module, stmt->as.let_.ns, stmt->as.let_.nn);
            if(binding && binding->is_ffi_module)
                return fail_at(c,stmt->pos,"cannot redeclare a native module alias");
            int value_type = NATIVE_UNKNOWN;
            if (!infer_expr_type(c, stmt->as.let_.value, NATIVE_UNKNOWN,
                                 &value_type)) return 0;
            if (value_type == NATIVE_VOID)
                return fail_at(c, stmt->pos,
                               "a void function cannot initialize a variable");
            if (c->is_main){
                Binding* global = add_global_binding(c->module,
                    stmt->as.let_.ns, stmt->as.let_.nn, value_type);
                if (!global) return fail_at(c, stmt->pos,
                    "global variable declaration conflicts with its earlier type");
                global->initializer = stmt->as.let_.value;
                Binding* visible = find_binding(c, stmt->as.let_.ns,
                                                stmt->as.let_.nn);
                if (!visible){
                    if (!add_binding(c, stmt->as.let_.ns, stmt->as.let_.nn,
                                     global->slot_id, global->type))
                        return fail_at(c, stmt->pos, "out of memory while inferring globals");
                    visible = find_binding(c, stmt->as.let_.ns, stmt->as.let_.nn);
                    visible->is_global = 1;
                } else {
                    visible->type = global->type;
                    visible->initializer = stmt->as.let_.value;
                }
            } else {
                Binding* local = find_binding(c, stmt->as.let_.ns,
                                              stmt->as.let_.nn);
                if (!local){
                    if (c->binding_count > UINT_MAX)
                        return fail_at(c, stmt->pos,
                                       "too many local variables for native frame");
                    unsigned slot = (unsigned)c->binding_count;
                    if (!add_binding(c, stmt->as.let_.ns, stmt->as.let_.nn,
                                     slot, value_type))
                        return fail_at(c, stmt->pos, "out of memory while inferring locals");
                    local = find_binding(c, stmt->as.let_.ns, stmt->as.let_.nn);
                }
                if (!constrain_local_type(c,&local->type,value_type,
                                          stmt->pos,"local variable")) return 0;
                local->initializer = stmt->as.let_.value;
            }
        } else if (stmt->kind == STMT_SET){
            Binding* binding = find_binding(c, stmt->as.set.ns, stmt->as.set.nn);
            if (!binding && !c->is_main)
                binding = infer_binding(c, stmt->as.set.ns, stmt->as.set.nn);
            if (!binding)
                return fail_at(c, stmt->pos, "assignment to unknown variable '%.*s'",
                    (int)stmt->as.set.nn, stmt->as.set.ns);
            int value_type = NATIVE_UNKNOWN;
            if (!infer_expr_type(c, stmt->as.set.value, NATIVE_UNKNOWN,
                                 &value_type)) return 0;
            if (binding->is_ffi_module)
                return fail_at(c,stmt->pos,"cannot rebind a native module alias");
            if (value_type!=NATIVE_UNKNOWN &&
                !(binding->is_global || binding->is_parameter ?
                  constrain_type(c,&binding->type,value_type,stmt->pos,"variable") :
                  constrain_local_type(c,&binding->type,value_type,stmt->pos,"variable"))) return 0;
            if (binding->is_global){
                Binding* global=find_global(c->module,stmt->as.set.ns,stmt->as.set.nn);
                if (global && value_type!=NATIVE_UNKNOWN &&
                    !constrain_type(c,&global->type,value_type,stmt->pos,"global variable")) return 0;
            }
        } else if (stmt->kind == STMT_SETATTR){
            int base,value;
            if (!infer_expr_type(c,stmt->as.sattr.obj,NATIVE_UNKNOWN,&base) ||
                !infer_expr_type(c,stmt->as.sattr.value,NATIVE_UNKNOWN,&value)) return 0;
            if (base==NATIVE_VOID || value==NATIVE_VOID)
                return fail_at(c,stmt->pos,"attribute assignment requires values");
            c->module->needs_runtime=1;
        } else if (stmt->kind == STMT_SETINDEX){
            int base,index,value;
            if (!infer_expr_type(c,stmt->as.sidx.base,NATIVE_UNKNOWN,&base) ||
                !infer_expr_type(c,stmt->as.sidx.idx,NATIVE_UNKNOWN,&index) ||
                !infer_expr_type(c,stmt->as.sidx.value,NATIVE_UNKNOWN,&value)) return 0;
            if (base==NATIVE_VOID || index==NATIVE_VOID || value==NATIVE_VOID)
                return fail_at(c,stmt->pos,"indexed assignment requires values");
            c->module->needs_runtime=1;
        } else if (stmt->kind == STMT_EXPR){
            int ignored;
            if (!infer_expr_type(c, stmt->as.expr, NATIVE_UNKNOWN, &ignored)) return 0;
        } else if (stmt->kind == STMT_PRINT){
            for (size_t argument_index=0;argument_index<stmt->as.print.argc;++argument_index){
                int value_type;
                if (!infer_expr_type(c,stmt->as.print.args[argument_index],NATIVE_UNKNOWN,
                                     &value_type)) return 0;
                if (value_type!=NATIVE_I64 && value_type!=NATIVE_BOOL &&
                    value_type!=NATIVE_STR && value_type!=NATIVE_NUM &&
                    value_type!=NATIVE_NIL && value_type!=NATIVE_ANY &&
                    value_type!=NATIVE_UNKNOWN)
                    return fail_at(c,stmt->as.print.args[argument_index]->pos,
                                   "print value type is not supported");
                /* The printf fast path is valid only for one i64 argument.
                   All other print expressions need the shared Runtime printer,
                   even if no other expression requires the Runtime. */
                if (value_type!=NATIVE_I64 || stmt->as.print.argc!=1)
                    c->module->needs_runtime=1;
            }
        } else if (stmt->kind == STMT_IF){
            int condition;
            if (!infer_expr_type(c, stmt->as.if_.cond, NATIVE_UNKNOWN,
                                 &condition)) return 0;
            if (condition != NATIVE_UNKNOWN && condition != NATIVE_I64 &&
                condition != NATIVE_BOOL && condition != NATIVE_STR &&
                condition != NATIVE_NUM && condition != NATIVE_NIL &&
                condition != NATIVE_ANY)
                return fail_at(c, stmt->pos, "condition requires integer, boolean, string or nil");
            if (!infer_statements(c, stmt->as.if_.then_b,
                                  stmt->as.if_.then_len) ||
                !infer_statements(c, stmt->as.if_.else_b,
                                  stmt->as.if_.else_len)) return 0;
        } else if (stmt->kind == STMT_WHILE){
            int condition;
            if (!infer_expr_type(c, stmt->as.wh.cond, NATIVE_UNKNOWN,
                                 &condition)) return 0;
            if (condition != NATIVE_UNKNOWN && condition != NATIVE_I64 &&
                condition != NATIVE_BOOL && condition != NATIVE_STR &&
                condition != NATIVE_NUM && condition != NATIVE_NIL &&
                condition != NATIVE_ANY)
                return fail_at(c, stmt->pos, "condition requires integer, boolean, string or nil");
            if (!infer_statements(c, stmt->as.wh.body, stmt->as.wh.body_len)) return 0;
        } else if (stmt->kind == STMT_FOREACH){
            int sequence_type=NATIVE_UNKNOWN;
            if (!infer_expr_type(c,stmt->as.fe.seq,NATIVE_UNKNOWN,
                                 &sequence_type)) return 0;
            if (sequence_type==NATIVE_VOID)
                return fail_at(c,stmt->pos,"foreach sequence needs a value");
            c->module->needs_runtime=1;
            Binding* iterator=find_binding(c,stmt->as.fe.it,stmt->as.fe.in);
            if (!iterator && !c->is_main)
                iterator=find_global(c->module,stmt->as.fe.it,stmt->as.fe.in);
            if (c->is_main){
                Binding* global=add_global_binding(c->module,
                    stmt->as.fe.it,stmt->as.fe.in,NATIVE_ANY);
                if (!global) return fail_at(c,stmt->pos,"failed to declare foreach variable");
                if (!iterator){
                    if (!add_binding(c,stmt->as.fe.it,stmt->as.fe.in,
                                     global->slot_id,global->type))
                        return fail_at(c,stmt->pos,"out of memory inferring foreach variable");
                    iterator=find_binding(c,stmt->as.fe.it,stmt->as.fe.in);
                    iterator->is_global=1;
                }
            } else if (!iterator){
                if (c->binding_count>UINT_MAX ||
                    !add_binding(c,stmt->as.fe.it,stmt->as.fe.in,
                                 (unsigned)c->binding_count,NATIVE_ANY))
                    return fail_at(c,stmt->pos,"out of memory inferring foreach variable");
                iterator=find_binding(c,stmt->as.fe.it,stmt->as.fe.in);
            }
            if (!iterator ||
                !(iterator->is_global || iterator->is_parameter ?
                    constrain_type(c,&iterator->type,NATIVE_ANY,stmt->pos,"iterator") :
                    constrain_local_type(c,&iterator->type,NATIVE_ANY,stmt->pos,"iterator"))) return 0;
            if (!infer_statements(c,stmt->as.fe.body,stmt->as.fe.body_len)) return 0;
        } else if (stmt->kind == STMT_BREAK || stmt->kind == STMT_CONTINUE){
            /* Legality is checked during lowering, where lexical loop nesting
               is available. A break in an if branch is otherwise type-neutral. */
        } else if (stmt->kind == STMT_RETURN){
            if (!c->function)
                return fail_at(c, stmt->pos,
                               "return from top-level initialization is not supported");
            if (stmt->as.ret.value){
                int value_type;
                if (!infer_expr_type(c, stmt->as.ret.value, NATIVE_UNKNOWN,
                                     &value_type)) return 0;
                c->function->saw_value_return = 1;
                if (value_type != NATIVE_UNKNOWN &&
                    !constrain_type(c, &c->function->return_type, value_type,
                                    stmt->pos, "function return")) return 0;
            } else {
                c->function->saw_void_return = 1;
                if (!constrain_type(c,&c->function->return_type,NATIVE_NIL,
                                    stmt->pos,"function return")) return 0;
                c->module->needs_runtime=1;
            }
        } else if (stmt->kind == STMT_TRY){
            c->module->needs_runtime=1;
            if(!infer_statements(c,stmt->as.try_.body,stmt->as.try_.body_len))return 0;
            if(stmt->as.try_.has_catch){
                const char* name=stmt->as.try_.catch_name;
                size_t length=stmt->as.try_.catch_name_len;
                if(!name || !length)
                    return fail_at(c,stmt->pos,"try/catch requires error name");
                /* A catch binding is lexical, not a module/global binding.
                   It may shadow an outer variable, but must become invisible
                   again before `всегда` and subsequent statements. */
                if(c->binding_count>UINT_MAX)return fail_at(c,stmt->pos,"too many catch locals");
                size_t scoped_index=c->binding_count;
                if(!add_binding(c,name,length,(unsigned)c->binding_count,NATIVE_ANY))
                    return fail_at(c,stmt->pos,"could not allocate Native catch variable");
                c->bindings[scoped_index].catch_owner=stmt;
                if(!infer_statements(c,stmt->as.try_.catch_body,stmt->as.try_.catch_body_len))return 0;
                c->bindings[scoped_index].hidden=1;
            }
            if(!infer_statements(c,stmt->as.try_.finally_body,stmt->as.try_.finally_body_len))return 0;
        } else if (stmt->kind == STMT_FUNCTION){
            /* Hoisted at the function entry, not executed at declaration site. */
        } else {
            return fail_at(c, stmt->pos, "statement kind %d is not supported",
                           (int)stmt->kind);
        }
    }
    return 1;
}

static int function_guarantees_return(AstStmt** statements, size_t count){
    for (size_t i = 0; i < count; ++i){
        AstStmt* stmt = statements[i];
        if (!stmt) continue;
        if (stmt->kind == STMT_RETURN) return 1;
        if (stmt->kind == STMT_IF && stmt->as.if_.has_else &&
            function_guarantees_return(stmt->as.if_.then_b, stmt->as.if_.then_len) &&
            function_guarantees_return(stmt->as.if_.else_b, stmt->as.if_.else_len))
            return 1;
    }
    return 0;
}

static int collect_function_one(NativeModule* module,AstFunc* ast,
                                int is_nested,AstFunc* parent){
    if (!ast) return 0;
    if(module->function_count==module->function_capacity){
        size_t capacity=module->function_capacity ? module->function_capacity*2:8;
        if(capacity<module->function_capacity || capacity>SIZE_MAX/sizeof(NativeFunction))
            return 0;
        NativeFunction* grown=(NativeFunction*)calloc(capacity,sizeof(*grown));
        if(!grown)return 0;
        if(module->function_count)
            memcpy(grown,module->functions,module->function_count*sizeof(*grown));
        free(module->functions);
        module->functions=grown;
        module->function_capacity=capacity;
    }
    int* params=ast->param_count ?
        (int*)malloc(ast->param_count*sizeof(int)):NULL;
    if(ast->param_count && !params)return 0;
    for(size_t p=0;p<ast->param_count;++p)
        params[p]=is_nested ? NATIVE_ANY:NATIVE_UNKNOWN;
    NativeFunction* function=&module->functions[module->function_count];
    memset(function,0,sizeof(*function));
    function->ast=ast;
    function->parent=parent;
    function->id=module->function_count;
    function->parameter_types=params;
    function->return_type=is_nested ? NATIVE_ANY:NATIVE_UNKNOWN;
    function->is_nested=is_nested;
    ++module->function_count;
    if(is_nested) module->needs_runtime=1;
    return 1;
}

static int collect_nested_statements(NativeModule* module,
                          AstStmt** statements,size_t count,AstFunc* parent){
    for(size_t i=0;i<count;++i){
        AstStmt* stmt=statements[i];
        if(!stmt)return 0;
        if(stmt->kind==STMT_FUNCTION){
            if(!collect_function_one(module,stmt->as.function,1,parent))return 0;
            if(!collect_nested_statements(module,stmt->as.function->body,
                stmt->as.function->body_len,stmt->as.function))return 0;
        } else if(stmt->kind==STMT_IF){
            if(!collect_nested_statements(module,stmt->as.if_.then_b,stmt->as.if_.then_len,parent)||
               !collect_nested_statements(module,stmt->as.if_.else_b,stmt->as.if_.else_len,parent))return 0;
        } else if(stmt->kind==STMT_WHILE){
            if(!collect_nested_statements(module,stmt->as.wh.body,stmt->as.wh.body_len,parent))return 0;
        } else if(stmt->kind==STMT_FOREACH){
            if(!collect_nested_statements(module,stmt->as.fe.body,stmt->as.fe.body_len,parent))return 0;
        } else if(stmt->kind==STMT_TRY){
            if(!collect_nested_statements(module,stmt->as.try_.body,stmt->as.try_.body_len,parent) ||
               !collect_nested_statements(module,stmt->as.try_.catch_body,stmt->as.try_.catch_body_len,parent) ||
               !collect_nested_statements(module,stmt->as.try_.finally_body,stmt->as.try_.finally_body_len,parent))return 0;
        }
    }
    return 1;
}

static int collect_functions(NativeModule* module){
    for(size_t i=0;i<module->program->func_count;++i){
        if(!collect_function_one(module,module->program->funcs[i],0,NULL))return 0;
    }
    /* Collect nested definitions only after all top-level functions, so
       existing direct-call function identifiers remain stable. */
    if(!collect_nested_statements(module,module->program->init,module->program->init_len,NULL))
        return 0;
    for(size_t i=0;i<module->program->func_count;++i){
        AstFunc* function=module->program->funcs[i];
        if(!collect_nested_statements(module,function->body,function->body_len,function))
            return 0;
    }
    return 1;
}

/* The interpreter hoists nested declarations throughout an enclosing body,
   including declarations written inside conditional or loop blocks. */
static int hoist_nested_bindings(Compiler* c,AstStmt** statements,size_t count){
    for(size_t i=0;i<count;++i){
        AstStmt* stmt=statements[i];
        if(!stmt)return fail_at(c,(AstPos){0},"missing statement");
        if(stmt->kind==STMT_FUNCTION){
            AstFunc* ast=stmt->as.function;
            if(!find_nested_function(c->module,ast))
                return fail_at(c,stmt->pos,"missing compiled nested function");
            if(c->is_main){
                Binding* global=add_global_binding(c->module,ast->name_start,
                                                   ast->name_len,NATIVE_ANY);
                if(!global || !add_binding(c,ast->name_start,ast->name_len,
                                           global->slot_id,NATIVE_ANY))
                    return fail_at(c,stmt->pos,"failed to hoist nested global");
                find_binding(c,ast->name_start,ast->name_len)->is_global=1;
            } else if(!find_binding(c,ast->name_start,ast->name_len)){
                if(c->binding_count>UINT_MAX ||
                   !add_binding(c,ast->name_start,ast->name_len,
                                (unsigned)c->binding_count,NATIVE_ANY))
                    return fail_at(c,stmt->pos,"failed to hoist nested local");
            }
        } else if(stmt->kind==STMT_IF){
            if(!hoist_nested_bindings(c,stmt->as.if_.then_b,stmt->as.if_.then_len)||
               !hoist_nested_bindings(c,stmt->as.if_.else_b,stmt->as.if_.else_len))return 0;
        } else if(stmt->kind==STMT_WHILE){
            if(!hoist_nested_bindings(c,stmt->as.wh.body,stmt->as.wh.body_len))return 0;
        } else if(stmt->kind==STMT_FOREACH){
            if(!hoist_nested_bindings(c,stmt->as.fe.body,stmt->as.fe.body_len))return 0;
        } else if(stmt->kind==STMT_TRY){
            if(!hoist_nested_bindings(c,stmt->as.try_.body,stmt->as.try_.body_len) ||
               !hoist_nested_bindings(c,stmt->as.try_.catch_body,stmt->as.try_.catch_body_len) ||
               !hoist_nested_bindings(c,stmt->as.try_.finally_body,stmt->as.try_.finally_body_len))return 0;
        }
    }
    return 1;
}

static int emit_hoisted_functions(Compiler* c,AstStmt** statements,size_t count){
    for(size_t i=0;i<count;++i){
        AstStmt* stmt=statements[i];
        if(!stmt)return fail_at(c,(AstPos){0},"missing statement");
        if(stmt->kind==STMT_FUNCTION){
            NativeFunction* fn=find_nested_function(c->module,stmt->as.function);
            Binding* binding=find_binding(c,stmt->as.function->name_start,
                                            stmt->as.function->name_len);
            if(!binding && c->is_main)
                binding=find_global(c->module,stmt->as.function->name_start,
                                    stmt->as.function->name_len);
            if(!fn || !binding || binding->type!=NATIVE_ANY)
                return fail_at(c,stmt->pos,"nested function binding is missing");
            unsigned value=c->next_value++;
            if(fn->capture_count){
                unsigned array=c->next_value++,ptr=c->next_value++;
                if(!text_appendf(&c->allocas,
                    "  %%capture_array%u = alloca [%zu x ptr]\n",
                    array,fn->capture_count))return 0;
                for(size_t j=0;j<fn->capture_count;++j){
                    NativeCapture* cap=&fn->captures[j];
                    Binding* parent=find_binding(c,cap->name,cap->length);
                    if(!parent || !parent->boxed)
                        return fail_at(c,stmt->pos,"capture has no mutable parent cell");
                    unsigned cell=c->next_value++,address=c->next_value++;
                    if(parent->is_capture){
                        if(!text_appendf(&c->initializers,
                            "  %%v%u = call ptr @kirton_native_capture_cell(ptr %%rt, ptr %%closure, i64 %u)\n",
                            cell,parent->capture_index))return 0;
                    } else if(!text_appendf(&c->initializers,
                        "  %%v%u = load ptr, ptr %%slot%u\n",cell,parent->slot_id))return 0;
                    if(!text_appendf(&c->initializers,
                        "  %%v%u = getelementptr inbounds [%zu x ptr], ptr %%capture_array%u, i64 0, i64 %zu\n"
                        "  store ptr %%v%u, ptr %%v%u\n",
                        address,fn->capture_count,array,j,cell,address))return 0;
                }
                if(!text_appendf(&c->initializers,
                    "  %%v%u = getelementptr inbounds [%zu x ptr], ptr %%capture_array%u, i64 0, i64 0\n"
                    "  %%v%u = call ptr @kirton_native_function_new_captured(ptr %%rt, i64 %zu, ptr %%v%u, i64 %zu)\n",
                    ptr,fn->capture_count,array,value,fn->id,ptr,fn->capture_count))return 0;
            } else if(!text_appendf(&c->initializers,
                "  %%v%u = call ptr @kirton_native_function_new(ptr %%rt, i64 %zu)\n",
                value,fn->id)) return fail_at(c,stmt->pos,"failed to emit function value");
            if(binding->boxed){
                unsigned cell=c->next_value++;
                if(!text_appendf(&c->initializers,
                    "  %%v%u = load ptr, ptr %%slot%u\n"
                    "  call void @kirton_native_cell_set(ptr %%rt, ptr %%v%u, ptr %%v%u)\n"
                    "  store i1 true, ptr %%init%u\n",
                    cell,binding->slot_id,cell,value,binding->slot_id))return 0;
            } else if(binding->is_global){
                if(!text_appendf(&c->initializers,
                    "  store ptr %%v%u, ptr @gslot%u\n"
                    "  store i1 true, ptr @ginit%u\n",
                    value,binding->slot_id,binding->slot_id))return 0;
            } else if(!text_appendf(&c->initializers,
                "  store ptr %%v%u, ptr %%slot%u\n"
                "  store i1 true, ptr %%init%u\n",
                value,binding->slot_id,binding->slot_id))return 0;
        } else if(stmt->kind==STMT_IF){
            if(!emit_hoisted_functions(c,stmt->as.if_.then_b,stmt->as.if_.then_len)||
               !emit_hoisted_functions(c,stmt->as.if_.else_b,stmt->as.if_.else_len))return 0;
        } else if(stmt->kind==STMT_WHILE){
            if(!emit_hoisted_functions(c,stmt->as.wh.body,stmt->as.wh.body_len))return 0;
        } else if(stmt->kind==STMT_FOREACH){
            if(!emit_hoisted_functions(c,stmt->as.fe.body,stmt->as.fe.body_len))return 0;
        } else if(stmt->kind==STMT_TRY){
            if(!emit_hoisted_functions(c,stmt->as.try_.body,stmt->as.try_.body_len) ||
               !emit_hoisted_functions(c,stmt->as.try_.catch_body,stmt->as.try_.catch_body_len) ||
               !emit_hoisted_functions(c,stmt->as.try_.finally_body,stmt->as.try_.finally_body_len))return 0;
        }
    }
    return 1;
}

static int infer_module_types(NativeModule* module){
    for (unsigned pass = 0; pass < 64; ++pass){
        module->changed = 0;
        Compiler main_infer;
        memset(&main_infer, 0, sizeof(main_infer));
        main_infer.module = module;
        main_infer.program = module->program;
        main_infer.is_main = 1;
        main_infer.error = module->error;
        main_infer.error_capacity = module->error_capacity;
        if (!hoist_nested_bindings(&main_infer,module->program->init,
                                    module->program->init_len) ||
            !infer_statements(&main_infer, module->program->init,
                              module->program->init_len)){
            free(main_infer.bindings);
            return 0;
        }
        free(main_infer.bindings);
        for (size_t i = 0; i < module->function_count; ++i){
            NativeFunction* function = &module->functions[i];
            if (!function->is_nested && find_function(module, function->ast->name_start,
                              function->ast->name_len) != function) continue;
            function->saw_value_return = 0;
            function->saw_void_return = 0;
            Compiler infer;
            memset(&infer, 0, sizeof(infer));
            infer.module = module;
            infer.program = module->program;
            infer.function = function;
            infer.error = module->error;
            infer.error_capacity = module->error_capacity;
            for (size_t p = 0; p < function->ast->param_count; ++p){
                const char* name = function->ast->params_start[p];
                size_t name_len = function->ast->params_len[p];
                Binding* prior = find_binding(&infer, name, name_len);
                if (prior){
                    if (prior->type != function->parameter_types[p] &&
                        function->parameter_types[p] != NATIVE_UNKNOWN){
                        free(infer.bindings);
                        return fail_at(&infer, function->ast->pos,
                                       "duplicate parameters with conflicting types");
                    }
                    continue;
                }
                if (!add_binding(&infer, name, name_len, (unsigned)p,
                                 function->parameter_types[p])){
                    free(infer.bindings);
                    return fail_at(&infer, function->ast->pos,
                                   "out of memory while inferring parameters");
                }
                Binding* parameter = find_binding(&infer, name, name_len);
                parameter->is_parameter = 1;
            }
            if (!setup_captures(&infer) ||
                !hoist_nested_bindings(&infer,function->ast->body,function->ast->body_len) ||
                !infer_statements(&infer, function->ast->body,
                                  function->ast->body_len)){
                free(infer.bindings);
                return 0;
            }
            /* Local analysis may widen a parameter after an assignment or
               loop binding. Propagate it to the externally visible function
               signature before lowering; otherwise opaque LLVM pointers can
               be loaded from an i64 parameter slot (undefined behavior). */
            for (size_t p=0;p<function->ast->param_count;++p){
                for (size_t b=0;b<infer.binding_count;++b){
                    Binding* param=&infer.bindings[b];
                    if (param->is_parameter && param->slot_id==p &&
                        !constrain_type(&infer,&function->parameter_types[p],
                                        param->type,function->ast->pos,
                                        "function parameter")){
                        free(infer.bindings);
                        return 0;
                    }
                }
            }
            /* Bare return and implicit fallthrough both have a nil value.
               When any other kind is returned, widen the signature to ANY. */
            if ((function->saw_value_return || function->saw_void_return) &&
                !function_guarantees_return(function->ast->body,
                                            function->ast->body_len) &&
                function->return_type != NATIVE_UNKNOWN &&
                function->return_type != NATIVE_NIL &&
                function->return_type != NATIVE_ANY){
                function->return_type=NATIVE_ANY;
                module->needs_runtime=1;
                module->changed=1;
            }
            if (!function->saw_value_return && !function->saw_void_return &&
                function->return_type == NATIVE_UNKNOWN){
                /* Falling off a function body returns nil in AST and Bytecode.
                   Preserve that observable value in compiled calls. */
                function->return_type = NATIVE_NIL;
                module->needs_runtime = 1;
                module->changed = 1;
            }
            free(infer.bindings);
        }
        if (!module->changed) break;
    }
    for (size_t i = 0; i < module->function_count; ++i){
        NativeFunction* function = &module->functions[i];
        if (find_function(module, function->ast->name_start,
                          function->ast->name_len) != function) continue;
        for (size_t p = 0; p < function->ast->param_count; ++p)
            if (function->parameter_types[p] == NATIVE_UNKNOWN)
                function->parameter_types[p] = NATIVE_I64;
    }
    module->changed = 0;
    for (size_t i = 0; i < module->function_count; ++i){
        NativeFunction* function = &module->functions[i];
        if (find_function(module, function->ast->name_start,
                          function->ast->name_len) != function) continue;
        function->saw_value_return = 0;
        function->saw_void_return = 0;
        Compiler infer;
        memset(&infer, 0, sizeof(infer));
        infer.module = module;
        infer.program = module->program;
        infer.function = function;
        infer.error = module->error;
        infer.error_capacity = module->error_capacity;
        for (size_t p = 0; p < function->ast->param_count; ++p){
            if (!add_binding(&infer, function->ast->params_start[p],
                             function->ast->params_len[p], (unsigned)p,
                             function->parameter_types[p])){
                free(infer.bindings);
                return fail_at(&infer, function->ast->pos,
                               "out of memory while inferring parameters");
            }
            Binding* parameter = find_binding(&infer,
                function->ast->params_start[p], function->ast->params_len[p]);
            parameter->is_parameter = 1;
        }
        int ok = setup_captures(&infer) &&
                 hoist_nested_bindings(&infer,function->ast->body,
                                       function->ast->body_len) &&
                 infer_statements(&infer, function->ast->body,
                                  function->ast->body_len);
        free(infer.bindings);
        if (!ok) return 0;
    }
    for (size_t i = 0; i < module->function_count; ++i){
        NativeFunction* function = &module->functions[i];
        if (find_function(module, function->ast->name_start,
                          function->ast->name_len) != function) continue;
        if (function->return_type == NATIVE_UNKNOWN){
            if (function->saw_value_return){
                if (module->error && module->error_capacity)
                    snprintf(module->error, module->error_capacity,
                        "native compiler: could not infer return type of '%.*s'",
                        (int)function->ast->name_len,
                        function->ast->name_start);
                return 0;
            }
            function->return_type = NATIVE_NIL;
            module->needs_runtime=1;
        }
        if (function->return_type != NATIVE_VOID &&
            function->return_type != NATIVE_NIL && function->return_type != NATIVE_ANY &&
            !function_guarantees_return(function->ast->body,
                                        function->ast->body_len)){
            if (module->error && module->error_capacity)
                snprintf(module->error, module->error_capacity,
                    "native compiler: value-returning function '%.*s' can fall through",
                    (int)function->ast->name_len, function->ast->name_start);
            return 0;
        }
    }
    return 1;
}

static int emit_store(Compiler* c, Binding* binding, NativeValue value,
                      AstPos pos){
    if (binding->is_global && !c->is_main &&
        native_type_is_managed(value.type)){
        unsigned rooted=c->next_value++;
        if (!text_appendf(&c->text,
            "  %%v%u = call ptr @kirton_native_root_value(ptr %%rt, ptr %%v%u)\n",
            rooted,value.id))
            return fail_at(c,pos,"out of memory while promoting global Value root");
        value.id=rooted;
    }
    if (binding->type != value.type)
        return fail_at(c, pos,
                       "assignment changes variable kind; Native currently requires stable variable kinds");
    if(binding->boxed) return emit_cell_store(c,binding,value,pos,0);
    if (!emit_uninitialized_check(c, binding, pos)) return 0;
    const char* llvm_type = llvm_value_type(value.type);
    if (!text_appendf(&c->text,
        binding->is_global ? "  store %s %%v%u, ptr @gslot%u\n" :
                             "  store %s %%v%u, ptr %%slot%u\n",
        llvm_type, value.id, binding->slot_id))
        return fail_at(c, pos, "out of memory while storing variable");
    if (binding->is_global){
        if (!text_appendf(&c->text,
            "  store i1 true, ptr @ginit%u\n", binding->slot_id))
            return fail_at(c, pos, "out of memory while marking global initialized");
    }
    return 1;
}

static int emit_gc_safepoint(Compiler* c,AstPos pos){
    if (!c->module->needs_runtime) return 1;
    size_t roots=0;
    for (size_t i=0;i<c->module->global_count;++i)
        if (native_type_is_managed(c->module->globals[i].type)) ++roots;
    if (!c->is_main)
        for (size_t i=0;i<c->binding_count;++i)
            if (!c->bindings[i].is_capture &&
                native_type_is_managed(c->bindings[i].type)) ++roots;
    for (NativeForeachRoot* it=c->active_foreach_roots;it;it=it->previous) ++roots;
    unsigned array_id=0;
    if (roots){
        array_id=c->next_root_array++;
        if (!text_appendf(&c->allocas,"  %%rootarr%u = alloca [%zu x ptr]\n",
                          array_id,roots))
            return fail_at(c,pos,"out of memory while allocating GC roots");
    }
    size_t index=0;
    for (size_t i=0;i<c->module->global_count;++i){
        Binding* binding=&c->module->globals[i];
        if (!native_type_is_managed(binding->type)) continue;
        unsigned value_id=c->next_value++,address_id=c->next_value++;
        if (!text_appendf(&c->text,
            "  %%v%u = load ptr, ptr @gslot%u\n"
            "  %%v%u = getelementptr inbounds [%zu x ptr], ptr %%rootarr%u, i64 0, i64 %zu\n"
            "  store ptr %%v%u, ptr %%v%u\n",
            value_id,binding->slot_id,address_id,roots,array_id,index++,
            value_id,address_id))
            return fail_at(c,pos,"out of memory while emitting global GC roots");
    }
    if (!c->is_main){
        for (size_t i=0;i<c->binding_count;++i){
            Binding* binding=&c->bindings[i];
            if (binding->is_capture || !native_type_is_managed(binding->type)) continue;
            unsigned value_id=c->next_value++,address_id=c->next_value++;
            if (!text_appendf(&c->text,
                "  %%v%u = load ptr, ptr %%slot%u\n"
                "  %%v%u = getelementptr inbounds [%zu x ptr], ptr %%rootarr%u, i64 0, i64 %zu\n"
                "  store ptr %%v%u, ptr %%v%u\n",
                value_id,binding->slot_id,address_id,roots,array_id,index++,
                value_id,address_id))
                return fail_at(c,pos,"out of memory while emitting local GC roots");
        }
    }
    for (NativeForeachRoot* it=c->active_foreach_roots;it;it=it->previous){
        unsigned address_id=c->next_value++;
        if (!text_appendf(&c->text,
            "  %%v%u = getelementptr inbounds [%zu x ptr], ptr %%rootarr%u, i64 0, i64 %zu\n"
            "  store ptr %%v%u, ptr %%v%u\n",
            address_id,roots,array_id,index++,it->value_id,address_id))
            return fail_at(c,pos,"out of memory preserving active foreach sequence");
    }
    if (roots){
        if (!text_appendf(&c->text,
            "  call void @kirton_native_safepoint(ptr %%rt, ptr %%rootarr%u, i64 %zu)\n",
            array_id,roots))
            return fail_at(c,pos,"out of memory while emitting GC safepoint");
    } else if (!text_appendf(&c->text,
        "  call void @kirton_native_safepoint(ptr %%rt, ptr null, i64 0)\n"))
        return fail_at(c,pos,"out of memory while emitting GC safepoint");
    return 1;
}

static int emit_try_status_guard(Compiler* c,AstPos pos){
    if(!c->module->needs_runtime) return 1;
    unsigned test=c->next_value++,cond=c->next_value++,resume=c->next_block++;
    int ok;
    if(c->try_error_target_plus_one){
        ok=text_appendf(&c->text,
            "  %%v%u = call i32 @kirton_native_error_pending(ptr %%rt)\n"
            "  %%v%u = icmp ne i32 %%v%u, 0\n"
            "  br i1 %%v%u, label %%try_error%u, label %%try_resume%u\n"
            "try_resume%u:\n",
            test,cond,test,cond,c->try_error_target_plus_one-1,resume,resume);
    } else if(c->function){
        ok=text_appendf(&c->text,
            "  %%v%u = call i32 @kirton_native_error_pending(ptr %%rt)\n"
            "  %%v%u = icmp ne i32 %%v%u, 0\n"
            "  br i1 %%v%u, label %%function_error%zu, label %%try_resume%u\n"
            "try_resume%u:\n",
            test,cond,test,cond,c->function->id,resume,resume);
    } else {
        ok=text_appendf(&c->text,
            "  %%v%u = call i32 @kirton_native_error_pending(ptr %%rt)\n"
            "  %%v%u = icmp ne i32 %%v%u, 0\n"
            "  br i1 %%v%u, label %%main_error, label %%try_resume%u\n"
            "try_resume%u:\n",
            test,cond,test,cond,resume,resume);
    }
    if(!ok) return fail_at(c,pos,"could not lower Native runtime error guard");
    return 1;
}

/* Emit lexical unwinding before a non-local return/break/continue.  Each
   active try is left exactly once; its finally runs in the surrounding lexical
   context, so an exit from finally overrides the pending exit.  A loop exit
   does not leave try scopes enclosing that loop.  The generated return Value
   already belongs to the current GC root frame and remains pinned while
   these finalizers execute. */
static int emit_native_early_unwind(Compiler* c,int loop_exit,AstPos pos){
    NativeTryScope* saved=c->active_try;
    unsigned saved_error=c->try_error_target_plus_one;
    for(NativeTryScope* scope=saved; scope; scope=scope->previous){
        if(loop_exit && scope->loop_depth < c->loop_depth) break;
        c->active_try=scope->previous;
        c->try_error_target_plus_one=scope->previous_error_target_plus_one;
        if(!text_appendf(&c->text,
                "  call void @kirton_native_try_leave(ptr %%rt)\n"))
            return fail_at(c,pos,"could not unwind Native try frame");
        if(scope->statement->as.try_.has_finally &&
           !emit_statements(c,scope->statement->as.try_.finally_body,
                             scope->statement->as.try_.finally_body_len))return 0;
        if(c->block_terminated)break;
        if(!emit_try_status_guard(c,pos))return 0;
    }
    c->active_try=saved;
    c->try_error_target_plus_one=saved_error;
    return 1;
}

static int emit_native_try(Compiler* c,AstStmt* stmt){
    unsigned block=c->next_block++;
    unsigned previous=c->try_error_target_plus_one;
    NativeTryScope scope={stmt,previous,c->loop_depth,c->active_try};
    c->active_try=&scope;
    if(!text_appendf(&c->text,"  call void @kirton_native_try_enter(ptr %%rt)\n"))return 0;
    c->try_error_target_plus_one=block+1;
    if(!emit_statements(c,stmt->as.try_.body,stmt->as.try_.body_len))return 0;
    if(!c->block_terminated && !text_appendf(&c->text,
        "  br label %%try_success%u\n",block))return 0;
    if(!text_appendf(&c->text,"try_error%u:\n",block))return 0;
    c->block_terminated=0;
    c->try_error_target_plus_one=0;
    if(stmt->as.try_.has_catch){
        Binding* binding=find_catch_binding(c,stmt);
        if(c->is_main){
            /* The emitter has its own compiler frame; unlike function emit,
               it does not reuse the type inference bindings. */
            if(c->binding_count>UINT_MAX ||
               !add_binding(c,stmt->as.try_.catch_name,
                             stmt->as.try_.catch_name_len,
                             (unsigned)c->binding_count,NATIVE_ANY))
                return fail_at(c,stmt->pos,"cannot allocate catch local");
            binding=&c->bindings[c->binding_count-1];
            binding->catch_owner=stmt;
            if(!text_appendf(&c->allocas,
                "  %%slot%u = alloca ptr\n  %%init%u = alloca i1\n",
                binding->slot_id,binding->slot_id) ||
               !text_appendf(&c->initializers,
                "  store ptr null, ptr %%slot%u\n"
                "  store i1 false, ptr %%init%u\n",
                binding->slot_id,binding->slot_id))return 0;
        }
        if(!binding || binding->type!=NATIVE_ANY || binding->boxed || binding->is_capture)
            return fail_at(c,stmt->pos,"unsupported Native catch binding");
        binding->hidden=0;
        unsigned caught=c->next_value++;
        if(!text_appendf(&c->text,
            "  %%v%u = call ptr @kirton_native_error_take(ptr %%rt)\n",
            caught))return 0;
        if(!text_appendf(&c->text,binding->is_global ?
             "  store ptr %%v%u, ptr @gslot%u\n  store i1 true, ptr @ginit%u\n" :
             "  store ptr %%v%u, ptr %%slot%u\n  store i1 true, ptr %%init%u\n",
             caught,binding->slot_id,binding->slot_id))return 0;
        /* Catch failures must still execute the finally block. */
        unsigned catch_error=c->next_block++;
        c->try_error_target_plus_one=catch_error+1;
        if(!emit_statements(c,stmt->as.try_.catch_body,
                            stmt->as.try_.catch_body_len))return 0;
        /* Drop catch-only name from lookup, including before finally. */
        Binding* scoped=find_catch_binding(c,stmt);
        if(scoped) scoped->hidden=1;
        c->try_error_target_plus_one=0;
        if(!c->block_terminated && !text_appendf(&c->text,
            "  br label %%try_finish%u\n",block))return 0;
        if(!text_appendf(&c->text,
            "try_error%u:\n  br label %%try_finish%u\n",
            catch_error,block))return 0;
    } else if(!text_appendf(&c->text,
        "  br label %%try_finish%u\n",block))return 0;
    if(!text_appendf(&c->text,
        "try_success%u:\n  br label %%try_finish%u\n"
        "try_finish%u:\n",block,block,block))return 0;
    c->block_terminated=0;
    /* Suspend an uncaught error before releasing the diagnostic silence;
       the finally can replace either the error or an earlier flow exit. */
    unsigned suspended=c->next_value++;
    if(!text_appendf(&c->text,
        "  %%v%u = call i32 @kirton_native_error_suspend(ptr %%rt)\n"
        "  call void @kirton_native_try_leave(ptr %%rt)\n",
        suspended))return 0;
    c->active_try=scope.previous;
    c->try_error_target_plus_one=previous;
    if(!emit_statements(c,stmt->as.try_.finally_body,
                        stmt->as.try_.finally_body_len))return 0;
    if(c->block_terminated)return 1;
    if(!text_appendf(&c->text,
        "  call void @kirton_native_error_resume(ptr %%rt, i32 %%v%u)\n",
        suspended))return 0;
    return emit_try_status_guard(c,stmt->pos);
}

static int emit_statements(Compiler* c, AstStmt** statements, size_t count){
    for (size_t i = 0; i < count; ++i){
        if (c->block_terminated) break;
        AstStmt* stmt = statements[i];
        if (!stmt) return fail_at(c, (AstPos){0}, "missing statement");
        if(stmt->kind==STMT_IMPORT){
            const AstImport* imp=&stmt->as.imp;
            const char* alias=imp->an?imp->alias:imp->name;
            size_t length=imp->an?imp->an:imp->nn;
            Binding* binding=find_binding(c,alias,length);
            if(c->is_main && !binding){
                Binding* global=find_global(c->module,alias,length);
                if(!global)return fail_at(c,stmt->pos,"missing global native module binding");
                if(!add_binding(c,alias,length,global->slot_id,NATIVE_ANY))return 0;
                binding=find_binding(c,alias,length);
                binding->is_global=1;
                binding->is_ffi_module=global->is_ffi_module;
            }
            if(!binding || !binding->is_ffi_module)
                return fail_at(c,stmt->pos,"missing native module binding");
            unsigned nil=c->next_value++;
            if(!text_appendf(&c->text,
                binding->is_global ?
                "  %%v%u = call ptr @kirton_native_nil(ptr %%rt)\n"
                "  store ptr %%v%u, ptr @gslot%u\n"
                "  store i1 true, ptr @ginit%u\n" :
                "  %%v%u = call ptr @kirton_native_nil(ptr %%rt)\n"
                "  store ptr %%v%u, ptr %%slot%u\n"
                "  store i1 true, ptr %%init%u\n",
                nil,nil,binding->slot_id,binding->slot_id))return 0;
        } else if (stmt->kind == STMT_LET){
            NativeValue value;
            if (!emit_expr(c, stmt->as.let_.value, &value)) return 0;
            if (value.type == NATIVE_VOID || value.type == NATIVE_UNKNOWN)
                return fail_at(c, stmt->pos,
                               "variable initializer must return a supported value");
            Binding* binding = find_binding(c, stmt->as.let_.ns,
                                            stmt->as.let_.nn);
            if (!binding && c->is_main)
                binding = find_global(c->module, stmt->as.let_.ns,
                                      stmt->as.let_.nn);
            if (binding && binding->is_ffi_module)
                return fail_at(c,stmt->pos,"cannot redeclare a native module alias");
            if (binding && binding->type != value.type &&
                !emit_coerce_type(c,&value,binding->type,stmt->pos))
                return fail_at(c,stmt->pos,
                    "variable '%.*s' changes to an unsupported Native type",
                    (int)stmt->as.let_.nn,stmt->as.let_.ns);
            if (!binding){
                if (c->is_main){
                    binding = add_global_binding(c->module, stmt->as.let_.ns,
                        stmt->as.let_.nn, value.type);
                    if (!binding) return fail_at(c, stmt->pos,
                        "global variable declaration conflicts with its earlier type");
                } else {
                    if (c->binding_count > UINT_MAX)
                        return fail_at(c, stmt->pos,
                                       "too many local variables for native frame");
                    unsigned slot = c->next_slot++;
                    if (!add_binding(c, stmt->as.let_.ns, stmt->as.let_.nn,
                                     slot, value.type) ||
                        !text_appendf(&c->allocas,
                            "  %%slot%u = alloca %s\n  %%init%u = alloca i1\n",
                            slot, llvm_value_type(value.type), slot) ||
                        !text_appendf(&c->initializers,
                            "  store i1 false, ptr %%init%u\n",slot) ||
                        (native_type_is_managed(value.type) &&
                         !text_appendf(&c->initializers,
                            "  store ptr null, ptr %%slot%u\n",slot)))
                        return fail_at(c, stmt->pos,
                                       "out of memory while allocating local variable");
                    binding = find_binding(c, stmt->as.let_.ns, stmt->as.let_.nn);
                }
            }
            if (!binding) return fail_at(c, stmt->pos,
                                         "failed to bind local variable");
            if (c->is_main && binding->is_global &&
                !find_binding(c, stmt->as.let_.ns, stmt->as.let_.nn)){
                if (!add_binding(c, stmt->as.let_.ns, stmt->as.let_.nn,
                                 binding->slot_id, binding->type))
                    return fail_at(c, stmt->pos,
                                   "out of memory while tracking global visibility");
                Binding* visible = find_binding(c, stmt->as.let_.ns,
                                                stmt->as.let_.nn);
                visible->is_global = 1;
            }
            if (binding->boxed){
                if (!emit_cell_store(c,binding,value,stmt->pos,1))return 0;
            } else if (binding->is_global){
                const char* llvm_type = llvm_value_type(value.type);
                if (!text_appendf(&c->text,
                    "  store %s %%v%u, ptr @gslot%u\n"
                    "  store i1 true, ptr @ginit%u\n",
                    llvm_type, value.id, binding->slot_id, binding->slot_id))
                    return fail_at(c, stmt->pos, "out of memory while initializing global");
            } else {
                const char* llvm_type = llvm_value_type(value.type);
                if (!text_appendf(&c->text,
                    "  store %s %%v%u, ptr %%slot%u\n"
                    "  store i1 true, ptr %%init%u\n",
                    llvm_type, value.id, binding->slot_id, binding->slot_id))
                    return fail_at(c, stmt->pos, "out of memory while initializing local");
            }
        } else if (stmt->kind == STMT_SET){
            NativeValue value;
            Binding* binding = find_binding(c, stmt->as.set.ns, stmt->as.set.nn);
            if (!binding && !c->is_main)
                binding = find_global(c->module, stmt->as.set.ns,
                                      stmt->as.set.nn);
            if (!binding)
                return fail_at(c, stmt->pos, "assignment to unknown variable '%.*s'",
                               (int)stmt->as.set.nn, stmt->as.set.ns);
            if (binding->is_ffi_module)
                return fail_at(c,stmt->pos,"cannot rebind a native module alias");
            if (!emit_expr(c, stmt->as.set.value, &value)) return 0;
            if (!emit_coerce_type(c,&value,binding->type,stmt->pos)) return 0;
            if (!emit_store(c, binding, value, stmt->pos)) return 0;
        } else if (stmt->kind == STMT_SETATTR){
            NativeValue base,value;
            size_t key_id;
            if (!emit_expr(c,stmt->as.sattr.obj,&base) ||
                !emit_coerce_type(c,&base,NATIVE_ANY,stmt->pos) ||
                !emit_string_constant(c->module,stmt->as.sattr.name,
                                      stmt->as.sattr.n,&key_id)) return 0;
            if (!emit_expr(c,stmt->as.sattr.value,&value) ||
                !emit_coerce_type(c,&value,NATIVE_ANY,stmt->pos)) return 0;
            if (!text_appendf(&c->text,
                "  call void @kirton_native_attr_set(ptr %%rt, ptr %%v%u, ptr @.kirton.str.%zu, i64 %zu, ptr %%v%u)\n",
                base.id,key_id,stmt->as.sattr.n,value.id))
                return fail_at(c,stmt->pos,"out of memory lowering attribute assignment");
        } else if (stmt->kind == STMT_SETINDEX){
            NativeValue base,index,value;
            if (!emit_expr(c,stmt->as.sidx.base,&base) ||
                !emit_expr(c,stmt->as.sidx.idx,&index) ||
                !emit_expr(c,stmt->as.sidx.value,&value) ||
                !emit_coerce_type(c,&base,NATIVE_ANY,stmt->pos) ||
                !emit_coerce_type(c,&index,NATIVE_ANY,stmt->pos) ||
                !emit_coerce_type(c,&value,NATIVE_ANY,stmt->pos)) return 0;
            if (!text_appendf(&c->text,
                "  call void @kirton_native_collection_set(ptr %%rt, ptr %%v%u, ptr %%v%u, ptr %%v%u)\n",
                base.id,index.id,value.id))
                return fail_at(c,stmt->pos,"out of memory assigning collection item");
        } else if (stmt->kind == STMT_EXPR){
            NativeValue value;
            if (!emit_expr(c, stmt->as.expr, &value)) return 0;
        } else if (stmt->kind == STMT_PRINT){
            if (c->module->needs_runtime){
                for (size_t argument_index=0;argument_index<stmt->as.print.argc;++argument_index){
                    NativeValue value;
                    if (!emit_expr(c,stmt->as.print.args[argument_index],&value)) return 0;
                    if (argument_index && !text_appendf(&c->text,
                        "  call void @kirton_native_print_space(ptr %%rt)\n"))
                        return fail_at(c,stmt->pos,"out of memory while emitting print separator");
                    if (native_type_is_managed(value.type)){
                        if (!text_appendf(&c->text,
                            "  call void @kirton_native_print_value(ptr %%rt, ptr %%v%u)\n",
                            value.id))
                            return fail_at(c,stmt->pos,"out of memory while emitting string print");
                    } else if (value.type==NATIVE_I64){
                        if (!text_appendf(&c->text,
                            "  call void @kirton_native_print_i64(ptr %%rt, i64 %%v%u)\n",
                            value.id))
                            return fail_at(c,stmt->pos,"out of memory while emitting integer print");
                    } else if (value.type==NATIVE_BOOL){
                        unsigned extended=c->next_value++;
                        if (!text_appendf(&c->text,
                            "  %%v%u = zext i1 %%v%u to i64\n"
                            "  call void @kirton_native_print_bool(ptr %%rt, i64 %%v%u)\n",
                            extended,value.id,extended))
                            return fail_at(c,stmt->pos,"out of memory while emitting boolean print");
                    } else return fail_at(c,stmt->pos,"print value type is not supported");
                }
                if (!text_appendf(&c->text,
                    "  call void @kirton_native_print_end(ptr %%rt)\n"))
                    return fail_at(c,stmt->pos,"out of memory while emitting print end");
            } else {
                if (stmt->as.print.argc != 1)
                    return fail_at(c, stmt->pos, "print requires exactly one argument");
                NativeValue value;
                if (!emit_expr(c, stmt->as.print.args[0], &value)) return 0;
                if (value.type != NATIVE_I64)
                    return fail_at(c, stmt->pos, "print supports integer values only");
                if (!text_appendf(&c->text,
                    "  %%print%u = call i32 (ptr, ...) @printf(ptr @.format, i64 %%v%u)\n",
                    c->next_value++, value.id))
                    return fail_at(c, stmt->pos, "out of memory while emitting LLVM IR");
            }
        } else if (stmt->kind == STMT_IF){
            if (!emit_if(c, stmt)) return 0;
        } else if (stmt->kind == STMT_WHILE){
            if (!emit_while(c, stmt)) return 0;
        } else if (stmt->kind == STMT_FOREACH){
            /* The type pass has already defined the global/local loop target.
               Register visibility only after the sequence expression, matching
               the AST evaluation order. */
            Binding* binding=find_binding(c,stmt->as.fe.it,stmt->as.fe.in);
            if (!binding && c->is_main){
                Binding* global=find_global(c->module,stmt->as.fe.it,stmt->as.fe.in);
                if (!global || !add_binding(c,stmt->as.fe.it,stmt->as.fe.in,
                                             global->slot_id,global->type))
                    return fail_at(c,stmt->pos,"failed to bind foreach global");
                binding=find_binding(c,stmt->as.fe.it,stmt->as.fe.in);
                binding->is_global=1;
            }
            if (!emit_foreach(c,stmt)) return 0;
        } else if (stmt->kind == STMT_BREAK || stmt->kind == STMT_CONTINUE){
            if (!c->loop_kind) return fail_at(c,stmt->pos,
                                              "break/continue outside a loop");
            const char* target=c->loop_kind==1 ?
                (stmt->kind==STMT_CONTINUE ? "while_cond" : "while_end") :
                (stmt->kind==STMT_CONTINUE ? "foreach_step" : "foreach_end");
            if(!emit_native_early_unwind(c,1,stmt->pos))return 0;
            if(!c->block_terminated){
                if (!text_appendf(&c->text,"  br label %%%s%u\n",target,c->loop_block))
                    return fail_at(c,stmt->pos,"out of memory emitting loop control");
                c->block_terminated=1;
            }
        } else if (stmt->kind == STMT_TRY){
            if(!emit_native_try(c,stmt)) return 0;
        } else if (stmt->kind == STMT_RETURN){
            if (!c->function)
                return fail_at(c, stmt->pos,
                               "return from top-level initialization is not supported");
            int expected = c->function->return_type;
            if (stmt->as.ret.value){
                NativeValue value;
                if (!emit_expr(c, stmt->as.ret.value, &value)) return 0;
                if (value.type!=expected &&
                    !emit_coerce_type(c,&value,expected,stmt->pos))
                    return fail_at(c,stmt->pos,
                        "return value type does not match function signature (expected %d, got %d)",
                        expected,value.type);
                if (expected == NATIVE_VOID || expected == NATIVE_UNKNOWN ||
                    value.type != expected)
                    return fail_at(c, stmt->pos,
                                   "return value type does not match function signature (expected %d, got %d)",
                                   expected,value.type);
                if(c->active_try && !emit_try_status_guard(c,stmt->pos))return 0;
                /* Finalizers can reach GC safepoints. Preserve the pending
                   return handle across their execution; otherwise safepoint
                   pruning may release it before frame_return reads it. */
                NativeForeachRoot pending_return={value.id,c->active_foreach_roots};
                if(native_type_is_managed(expected))
                    c->active_foreach_roots=&pending_return;
                int unwound=emit_native_early_unwind(c,0,stmt->pos);
                if(native_type_is_managed(expected))
                    c->active_foreach_roots=pending_return.previous;
                if(!unwound)return 0;
                if(c->block_terminated)continue;
                if (native_type_is_managed(expected) && c->module->needs_runtime){
                    unsigned transferred=c->next_value++;
                    if (!text_appendf(&c->text,
                        "  %%v%u = call ptr @kirton_native_frame_return(ptr %%rt, ptr %%frame, ptr %%v%u)\n"
                        "  ret ptr %%v%u\n",transferred,value.id,transferred))
                        return fail_at(c,stmt->pos,"out of memory while emitting dynamic return");
                } else {
                    if (c->module->needs_runtime &&
                        !text_appendf(&c->text,
                            "  call void @kirton_native_frame_leave(ptr %%rt, ptr %%frame)\n"))
                        return fail_at(c,stmt->pos,"out of memory while emitting frame cleanup");
                    if (!text_appendf(&c->text, "  ret %s %%v%u\n",
                        llvm_value_type(expected), value.id))
                        return fail_at(c, stmt->pos, "out of memory while emitting return");
                }
            } else {
                if(!emit_native_early_unwind(c,0,stmt->pos))return 0;
                if(c->block_terminated)continue;
                if (expected == NATIVE_NIL || expected == NATIVE_ANY){
                    unsigned nil_id=c->next_value++,returned=c->next_value++;
                    if (!text_appendf(&c->text,
                        "  %%v%u = call ptr @kirton_native_nil(ptr %%rt)\n"
                        "  %%v%u = call ptr @kirton_native_frame_return(ptr %%rt, ptr %%frame, ptr %%v%u)\n"
                        "  ret ptr %%v%u\n",
                        nil_id,returned,nil_id,returned))
                        return fail_at(c,stmt->pos,"out of memory while emitting nil return");
                } else if (expected == NATIVE_VOID){
                    if ((c->module->needs_runtime &&
                         !text_appendf(&c->text,
                            "  call void @kirton_native_frame_leave(ptr %%rt, ptr %%frame)\n")) ||
                        !text_appendf(&c->text, "  ret void\n"))
                        return fail_at(c, stmt->pos, "out of memory while emitting return");
                } else return fail_at(c,stmt->pos,"invalid bare return type");
            }
            c->block_terminated = 1;
        } else if (stmt->kind == STMT_FUNCTION){
            /* Hoisted at the function entry, not executed at declaration site. */
        } else {
            return fail_at(c, stmt->pos, "statement kind %d is not supported",
                           (int)stmt->kind);
        }
        if (c->module->needs_runtime && !c->block_terminated &&
            (!emit_gc_safepoint(c,stmt->pos) ||
             !emit_try_status_guard(c,stmt->pos))) return 0;
    }
    return 1;
}

static int emit_program(Compiler* c){
    return hoist_nested_bindings(c,c->program->init,c->program->init_len) &&
           emit_hoisted_functions(c,c->program->init,c->program->init_len) &&
           emit_statements(c,c->program->init,c->program->init_len);
}

static const char* llvm_value_type(int type){
    if (type==NATIVE_BOOL) return "i1";
    if (native_type_is_managed(type)) return "ptr";
    return "i64";
}

static int emit_function_definition(NativeModule* module,
                                    NativeFunction* function,
                                    Text* output){
    Compiler c;
    memset(&c, 0, sizeof(c));
    c.module = module;
    c.program = module->program;
    c.function = function;
    c.error = module->error;
    c.error_capacity = module->error_capacity;
    Text params = {0};
    if (module->needs_runtime && !text_appendf(&params,"ptr %%rt")) goto oom;
    if (function->is_nested &&
        !text_appendf(&params,", ptr %%closure"))goto oom;
    if (module->needs_runtime &&
        !text_appendf(&c.initializers,
            "  %%frame = call ptr @kirton_native_frame_enter(ptr %%rt)\n")) goto oom;
    for (size_t i = 0; i < function->ast->param_count; ++i){
        int type = function->parameter_types[i];
        if (type != NATIVE_I64 && type != NATIVE_BOOL &&
            type != NATIVE_STR && type != NATIVE_NUM &&
            type != NATIVE_NIL && type != NATIVE_ANY){
            free(params.data);
            return fail_at(&c, function->ast->pos,
                           "could not infer native function parameter type");
        }
        if (((i || module->needs_runtime) && !text_appendf(&params, ", ")) ||
            !text_appendf(&params, "%s %%arg%zu", llvm_value_type(type), i))
            goto oom;
        const char* name = function->ast->params_start[i];
        size_t name_len = function->ast->params_len[i];
        Binding* binding = find_binding(&c, name, name_len);
        if (!binding){
            unsigned slot = c.next_slot++;
            if (!add_binding(&c, name, name_len, slot, type)) goto oom;
            binding = find_binding(&c, name, name_len);
            binding->is_parameter = 1;
            if (!text_appendf(&c.allocas,
                "  %%slot%u = alloca %s\n  %%init%u = alloca i1\n",
                slot, llvm_value_type(type), slot) ||
                !text_appendf(&c.initializers,
                    "  store i1 false, ptr %%init%u\n",slot) ||
                (native_type_is_managed(type) &&
                 !text_appendf(&c.initializers,
                    "  store ptr null, ptr %%slot%u\n",slot))) goto oom;
        } else if (binding->type != type){
            free(params.data); free(c.bindings);
            free(c.allocas.data); free(c.initializers.data);
            return fail_at(&c, function->ast->pos,
                "duplicate parameter names with conflicting types are not supported");
        }
        if(binding->boxed){
            unsigned created=c.next_value++;
            if(!text_appendf(&c.initializers,
                "  %%v%u = call ptr @kirton_native_cell_new(ptr %%rt)\n"
                "  store ptr %%v%u, ptr %%slot%u\n",
                created,created,binding->slot_id))goto oom;
            unsigned cell=c.next_value++;
            if(!text_appendf(&c.text,
                "  %%v%u = load ptr, ptr %%slot%u\n"
                "  call void @kirton_native_cell_set(ptr %%rt, ptr %%v%u, ptr %%arg%zu)\n"
                "  store i1 true, ptr %%init%u\n",
                cell,binding->slot_id,cell,i,binding->slot_id))goto oom;
        } else if(!text_appendf(&c.text,
            "  store %s %%arg%zu, ptr %%slot%u\n"
            "  store i1 true, ptr %%init%u\n",
            llvm_value_type(type),i,binding->slot_id,binding->slot_id)) goto oom;
    }
    if (!setup_captures(&c) ||
        !hoist_nested_bindings(&c,function->ast->body,function->ast->body_len) ||
        !infer_statements(&c,function->ast->body,function->ast->body_len)){
        free(params.data); free(c.text.data); free(c.bindings);
        free(c.allocas.data); free(c.initializers.data);
        return 0;
    }
    for (size_t p=0;p<function->ast->param_count;++p){
        for (size_t b=0;b<c.binding_count;++b){
            Binding* param=&c.bindings[b];
            if (param->is_parameter && param->slot_id==p &&
                param->type!=function->parameter_types[p]){
                free(params.data); free(c.text.data); free(c.bindings);
                free(c.allocas.data); free(c.initializers.data);
                return fail_at(&c,function->ast->pos,
                    "parameter inferred type differs from its Native ABI signature");
            }
        }
    }
    c.next_slot=(unsigned)c.binding_count;
    for (size_t i=0;i<c.binding_count;++i){
        Binding* binding=&c.bindings[i];
        if (binding->is_parameter || binding->is_capture) continue;
        if (!text_appendf(&c.allocas,
            "  %%slot%u = alloca %s\n  %%init%u = alloca i1\n",
            binding->slot_id,llvm_value_type(binding->type),binding->slot_id) ||
            !text_appendf(&c.initializers,
                "  store i1 false, ptr %%init%u\n",binding->slot_id) ||
            (native_type_is_managed(binding->type) &&
             !text_appendf(&c.initializers,
                "  store ptr null, ptr %%slot%u\n",binding->slot_id))) goto oom;
        if(binding->boxed){
            unsigned cell=c.next_value++;
            if(!text_appendf(&c.initializers,
                "  %%v%u = call ptr @kirton_native_cell_new(ptr %%rt)\n"
                "  store ptr %%v%u, ptr %%slot%u\n",
                cell,cell,binding->slot_id))goto oom;
        }
    }
    if (!emit_hoisted_functions(&c,function->ast->body,function->ast->body_len) ||
        !emit_statements(&c, function->ast->body, function->ast->body_len)){
        free(params.data); free(c.text.data); free(c.bindings);
        free(c.allocas.data); free(c.initializers.data);
        return 0;
    }
    if (!c.block_terminated){
        if (function->return_type == NATIVE_NIL || function->return_type == NATIVE_ANY){
            unsigned nil_id=c.next_value++,transferred=c.next_value++;
            if (!text_appendf(&c.text,
                "  %%v%u = call ptr @kirton_native_nil(ptr %%rt)\n"
                "  %%v%u = call ptr @kirton_native_frame_return(ptr %%rt, ptr %%frame, ptr %%v%u)\n"
                "  ret ptr %%v%u\n",
                nil_id,transferred,nil_id,transferred)) goto oom;
        } else if (function->return_type != NATIVE_VOID){
            free(params.data); free(c.text.data); free(c.bindings);
            free(c.allocas.data); free(c.initializers.data);
            return fail_at(&c, function->ast->pos,
                           "value-returning function can fall through");
        }
        if (function->return_type == NATIVE_VOID &&
            ((module->needs_runtime &&
              !text_appendf(&c.text,
                 "  call void @kirton_native_frame_leave(ptr %%rt, ptr %%frame)\n")) ||
             !text_appendf(&c.text, "  ret void\n"))) goto oom;
    }
    if(module->needs_runtime){
        if(!text_appendf(&c.text,
            "function_error%zu:\n"
            "  call void @kirton_native_frame_leave(ptr %%rt, ptr %%frame)\n",
            function->id)) goto oom;
        if(function->return_type==NATIVE_VOID){
            if(!text_appendf(&c.text,"  ret void\n"))goto oom;
        } else if(native_type_is_managed(function->return_type)){
            if(!text_appendf(&c.text,"  ret ptr null\n"))goto oom;
        } else if(function->return_type==NATIVE_BOOL){
            if(!text_appendf(&c.text,"  ret i1 false\n"))goto oom;
        } else if(!text_appendf(&c.text,"  ret i64 0\n"))goto oom;
    }
    int ok = text_appendf(output,
        "define %s @kirton_fn_%zu(%s) {\nentry:\n%s%s%s}\n\n",
        function->return_type == NATIVE_VOID ? "void" :
            llvm_value_type(function->return_type), function->id,
        params.data ? params.data : "",
        c.allocas.data ? c.allocas.data : "",
        c.initializers.data ? c.initializers.data : "",
        c.text.data ? c.text.data : "");
    free(params.data); free(c.text.data); free(c.bindings);
    free(c.allocas.data); free(c.initializers.data);
    if (!ok) return fail_at(&c, function->ast->pos,
                            "out of memory while emitting function");
    return 1;
oom:
    free(params.data); free(c.text.data); free(c.bindings);
    free(c.allocas.data); free(c.initializers.data);
    return fail_at(&c, function->ast->pos,
                   "out of memory while emitting function");
}


/* Module linker: compile user modules as Native closures instead of evaluating
   their AST at runtime. Lazy global slots preserve shared module identity;
   imported user type declarations are explicitly rejected. */
typedef struct {
    char path[1024];
    int loading;
    char* source_file;
    char loader_name[80];
    char default_alias[256];
} NativeModulePath;
typedef struct {
    NativeModulePath paths[128];
    size_t count;
    char* error;
    size_t capacity;
    AstProgram* root;
    AstStmt* cache_initializers[128];
    size_t cache_count;
    int allow_ffi;
} NativeModuleLinker;

static int native_module_error(NativeModuleLinker* linker,AstPos where,const char* reason){
    if(linker->error && linker->capacity)
        snprintf(linker->error,linker->capacity,
                 "native compiler: %s:%d: %s",
                 where.file?where.file:"<module>",where.line,reason);
    return 0;
}

static int native_expand_import_list(NativeModuleLinker* linker,AstProgram* prog);

static int native_expand_single_import(NativeModuleLinker* linker,
                                       AstProgram* owner,AstStmt* statement){
    (void)owner;
    const AstImport* imp=&statement->as.imp;
    char name[512], from[512], path[1024];
    if(!imp->nn || imp->nn>=sizeof(name))
        return native_module_error(linker,statement->pos,"invalid module name");
    memcpy(name,imp->name,imp->nn);name[imp->nn]=0;
    const char* from_path=NULL;
#if KIRTON_AST_IMPORT_HAS_FROM
    if(imp->from_dir_len){
        if(imp->from_dir_len>=sizeof(from))
            return native_module_error(linker,statement->pos,"module search directory is too long");
        memcpy(from,imp->from_dir,imp->from_dir_len);from[imp->from_dir_len]=0;
        from_path=from;
    }
#endif
    if(!from_path && (!strcmp(name,"внешний") || !strcmp(name,"память"))) {
        if(!strcmp(name,"внешний") && !linker->allow_ffi)
            return native_module_error(linker,statement->pos,
                "foreign module requires explicit --allow-ffi");
        /* Keep the AST import for the Native lowering phase. */
        return 1;
    }
    if(!kir_resolve_module_path(name,from_path,path,sizeof(path)))
        return native_module_error(linker,statement->pos,
            "native import requires a resolvable user module file");
    char canonical[1024];
    kir_normalize_module_key(path,canonical,sizeof(canonical));
    for(size_t i=0;i<linker->count;++i){
        NativeModulePath* existing=&linker->paths[i];
        if(strcmp(existing->path,canonical))continue;
        if(existing->loading)
            return native_module_error(linker,statement->pos,"cyclic native module import");
        const char* alias=imp->an?imp->alias:existing->default_alias;
        size_t alias_len=imp->an?imp->an:strlen(existing->default_alias);
        AstStmt* replacement=ast_stmt_let(alias,alias_len,
                  ast_call(ast_var(existing->loader_name,
                                   strlen(existing->loader_name)),NULL,0));
        free((void*)imp->name);free((void*)imp->alias);
#if KIRTON_AST_IMPORT_HAS_FROM
        free((void*)imp->from_dir);
#endif
        statement->kind=replacement->kind;
        statement->as=replacement->as;
        free(replacement);
        return 1;
    }
    if(linker->count>=128)
        return native_module_error(linker,statement->pos,"native module import limit (128) exceeded");
    size_t module_id=linker->count++;
    snprintf(linker->paths[module_id].path,sizeof(linker->paths[module_id].path),"%s",canonical);
    linker->paths[module_id].loading=1;
    size_t length=0;
    char* source=kir_read_text_file(path,&length);
    if(!source)
        return native_module_error(linker,statement->pos,"cannot read module file");
    if(length>8*1024*1024){
        kir_free(source);
        return native_module_error(linker,statement->pos,"module file exceeds 8 MiB limit");
    }
    unsigned old_errors=kir_diag_error_count();
    AstProgram* parsed=parse_program(source,length,path);
    kir_free(source);
    if(!parsed || parsed->parse_errors || kir_diag_error_count()!=old_errors){
        ast_program_destroy(parsed);
        return native_module_error(linker,statement->pos,"imported module parse failed");
    }
    if(parsed->module_name && parsed->module_name_len){
        if(parsed->module_name_len>=sizeof(linker->paths[module_id].default_alias)){
            ast_program_destroy(parsed);
            return native_module_error(linker,statement->pos,"module name is too long");
        }
        memcpy(linker->paths[module_id].default_alias,
               parsed->module_name,parsed->module_name_len);
        linker->paths[module_id].default_alias[parsed->module_name_len]=0;
    }else{
        kir_module_name_from_path(canonical,linker->paths[module_id].default_alias,
                                  sizeof(linker->paths[module_id].default_alias));
    }
    if(!native_expand_import_list(linker,parsed)){
        ast_program_destroy(parsed);return 0;
    }
    /* Imported module functions execute in a private closure environment,
       but their structure/enum constructors are resolved by the Native
       backend's shared type table. Transfer the declarations to the root
       program before inference so IDs, runtime metadata and object identity
       remain stable across module calls. User types are *not* exported as
       dictionary entries (the AST interpreter exports functions only).

       Constructor lookup is scoped to the declaring source file; typed fields
       carry resolved IDs so same-named types in separate modules are distinct. */
    for(size_t t=0;t<parsed->type_count;++t)
        ast_program_add_type(linker->root,parsed->types[t]);
    free(parsed->types);
    parsed->types=NULL;
    parsed->type_count=0;
    char unique[80];
    int un=snprintf(unique,sizeof(unique),"$kirton_native_module_%zu",module_id);
    if(un<=0 || (size_t)un>=sizeof(unique)){
        ast_program_destroy(parsed);
        return native_module_error(linker,statement->pos,"internal module identifier overflow");
    }
    AstFunc* initializer=ast_func(unique,(size_t)un);
    /* Top-level functions in an imported file are visible only inside its
       module environment; only those functions are exported. */
    size_t fn_count=parsed->func_count;
    const char** keys=fn_count?(const char**)calloc(fn_count,sizeof(char*)):NULL;
    size_t* key_lens=fn_count?(size_t*)calloc(fn_count,sizeof(size_t)):NULL;
    AstExpr** values=fn_count?(AstExpr**)calloc(fn_count,sizeof(AstExpr*)):NULL;
    if(fn_count && (!keys || !key_lens || !values)){
        free(keys);free(key_lens);free(values);
        ast_program_destroy(parsed);
        return native_module_error(linker,statement->pos,"out of memory");
    }
    for(size_t i=0;i<fn_count;++i){
        AstFunc* fn=parsed->funcs[i];
        keys[i]=fn->name_start;key_lens[i]=fn->name_len;
        values[i]=ast_var(fn->name_start,fn->name_len);
        ast_func_push(initializer,ast_stmt_function(fn));
    }
    for(size_t i=0;i<parsed->init_len;++i)
        ast_func_push(initializer,parsed->init[i]);
    ast_func_push(initializer,
                  ast_stmt_return(ast_dict(keys,key_lens,values,fn_count)));
    /* Functions/statements have been transferred into initializer. Keep only
       the source filename alive through IR emission (AstPos references it). */
    free(parsed->funcs);parsed->funcs=NULL;parsed->func_count=0;
    free(parsed->init);parsed->init=NULL;parsed->init_len=0;
    /* Filename owns the positions within the transferred AST. We retain it
       until after lowering by keeping its allocation in the path table. */
    ast_program_init_push(linker->root,ast_stmt_function(initializer));
    /* All imports of this path use one lazy initializer through a root cache.
       This preserves one-time side effects, even when separate modules import
       the same dependency. The check executes at each import statement. */
    char loader_name[80],cache_name[80];
    int ln=snprintf(loader_name,sizeof(loader_name),"$kirton_native_load_%zu",module_id);
    int cn=snprintf(cache_name,sizeof(cache_name),"$kirton_native_cache_%zu",module_id);
    if(ln<=0 || cn<=0 || (size_t)ln>=sizeof(loader_name) ||
       (size_t)cn>=sizeof(cache_name))
        return native_module_error(linker,statement->pos,"module cache identifier overflow");
    snprintf(linker->paths[module_id].loader_name,
             sizeof(linker->paths[module_id].loader_name),"%s",loader_name);
    AstFunc* loader=ast_func(loader_name,(size_t)ln);
    AstStmt* guard=ast_stmt_if(ast_bin("==",ast_var(cache_name,(size_t)cn),ast_nil()));
    ast_if_then_push(guard,ast_stmt_set(cache_name,(size_t)cn,
            ast_call(ast_var(unique,(size_t)un),NULL,0)));
    ast_func_push(loader,guard);
    ast_func_push(loader,ast_stmt_return(ast_var(cache_name,(size_t)cn)));
    ast_program_init_push(linker->root,ast_stmt_function(loader));
    linker->cache_initializers[linker->cache_count++]=
        ast_stmt_let(cache_name,(size_t)cn,ast_nil());
    const char* alias=imp->alias;
    size_t alias_len=imp->an;
    if(!alias_len){
        alias=linker->paths[module_id].default_alias;
        alias_len=strlen(alias);
    }
    AstExpr* call=ast_call(ast_var(loader_name,(size_t)ln),NULL,0);
    AstStmt* replacement=ast_stmt_let(alias,alias_len,call);
    /* Transfer AST ownership without leaking the import string slices. */
    free((void*)imp->name);free((void*)imp->alias);
#if KIRTON_AST_IMPORT_HAS_FROM
    free((void*)imp->from_dir);
#endif
    statement->kind=replacement->kind;
    statement->as=replacement->as;
    free(replacement);
    /* ast_program_destroy releases the untransferred metadata. Transferred
       nodes use file metadata only in diagnostics while the emitter is active. */
    linker->paths[module_id].source_file=(char*)parsed->source_file;
    parsed->source_file=NULL;
    ast_program_destroy(parsed);
    linker->paths[module_id].loading=0;
    return 1;
}

static void native_module_linker_release(NativeModuleLinker* linker){
    /* The AST tree owns transferred nodes and their AstPos.file strings.
       Preserve those filenames until ast_program_destroy(), including when
       the emitter is called again on the same parsed AstProgram. */
    for(size_t i=0;i<linker->count;++i){
        char* path=linker->paths[i].source_file;
        if(!path) continue;
        AstProgram* root=linker->root;
        size_t count=root->native_linked_source_count;
        /* Follow Kirton's fatal-OOM allocator contract: freeing `path` on
           realloc failure would leave transferred AstPos.file pointers
           dangling inside the root AST, causing a later use-after-free. */
        if(count>=SIZE_MAX/sizeof(char*))
            kir_panic_overflow(count,sizeof(char*));
        char** more=(char**)kir_realloc(root->native_linked_sources,
                                      (count+1)*sizeof(char*));
        root->native_linked_sources=more;
        root->native_linked_sources[count]=path;
        root->native_linked_source_count=count+1;
        linker->paths[i].source_file=NULL;
    }
}

static int native_expand_statement_list(NativeModuleLinker* linker,
                                        AstProgram* owner,AstStmt** stmts,size_t count){
    for(size_t i=0;i<count;++i){
        AstStmt* stmt=stmts[i];
        if(!stmt)continue;
        switch(stmt->kind){
        case STMT_IMPORT:
            if(!native_expand_single_import(linker,owner,stmt))return 0;
            break;
        case STMT_IF:
            if(!native_expand_statement_list(linker,owner,stmt->as.if_.then_b,
                                             stmt->as.if_.then_len) ||
               !native_expand_statement_list(linker,owner,stmt->as.if_.else_b,
                                             stmt->as.if_.else_len))return 0;
            break;
        case STMT_WHILE:
            if(!native_expand_statement_list(linker,owner,stmt->as.wh.body,
                                             stmt->as.wh.body_len))return 0;
            break;
        case STMT_FOREACH:
            if(!native_expand_statement_list(linker,owner,stmt->as.fe.body,
                                             stmt->as.fe.body_len))return 0;
            break;
        case STMT_FUNCTION:
            if(!native_expand_statement_list(linker,owner,stmt->as.function->body,
                                             stmt->as.function->body_len))return 0;
            break;
        case STMT_TRY:
            if(!native_expand_statement_list(linker,owner,stmt->as.try_.body,
                                             stmt->as.try_.body_len) ||
               !native_expand_statement_list(linker,owner,stmt->as.try_.catch_body,
                                             stmt->as.try_.catch_body_len) ||
               !native_expand_statement_list(linker,owner,stmt->as.try_.finally_body,
                                             stmt->as.try_.finally_body_len))return 0;
            break;
        default:break;
        }
    }
    return 1;
}

static int native_expand_import_list(NativeModuleLinker* linker,AstProgram* prog){
    size_t initial=prog->init_len;
    /* Loading a module appends synthetic Native functions to the root init
       array and can realloc it. Never retain an interior pointer to init. */
    for(size_t i=0;i<initial;++i){
        AstStmt* item=prog->init[i];
        if(!native_expand_statement_list(linker,prog,&item,1))return 0;
    }
    for(size_t i=0;i<prog->func_count;++i)
        if(!native_expand_statement_list(linker,prog,prog->funcs[i]->body,
                                         prog->funcs[i]->body_len))return 0;
    return 1;
}

int kirton_llvm_emit_options(AstProgram* program,char** out_ir,
                             int* out_requires_runtime,int allow_ffi,
                             char* error,size_t error_capacity){
    if (out_ir) *out_ir = NULL;
    if (out_requires_runtime) *out_requires_runtime=0;
    if (!program || !out_ir) return 0;
    if (error && error_capacity) error[0] = '\0';
    NativeModule module;
    memset(&module, 0, sizeof(module));
    module.program = program;
    module.allow_ffi=allow_ffi;
    module.error = error;
    module.error_capacity = error_capacity;
    NativeModuleLinker* linker=(NativeModuleLinker*)calloc(1,sizeof(*linker));
    if (!linker){
        if(error && error_capacity)
            snprintf(error,error_capacity,
                     "native compiler: out of memory while preparing module linker");
        return 0;
    }
    linker->error=error;linker->capacity=error_capacity;linker->root=program;
    linker->allow_ffi=allow_ffi;
    if (!native_expand_import_list(linker,program)){
        native_module_linker_release(linker);
        free(linker);
        return 0;
    }
    /* Cache slots must exist before user code can invoke a module loader.
       Prepending them avoids eager module initialization and preserves imports'
       original execution order. */
    if(linker->cache_count){
        size_t old_count=program->init_len;
        if(old_count>SIZE_MAX/sizeof(AstStmt*)-linker->cache_count) goto oom;
        AstStmt** grown=(AstStmt**)realloc(program->init,
            (old_count+linker->cache_count)*sizeof(AstStmt*));
        if(!grown) goto oom;
        program->init=grown;
        memmove(program->init+linker->cache_count,program->init,
                old_count*sizeof(AstStmt*));
        memcpy(program->init,linker->cache_initializers,
               linker->cache_count*sizeof(AstStmt*));
        program->init_len=old_count+linker->cache_count;
    }
    if (!collect_functions(&module)) goto oom;
    if (program->type_count) module.needs_runtime=1;
    if (!infer_module_types(&module)) goto fail;

    Text globals = {0};
    for (size_t i = 0; i < module.global_count; ++i){
        Binding* global = &module.globals[i];
        if (global->type != NATIVE_I64 && global->type != NATIVE_BOOL &&
            global->type != NATIVE_STR && global->type != NATIVE_NUM &&
            global->type != NATIVE_NIL && global->type != NATIVE_ANY){
            if (error && error_capacity)
                snprintf(error, error_capacity,
                         "native compiler: could not infer global variable type");
            free(globals.data);
            goto fail;
        }
        const char* initial=native_type_is_managed(global->type) ? "null" : "0";
        if (!text_appendf(&globals,
            "@gslot%u = internal global %s %s\n"
            "@ginit%u = internal global i1 false\n",
            global->slot_id, llvm_value_type(global->type), initial,
            global->slot_id)){
            free(globals.data);
            goto oom;
        }
    }

    Compiler main_compiler;
    memset(&main_compiler, 0, sizeof(main_compiler));
    main_compiler.program = program;
    main_compiler.module = &module;
    main_compiler.is_main = 1;
    main_compiler.error = error;
    main_compiler.error_capacity = error_capacity;
    int ok = emit_program(&main_compiler);
    if (!ok){
        free(main_compiler.text.data); free(main_compiler.bindings);
        free(main_compiler.allocas.data); free(main_compiler.initializers.data);
        free(globals.data);
        goto fail;
    }

    Text type_init={0};
    if(allow_ffi && module.needs_runtime &&
       !text_appendf(&type_init,
            "  call void @kirton_native_ffi_enable(ptr %%rt)\n"))goto oom;
    for(size_t t=0;t<program->type_count;++t){
        AstUserType* type=program->types[t];
        size_t type_name_id;
        if(!emit_string_constant(&module,type->name,type->name_len,&type_name_id) ||
           !text_appendf(&type_init,
               "  call void @kirton_native_type_define(ptr %%rt, i64 %zu, i32 %d, ptr @.kirton.str.%zu, i64 %zu, i64 %zu)\n",
               t,type->kind==AST_TYPE_STRUCT?0:1,type_name_id,type->name_len,
               type->kind==AST_TYPE_STRUCT?type->field_count:0)){
            free(type_init.data);free(globals.data);
            free(main_compiler.text.data);free(main_compiler.bindings);
            free(main_compiler.allocas.data);free(main_compiler.initializers.data);
            goto oom;
        }
        if(type->kind==AST_TYPE_STRUCT) for(size_t f=0;f<type->field_count;++f){
            AstTypeField* field=&type->fields[f];
            size_t name_id,field_type_id,referenced_id=0;
            int has_reference = find_user_type(&module,type->pos,
                field->type_name,field->type_name_len,AST_TYPE_STRUCT,
                &referenced_id)!=NULL ||
                find_user_type(&module,type->pos,field->type_name,
                field->type_name_len,AST_TYPE_ENUM,&referenced_id)!=NULL;
            if(!emit_string_constant(&module,field->name,field->name_len,&name_id) ||
               !emit_string_constant(&module,field->type_name,field->type_name_len,&field_type_id) ||
               !text_appendf(&type_init,
                   "  call void @kirton_native_type_field(ptr %%rt, i64 %zu, i64 %zu, ptr @.kirton.str.%zu, i64 %zu, ptr @.kirton.str.%zu, i64 %zu, i64 %zu)\n",
                   t,f,name_id,field->name_len,field_type_id,field->type_name_len,
                   has_reference ? referenced_id+1 : 0)){
                free(type_init.data);free(globals.data);
                free(main_compiler.text.data);free(main_compiler.bindings);
                free(main_compiler.allocas.data);free(main_compiler.initializers.data);
                goto oom;
            }
        }
    }
    for(size_t i=0;i<module.function_count;++i){
        NativeFunction* fn=&module.functions[i];
        if(fn->is_nested && !text_appendf(&type_init,
            "  call void @kirton_native_function_register(ptr %%rt, i64 %zu, i64 %zu, ptr @kirton_native_bridge_%zu)\n",
            fn->id,fn->ast->param_count,fn->id)){
            free(type_init.data);free(globals.data);
            free(main_compiler.text.data);free(main_compiler.bindings);
            free(main_compiler.allocas.data);free(main_compiler.initializers.data);
            goto oom;
        }
    }
    Text ir = {0};
    const char* runtime_declarations=module.needs_runtime ?
        "declare void @kirton_native_function_register(ptr, i64, i64, ptr)\n"
        "declare ptr @kirton_native_function_new(ptr, i64)\n"
        "declare ptr @kirton_native_function_new_captured(ptr, i64, ptr, i64)\n"
        "declare ptr @kirton_native_cell_new(ptr)\n"
        "declare ptr @kirton_native_cell_get(ptr, ptr)\n"
        "declare void @kirton_native_cell_set(ptr, ptr, ptr)\n"
        "declare ptr @kirton_native_capture_cell(ptr, ptr, i64)\n"
        "declare ptr @kirton_native_function_call(ptr, ptr, ptr, i64)\n"
        "declare ptr @kirton_native_runtime_new()\n"
        "declare void @kirton_native_ffi_enable(ptr)\n"
        "declare ptr @kirton_native_ffi_invoke(ptr, i32, ptr, i64)\n"
        "declare ptr @kirton_native_memory_call(ptr, ptr, i64, ptr, i64)\n"
        "declare i32 @kirton_native_runtime_destroy(ptr)\n"
        "declare void @kirton_native_runtime_abort(ptr, i32)\n"
        "declare void @kirton_native_try_enter(ptr)\n"
        "declare void @kirton_native_try_leave(ptr)\n"
        "declare i32 @kirton_native_error_pending(ptr)\n"
        "declare i32 @kirton_native_error_suspend(ptr)\n"
        "declare void @kirton_native_error_resume(ptr, i32)\n"
        "declare ptr @kirton_native_error_take(ptr)\n"
        "declare ptr @kirton_native_frame_enter(ptr)\n"
        "declare void @kirton_native_frame_leave(ptr, ptr)\n"
        "declare ptr @kirton_native_frame_return(ptr, ptr, ptr)\n"
        "declare ptr @kirton_native_root_value(ptr, ptr)\n"
        "declare ptr @kirton_native_nil(ptr)\n"
        "declare ptr @kirton_native_string(ptr, ptr, i64)\n"
        "declare ptr @kirton_native_number(ptr, ptr, i64, i32)\n"
        "declare ptr @kirton_native_i64(ptr, i64)\n"
        "declare ptr @kirton_native_bool(ptr, i64)\n"
        "declare ptr @kirton_native_list_new(ptr)\n"
        "declare ptr @kirton_native_dict_new(ptr)\n"
        "declare void @kirton_native_type_define(ptr, i64, i32, ptr, i64, i64)\n"
        "declare void @kirton_native_type_field(ptr, i64, i64, ptr, i64, ptr, i64, i64)\n"
        "declare ptr @kirton_native_struct_construct(ptr, i64, ptr, ptr, ptr, i64)\n"
        "declare ptr @kirton_native_enum_constant(ptr, i64, ptr, i64)\n"
        "declare ptr @kirton_native_attr_get(ptr, ptr, ptr, i64)\n"
        "declare void @kirton_native_attr_set(ptr, ptr, ptr, i64, ptr)\n"
        "declare void @kirton_native_list_append(ptr, ptr, ptr)\n"
        "declare void @kirton_native_dict_put_literal(ptr, ptr, ptr, i64, ptr)\n"
        "declare ptr @kirton_native_collection_index(ptr, ptr, ptr)\n"
        "declare ptr @kirton_native_iter_next(ptr, ptr, ptr)\n"
        "declare ptr @kirton_native_collection_builtin(ptr, i32, ptr, i64)\n"
        "declare void @kirton_native_collection_set(ptr, ptr, ptr, ptr)\n"
        "declare ptr @kirton_native_dynamic_add(ptr, ptr, ptr)\n"
        "declare ptr @kirton_native_dynamic_numeric(ptr, i32, ptr, ptr)\n"
        "declare ptr @kirton_native_dynamic_compare(ptr, i32, ptr, ptr)\n"
        "declare ptr @kirton_native_dynamic_neg(ptr, ptr)\n"
        "declare i32 @kirton_native_equal(ptr, ptr, ptr)\n"
        "declare ptr @kirton_native_string_concat(ptr, ptr, ptr)\n"
        "declare i32 @kirton_native_string_equal(ptr, ptr, ptr)\n"
        "declare ptr @kirton_native_hold_value(ptr, ptr)\n"
        "declare ptr @kirton_native_numeric_binary(ptr, i32, ptr, ptr)\n"
        "declare ptr @kirton_native_numeric_neg(ptr, ptr)\n"
        "declare ptr @kirton_native_numeric_bitnot(ptr, ptr)\n"
        "declare i32 @kirton_native_numeric_compare(ptr, i32, ptr, ptr)\n"
        "declare void @kirton_native_print_value(ptr, ptr)\n"
        "declare void @kirton_native_print_i64(ptr, i64)\n"
        "declare void @kirton_native_print_bool(ptr, i64)\n"
        "declare void @kirton_native_print_space(ptr)\n"
        "declare void @kirton_native_print_end(ptr)\n"
        "declare i32 @kirton_native_truthy(ptr, ptr)\n"
        "declare void @kirton_native_safepoint(ptr, ptr, i64)\n" : "";
    const char* runtime_init=module.needs_runtime ?
        "  %rt = call ptr @kirton_native_runtime_new()\n"
        "  %rtbad = icmp eq ptr %rt, null\n"
        "  br i1 %rtbad, label %runtime_failed, label %runtime_start\n"
        "runtime_failed:\n"
        "  ret i32 70\n"
        "runtime_start:\n" : "";
    const char* runtime_finish=module.needs_runtime ?
        "  %runtime_status = call i32 @kirton_native_runtime_destroy(ptr %rt)\n"
        "  ret i32 %runtime_status\n"
        "main_error:\n"
        "  %runtime_error_status = call i32 @kirton_native_runtime_destroy(ptr %rt)\n"
        "  ret i32 %runtime_error_status\n" : "  ret i32 0\n";
    ok = text_appendf(&ir,
        "; Kirton Native Compiler (experimental) — generated from parsed AST\n"
        "@.format = private unnamed_addr constant [6 x i8] c\"%%lld\\0A\\00\", align 1\n"
        "@.uninitialized_message = private unnamed_addr constant [31 x i8] c\"Kirton: uninitialized variable\\00\", align 1\n"
        "declare i32 @printf(ptr, ...)\n"
        "declare i32 @puts(ptr)\n"
        "declare void @exit(i32) noreturn\n"
        "%s%s\n"
        "define i32 @main() {\n"
        "entry:\n%s%s%s%s%s%s}\n\n",
        globals.data ? globals.data : "",
        runtime_declarations,
        runtime_init,
        main_compiler.allocas.data ? main_compiler.allocas.data : "",
        type_init.data ? type_init.data : "",
        main_compiler.initializers.data ? main_compiler.initializers.data : "",
        main_compiler.text.data ? main_compiler.text.data : "",
        runtime_finish);
    free(type_init.data);
    free(globals.data);
    free(main_compiler.text.data); free(main_compiler.bindings);
    free(main_compiler.allocas.data); free(main_compiler.initializers.data);
    if (!ok){ free(ir.data); goto oom; }

    for (size_t i = 0; i < module.function_count; ++i){
        NativeFunction* function = &module.functions[i];
        if (!function->is_nested && find_function(&module, function->ast->name_start,
                          function->ast->name_len) != function) continue;
        if (!emit_function_definition(&module, function, &ir)){
            free(ir.data);
            goto fail;
        }
        if(function->is_nested){
            if(!text_appendf(&ir,
                "define ptr @kirton_native_bridge_%zu(ptr %%rt, ptr %%closure, ptr %%argv) {\nentry:\n",
                function->id)){free(ir.data);goto oom;}
            for(size_t arg=0;arg<function->ast->param_count;++arg){
                if(!text_appendf(&ir,
                    "  %%arg_slot%zu = getelementptr ptr, ptr %%argv, i64 %zu\n"
                    "  %%arg%zu = load ptr, ptr %%arg_slot%zu\n",
                    arg,arg,arg,arg)){free(ir.data);goto oom;}
            }
            if(!text_appendf(&ir,
                "  %%result = call ptr @kirton_fn_%zu(ptr %%rt, ptr %%closure",
                function->id)){free(ir.data);goto oom;}
            for(size_t arg=0;arg<function->ast->param_count;++arg)
                if(!text_appendf(&ir,", ptr %%arg%zu",arg)){
                    free(ir.data);goto oom;
                }
            if(!text_appendf(&ir,")\n  ret ptr %%result\n}\n\n")){
                free(ir.data);goto oom;
            }
        }
    }
    if (module.string_constants.len &&
        !text_appendf(&ir,"%s\n",module.string_constants.data)){
        free(ir.data);
        goto oom;
    }
    *out_ir = ir.data;
    if (out_requires_runtime) *out_requires_runtime=module.needs_runtime;
    for (size_t i = 0; i < module.function_count; ++i) {
        free(module.functions[i].parameter_types);
        free(module.functions[i].captures);
    }
    free(module.functions); free(module.globals);
    free(module.global_definitions.data);
    free(module.string_constants.data);
    native_module_linker_release(linker);
    free(linker);
    return 1;
oom:
    if (error && error_capacity && !error[0])
        snprintf(error, error_capacity,
                 "native compiler: out of memory while emitting LLVM IR");
fail:
    for (size_t i = 0; i < module.function_count; ++i) {
        free(module.functions[i].parameter_types);
        free(module.functions[i].captures);
    }
    free(module.functions); free(module.globals);
    free(module.global_definitions.data);
    free(module.string_constants.data);
    native_module_linker_release(linker);
    free(linker);
    return 0;
}

int kirton_llvm_emit_ex(AstProgram* program,char** out_ir,
                        int* out_requires_runtime,char* error,size_t error_capacity){
    return kirton_llvm_emit_options(program,out_ir,out_requires_runtime,0,error,error_capacity);
}

int kirton_llvm_emit(AstProgram* program,char** out_ir,
                     char* error,size_t error_capacity){
    return kirton_llvm_emit_ex(program,out_ir,NULL,error,error_capacity);
}

static char* native_copy_string(const char* source){
    if(!source || !source[0])return NULL;
    size_t length=strlen(source);
    if(length==SIZE_MAX)return NULL;
    char* copy=(char*)malloc(length+1);
    if(copy)memcpy(copy,source,length+1);
    return copy;
}

#ifdef _WIN32
static char* native_wide_path_to_utf8(const wchar_t* path){
    int length=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,path,-1,
                                   NULL,0,NULL,NULL);
    if(length<=0)return NULL;
    char* result=(char*)malloc((size_t)length);
    if(!result)return NULL;
    if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,path,-1,
                            result,length,NULL,NULL)){
        free(result);
        return NULL;
    }
    return result;
}
#endif

/* Prefer the Runtime archive next to the CLI's installed SDK. The CMake
   build-tree path remains a fallback for ordinary developer builds. */
static char* native_find_runtime_library(void){
#ifdef _WIN32
    const size_t capacity=32768;
    wchar_t* executable=(wchar_t*)malloc(capacity*sizeof(*executable));
    if(executable){
        DWORD length=GetModuleFileNameW(NULL,executable,(DWORD)capacity);
        if(length>0 && (size_t)length<capacity){
            executable[length]=L'\0';
            wchar_t* separator=wcsrchr(executable,L'\\');
            wchar_t* forward=wcsrchr(executable,L'/');
            if(forward && (!separator || forward>separator))separator=forward;
            if(separator){
                size_t directory_length=(size_t)(separator-executable)+1;
                static const wchar_t* candidates[]={
                    L"..\\lib\\kirton.lib",L"kirton.lib"
                };
                for(size_t i=0;i<sizeof(candidates)/sizeof(candidates[0]);++i){
                    size_t suffix_length=wcslen(candidates[i]);
                    if(directory_length>capacity-suffix_length-1)continue;
                    wchar_t* candidate=(wchar_t*)malloc(capacity*sizeof(*candidate));
                    if(!candidate)break;
                    memcpy(candidate,executable,directory_length*sizeof(*candidate));
                    memcpy(candidate+directory_length,candidates[i],
                           (suffix_length+1)*sizeof(*candidate));
                    DWORD attributes=GetFileAttributesW(candidate);
                    if(attributes!=INVALID_FILE_ATTRIBUTES &&
                       !(attributes&FILE_ATTRIBUTE_DIRECTORY)){
                        char* result=native_wide_path_to_utf8(candidate);
                        free(candidate);
                        free(executable);
                        if(result)return result;
                        break;
                    }
                    free(candidate);
                }
            }
        }
        free(executable);
    }
#else
    char executable[PATH_MAX+1];
    ssize_t length=readlink("/proc/self/exe",executable,sizeof(executable)-1);
    if(length>0 && (size_t)length<sizeof(executable)-1){
        executable[length]='\0';
        char* separator=strrchr(executable,'/');
        if(separator){
            size_t directory_length=(size_t)(separator-executable);
            static const char* candidates[]={
                "/../lib/libkirton.a","/libkirton.a"
            };
            for(size_t i=0;i<sizeof(candidates)/sizeof(candidates[0]);++i){
                size_t suffix_length=strlen(candidates[i]);
                if(directory_length>SIZE_MAX-suffix_length-1)continue;
                char* candidate=(char*)malloc(directory_length+suffix_length+1);
                if(!candidate)break;
                memcpy(candidate,executable,directory_length);
                memcpy(candidate+directory_length,candidates[i],suffix_length+1);
                if(access(candidate,R_OK)==0)return candidate;
                free(candidate);
            }
        }
    }
#endif
    return native_copy_string(KIRTON_RUNTIME_LIBRARY_PATH);
}

#ifdef _WIN32
static wchar_t* utf8_to_wide(const char* text){
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t* wide = (wchar_t*)malloc((size_t)n * sizeof(wchar_t));
    if (!wide) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, n)){
        free(wide);
        return NULL;
    }
    return wide;
}

static int native_windows_file_exists(const wchar_t* path){
    DWORD attributes=GetFileAttributesW(path);
    return attributes!=INVALID_FILE_ATTRIBUTES &&
           !(attributes&FILE_ATTRIBUTE_DIRECTORY);
}

static char* native_find_clang_path(void){
    if(KIRTON_CLANG_PATH[0]){
        wchar_t* configured=utf8_to_wide(KIRTON_CLANG_PATH);
        if(configured){
            int exists=native_windows_file_exists(configured);
            char* result=exists?native_wide_path_to_utf8(configured):NULL;
            free(configured);
            if(result)return result;
        }
    }

    DWORD local_required=GetEnvironmentVariableW(L"LOCALAPPDATA",NULL,0);
    if(local_required){
        wchar_t* local=(wchar_t*)malloc((size_t)local_required*sizeof(*local));
        if(local){
            DWORD local_length=GetEnvironmentVariableW(L"LOCALAPPDATA",local,
                                                        local_required);
            if(local_length && local_length<local_required){
                static const wchar_t suffix[]=L"\\Kirton\\LLVM-23.1.5\\bin\\clang.exe";
                size_t local_length_chars=(size_t)local_length;
                size_t suffix_length=sizeof(suffix)/sizeof(suffix[0])-1;
                if(local_length_chars<=SIZE_MAX/sizeof(*local)-suffix_length-1){
                    wchar_t* candidate=(wchar_t*)malloc(
                        (local_length_chars+suffix_length+1)*sizeof(*candidate));
                    if(candidate){
                        memcpy(candidate,local,local_length_chars*sizeof(*candidate));
                        memcpy(candidate+local_length_chars,suffix,
                               (suffix_length+1)*sizeof(*candidate));
                        char* result=native_windows_file_exists(candidate)?
                            native_wide_path_to_utf8(candidate):NULL;
                        free(candidate);
                        free(local);
                        if(result)return result;
                        local=NULL;
                    }
                }
            }
            free(local);
        }
    }

    DWORD path_required=GetEnvironmentVariableW(L"PATH",NULL,0);
    if(path_required){
        wchar_t* path=(wchar_t*)malloc((size_t)path_required*sizeof(*path));
        if(path){
            DWORD path_length=GetEnvironmentVariableW(L"PATH",path,path_required);
            if(path_length && path_length<path_required){
                DWORD capacity=32768;
                wchar_t* candidate=(wchar_t*)malloc((size_t)capacity*sizeof(*candidate));
                if(candidate){
                    DWORD length=SearchPathW(path,L"clang.exe",NULL,capacity,
                                             candidate,NULL);
                    if(length && length<capacity &&
                       native_windows_file_exists(candidate)){
                        char* result=native_wide_path_to_utf8(candidate);
                        free(candidate);
                        free(path);
                        return result;
                    }
                    free(candidate);
                }
            }
            free(path);
        }
    }
    return NULL;
}

static int append_win_arg(wchar_t* dst, size_t capacity, size_t* length,
                          const wchar_t* arg){
    if (*length + 2 >= capacity) return 0;
    dst[(*length)++] = L'"';
    size_t slashes = 0;
    for (const wchar_t* p = arg; ; ++p){
        if (*p == L'\\') { ++slashes; continue; }
        if (*p == L'"'){
            while (slashes--) {
                if (*length + 2 >= capacity) return 0;
                dst[(*length)++] = L'\\';
                dst[(*length)++] = L'\\';
            }
            slashes = 0;
            if (*length + 2 >= capacity) return 0;
            dst[(*length)++] = L'\\';
            dst[(*length)++] = L'"';
            continue;
        }
        if (*p == L'\0'){
            while (slashes--) {
                if (*length + 2 >= capacity) return 0;
                dst[(*length)++] = L'\\';
                dst[(*length)++] = L'\\';
            }
            break;
        }
        while (slashes--) {
            if (*length + 1 >= capacity) return 0;
            dst[(*length)++] = L'\\';
        }
        slashes = 0;
        if (*length + 2 >= capacity) return 0;
        dst[(*length)++] = *p;
    }
    dst[(*length)++] = L'"';
    dst[*length] = L'\0';
    return 1;
}

static int append_win_spaced_arg(wchar_t* dst,size_t capacity,size_t* length,
                                 const wchar_t* arg){
    return (*length+1<capacity) && (dst[(*length)++]=L' ',1) &&
           append_win_arg(dst,capacity,length,arg);
}

static wchar_t* win_prefixed_path(const wchar_t* prefix,const wchar_t* path){
    size_t prefix_len=wcslen(prefix),path_len=wcslen(path);
    if (prefix_len>SIZE_MAX/sizeof(wchar_t)-path_len-1) return NULL;
    wchar_t* result=(wchar_t*)malloc((prefix_len+path_len+1)*sizeof(wchar_t));
    if (!result) return NULL;
    memcpy(result,prefix,prefix_len*sizeof(wchar_t));
    memcpy(result+prefix_len,path,(path_len+1)*sizeof(wchar_t));
    return result;
}

static int run_win_process(const wchar_t* application,wchar_t* command){
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    memset(&startup,0,sizeof(startup));
    memset(&process,0,sizeof(process));
    startup.cb=sizeof(startup);
    if (!CreateProcessW(application,command,NULL,NULL,FALSE,0,NULL,NULL,
                        &startup,&process)) return 0;
    WaitForSingleObject(process.hProcess,INFINITE);
    DWORD code=1;
    int ok=GetExitCodeProcess(process.hProcess,&code) && code==0;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return ok;
}

static int compile_asan_native_windows(const char* llvm_path,
                                      const char* output_path,
                                      const char* clang_path,
                                      const char* runtime_library_path,
                                      char* error,size_t error_capacity){
    size_t output_len=strlen(output_path);
    static const char suffix[]=".kirton-native.obj";
    if (output_len>SIZE_MAX-sizeof(suffix)) return 0;
    char* object_path=(char*)malloc(output_len+sizeof(suffix));
    if (!object_path) return 0;
    memcpy(object_path,output_path,output_len);
    memcpy(object_path+output_len,suffix,sizeof(suffix));

    wchar_t* clang=utf8_to_wide(clang_path);
    wchar_t* linker=utf8_to_wide(KIRTON_NATIVE_ASAN_LINKER_PATH);
    wchar_t* input=utf8_to_wide(llvm_path);
    wchar_t* output=utf8_to_wide(output_path);
    wchar_t* object=utf8_to_wide(object_path);
    wchar_t* runtime_library=utf8_to_wide(runtime_library_path);
    wchar_t* msvc_lib=utf8_to_wide(KIRTON_NATIVE_ASAN_MSVCLIB_DIR);
    wchar_t* ucrt_lib=utf8_to_wide(KIRTON_NATIVE_ASAN_UCRT_DIR);
    wchar_t* um_lib=utf8_to_wide(KIRTON_NATIVE_ASAN_UM_DIR);
    wchar_t* win_libs=utf8_to_wide(KIRTON_NATIVE_ASAN_LINK_LIBS);
    free(object_path);
    if (!clang||!linker||!input||!output||!object||!runtime_library||
        !msvc_lib||!ucrt_lib||!um_lib||!win_libs) goto fail;

    size_t compile_cap=wcslen(clang)+wcslen(input)+wcslen(object)+128;
    wchar_t* compile_cmd=(wchar_t*)malloc(compile_cap*sizeof(wchar_t));
    size_t compile_len=0;
    int compile_built=compile_cmd &&
        append_win_arg(compile_cmd,compile_cap,&compile_len,clang) &&
        append_win_spaced_arg(compile_cmd,compile_cap,&compile_len,L"-Wno-override-module") &&
        append_win_spaced_arg(compile_cmd,compile_cap,&compile_len,L"-fsanitize=address") &&
        append_win_spaced_arg(compile_cmd,compile_cap,&compile_len,L"-fno-sanitize-link-runtime") &&
        append_win_spaced_arg(compile_cmd,compile_cap,&compile_len,L"-c") &&
        append_win_spaced_arg(compile_cmd,compile_cap,&compile_len,input) &&
        append_win_spaced_arg(compile_cmd,compile_cap,&compile_len,L"-o") &&
        append_win_spaced_arg(compile_cmd,compile_cap,&compile_len,object);
    int ok=compile_built && run_win_process(clang,compile_cmd);
    free(compile_cmd);
    if (!ok) goto fail;

    wchar_t* asan_thunk=win_prefixed_path(msvc_lib,L"/clang_rt.asan_dynamic_runtime_thunk-x86_64.lib");
    wchar_t* asan_runtime=win_prefixed_path(msvc_lib,L"/clang_rt.asan_dynamic-x86_64.lib");
    wchar_t* vcasan=win_prefixed_path(msvc_lib,L"/vcasan.lib");
    wchar_t* whole_thunk=asan_thunk?win_prefixed_path(L"/WHOLEARCHIVE:",asan_thunk):NULL;
    wchar_t* msvc_lib_arg=win_prefixed_path(L"/LIBPATH:",msvc_lib);
    wchar_t* ucrt_lib_arg=win_prefixed_path(L"/LIBPATH:",ucrt_lib);
    wchar_t* um_lib_arg=win_prefixed_path(L"/LIBPATH:",um_lib);
    wchar_t* output_arg=win_prefixed_path(L"/Fe:",output);
    if (!asan_thunk||!asan_runtime||!vcasan||!whole_thunk||
        !msvc_lib_arg||!ucrt_lib_arg||!um_lib_arg||!output_arg){
        free(asan_thunk);free(asan_runtime);free(vcasan);free(whole_thunk);
        free(msvc_lib_arg);free(ucrt_lib_arg);free(um_lib_arg);free(output_arg);
        goto fail;
    }
    size_t link_cap=wcslen(linker)+wcslen(output_arg)+wcslen(object)+
        wcslen(runtime_library)+wcslen(asan_runtime)+wcslen(vcasan)+
        wcslen(win_libs)+wcslen(whole_thunk)+wcslen(msvc_lib_arg)+
        wcslen(ucrt_lib_arg)+wcslen(um_lib_arg)+256;
    wchar_t* link_cmd=(wchar_t*)malloc(link_cap*sizeof(wchar_t));
    size_t link_len=0;
    int link_built=link_cmd && append_win_arg(link_cmd,link_cap,&link_len,linker) &&
        append_win_spaced_arg(link_cmd,link_cap,&link_len,L"/nologo") &&
        append_win_spaced_arg(link_cmd,link_cap,&link_len,output_arg) &&
        append_win_spaced_arg(link_cmd,link_cap,&link_len,object) &&
        append_win_spaced_arg(link_cmd,link_cap,&link_len,runtime_library) &&
        append_win_spaced_arg(link_cmd,link_cap,&link_len,asan_runtime) &&
        append_win_spaced_arg(link_cmd,link_cap,&link_len,vcasan);
    if (link_built && win_libs[0]){
        size_t libs_len=wcslen(win_libs);
        link_built=(link_len+libs_len+2<link_cap) &&
            (link_cmd[link_len++]=L' ',1) &&
            (wmemcpy(link_cmd+link_len,win_libs,libs_len),
             link_len+=libs_len,link_cmd[link_len]=L'\0',1);
    }
    link_built=link_built &&
        append_win_spaced_arg(link_cmd,link_cap,&link_len,L"/link") &&
        append_win_spaced_arg(link_cmd,link_cap,&link_len,L"/INFERASANLIBS:NO") &&
        append_win_spaced_arg(link_cmd,link_cap,&link_len,whole_thunk) &&
        append_win_spaced_arg(link_cmd,link_cap,&link_len,L"/OPT:NOREF") &&
        append_win_spaced_arg(link_cmd,link_cap,&link_len,L"/OPT:NOICF") &&
        append_win_spaced_arg(link_cmd,link_cap,&link_len,L"/DEBUG") &&
        append_win_spaced_arg(link_cmd,link_cap,&link_len,L"/SUBSYSTEM:CONSOLE") &&
        append_win_spaced_arg(link_cmd,link_cap,&link_len,msvc_lib_arg) &&
        append_win_spaced_arg(link_cmd,link_cap,&link_len,ucrt_lib_arg) &&
        append_win_spaced_arg(link_cmd,link_cap,&link_len,um_lib_arg);
    ok=link_built && run_win_process(linker,link_cmd);
    free(link_cmd);free(asan_thunk);free(asan_runtime);free(vcasan);
    free(whole_thunk);free(msvc_lib_arg);free(ucrt_lib_arg);free(um_lib_arg);
    free(output_arg);
    DeleteFileW(object);
    free(clang);free(linker);free(input);free(output);free(object);
    free(runtime_library);free(msvc_lib);free(ucrt_lib);free(um_lib);free(win_libs);
    if (!ok && error && error_capacity)
        snprintf(error,error_capacity,"native compiler: MSVC ASan link failed");
    return ok;
fail:
    if (object) DeleteFileW(object);
    free(clang);free(linker);free(input);free(output);free(object);
    free(runtime_library);free(msvc_lib);free(ucrt_lib);free(um_lib);free(win_libs);
    if (error && error_capacity)
        snprintf(error,error_capacity,"native compiler: ASan compile/link setup failed");
    return 0;
}
#endif

#ifndef _WIN32
static char* native_find_clang_path(void){
    if(KIRTON_CLANG_PATH[0] && access(KIRTON_CLANG_PATH,X_OK)==0)
        return native_copy_string(KIRTON_CLANG_PATH);
    const char* environment_path=getenv("PATH");
    if(!environment_path)return NULL;
    const char* entry=environment_path;
    while(*entry){
        const char* end=strchr(entry,':');
        size_t directory_length=end?(size_t)(end-entry):strlen(entry);
        if(directory_length){
            static const char executable[]="clang";
            if(directory_length<=SIZE_MAX-sizeof(executable)-1){
                char* candidate=(char*)malloc(directory_length+1+sizeof(executable));
                if(!candidate)return NULL;
                memcpy(candidate,entry,directory_length);
                candidate[directory_length]='/';
                memcpy(candidate+directory_length+1,executable,sizeof(executable));
                if(access(candidate,X_OK)==0)return candidate;
                free(candidate);
            }
        }
        if(!end)break;
        entry=end+1;
    }
    return NULL;
}
#endif

int kirton_native_compile_ex(const char* llvm_path,const char* output_path,
                             int link_runtime,
                             char* error,size_t error_capacity){
    if (error && error_capacity) error[0] = '\0';
    if (!llvm_path || !output_path) return 0;
    char* clang_path=native_find_clang_path();
    if (!clang_path){
        if (error && error_capacity)
            snprintf(error, error_capacity,
                     "native compiler: Clang was not found when Kirton was configured");
        return 0;
    }
    char* runtime_library_path=link_runtime ? native_find_runtime_library() : NULL;
    if(link_runtime && !runtime_library_path){
        free(clang_path);
        if(error && error_capacity)
            snprintf(error,error_capacity,
                     "native compiler: could not locate the Kirton Runtime library beside the SDK executable or at its configured build path");
        return 0;
    }
#ifdef _WIN32
    if (link_runtime && KIRTON_NATIVE_ASAN_LINK)
    {
        int result=compile_asan_native_windows(llvm_path,output_path,clang_path,
                                               runtime_library_path,
                                               error,error_capacity);
        free(clang_path);
        free(runtime_library_path);
        return result;
    }
    wchar_t* clang = utf8_to_wide(clang_path);
    wchar_t* input = utf8_to_wide(llvm_path);
    wchar_t* output = utf8_to_wide(output_path);
    wchar_t* runtime_library = link_runtime ? utf8_to_wide(runtime_library_path) : NULL;
    wchar_t* link_flags = link_runtime ? utf8_to_wide(KIRTON_RUNTIME_LINK_FLAGS) : NULL;
    if (!clang || !input || !output ||
        (link_runtime && (!runtime_library || !link_flags))){
        free(clang); free(input); free(output);
        free(runtime_library); free(link_flags);
        free(clang_path);
        free(runtime_library_path);
        if (error && error_capacity) snprintf(error, error_capacity,
            "native compiler: command path is not valid UTF-8 or memory allocation failed");
        return 0;
    }
    size_t capacity = wcslen(clang) + wcslen(input) + wcslen(output) +
                      (runtime_library ? wcslen(runtime_library) : 0) +
                      (link_flags ? wcslen(link_flags) : 0) + 96;
    wchar_t* command = (wchar_t*)malloc(capacity * sizeof(wchar_t));
    size_t length = 0;
    int built = command && append_win_arg(command, capacity, &length, clang) &&
        (length + 1 < capacity) && (command[length++] = L' ', 1) &&
        append_win_arg(command, capacity, &length, L"-Wno-override-module") &&
        (length + 1 < capacity) && (command[length++] = L' ', 1) &&
        append_win_arg(command, capacity, &length, input);
    if (built && link_runtime)
        built=(length+1<capacity) && (command[length++]=L' ',1) &&
              append_win_arg(command,capacity,&length,runtime_library);
    if (built)
        built=(length+1<capacity) && (command[length++]=L' ',1) &&
              append_win_arg(command,capacity,&length,L"-o") &&
              (length+1<capacity) && (command[length++]=L' ',1) &&
              append_win_arg(command,capacity,&length,output);
    if (built && link_runtime && link_flags[0])
        built=(length+wcslen(link_flags)+2<capacity) &&
              (command[length++]=L' ',1) &&
              (wmemcpy(command+length,link_flags,wcslen(link_flags)),
               length+=wcslen(link_flags),command[length]=L'\0',1);
    int rc = 0;
    if (built){
        STARTUPINFOW startup;
        PROCESS_INFORMATION process;
        memset(&startup, 0, sizeof(startup));
        memset(&process, 0, sizeof(process));
        startup.cb = sizeof(startup);
        if (CreateProcessW(clang, command, NULL, NULL, FALSE, 0,
                           NULL, NULL, &startup, &process)){
            WaitForSingleObject(process.hProcess, INFINITE);
            DWORD code = 1;
            if (GetExitCodeProcess(process.hProcess, &code)) rc = code == 0;
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
    }
    free(command); free(clang); free(input); free(output);
    free(runtime_library); free(link_flags);
    free(clang_path);
    free(runtime_library_path);
    if (!rc && error && error_capacity)
        snprintf(error, error_capacity, "native compiler: Clang failed to compile LLVM IR");
    return rc;
#else
    pid_t child = fork();
    if (child == 0){
        if (link_runtime){
            if (KIRTON_NATIVE_POSIX_SANITIZER_FLAGS[0])
                execl(clang_path,clang_path,
                    "-Wno-override-module",llvm_path,runtime_library_path,
                    "-o",output_path,"-lm","-ldl",
                    KIRTON_NATIVE_POSIX_SANITIZER_FLAGS,(char*)NULL);
            else
                execl(clang_path,clang_path,
                    "-Wno-override-module",llvm_path,runtime_library_path,
                    "-o",output_path,"-lm","-ldl",(char*)NULL);
        }
        else
            execl(clang_path,clang_path,
                "-Wno-override-module",llvm_path,"-o",output_path,(char*)NULL);
        _exit(127);
    }
    int status = 0;
    if (child < 0 || waitpid(child, &status, 0) < 0 ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 0){
        free(clang_path);
        free(runtime_library_path);
        if (error && error_capacity)
            snprintf(error, error_capacity, "native compiler: Clang failed to compile LLVM IR");
        return 0;
    }
    free(clang_path);
    free(runtime_library_path);
    return 1;
#endif
}

int kirton_native_compile(const char* llvm_path,const char* output_path,
                          char* error,size_t error_capacity){
    return kirton_native_compile_ex(llvm_path,output_path,1,error,error_capacity);
}
