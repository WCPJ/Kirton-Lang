/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------------
   Kirton: токены лексера
   - Все координаты (line/col) 1-based.
   - col — колонка начала токена в символах (а не в байтах), как видит пользователь.
   - start/end указывают на байтовые позиции в исходнике (UTF-8).
   ---------------------------------------------------------------------- */

typedef enum {
    /* Лексемы-лiterals/идентификаторы */
    TK_NUMBER, TK_STRING, TK_ID,

    /* Ключевые слова (кириллица) */
    KW_MODUL,       /* модуль — необязательное объявление имени файла-модуля */
    KW_FUNKCIYA,    /* функция           */
    KW_PUST,        /* пусть             */
    KW_ESLI,        /* если              */
    KW_INACHE,      /* иначе             */
    KW_POKA,        /* пока              */
    KW_DLYA,        /* для               */
    KW_BREAK,       /* прервать          */
    KW_CONTINUE,    /* продолжить        */
    KW_KAZHDYJ,     /* каждого; каждый — совместимое старое написание */
    KW_V,           /* в                 */
    KW_VOZVRAT,     /* вернуть; возврат — совместимое старое написание */
    KW_PECHATAT,    /* печатать          */
    KW_KONEC,       /* конец             */
    KW_TO,          /* то (опционально в if/while) */
    KW_ISTINA,      /* истина            */
    KW_LOZH,        /* ложь              */
    KW_PUSTO,       /* пусто — отсутствие значения */
    KW_I,           /* и   (бинарный логический оператор) */
    KW_ILI,         /* или (бинарный логический оператор) */
    KW_NE,          /* не  (унарный op, поддержан парсером) */
    KW_NATSELO,     /* нацело — деление с округлением вниз */
    KW_IMPORT,      /* импорт            */
    KW_KAK,         /* как               */
    KW_IZ,          /* из                */
    KW_VKLYUCHIT,   /* включить          */
    KW_TRY,         /* попробовать       */
    KW_CATCH,       /* перехватить        */
    KW_FINALLY,     /* всегда             */
    KW_STRUCT,      /* структура          */
    KW_ENUM,        /* перечисление       */

    /* Операторы и пунктуация */
    TK_EQ, TK_EQEQ, TK_NEQ, TK_LT, TK_GT, TK_LE, TK_GE,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_AMP, TK_PIPE, TK_CARET, TK_TILDE, TK_SHL, TK_SHR,
    TK_LPAREN, TK_RPAREN,
    TK_LBRACK, TK_RBRACK,
    TK_LBRACE, TK_RBRACE,
    TK_DOT, TK_COLON, TK_COMMA,

    /* Служебные */
    TK_EOF,
    TK_ERROR
} KirTokenKind;

typedef struct {
    KirTokenKind kind;
    const char*  start;   /* [start, end) — байтовые границы токена в исходнике */
    const char*  end;
    int          line;    /* 1-based */
    int          col;     /* 1-based, в символах (UTF-8 колонки считает лексер) */
    const char*  file;    /* для диагностики: имя файла/модуля */
} KirToken;

/* ----------------------------------------------------------------------
   Утилиты имён токенов для диагностики:
   Реализация в token.c (или lexer.c), парсер пользуется для сообщений об ошибках.
   ---------------------------------------------------------------------- */

/* Читабельное имя токена (кириллица для ключевых слов, символы для операторов). */
const char* kir_token_kind_name(KirTokenKind k);

#ifdef __cplusplus
}
#endif
