/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
// src/mod_dict.c — Киртон: операции со словарями (все экспортные имена на кириллице)

#include "kirton/native.h"
#include "native_internal.h"
#include "kirton/value.h"
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

/* ============ вспомогалки ============ */

static inline bool is_dict(const Value* v){
    return v && v->kind == VAL_DICT;
}

static inline bool is_str(const Value* v){
    return v && v->kind == VAL_STR && v->as.str.s != NULL;
}

/* логическое «удаление»: считаем элементом только те пары,
   где значение НЕ nil. Это даёт нам безопасный remove без лезтья в аллокатор. */
static inline bool pair_alive(const Value* v){
    return v && v->kind != VAL_NIL;
}

/* ============ функции ============ */

/* словарь_пустой() -> {} */
static Value словарь_пустой_fn(int argc, const Value* a){
    (void)argc; (void)a;
    return v_dict_new();
}

/* словарь_длина(D) -> число (считает только пары со значением != nil) */
static Value словарь_длина_fn(int argc, const Value* a){
    if (argc < 1 || !is_dict(&a[0])) return v_int(0);
    const VDict* d = a[0].as.dict;
    size_t alive = 0;
    for (size_t i=0;i<d->len;i++){
        if (pair_alive(&d->vals[i])) alive++;
    }
    return v_int_from_size(alive);
}

/* словарь_копия(D) -> новый словарь (поверхностная копия живых пар) */
static Value словарь_копия_fn(int argc, const Value* a){
    if (argc < 1 || !is_dict(&a[0])) return v_dict_new();
    const VDict* d = a[0].as.dict;
    Value out = v_dict_new();
    for (size_t i=0;i<d->len;i++){
        if (!pair_alive(&d->vals[i])) continue;
        v_dict_set(&out, d->keys[i], d->kn[i], d->vals[i]);
    }
    return out;
}

/* словарь_ключи(D) -> список строк (только живые пары) */
static Value словарь_ключи_fn(int argc, const Value* a){
    if (argc < 1 || !is_dict(&a[0])) return v_list_new();
    const VDict* d = a[0].as.dict;
    Value L = v_list_new();
    for (size_t i=0;i<d->len;i++){
        if (!pair_alive(&d->vals[i])) continue;
        v_list_push(&L, v_str_copy(d->keys[i], (int)d->kn[i]));
    }
    return L;
}

/* словарь_значения(D) -> список значений (только живые пары) */
static Value словарь_значения_fn(int argc, const Value* a){
    if (argc < 1 || !is_dict(&a[0])) return v_list_new();
    const VDict* d = a[0].as.dict;
    Value L = v_list_new();
    for (size_t i=0;i<d->len;i++){
        if (!pair_alive(&d->vals[i])) continue;
        v_list_push(&L, d->vals[i]);
    }
    return L;
}

/* словарь_пары(D) -> список пар [ [ключ, значение], ... ] (только живые) */
static Value словарь_пары_fn(int argc, const Value* a){
    if (argc < 1 || !is_dict(&a[0])) return v_list_new();
    const VDict* d = a[0].as.dict;
    Value L = v_list_new();
    for (size_t i=0;i<d->len;i++){
        if (!pair_alive(&d->vals[i])) continue;
        Value pair = v_list_new();
        v_list_push(&pair, v_str_copy(d->keys[i], (int)d->kn[i]));
        v_list_push(&pair, d->vals[i]);
        v_list_push(&L, pair);
    }
    return L;
}

/* словарь_есть(D, ключ) -> истина/ложь (живой ключ) */
static Value словарь_есть_fn(int argc, const Value* a){
    if (argc < 2 || !is_dict(&a[0]) || !is_str(&a[1])) return v_bool(false);
    Value tmp;
    int ok = v_dict_get((Value*)&a[0], a[1].as.str.s, (size_t)a[1].as.str.n, &tmp);
    return v_bool(ok && pair_alive(&tmp));
}

/* словарь_получить(D, ключ [, по_умолчанию]) -> значение/по_умолчанию/nil */
static Value словарь_получить_fn(int argc, const Value* a){
    if (argc < 2 || !is_dict(&a[0]) || !is_str(&a[1])) return v_nil();
    Value out;
    int ok = v_dict_get((Value*)&a[0], a[1].as.str.s, (size_t)a[1].as.str.n, &out);
    if (!ok || !pair_alive(&out)){
        if (argc >= 3) return a[2];
        return v_nil();
    }
    return out;
}

/* словарь_установить(D, ключ, значение) -> истина/ложь */
static Value словарь_установить_fn(int argc, const Value* a){
    if (argc < 3 || !is_dict(&a[0]) || !is_str(&a[1])) return v_bool(false);
    v_dict_set((Value*)&a[0], a[1].as.str.s, (size_t)a[1].as.str.n, a[2]);
    return v_bool(true);
}

/* словарь_удалить(D, ключ) -> удалённое значение или nil.
   Реализация: помечаем как удалённый (value=nil), ключ сохраняем.
   Все перечислители (ключи/значения/пары/длина) игнорируют такие записи. */
static Value словарь_удалить_fn(int argc, const Value* a){
    if (argc < 2 || !is_dict(&a[0]) || !is_str(&a[1])) return v_nil();
    Value prev;
    int ok = v_dict_get((Value*)&a[0], a[1].as.str.s, (size_t)a[1].as.str.n, &prev);
    if (!ok || !pair_alive(&prev)) return v_nil();
    v_dict_set((Value*)&a[0], a[1].as.str.s, (size_t)a[1].as.str.n, v_nil());
    return prev;
}

/* словарь_объединить(A, B) -> новый словарь.
   Берём живые пары из A, затем живые пары из B (B перекрывает A). */
static Value словарь_объединить_fn(int argc, const Value* a){
    if (argc < 2 || !is_dict(&a[0]) || !is_dict(&a[1])) return v_dict_new();
    const VDict* A = a[0].as.dict;
    const VDict* B = a[1].as.dict;
    Value out = v_dict_new();
    for (size_t i=0;i<A->len;i++){
        if (!pair_alive(&A->vals[i])) continue;
        v_dict_set(&out, A->keys[i], A->kn[i], A->vals[i]);
    }
    for (size_t i=0;i<B->len;i++){
        if (!pair_alive(&B->vals[i])) continue;
        v_dict_set(&out, B->keys[i], B->kn[i], B->vals[i]);
    }
    return out;
}

/* словарь_очистить(D) -> истина (делает все значения nil, чтобы быстро «обнулить») */
static Value словарь_очистить_fn(int argc, const Value* a){
    if (argc < 1 || !is_dict(&a[0])) return v_bool(false);
    VDict* d = ((Value*)&a[0])->as.dict;
    for (size_t i=0;i<d->len;i++){
        d->vals[i] = v_nil();
    }
    return v_bool(true);
}

/* ============ регистрация ============ */

static const KirNativeDef DICT_FUNCS[] = {
    {"словарь_пустой",      словарь_пустой_fn},
    {"словарь_длина",       словарь_длина_fn},
    {"словарь_копия",       словарь_копия_fn},
    {"словарь_ключи",       словарь_ключи_fn},
    {"словарь_значения",    словарь_значения_fn},
    {"словарь_пары",        словарь_пары_fn},
    {"словарь_есть",        словарь_есть_fn},
    {"словарь_получить",    словарь_получить_fn},
    {"словарь_установить",  словарь_установить_fn},
    {"словарь_удалить",     словарь_удалить_fn},
    {"словарь_объединить",  словарь_объединить_fn},
    {"словарь_очистить",    словарь_очистить_fn},
};

void dict_init(void){
    kir_native_register(DICT_FUNCS, (int)(sizeof(DICT_FUNCS)/sizeof(DICT_FUNCS[0])));
    kir_native_mark_bytecode_safe(DICT_FUNCS, sizeof(DICT_FUNCS)/sizeof(DICT_FUNCS[0]));
}
