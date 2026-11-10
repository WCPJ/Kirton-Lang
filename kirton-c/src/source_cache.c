/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
// src/source_cache.c
#include "kirton/source_cache.h"
#include "kirton/common.h"
#include <string.h>

/* Кэш исходников для диагностики: быстрый доступ к строкам по номеру. */

typedef struct {
    char*  file;        /* UTF-8, ключ записи */
    char*  text;        /* полный текст (UTF-8) */
    int    len;         /* длина текста в байтах */
    int*   line_starts; /* offsets начала каждой строки (0-based) */
    int    lines;       /* число строк (>=1, даже если файл пустой) */
} SrcEntry;

static SrcEntry* G  = NULL;
static size_t    GN = 0;
static size_t    GC = 0;

/* Поиск записи по имени файла (точное совпадение UTF-8 строки). */
static int find_entry(const char* file){
    if (!file) return -1;
    for (size_t i = 0; i < GN; i++){
        /* гарантия: G[i].file всегда непустой валидный указатель */
        if (strcmp(G[i].file, file) == 0) return (int)i;
    }
    return -1;
}

/* Индексация позиций начала строк. */
static void index_lines(SrcEntry* e){
    /* посчитаем количество строк: считаем '\n'; минимум 1 строка */
    int lines = 1;
    for (int i = 0; i < e->len; i++){
        if (e->text[i] == '\n') lines++;
    }

    e->line_starts = (int*)kir_xmallocn((size_t)lines, sizeof(int));
    e->lines       = lines;

    int idx = 0;
    e->line_starts[idx++] = 0;
    for (int i = 0; i < e->len; i++){
        if (e->text[i] == '\n'){
            e->line_starts[idx++] = i + 1;
        }
    }
    /* Строка после завершающего '\n' тоже получает валидное смещение. */
}

/* Расширение массива SrcEntry с проверкой переполнения. */
static void ensure_capacity(size_t need){
    if (GC >= need) return;
    size_t nc = GC ? GC * 2 : 16;
    if (nc < need) nc = need;
    G  = (SrcEntry*)kir_xreallocn(G, nc, sizeof(SrcEntry));
    GC = nc;
}

void sc_cache_file(const char* file, const char* text, int len){
    if (!file) return;
    if (!text){ text = ""; len = 0; }
    if (len < 0) len = (int)strlen(text);

    int at = find_entry(file);
    SrcEntry* e = NULL;

    if (at >= 0){
        e = &G[at];
        kir_free(e->file);        e->file = NULL;
        kir_free(e->text);        e->text = NULL;
        kir_free(e->line_starts); e->line_starts = NULL;
        e->len   = 0;
        e->lines = 0;
    } else {
        ensure_capacity(GN + 1);
        e = &G[GN++];
        memset(e, 0, sizeof(*e));
    }

    size_t flen = strlen(file);
    e->file = (char*)kir_malloc(flen + 1);
    memcpy(e->file, file, flen + 1);

    e->text = (char*)kir_malloc((size_t)len + 1);
    memcpy(e->text, text, (size_t)len);
    e->text[len] = '\0';
    e->len       = len;

    index_lines(e);
}


/* Получить строку по 1-based номеру. Возвращает стабильный буфер на поток (thread-local). */
const char* sc_get_line(const char* file, int line){
    static _Thread_local char*  LBUF = NULL;
    static _Thread_local size_t LCAP = 0;

    if (!file || line < 1) return NULL;
    int idx = find_entry(file);
    if (idx < 0) return NULL;

    SrcEntry* e = &G[idx];
    if (line > e->lines) return NULL;

    int start = e->line_starts[line - 1];
    int end   = e->len;

    /* конец строки — либо до '\n', либо до конца текста */
    for (int i = start; i < e->len; ++i){
        if (e->text[i] == '\n'){ end = i; break; }
    }
    /* убираем завершающий '\r' (CRLF) */
    if (end > start && e->text[end - 1] == '\r') end--;

    int need = (end - start) + 1; /* +NUL */
    if (need <= 0){
        /* Пустая строка */
        if (LBUF == NULL){
            LBUF = (char*)kir_malloc(1);
            LCAP = 1;
        }
        LBUF[0] = '\0';
        return LBUF;
    }

    if ((size_t)need > LCAP){
        size_t nc = LCAP ? LCAP * 2 : 128;
        if (nc < (size_t)need) nc = (size_t)need;
        LBUF = (char*)kir_realloc(LBUF, nc);
        LCAP = nc;
    }

    memcpy(LBUF, e->text + start, (size_t)(end - start));
    LBUF[end - start] = '\0';
    return LBUF;
}

/* Полная очистка кэша (если потребуется). */
void sc_clear_all(void){
    for (size_t i = 0; i < GN; i++){
        kir_free(G[i].file);
        kir_free(G[i].text);
        kir_free(G[i].line_starts);
    }
    kir_free(G);
    G  = NULL;
    GN = 0;
    GC = 0;
}
