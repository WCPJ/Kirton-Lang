/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
// src/mod_clipboard.c — Буфер обмена для Киртона (UTF-8 строки).
// Функции языка:
//   буфер_прочитать()              -> STRING          // читает текст из буфера обмена; CRLF нормализуется в '\n'
//   буфер_записать(строка)         -> BOOL            // пишет текст; '\n' преобразуется в CRLF
//   клипборд_прочитать()           -> STRING          // алиас
//   клипборд_записать(строка)      -> BOOL            // алиас
//
// Требует: kirton/native.h, kirton/value.h
//
// Примечания:
//  - На Windows используется CF_UNICODETEXT, полная Unicode-цепочка.
//  - На не-Windows платформах пока возвращаются заглушки (пустая строка/false).

#include "kirton/native.h"
#include "native_internal.h"
#include "kirton/value.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <wchar.h>

/* ===================== UTF-8 <-> UTF-16 helpers ===================== */

static wchar_t* u8_to_w(const char* u8, int n){
    if (!u8) return NULL;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, u8, (n>=0)?n:-1, NULL, 0);
    if (wlen <= 0) return NULL;
    wchar_t* w = (wchar_t*)malloc((size_t)(wlen+1)*sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, u8, (n>=0)?n:-1, w, wlen);
    w[wlen] = 0;
    return w;
}

static char* w_to_u8(const wchar_t* w, int wn){
    if (!w) return NULL;
    int u8len = WideCharToMultiByte(CP_UTF8, 0, w, (wn>=0)?wn:-1, NULL, 0, NULL, NULL);
    if (u8len <= 0) return NULL;
    char* out = (char*)malloc((size_t)u8len + 1);
    if (!out) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, (wn>=0)?wn:-1, out, u8len, NULL, NULL);
    out[u8len] = 0;
    return out;
}

/* ===================== CRLF <-> LF нормализация ===================== */

// "\r\n" -> "\n" (оставляем одиночные '\r' как есть)
static char* normalize_crlf_to_lf(const char* s){
    if (!s) return NULL;
    size_t n = strlen(s);
    char* out = (char*)malloc(n + 1);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i=0; i<n; i++){
        if (s[i] == '\r' && (i+1<n) && s[i+1] == '\n'){
            out[j++] = '\n';
            i++; // пропускаем '\n'
        } else {
            out[j++] = s[i];
        }
    }
    out[j] = 0;
    return out;
}

// "\n" -> "\r\n" (если уже есть "\r\n", оставляем как есть)
static char* expand_lf_to_crlf(const char* s, int n){
    if (!s) return NULL;
    if (n < 0) n = (int)strlen(s);
    // В худшем случае каждый '\n' превратится в "\r\n" => нужен буфер ~2x.
    char* out = (char*)malloc((size_t)n * 2 + 1);
    if (!out) return NULL;
    size_t j = 0;
    for (int i=0; i<n; i++){
        char c = s[i];
        if (c == '\n'){
            // если предыдущий символ не '\r', вставим '\r'
            if (i==0 || s[i-1] != '\r') out[j++] = '\r';
            out[j++] = '\n';
        } else {
            out[j++] = c;
        }
    }
    out[j] = 0;
    return out;
}

/* ===================== Реализация буфера обмена (Windows) ===================== */

static Value cb_read(int argc, const Value* argv){
    (void)argc; (void)argv;
    Value empty = v_str_copy("", 0);

    if (!OpenClipboard(NULL)) return empty;

    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (!h){
        CloseClipboard();
        return empty;
    }
    const wchar_t* w = (const wchar_t*)GlobalLock(h);
    if (!w){
        CloseClipboard();
        return empty;
    }

    // UTF-16 -> UTF-8
    char* u8_raw = w_to_u8(w, -1);
    GlobalUnlock(h);
    CloseClipboard();

    if (!u8_raw) return empty;

    // CRLF -> LF
    char* u8_norm = normalize_crlf_to_lf(u8_raw);
    if (!u8_norm){
        Value v = v_str_copy(u8_raw, -1);
        free(u8_raw);
        return v;
    }

    Value v = v_str_copy(u8_norm, -1);
    free(u8_raw);
    free(u8_norm);
    return v;
}

static Value cb_write(int argc, const Value* argv){
    if (argc < 1 || argv[0].kind != VAL_STR) return v_bool(false);
    const char* s = argv[0].as.str.s;
    int n = argv[0].as.str.n;

    // LF -> CRLF
    char* crlf = expand_lf_to_crlf(s, n);
    if (!crlf) return v_bool(false);

    // UTF-8 -> UTF-16
    wchar_t* w = u8_to_w(crlf, -1);
    free(crlf);
    if (!w) return v_bool(false);

    size_t bytes = (wcslen(w) + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!mem){ free(w); return v_bool(false); }

    void* dst = GlobalLock(mem);
    if (!dst){ GlobalFree(mem); free(w); return v_bool(false); }
    memcpy(dst, w, bytes);
    GlobalUnlock(mem);
    free(w);

    if (!OpenClipboard(NULL)){ GlobalFree(mem); return v_bool(false); }
    if (!EmptyClipboard()){ CloseClipboard(); GlobalFree(mem); return v_bool(false); }

    // Передаём владение ОС — после SetClipboardData память освобождать нельзя
    if (!SetClipboardData(CF_UNICODETEXT, mem)){
        CloseClipboard();
        GlobalFree(mem);
        return v_bool(false);
    }

    CloseClipboard();
    return v_bool(true);
}

#else /* ===================== Заглушки для не-Windows ===================== */

static Value cb_read(int argc, const Value* argv){
    (void)argc; (void)argv;
    // Можно позже реализовать через xclip/wl-clipboard; сейчас заглушка.
    return v_str_copy("", 0);
}

static Value cb_write(int argc, const Value* argv){
    (void)argc; (void)argv;
    return v_bool(false);
}

#endif /* _WIN32 */

/* ===================== Регистрация ===================== */

static const KirNativeDef CLIP_FUNCS[] = {
    {"буфер_прочитать",     cb_read},
    {"буфер_записать",      cb_write},
    {"клипборд_прочитать",  cb_read},   // алиасы
    {"клипборд_записать",   cb_write},
};

void clipboard_init(void){
    kir_native_register(CLIP_FUNCS, (int)(sizeof(CLIP_FUNCS)/sizeof(CLIP_FUNCS[0])));
    kir_native_mark_bytecode_safe(CLIP_FUNCS, sizeof(CLIP_FUNCS)/sizeof(CLIP_FUNCS[0]));
}
