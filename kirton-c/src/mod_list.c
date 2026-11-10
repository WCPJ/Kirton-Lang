/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
// src/mod_list.c — Киртон: операции со списками (все экспортные имена на кириллице)

#include "kirton/native.h"
#include "native_internal.h"
#include "kirton/value.h"
#include "kirton/common.h"
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

/* ============ вспомогалки ============ */

static inline bool is_list(const Value* v){
    return v && v->kind == VAL_LIST;
}

static inline bool ensure_cap(VList* L, size_t need){
    if (L->cap >= need) return true;
    size_t nc;
    if (!L->cap) nc=8;
    else if (L->cap<=SIZE_MAX/2) nc=L->cap*2;
    else nc=need;
    if (nc < need) nc = need;
    size_t bytes=0;
    if (kir_mul_overflow_size(nc,sizeof(Value),&bytes)) return false;
    Value* nd = (Value*)kir_oom_realloc(L->data,bytes);
    if (!nd) return false;
    L->data = nd;
    L->cap  = nc;
    return true;
}

/* поверхностное равенство (для список_индекс) */
static bool value_eq_shallow(Value a, Value b){
    if (a.kind != b.kind) return false;
    switch (a.kind){
        case VAL_NIL:  return true;
        case VAL_BOOL: return a.as.boolean == b.as.boolean;
        case VAL_NUM:  return v_num_equal(a, b);
        case VAL_STR:
            if (a.as.str.n != b.as.str.n) return false;
            return memcmp(a.as.str.s, b.as.str.s, (size_t)a.as.str.n) == 0;
        case VAL_LIST: /* по идентичности объекта */
            return a.as.list == b.as.list;
        case VAL_DICT:
            return a.as.dict == b.as.dict; /* идентичность */
        case VAL_ARENA: return a.as.arena == b.as.arena;
        case VAL_BUFFER: return a.as.buffer == b.as.buffer;
        case VAL_TYPED_ARRAY: return a.as.typed_array == b.as.typed_array;
        case VAL_RECORD_LAYOUT: return a.as.record_layout == b.as.record_layout;
        case VAL_RECORD: return a.as.record == b.as.record;
        case VAL_NATIVE_RESOURCE: return a.as.native_resource == b.as.native_resource &&
                                         a.integer_small == b.integer_small;
        case VAL_USER_STRUCT: return a.as.user_struct==b.as.user_struct &&
                                     a.integer_small==b.integer_small;
        case VAL_ENUM_VALUE: {
            const char* an=NULL; const char* bn=NULL;
            size_t al=0,bl=0;
            return v_enum_value_type(a)==v_enum_value_type(b) &&
                   v_enum_value_variant(a,&an,&al) &&
                   v_enum_value_variant(b,&bn,&bl) && al==bl &&
                   (al==0 || memcmp(an,bn,al)==0);
        }
        case VAL_FUNCTION: return a.as.function == b.as.function &&
                                   a.integer_small == b.integer_small;
    }
    return false;
}

/* ============ функции ============ */

/* список_пустой() -> [] */
static Value список_пустой_fn(int argc, const Value* argv){
    (void)argc; (void)argv;
    return v_list_new();
}

/* список_длина(L) -> число */
static Value список_длина_fn(int argc, const Value* argv){
    if (argc < 1 || !is_list(&argv[0])) return v_int(0);
    return v_int_from_size(argv[0].as.list->len);
}

/* список_копия(L) -> новый список (поверхностная копия) */
static Value список_копия_fn(int argc, const Value* argv){
    if (argc < 1 || !is_list(&argv[0])) return v_list_new();
    const VList* src = argv[0].as.list;
    Value out = v_list_new();
    if (src->len == 0) return out;
    if (!ensure_cap(out.as.list, src->len)) return v_list_new();
    memcpy(out.as.list->data, src->data, src->len * sizeof(Value));
    out.as.list->len = src->len;
    return out;
}

/* список_добавить(L, x) -> истина/ложь (мутирует L) */
static Value список_добавить_fn(int argc, const Value* argv){
    if (argc < 2 || !is_list(&argv[0])) return v_bool(false);
    Value* Lval = (Value*)&argv[0];
    VList* L=Lval->as.list;
    if (L->len==SIZE_MAX || !ensure_cap(L,L->len+1)) return v_bool(false);
    L->data[L->len++]=argv[1];
    return v_bool(true);
}

/* список_вставить(L, i, x) -> истина/ложь (мутирует L) */
static Value список_вставить_fn(int argc, const Value* argv){
    if (argc < 3 || !is_list(&argv[0]) || argv[1].kind != VAL_NUM) return v_bool(false);
    Value* Lval = (Value*)&argv[0];
    VList* L = Lval->as.list;
    int64_t idx = 0;
    if (!v_num_fits_i64(argv[1], &idx)) return v_bool(false);
    if (L->len==SIZE_MAX) return v_bool(false);
    if (idx < 0) idx = 0;
    if ((uint64_t)idx > (uint64_t)L->len) idx = (int64_t)L->len;
    size_t pos = (size_t)idx;
    if (!ensure_cap(L, L->len + 1)) return v_bool(false);
    /* сдвигаем хвост вправо */
    if (pos < L->len){
        memmove(L->data + pos + 1, L->data + pos, (L->len - pos) * sizeof(Value));
    }
    L->data[pos] = argv[2];
    L->len += 1;
    return v_bool(true);
}

/* список_удалить(L, i) -> удалённый элемент или nil (мутирует L) */
static Value список_удалить_fn(int argc, const Value* argv){
    if (argc < 2 || !is_list(&argv[0]) || argv[1].kind != VAL_NUM) return v_nil();
    Value* Lval = (Value*)&argv[0];
    VList* L = Lval->as.list;
    int64_t idx = 0;
    if (!v_num_fits_i64(argv[1], &idx) || idx < 0 || (uint64_t)idx >= (uint64_t)L->len) return v_nil();
    size_t pos = (size_t)idx;
    Value removed = L->data[pos];
    if (pos + 1 < L->len){
        memmove(L->data + pos, L->data + pos + 1, (L->len - pos - 1) * sizeof(Value));
    }
    L->len -= 1;
    return removed;
}

/* список_получить(L, i [, по_умолчанию]) -> элемент/по_умолчанию/nil */
static Value список_получить_fn(int argc, const Value* argv){
    if (argc < 2 || !is_list(&argv[0]) || argv[1].kind != VAL_NUM) return v_nil();
    const VList* L = argv[0].as.list;
    int64_t idx = 0;
    if (!v_num_fits_i64(argv[1], &idx) || idx < 0 || (uint64_t)idx >= (uint64_t)L->len){
        if (argc >= 3) return argv[2];
        return v_nil();
    }
    return L->data[(size_t)idx];
}

/* список_установить(L, i, x) -> истина/ложь (мутирует L) */
static Value список_установить_fn(int argc, const Value* argv){
    if (argc < 3 || !is_list(&argv[0]) || argv[1].kind != VAL_NUM) return v_bool(false);
    Value* Lval = (Value*)&argv[0];
    VList* L = Lval->as.list;
    int64_t idx = 0;
    if (!v_num_fits_i64(argv[1], &idx) || idx < 0 || (uint64_t)idx >= (uint64_t)L->len) return v_bool(false);
    L->data[(size_t)idx] = argv[2];
    return v_bool(true);
}

/* список_очистить(L) -> истина (len = 0) */
static Value список_очистить_fn(int argc, const Value* argv){
    if (argc < 1 || !is_list(&argv[0])) return v_bool(false);
    Value* Lval = (Value*)&argv[0];
    Lval->as.list->len = 0;
    return v_bool(true);
}

/* список_соединить(A, B) -> новый список */
static Value список_соединить_fn(int argc, const Value* argv){
    if (argc < 2 || !is_list(&argv[0]) || !is_list(&argv[1])) return v_list_new();
    const VList* A = argv[0].as.list;
    const VList* B = argv[1].as.list;
    if (B->len>SIZE_MAX-A->len) return v_nil();
    Value out = v_list_new();
    size_t need = A->len + B->len;
    if (!ensure_cap(out.as.list, need)) return v_list_new();
    if (A->len) memcpy(out.as.list->data + out.as.list->len, A->data, A->len * sizeof(Value)), out.as.list->len += A->len;
    if (B->len) memcpy(out.as.list->data + out.as.list->len, B->data, B->len * sizeof(Value)), out.as.list->len += B->len;
    return out;
}

/* подсписок(L, start, длина) -> новый список */
static Value подсписок_fn(int argc, const Value* argv){
    if (argc < 3 || !is_list(&argv[0]) || argv[1].kind != VAL_NUM || argv[2].kind != VAL_NUM) return v_list_new();
    const VList* S = argv[0].as.list;
    int64_t start = 0, count = 0;
    if (!v_num_fits_i64(argv[1], &start) || !v_num_fits_i64(argv[2], &count)) return v_list_new();
    if (start < 0) start = 0;
    if (count < 0) count = 0;
    size_t from = (uint64_t)start > (uint64_t)S->len ? S->len : (size_t)start;
    size_t take = (uint64_t)count > (uint64_t)(S->len - from) ? S->len - from : (size_t)count;

    Value out = v_list_new();
    if (take == 0) return out;
    if (!ensure_cap(out.as.list, take)) return v_list_new();
    memcpy(out.as.list->data, S->data + from, take * sizeof(Value));
    out.as.list->len = take;
    return out;
}

/* список_индекс(L, x) -> позиция или -1 */
static Value список_индекс_fn(int argc, const Value* argv){
    if (argc < 2 || !is_list(&argv[0])) return v_int(-1);
    const VList* L = argv[0].as.list;
    for (size_t i=0;i<L->len;i++){
        if (value_eq_shallow(L->data[i], argv[1])){
            return v_int((int64_t)i);
        }
    }
    return v_int(-1);
}

/* ============ регистрация ============ */

static const KirNativeDef LIST_FUNCS[] = {
    {"список_пустой",     список_пустой_fn},
    {"список_длина",      список_длина_fn},
    {"список_копия",      список_копия_fn},
    {"список_добавить",   список_добавить_fn},
    {"список_вставить",   список_вставить_fn},
    {"список_удалить",    список_удалить_fn},
    {"список_получить",   список_получить_fn},
    {"список_установить", список_установить_fn},
    {"список_очистить",   список_очистить_fn},
    {"список_соединить",  список_соединить_fn},
    {"подсписок",         подсписок_fn},
    {"список_индекс",     список_индекс_fn},
};

void list_init(void){
    kir_native_register(LIST_FUNCS, (int)(sizeof(LIST_FUNCS)/sizeof(LIST_FUNCS[0])));
    kir_native_mark_bytecode_safe(LIST_FUNCS, sizeof(LIST_FUNCS)/sizeof(LIST_FUNCS[0]));
}
