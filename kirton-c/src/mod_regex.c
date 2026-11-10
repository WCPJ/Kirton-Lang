/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
// src/mod_regex.c — простой регвыр для Киртона (., *, +, ?, [], ^, $, \-экранирование; флаги i m s g)
// Индексы и длины — в байтах (UTF-8 строка безопасна, но регистр/классы — ASCII).

#include "kirton/native.h"
#include "native_internal.h"
#include "kirton/value.h"
#include "kirton/common.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <limits.h>

/* ===================== Флаги ===================== */
typedef struct {
    int icase;   // i
    int mline;   // m
    int dotall;  // s
    int global;  // g (имеет смысл только для заменить)
} RFlags;

static RFlags parse_flags_opt(int argc, const Value* a, int idx){
    RFlags f = {0,0,0,0};
    if (idx < argc && a[idx].kind == VAL_STR){
        const char* s = a[idx].as.str.s;
        int n = a[idx].as.str.n;
        for (int i=0;i<n;i++){
            char c = s[i];
            if (c=='i' || c=='I') f.icase=1;
            else if (c=='m' || c=='M') f.mline=1;
            else if (c=='s' || c=='S') f.dotall=1;
            else if (c=='g' || c=='G') f.global=1;
        }
    }
    return f;
}

static inline unsigned char lower_ascii(unsigned char c){
    if (c>='A' && c<='Z') return (unsigned char)(c + ('a'-'A'));
    return c;
}
static inline int eq_ch(unsigned char a, unsigned char b, int icase){
    if (!icase) return a==b;
    return lower_ascii(a) == lower_ascii(b);
}

/* ===================== Класс символов [..] ===================== */

typedef struct {
    unsigned char set[256]; // 0/1
    int neg;                // 1, если [^...]
} CharClass;

/* pattern[p ... pn) — содержимое, начинающееся с '['; на выходе p ссылается на символ после ']' */
static int parse_class(const char* p, int pn, int *pi, RFlags f, CharClass* out){
    int i = *pi; // указывает на '['
    memset(out->set, 0, sizeof(out->set));
    out->neg = 0;
    i++; // пропускаем '['
    if (i < pn && p[i] == '^'){ out->neg = 1; i++; }

    int first = 1;
    while (i < pn){
        unsigned char c;
        if (p[i] == ']' && !first){ i++; break; }
        first = 0;

        // элемент класса (с поддержкой escapes)
        if (p[i] == '\\' && i+1 < pn){
            c = (unsigned char)p[i+1];
            i += 2;
            out->set[c] = 1;
            if (f.icase){
                out->set[lower_ascii(c)] = 1;
                // добавим и верхний регистр, если исходный нижний
                if (c>='a'&&c<='z') out->set[(unsigned char)(c-('a'-'A'))] = 1;
                if (c>='A'&&c<='Z') out->set[(unsigned char)(c+('a'-'A'))] = 1;
            }
        } else {
            c = (unsigned char)p[i++];
            if (i+1 < pn && p[i] == '-' && p[i+1] != ']'){
                // диапазон a-z
                unsigned char e = (unsigned char)p[i+1];
                i += 2;
                unsigned char lo = c < e ? c : e;
                unsigned char hi = c < e ? e : c;
                for (unsigned int cc=lo; cc<=hi; cc++){
                    out->set[cc] = 1;
                    if (f.icase){
                        out->set[lower_ascii((unsigned char)cc)] = 1;
                        if (cc>='a'&&cc<='z') out->set[(unsigned char)(cc-('a'-'A'))] = 1;
                        if (cc>='A'&&cc<='Z') out->set[(unsigned char)(cc+('a'-'A'))] = 1;
                    }
                    if (cc == 255) break; // защита от переполнения цикла
                }
            } else {
                out->set[c] = 1;
                if (f.icase){
                    out->set[lower_ascii(c)] = 1;
                    if (c>='a'&&c<='z') out->set[(unsigned char)(c-('a'-'A'))] = 1;
                    if (c>='A'&&c<='Z') out->set[(unsigned char)(c+('a'-'A'))] = 1;
                }
            }
        }
    }
    *pi = i;
    return 1;
}

static inline int class_match(const CharClass* cl, unsigned char ch){
    int in = cl->set[ch] ? 1 : 0;
    return cl->neg ? !in : in;
}

/* ===================== Сопоставление ===================== */

// Якоря: начало строки?
static inline int is_bol(const char* s, int sn, int pos, RFlags f){
    if (pos == 0) return 1;
    if (f.mline && pos > 0 && s[pos-1] == '\n') return 1;
    return 0;
}
// Якоря: конец строки?
static inline int is_eol(const char* s, int sn, int pos, RFlags f){
    if (pos == sn) return 1;
    if (f.mline && pos < sn && s[pos] == '\n') return 1;
    return 0;
}

// Сопоставление "одного атома" (символ/точка/класс) с одним байтом из строки
typedef enum { AT_LIT, AT_DOT, AT_CLASS } AtomKind;
typedef struct {
    AtomKind k;
    unsigned char lit;
    CharClass cls;
} Atom;

static int parse_atom(const char* p, int pn, int *pi, RFlags f, Atom* out){
    int i = *pi;
    if (i >= pn) return 0;

    if (p[i] == '.'){
        out->k = AT_DOT;
        i++;
        *pi = i;
        return 1;
    }
    if (p[i] == '['){
        out->k = AT_CLASS;
        parse_class(p, pn, &i, f, &out->cls);
        *pi = i;
        return 1;
    }
    if (p[i] == '\\' && i+1 < pn){
        out->k = AT_LIT;
        out->lit = (unsigned char)p[i+1];
        i += 2;
        *pi = i;
        return 1;
    }
    // обычный литерал
    out->k = AT_LIT;
    out->lit = (unsigned char)p[i++];
    *pi = i;
    return 1;
}

static inline int atom_matches(const Atom* a, unsigned char ch, RFlags f){
    if (a->k == AT_DOT){
        if (f.dotall) return 1;
        return ch != '\n';
    } else if (a->k == AT_LIT){
        return eq_ch(ch, a->lit, f.icase);
    } else { // CLASS
        return class_match(&a->cls, ch);
    }
}

// Рекурсивное сопоставление от позиции (pi, si). Возвращает 1/0 и end_out (конец совпадения).
static int match_here(const char* p, int pn, int pi,
                      const char* s, int sn, int si,
                      RFlags f, int* end_out)
{
    // конец шаблона — успех
    if (pi >= pn){ *end_out = si; return 1; }

    // якорь $ в конце шаблона
    if (p[pi] == '$' && (pi+1) >= pn){
        if (is_eol(s, sn, si, f)){ *end_out = si; return 1; }
        return 0;
    }

    // обычный атом (+ квантификатор)
    Atom at;
    int pi_start = pi;
    if (!parse_atom(p, pn, &pi, f, &at)) return 0;

    // квантификатор?
    char q = 0;
    if (pi < pn){
        if (p[pi] == '*' || p[pi] == '+' || p[pi] == '?'){
            q = p[pi]; pi++;
        }
    }

    if (q == '*'){
        // максимально возможное число повторов
        int k = 0, pos = si;
        while (pos < sn && atom_matches(&at, (unsigned char)s[pos], f)){ pos++; k++; }
        // жадный бэктрекинг
        for (int take = k; take >= 0; --take){
            int si2 = si + take;
            int end2=0;
            if (match_here(p, pn, pi, s, sn, si2, f, &end2)){ *end_out = end2; return 1; }
        }
        return 0;
    } else if (q == '+'){
        // нужно хотя бы 1
        if (!(si < sn && atom_matches(&at, (unsigned char)s[si], f))) return 0;
        int pos = si+1;
        while (pos < sn && atom_matches(&at, (unsigned char)s[pos], f)) pos++;
        for (int take = pos - si; take >= 1; --take){
            int si2 = si + take;
            int end2=0;
            if (match_here(p, pn, pi, s, sn, si2, f, &end2)){ *end_out = end2; return 1; }
        }
        return 0;
    } else if (q == '?'){
        // либо 1, либо 0
        if (si < sn && atom_matches(&at, (unsigned char)s[si], f)){
            int end2=0;
            if (match_here(p, pn, pi, s, sn, si+1, f, &end2)){ *end_out = end2; return 1; }
        }
        return match_here(p, pn, pi, s, sn, si, f, end_out);
    } else {
        // один символ обязателен
        if (si < sn && atom_matches(&at, (unsigned char)s[si], f)){
            return match_here(p, pn, pi, s, sn, si+1, f, end_out);
        }
        return 0;
    }
}

// Поиск первого совпадения: выдаёт start/end; учитывает ^ и многострочность
static int find_first(const char* p, int pn,
                      const char* s, int sn,
                      int start_from,
                      RFlags f,
                      int* out_start, int* out_end)
{
    int pi0 = 0;
    int anchored = 0;
    if (pn > 0 && p[0] == '^'){ anchored = 1; pi0 = 1; }

    if (anchored){
        // только BOL-позиции
        for (int pos = start_from; pos <= sn; ++pos){
            if (!is_bol(s, sn, pos, f)) {
                // если m-режим выключен — BOL только в 0; ускорим
                if (!f.mline) { if (pos>0) return 0; }
                continue;
            }
            int end=0;
            if (match_here(p, pn, pi0, s, sn, pos, f, &end)){
                *out_start = pos; *out_end = end; return 1;
            }
            if (!f.mline) break; // при строгом якоре и без m дальше BOL нет
        }
        return 0;
    } else {
        for (int pos = start_from; pos <= sn; ++pos){
            int end=0;
            if (match_here(p, pn, 0, s, sn, pos, f, &end)){
                *out_start = pos; *out_end = end; return 1;
            }
        }
        return 0;
    }
}

/* ===================== Публичные функции ===================== */

// регвыр_совпадает(шаблон, строка, [флаги]) -> лог
static Value rx_match(int argc, const Value* a){
    if (argc < 2 || a[0].kind != VAL_STR || a[1].kind != VAL_STR) return v_bool(false);
    RFlags f = parse_flags_opt(argc, a, 2);
    const char* p = a[0].as.str.s; int pn = a[0].as.str.n;
    const char* s = a[1].as.str.s; int sn = a[1].as.str.n;
    int st=0,en=0;
    int ok = find_first(p,pn,s,sn,0,f,&st,&en);
    return v_bool(ok ? true : false);
}

// регвыр_найти(шаблон, строка, [флаги]) -> [начало, конец] | nil
static Value rx_find(int argc, const Value* a){
    if (argc < 2 || a[0].kind != VAL_STR || a[1].kind != VAL_STR) return v_nil();
    RFlags f = parse_flags_opt(argc, a, 2);
    const char* p = a[0].as.str.s; int pn = a[0].as.str.n;
    const char* s = a[1].as.str.s; int sn = a[1].as.str.n;
    int st=0,en=0;
    if (!find_first(p,pn,s,sn,0,f,&st,&en)) return v_nil();
    Value L = v_list_new();
    v_list_push(&L, v_int(st));
    v_list_push(&L, v_int(en));
    return L;
}

static int rx_output_reserve(char** output,size_t* capacity,size_t length,size_t add){
    if (length==SIZE_MAX || add>SIZE_MAX-length-1) return 0;
    size_t needed=length+add+1;
    if (needed<=*capacity) return 1;
    size_t next=*capacity?*capacity:1024;
    while (next<needed){
        if (next>SIZE_MAX/2){ next=needed; break; }
        next*=2;
    }
    char* grown=(char*)kir_oom_realloc(*output,next);
    if (!grown) return 0;
    *output=grown;
    *capacity=next;
    return 1;
}

// регвыр_заменить(шаблон, строка, замена, [флаги]) -> строка (без бэкрефов; g=глобально)
static Value rx_replace(int argc, const Value* a){
    if (argc < 3 || a[0].kind != VAL_STR || a[1].kind != VAL_STR || a[2].kind != VAL_STR) return v_str_copy("",0);
    RFlags f = parse_flags_opt(argc, a, 3);
    const char* p = a[0].as.str.s; int pn = a[0].as.str.n;
    const char* s = a[1].as.str.s; int sn = a[1].as.str.n;
    const char* r = a[2].as.str.s; int rn = a[2].as.str.n;

    char* out = NULL; size_t cap=0, len=0;
    int pos = 0;
    if (sn==INT_MAX) return v_nil();
    int max_iter = sn + 1; // защита от пустых матчей

    while (pos <= sn && max_iter-- > 0){
        int st,en;
        if (!find_first(p,pn,s,sn,pos,f,&st,&en)){
            // дописать остаток
            size_t add = (size_t)(sn - pos);
            if (add){
                if (!rx_output_reserve(&out,&cap,len,add)){ free(out); return v_nil(); }
                memcpy(out + len, s + pos, add); len += add;
            }
            break;
        }
        // дописать кусок до начала совпадения
        if (st > pos){
            size_t add = (size_t)(st - pos);
            if (!rx_output_reserve(&out,&cap,len,add)){ free(out); return v_nil(); }
            memcpy(out + len, s + pos, add); len += add;
        }
        // вставить замену
        if (rn > 0){
            if (!rx_output_reserve(&out,&cap,len,(size_t)rn)){ free(out); return v_nil(); }
            memcpy(out + len, r, (size_t)rn); len += (size_t)rn;
        }
        // продвинуться
        if (en == st){ // пустое совпадение — сдвигаем на 1 байт, чтобы не зациклиться
            if (pos < sn){
                if (!rx_output_reserve(&out,&cap,len,1)){ free(out); return v_nil(); }
                out[len++] = s[pos];
                pos++;
            } else {
                // уже в конце
                break;
            }
        } else {
            pos = en;
        }
        if (!f.global){
            // дописать остаток и выходим
            size_t add = (size_t)(sn - pos);
            if (add){
                if (!rx_output_reserve(&out,&cap,len,add)){ free(out); return v_nil(); }
                memcpy(out + len, s + pos, add); len += add;
            }
            break;
        }
    }

    if (!out){
        // не было замен
        return v_str_copy(s, sn);
    }
    if (len>(size_t)INT_MAX){ free(out); return v_nil(); }
    out[len] = 0;
    Value V = v_str_copy(out, (int)len);
    free(out);
    return V;
}

/* ===================== Регистрация ===================== */

static const KirNativeDef REGEX_FUNCS[] = {
    {"регвыр_совпадает", rx_match},
    {"регвыр_найти",     rx_find},
    {"регвыр_заменить",  rx_replace},
};

void regex_init(void){
    kir_native_register(REGEX_FUNCS, (int)(sizeof(REGEX_FUNCS)/sizeof(REGEX_FUNCS[0])));
    kir_native_mark_bytecode_safe(REGEX_FUNCS, sizeof(REGEX_FUNCS)/sizeof(REGEX_FUNCS[0]));
}
