/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
// src/mod_json.c — JSON-модуль Киртона (парсинг и запись)
// API:
//   json_прочитать(s: STR) -> VALUE | nil
//   json_валиден(s: STR)   -> BOOL
//   json_написать(v: ANY, [indent: NUM]) -> STR
// Алиасы:
//   json_разобрать == json_прочитать
//   json_собрать   == json_написать
//
// Соответствия типов:
//   JSON null       -> nil
//   JSON true/false -> лог
//   JSON number     -> число (double)
//   JSON string     -> строка (UTF-8)
//   JSON array      -> список
//   JSON object     -> словарь (ключи — строки)
//
// Особенности:
//  - Полная поддержка \uXXXX и суррогатных пар (\uD83D\uDE00).
//  - stringify: компактно по умолчанию; при indent>0 — красивый вывод.
//
// Подстроено под твой VDict из include/kirton/value.h:
//   typedef struct { char** keys; size_t* kn; Value* vals; size_t len; size_t cap; } VDict;

#include "kirton/native.h"
#include "native_internal.h"
#include "kirton/value.h"
#include "kirton/common.h"
#include "kirton/diag.h"
#include "kirton/common.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <limits.h>

/* ---------- Макро-доступ к словарю (конкретно под твою структуру) ---------- */
#ifndef KIR_DICT_LEN
#  define KIR_DICT_LEN(v)          ((v).as.dict->len)
#endif
#ifndef KIR_DICT_KEY_AT
#  define KIR_DICT_KEY_AT(v,i)     ((v).as.dict->keys[(i)])
#endif
#ifndef KIR_DICT_KEYLEN_AT
#  define KIR_DICT_KEYLEN_AT(v,i)  ((v).as.dict->kn[(i)])
#endif
#ifndef KIR_DICT_VAL_AT
#  define KIR_DICT_VAL_AT(v,i)     ((v).as.dict->vals[(i)])
#endif

/* ---------- Мелкий string builder ---------- */

typedef struct {
    char*  s;
    size_t n;
    size_t cap;
} SB;

static int sb_reserve(SB* b, size_t add){
    if (b->n + add + 1 <= b->cap) return 1;
    size_t nc = b->cap ? b->cap*2 : 256;
    while (nc < b->n + add + 1) nc *= 2;
    char* ns = (char*)kir_oom_realloc(b->s, nc);
    if (!ns) return 0;
    b->s = ns; b->cap = nc;
    return 1;
}
static int sb_putc(SB* b, char c){
    if (!sb_reserve(b,1)) return 0;
    b->s[b->n++] = c; b->s[b->n] = 0; return 1;
}
static int sb_puts(SB* b, const char* s, size_t n){
    if (!sb_reserve(b,n)) return 0;
    memcpy(b->s + b->n, s, n);
    b->n += n; b->s[b->n] = 0; return 1;
}

/* ---------- UTF-8 helpers ---------- */

static int utf8_from_codepoint(unsigned cp, char out[4]){
    if (cp <= 0x7F){ out[0]=(char)cp; return 1; }
    else if (cp <= 0x7FF){
        out[0]=(char)(0xC0 | ((cp>>6)&0x1F));
        out[1]=(char)(0x80 | (cp & 0x3F)); return 2;
    } else if (cp <= 0xFFFF){
        out[0]=(char)(0xE0 | ((cp>>12)&0x0F));
        out[1]=(char)(0x80 | ((cp>>6) &0x3F));
        out[2]=(char)(0x80 | (cp & 0x3F)); return 3;
    } else if (cp <= 0x10FFFF){
        out[0]=(char)(0xF0 | ((cp>>18)&0x07));
        out[1]=(char)(0x80 | ((cp>>12)&0x3F));
        out[2]=(char)(0x80 | ((cp>>6) &0x3F));
        out[3]=(char)(0x80 | (cp & 0x3F)); return 4;
    }
    return 0;
}

/* ---------- Парсер ---------- */

typedef struct {
    const char* s;  /* начало */
    const char* p;  /* текущее */
    const char* e;  /* конец (s + n) */
    unsigned depth;
} J;

#define JSON_MAX_DEPTH 256u

static void j_skip_ws(J* j){
    while (j->p < j->e && (*j->p==' ' || *j->p=='\t' || *j->p=='\n' || *j->p=='\r')) j->p++;
}
static int j_match(J* j, char c){ if (j->p<j->e && *j->p==c){ j->p++; return 1; } return 0; }
static int j_starts(J* j, const char* k){
    const char* p=j->p;
    while (*k && p<j->e && *p==*k){ p++; k++; }
    if (*k==0){ j->p=p; return 1; }
    return 0;
}

static int hexval(char c){
    if (c>='0'&&c<='9') return c-'0';
    if (c>='a'&&c<='f') return 10 + (c-'a');
    if (c>='A'&&c<='F') return 10 + (c-'A');
    return -1;
}

static int j_parse_string(J* j, Value* out){
    if (!j_match(j, '"')) return 0;
    SB b={0};
    while (j->p < j->e){
        char c = *j->p++;
        if (c=='"') {
            *out = v_str_copy(b.s?b.s:"", (int)((b.n > (size_t)INT_MAX) ? INT_MAX : (int)b.n));
            free(b.s);
            return 1;
        }
        if (c=='\\'){
            if (j->p >= j->e) break;
            char esc = *j->p++;
            switch (esc){
                case '"':  if(!sb_putc(&b,'"')) goto oom;  break;
                case '\\': if(!sb_putc(&b,'\\')) goto oom; break;
                case '/':  if(!sb_putc(&b,'/')) goto oom;  break;
                case 'b':  if(!sb_putc(&b,'\b')) goto oom; break;
                case 'f':  if(!sb_putc(&b,'\f')) goto oom; break;
                case 'n':  if(!sb_putc(&b,'\n')) goto oom; break;
                case 'r':  if(!sb_putc(&b,'\r')) goto oom; break;
                case 't':  if(!sb_putc(&b,'\t')) goto oom; break;
                case 'u': {
                    if (j->e - j->p < 4) goto fail;
                    int h0=hexval(j->p[0]), h1=hexval(j->p[1]), h2=hexval(j->p[2]), h3=hexval(j->p[3]);
                    if (h0<0||h1<0||h2<0||h3<0) goto fail;
                    unsigned cp = (unsigned)((h0<<12)|(h1<<8)|(h2<<4)|h3);
                    j->p += 4;
                    /* суррогаты */
                    if (cp>=0xD800 && cp<=0xDBFF){
                        const char* save = j->p;
                        if (j->e - j->p >= 6 && j->p[0]=='\\' && j->p[1]=='u'){
                            int h4=hexval(j->p[2]), h5=hexval(j->p[3]), h6=hexval(j->p[4]), h7=hexval(j->p[5]);
                            if (h4>=0&&h5>=0&&h6>=0&&h7>=0){
                                unsigned cp2 = (unsigned)((h4<<12)|(h5<<8)|(h6<<4)|h7);
                                if (cp2>=0xDC00 && cp2<=0xDFFF){
                                    j->p += 6;
                                    unsigned H = cp, L = cp2;
                                    cp = 0x10000 + (((H-0xD800)<<10) | (L-0xDC00));
                                } else {
                                    j->p = save; /* оставим одиночный старший суррогат как есть */
                                }
                            }
                        }
                    }
                    char u8[4]; int m = utf8_from_codepoint(cp, u8);
                    if (m<=0 || !sb_puts(&b, u8, (size_t)m)) goto oom;
                } break;
                default: goto fail;
            }
        } else {
            if ((unsigned char)c < 0x20) goto fail;
            if (!sb_putc(&b, c)) goto oom;
        }
    }
fail:
    free(b.s);
    *out = v_nil();
    return 0;
oom:
    free(b.s);
    *out = v_nil();
    return 0;
}

static int j_parse_number(J* j, Value* out){
    const char* start = j->p;
    if (j->p<j->e && *j->p=='-') j->p++;
    if (j->p>=j->e) return 0;
    if (*j->p=='0'){ j->p++; }
    else {
        if (!isdigit((unsigned char)*j->p)) return 0;
        while (j->p<j->e && isdigit((unsigned char)*j->p)) j->p++;
    }
    if (j->p<j->e && *j->p=='.'){
        j->p++;
        if (j->p>=j->e || !isdigit((unsigned char)*j->p)) return 0;
        while (j->p<j->e && isdigit((unsigned char)*j->p)) j->p++;
    }
    if (j->p<j->e && (*j->p=='e' || *j->p=='E')){
        j->p++;
        if (j->p<j->e && (*j->p=='+' || *j->p=='-')) j->p++;
        if (j->p>=j->e || !isdigit((unsigned char)*j->p)) return 0;
        while (j->p<j->e && isdigit((unsigned char)*j->p)) j->p++;
    }
    size_t n = (size_t)(j->p - start);
    char* tmp = (char*)kir_oom_malloc(n+1);
    if (!tmp) { *out=v_num(0); return 0; }
    memcpy(tmp, start, n); tmp[n]=0;
    char* endptr=NULL;
    int is_integer = strchr(tmp, '.') == NULL && strchr(tmp, 'e') == NULL && strchr(tmp, 'E') == NULL;
    Value v = v_nil();
    if (is_integer){
        v=v_int_parse(tmp,n,10);
    } else {
        double number=strtod(tmp,&endptr);
        if (!isfinite(number)){ free(tmp); return 0; }
        v=v_num(number);
    }
    free(tmp);
    (void)endptr;
    *out = v;
    return 1;
}

static int j_parse_value(J* j, Value* out); /* fwd */

static int j_parse_array(J* j, Value* out){
    if (!j_match(j, '[')) return 0;
    Value L = v_list_new();
    j_skip_ws(j);
    if (j_match(j, ']')){ *out=L; return 1; }
    for (;;){
        j_skip_ws(j);
        Value v;
        if (!j_parse_value(j, &v)){ return 0; }
        v_list_push(&L, v);
        j_skip_ws(j);
        if (j_match(j, ']')){ *out=L; return 1; }
        if (!j_match(j, ',')) return 0;
    }
}

static int j_parse_object(J* j, Value* out){
    if (!j_match(j, '{')) return 0;
    Value D = v_dict_new();
    j_skip_ws(j);
    if (j_match(j, '}')){ *out=D; return 1; }
    for (;;){
        j_skip_ws(j);
        Value k;
        if (!j_parse_string(j, &k)) return 0;
        j_skip_ws(j);
        if (!j_match(j, ':')) return 0;
        j_skip_ws(j);
        Value v;
        if (!j_parse_value(j, &v)) return 0;
        v_dict_set(&D, k.as.str.s, (size_t)k.as.str.n, v);
        j_skip_ws(j);
        if (j_match(j, '}')){ *out=D; return 1; }
        if (!j_match(j, ',')) return 0;
    }
}

static int j_parse_value(J* j, Value* out){
    j_skip_ws(j);
    if (j->p>=j->e) return 0;
    char c = *j->p;
    if (c=='"') return j_parse_string(j,out);
    if (c=='{' || c=='['){
        if (j->depth>=JSON_MAX_DEPTH) return 0;
        ++j->depth;
        int ok=c=='{' ? j_parse_object(j,out) : j_parse_array(j,out);
        --j->depth;
        return ok;
    }
    if (c=='t' ) { if (j_starts(j,"true"))  { *out=v_bool(true);  return 1; } return 0; }
    if (c=='f' ) { if (j_starts(j,"false")) { *out=v_bool(false); return 1; } return 0; }
    if (c=='n' ) { if (j_starts(j,"null"))  { *out=v_nil();       return 1; } return 0; }
    return j_parse_number(j,out);
}

/* json_прочитать(s) */
static Value fn_json_read(int argc, const Value* a){
    if (argc<1 || a[0].kind!=VAL_STR) return v_nil();
    J j = { a[0].as.str.s, a[0].as.str.s, a[0].as.str.s + a[0].as.str.n, 0 };
    Value v;
    if (!j_parse_value(&j, &v)) return v_nil();
    j_skip_ws(&j);
    if (j.p != j.e) return v_nil(); /* лишний мусор в конце */
    return v;
}

/* json_валиден(s) */
static Value fn_json_valid(int argc, const Value* a){
    if (argc<1 || a[0].kind!=VAL_STR) return v_bool(false);
    J j = { a[0].as.str.s, a[0].as.str.s, a[0].as.str.s + a[0].as.str.n, 0 };
    Value v;
    if (!j_parse_value(&j, &v)) return v_bool(false);
    j_skip_ws(&j);
    return v_bool(j.p == j.e);
}

/* ---------- Запись JSON ---------- */

static int json_write_string(SB* b, const char* s, int n){
    if (!sb_putc(b, '"')) return 0;
    for (int i=0;i<n;i++){
        unsigned char c = (unsigned char)s[i];
        switch (c){
            case '"':  if(!sb_puts(b,"\\\"",2)) return 0; break;
            case '\\': if(!sb_puts(b,"\\\\",2)) return 0; break;
            case '\b': if(!sb_puts(b,"\\b",2)) return 0; break;
            case '\f': if(!sb_puts(b,"\\f",2)) return 0; break;
            case '\n': if(!sb_puts(b,"\\n",2)) return 0; break;
            case '\r': if(!sb_puts(b,"\\r",2)) return 0; break;
            case '\t': if(!sb_puts(b,"\\t",2)) return 0; break;
            default:
                if (c < 0x20){
                    char esc[6];
                    esc[0]='\\'; esc[1]='u'; esc[2]='0'; esc[3]='0';
                    const char* hex="0123456789ABCDEF";
                    esc[4]=hex[(c>>4)&0xF]; esc[5]=hex[c&0xF];
                    if(!sb_puts(b,esc,6)) return 0;
                }else{
                    if(!sb_putc(b,(char)c)) return 0;
                }
        }
    }
    return sb_putc(b, '"');
}

static int json_write_value_rec(SB* b, const Value* v, int indent, int depth);

static int json_indent(SB* b, int indent, int depth){
    if (indent<=0) return 1;
    if (!sb_putc(b, '\n')) return 0;
    for (int i=0;i<depth*indent;i++) if(!sb_putc(b,' ')) return 0;
    return 1;
}

static int json_write_list(SB* b, const Value* v, int indent, int depth){
    if (!sb_putc(b, '[')) return 0;
    size_t n = v->as.list->len;
    if (n==0) return sb_putc(b, ']');
    for (size_t i=0;i<n;i++){
        if (i==0){
            if (indent>0 && !json_indent(b, indent, depth+1)) return 0;
        }else{
            if (!sb_putc(b, ',')) return 0;
            if (indent>0 && !json_indent(b, indent, depth+1)) return 0;
            if (indent==0 && !sb_putc(b,' ')) return 0;
        }
        if (!json_write_value_rec(b, &v->as.list->data[i], indent, depth+1)) return 0;
    }
    if (indent>0 && !json_indent(b, indent, depth)) return 0;
    return sb_putc(b, ']');
}

static int json_write_dict(SB* b, const Value* v, int indent, int depth){
    if (!sb_putc(b, '{')) return 0;
    size_t n = KIR_DICT_LEN((*v));
    if (n==0) return sb_putc(b, '}');
    for (size_t i=0;i<n;i++){
        const char* key   = KIR_DICT_KEY_AT((*v), i);
        size_t      klenS = KIR_DICT_KEYLEN_AT((*v), i);
        int         klen  = (int)(klenS > (size_t)INT_MAX ? INT_MAX : (int)klenS);

        if (i==0){
            if (indent>0 && !json_indent(b, indent, depth+1)) return 0;
        }else{
            if (!sb_putc(b, ',')) return 0;
            if (indent>0 && !json_indent(b, indent, depth+1)) return 0;
            if (indent==0 && !sb_putc(b,' ')) return 0;
        }
        /* ключ */
        if (!json_write_string(b, key, klen)) return 0;
        if (!sb_putc(b, ':')) return 0;
        if (!sb_putc(b,' ')) return 0;
        /* значение */
        if (!json_write_value_rec(b, &KIR_DICT_VAL_AT((*v), i), indent, depth+1)) return 0;
    }
    if (indent>0 && !json_indent(b, indent, depth)) return 0;
    return sb_putc(b, '}');
}

static int json_write_value_rec(SB* b, const Value* v, int indent, int depth){
    switch (v->kind){
        case VAL_NIL:  return sb_puts(b, "null", 4);
        case VAL_BOOL: return v->as.boolean ? sb_puts(b,"true",4) : sb_puts(b,"false",5);
        case VAL_NUM: {
            if (!v->is_integer && !isfinite(v->as.num)){
                kir_diag_error("<json>",1,1,"NaN и бесконечность нельзя сериализовать в JSON");
                return 0;
            }
            size_t n = 0;
            char* text = v_num_to_string(*v, &n);
            int ok = sb_puts(b, text, n);
            kir_free(text);
            return ok;
        }
        case VAL_STR:
            return json_write_string(b, v->as.str.s, v->as.str.n);
        case VAL_LIST:
            return json_write_list(b, v, indent, depth);
        case VAL_DICT:
            return json_write_dict(b, v, indent, depth);
        case VAL_ARENA:
        case VAL_BUFFER:
        case VAL_TYPED_ARRAY:
        case VAL_RECORD_LAYOUT:
        case VAL_RECORD:
        case VAL_NATIVE_RESOURCE:
        case VAL_FUNCTION:
        case VAL_USER_STRUCT:
        case VAL_ENUM_VALUE:
            kir_diag_error("<json>",1,1,"Области памяти, буферы, массивы, записи и внешние ресурсы нельзя сериализовать в JSON");
            return 0;
        default:
            return sb_puts(b,"null",4);
    }
}

/* json_написать(v, [indent]) */
static Value fn_json_write(int argc, const Value* a){
    if (argc<1) return v_str_copy("null",4);
    int indent = 0;
    if (argc>=2 && a[1].kind==VAL_NUM){
        int64_t requested = 0;
        if (!v_num_fits_i64(a[1], &requested)) return v_str_copy("",0);
        int t = requested > INT_MAX ? INT_MAX : requested < INT_MIN ? INT_MIN : (int)requested;
        if (t<0) t=0; if (t>8) t=8;
        indent = t;
    }
    SB b={0};
    if (!json_write_value_rec(&b, &a[0], indent, 0)){
        free(b.s);
        return v_str_copy("",0);
    }
    Value out = v_str_copy(b.s?b.s:"", (int)((b.n > (size_t)INT_MAX) ? INT_MAX : (int)b.n));
    free(b.s);
    return out;
}

/* ---------- Регистрация ---------- */

static const KirNativeDef JSON_FUNCS[] = {
    {"json_прочитать", fn_json_read},
    {"json_валиден",   fn_json_valid},
    {"json_написать",  fn_json_write},
    /* алиасы */
    {"json_разобрать", fn_json_read},
    {"json_собрать",   fn_json_write},
};

void json_init(void){
    kir_native_register(JSON_FUNCS, (int)(sizeof(JSON_FUNCS)/sizeof(JSON_FUNCS[0])));
    kir_native_mark_bytecode_safe(JSON_FUNCS, sizeof(JSON_FUNCS)/sizeof(JSON_FUNCS[0]));
}
