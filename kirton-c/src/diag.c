/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
// src/diag.c
#include "kirton/diag.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#ifndef KIR_DIAG_TAB_WIDTH
#define KIR_DIAG_TAB_WIDTH 4
#endif

static unsigned int g_error_count;
static KirDiagnostic g_last_error;
static unsigned int g_silence_depth;

unsigned int kir_diag_error_count(void){ return g_error_count; }
void kir_diag_reset_errors(void){ g_error_count = 0; }
void kir_diag_last_error(KirDiagnostic* out){ if (out) *out=g_last_error; }
void kir_diag_silence_push(void){ ++g_silence_depth; }
void kir_diag_silence_pop(void){ if (g_silence_depth) --g_silence_depth; }

static void record_error(const char* file,int line,int col,
                         const char* fmt,va_list ap){
    snprintf(g_last_error.file,sizeof(g_last_error.file),"%s",file?file:"<скрипт>");
    g_last_error.line=line>0?line:1;
    g_last_error.col=col>0?col:1;
    vsnprintf(g_last_error.message,sizeof(g_last_error.message),fmt,ap);
}

static void format_vmessage(char* dst, size_t dst_size, const char* fmt, va_list ap){
    va_list copy;
    va_copy(copy, ap);
    vsnprintf(dst, dst_size, fmt, copy);
    va_end(copy);
    if (dst_size) dst[dst_size - 1] = '\0';
}

/* ---------- UTF-8 helpers (локальные, для подсчёта кареток) ---------- */
static int utf8_decode(const char* s, unsigned* out_cp){
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80){ *out_cp = c; return 1; }
    if ((c >> 5) == 0x6){
        unsigned char c1 = (unsigned char)s[1];
        if ((c1 & 0xC0) != 0x80) return 0;
        *out_cp = ((unsigned)(c & 0x1F) << 6) | (unsigned)(c1 & 0x3F);
        if (*out_cp < 0x80) return 0;
        return 2;
    }
    if ((c >> 4) == 0xE){
        unsigned char c1 = (unsigned char)s[1], c2 = (unsigned char)s[2];
        if (((c1 & 0xC0) != 0x80) || ((c2 & 0xC0) != 0x80)) return 0;
        *out_cp = ((unsigned)(c & 0x0F) << 12) | ((unsigned)(c1 & 0x3F) << 6) | (unsigned)(c2 & 0x3F);
        if (*out_cp < 0x800) return 0;
        if (*out_cp >= 0xD800 && *out_cp <= 0xDFFF) return 0;
        return 3;
    }
    if ((c >> 3) == 0x1E){
        unsigned char c1 = (unsigned char)s[1], c2 = (unsigned char)s[2], c3 = (unsigned char)s[3];
        if (((c1 & 0xC0) != 0x80) || ((c2 & 0xC0) != 0x80) || ((c3 & 0xC0) != 0x80)) return 0;
        *out_cp = ((unsigned)(c & 0x07) << 18) | ((unsigned)(c1 & 0x3F) << 12) |
                  ((unsigned)(c2 & 0x3F) << 6) | (unsigned)(c3 & 0x3F);
        if (*out_cp < 0x10000 || *out_cp > 0x10FFFF) return 0;
        return 4;
    }
    return 0;
}

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <wchar.h>

static void win_write_u8_stderr(const char* s, size_t n){
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode = 0;
    if (h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)){
        int wlen = MultiByteToWideChar(CP_UTF8, 0, s, (int)n, NULL, 0);
        if (wlen <= 0){ /* fallback */ fwrite(s,1,n,stderr); return; }
        WCHAR* wbuf = (WCHAR*)malloc((size_t)wlen * sizeof(WCHAR));
        if (!wbuf){ fwrite(s,1,n,stderr); return; }
        MultiByteToWideChar(CP_UTF8, 0, s, (int)n, wbuf, wlen);
        DWORD written = 0;
        WriteConsoleW(h, wbuf, (DWORD)wlen, &written, NULL);
        free(wbuf);
        return;
    }
    /* stderr перенаправлен — пишем байты как есть */
    fwrite(s, 1, n, stderr);
}

static void win_putn_u8(const char* s, size_t n){
    win_write_u8_stderr(s, n);
}

static void win_puts_u8(const char* s){
    win_write_u8_stderr(s, strlen(s));
}

static void win_printf_u8(const char* fmt, ...){
    char buf[2048];
    va_list ap; va_start(ap, fmt);
    int m = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (m <= 0) return;
    if (m >= (int)sizeof(buf)) m = (int)sizeof(buf) - 1; /* обрезаем до реального размера буфера */
    win_putn_u8(buf, (size_t)m);
}
#endif

/* ---------- Public API (old) ---------- */
void kir_diag_error(const char* file, int line, int col, const char* fmt, ...){
    ++g_error_count;
    va_list record_args; va_start(record_args,fmt);
    record_error(file,line,col,fmt,record_args);
    va_end(record_args);
    if (g_silence_depth) return;
#ifdef _WIN32
    char head[512];
    int m = snprintf(head, sizeof(head), "%s:%d:%d: ",
                     file ? file : "<скрипт>", line > 0 ? line : 1, col > 0 ? col : 1);
    if (m > 0){
        if (m >= (int)sizeof(head)) m = (int)sizeof(head) - 1;
        win_putn_u8(head, (size_t)m);
    }

    char msg[1024];
    va_list ap; va_start(ap, fmt);
    m = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (m > 0){
        if (m >= (int)sizeof(msg)) m = (int)sizeof(msg) - 1;
        win_putn_u8(msg, (size_t)m);
    }
    win_puts_u8("\n");
#else
    fprintf(stderr, "%s:%d:%d: ", file?file:"<скрипт>", line>0?line:1, col>0?col:1);
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
#endif
}

void kir_vdiag_error(const char* file, int line, int col, const char* fmt, va_list ap){
    char msg[1024];
    format_vmessage(msg, sizeof(msg), fmt, ap);
    kir_diag_error(file, line, col, "%s", msg);
}

void kir_diag_error_simple(const char* file, const char* fmt, ...){
    ++g_error_count;
    va_list record_args; va_start(record_args,fmt);
    record_error(file,1,1,fmt,record_args);
    va_end(record_args);
    if (g_silence_depth) return;
#ifdef _WIN32
    char head[256];
    int m = snprintf(head, sizeof(head), "%s: ", file ? file : "<скрипт>");
    if (m > 0){
        if (m >= (int)sizeof(head)) m = (int)sizeof(head) - 1;
        win_putn_u8(head, (size_t)m);
    }

    char msg[1024];
    va_list ap; va_start(ap, fmt);
    m = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (m > 0){
        if (m >= (int)sizeof(msg)) m = (int)sizeof(msg) - 1;
        win_putn_u8(msg, (size_t)m);
    }
    win_puts_u8("\n");
#else
    fprintf(stderr, "%s: ", file?file:"<скрипт>");
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
#endif
}

void kir_vdiag_error_simple(const char* file, const char* fmt, va_list ap){
    char msg[1024];
    format_vmessage(msg, sizeof(msg), fmt, ap);
    kir_diag_error_simple(file, "%s", msg);
}

/* ---------- Public API (new) ---------- */
void kir_diag_error_with_line(const char* file, int line, int col,
                              const char* source_line, int caret_len,
                              const char* fmt, ...){
    ++g_error_count;
    va_list record_args; va_start(record_args,fmt);
    record_error(file,line,col,fmt,record_args);
    va_end(record_args);
    if (g_silence_depth) return;
    if (col < 1) col = 1;
    if (caret_len < 1) caret_len = 1;

#ifdef _WIN32
    /* header + message */
    char head[512];
    int m = snprintf(head, sizeof(head), "%s:%d:%d: ",
                     file ? file : "<скрипт>", line > 0 ? line : 1, col);
    if (m > 0){
        if (m >= (int)sizeof(head)) m = (int)sizeof(head) - 1;
        win_putn_u8(head, (size_t)m);
    }

    char msg[1024];
    va_list ap; va_start(ap, fmt);
    m = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (m > 0){
        if (m >= (int)sizeof(msg)) m = (int)sizeof(msg) - 1;
        win_putn_u8(msg, (size_t)m);
    }
    win_puts_u8("\n");
#else
    fprintf(stderr, "%s:%d:%d: ", file?file:"<скрипт>", line>0?line:1, col);
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
#endif

    if (!source_line || *source_line == '\0') return;

    /* trim CR/LF */
    size_t len = strlen(source_line);
    while (len && (source_line[len-1] == '\n' || source_line[len-1] == '\r')) len--;

#ifdef _WIN32
    win_puts_u8("    ");
    win_putn_u8(source_line, len);
    win_puts_u8("\n");
#else
    fwrite("    ", 1, 4, stderr);
    fwrite(source_line, 1, len, stderr);
    fputc('\n', stderr);
#endif

    /* compute caret position in visual columns */
    int target_cp_index = col - 1;
    int cp_index = 0;
    int visual_cols = 0;
    const char* p = source_line;

    while (*p && cp_index < target_cp_index){
        if (*p == '\n' || *p == '\r') break;
        if (*p == '\t'){
            int to_tab = KIR_DIAG_TAB_WIDTH - (visual_cols % KIR_DIAG_TAB_WIDTH);
            visual_cols += to_tab;
            p++; cp_index++;
            continue;
        }
        unsigned cp = 0;
        int adv = utf8_decode(p, &cp);
        if (adv == 0) adv = 1;
        p += adv;
        cp_index++;
        visual_cols += 1;
    }

#ifdef _WIN32
    /* print caret line */
    win_puts_u8("    ");
    for (int i = 0; i < visual_cols; i++) win_puts_u8(" ");
    for (int i = 0; i < caret_len; i++)   win_puts_u8("^");
    win_puts_u8("\n");
#else
    fputs("    ", stderr);
    for (int i = 0; i < visual_cols; i++) fputc(' ', stderr);
    for (int i = 0; i < caret_len; i++)   fputc('^', stderr);
    fputc('\n', stderr);
#endif
}

void kir_vdiag_error_with_line(const char* file, int line, int col,
                               const char* source_line, int caret_len,
                               const char* fmt, va_list ap){
    char msg[1024];
    format_vmessage(msg, sizeof(msg), fmt, ap);
    kir_diag_error_with_line(file, line, col, source_line, caret_len, "%s", msg);
}

void kir_diag_replay_last_error(void){
    if (g_silence_depth || !g_last_error.message[0]) return;
#ifdef _WIN32
    win_printf_u8("%s:%d:%d: %s\n",g_last_error.file,g_last_error.line,
                  g_last_error.col,g_last_error.message);
#else
    fprintf(stderr,"%s:%d:%d: %s\n",g_last_error.file,g_last_error.line,
            g_last_error.col,g_last_error.message);
#endif
}
