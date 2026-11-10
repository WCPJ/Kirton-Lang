/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
// src/parser.c
#include "kirton/parser.h"
#include "kirton/lexer.h"
#include "kirton/token.h"
#include "kirton/ast.h"
#include "kirton/diag.h"
#include "kirton/utf8.h"
#include "kirton/source_cache.h"
#include "kirton/common.h"

#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

/* -----------------------------------------------------------------------
   Примечание к импорту:
   - Если в сборке определён макрос KIRTON_AST_IMPORT_HAS_FROM и есть
     конструктор ast_stmt_import_ex(name, nn, alias, an, from_dir, fn),
     мы прокидываем путь из "из \"...\"" в AST.
   - Иначе — используем старый ast_stmt_import(...) и поведение будет,
     как раньше (путь проигнорируется на уровне AST).
   ----------------------------------------------------------------------- */

/* ----------------------------- Parser state ----------------------------- */

typedef struct {
    KirLexer   lx;
    KirToken   cur, prev;
    const char* file;
    unsigned int loop_depth;
} Parser;

static inline void next(Parser* p){ p->prev = p->cur; p->cur = kir_lexer_next(&p->lx); }
static inline bool check(Parser* p, KirTokenKind k){ return p->cur.kind == k; }
static inline bool match(Parser* p, KirTokenKind k){ if (check(p,k)){ next(p); return true; } return false; }

static int tok_cp_len(const KirToken* t){
    int cps = 0;
    const char* s = t->start; const char* e = t->end; uint32_t cp;
    while (s < e){
        int n = kir_utf8_decode(s, e, &cp);
        if (n <= 0){ s++; cps++; } else { s += n; cps++; }
    }
    if (cps <= 0) cps = 1;
    return cps;
}

static void error_here(Parser* p, const char* fmt, ...){
    int caret = tok_cp_len(&p->cur);
    const char* where = p->cur.file ? p->cur.file : p->file;
    const char* line  = sc_get_line(where, p->cur.line);

    const char* kind_name = kir_token_kind_name(p->cur.kind);

    /* Спец-случай: лексическая ошибка -> даём понятное сообщение. */
    if (kind_name && strcmp(kind_name, "ERROR") == 0){
        kir_diag_error_with_line(
            where,
            p->cur.line,
            p->cur.col,
            line ? line : "",
            line ? caret : 1,
            "%s",
            "Лексическая ошибка (битый UTF-8 или недопустимый символ)"
        );
        return;
    }

    va_list ap; va_start(ap, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    kir_diag_error_with_line(
        where,
        p->cur.line,
        p->cur.col,
        line ? line : "",
        line ? caret : 1,
        "%s",
        buf
    );
}

static void expect(Parser* p, KirTokenKind k, const char* what){
    if (!check(p,k)){
        error_here(p, "Ожидалось %s, найдено %s", what, kir_token_kind_name(p->cur.kind));
    }
    next(p);
}

static bool reject_unmatched_closing_brace(Parser* p, const char* context){
    if (!check(p,TK_RBRACE)) return false;
    error_here(p,"Неожиданная '}' в %s; возможно, перед блоком пропущена '{'",context);
    next(p);
    return true;
}


/* ----------------------------- Expressions ----------------------------- */

static AstExpr* parse_expr(Parser* p); /* fwd */
static AstStmt* parse_stmt(Parser* p); /* fwd */
static AstExpr* parse_bin_rhs(Parser* p, AstExpr* lhs, int min_prec); /* fwd */

static AstExpr* expr_at(AstExpr* expr, const KirToken* token){
    if (expr && token){
        expr->pos.file = token->file;
        expr->pos.line = token->line;
        expr->pos.col = token->col;
    }
    return expr;
}

static AstStmt* stmt_at(AstStmt* stmt, const KirToken* token){
    if (stmt && token){
        stmt->pos.file = token->file;
        stmt->pos.line = token->line;
        stmt->pos.col = token->col;
    }
    return stmt;
}

static char* decode_string_token(const KirToken* token, size_t* out_len){
    const char* raw = token->start + 1;
    size_t raw_len = (size_t)(token->end - token->start - 2);
    char* decoded = (char*)kir_malloc(raw_len + 1);
    size_t out = 0;

    for (size_t i = 0; i < raw_len; ++i){
        if (raw[i] != '\\'){
            decoded[out++] = raw[i];
            continue;
        }

        if (i + 1 >= raw_len){
            kir_diag_error(token->file, token->line, token->col,
                           "Незавершённая escape-последовательность в строке");
            break;
        }

        char escaped = raw[++i];
        switch (escaped){
            case '\\': decoded[out++] = '\\'; break;
            case '"':  decoded[out++] = '"';  break;
            case 'n':  decoded[out++] = '\n'; break;
            case 'r':  decoded[out++] = '\r'; break;
            case 't':  decoded[out++] = '\t'; break;
            default: {
                int col = token->col + 1;
                for (size_t j = 0; j < i - 1; ++j){
                    unsigned char byte = (unsigned char)raw[j];
                    if ((byte & 0xC0) != 0x80) ++col;
                }
                kir_diag_error_with_line(token->file, token->line, col,
                                         sc_get_line(token->file, token->line), 2,
                                         "Неизвестная escape-последовательность");
                decoded[out++] = '\\';
                decoded[out++] = escaped;
                break;
            }
        }
    }

    decoded[out] = '\0';
    if (out_len) *out_len = out;
    return decoded;
}

/* primaries: NUMBER | STRING | ID | '(' expr ')' | '[' list ']' | '{' dict '}' | истина | ложь */
static AstExpr* parse_primary(Parser* p){
    if (match(p, TK_NUMBER)){
        KirToken token = p->prev;
        size_t n = (size_t)(p->prev.end - p->prev.start);
        char* tmp = (char*)kir_malloc(n+1);
        memcpy(tmp, p->prev.start, n);
        tmp[n] = 0;

        bool is_hex = n >= 3 && tmp[0] == '0' && (tmp[1] == 'x' || tmp[1] == 'X');
        bool is_integer = is_hex || memchr(tmp, '.', n) == NULL;
        Value value = is_integer
            ? v_int_parse(tmp, n, is_hex ? 16u : 10u)
            : v_num(strtod(tmp, NULL));

        kir_free(tmp);
        return expr_at(ast_numeric(value), &token);
    }

    if (match(p, TK_STRING)){
        KirToken token = p->prev;
        size_t value_len = 0;
        char* value = decode_string_token(&token, &value_len);
        AstExpr* expr = ast_str(value, value_len);
        kir_free(value);
        return expr_at(expr, &token);
    }
    if (match(p, KW_ISTINA)) return expr_at(ast_bool(true), &p->prev);
    if (match(p, KW_LOZH))   return expr_at(ast_bool(false), &p->prev);
    if (match(p, KW_PUSTO))  return expr_at(ast_nil(), &p->prev);

    if (match(p, TK_LPAREN)){
        AstExpr* e = parse_expr(p);
        expect(p, TK_RPAREN, "')'");
        return e;
    }

    if (match(p, TK_LBRACK)){
        KirToken token = p->prev;
        /* [e1, e2, ...] */
        AstExpr** items = NULL; size_t len=0;
        if (!check(p, TK_RBRACK)){
            for (;;){
                AstExpr* it = parse_expr(p);
                items = (AstExpr**)kir_realloc(items, (len+1)*sizeof(AstExpr*));
                items[len++] = it;
                if (match(p, TK_COMMA)) continue;
                break;
            }
        }
        expect(p, TK_RBRACK, "']'");
        return expr_at(ast_list(items, len), &token);
    }

    if (match(p, TK_LBRACE)){
        KirToken token = p->prev;
        /* { "k": v, ... } — ключи как строки/идентификаторы */
        const char** ks = NULL; size_t* kn = NULL; AstExpr** vs = NULL; char** owned_keys = NULL; size_t len=0;
        if (!check(p, TK_RBRACE)){
            for (;;){
                const char* kstart=NULL; size_t klen=0;
                char* owned_key = NULL;
                if (match(p, TK_STRING)){ owned_key = decode_string_token(&p->prev, &klen); kstart = owned_key; }
                else if (match(p, TK_ID)) { kstart = p->prev.start;   klen = (size_t)(p->prev.end - p->prev.start); }
                else { error_here(p, "Ожидался ключ словаря (строка или идентификатор)"); break; }
                expect(p, TK_COLON, "':'");
                AstExpr* v = parse_expr(p);
                ks = (const char**)kir_realloc((void*)ks, (len+1)*sizeof(char*));
                kn = (size_t*)kir_realloc((void*)kn, (len+1)*sizeof(size_t));
                vs = (AstExpr**)kir_realloc((void*)vs, (len+1)*sizeof(AstExpr*));
                owned_keys = (char**)kir_realloc(owned_keys, (len+1)*sizeof(char*));
                ks[len]=kstart; kn[len]=klen; vs[len]=v; len++;
                owned_keys[len-1] = owned_key;
                if (match(p, TK_COMMA)) continue;
                break;
            }
        }
        expect(p, TK_RBRACE, "'}'");
        AstExpr* dict = ast_dict(ks, kn, vs, len);
        for (size_t i = 0; i < len; ++i) kir_free(owned_keys[i]);
        kir_free(owned_keys);
        return expr_at(dict, &token);
    }

    if (match(p, TK_ID)){
        /* может продолжаться вызовом, индексом или доступом к атрибуту */
        AstExpr* base = ast_var(p->prev.start, (size_t)(p->prev.end - p->prev.start));
        expr_at(base, &p->prev);

        for (;;){
            if (match(p, TK_LPAREN)){
                /* вызов: base(args...) */
                AstExpr** args = NULL; size_t argc=0;
                const char** arg_names=NULL; size_t* arg_name_lens=NULL;
                if (!check(p, TK_RPAREN)){
                    for (;;){
                        AstExpr* a = parse_expr(p);
                        char* arg_name=NULL; size_t arg_name_len=0;
                        if (match(p,TK_COLON)){
                            if (a->kind!=AST_VAR){
                                error_here(p,"Именованный аргумент должен начинаться с имени поля");
                            } else {
                                arg_name=kir_strdupn(a->as.var.name_start,
                                                     a->as.var.name_len);
                                arg_name_len=a->as.var.name_len;
                                ast_expr_destroy(a);
                                a=parse_expr(p);
                            }
                        }
                        args = (AstExpr**)kir_realloc(args, (argc+1)*sizeof(AstExpr*));
                        arg_names=(const char**)kir_realloc((void*)arg_names,(argc+1)*sizeof(char*));
                        arg_name_lens=(size_t*)kir_realloc(arg_name_lens,(argc+1)*sizeof(size_t));
                        args[argc++] = a;
                        arg_names[argc-1]=arg_name;
                        arg_name_lens[argc-1]=arg_name_len;
                        if (match(p, TK_COMMA)) continue;
                        break;
                    }
                }
                expect(p, TK_RPAREN, "')'");
                AstExpr* call = ast_call(base, args, argc);
                call->as.call.arg_names=arg_names;
                call->as.call.arg_name_lens=arg_name_lens;
                call->pos = base->pos;
                base = call;
                continue;
            }
            if (match(p, TK_LBRACK)){
                AstExpr* idx = parse_expr(p);
                expect(p, TK_RBRACK, "']'");
                AstExpr* indexed = ast_index(base, idx);
                indexed->pos = base->pos;
                base = indexed;
                continue;
            }
            if (match(p, TK_DOT)){
                if (!match(p,TK_ID) && !match(p,KW_STRUCT) && !match(p,KW_ENUM)){
                    error_here(p, "Ожидался идентификатор после '.'");
                    break;
                }
                AstExpr* attr = ast_attr(base, p->prev.start, (size_t)(p->prev.end - p->prev.start));
                attr->pos = base->pos;
                base = attr;
                continue;
            }
            break;
        }
        return base;
    }

    error_here(p, "Неожиданный токен в выражении: %s", kir_token_kind_name(p->cur.kind));
    next(p);
    return ast_num(0.0);
}

/* Pratt parser: precedence climbing */
static const char* binary_op_name(KirTokenKind k){
    switch (k){
        case TK_PLUS:  return "+";
        case TK_MINUS: return "-";
        case TK_STAR:  return "*";
        case TK_SLASH: return "/";
        case TK_PERCENT:return "%";
        case TK_AMP:   return "&";
        case TK_PIPE:  return "|";
        case TK_CARET: return "^";
        case TK_SHL:   return "<<";
        case TK_SHR:   return ">>";
        case KW_NATSELO:return "нацело";
        case TK_EQEQ:  return "==";
        case TK_NEQ:   return "!=";
        case TK_LT:    return "<";
        case TK_LE:    return "<=";
        case TK_GT:    return ">";
        case TK_GE:    return ">=";
        case KW_I:     return "&&";
        case KW_ILI:   return "||";
        default:       return NULL;
    }
}

static int precedence_of(KirTokenKind k){
    /* Уровни: умножение, сумма, сдвиги, биты, сравнения и логика. */
    switch (k){
        case TK_STAR:
        case TK_SLASH:
        case TK_PERCENT:
        case KW_NATSELO: return 60;
        case TK_PLUS:
        case TK_MINUS: return 50;
        case TK_SHL:
        case TK_SHR:   return 45;
        case TK_AMP:   return 40;
        case TK_CARET: return 35;
        case TK_PIPE:  return 30;
        case TK_LT:
        case TK_LE:
        case TK_GT:
        case TK_GE:    return 20;
        case TK_EQEQ:
        case TK_NEQ:   return 15;
        case KW_I:     return 10;
        case KW_ILI:   return 5;
        default:       return -1;
    }
}

static AstExpr* parse_unary(Parser* p){
    KirToken op = p->cur;
    if (match(p, KW_NE)){ /* 'не' как логическое отрицание */
        AstExpr* e = parse_unary(p);
        return expr_at(ast_unary("!", e), &op);
    }
    if (match(p, TK_MINUS)){
        AstExpr* e = parse_unary(p);
        return expr_at(ast_unary("-", e), &op);
    }
    if (match(p, TK_TILDE)){
        AstExpr* e = parse_unary(p);
        return expr_at(ast_unary("~", e), &op);
    }
    return parse_primary(p);
}

static AstExpr* parse_bin_rhs(Parser* p, AstExpr* lhs, int min_prec){
    for (;;){
        KirToken op = p->cur;
        const char* opstr = binary_op_name(op.kind);
        int prec = precedence_of(op.kind);
        if (!opstr) break;

        /* Проверяем приоритет до сдвига: иначе вложенный разбор съедает
           следующий оператор с более низким приоритетом. */
        if (prec < min_prec) break;
        next(p);

        AstExpr* rhs = parse_unary(p);

        /* Операторы с равным приоритетом остаются в цикле и связываются слева. */
        int next_prec = precedence_of(p->cur.kind);
        if (next_prec > prec){
            rhs = parse_bin_rhs(p, rhs, prec+1);
        }

        lhs = expr_at(ast_bin(opstr, lhs, rhs), &op);
    }
    return lhs;
}

static AstExpr* parse_expr(Parser* p){
    AstExpr* lhs = parse_unary(p);
    return parse_bin_rhs(p, lhs, 0);
}

/* ----------------------------- Statements ----------------------------- */

static AstStmt* parse_stmt_let(Parser* p){
    /* пусть name = expr */
    if (!match(p, TK_ID)){ error_here(p, "После 'пусть' требуется имя"); return ast_stmt_expr(ast_num(0)); }
    const char* ns = p->prev.start; size_t nn = (size_t)(p->prev.end - p->prev.start);
    expect(p, TK_EQ, "'='");
    AstExpr* v = parse_expr(p);
    return ast_stmt_let(ns, nn, v);
}

static AstStmt* parse_stmt_print(Parser* p){
    /* печатать(expr, expr, ...) */
    expect(p, TK_LPAREN, "'('");
    AstExpr** args=NULL; size_t argc=0;
    if (!check(p, TK_RPAREN)){
        for(;;){
            AstExpr* a = parse_expr(p);
            args = (AstExpr**)kir_realloc(args, (argc+1)*sizeof(AstExpr*));
            args[argc++]=a;
            if (match(p, TK_COMMA)) continue;
            break;
        }
    }
    expect(p, TK_RPAREN, "')'");
    return ast_stmt_print(args, argc);
}

static AstStmt* parse_stmt_if(Parser* p){
    /* если <cond> [то] {<block>} [иначе {<block>}] или устаревший вариант ... конец */
    AstExpr* cond = parse_expr(p);
    (void)match(p, KW_TO); /* 'то' можно опускать в фигурных скобках */
    AstStmt* s = ast_stmt_if(cond);

    bool then_uses_braces = false;
    if (match(p, TK_LBRACE)){
        then_uses_braces = true;
        for (;;){
            if (check(p, TK_RBRACE)) break;
            if (check(p, TK_EOF)){
                error_here(p, "Ожидалось '}' для блока 'если' перед концом файла");
                return s;
            }
            AstStmt* st = parse_stmt(p);
            ast_if_then_push(s, st);
        }
        expect(p, TK_RBRACE, "'}'");
    } else {
        for (;;){
            if (check(p, KW_INACHE) || check(p, KW_KONEC)) break;
            if (reject_unmatched_closing_brace(p,"legacy-блоке 'если'")) return s;
            if (check(p, TK_EOF)){
                error_here(p, "Ожидалось 'конец' или 'иначе' после блока 'если' перед концом файла");
                return s;
            }
            AstStmt* st = parse_stmt(p);
            ast_if_then_push(s, st);
        }
    }

    bool has_else = false;
    bool else_uses_braces = false;
    if (match(p, KW_INACHE)){
        has_else = true;
        if (match(p, TK_LBRACE)){
            else_uses_braces = true;
            for (;;){
                if (check(p, TK_RBRACE)) break;
                if (check(p, TK_EOF)){
                    error_here(p, "Ожидалось '}' после 'иначе' перед концом файла");
                    return s;
                }
                AstStmt* st = parse_stmt(p);
                ast_if_else_push(s, st);
            }
            expect(p, TK_RBRACE, "'}'");
            if (!then_uses_braces){
                error_here(p,"Ветви одного 'если' должны использовать одинаковый стиль блоков");
                return s;
            }
        } else {
            if (then_uses_braces){
                error_here(p, "После фигурного блока 'если' ожидается '{' после 'иначе'");
                return s;
            }
            for (;;){
                if (check(p, KW_KONEC)) break;
                if (reject_unmatched_closing_brace(p,"legacy-блоке 'иначе'")) return s;
                if (check(p, TK_EOF)){
                    error_here(p, "Ожидалось 'конец' после 'иначе' перед концом файла");
                    return s;
                }
                AstStmt* st = parse_stmt(p);
                ast_if_else_push(s, st);
            }
        }
    }

    bool need_konec = (!then_uses_braces) || (has_else && !else_uses_braces);
    if (need_konec){
        expect(p, KW_KONEC, "'конец' для legacy-блока 'если'");
    }
    return s;
}
static AstStmt* parse_stmt_while(Parser* p){
    /* пока <cond> [то] {<block>} */
    AstExpr* cond = parse_expr(p);
    (void)match(p, KW_TO);
    AstStmt* s = ast_stmt_while(cond);
    p->loop_depth++;

    bool uses_braces = false;
    if (match(p, TK_LBRACE)){
        uses_braces = true;
        for (;;){
            if (check(p, TK_RBRACE)) break;
            if (check(p, TK_EOF)){
                error_here(p, "Ожидалось '}' для 'пока' перед концом файла");
                p->loop_depth--;
                return s;
            }
            AstStmt* st = parse_stmt(p);
            ast_while_push(s, st);
        }
        expect(p, TK_RBRACE, "'}'");
    } else {
        for (;;){
            if (check(p, KW_KONEC)) break;
            if (reject_unmatched_closing_brace(p,"legacy-блоке 'пока'")) { p->loop_depth--; return s; }
            if (check(p, TK_EOF)){
                error_here(p, "Ожидалось 'конец' для 'пока' перед концом файла");
                p->loop_depth--;
                return s;
            }
            AstStmt* st = parse_stmt(p);
            ast_while_push(s, st);
        }
    }

    if (!uses_braces) expect(p, KW_KONEC, "'конец' для legacy-блока 'пока'");
    p->loop_depth--;
    return s;
}
static AstStmt* parse_stmt_foreach(Parser* p){
    /* для каждого <id> в <expr> {<block>} */
    if (!match(p, KW_KAZHDYJ)){ error_here(p,"Ожидалось ключевое слово 'каждого' после 'для'"); return ast_stmt_expr(ast_num(0)); }
    if (!match(p, TK_ID)){ error_here(p,"Ожидалось имя итератора"); return ast_stmt_expr(ast_num(0)); }
    const char* it = p->prev.start; size_t in = (size_t)(p->prev.end - p->prev.start);
    expect(p, KW_V, "'в'");
    AstExpr* seq = parse_expr(p);
    AstStmt* s = ast_stmt_foreach(it, in, seq);
    p->loop_depth++;

    bool uses_braces = false;
    if (match(p, TK_LBRACE)){
        uses_braces = true;
        for (;;){
            if (check(p, TK_RBRACE)) break;
            if (check(p, TK_EOF)){
                error_here(p, "Ожидалось '}' для 'для каждого' перед концом файла");
                p->loop_depth--;
                return s;
            }
            AstStmt* st = parse_stmt(p);
            ast_foreach_push(s, st);
        }
        expect(p, TK_RBRACE, "'}'");
    } else {
        for (;;){
            if (check(p, KW_KONEC)) break;
            if (reject_unmatched_closing_brace(p,"legacy-блоке 'для каждого'")) { p->loop_depth--; return s; }
            if (check(p, TK_EOF)){
                error_here(p, "Ожидалось 'конец' для 'для каждого' перед концом файла");
                p->loop_depth--;
                return s;
            }
            AstStmt* st = parse_stmt(p);
            ast_foreach_push(s, st);
        }
    }

    if (!uses_braces) expect(p, KW_KONEC, "'конец' для legacy-блока 'для каждого'");
    p->loop_depth--;
    return s;
}

static int token_starts_expr(KirTokenKind k){
    return
        k == KW_NE      ||  /* не ... */
        k == TK_MINUS   ||  /* -x */
        k == TK_TILDE   ||  /* ~x */
        k == TK_NUMBER  ||
        k == TK_STRING  ||
        k == KW_ISTINA  ||
        k == KW_LOZH    ||
        k == KW_PUSTO   ||
        k == TK_LPAREN  ||  /* (...) */
        k == TK_LBRACK  ||  /* [...] */
        k == TK_LBRACE  ||  /* {...} */
        k == TK_ID;         /* имя */
}

static AstStmt* parse_stmt_return(Parser* p){
    /* поддержка:
       - "вернуть"        — без значения
       - "вернуть expr"   — с выражением
       - "возврат"        — совместимое старое написание
    */
    if (!token_starts_expr(p->cur.kind)){
        return ast_stmt_return(ast_nil());
    }
    AstExpr* v = parse_expr(p);
    return ast_stmt_return(v);
}

static AstStmt* parse_stmt_include(Parser* p){
    if (!match(p, TK_STRING)){ error_here(p, "Ожидался путь строки после 'включить'"); return ast_stmt_expr(ast_num(0)); }
    size_t n = 0;
    char* path = decode_string_token(&p->prev, &n);
    AstStmt* stmt = ast_stmt_include(path, n);
    kir_free(path);
    return stmt;
}

/* -------- helper: собрать dotted-name:  ID ('.' ID)*  -> heap-строка -------- */
static char* parse_dotted_name(Parser* p, size_t* out_len){
    if (!match(p, TK_ID)){
        error_here(p, "Ожидалось имя модуля");
        return NULL;
    }
    const char* s0 = p->prev.start;
    size_t n0 = (size_t)(p->prev.end - p->prev.start);

    char buf[512];
    size_t pos = 0;

    size_t initial = n0 < sizeof(buf)-1 ? n0 : sizeof(buf)-1;
    for (size_t i=0;i<initial;++i) buf[pos++]=s0[i];

    while (match(p, TK_DOT)){
        if (pos+1 < sizeof(buf)) buf[pos++]='.'; else { pos = sizeof(buf)-1; }
        if (!match(p, TK_ID)){
            error_here(p, "После точки ожидается идентификатор");
            break;
        }
        size_t token_len = (size_t)(p->prev.end - p->prev.start);
        size_t available = sizeof(buf) - 1 - pos;
        size_t copy_len = token_len < available ? token_len : available;
        for (size_t j=0;j<copy_len;++j) buf[pos+j]=p->prev.start[j];
        pos += copy_len;
    }

    buf[pos] = 0;
    char* out = (char*)kir_malloc(pos+1);
    for (size_t i=0;i<=pos;++i) out[i]=buf[i];
    if (out_len) *out_len = pos;
    return out;
}

/* импорт имя[.подимя[...]] | "строка" [как алиас] [из "dir"] */
static AstStmt* parse_stmt_import(Parser* p){
    char* name_heap = NULL;
    size_t nn = 0;

    if (check(p, TK_ID)){
        name_heap = parse_dotted_name(p, &nn);
        if (!name_heap){
            /* parse_dotted_name уже выдал ошибку */
            return ast_stmt_expr(ast_num(0));
        }
    } else if (match(p, TK_STRING)){
        name_heap = decode_string_token(&p->prev, &nn);
    } else {
        error_here(p, "Ожидалось имя модуля (идентификатор или строка)");
        return ast_stmt_expr(ast_num(0));
    }

    const char* alias=NULL; size_t an=0;
    const char* from_dir=NULL; char* from_dir_heap=NULL; size_t fn=0;

    if (match(p, KW_KAK)){
        if (!match(p, TK_ID)){
            error_here(p,"После 'как' нужен идентификатор");
        } else {
            alias = p->prev.start; an = (size_t)(p->prev.end - p->prev.start);
        }
    }
    if (match(p, KW_IZ)){
        if (!match(p, TK_STRING)){
            error_here(p, "После 'из' нужен путь-строка");
        } else {
            from_dir_heap = decode_string_token(&p->prev, &fn);
            from_dir = from_dir_heap;
        }
    }

#ifdef KIRTON_AST_IMPORT_HAS_FROM
    if (from_dir){
        AstStmt* stmt = ast_stmt_import_ex(name_heap, nn, alias, an, from_dir, fn);
        kir_free(name_heap);
        kir_free(from_dir_heap);
        return stmt;
    }
    AstStmt* stmt = ast_stmt_import(name_heap, nn, alias, an);
    kir_free(name_heap);
    kir_free(from_dir_heap);
    return stmt;
#else
    (void)from_dir; (void)fn;
    AstStmt* stmt = ast_stmt_import(name_heap, nn, alias, an);
    kir_free(name_heap);
    kir_free(from_dir_heap);
    return stmt;
#endif
}

/* присваивание: lhs '=' expr, где lhs = ID | obj.attr | obj[idx] */
static AstStmt* parse_assignment_or_expr_stmt(Parser* p){
    if (!check(p, TK_ID)) {
        AstExpr* e = parse_expr(p);
        return ast_stmt_expr(e);
    }

    /* распарсим lhs как выражение (var/attr/index/вызов), затем проверим '=' */
    AstExpr* lhs = parse_primary(p);

    /* если нет '=', это просто выражение,
       докручиваем правую часть через тот же Pratt-парсер */
    if (!match(p, TK_EQ)){
        AstExpr* full = parse_bin_rhs(p, lhs, 0);
        return ast_stmt_expr(full);
    }

    /* есть '=', значит это присваивание */
    AstExpr* rhs = parse_expr(p);

    if (lhs->kind == AST_VAR){
        AstStmt* stmt=ast_stmt_set(lhs->as.var.name_start, lhs->as.var.name_len, rhs);
        ast_expr_destroy(lhs);
        return stmt;
    } else if (lhs->kind == AST_INDEX){
        AstExpr* base=lhs->as.index.base;
        AstExpr* index=lhs->as.index.index;
        lhs->as.index.base=NULL;
        lhs->as.index.index=NULL;
        ast_expr_destroy(lhs);
        return ast_stmt_setindex(base,index,rhs);
    } else if (lhs->kind == AST_ATTR){
        AstStmt* stmt=ast_stmt_setattr(lhs->as.attr.obj, lhs->as.attr.name, lhs->as.attr.n, rhs);
        lhs->as.attr.obj=NULL; /* владение перенесено в stmt */
        ast_expr_destroy(lhs);
        return stmt;
    } else {
        error_here(p, "Слева от '=' допустимы: имя, индекс или атрибут");
        ast_expr_destroy(lhs);
        return ast_stmt_expr(rhs);
    }
}

static AstFunc* parse_function(Parser* p);
static AstStmt* parse_stmt(Parser* p);

static void parse_try_body(Parser* p,AstStmt* stmt,int target,bool uses_braces){
    while (!(uses_braces ? check(p,TK_RBRACE) :
             check(p,KW_CATCH) || check(p,KW_FINALLY) || check(p,KW_KONEC)) && !check(p,TK_EOF)){
        if (!uses_braces && reject_unmatched_closing_brace(p,"legacy-блоке обработки ошибки")) return;
        AstStmt* body=parse_stmt(p);
        if (target==0) ast_try_body_push(stmt,body);
        else if (target==1) ast_try_catch_push(stmt,body);
        else ast_try_finally_push(stmt,body);
    }
}

static AstStmt* parse_stmt_try(Parser* p){
    next(p); /* попробовать */
    AstStmt* stmt=ast_stmt_try(NULL,0,false,false);
    bool uses_braces=match(p,TK_LBRACE);
    parse_try_body(p,stmt,0,uses_braces);
    if (uses_braces){
        if (check(p,TK_EOF)) error_here(p,"Ожидалось '}' для блока 'попробовать' перед концом файла");
        else expect(p,TK_RBRACE,"'}' для блока 'попробовать'");
    }
    if (match(p,KW_CATCH)){
        if (!match(p,TK_ID)) error_here(p,"После 'перехватить' требуется имя ошибки");
        else {
            stmt->as.try_.catch_name=kir_strdupn(p->prev.start,
                (size_t)(p->prev.end-p->prev.start));
            stmt->as.try_.catch_name_len=(size_t)(p->prev.end-p->prev.start);
            stmt->as.try_.has_catch=true;
        }
        if (uses_braces && !match(p,TK_LBRACE))
            error_here(p,"После имени ошибки ожидается '{'");
        parse_try_body(p,stmt,1,uses_braces);
        if (uses_braces){
            if (check(p,TK_EOF)) error_here(p,"Ожидалось '}' для блока 'перехватить' перед концом файла");
            else expect(p,TK_RBRACE,"'}' для блока 'перехватить'");
        }
    }
    if (match(p,KW_FINALLY)){
        stmt->as.try_.has_finally=true;
        if (uses_braces && !match(p,TK_LBRACE)) error_here(p,"После 'всегда' ожидается '{'");
        while (!(uses_braces ? check(p,TK_RBRACE) : check(p,KW_KONEC)) && !check(p,TK_EOF)){
            if (!uses_braces && reject_unmatched_closing_brace(p,"legacy-блоке 'всегда'")) break;
            ast_try_finally_push(stmt,parse_stmt(p));
        }
        if (uses_braces){
            if (check(p,TK_EOF)) error_here(p,"Ожидалось '}' для блока 'всегда' перед концом файла");
            else expect(p,TK_RBRACE,"'}' для блока 'всегда'");
        }
    }
    if (!stmt->as.try_.has_catch && !stmt->as.try_.has_finally)
        error_here(p,"После блока 'попробовать' ожидается 'перехватить' или 'всегда'");
    if (uses_braces){
        if (check(p,KW_KONEC)) error_here(p,"После фигурной обработки ошибки не требуется 'конец'");
    } else {
        expect(p,KW_KONEC,"'конец' после обработки ошибки");
    }
    return stmt;
}

static AstStmt* parse_stmt(Parser* p){
    KirToken start = p->cur;
    AstStmt* stmt = NULL;
    if (match(p, KW_PUST))      stmt = parse_stmt_let(p);
    else if (match(p, KW_PECHATAT))  stmt = parse_stmt_print(p);
    else if (match(p, KW_ESLI))      stmt = parse_stmt_if(p);
    else if (match(p, KW_POKA))      stmt = parse_stmt_while(p);
    else if (match(p, KW_DLYA))      stmt = parse_stmt_foreach(p);
    else if (check(p, KW_BREAK)){
        if (p->loop_depth == 0) error_here(p, "'прервать' допустимо только внутри цикла");
        next(p);
        stmt = ast_stmt_break();
    }
    else if (check(p, KW_CONTINUE)){
        if (p->loop_depth == 0) error_here(p, "'продолжить' допустимо только внутри цикла");
        next(p);
        stmt = ast_stmt_continue();
    }
    else if (match(p, KW_VOZVRAT))   stmt = parse_stmt_return(p);
    else if (check(p,KW_TRY))        stmt = parse_stmt_try(p);
    else if (check(p, KW_FUNKCIYA))  stmt = ast_stmt_function(parse_function(p));
    else if (match(p, KW_VKLYUCHIT)) stmt = parse_stmt_include(p);
    else if (match(p, KW_IMPORT))    stmt = parse_stmt_import(p);
    /* по умолчанию — выражение/присваивание */
    else stmt = parse_assignment_or_expr_stmt(p);
    return stmt_at(stmt, &start);
}

/* функция: функция имя(парамы) {<block>} (или устаревший ... конец) */
static AstFunc* parse_function(Parser* p){
    KirToken start = p->cur;
    expect(p, KW_FUNKCIYA, "'функция'");
    if (!match(p, TK_ID)){ error_here(p, "Ожидалось имя функции"); return ast_func("<безымянная>", 16); }
    const char* ns = p->prev.start; size_t nn = (size_t)(p->prev.end - p->prev.start);
    AstFunc* f = ast_func(ns, nn);
    f->pos.file = start.file;
    f->pos.line = start.line;
    f->pos.col = start.col;
    expect(p, TK_LPAREN, "'('");
    if (!check(p, TK_RPAREN)){
        for (;;){
            if (!match(p, TK_ID)){ error_here(p, "Ожидался идентификатор параметра"); break; }
            ast_func_add_param(f, p->prev.start, (size_t)(p->prev.end - p->prev.start));
            if (match(p, TK_COMMA)) continue;
            break;
        }
    }
    expect(p, TK_RPAREN, "')'");

    unsigned int enclosing_loop_depth = p->loop_depth;
    p->loop_depth = 0;

    bool uses_braces = false;
    if (match(p, TK_LBRACE)){
        uses_braces = true;
        for (;;){
            if (check(p, TK_RBRACE)) break;
            if (check(p, TK_EOF)){
                error_here(p, "Ожидалось '}' для тела функции перед концом файла");
                break;
            }
            AstStmt* st = parse_stmt(p);
            ast_func_push(f, st);
        }
        if (check(p,TK_RBRACE)) expect(p, TK_RBRACE, "'}'");
    } else {
        /* тело до 'конец' */
        for (;;){
            if (check(p, KW_KONEC)){ next(p); break; }
            if (reject_unmatched_closing_brace(p,"legacy-теле функции")) break;
            if (check(p, TK_EOF)){
                error_here(p, "Ожидалось 'конец' для функции перед концом файла");
                break;
            }
            AstStmt* st = parse_stmt(p);
            ast_func_push(f, st);
        }
    }

    p->loop_depth = enclosing_loop_depth;
    return f;
}

static AstUserType* parse_user_type(Parser* p){
    KirToken start=p->cur;
    AstUserTypeKind kind=check(p,KW_STRUCT)?AST_TYPE_STRUCT:AST_TYPE_ENUM;
    next(p);
    if (!match(p,TK_ID)){
        error_here(p,"После объявления типа требуется имя");
        return ast_user_type(kind,"<ошибка>",8);
    }
    const char* name=p->prev.start;
    size_t name_len=(size_t)(p->prev.end-p->prev.start);
    AstUserType* type=ast_user_type(kind,name,name_len);
    type->pos=(AstPos){start.file,start.line,start.col};
    bool uses_braces=match(p,TK_LBRACE);
    while (!(uses_braces ? check(p,TK_RBRACE) : check(p,KW_KONEC)) && !check(p,TK_EOF)){
        if (!uses_braces && reject_unmatched_closing_brace(p,"legacy-объявлении типа")) break;
        if (!match(p,TK_ID)){
            error_here(p,"Ожидалось имя поля или варианта типа");
            next(p);
            continue;
        }
        const char* member=p->prev.start;
        size_t member_len=(size_t)(p->prev.end-p->prev.start);
        if (kind==AST_TYPE_STRUCT){
            for (size_t i=0;i<type->field_count;++i){
                if (type->fields[i].name_len==member_len &&
                    memcmp(type->fields[i].name,member,member_len)==0)
                    error_here(p,"Повторное имя поля структуры");
            }
            expect(p,TK_COLON,"':' после имени поля");
            if (!match(p,TK_ID)){
                error_here(p,"Ожидался тип поля");
                continue;
            }
            ast_user_type_add_field(type,member,member_len,p->prev.start,
                                    (size_t)(p->prev.end-p->prev.start));
        } else {
            for (size_t i=0;i<type->variant_count;++i){
                if (type->variant_lens[i]==member_len &&
                    memcmp(type->variants[i],member,member_len)==0)
                    error_here(p,"Повторный вариант перечисления");
            }
            ast_user_type_add_variant(type,member,member_len);
        }
    }
    if (uses_braces){
        expect(p,TK_RBRACE,"'}' после объявления типа");
    }
    else expect(p,KW_KONEC,"'конец' после объявления типа");
    return type;
}

AstProgram* parse_program(const char* src, size_t len, const char* filename){
    unsigned int errors_before = kir_diag_error_count();
    Parser P;
    /* AST source positions outlive the caller's temporary path buffers. */
    const char* stable_filename = kir_strdup(filename ? filename : "<скрипт>");
    kir_lexer_init_with_file(&P.lx, stable_filename, src, len);
    P.file = stable_filename;
    P.loop_depth = 0;
    P.prev = (KirToken){0};
    P.cur  = kir_lexer_next(&P.lx);

    AstProgram* prog = ast_program();
    prog->source_file = stable_filename;

    if (match(&P, KW_MODUL)){
        if (match(&P, TK_ID)){
            prog->module_name = kir_strdupn(P.prev.start, (size_t)(P.prev.end - P.prev.start));
            prog->module_name_len = (size_t)(P.prev.end - P.prev.start);
        } else if (match(&P, TK_STRING)){
            prog->module_name = decode_string_token(&P.prev, &prog->module_name_len);
        } else {
            error_here(&P, "После 'модуль' ожидается имя модуля");
        }
        if (prog->module_name_len == 0){
            kir_diag_error(P.file, P.prev.line, P.prev.col, "Имя модуля не может быть пустым");
        }
    }

    while (!check(&P, TK_EOF)){
        if (check(&P,KW_KONEC)){
            error_here(&P,"Legacy-терминатор 'конец' не закрывает уже закрытый фигурный блок");
            next(&P);
            continue;
        }
        if (check(&P, KW_FUNKCIYA)){
            AstFunc* f = parse_function(&P);
            ast_program_push(prog, f);
            continue;
        }
        if (check(&P,KW_STRUCT) || check(&P,KW_ENUM)){
            AstUserType* type=parse_user_type(&P);
            for (size_t i=0;i<prog->type_count;++i){
                if (prog->types[i]->name_len==type->name_len &&
                    memcmp(prog->types[i]->name,type->name,type->name_len)==0)
                    kir_diag_error(P.file,type->pos.line,type->pos.col,
                                   "Повторное имя пользовательского типа '%.*s'",
                                   (int)type->name_len,type->name);
            }
            ast_program_add_type(prog,type);
            continue;
        }
        /* остальное — это init-операторы */
        AstStmt* st = parse_stmt(&P);
        ast_program_init_push(prog, st);
    }
    prog->parse_errors = kir_diag_error_count() - errors_before;
    return prog;
}
