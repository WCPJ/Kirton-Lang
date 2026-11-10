/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#pragma once
#include "kirton/value.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef Value (*KirNativeFn)(int argc, const Value* argv);

typedef struct KirNativeDef {
    const char* name;  /* UTF-8 имя функции БЕЗ префикса модуля */
    KirNativeFn fn;
} KirNativeDef;

/* Регистрация набора глобальных нативных функций.
   Имена берутся как есть (например "фс_прочитать_текст"). */
void kir_native_register(const KirNativeDef* defs, size_t count);

/* Регистрация функций как методов модуля.
   Для module_name="fs" и функции name="фс_список" в таблицу попадает
   полное имя "fs.фс_список". */
void kir_native_register_module(const char* module_name,
                                const KirNativeDef* defs,
                                size_t count);

/* Вызов нативной функции по имени (глобальному или "модуль.функция").
   Возвращает 1, если функция найдена и вызвана, 0 — если такого имени нет. */
int kir_native_call(const char* name, size_t name_len,
                    int argc, const Value* argv, Value* out);

/* Проверка наличия нативного модуля (по префиксу до точки) и создание алиаса для всех его функций. */
int  kir_native_has_module(const char* module);
void kir_native_alias_module(const char* module, const char* alias);

#ifdef __cplusplus
}
#endif
