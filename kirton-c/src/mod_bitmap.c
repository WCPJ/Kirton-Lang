/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
// src/mod_bitmap.c — модуль загрузки BMP-картинок для Киртона
// API в языке:
//   картинка_загрузить(путь)        -> id (>0) или 0 при ошибке
//   картинка_размер(id)             -> [w, h] или [0, 0] при ошибке
//   картинка_пиксель(id, x, y)      -> 0xRRGGBB или -1 при выходе за границы/ошибке
//   картинка_освободить(id)         -> bool
//
// Поддерживаются BMP:
//   - без сжатия (BI_RGB)
//   - 24 bpp (BGR) и 32 bpp (BGRA)
// Координаты: (0,0) — левый верхний угол.

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "kirton/native.h"
#include "native_internal.h"
#include "kirton/value.h"
#include "kirton/common.h"

#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---------------- Структуры BMP ---------------- */

#pragma pack(push,1)
typedef struct {
    uint16_t bfType;      // "BM" = 0x4D42
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;
} BmpFileHeader;

typedef struct {
    uint32_t biSize;          // 40 для BITMAPINFOHEADER
    int32_t  biWidth;
    int32_t  biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;      // 24 или 32
    uint32_t biCompression;   // 0 = BI_RGB
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
} BmpInfoHeader;
#pragma pack(pop)

/* ---------------- Хранилище картинок ---------------- */

typedef struct {
    int      w, h;
    uint32_t* px;   // массив w*h, формат 0xRRGGBB
} KBitmap;

#define KBITMAP_MAX 256
#define KBITMAP_MAX_BYTES (256u * 1024u * 1024u)

static KBitmap g_bitmaps[KBITMAP_MAX];
static int g_bitmap_atexit_registered;
static void kbitmap_free_slot(int idx);

static void kbitmaps_cleanup(void){
    for (int i=0;i<KBITMAP_MAX;++i) kbitmap_free_slot(i);
}

static void kbitmaps_init_once(void){
    static int inited = 0;
    if (inited) return;
    inited = 1;
    for (int i = 0; i < KBITMAP_MAX; ++i){
        g_bitmaps[i].w = g_bitmaps[i].h = 0;
        g_bitmaps[i].px = NULL;
    }
    if (!g_bitmap_atexit_registered){
        atexit(kbitmaps_cleanup);
        g_bitmap_atexit_registered=1;
    }
}

static int kbitmap_alloc_slot(void){
    for (int i = 0; i < KBITMAP_MAX; ++i){
        if (!g_bitmaps[i].px) return i;
    }
    return -1;
}

static void kbitmap_free_slot(int idx){
    if (idx < 0 || idx >= KBITMAP_MAX) return;
    if (g_bitmaps[idx].px){
        free(g_bitmaps[idx].px);
        g_bitmaps[idx].px = NULL;
        g_bitmaps[idx].w = g_bitmaps[idx].h = 0;
    }
}

/* ---------------- Загрузка BMP ---------------- */

static int load_bmp_rgb(const char* path, int path_len, int* out_w, int* out_h, uint32_t** out_px){
    *out_w = *out_h = 0;
    *out_px = NULL;

    if (!path) return 0;

    FILE* f = NULL;
#ifdef _WIN32
    // Пути в проекте у тебя и так ASCII/UTF-8 без экзотики, так что обычный fopen сойдёт.
    f = fopen(path, "rb");
#else
    f = fopen(path, "rb");
#endif
    if (!f) return 0;

    BmpFileHeader fh;
    if (fread(&fh, sizeof(fh), 1, f) != 1){ fclose(f); return 0; }
    if (fh.bfType != 0x4D42){ fclose(f); return 0; } // "BM"

    BmpInfoHeader ih;
    if (fread(&ih, sizeof(ih), 1, f) != 1){ fclose(f); return 0; }

    if (ih.biSize < 40){ fclose(f); return 0; }
    if (ih.biCompression != 0){ fclose(f); return 0; } // только BI_RGB
    if (ih.biBitCount != 24 && ih.biBitCount != 32){ fclose(f); return 0; }

    int w = ih.biWidth;
    int h = ih.biHeight;
    if (w <= 0 || h == 0 || h==INT_MIN){ fclose(f); return 0; }

    int bottom_up = 1;
    if (h < 0){
        bottom_up = 0;
        h = -h;
    }

    if (fseek(f, (long)fh.bfOffBits, SEEK_SET) != 0){ fclose(f); return 0; }

    size_t bytes_per_pixel=(size_t)(ih.biBitCount/8);
    size_t width=(size_t)w;
    size_t height=(size_t)h;
    if (width>SIZE_MAX/bytes_per_pixel){ fclose(f); return 0; }
    size_t row_bytes_raw=width*bytes_per_pixel;
    if (row_bytes_raw>SIZE_MAX-3){ fclose(f); return 0; }
    /* BMP rows are aligned to four bytes. */
    size_t row_stride=(row_bytes_raw+3)&~(size_t)3;
    if (width>SIZE_MAX/height || width*height>SIZE_MAX/sizeof(uint32_t) ||
        width*height>KBITMAP_MAX_BYTES/sizeof(uint32_t)){
        fclose(f); return 0;
    }
    size_t pixel_bytes=width*height*sizeof(uint32_t);

    uint8_t* row_buf = (uint8_t*)kir_oom_malloc(row_stride);
    if (!row_buf){ fclose(f); return 0; }

    uint32_t* pixels = (uint32_t*)kir_oom_malloc(pixel_bytes);
    if (!pixels){ free(row_buf); fclose(f); return 0; }

    for (size_t y = 0; y < height; ++y){
        if (fread(row_buf, 1, row_stride, f) != row_stride){
            free(row_buf);
            free(pixels);
            fclose(f);
            return 0;
        }

        size_t dst_y=bottom_up?(height-1-y):y;
        uint32_t* dst=pixels+dst_y*width;

        for (size_t x=0;x<width;++x){
            uint8_t b=row_buf[x*bytes_per_pixel+0];
            uint8_t g=row_buf[x*bytes_per_pixel+1];
            uint8_t r=row_buf[x*bytes_per_pixel+2];
            uint32_t rgb = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            dst[x] = rgb;
        }
    }

    free(row_buf);
    fclose(f);

    *out_w = w;
    *out_h = h;
    *out_px = pixels;
    return 1;
}

/* ---------------- Нативные функции ---------------- */

// картинка_загрузить(путь) -> id или 0
static Value k_картинка_загрузить(int n, const Value* a){
    kbitmaps_init_once();
    if (n < 1 || a[0].kind != VAL_STR) return v_int(0);

    int w = 0, h = 0;
    uint32_t* px = NULL;

    if (!load_bmp_rgb(a[0].as.str.s, a[0].as.str.n, &w, &h, &px)){
        return v_int(0);
    }

    int slot = kbitmap_alloc_slot();
    if (slot < 0){
        free(px);
        return v_int(0);
    }

    g_bitmaps[slot].w  = w;
    g_bitmaps[slot].h  = h;
    g_bitmaps[slot].px = px;

    int id = slot + 1;
    return v_int(id);
}

// картинка_освободить(id) -> bool
static Value k_картинка_освободить(int n, const Value* a){
    kbitmaps_init_once();
    if (n < 1 || a[0].kind != VAL_NUM) return v_bool(false);
    int64_t id64=0;
    if (!v_num_fits_i64(a[0], &id64) || id64 < INT_MIN || id64 > INT_MAX) return v_bool(false);
    int id = (int)id64;
    int idx = id - 1;
    if (idx < 0 || idx >= KBITMAP_MAX) return v_bool(false);
    kbitmap_free_slot(idx);
    return v_bool(true);
}

// картинка_размер(id) -> [w, h] или [0,0]
static Value k_картинка_размер(int n, const Value* a){
    kbitmaps_init_once();
    Value L = v_list_new();
    if (n < 1 || a[0].kind != VAL_NUM){
        v_list_push(&L, v_int(0));
        v_list_push(&L, v_int(0));
        return L;
    }
    int64_t id64=0;
    if (!v_num_fits_i64(a[0], &id64) || id64 < INT_MIN || id64 > INT_MAX){
        v_list_push(&L, v_int(0));
        v_list_push(&L, v_int(0));
        return L;
    }
    int id = (int)id64;
    int idx = id - 1;
    if (idx < 0 || idx >= KBITMAP_MAX || !g_bitmaps[idx].px){
        v_list_push(&L, v_int(0));
        v_list_push(&L, v_int(0));
        return L;
    }
    v_list_push(&L, v_int(g_bitmaps[idx].w));
    v_list_push(&L, v_int(g_bitmaps[idx].h));
    return L;
}

// картинка_пиксель(id, x, y) -> 0xRRGGBB или -1
static Value k_картинка_пиксель(int n, const Value* a){
    kbitmaps_init_once();
    if (n < 3 || a[0].kind != VAL_NUM || a[1].kind != VAL_NUM || a[2].kind != VAL_NUM){
        return v_int(-1);
    }
    int64_t id64=0, x64=0, y64=0;
    if (!v_num_fits_i64(a[0], &id64) || !v_num_fits_i64(a[1], &x64) || !v_num_fits_i64(a[2], &y64) ||
        id64 < INT_MIN || id64 > INT_MAX || x64 < INT_MIN || x64 > INT_MAX || y64 < INT_MIN || y64 > INT_MAX)
        return v_int(-1);
    int id = (int)id64;
    int x  = (int)x64;
    int y  = (int)y64;
    int idx = id - 1;
    if (idx < 0 || idx >= KBITMAP_MAX || !g_bitmaps[idx].px){
        return v_int(-1);
    }
    KBitmap* bm = &g_bitmaps[idx];
    if (x < 0 || y < 0 || x >= bm->w || y >= bm->h){
        return v_int(-1);
    }
    uint32_t rgb = bm->px[(size_t)y * (size_t)bm->w + (size_t)x];
    return v_int((int64_t)rgb);
}

/* ---------------- Регистрация ---------------- */

static const KirNativeDef FUNCS[] = {
    {"картинка_загрузить",  k_картинка_загрузить},
    {"картинка_освободить", k_картинка_освободить},
    {"картинка_размер",     k_картинка_размер},
    {"картинка_пиксель",    k_картинка_пиксель},
};

void bitmap_init(void){
    kbitmaps_init_once();
    kir_native_register(FUNCS, (int)(sizeof(FUNCS)/sizeof(FUNCS[0])));
    kir_native_mark_bytecode_safe(FUNCS, sizeof(FUNCS)/sizeof(FUNCS[0]));
}
