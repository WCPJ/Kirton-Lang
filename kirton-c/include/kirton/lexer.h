/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#pragma once
#include "token.h"
#include <stddef.h>

typedef struct {
    const char* src;
    const char* cur;
    const char* end;
    int line;
    int col;
    const char* file;   /* НОВОЕ */
} KirLexer;

/* Была: kir_lexer_init(...). Сохраним её как враппер и добавим вариант с именем файла. */
void     kir_lexer_init_with_file(KirLexer* lx, const char* filename,
                                  const char* buf, size_t len);
void     kir_lexer_init(KirLexer* lx, const char* buf, size_t len); /* вызывает *_with_file(..., "<script>", ...) */

KirToken kir_lexer_next(KirLexer* lx);
const char* kir_token_kind_name(KirTokenKind k);
