/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Кадр стека вызовов Kirton.
   ВАЖНО: func/file — только указатели на строки, владения не берём. */
typedef struct {
    const char* func;
    const char* file;
    int         line;
} KirFrame;

/* Добавить кадр в thread-local стек.
   func/file могут быть NULL — будут заменены на "<func>"/"<скрипт>". */
void cs_push(const char* func, const char* file, int line);

/* Снять последний кадр (если стек не пустой). */
void cs_pop(void);

/* Напечатать стек в stderr в человекочитаемом виде. */
void cs_dump(void);
size_t cs_snapshot(KirFrame* frames,size_t capacity);

/* Полностью очистить стек и освободить память.
   Не обязательно вызывать, но полезно в тестах/при контролируемом shutdown. */
void cs_clear(void);

#ifdef __cplusplus
}
#endif
