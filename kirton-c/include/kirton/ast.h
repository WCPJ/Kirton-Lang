/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "kirton/token.h"
#include "kirton/value.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Включаем расширенный импорт: import ... из "путь" */
#define KIRTON_AST_IMPORT_HAS_FROM 1

/* ========================= ПОЗИЦИИ В AST =========================
   Минимальная база для нормальной диагностики/стэктрейсов.
   Пока фабрики ast_* НЕ требуют позицию — поля могут оставаться
   незаполненными, но парсер в будущем сможет их выставлять.     */

typedef struct {
    const char* file;   /* UTF-8 путь/метка файла или NULL */
    int         line;   /* 1-based, 0 если неизвестно      */
    int         col;    /* 1-based, 0 если неизвестно      */
} AstPos;

/* ========================= ВЫРАЖЕНИЯ ========================= */

typedef struct AstExpr AstExpr;

typedef enum {
    AST_NUM,
    AST_STR,
    AST_BOOL,
    AST_NIL,
    AST_UNARY,
    AST_BIN,
    AST_VAR,
    AST_CALL,
    AST_LIST,
    AST_DICT,
    AST_INDEX,
    AST_ATTR
} AstExprKind;

typedef struct { Value value; }                              AstNum;
typedef struct { const char* start; size_t len; }           AstStr;
typedef struct { bool value; }                              AstBool;
typedef struct { const char* op; AstExpr* e; }              AstUnary;
typedef struct { const char* op; AstExpr* a; AstExpr* b; }  AstBin;
typedef struct { const char* name_start; size_t name_len; } AstVar;

typedef struct {
    AstExpr** items;
    size_t    len;
} AstList;

typedef struct {
    const char** keys_s;
    size_t*      keys_n;
    AstExpr**    vals;
    size_t       len;
} AstDict;

typedef struct {
    AstExpr* base;
    AstExpr* index;
} AstIndex;

typedef struct {
    AstExpr*    obj;
    const char* name;
    size_t      n;
} AstAttr;

typedef struct {
    AstExpr*  callee;
    AstExpr** args;
    const char** arg_names;
    size_t* arg_name_lens;
    size_t    argc;
} AstCall;

struct AstExpr {
    AstExprKind kind;
    AstPos      pos;    /* позиция в исходнике (может быть не заполнена) */
    union {
        AstNum   num;
        AstStr   str;
        AstBool  boolean;
        AstUnary unary;
        AstBin   bin;
        AstVar   var;
        AstCall  call;
        AstList  list;
        AstDict  dict;
        AstIndex index;
        AstAttr  attr;
    } as;
};

/* ========================= ОПЕРАТОРЫ ========================= */

typedef struct AstStmt AstStmt;

typedef enum {
    STMT_PRINT,
    STMT_EXPR,
    STMT_LET,
    STMT_SET,
    STMT_RETURN,
    STMT_BREAK,
    STMT_CONTINUE,
    STMT_IF,
    STMT_WHILE,
    STMT_FOREACH,
    STMT_SETINDEX,
    STMT_SETATTR,
    STMT_IMPORT,   /* import имя [как псевдоним] [из "путь"] */
    STMT_INCLUDE,  /* включить "путь.кир"                    */
    STMT_FUNCTION, /* вложенное объявление функции */
    STMT_TRY
} AstStmtKind;

typedef struct {
    AstExpr** args;
    size_t    argc;
} AstPrint;

typedef struct {
    const char* ns;
    size_t      nn;
    AstExpr*    value;
} AstLet;

typedef struct {
    const char* ns;
    size_t      nn;
    AstExpr*    value;
} AstSet;

typedef struct {
    AstExpr* value;
} AstReturn;

typedef struct {
    AstExpr*  cond;
    AstStmt** then_b;
    size_t    then_len;
    AstStmt** else_b;
    size_t    else_len;
    int       has_else;
} AstIf;

typedef struct {
    AstExpr*  cond;
    AstStmt** body;
    size_t    body_len;
} AstWhile;

typedef struct {
    const char* it;
    size_t      in;
    AstExpr*    seq;
    AstStmt**   body;
    size_t      body_len;
} AstForeach;

typedef struct {
    AstExpr* base;
    AstExpr* idx;
    AstExpr* value;
} AstSetIndex;

typedef struct {
    AstExpr*    obj;
    const char* name;
    size_t      n;
    AstExpr*    value;
} AstSetAttr;

/* import / include */

typedef struct {
    const char* name;
    size_t      nn;            /* имя модуля */

    const char* alias;
    size_t      an;            /* псевдоним (опц.) */

#if KIRTON_AST_IMPORT_HAS_FROM
    const char* from_dir;
    size_t      from_dir_len;  /* каталог для поиска */
#endif
} AstImport;

typedef struct {
    const char* path;
    size_t      pn;
} AstInclude;

typedef struct AstFunc AstFunc;

typedef struct {
    AstStmt** body; size_t body_len;
    const char* catch_name; size_t catch_name_len;
    AstStmt** catch_body; size_t catch_body_len;
    AstStmt** finally_body; size_t finally_body_len;
    bool has_catch, has_finally;
} AstTry;

struct AstStmt {
    AstStmtKind kind;
    AstPos      pos;   /* позиция начала оператора в исходнике */
    union {
        AstPrint    print;
        AstExpr*    expr;
        AstLet      let_;
        AstSet      set;
        AstReturn   ret;
        AstIf       if_;
        AstWhile    wh;
        AstForeach  fe;
        AstSetIndex sidx;
        AstSetAttr  sattr;
        AstImport   imp;   /* import */
        AstInclude  inc;   /* include */
        AstFunc*    function;
        AstTry      try_;
    } as;
};

/* ========================= ФУНКЦИИ ========================= */

struct AstFunc {
    const char*  name_start;
    size_t       name_len;

    const char** params_start;
    size_t*      params_len;
    size_t       param_count;

    AstStmt**    body;
    size_t       body_len;
    AstPos       pos; /* место объявления функции */
};

typedef enum { AST_TYPE_STRUCT, AST_TYPE_ENUM } AstUserTypeKind;
typedef struct {
    const char* name; size_t name_len;
    const char* type_name; size_t type_name_len;
    /* Native runtime metadata only: resolved referenced type ID + 1.
       Zero means a builtin type or an unresolved name. */
    size_t native_type_id_plus_one;
} AstTypeField;
typedef struct AstUserType {
    AstUserTypeKind kind;
    const char* name; size_t name_len;
    AstTypeField* fields; size_t field_count;
    const char** variants; size_t* variant_lens; size_t variant_count;
    AstPos pos;
} AstUserType;

/* ========================= ПРОГРАММА =========================
   Top-level init — последовательность операторов (import/включить/let/expr ...),
   которая исполняется до любых вызовов пользовательских функций.           */

typedef struct {
    const char* source_file; /* владеет именем файла, общим для AstPos в этом дереве */
    const char* module_name; /* необязательное имя после заголовка «модуль» */
    size_t      module_name_len;

    AstStmt** init;
    size_t    init_len;    /* top-level секция */

    AstFunc** funcs;
    size_t    func_count;

    AstUserType** types;
    size_t type_count;

    /* Owned source filenames retained by the Native module static linker;
       imported statement positions may outlive the linker invocation. */
    char** native_linked_sources;
    size_t native_linked_source_count;

    unsigned int parse_errors; /* диагностики, найденные при разборе программы */
} AstProgram;

/* ========================= ФАБРИКИ ВЫРАЖЕНИЙ =========================
   ВАЖНО: сигнатуры оставлены простыми (без AstPos), чтобы не ломать
   parser.c / eval.c. Позиции можно будет проставлять в ast.c отдельной
   функцией, когда будет нужно.                                             */

AstExpr* ast_num  (double v);
AstExpr* ast_numeric(Value v);
AstExpr* ast_bool (bool v);
AstExpr* ast_nil  (void);
AstExpr* ast_str  (const char* start, size_t len);
AstExpr* ast_var  (const char* s, size_t n);
AstExpr* ast_unary(const char* op, AstExpr* e);
AstExpr* ast_bin  (const char* op, AstExpr* a, AstExpr* b);
AstExpr* ast_call (AstExpr* callee, AstExpr** args, size_t argc);
AstExpr* ast_list (AstExpr** items, size_t n);
AstExpr* ast_dict (const char** ks, size_t* kn, AstExpr** vs, size_t n);
AstExpr* ast_index(AstExpr* base, AstExpr* idx);
AstExpr* ast_attr (AstExpr* obj, const char* name, size_t n);

/* ========================= ФАБРИКИ ОПЕРАТОРОВ ========================= */

AstStmt* ast_stmt_print   (AstExpr** args, size_t argc);
AstStmt* ast_stmt_expr    (AstExpr* e);
AstStmt* ast_stmt_let     (const char* ns, size_t nn, AstExpr* v);
AstStmt* ast_stmt_set     (const char* ns, size_t nn, AstExpr* v);
AstStmt* ast_stmt_return  (AstExpr* v);
AstStmt* ast_stmt_break   (void);
AstStmt* ast_stmt_continue(void);
AstStmt* ast_stmt_if      (AstExpr* cond);
void     ast_if_then_push (AstStmt* s_if, AstStmt* st);
void     ast_if_else_push (AstStmt* s_if, AstStmt* st);
AstStmt* ast_stmt_while   (AstExpr* cond);
void     ast_while_push   (AstStmt* s_wh, AstStmt* st);
AstStmt* ast_stmt_foreach (const char* it, size_t in, AstExpr* seq);
void     ast_foreach_push (AstStmt* s_fe, AstStmt* st);
AstStmt* ast_stmt_setindex(AstExpr* base, AstExpr* idx, AstExpr* v);
AstStmt* ast_stmt_setattr (AstExpr* obj, const char* name, size_t n, AstExpr* v);

/* import / include */
AstStmt* ast_stmt_import  (const char* name, size_t nn,
                           const char* alias, size_t an);
AstStmt* ast_stmt_include (const char* path, size_t pn);
AstStmt* ast_stmt_function(AstFunc* function);
AstStmt* ast_stmt_try(const char* catch_name,size_t catch_name_len,
                      bool has_catch,bool has_finally);
void ast_try_body_push(AstStmt* s,AstStmt* body);
void ast_try_catch_push(AstStmt* s,AstStmt* body);
void ast_try_finally_push(AstStmt* s,AstStmt* body);

/* Расширенный импорт с указанием каталога (совместимо со старым) */
AstStmt* ast_stmt_import_ex(const char* name, size_t nn,
                            const char* alias, size_t an,
                            const char* from_dir, size_t from_dir_len);

/* ========================= ФАБРИКИ ФУНКЦИЙ/ПРОГРАММ ========================= */

AstFunc*    ast_func               (const char* name_start, size_t name_len);
void        ast_func_add_param     (AstFunc* f, const char* ns, size_t nn);
void        ast_func_push          (AstFunc* f, AstStmt* st);

AstProgram* ast_program            (void);
void        ast_program_push       (AstProgram* p, AstFunc* f);
void        ast_program_init_push  (AstProgram* p, AstStmt* st); /* top-level */
AstUserType* ast_user_type(AstUserTypeKind kind,const char* name,size_t name_len);
void ast_user_type_add_field(AstUserType* type,const char* name,size_t name_len,
                             const char* type_name,size_t type_name_len);
void ast_user_type_add_variant(AstUserType* type,const char* name,size_t name_len);
void ast_program_add_type(AstProgram* p,AstUserType* type);
/* Уничтожает дерево и его числовые литералы. AST владеет своими узлами,
   строковыми полями, именем файла и BigInt значениями AST_NUM. */
void        ast_program_destroy    (AstProgram* p);
/* Для парсера: освободить отброшенное выражение или частичное дерево. */
void        ast_expr_destroy       (AstExpr* e);

#ifdef __cplusplus
}
#endif
