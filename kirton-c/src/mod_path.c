/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
// src/mod_path.c — модуль путей Киртона (кроссплатформенно, UTF-8)
// Экспорт: путь_соединить, расширение, имя, папка, нормализовать

#include "kirton/native.h"
#include "native_internal.h"
#include "kirton/value.h"
#include "kirton/common.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* ===== Helpers ===== */

static inline char* k_dup_n(const char* s, size_t n){
    if (n==SIZE_MAX) return NULL;
    char* r = (char*)kir_oom_malloc(n + 1);
    if (!r) return NULL;
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

static inline int is_sep(char c){ return c=='/' || c=='\\'; }

#ifdef _WIN32
  #define SEP '\\'
#else
  #define SEP '/'
#endif

/* Detect Windows drive letter like "C:" */
static int win_drive_len(const char* s, size_t n){
#ifdef _WIN32
    if (n >= 2 && ((s[0]>='A'&&s[0]<='Z')||(s[0]>='a'&&s[0]<='z')) && s[1]==':') return 2;
#else
    (void)s;(void)n;
#endif
    return 0;
}

/* Detect UNC prefix: \\server\share\  -> returns length of the prefix in bytes (without trailing path) */
static size_t unc_prefix_len(const char* s, size_t n){
#ifdef _WIN32
    if (n >= 2 && s[0]=='\\' && s[1]=='\\'){
        /* \\server\share\[...]; find two segments (server, share) */
        size_t i = 2, segs = 0;
        /* consume any extra slashes after \\ */
        while (i < n && is_sep(s[i])) i++;
        while (i < n){
            if (is_sep(s[i])){
                segs++;
                while (i < n && is_sep(s[i])) i++;
                if (segs >= 2) break;
            } else {
                i++;
            }
        }
        /* consume trailing slashes after \\server\share */
        while (i < n && is_sep(s[i])) i++;
        return i;
    }
#else
    (void)s;(void)n;
#endif
    return 0;
}

/* Push a segment onto a stack of segments (vector) */
typedef struct {
    char** items;
    size_t len, cap;
} SegVec;

static int seg_push(SegVec* v, const char* s, size_t n){
    if (n==0) return 1;
    if (v->len==SIZE_MAX) return 0;
    if (v->len + 1 > v->cap){
        size_t need=v->len+1;
        size_t nc;
        if (!v->cap) nc=8;
        else if (v->cap<=SIZE_MAX/2) nc=v->cap*2;
        else nc=need;
        if (nc<need) nc=need;
        size_t bytes=0;
        if (kir_mul_overflow_size(nc,sizeof(char*),&bytes)) return 0;
        char** ni = (char**)kir_oom_realloc(v->items,bytes);
        if (!ni) return 0;
        v->items = ni; v->cap = nc;
    }
    char* segment=k_dup_n(s,n);
    if (!segment) return 0;
    v->items[v->len++] = segment;
    return 1;
}

static void seg_pop(SegVec* v){
    if (v->len==0) return;
    char* s = v->items[--v->len];
    free(s);
}

static void seg_free(SegVec* v){
    for (size_t i=0;i<v->len;i++) free(v->items[i]);
    free(v->items);
    v->items=NULL; v->len=v->cap=0;
}

/* Normalize a path (without considering working directory).
   - Collapses repeated separators
   - Resolves "." segments
   - Resolves ".." segments (without crossing root/UNC/share/drive)
   - Preserves leading root/drive/UNC prefix
   - Uses platform-preferred output separator (SEP)
*/
static char* normalize_path(const char* in, size_t n){
    if (!in) return k_dup_n("", 0);
    const char* s = in;

    /* Identify prefix */
    size_t prefix_len = 0;
    int rooted = 0;

#ifdef _WIN32
    size_t unc = unc_prefix_len(s, n);
    if (unc){
        prefix_len = unc;
        rooted = 1;
    } else {
        int dl = win_drive_len(s, n);
        if (dl){
            prefix_len = (size_t)dl;
            if ((size_t)dl < n && is_sep(s[dl])) rooted = 1; /* C:\... */
        } else if (n>0 && is_sep(s[0])) {
            rooted = 1;
        }
    }
#else
    if (n>0 && s[0]=='/') rooted = 1;
#endif

    /* Collect segments */
    SegVec vec={0};
    size_t i = prefix_len;
    while (i<n && is_sep(s[i])) i++;

    while (i < n){
        size_t start = i;
        while (i < n && !is_sep(s[i])) i++;
        size_t seglen = i - start;

        if (seglen == 0){
            /* nothing */
        } else if (seglen==1 && s[start]=='.'){
            /* skip "." */
        } else if (seglen==2 && s[start]=='.' && s[start+1]=='.'){
            if (vec.len>0){
                seg_pop(&vec);
            } else {
                if (!rooted && !seg_push(&vec,"..",2)){
                    seg_free(&vec);
                    return NULL;
                }
            }
        } else {
            if (!seg_push(&vec,s+start,seglen)){
                seg_free(&vec);
                return NULL;
            }
        }

        while (i < n && is_sep(s[i])) i++;
    }

    /* Assemble output length */
    size_t out_len = 0;

#ifdef _WIN32
    out_len += prefix_len; /* we'll copy prefix verbatim (seps will be normalized) */
    int will_emit_leading_sep = rooted && (prefix_len == 0);
    if (will_emit_leading_sep) out_len += 1;
#else
    int will_emit_leading_sep = rooted;
    if (will_emit_leading_sep) out_len += 1;
#endif

    for (size_t k=0;k<vec.len;k++){
        int need_sep =
            (prefix_len > 0) ||
            (!will_emit_leading_sep ? (k > 0) : (k > 0));
        if (need_sep) out_len += 1;
        out_len += strlen(vec.items[k]);
    }

    if (!rooted && prefix_len==0 && vec.len==0){
        out_len = 1; /* "." */
    }

    if (out_len==SIZE_MAX){ seg_free(&vec); return NULL; }

    char* out = (char*)kir_oom_malloc(out_len + 1);
    if (!out){ seg_free(&vec); return NULL; }
    size_t pos = 0;

#ifdef _WIN32
    if (prefix_len){
        for (size_t j=0;j<prefix_len && j<n;j++){
            char c = s[j];
            out[pos++] = is_sep(c) ? SEP : c;
        }
    } else if (will_emit_leading_sep){
        out[pos++] = SEP;
    }
#else
    if (will_emit_leading_sep) out[pos++] = SEP;
#endif

    for (size_t k=0;k<vec.len;k++){
        int need_sep =
            (prefix_len > 0) ||
            (!will_emit_leading_sep ? (k > 0) : (k > 0));
        if (need_sep) out[pos++] = SEP;

        size_t L = strlen(vec.items[k]);
        memcpy(out + pos, vec.items[k], L);
        pos += L;
    }

    if (!rooted && prefix_len==0 && vec.len==0){
        out[pos++] = '.';
    }

    out[pos]=0;
    seg_free(&vec);
    return out;
}

/* ===== Exposed functions to language ===== */

/* путь_соединить(a, b, ...) -> нормализованный путь */
static Value path_join(int argc, const Value* argv){
    if (argc<1) return v_str_copy("",0);

    char* acc = NULL; size_t accn = 0;

    for (int i=0;i<argc;i++){
        if (argv[i].kind != VAL_STR) continue;
        const char* s = argv[i].as.str.s;
        size_t n = argv[i].as.str.n;
        if (!s || n==0) continue;

#ifdef _WIN32
        if (n>=1 && is_sep(s[0])){ free(acc); acc=NULL; accn=0; }
        else if (win_drive_len(s,n)){ free(acc); acc=NULL; accn=0; }
        else if (unc_prefix_len(s,n)){ free(acc); acc=NULL; accn=0; }
#else
        if (s[0]=='/'){ free(acc); acc=NULL; accn=0; }
#endif
        size_t need = acc?1:0;
        if (n>SIZE_MAX-need){ free(acc); return v_str_copy("",0); }
        need+=n;
        if (accn>SIZE_MAX-need || accn+need==SIZE_MAX){
            free(acc);
            return v_str_copy("",0);
        }
        char* out = (char*)kir_oom_malloc(accn + need + 1);
        if (!out){ free(acc); return v_str_copy("",0); }
        size_t pos = 0;
        if (acc){
            memcpy(out, acc, accn); pos = accn;
            if (pos>0 && !is_sep(out[pos-1])) out[pos++] = SEP;
            free(acc);
        }
        memcpy(out+pos, s, n); pos += n;
        out[pos]=0;
        acc = out; accn = pos;
    }

    if (!acc) return v_str_copy("",0);

    char* norm = normalize_path(acc, accn);
    free(acc);
    if (!norm) return v_str_copy("",0);
    Value v = v_str_copy(norm, -1);
    free(norm);
    return v;
}

/* расширение(путь) -> ".ext" или "" */
static const char* last_sep_ptr(const char* s, size_t n){
    const char* last = NULL;
    for (size_t i=0;i<n;i++){
        if (is_sep(s[i])) last = s+i;
    }
    return last;
}

static Value path_ext(int argc, const Value* argv){
    if (argc<1 || argv[0].kind!=VAL_STR) return v_str_copy("",0);
    const char* p = argv[0].as.str.s; size_t n = argv[0].as.str.n;
    const char* last_sep = last_sep_ptr(p,n);
    const char* last_dot = NULL;
    for (size_t i=0;i<n;i++) if (p[i]=='.') last_dot = p+i;
    if (!last_dot) return v_str_copy("",0);
    if (last_sep && last_dot < last_sep) return v_str_copy("",0);
    if ((!last_sep && last_dot==p) || (last_sep && last_dot==last_sep+1)) return v_str_copy("",0);
    return v_str_copy(last_dot, (int)(p+n - last_dot));
}

/* имя(путь) -> последний компонент */
static Value path_basename(int argc, const Value* argv){
    if (argc<1 || argv[0].kind!=VAL_STR) return v_str_copy("",0);
    const char* p = argv[0].as.str.s; size_t n = argv[0].as.str.n;

#ifdef _WIN32
    int dl = win_drive_len(p, n);
    size_t i = (size_t)dl;
#else
    size_t i = 0;
#endif
    while (n>0 && is_sep(p[n-1])) n--;

    size_t last = i;
    for (size_t j=i;j<n;j++) if (is_sep(p[j])) last = j+1;
    if (last>=n) return v_str_copy("",0);
    return v_str_copy(p+last, (int)(n - last));
}

/* папка(путь) -> путь без последнего компонента */
static Value path_dirname(int argc, const Value* argv){
    if (argc<1 || argv[0].kind!=VAL_STR) return v_str_copy(".",1);
    const char* p = argv[0].as.str.s; size_t n = argv[0].as.str.n;

#ifdef _WIN32
    int dl = win_drive_len(p, n);
    size_t base = (size_t)dl;
#else
    size_t base = 0;
#endif
    while (n>base && is_sep(p[n-1])) n--;

    size_t cut = base;
    for (size_t j=base;j<n;j++) if (is_sep(p[j])) cut = j;

    if (cut<=base){
#ifdef _WIN32
        if (base){
            char tmp[4] = {p[0], p[1], SEP, 0};
            return v_str_copy(tmp, 3);
        }
#endif
        if (n>0 && p[0]==SEP) return v_str_copy("/",1);
        return v_str_copy(".",1);
    }
    return v_str_copy(p, (int)cut);
}

/* нормализовать(путь) -> platform style canonical-ish path */
static Value path_normalize(int argc, const Value* argv){
    if (argc<1 || argv[0].kind!=VAL_STR) return v_str_copy("",0);
    char* out = normalize_path(argv[0].as.str.s, argv[0].as.str.n);
    if (!out) return v_str_copy("",0);
    Value v = v_str_copy(out, -1);
    free(out);
    return v;
}

static const KirNativeDef PATH_FUNCS[] = {
    {"путь_соединить", path_join},
    {"расширение",     path_ext},
    {"имя",            path_basename},
    {"папка",          path_dirname},
    {"нормализовать",  path_normalize},
};

void path_init(void){
    kir_native_register(PATH_FUNCS, (int)(sizeof(PATH_FUNCS)/sizeof(PATH_FUNCS[0])));
    kir_native_mark_bytecode_safe(PATH_FUNCS, sizeof(PATH_FUNCS)/sizeof(PATH_FUNCS[0]));
}
