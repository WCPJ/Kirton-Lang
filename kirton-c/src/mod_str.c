/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
// src/mod_str.c — Киртон: строковые нативы (все имена на кириллице)

#include "kirton/native.h"
#include "native_internal.h"
#include "kirton/value.h"
#include "kirton/common.h"
#include "kirton/utf8.h"   // для корректного подсчёта длины в UTF-8
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* ===== Вспомогательные ===== */

static const char* find_bytes(const char* source,size_t source_len,
                              const char* needle,size_t needle_len){
    if (!needle_len || needle_len>source_len) return NULL;
    size_t last=source_len-needle_len;
    for (size_t i=0;i<=last;++i)
        if (memcmp(source+i,needle,needle_len)==0) return source+i;
    return NULL;
}

/* Длина строки в Юникод-кодпоинтах (UTF-8). При битых байтах считаем как 1 символ. */
static int utf8_codepoint_len(const char* s, size_t n){
    if (!s) return 0;
    const char* p = s;
    const char* e = s + n;
    int count = 0;
    while (p < e){
        uint32_t cp = 0;
        int adv = kir_utf8_decode(p, e, &cp);
        if (adv <= 0){ p += 1; count += 1; } else { p += adv; count += 1; }
    }
    return count;
}

/* ===== Базовые функции (сохранены) ===== */

/* строка_найти(s, sub) -> число (позиция в байтах; нет -> -1) */
static Value строка_найти_fn(int argc, const Value* argv){
    if (argc < 2 || argv[0].kind != VAL_STR || argv[1].kind != VAL_STR) return v_int(-1);
    const char* s   = argv[0].as.str.s;
    const char* sub = argv[1].as.str.s;
    if (argv[1].as.str.n == 0) return v_int(0);
    const char* p = strstr(s, sub);
    if (!p) return v_int(-1);
    return v_int((int64_t)(p - s));
}

/* строка_заменить(s, old, new) — заменить все вхождения */
static Value строка_заменить_fn(int argc, const Value* argv){
    if (argc < 3 || argv[0].kind != VAL_STR || argv[1].kind != VAL_STR || argv[2].kind != VAL_STR) return v_str_copy("",0);
    const char* s   = argv[0].as.str.s;
    const char* old = argv[1].as.str.s;
    const char* neu = argv[2].as.str.s;
    size_t sN = argv[0].as.str.n, oN = argv[1].as.str.n, nN = argv[2].as.str.n;
    if (!s || !old || !neu) return v_str_copy("",0);
    if (sN>(size_t)INT_MAX) return v_str_copy("",0);
    if (oN==0) return v_str_copy(s,(int)sN);
    size_t count = 0,read_pos=0;
    while (read_pos<=sN){
        const char* match=find_bytes(s+read_pos,sN-read_pos,old,oN);
        if (!match) break;
        ++count;
        read_pos=(size_t)(match-s)+oN;
    }
    if (count == 0) return v_str_copy(s, (int)sN);
    size_t outN=0;
    if (nN>=oN){
        size_t growth=nN-oN;
        if (growth && count>(SIZE_MAX-sN)/growth) return v_str_copy("",0);
        outN=sN+count*growth;
    } else {
        size_t shrink=oN-nN;
        if (shrink && count>sN/shrink) return v_str_copy("",0);
        outN=sN-count*shrink;
    }
    if (outN>(size_t)INT_MAX) return v_str_copy("",0);
    char* out = (char*)kir_oom_malloc(outN+1);
    if (!out) return v_str_copy("",0);
    size_t src_pos=0,dst_pos=0;
    while (src_pos<=sN){
        const char* match=find_bytes(s+src_pos,sN-src_pos,old,oN);
        if (!match) break;
        size_t prefix=(size_t)(match-(s+src_pos));
        if (prefix) memcpy(out+dst_pos,s+src_pos,prefix);
        dst_pos+=prefix;
        if (nN) memcpy(out+dst_pos,neu,nN);
        dst_pos+=nN;
        src_pos=(size_t)(match-s)+oN;
    }
    if (src_pos<sN){
        size_t tail=sN-src_pos;
        memcpy(out+dst_pos,s+src_pos,tail);
        dst_pos+=tail;
    }
    Value v = v_str_copy(out,(int)dst_pos);
    free(out);
    return v;
}

/* строка_обрезать(s) — trim ASCII-пробелов по краям */
static Value строка_обрезать_fn(int argc, const Value* argv){
    if (argc < 1 || argv[0].kind != VAL_STR) return v_str_copy("",0);
    const char* s = argv[0].as.str.s; size_t n = argv[0].as.str.n;
    const char* a = s; const char* b = s + n;
    while (a < b && isspace((unsigned char)*a)) ++a;
    while (b > a && isspace((unsigned char)b[-1])) --b;
    return v_str_copy(a, (int)(b - a));
}

/* начинается_с(s, pfx) */
static Value начинается_с_fn(int argc, const Value* argv){
    if (argc < 2 || argv[0].kind != VAL_STR || argv[1].kind != VAL_STR) return v_bool(false);
    const char* s = argv[0].as.str.s; size_t ns = argv[0].as.str.n;
    const char* p = argv[1].as.str.s; size_t np = argv[1].as.str.n;
    if (np > ns) return v_bool(false);
    return v_bool(memcmp(s, p, np) == 0);
}

/* заканчивается_на(s, sfx) */
static Value заканчивается_на_fn(int argc, const Value* argv){
    if (argc < 2 || argv[0].kind != VAL_STR || argv[1].kind != VAL_STR) return v_bool(false);
    const char* s = argv[0].as.str.s; size_t ns = argv[0].as.str.n;
    const char* p = argv[1].as.str.s; size_t np = argv[1].as.str.n;
    if (np > ns) return v_bool(false);
    return v_bool(memcmp(s + ns - np, p, np) == 0);
}

/* строка_разделить(s, sep) -> список */
static Value строка_разделить_fn(int argc, const Value* argv){
    if (argc < 2 || argv[0].kind != VAL_STR || argv[1].kind != VAL_STR) return v_list_new();
    const char* s = argv[0].as.str.s; size_t n = argv[0].as.str.n;
    const char* sep = argv[1].as.str.s; size_t sn = argv[1].as.str.n;
    Value L = v_list_new();
    if (sn == 0){ v_list_push(&L, v_str_copy(s, (int)n)); return L; }
    const char* r = s;
    while (1){
        const char* p = strstr(r, sep);
        if (!p){ v_list_push(&L, v_str_copy(r, (int)strlen(r))); break; }
        size_t len = (size_t)(p - r);
        v_list_push(&L, v_str_copy(r, (int)len));
        r = p + sn;
    }
    return L;
}

/* ===== Новые функции (всё по-русски) ===== */

/* строка_длина(s) — длина в кодпоинтах UTF-8 */
static Value строка_длина_fn(int argc, const Value* argv){
    if (argc < 1 || argv[0].kind != VAL_STR) return v_int(0);
    return v_int((int64_t)utf8_codepoint_len(argv[0].as.str.s, (size_t)argv[0].as.str.n));
}

/* срез(s, from, len) / подстрока(s, from, len) — байтовые границы, безопасно */
static Value срез_fn(int argc, const Value* argv){
    if (argc < 3 || argv[0].kind != VAL_STR || argv[1].kind != VAL_NUM || argv[2].kind != VAL_NUM) return v_str_copy("",0);
    const char* s = argv[0].as.str.s; int n = argv[0].as.str.n;
    int64_t from_value = 0, len_value = 0;
    if (!v_num_fits_i64(argv[1], &from_value) || !v_num_fits_i64(argv[2], &len_value)) return v_str_copy("",0);
    if (from_value < 0) from_value = 0;
    if (len_value < 0) len_value = 0;
    int from = from_value > n ? n : (int)from_value;
    int len = len_value > n - from ? n - from : (int)len_value;
    return v_str_copy(s + from, len);
}

/* строка_склеить(список, sep="") — join элементов списка (только строки) */
static Value строка_склеить_fn(int argc, const Value* argv){
    if (argc < 1 || argv[0].kind != VAL_LIST) return v_str_copy("", 0);
    const char* sep = "";
    size_t sep_n = 0;
    if (argc >= 2 && argv[1].kind == VAL_STR){ sep = argv[1].as.str.s; sep_n = argv[1].as.str.n; }
    if (sep_n && !sep) return v_str_copy("",0);

    VList L = *argv[0].as.list;
    size_t total = 0,parts = 0;
    for (size_t i=0;i<L.len;i++){
        if (L.data[i].kind == VAL_STR){
            size_t item_len=L.data[i].as.str.n;
            if (item_len && !L.data[i].as.str.s) return v_str_copy("",0);
            if (item_len>SIZE_MAX-total) return v_str_copy("",0);
            total += item_len;
            if (parts==SIZE_MAX) return v_str_copy("",0);
            parts++;
        }
    }
    if (parts>1){
        size_t separators=0;
        if (kir_mul_overflow_size(sep_n,parts-1,&separators) || separators>SIZE_MAX-total)
            return v_str_copy("",0);
        total+=separators;
    }
    if (total>(size_t)INT_MAX) return v_str_copy("",0);

    char* out = (char*)kir_oom_malloc(total + 1);
    if (!out) return v_str_copy("", 0);

    size_t pos = 0,em = 0;
    for (size_t i=0;i<L.len;i++){
        if (L.data[i].kind != VAL_STR) continue;
        if (em > 0 && sep_n > 0){ memcpy(out + pos, sep, sep_n); pos += sep_n; }
        size_t item_len=L.data[i].as.str.n;
        if (item_len) memcpy(out + pos, L.data[i].as.str.s, item_len);
        pos += item_len;
        em++;
    }
    out[pos] = 0;
    Value v = v_str_copy(out, (int)pos);
    free(out);
    return v;
}

/* в_верхнем(s) — ASCII uppercase; байты >=128 не трогаем */
static Value в_верхнем_fn(int argc, const Value* argv){
    if (argc < 1 || argv[0].kind != VAL_STR) return v_str_copy("", 0);
    const char* s = argv[0].as.str.s; size_t n = argv[0].as.str.n;
    if (!s || n>(size_t)INT_MAX) return v_str_copy("",0);
    char* out = (char*)kir_oom_malloc(n + 1); if (!out) return v_str_copy("",0);
    for (size_t i=0;i<n;i++){ unsigned char ch = (unsigned char)s[i]; out[i] = (ch < 128) ? (char)toupper(ch) : (char)ch; }
    out[n] = 0;
    Value v = v_str_copy(out, (int)n);
    free(out);
    return v;
}

/* в_нижнем(s) — ASCII lowercase; байты >=128 не трогаем */
static Value в_нижнем_fn(int argc, const Value* argv){
    if (argc < 1 || argv[0].kind != VAL_STR) return v_str_copy("", 0);
    const char* s = argv[0].as.str.s; size_t n = argv[0].as.str.n;
    if (!s || n>(size_t)INT_MAX) return v_str_copy("",0);
    char* out = (char*)kir_oom_malloc(n + 1); if (!out) return v_str_copy("",0);
    for (size_t i=0;i<n;i++){ unsigned char ch = (unsigned char)s[i]; out[i] = (ch < 128) ? (char)tolower(ch) : (char)ch; }
    out[n] = 0;
    Value v = v_str_copy(out, (int)n);
    free(out);
    return v;
}

/* ===== Регистрация: только РУССКИЕ имена ===== */

static const KirNativeDef STR_FUNCS[] = {
    {"строка_найти",        строка_найти_fn},
    {"строка_заменить",     строка_заменить_fn},
    {"строка_разделить",    строка_разделить_fn},
    {"строка_обрезать",     строка_обрезать_fn},
    {"начинается_с",        начинается_с_fn},
    {"заканчивается_на",    заканчивается_на_fn},

    {"строка_длина",        строка_длина_fn},
    {"срез",                срез_fn},
    {"подстрока",           срез_fn},          // синоним, но тоже кириллицей
    {"строка_склеить",      строка_склеить_fn},
    {"в_верхнем",           в_верхнем_fn},
    {"в_нижнем",            в_нижнем_fn},
};

void str_init(void){
    kir_native_register(STR_FUNCS, (int)(sizeof(STR_FUNCS)/sizeof(STR_FUNCS[0])));
    kir_native_mark_bytecode_safe(STR_FUNCS, sizeof(STR_FUNCS)/sizeof(STR_FUNCS[0]));
}
