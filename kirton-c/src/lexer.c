/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
// src/lexer.c — полностью исправленная версия
#include "kirton/lexer.h"
#include "kirton/utf8.h"
#include "kirton/token.h"
#include "kirton/common.h"

#include <string.h>
#include <stdbool.h>
#include <stdint.h>

/* ========= ВНУТРЕННИЕ ХЕЛПЕРЫ ========= */

static inline void set_token(KirToken* t, KirTokenKind k,
                             const char* st, const char* en,
                             int line, int col, const char* file)
{
    t->kind  = k;
    t->start = st;
    t->end   = en;
    t->line  = line;
    t->col   = col;
    t->file  = file;
}

static inline bool at_end(const KirLexer* lx){
    return lx->cur >= lx->end;
}

static inline void advance_n(KirLexer* lx, int n){
    lx->cur += n;
    lx->col += n;
}

static inline void advance1(KirLexer* lx){
    lx->cur += 1;
    lx->col += 1;
}

static inline void advance_cp(KirLexer* lx, int nbytes){
    lx->cur += nbytes;
    lx->col += 1; /* считаем в колонках кодпоинты, а не байты */
}

static inline void newline(KirLexer* lx){
    lx->line += 1;
    lx->col   = 1;
}

/* ========= ИНИЦИАЛИЗАЦИЯ ========= */

void kir_lexer_init_with_file(KirLexer* lx, const char* filename,
                              const char* buf, size_t len)
{
    lx->src  = buf;
    lx->cur  = buf;
    lx->end  = buf + len;
    lx->line = 1;
    lx->col  = 1;
    lx->file = filename ? filename : "<скрипт>";
}

void kir_lexer_init(KirLexer* lx, const char* buf, size_t len){
    kir_lexer_init_with_file(lx, "<скрипт>", buf, len);
}

/* ========= ПРОБЕЛЫ + КОММЕНТАРИИ ========= */

static int skip_ws_and_comments(KirLexer* lx){
    for (;;){
        if (at_end(lx))
            return 1;

        char c = *lx->cur;

        /* пробелы и табы */
        if (c == ' ' || c == '\t'){
            advance1(lx);
            continue;
        }

        /* переводы строк */
        if (c == '\n'){
            advance1(lx);
            newline(lx);
            continue;
        }
        if (c == '\r'){
            advance1(lx);
            if (!at_end(lx) && *lx->cur == '\n')
                advance1(lx);
            newline(lx);
            continue;
        }

        /* ----- однострочный комментарий // ----- */
        if (c == '/' && lx->cur + 1 < lx->end && lx->cur[1] == '/'){
            lx->cur += 2;
            lx->col += 2;

            while (!at_end(lx)){
                char d = *lx->cur;

                if (d == '\n'){
                    advance1(lx);
                    newline(lx);
                    break;
                }
                if (d == '\r'){
                    advance1(lx);
                    if (!at_end(lx) && *lx->cur == '\n')
                        advance1(lx);
                    newline(lx);
                    break;
                }

                /* внутри комментария нас не волнует точная колонка */
                lx->cur++;
            }
            continue;
        }

        /* ----- однострочный комментарий # ----- */
        if (c == '#'){
            /* стиль «скриптовый» — до конца строки */
            advance1(lx); /* съели '#' */

            while (!at_end(lx)){
                char d = *lx->cur;

                if (d == '\n'){
                    advance1(lx);
                    newline(lx);
                    break;
                }
                if (d == '\r'){
                    advance1(lx);
                    if (!at_end(lx) && *lx->cur == '\n')
                        advance1(lx);
                    newline(lx);
                    break;
                }

                lx->cur++;
            }
            continue;
        }

        /*
          ----- блочный комментарий вида «слэш-звёздочка ... звёздочка-слэш» -----
          ВАЖНО: в тексте этого комментария мы не пишем подряд символы / * или * /,
          чтобы не порвать его раньше времени в старых компиляторах.
        */
        if (c == '/' && lx->cur + 1 < lx->end && lx->cur[1] == '*'){
            lx->cur += 2;
            lx->col += 2;
            int closed = 0;

            while (!at_end(lx)){
                char d = *lx->cur;

                if (d == '\n'){
                    advance1(lx);
                    newline(lx);
                    continue;
                }
                if (d == '\r'){
                    advance1(lx);
                    if (!at_end(lx) && *lx->cur == '\n')
                        advance1(lx);
                    newline(lx);
                    continue;
                }

                /* конец комментария: звёздочка-слэш */
                if (d == '*' &&
                    lx->cur + 1 < lx->end &&
                    lx->cur[1] == '/')
                {
                    lx->cur += 2;
                    lx->col += 2;
                    closed = 1;
                    break;
                }

                lx->cur++;
            }
            if (!closed) return 0;
            continue;
        }

        /* не пробел и не комментарий → стоп */
        return 1;
    }
}

/* ========= КЛЮЧЕВЫЕ СЛОВА ========= */

static KirTokenKind keyword_kind(const char* s, size_t n){
#define KW(word, tok) do{               \
        size_t L = strlen(word);        \
        if (L == n && memcmp(s, word, n) == 0) return tok; \
    } while(0)

    KW("модуль",    KW_MODUL);
    KW("функция",   KW_FUNKCIYA);
    KW("пусть",     KW_PUST);
    KW("если",      KW_ESLI);
    KW("иначе",     KW_INACHE);
    KW("пока",      KW_POKA);
    KW("для",       KW_DLYA);
    KW("прервать",  KW_BREAK);
    KW("продолжить", KW_CONTINUE);
    KW("каждого",   KW_KAZHDYJ);
    KW("каждый",    KW_KAZHDYJ);
    KW("в",         KW_V);
    KW("вернуть",   KW_VOZVRAT);
    KW("возврат",   KW_VOZVRAT);
    KW("печатать",  KW_PECHATAT);
    KW("конец",     KW_KONEC);
    KW("то",        KW_TO);
    KW("истина",    KW_ISTINA);
    KW("ложь",      KW_LOZH);
    KW("пусто",     KW_PUSTO);
    KW("и",         KW_I);
    KW("или",       KW_ILI);
    KW("не",        KW_NE);
    KW("нацело",    KW_NATSELO);
    KW("импорт",    KW_IMPORT);
    KW("как",       KW_KAK);
    KW("из",        KW_IZ);
    KW("включить",  KW_VKLYUCHIT);
    KW("попробовать",KW_TRY);
    KW("перехватить",KW_CATCH);
    KW("всегда",    KW_FINALLY);
    KW("структура", KW_STRUCT);
    KW("перечисление",KW_ENUM);

#undef KW
    return TK_ID;
}

/* ========= ИДЕНТИФИКАТОРЫ ========= */

static inline bool is_ascii_alpha(char c){
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}
static inline bool is_ascii_digit(char c){
    return (c >= '0' && c <= '9');
}

static inline bool is_ident_start(unsigned char b){
    return b == '_' || is_ascii_alpha((char)b) || b >= 0x80;
}

static inline bool is_ident_continue(unsigned char b){
    return is_ident_start(b) || is_ascii_digit((char)b);
}

/* ========= ЧИСЛА ========= */

static KirToken make_simple(KirLexer* lx, KirTokenKind k,
                            const char* st, const char* en,
                            int line, int col)
{
    KirToken t;
    set_token(&t, k, st, en, line, col, lx->file);
    return t;
}

static KirToken lex_number(KirLexer* lx){
    const char* st = lx->cur;
    int line = lx->line;
    int col  = lx->col;

    /* HEX-литерал: 0xFF, 0X10 и т.п. */
    if (*lx->cur == '0' &&
        lx->cur + 1 < lx->end &&
        (lx->cur[1] == 'x' || lx->cur[1] == 'X'))
    {
        advance1(lx); /* 0 */
        advance1(lx); /* x */

        while (!at_end(lx)){
            char c = *lx->cur;
            if ((c >= '0' && c <= '9') ||
                (c >= 'a' && c <= 'f') ||
                (c >= 'A' && c <= 'F'))
            {
                advance1(lx);
            } else {
                break;
            }
        }
        return make_simple(lx, TK_NUMBER, st, lx->cur, line, col);
    }

    /* десятичное / с плавающей точкой */
    while (!at_end(lx)){
        char c = *lx->cur;
        if (c >= '0' && c <= '9')
            advance1(lx);
        else
            break;
    }

    if (!at_end(lx) && *lx->cur == '.' &&
        lx->cur + 1 < lx->end &&
        (lx->cur[1] >= '0' && lx->cur[1] <= '9'))
    {
        advance1(lx); /* '.' */
        while (!at_end(lx)){
            char c = *lx->cur;
            if (c >= '0' && c <= '9')
                advance1(lx);
            else
                break;
        }
    }

    return make_simple(lx, TK_NUMBER, st, lx->cur, line, col);
}

/* ========= СТРОКИ ========= */

static KirToken lex_string(KirLexer* lx){
    const char* st = lx->cur;
    int line = lx->line;
    int col  = lx->col;

    advance1(lx); /* открывающая " */

    while (!at_end(lx)){
        char c = *lx->cur;

        if (c == '"'){
            advance1(lx);
            return make_simple(lx, TK_STRING, st, lx->cur, line, col);
        }

        if (c == '\\'){
            /* Пропускаем экранированный символ; декодирование делает парсер. */
            if (lx->cur + 1 < lx->end){
                advance_n(lx, 2);
                continue;
            } else {
                advance1(lx);
                return make_simple(lx, TK_ERROR, st, lx->cur, line, col);
            }
        }

        if (c == '\n'){
            /* незакрытая строка до конца строки — ошибка */
            advance1(lx);
            newline(lx);
            return make_simple(lx, TK_ERROR, st, lx->cur, line, col);
        }

        if (c == '\r'){
            advance1(lx);
            if (!at_end(lx) && *lx->cur == '\n') advance1(lx);
            newline(lx);
            return make_simple(lx, TK_ERROR, st, lx->cur, line, col);
        }

        unsigned char byte = (unsigned char)c;
        if (byte < 0x20){
            advance1(lx);
            return make_simple(lx, TK_ERROR, st, lx->cur, line, col);
        }

        if (byte >= 0x80){
            uint32_t cp;
            int adv = kir_utf8_decode(lx->cur, lx->end, &cp);
            if (adv <= 0){
                advance1(lx);
                return make_simple(lx, TK_ERROR, st, lx->cur, line, col);
            }
            advance_cp(lx, adv);
        } else {
            advance1(lx);
        }
    }

    /* EOF внутри строки */
    return make_simple(lx, TK_ERROR, st, lx->cur, line, col);
}

/* ========= ИДЕНТИФИКАТОР / КЛЮЧЕВОЕ СЛОВО ========= */

static KirToken lex_ident_or_kw(KirLexer* lx){
    const char* st = lx->cur;
    int line = lx->line;
    int col  = lx->col;

    unsigned char b0 = (unsigned char)*lx->cur;

    if (b0 < 0x80){
        if (!is_ident_start(b0))
            return make_simple(lx, TK_ERROR, st, lx->cur, line, col);
        advance1(lx);
    } else {
        /* первый UTF-8 кодпоинт */
        uint32_t cp;
        int adv = kir_utf8_decode(lx->cur, lx->end, &cp);
        if (adv <= 0 || !kir_is_ident_start(cp)){
            if (adv > 0) advance_cp(lx, adv);
            else advance1(lx);
            return make_simple(lx, TK_ERROR, st, lx->cur, line, col);
        }
        advance_cp(lx, adv);
    }

    while (!at_end(lx)){
        unsigned char b = (unsigned char)*lx->cur;

        if (b < 0x80){
            if (!is_ident_continue(b))
                break;
            advance1(lx);
        } else {
            uint32_t cp;
            int adv = kir_utf8_decode(lx->cur, lx->end, &cp);
            if (adv <= 0 || !kir_is_ident_cont(cp))
                break;
            advance_cp(lx, adv);
        }
    }

    KirToken t = make_simple(lx, TK_ID, st, lx->cur, line, col);
    KirTokenKind kw = keyword_kind(st, (size_t)(lx->cur - st));
    if (kw != TK_ID)
        t.kind = kw;
    return t;
}

/* ========= ПУБЛИЧНЫЙ API ========= */

KirToken kir_lexer_next(KirLexer* lx){
    if (!skip_ws_and_comments(lx)){
        KirToken t;
        set_token(&t, TK_ERROR, lx->cur, lx->cur, lx->line, lx->col, lx->file);
        return t;
    }

    if (at_end(lx)){
        KirToken t;
        set_token(&t, TK_EOF, lx->cur, lx->cur, lx->line, lx->col, lx->file);
        return t;
    }

    const char* st = lx->cur;
    int line = lx->line;
    int col  = lx->col;
    char c   = *lx->cur;

    switch (c){
        case '(' : advance1(lx); return make_simple(lx, TK_LPAREN, st, lx->cur, line, col);
        case ')' : advance1(lx); return make_simple(lx, TK_RPAREN, st, lx->cur, line, col);
        case '[' : advance1(lx); return make_simple(lx, TK_LBRACK, st, lx->cur, line, col);
        case ']' : advance1(lx); return make_simple(lx, TK_RBRACK, st, lx->cur, line, col);
        case '{' : advance1(lx); return make_simple(lx, TK_LBRACE, st, lx->cur, line, col);
        case '}' : advance1(lx); return make_simple(lx, TK_RBRACE, st, lx->cur, line, col);
        case '.' : advance1(lx); return make_simple(lx, TK_DOT,   st, lx->cur, line, col);
        case ':' : advance1(lx); return make_simple(lx, TK_COLON, st, lx->cur, line, col);
        case ',' : advance1(lx); return make_simple(lx, TK_COMMA, st, lx->cur, line, col);
        case '+' : advance1(lx); return make_simple(lx, TK_PLUS,  st, lx->cur, line, col);
        case '-' : advance1(lx); return make_simple(lx, TK_MINUS, st, lx->cur, line, col);
        case '*' : advance1(lx); return make_simple(lx, TK_STAR,  st, lx->cur, line, col);
        case '%' : advance1(lx); return make_simple(lx, TK_PERCENT, st, lx->cur, line, col);
        case '&' : advance1(lx); return make_simple(lx, TK_AMP, st, lx->cur, line, col);
        case '|' : advance1(lx); return make_simple(lx, TK_PIPE, st, lx->cur, line, col);
        case '^' : advance1(lx); return make_simple(lx, TK_CARET, st, lx->cur, line, col);
        case '~' : advance1(lx); return make_simple(lx, TK_TILDE, st, lx->cur, line, col);

        case '/':
            advance1(lx);
            return make_simple(lx, TK_SLASH, st, lx->cur, line, col);

        case '=':
            advance1(lx);
            if (!at_end(lx) && *lx->cur == '='){
                advance1(lx);
                return make_simple(lx, TK_EQEQ, st, lx->cur, line, col);
            }
            return make_simple(lx, TK_EQ, st, lx->cur, line, col);

        case '!':
            advance1(lx);
            if (!at_end(lx) && *lx->cur == '='){
                advance1(lx);
                return make_simple(lx, TK_NEQ, st, lx->cur, line, col);
            }
            return make_simple(lx, TK_ERROR, st, lx->cur, line, col);

        case '<':
            advance1(lx);
            if (!at_end(lx) && *lx->cur == '<'){
                advance1(lx);
                return make_simple(lx, TK_SHL, st, lx->cur, line, col);
            }
            if (!at_end(lx) && *lx->cur == '='){
                advance1(lx);
                return make_simple(lx, TK_LE, st, lx->cur, line, col);
            }
            return make_simple(lx, TK_LT, st, lx->cur, line, col);

        case '>':
            advance1(lx);
            if (!at_end(lx) && *lx->cur == '>'){
                advance1(lx);
                return make_simple(lx, TK_SHR, st, lx->cur, line, col);
            }
            if (!at_end(lx) && *lx->cur == '='){
                advance1(lx);
                return make_simple(lx, TK_GE, st, lx->cur, line, col);
            }
            return make_simple(lx, TK_GT, st, lx->cur, line, col);

        case '"':
            return lex_string(lx);
    }

    /* число? */
    if (c >= '0' && c <= '9')
        return lex_number(lx);

    /* идентификатор / ключевое слово? */
    if ((unsigned char)c < 0x80 && is_ident_start((unsigned char)c))
        return lex_ident_or_kw(lx);

    if ((unsigned char)c >= 0x80){
        uint32_t cp;
        int adv = kir_utf8_decode(lx->cur, lx->end, &cp);
        if (adv > 0 && kir_is_ident_start(cp))
            return lex_ident_or_kw(lx);
        if (adv > 0) advance_cp(lx, adv);
        else advance1(lx);
        return make_simple(lx, TK_ERROR, st, lx->cur, line, col);
    }

    /* неизвестный символ */
    advance1(lx);
    return make_simple(lx, TK_ERROR, st, lx->cur, line, col);
}

/* ========= ИМЕНА ТОКЕНОВ ========= */

const char* kir_token_kind_name(KirTokenKind k){
    switch (k){
        case TK_NUMBER: return "NUMBER";
        case TK_STRING: return "STRING";
        case TK_ID:     return "ID";

        case KW_MODUL:    return "модуль";
        case KW_FUNKCIYA: return "функция";
        case KW_PUST:     return "пусть";
        case KW_ESLI:     return "если";
        case KW_INACHE:   return "иначе";
        case KW_POKA:     return "пока";
        case KW_DLYA:     return "для";
        case KW_BREAK:    return "прервать";
        case KW_CONTINUE: return "продолжить";
        case KW_KAZHDYJ:  return "каждого";
        case KW_V:        return "в";
        case KW_VOZVRAT:  return "вернуть";
        case KW_PECHATAT: return "печатать";
        case KW_KONEC:    return "конец";
        case KW_TO:       return "то";
        case KW_ISTINA:   return "истина";
        case KW_LOZH:     return "ложь";
        case KW_PUSTO:    return "пусто";
        case KW_I:        return "и";
        case KW_ILI:      return "или";
        case KW_NE:       return "не";
        case KW_NATSELO:  return "нацело";
        case KW_IMPORT:   return "импорт";
        case KW_KAK:      return "как";
        case KW_IZ:       return "из";
        case KW_VKLYUCHIT:return "включить";
        case KW_TRY:     return "попробовать";
        case KW_CATCH:   return "перехватить";
        case KW_FINALLY: return "всегда";
        case KW_STRUCT:  return "структура";
        case KW_ENUM:    return "перечисление";

        case TK_EQ:     return "=";
        case TK_EQEQ:   return "==";
        case TK_NEQ:    return "!=";
        case TK_LT:     return "<";
        case TK_GT:     return ">";
        case TK_LE:     return "<=";
        case TK_GE:     return ">=";
        case TK_PLUS:   return "+";
        case TK_MINUS:  return "-";
        case TK_STAR:   return "*";
        case TK_SLASH:  return "/";
        case TK_PERCENT:return "%";
        case TK_AMP:    return "&";
        case TK_PIPE:   return "|";
        case TK_CARET:  return "^";
        case TK_TILDE:  return "~";
        case TK_SHL:    return "<<";
        case TK_SHR:    return ">>";
        case TK_LPAREN: return "(";
        case TK_RPAREN: return ")";
        case TK_LBRACK: return "[";
        case TK_RBRACK: return "]";
        case TK_LBRACE: return "{";
        case TK_RBRACE: return "}";
        case TK_DOT:    return ".";
        case TK_COLON:  return ":";
        case TK_COMMA:  return ",";
        case TK_EOF:    return "EOF";
        case TK_ERROR:  return "ERROR";
    }
    return "?";
}
