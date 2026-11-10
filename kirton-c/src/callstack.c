/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
// src/callstack.c
#include "kirton/callstack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Небольшой thread-local стек кадров.
   Хранит УКАЗАТЕЛИ на строки (имена функции/файла) — владения не берём.
   Каждому потоку соответствует свой стек (через _Thread_local). */

typedef struct {
    KirFrame* data;
    size_t    len;
    size_t    cap;
} KirStack;

static _Thread_local KirStack G = {0};

static void ensure_cap(size_t need){
    if (G.len + need <= G.cap) return;

    size_t nc = G.cap ? G.cap * 2 : 32;
    while (nc < G.len + need) {
        if (nc > (size_t)1e6) {
            /* Небольшая защита от безумного роста, в debug можно логнуть. */
            return;
        }
        nc *= 2;
    }

    KirFrame* nd = (KirFrame*)realloc(G.data, nc * sizeof(KirFrame));
    if (!nd) {
        /* OOM — оставляем старый стек как есть, просто не растём. */
        return;
    }
    G.data = nd;
    G.cap  = nc;
}

void cs_push(const char* func, const char* file, int line){
    ensure_cap(1);
    if (G.len < G.cap){
        KirFrame* fr = &G.data[G.len++];
        fr->func = func ? func : "<func>";
        fr->file = file ? file : "<скрипт>";
        fr->line = (line > 0) ? line : 1;
    }
}

void cs_pop(void){
    if (G.len > 0) {
        G.len--;
    }
}

void cs_dump(void){
    if (G.len == 0){
        fprintf(stderr, "Трассировка вызовов: <пусто>\n");
        return;
    }
    fprintf(stderr, "Трассировка вызовов (последние сверху):\n");
    for (size_t i = G.len; i-- > 0; ){
        const KirFrame* fr = &G.data[i];
        fprintf(stderr, "  at %s (%s:%d)\n",
                fr->func ? fr->func : "<func>",
                fr->file ? fr->file : "<скрипт>",
                fr->line > 0 ? fr->line : 1);
    }
}

size_t cs_snapshot(KirFrame* frames,size_t capacity){
    size_t count=G.len<capacity?G.len:capacity;
    if (frames && count) memcpy(frames,G.data,count*sizeof(*frames));
    return G.len;
}

void cs_clear(void){
    if (G.data) {
        free(G.data);
        G.data = NULL;
    }
    G.len = 0;
    G.cap = 0;
}
