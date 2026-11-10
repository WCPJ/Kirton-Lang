/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#pragma once
#include <stddef.h>
#include "kirton/ast.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------
   Парсер Kirton

   parse_program:
     - разбирает исходник UTF-8 (filename — только для диагностики),
     - возвращает AstProgram с:
         * prog->init  — top-level init-операторы (import/включить/пусть/expr),
         * prog->funcs — массив объявленных функций.

   ВАЖНО:
     - src_utf8 нужен только на время вызова parse_program.
     - имя файла копируется и остаётся доступным через позиции AST.
   -------------------------------------------------------------------- */

/* Парсит полный модуль: prog->init + prog->funcs. */
AstProgram* parse_program(const char* src_utf8, size_t len, const char* filename);

/* Совместимость со старым прототипом без имени файла. */
static inline AstProgram* parse_program_compat(const char* src_utf8, size_t len){
    return parse_program(src_utf8, len, "<скрипт>");
}

#ifdef __cplusplus
}
#endif
