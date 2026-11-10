/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
// src/ast.c
#include "kirton/ast.h"
#include "kirton/common.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

#define NEW(T)   (T*)kir_malloc(sizeof(T))

static char* dup_slice(const char* s, size_t n){
    char* r = (char*)kir_malloc(n + 1);
    if (n) memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

static AstExpr* new_expr(AstExprKind k){
    AstExpr* e = NEW(AstExpr);
    memset(e, 0, sizeof(*e));   /* в том числе обнуляем pos */
    e->kind = k;
    return e;
}

static AstStmt* new_stmt(AstStmtKind k){
    AstStmt* s = NEW(AstStmt);
    memset(s, 0, sizeof(*s));   /* pos тоже в нулях */
    s->kind = k;
    return s;
}

/* ------------------------------------------------------------------------- */
/* Expr factories                                                            */
/* ------------------------------------------------------------------------- */

AstExpr* ast_num(double v){
    return ast_numeric(v_num(v));
}

AstExpr* ast_numeric(Value v){
    AstExpr* e = new_expr(AST_NUM);
    e->as.num.value = v;
    return e;
}

AstExpr* ast_bool(bool v){
    AstExpr* e = new_expr(AST_BOOL);
    e->as.boolean.value = v;
    return e;
}

AstExpr* ast_nil(void){
    return new_expr(AST_NIL);
}

AstExpr* ast_str(const char* start, size_t len){
    AstExpr* e = new_expr(AST_STR);
    e->as.str.start = dup_slice(start, len);
    e->as.str.len   = len;
    return e;
}

AstExpr* ast_var(const char* s, size_t n){
    AstExpr* e = new_expr(AST_VAR);
    e->as.var.name_start = dup_slice(s, n);
    e->as.var.name_len   = n;
    return e;
}

AstExpr* ast_unary(const char* op, AstExpr* a){
    AstExpr* e = new_expr(AST_UNARY);
    e->as.unary.op = op;
    e->as.unary.e  = a;
    return e;
}

AstExpr* ast_bin(const char* op, AstExpr* a, AstExpr* b){
    AstExpr* e = new_expr(AST_BIN);
    e->as.bin.op = op;
    e->as.bin.a  = a;
    e->as.bin.b  = b;
    return e;
}

AstExpr* ast_call(AstExpr* callee, AstExpr** args, size_t argc){
    AstExpr* e = new_expr(AST_CALL);
    e->as.call.callee = callee;
    e->as.call.args   = args;
    e->as.call.arg_names=NULL;
    e->as.call.arg_name_lens=NULL;
    e->as.call.argc   = argc;
    return e;
}

AstExpr* ast_list(AstExpr** items, size_t n){
    AstExpr* e = new_expr(AST_LIST);
    e->as.list.items = items;
    e->as.list.len   = n;
    return e;
}

AstExpr* ast_dict(const char** ks, size_t* kn, AstExpr** vs, size_t n){
    AstExpr* e = new_expr(AST_DICT);
    for (size_t i = 0; i < n; i++){
        ks[i] = dup_slice(ks[i], kn[i]);
    }
    e->as.dict.keys_s = ks;
    e->as.dict.keys_n = kn;
    e->as.dict.vals   = vs;
    e->as.dict.len    = n;
    return e;
}

AstExpr* ast_index(AstExpr* base, AstExpr* idx){
    AstExpr* e = new_expr(AST_INDEX);
    e->as.index.base  = base;
    e->as.index.index = idx;
    return e;
}

AstExpr* ast_attr(AstExpr* obj, const char* name, size_t n){
    AstExpr* e = new_expr(AST_ATTR);
    e->as.attr.obj  = obj;
    e->as.attr.name = dup_slice(name, n);
    e->as.attr.n    = n;
    return e;
}

/* ------------------------------------------------------------------------- */
/* Stmt factories                                                            */
/* ------------------------------------------------------------------------- */

AstStmt* ast_stmt_print(AstExpr** args, size_t argc){
    AstStmt* s = new_stmt(STMT_PRINT);
    s->as.print.args = args;
    s->as.print.argc = argc;
    return s;
}

AstStmt* ast_stmt_expr(AstExpr* e){
    AstStmt* s = new_stmt(STMT_EXPR);
    s->as.expr = e;
    return s;
}

AstStmt* ast_stmt_let(const char* ns, size_t nn, AstExpr* v){
    AstStmt* s = new_stmt(STMT_LET);
    s->as.let_.ns    = dup_slice(ns, nn);
    s->as.let_.nn    = nn;
    s->as.let_.value = v;
    return s;
}

AstStmt* ast_stmt_set(const char* ns, size_t nn, AstExpr* v){
    AstStmt* s = new_stmt(STMT_SET);
    s->as.set.ns    = dup_slice(ns, nn);
    s->as.set.nn    = nn;
    s->as.set.value = v;
    return s;
}

AstStmt* ast_stmt_return(AstExpr* v){
    AstStmt* s = new_stmt(STMT_RETURN);
    s->as.ret.value = v;
    return s;
}

AstStmt* ast_stmt_break(void){
    return new_stmt(STMT_BREAK);
}

AstStmt* ast_stmt_continue(void){
    return new_stmt(STMT_CONTINUE);
}

AstStmt* ast_stmt_if(AstExpr* cond){
    AstStmt* s = new_stmt(STMT_IF);
    s->as.if_.cond     = cond;
    s->as.if_.then_b   = NULL;
    s->as.if_.then_len = 0;
    s->as.if_.else_b   = NULL;
    s->as.if_.else_len = 0;
    s->as.if_.has_else = 0;
    return s;
}

void ast_if_then_push(AstStmt* s, AstStmt* st){
    size_t newn = s->as.if_.then_len + 1;
    AstStmt** nb = (AstStmt**)kir_realloc(s->as.if_.then_b,
                                          newn * sizeof(AstStmt*));
    if (!nb) return; /* OOM — просто не добавили строчку */
    s->as.if_.then_b = nb;
    s->as.if_.then_b[s->as.if_.then_len++] = st;
}

void ast_if_else_push(AstStmt* s, AstStmt* st){
    size_t newn = s->as.if_.else_len + 1;
    AstStmt** nb = (AstStmt**)kir_realloc(s->as.if_.else_b,
                                          newn * sizeof(AstStmt*));
    if (!nb) return;
    s->as.if_.else_b = nb;
    s->as.if_.else_b[s->as.if_.else_len++] = st;
    s->as.if_.has_else = 1;
}

AstStmt* ast_stmt_while(AstExpr* cond){
    AstStmt* s = new_stmt(STMT_WHILE);
    s->as.wh.cond     = cond;
    s->as.wh.body     = NULL;
    s->as.wh.body_len = 0;
    return s;
}

void ast_while_push(AstStmt* s_wh, AstStmt* st){
    size_t newn = s_wh->as.wh.body_len + 1;
    AstStmt** nb = (AstStmt**)kir_realloc(s_wh->as.wh.body,
                                          newn * sizeof(AstStmt*));
    if (!nb) return;
    s_wh->as.wh.body = nb;
    s_wh->as.wh.body[s_wh->as.wh.body_len++] = st;
}

AstStmt* ast_stmt_foreach(const char* it, size_t in, AstExpr* seq){
    AstStmt* s = new_stmt(STMT_FOREACH);
    s->as.fe.it      = dup_slice(it, in);
    s->as.fe.in      = in;
    s->as.fe.seq     = seq;
    s->as.fe.body    = NULL;
    s->as.fe.body_len= 0;
    return s;
}

void ast_foreach_push(AstStmt* s_fe, AstStmt* st){
    size_t newn = s_fe->as.fe.body_len + 1;
    AstStmt** nb = (AstStmt**)kir_realloc(s_fe->as.fe.body,
                                          newn * sizeof(AstStmt*));
    if (!nb) return;
    s_fe->as.fe.body = nb;
    s_fe->as.fe.body[s_fe->as.fe.body_len++] = st;
}

AstStmt* ast_stmt_setindex(AstExpr* base, AstExpr* idx, AstExpr* v){
    AstStmt* s = new_stmt(STMT_SETINDEX);
    s->as.sidx.base  = base;
    s->as.sidx.idx   = idx;
    s->as.sidx.value = v;
    return s;
}

AstStmt* ast_stmt_setattr(AstExpr* obj, const char* name, size_t n, AstExpr* v){
    AstStmt* s = new_stmt(STMT_SETATTR);
    s->as.sattr.obj   = obj;
    s->as.sattr.name  = dup_slice(name, n);
    s->as.sattr.n     = n;
    s->as.sattr.value = v;
    return s;
}

/* ----------------------------- Modules ---------------------------------- */

AstStmt* ast_stmt_import(const char* name, size_t nn,
                         const char* alias, size_t an){
    AstStmt* s = new_stmt(STMT_IMPORT);
    s->as.imp.name  = dup_slice(name, nn);
    s->as.imp.nn    = nn;
    s->as.imp.alias = (alias && an) ? dup_slice(alias, an) : NULL;
    s->as.imp.an    = an;
#if KIRTON_AST_IMPORT_HAS_FROM
    s->as.imp.from_dir     = NULL;
    s->as.imp.from_dir_len = 0;
#endif
    return s;
}

#if KIRTON_AST_IMPORT_HAS_FROM
AstStmt* ast_stmt_import_ex(const char* name, size_t nn,
                            const char* alias, size_t an,
                            const char* from_dir, size_t from_dir_len){
    AstStmt* s = new_stmt(STMT_IMPORT);
    s->as.imp.name          = dup_slice(name, nn);
    s->as.imp.nn            = nn;
    s->as.imp.alias         = (alias && an) ? dup_slice(alias, an) : NULL;
    s->as.imp.an            = an;
    s->as.imp.from_dir      = (from_dir && from_dir_len) ? dup_slice(from_dir, from_dir_len) : NULL;
    s->as.imp.from_dir_len  = from_dir_len;
    return s;
}
#endif

AstStmt* ast_stmt_include(const char* path, size_t pn){
    AstStmt* s = new_stmt(STMT_INCLUDE);
    s->as.inc.path = dup_slice(path, pn);
    s->as.inc.pn   = pn;
    return s;
}

AstStmt* ast_stmt_function(AstFunc* function){
    AstStmt* s = new_stmt(STMT_FUNCTION);
    s->as.function = function;
    return s;
}

AstStmt* ast_stmt_try(const char* name,size_t name_len,bool has_catch,bool has_finally){
    AstStmt* s=new_stmt(STMT_TRY);
    memset(&s->as.try_,0,sizeof(s->as.try_));
    s->as.try_.catch_name=has_catch?dup_slice(name,name_len):NULL;
    s->as.try_.catch_name_len=has_catch?name_len:0;
    s->as.try_.has_catch=has_catch;
    s->as.try_.has_finally=has_finally;
    return s;
}

static void ast_try_push(AstStmt*** body,size_t* len,AstStmt* stmt){
    AstStmt** next=(AstStmt**)kir_realloc(*body,(*len+1)*sizeof(**body));
    if (!next) return;
    *body=next;
    (*body)[(*len)++]=stmt;
}
void ast_try_body_push(AstStmt* s,AstStmt* body){
    ast_try_push(&s->as.try_.body,&s->as.try_.body_len,body);
}
void ast_try_catch_push(AstStmt* s,AstStmt* body){
    ast_try_push(&s->as.try_.catch_body,&s->as.try_.catch_body_len,body);
}
void ast_try_finally_push(AstStmt* s,AstStmt* body){
    ast_try_push(&s->as.try_.finally_body,&s->as.try_.finally_body_len,body);
}

/* ------------------------------------------------------------------------- */
/* Func/Program factories                                                    */
/* ------------------------------------------------------------------------- */

AstFunc* ast_func(const char* name_start, size_t name_len){
    AstFunc* f = NEW(AstFunc);
    memset(f, 0, sizeof(*f));
    f->name_start   = dup_slice(name_start, name_len);
    f->name_len     = name_len;
    f->params_start = NULL;
    f->params_len   = NULL;
    f->param_count  = 0;
    f->body         = NULL;
    f->body_len     = 0;
    return f;
}

void ast_func_add_param(AstFunc* f, const char* ns, size_t nn){
    size_t newn = f->param_count + 1;

    const char** new_ps = (const char**)kir_realloc(
        (void*)f->params_start, newn * sizeof(const char*)
    );
    size_t* new_pl = (size_t*)kir_realloc(
        (void*)f->params_len, newn * sizeof(size_t)
    );

    if (!new_ps || !new_pl){
        /* Если что-то одно упало — не портим старые указатели */
        return;
    }

    f->params_start = new_ps;
    f->params_len   = new_pl;

    size_t idx = f->param_count;
    f->params_start[idx] = dup_slice(ns, nn);
    f->params_len[idx]   = nn;
    f->param_count       = newn;
}

void ast_func_push(AstFunc* f, AstStmt* st){
    size_t newn = f->body_len + 1;
    AstStmt** nb = (AstStmt**)kir_realloc(f->body,
                                          newn * sizeof(AstStmt*));
    if (!nb) return;
    f->body = nb;
    f->body[f->body_len++] = st;
}

AstProgram* ast_program(void){
    AstProgram* p = NEW(AstProgram);
    memset(p, 0, sizeof(*p));
    p->init       = NULL;
    p->init_len   = 0;
    p->funcs      = NULL;
    p->func_count = 0;
    return p;
}

AstUserType* ast_user_type(AstUserTypeKind kind,const char* name,size_t name_len){
    AstUserType* type=NEW(AstUserType);
    memset(type,0,sizeof(*type));
    type->kind=kind;
    type->name=dup_slice(name,name_len);
    type->name_len=name_len;
    return type;
}
void ast_user_type_add_field(AstUserType* type,const char* name,size_t name_len,
                             const char* type_name,size_t type_name_len){
    size_t count=type->field_count+1;
    AstTypeField* fields=(AstTypeField*)kir_realloc(type->fields,count*sizeof(*fields));
    if (!fields) return;
    type->fields=fields;
    fields[type->field_count++]=(AstTypeField){dup_slice(name,name_len),name_len,
        dup_slice(type_name,type_name_len),type_name_len,0};
}
void ast_user_type_add_variant(AstUserType* type,const char* name,size_t name_len){
    size_t count=type->variant_count+1;
    const char** variants=(const char**)kir_realloc((void*)type->variants,count*sizeof(*variants));
    size_t* lens=(size_t*)kir_realloc(type->variant_lens,count*sizeof(*lens));
    if (!variants || !lens) return;
    type->variants=variants;
    type->variant_lens=lens;
    variants[type->variant_count]=dup_slice(name,name_len);
    lens[type->variant_count++]=name_len;
}
void ast_program_add_type(AstProgram* p,AstUserType* type){
    size_t count=p->type_count+1;
    AstUserType** types=(AstUserType**)kir_realloc(p->types,count*sizeof(*types));
    if (!types) return;
    p->types=types;
    p->types[p->type_count++]=type;
}

void ast_program_push(AstProgram* p, AstFunc* f){
    size_t newn = p->func_count + 1;
    AstFunc** nf = (AstFunc**)kir_realloc(p->funcs,
                                          newn * sizeof(AstFunc*));
    if (!nf) return;
    p->funcs = nf;
    p->funcs[p->func_count++] = f;
}

void ast_program_init_push(AstProgram* p, AstStmt* st){
    size_t newn = p->init_len + 1;
    AstStmt** ni = (AstStmt**)kir_realloc(p->init,
                                          newn * sizeof(AstStmt*));
    if (!ni) return;
    p->init = ni;
    p->init[p->init_len++] = st;
}

/* ======================== Уничтожение AST ======================== */

void ast_expr_destroy(AstExpr* e){
    if (!e) return;
    switch (e->kind){
        case AST_NUM:
            if (e->as.num.value.is_integer && e->as.num.value.integer_big)
                kir_bigint_free(e->as.num.value.integer_big);
            break;
        case AST_STR: kir_free((void*)e->as.str.start); break;
        case AST_VAR: kir_free((void*)e->as.var.name_start); break;
        case AST_UNARY: ast_expr_destroy(e->as.unary.e); break;
        case AST_BIN:
            ast_expr_destroy(e->as.bin.a);
            ast_expr_destroy(e->as.bin.b);
            break;
        case AST_CALL:
            ast_expr_destroy(e->as.call.callee);
            for (size_t i=0;i<e->as.call.argc;++i)
                ast_expr_destroy(e->as.call.args[i]);
            kir_free(e->as.call.args);
            if (e->as.call.arg_names){
                for (size_t i=0;i<e->as.call.argc;++i)
                    kir_free((void*)e->as.call.arg_names[i]);
            }
            kir_free((void*)e->as.call.arg_names);
            kir_free(e->as.call.arg_name_lens);
            break;
        case AST_LIST:
            for (size_t i=0;i<e->as.list.len;++i)
                ast_expr_destroy(e->as.list.items[i]);
            kir_free(e->as.list.items);
            break;
        case AST_DICT:
            for (size_t i=0;i<e->as.dict.len;++i){
                kir_free((void*)e->as.dict.keys_s[i]);
                ast_expr_destroy(e->as.dict.vals[i]);
            }
            kir_free((void*)e->as.dict.keys_s);
            kir_free(e->as.dict.keys_n);
            kir_free(e->as.dict.vals);
            break;
        case AST_INDEX:
            ast_expr_destroy(e->as.index.base);
            ast_expr_destroy(e->as.index.index);
            break;
        case AST_ATTR:
            ast_expr_destroy(e->as.attr.obj);
            kir_free((void*)e->as.attr.name);
            break;
        case AST_BOOL:
        case AST_NIL: break;
    }
    kir_free(e);
}

static void ast_function_destroy(AstFunc* f);

static void ast_stmt_destroy(AstStmt* s){
    if (!s) return;
    switch (s->kind){
        case STMT_PRINT:
            for (size_t i=0;i<s->as.print.argc;++i)
                ast_expr_destroy(s->as.print.args[i]);
            kir_free(s->as.print.args);
            break;
        case STMT_EXPR: ast_expr_destroy(s->as.expr); break;
        case STMT_LET:
            kir_free((void*)s->as.let_.ns);
            ast_expr_destroy(s->as.let_.value);
            break;
        case STMT_SET:
            kir_free((void*)s->as.set.ns);
            ast_expr_destroy(s->as.set.value);
            break;
        case STMT_RETURN: ast_expr_destroy(s->as.ret.value); break;
        case STMT_IF:
            ast_expr_destroy(s->as.if_.cond);
            for (size_t i=0;i<s->as.if_.then_len;++i)
                ast_stmt_destroy(s->as.if_.then_b[i]);
            for (size_t i=0;i<s->as.if_.else_len;++i)
                ast_stmt_destroy(s->as.if_.else_b[i]);
            kir_free(s->as.if_.then_b);
            kir_free(s->as.if_.else_b);
            break;
        case STMT_WHILE:
            ast_expr_destroy(s->as.wh.cond);
            for (size_t i=0;i<s->as.wh.body_len;++i)
                ast_stmt_destroy(s->as.wh.body[i]);
            kir_free(s->as.wh.body);
            break;
        case STMT_FOREACH:
            kir_free((void*)s->as.fe.it);
            ast_expr_destroy(s->as.fe.seq);
            for (size_t i=0;i<s->as.fe.body_len;++i)
                ast_stmt_destroy(s->as.fe.body[i]);
            kir_free(s->as.fe.body);
            break;
        case STMT_SETINDEX:
            ast_expr_destroy(s->as.sidx.base);
            ast_expr_destroy(s->as.sidx.idx);
            ast_expr_destroy(s->as.sidx.value);
            break;
        case STMT_SETATTR:
            ast_expr_destroy(s->as.sattr.obj);
            kir_free((void*)s->as.sattr.name);
            ast_expr_destroy(s->as.sattr.value);
            break;
        case STMT_IMPORT:
            kir_free((void*)s->as.imp.name);
            kir_free((void*)s->as.imp.alias);
#if KIRTON_AST_IMPORT_HAS_FROM
            kir_free((void*)s->as.imp.from_dir);
#endif
            break;
        case STMT_INCLUDE: kir_free((void*)s->as.inc.path); break;
        case STMT_FUNCTION: ast_function_destroy(s->as.function); break;
        case STMT_TRY:
            for (size_t i=0;i<s->as.try_.body_len;++i) ast_stmt_destroy(s->as.try_.body[i]);
            for (size_t i=0;i<s->as.try_.catch_body_len;++i) ast_stmt_destroy(s->as.try_.catch_body[i]);
            for (size_t i=0;i<s->as.try_.finally_body_len;++i) ast_stmt_destroy(s->as.try_.finally_body[i]);
            kir_free(s->as.try_.body);
            kir_free(s->as.try_.catch_body);
            kir_free(s->as.try_.finally_body);
            kir_free((void*)s->as.try_.catch_name);
            break;
        case STMT_BREAK:
        case STMT_CONTINUE: break;
    }
    kir_free(s);
}

static void ast_function_destroy(AstFunc* f){
    if (!f) return;
    kir_free((void*)f->name_start);
    for (size_t j=0;j<f->param_count;++j)
        kir_free((void*)f->params_start[j]);
    kir_free((void*)f->params_start);
    kir_free(f->params_len);
    for (size_t j=0;j<f->body_len;++j) ast_stmt_destroy(f->body[j]);
    kir_free(f->body);
    kir_free(f);
}

void ast_program_destroy(AstProgram* p){
    if (!p) return;
    for (size_t i=0;i<p->init_len;++i) ast_stmt_destroy(p->init[i]);
    for (size_t i=0;i<p->func_count;++i){
        ast_function_destroy(p->funcs[i]);
    }
    for (size_t i=0;i<p->type_count;++i){
        AstUserType* type=p->types[i];
        kir_free((void*)type->name);
        for (size_t j=0;j<type->field_count;++j){
            kir_free((void*)type->fields[j].name);
            kir_free((void*)type->fields[j].type_name);
        }
        kir_free(type->fields);
        for (size_t j=0;j<type->variant_count;++j)
            kir_free((void*)type->variants[j]);
        kir_free((void*)type->variants);
        kir_free(type->variant_lens);
        kir_free(type);
    }
    kir_free(p->init);
    kir_free(p->funcs);
    kir_free(p->types);
    for (size_t i=0;i<p->native_linked_source_count;++i)
        kir_free(p->native_linked_sources[i]);
    kir_free(p->native_linked_sources);
    kir_free((void*)p->source_file);
    kir_free((void*)p->module_name);
    kir_free(p);
}
