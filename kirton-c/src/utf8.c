/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
// src/utf8.c
#include "kirton/utf8.h"
#include <stdint.h>
#include <stdbool.h>

/* ---------------- UTF-8 strict decoder ----------------
   Возвращает число байт (1..4) либо 0 при ошибке.
   Строго запрещаем:
     - оверлонги
     - суррогаты U+D800..U+DFFF
     - кодпоинты > U+10FFFF
*/
int kir_utf8_decode(const char* s, const char* end, uint32_t* out_cp){
    if (!s || s>=end) return 0;
    const unsigned char b0 = (unsigned char)s[0];

    if (b0 < 0x80){ /* ASCII */
        if (out_cp) *out_cp = b0;
        return 1;
    }

    if (b0 >= 0xC2 && b0 <= 0xDF){ /* 2-byte: 110xxxxx 10xxxxxx */
        if (s+1 >= end) return 0;
        const unsigned char b1 = (unsigned char)s[1];
        if ((b1 & 0xC0) != 0x80) return 0;
        if (out_cp) *out_cp = ((uint32_t)(b0 & 0x1F) << 6) | (uint32_t)(b1 & 0x3F);
        return 2;
    }

    if (b0 == 0xE0){ /* 3-byte (special min bound for overlong) */
        if (s+2 >= end) return 0;
        const unsigned char b1 = (unsigned char)s[1];
        const unsigned char b2 = (unsigned char)s[2];
        if (b1 < 0xA0 || b1 > 0xBF) return 0; /* prevent overlong */
        if ((b2 & 0xC0) != 0x80) return 0;
        uint32_t cp = ((uint32_t)(b0 & 0x0F) << 12) | ((uint32_t)(b1 & 0x3F) << 6) | (uint32_t)(b2 & 0x3F);
        if (out_cp) *out_cp = cp;
        return 3;
    }
    if (b0 >= 0xE1 && b0 <= 0xEC){ /* 3-byte generic */
        if (s+2 >= end) return 0;
        const unsigned char b1 = (unsigned char)s[1];
        const unsigned char b2 = (unsigned char)s[2];
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) return 0;
        uint32_t cp = ((uint32_t)(b0 & 0x0F) << 12) | ((uint32_t)(b1 & 0x3F) << 6) | (uint32_t)(b2 & 0x3F);
        if (out_cp) *out_cp = cp;
        return 3;
    }
    if (b0 == 0xED){ /* 3-byte but not surrogates */
        if (s+2 >= end) return 0;
        const unsigned char b1 = (unsigned char)s[1];
        const unsigned char b2 = (unsigned char)s[2];
        if (b1 < 0x80 || b1 > 0x9F) return 0; /* disallow D800..DFFF */
        if ((b2 & 0xC0) != 0x80) return 0;
        uint32_t cp = ((uint32_t)(b0 & 0x0F) << 12) | ((uint32_t)(b1 & 0x3F) << 6) | (uint32_t)(b2 & 0x3F);
        if (out_cp) *out_cp = cp;
        return 3;
    }
    if (b0 >= 0xEE && b0 <= 0xEF){ /* 3-byte generic */
        if (s+2 >= end) return 0;
        const unsigned char b1 = (unsigned char)s[1];
        const unsigned char b2 = (unsigned char)s[2];
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) return 0;
        uint32_t cp = ((uint32_t)(b0 & 0x0F) << 12) | ((uint32_t)(b1 & 0x3F) << 6) | (uint32_t)(b2 & 0x3F);
        if (out_cp) *out_cp = cp;
        return 3;
    }

    if (b0 == 0xF0){ /* 4-byte, min bound */
        if (s+3 >= end) return 0;
        const unsigned char b1 = (unsigned char)s[1];
        const unsigned char b2 = (unsigned char)s[2];
        const unsigned char b3 = (unsigned char)s[3];
        if (b1 < 0x90 || b1 > 0xBF) return 0; /* prevent overlong */
        if (((b2 & 0xC0)!=0x80) || ((b3 & 0xC0)!=0x80)) return 0;
        uint32_t cp = ((uint32_t)(b0 & 0x07) << 18) | ((uint32_t)(b1 & 0x3F) << 12)
                    | ((uint32_t)(b2 & 0x3F) << 6) | (uint32_t)(b3 & 0x3F);
        if (out_cp) *out_cp = cp;
        return 4;
    }
    if (b0 >= 0xF1 && b0 <= 0xF3){ /* 4-byte generic */
        if (s+3 >= end) return 0;
        const unsigned char b1 = (unsigned char)s[1];
        const unsigned char b2 = (unsigned char)s[2];
        const unsigned char b3 = (unsigned char)s[3];
        if (((b1 & 0xC0)!=0x80) || ((b2 & 0xC0)!=0x80) || ((b3 & 0xC0)!=0x80)) return 0;
        uint32_t cp = ((uint32_t)(b0 & 0x07) << 18) | ((uint32_t)(b1 & 0x3F) << 12)
                    | ((uint32_t)(b2 & 0x3F) << 6) | (uint32_t)(b3 & 0x3F);
        if (out_cp) *out_cp = cp;
        return 4;
    }
    if (b0 == 0xF4){ /* 4-byte, max bound U+10FFFF */
        if (s+3 >= end) return 0;
        const unsigned char b1 = (unsigned char)s[1];
        const unsigned char b2 = (unsigned char)s[2];
        const unsigned char b3 = (unsigned char)s[3];
        if (b1 < 0x80 || b1 > 0x8F) return 0;
        if (((b2 & 0xC0)!=0x80) || ((b3 & 0xC0)!=0x80)) return 0;
        uint32_t cp = ((uint32_t)(b0 & 0x07) << 18) | ((uint32_t)(b1 & 0x3F) << 12)
                    | ((uint32_t)(b2 & 0x3F) << 6) | (uint32_t)(b3 & 0x3F);
        if (out_cp) *out_cp = cp;
        return 4;
    }
    return 0;
}

/* ---------------- Unicode classes for identifiers ---------------- */

static inline bool is_ascii_letter(uint32_t cp){
    return (cp>='A' && cp<='Z') || (cp>='a' && cp<='z');
}
static inline bool is_ascii_digit(uint32_t cp){
    return (cp>='0' && cp<='9');
}

/* Кириллица: базовый блок и расширения */
static inline bool is_cyrillic(uint32_t cp){
    /* Cyrillic, Cyrillic Supplement, Cyrillic Extended-A/B/C */
    if (cp >= 0x0400 && cp <= 0x04FF) return true;
    if (cp >= 0x0500 && cp <= 0x052F) return true;
    if (cp >= 0x2DE0 && cp <= 0x2DFF) return true;
    if (cp >= 0xA640 && cp <= 0xA69F) return true;
    if (cp >= 0x1C80 && cp <= 0x1C8F) return true;
    return false;
}

/* Комбинируемые знаки (ударения и пр.) — допускаем как продолжение идентификатора */
static inline bool is_combining_mark(uint32_t cp){
    if (cp >= 0x0300 && cp <= 0x036F) return true;  /* Combining Diacritical Marks */
    if (cp >= 0x1AB0 && cp <= 0x1AFF) return true;  /* Combining Diacritical Marks Extended */
    if (cp >= 0x1DC0 && cp <= 0x1DFF) return true;  /* Combining Diacritical Marks Supplement */
    if (cp >= 0x20D0 && cp <= 0x20FF) return true;  /* Combining Marks for Symbols */
    if (cp >= 0xFE20 && cp <= 0xFE2F) return true;  /* Combining Half Marks */
    return false;
}

/* Публичные функции, которые использует лексер */
bool kir_is_ident_start(uint32_t cp){
    if (cp == (uint32_t)'_') return true;
    if (is_ascii_letter(cp)) return true;
    if (is_cyrillic(cp))     return true;
    /* при желании сюда можно добавить другие алфавиты */
    return false;
}
bool kir_is_ident_cont(uint32_t cp){
    if (cp == (uint32_t)'_') return true;
    if (is_ascii_letter(cp) || is_ascii_digit(cp)) return true;
    if (is_cyrillic(cp)) return true;
    if (is_combining_mark(cp)) return true;
    return false;
}
