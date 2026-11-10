/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
// src/mod_draw2d.c — простой 2D-рендер в память + сохранение BMP
// Зависимости: стандартная библиотека C. Кодировка UTF-8.

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "kirton/native.h"
#include "native_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

// ---------- Внутренний формат изображения ----------
typedef struct {
    int w, h;
    uint32_t* px; // RGBA (8-8-8-8)
} KImg;

static KImg* g_imgs = NULL;
static int   g_imgs_count = 0;
static int   g_imgs_cap   = 0;

static int img_new(int w, int h){
    if (w <= 0 || h <= 0) return -1;
    if (g_imgs_count == g_imgs_cap){
        int nc = g_imgs_cap ? g_imgs_cap*2 : 8;
        KImg* ni = (KImg*)realloc(g_imgs, sizeof(KImg)*nc);
        if (!ni) return -1;
        g_imgs = ni; g_imgs_cap = nc;
    }
    uint32_t* buf = (uint32_t*)calloc((size_t)w*h, sizeof(uint32_t));
    if (!buf) return -1;
    KImg img; img.w = w; img.h = h; img.px = buf;
    g_imgs[g_imgs_count] = img;
    return g_imgs_count++;
}

static KImg* img_get(int id){
    if (id < 0 || id >= g_imgs_count) return NULL;
    if (!g_imgs[id].px) return NULL;
    return &g_imgs[id];
}

static bool img_del(int id){
    KImg* im = img_get(id);
    if (!im) return false;
    free(im->px);
    im->px = NULL; im->w = im->h = 0;
    return true;
}

static inline uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a){
    return ((uint32_t)r<<24) | ((uint32_t)g<<16) | ((uint32_t)b<<8) | (uint32_t)a;
}

static inline void put_px(KImg* im, int x, int y, uint32_t c){
    if ((unsigned)x < (unsigned)im->w && (unsigned)y < (unsigned)im->h){
        im->px[(size_t)y*im->w + x] = c;
    }
}

// ---------- Фигура: очистка/пиксель/линия/прямоугольник/окружность ----------
static void img_clear(KImg* im, uint32_t c){
    size_t n = (size_t)im->w * im->h;
    for (size_t i=0;i<n;i++) im->px[i] = c;
}

static void img_line(KImg* im, int x1,int y1,int x2,int y2, uint32_t c){
    // Bresenham
    int dx = abs(x2 - x1), sx = x1 < x2 ? 1 : -1;
    int dy = -abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
    int err = dx + dy, e2;
    for(;;){
        put_px(im, x1, y1, c);
        if (x1==x2 && y1==y2) break;
        e2 = 2*err;
        if (e2 >= dy){ err += dy; x1 += sx; }
        if (e2 <= dx){ err += dx; y1 += sy; }
    }
}

static void img_rect(KImg* im, int x,int y,int w,int h, uint32_t c, bool fill){
    if (w<=0 || h<=0) return;
    if (fill){
        int x2 = x + w;
        int y2 = y + h;
        if (x < 0) x = 0; if (y < 0) y = 0;
        if (x2 > im->w) x2 = im->w; if (y2 > im->h) y2 = im->h;
        for (int yy = y; yy < y2; ++yy){
            for (int xx = x; xx < x2; ++xx){
                put_px(im, xx, yy, c);
            }
        }
    }else{
        img_line(im, x, y, x+w-1, y, c);
        img_line(im, x, y, x, y+h-1, c);
        img_line(im, x+w-1, y, x+w-1, y+h-1, c);
        img_line(im, x, y+h-1, x+w-1, y+h-1, c);
    }
}

static void img_circle(KImg* im, int cx,int cy,int r, uint32_t c, bool fill){
    if (r <= 0) return;
    int x = r, y = 0;
    int err = 1 - r;
    if (fill){
        while (x >= y){
            for (int xx = cx - x; xx <= cx + x; ++xx){ put_px(im, xx, cy + y, c); put_px(im, xx, cy - y, c); }
            for (int xx = cx - y; xx <= cx + y; ++xx){ put_px(im, xx, cy + x, c); put_px(im, xx, cy - x, c); }
            y++;
            if (err < 0) err += 2*y + 1;
            else { x--; err += 2*(y - x + 1); }
        }
    }else{
        while (x >= y){
            put_px(im, cx + x, cy + y, c);
            put_px(im, cx + y, cy + x, c);
            put_px(im, cx - y, cy + x, c);
            put_px(im, cx - x, cy + y, c);
            put_px(im, cx - x, cy - y, c);
            put_px(im, cx - y, cy - x, c);
            put_px(im, cx + y, cy - x, c);
            put_px(im, cx + x, cy - y, c);
            y++;
            if (err < 0) err += 2*y + 1;
            else { x--; err += 2*(y - x + 1); }
        }
    }
}

// ---------- BMP (24-бит, без альфы) ----------
static bool img_save_bmp(KImg* im, const char* path_utf8){
    FILE* f = fopen(path_utf8, "wb");
    if (!f) return false;

    int w = im->w, h = im->h;
    int rowSize = ((w * 3 + 3) / 4) * 4;
    int dataSize = rowSize * h;
    int fileSize = 14 + 40 + dataSize;

    // BITMAPFILEHEADER
    uint8_t fileHdr[14] = {
        'B','M',
        (uint8_t)(fileSize      ), (uint8_t)(fileSize>>8),
        (uint8_t)(fileSize>>16  ), (uint8_t)(fileSize>>24),
        0,0, 0,0,
        54,0,0,0
    };
    // BITMAPINFOHEADER
    uint8_t infoHdr[40];
    memset(infoHdr, 0, sizeof(infoHdr));
    *(uint32_t*)(infoHdr+ 0) = 40;         // biSize
    *(int32_t* )(infoHdr+ 4) = w;          // biWidth
    *(int32_t* )(infoHdr+ 8) = h;          // biHeight (положительный => нижняя строка первая)
    *(uint16_t*)(infoHdr+12) = 1;          // biPlanes
    *(uint16_t*)(infoHdr+14) = 24;         // biBitCount
    *(uint32_t*)(infoHdr+20) = (uint32_t)dataSize;   // biSizeImage

    fwrite(fileHdr, 1, 14, f);
    fwrite(infoHdr, 1, 40, f);

    // данные снизу вверх
    uint8_t* row = (uint8_t*)malloc((size_t)rowSize);
    if (!row){ fclose(f); return false; }
    for (int y = h-1; y >= 0; --y){
        uint8_t* p = row;
        for (int x = 0; x < w; ++x){
            uint32_t c = im->px[(size_t)y*w + x];
            uint8_t r = (uint8_t)(c>>24), g = (uint8_t)(c>>16), b = (uint8_t)(c>>8);
            *p++ = b; *p++ = g; *p++ = r;
        }
        while ((p - row) % 4) *p++ = 0;  // паддинг
        fwrite(row, 1, (size_t)(p - row), f);
    }
    free(row);
    fclose(f);
    return true;
}

// ---------- Киртон-обёртки ----------
static bool draw_read_rgba(const Value* args, int offset, uint8_t out[4]){
    for (int i=0; i<4; ++i){
        int channel;
        if (!v_num_to_int_trunc(args[offset+i], &channel)) return false;
        out[i] = (uint8_t)channel;
    }
    return true;
}

static Value f_img_create(int n, const Value* a){
    if (n<2 || a[0].kind!=VAL_NUM || a[1].kind!=VAL_NUM) return v_int(-1);
    int w, h;
    if (!v_num_to_int_trunc(a[0], &w) || !v_num_to_int_trunc(a[1], &h)) return v_int(-1);
    int id = img_new(w,h);
    return v_int(id);
}

static Value f_img_delete(int n, const Value* a){
    if (n<1 || a[0].kind!=VAL_NUM) return v_bool(false);
    int id;
    if (!v_num_to_int_trunc(a[0], &id)) return v_bool(false);
    return v_bool(img_del(id));
}

static Value f_img_clear(int n, const Value* a){
    if (n<5 || a[0].kind!=VAL_NUM) return v_bool(false);
    int id; uint8_t color[4];
    if (!v_num_to_int_trunc(a[0], &id) || !draw_read_rgba(a, 1, color)) return v_bool(false);
    KImg* im = img_get(id); if (!im) return v_bool(false);
    img_clear(im, rgba(color[0],color[1],color[2],color[3]));
    return v_bool(true);
}

static Value f_px(int n, const Value* a){
    if (n<7 || a[0].kind!=VAL_NUM) return v_bool(false);
    int id, x, y; uint8_t color[4];
    if (!v_num_to_int_trunc(a[0], &id) || !v_num_to_int_trunc(a[1], &x) || !v_num_to_int_trunc(a[2], &y) || !draw_read_rgba(a, 3, color)) return v_bool(false);
    KImg* im = img_get(id); if (!im) return v_bool(false);
    put_px(im, x,y, rgba(color[0],color[1],color[2],color[3]));
    return v_bool(true);
}

static Value f_line(int n, const Value* a){
    if (n<9 || a[0].kind!=VAL_NUM) return v_bool(false);
    int id, x1, y1, x2, y2; uint8_t color[4];
    if (!v_num_to_int_trunc(a[0], &id) || !v_num_to_int_trunc(a[1], &x1) || !v_num_to_int_trunc(a[2], &y1) ||
        !v_num_to_int_trunc(a[3], &x2) || !v_num_to_int_trunc(a[4], &y2) || !draw_read_rgba(a, 5, color)) return v_bool(false);
    KImg* im = img_get(id); if (!im) return v_bool(false);
    img_line(im, x1,y1,x2,y2, rgba(color[0],color[1],color[2],color[3]));
    return v_bool(true);
}

static Value f_rect(int n, const Value* a){
    if (n<9 || a[0].kind!=VAL_NUM) return v_bool(false);
    int id, x, y, w, h, fill_value=0; uint8_t color[4];
    if (!v_num_to_int_trunc(a[0], &id) || !v_num_to_int_trunc(a[1], &x) || !v_num_to_int_trunc(a[2], &y) ||
        !v_num_to_int_trunc(a[3], &w) || !v_num_to_int_trunc(a[4], &h) || !draw_read_rgba(a, 5, color) ||
        (n>=10 && a[9].kind==VAL_NUM && !v_num_to_int_trunc(a[9], &fill_value))) return v_bool(false);
    KImg* im = img_get(id); if (!im) return v_bool(false);
    bool fill = (n>=10 && a[9].kind==VAL_NUM ? fill_value!=0 : true);
    img_rect(im, x,y,w,h, rgba(color[0],color[1],color[2],color[3]), fill);
    return v_bool(true);
}

static Value f_circle(int n, const Value* a){
    if (n<8 || a[0].kind!=VAL_NUM) return v_bool(false);
    int id, cx, cy, radius, fill_value=0; uint8_t color[4];
    if (!v_num_to_int_trunc(a[0], &id) || !v_num_to_int_trunc(a[1], &cx) || !v_num_to_int_trunc(a[2], &cy) ||
        !v_num_to_int_trunc(a[3], &radius) || !draw_read_rgba(a, 4, color) ||
        (n>=9 && a[8].kind==VAL_NUM && !v_num_to_int_trunc(a[8], &fill_value))) return v_bool(false);
    KImg* im = img_get(id); if (!im) return v_bool(false);
    bool fill = (n>=9 && a[8].kind==VAL_NUM ? fill_value!=0 : false);
    img_circle(im, cx,cy,radius, rgba(color[0],color[1],color[2],color[3]), fill);
    return v_bool(true);
}

static Value f_save_bmp(int n, const Value* a){
    if (n<2 || a[0].kind!=VAL_NUM || a[1].kind!=VAL_STR) return v_bool(false);
    int id;
    if (!v_num_to_int_trunc(a[0], &id)) return v_bool(false);
    KImg* im = img_get(id); if (!im) return v_bool(false);
    const char* s = a[1].as.str.s;
    size_t len = a[1].as.str.n;
    char* path = (char*)malloc(len+1);
    if (!path) return v_bool(false);
    memcpy(path, s, len); path[len]=0;
    bool ok = img_save_bmp(im, path);
    free(path);
    return v_bool(ok);
}

// ---------- Регистрация ----------
static const KirNativeDef DRAW_FUNCS[] = {
    {"img_создать",        f_img_create},
    {"img_удалить",        f_img_delete},
    {"img_очистить",       f_img_clear},
    {"пиксель",            f_px},
    {"линия",              f_line},
    {"прямоугольник",      f_rect},
    {"окружность",         f_circle},
    {"img_сохранить_bmp",  f_save_bmp},
};

void draw2d_init(void) {  // <- ИМЕННО ТАК, без других имён
    kir_native_register(DRAW_FUNCS, (int)(sizeof(DRAW_FUNCS)/sizeof(DRAW_FUNCS[0])));
    kir_native_mark_bytecode_safe(DRAW_FUNCS, sizeof(DRAW_FUNCS)/sizeof(DRAW_FUNCS[0]));
}
