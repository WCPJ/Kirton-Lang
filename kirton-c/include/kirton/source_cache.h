/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
   Кэш исходников для диагностики:
   - Позволяет быстро получить строку по номеру (1-based),
     чтобы печатать сообщение об ошибке + «каретку».
   ------------------------------------------------------------------ */

/* Кладёт (или перезаписывает) файл в кэш.
   file           : имя файла (UTF-8, ключ записи)
   contents_utf8  : полный текст файла в UTF-8 (может быть NULL → трактуется как "")
   len_or_minus1  : длина contents_utf8 в байтах; если < 0 — длина вычисляется через strlen */
void        sc_cache_file(const char* file, const char* contents_utf8, int len_or_minus1);

/* Возвращает указатель на строку № line (1-based) из кэша.
   ВАЖНО: возвращаемый буфер — thread-local (static _Thread_local);
          он валиден до следующего вызова sc_get_line() в ЭТОМ ЖЕ потоке.
   Возвращает NULL, если файл/строка не найдены. */
const char* sc_get_line(const char* file, int line);

/* Полностью очищает кэш (освобождает всю память). */
void        sc_clear_all(void);

#ifdef __cplusplus
}
#endif
