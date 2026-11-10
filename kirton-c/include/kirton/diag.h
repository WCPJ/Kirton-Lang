/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#pragma once
#include <stdarg.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------
   Диагностика компилятора/интерпретатора Kirton.

   Координаты:
     - line, col — 1-based.
     - col трактуется как «колонка в символах» (code points), а не в байтах.
       Подсветка в kir_diag_error_with_line должна принимать длину в символах.

   Поток:
     - Реализация печатает в stderr (или в пользовательский callback, если заведёте).

   Безопасность форматов:
     - Добавлены GCC/Clang-совместимые атрибуты форматирования для
       статической проверки printf-подобных строк.
   -------------------------------------------------------------------- */

/* --- Атрибуты форматирования (компиляторные подсказки) --- */
#if defined(__clang__) || defined(__GNUC__)
  #define KIR_FMT_ATTR(archetype, fmt_index, first_arg) __attribute__((format(archetype, fmt_index, first_arg)))
#else
  #define KIR_FMT_ATTR(archetype, fmt_index, first_arg)
#endif

/* ============================================================
   Совместимостьные функции (вариадики) + v-аналоги
   ============================================================ */

/* Печать ошибки с координатами (без строки исходника). */
void kir_diag_error(const char* file, int line, int col,
                    const char* fmt, ...) KIR_FMT_ATTR(printf, 4, 5);

/* v-версия kir_diag_error (используйте, если уже есть va_list). */
void kir_vdiag_error(const char* file, int line, int col,
                     const char* fmt, va_list ap) KIR_FMT_ATTR(printf, 4, 0);

/* Печать простой ошибки без координат. */
void kir_diag_error_simple(const char* file,
                           const char* fmt, ...) KIR_FMT_ATTR(printf, 2, 3);

/* v-версия kir_diag_error_simple. */
void kir_vdiag_error_simple(const char* file,
                            const char* fmt, va_list ap) KIR_FMT_ATTR(printf, 2, 0);

/* Новая точка: сообщение + (опционально) строка исходника и «каретка».
   - file, line, col: координаты (col — 1-based, в символах).
   - source_line: строка исходного текста (UTF-8) или NULL (тогда без подсветки).
   - caret_len: длина подсветки в символах; если <1, реализация должна считать 1.
   - fmt: printf-подобное сообщение. */
void kir_diag_error_with_line(const char* file, int line, int col,
                              const char* source_line, int caret_len,
                              const char* fmt, ...) KIR_FMT_ATTR(printf, 6, 7);

/* Количество диагностированных ошибок для CLI и хостов интерпретатора. */
unsigned int kir_diag_error_count(void);
void kir_diag_reset_errors(void);

typedef struct {
    char file[512];
    char message[1024];
    int line;
    int col;
} KirDiagnostic;

/* Copy the most recently emitted diagnostic. */
void kir_diag_last_error(KirDiagnostic* out);
void kir_diag_silence_push(void);
void kir_diag_silence_pop(void);
void kir_diag_replay_last_error(void);

/* v-версия kir_diag_error_with_line. */
void kir_vdiag_error_with_line(const char* file, int line, int col,
                               const char* source_line, int caret_len,
                               const char* fmt, va_list ap) KIR_FMT_ATTR(printf, 6, 0);

#ifdef __cplusplus
}
#endif
