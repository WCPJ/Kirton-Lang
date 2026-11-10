/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
// src/mod_window.c — оконный модуль Киртона (Windows, GDI + DIB32 backbuffer)
// Возможности:
//   окно(w,h,title), окно_открыто()
//   очистить(...), пиксель, линия, прямоугольник(x,y,w,h, цвет, [заливка=1])
//   окружность(cx,cy,r, цвет, [заливка=0])
//   текст(x,y,строка, размер_px, цветRGB)
//   живо_шаг()
//   читать_клавишу(), читать_клавишу_если_есть()
//   клавиша_зажата(vk), клавиша_нажата(vk), клавиша_отпущена(vk)
//   курсор_позиция() -> [x,y], кнопка_мыши(vk), колесо_мыши() -> int
//   размер_окна() -> [w,h], установить_заголовок(str)
//   FPS_цель(fps)
//   текст_ширина(строка, размер_px) -> число
//   текст_размер(строка, размер_px) -> [w,h]
//   шрифт_метрики(размер_px) -> {высота, асцент, десцент, межстрочный, средняя_ширина}
//   текст_метрики(размер_px) -> алиас шрифт_метрики
//
// Требует: kirton/native.h, kirton/value.h
// Компиляция: только на Windows.

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "kirton/native.h"
#include "native_internal.h"
#include "kirton/value.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <wchar.h>
#endif

#ifndef _WIN32
// Заглушки для не-Windows
static const KirNativeDef FUNCS[] = {};
void window_init(void){ (void)FUNCS; }
#else

/* ===================== Утилиты цвета/строк ===================== */

// DIB32 пиксель: 0x00BBGGRR (в памяти [B][G][R][0])
static inline uint32_t dib_rgb(uint8_t r, uint8_t g, uint8_t b){
    return (uint32_t)( (uint32_t)b | ((uint32_t)g<<8) | ((uint32_t)r<<16) );
}

// вход: либо 0xRRGGBB одним числом, либо r,g,b по отдельности
static inline uint32_t color_from_args(int n, const Value* a, int idx){
    if (idx<n && a[idx].kind==VAL_NUM && (idx+1>=n || a[idx+1].kind!=VAL_NUM)){
        int c;
        if (!v_num_to_int_trunc(a[idx], &c)) return dib_rgb(0,0,0);
        uint8_t r=(uint8_t)((c>>16)&255), g=(uint8_t)((c>>8)&255), b=(uint8_t)(c&255);
        return dib_rgb(r,g,b);
    }
    if (idx+2<n && a[idx].kind==VAL_NUM && a[idx+1].kind==VAL_NUM && a[idx+2].kind==VAL_NUM){
        int ri, gi, bi;
        if (!v_num_to_int_trunc(a[idx], &ri) || !v_num_to_int_trunc(a[idx+1], &gi) || !v_num_to_int_trunc(a[idx+2], &bi)) return dib_rgb(0,0,0);
        uint8_t r=(uint8_t)ri, g=(uint8_t)gi, b=(uint8_t)bi;
        return dib_rgb(r,g,b);
    }
    return dib_rgb(0,0,0);
}

static wchar_t* u8_to_w(const char* s, int n){
    if (!s) return NULL;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s, (n>=0)?n:-1, NULL, 0);
    if (wlen<=0) return NULL;
    wchar_t* w = (wchar_t*)malloc((size_t)(wlen+1)*sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, (n>=0)?n:-1, w, wlen);
    w[wlen]=0; return w;
}

/* ===================== Бэкбуфер ===================== */

typedef struct { int w,h; uint32_t* px; } KBuf;
static KBuf g_buf = {0,0,NULL};

static inline void buf_put(int x,int y,uint32_t c){
    if ((unsigned)x<(unsigned)g_buf.w && (unsigned)y<(unsigned)g_buf.h)
        g_buf.px[(size_t)y*g_buf.w + x] = c;
}
static bool buf_create(int w,int h){
    if (w<=0||h<=0) return false;
    free(g_buf.px); g_buf.px=NULL; g_buf.w=g_buf.h=0;
    g_buf.px = (uint32_t*)malloc((size_t)w*h*sizeof(uint32_t));
    if (!g_buf.px) return false;
    g_buf.w=w; g_buf.h=h;
    memset(g_buf.px,0,(size_t)w*h*sizeof(uint32_t));
    return true;
}
static void buf_destroy(void){ free(g_buf.px); g_buf.px=NULL; g_buf.w=g_buf.h=0; }
static void buf_clear(uint32_t c){
    if (!g_buf.px) return;
    size_t n=(size_t)g_buf.w*g_buf.h;
    for (size_t i=0;i<n;i++) g_buf.px[i]=c;
}

/* ===================== Примитивы рисования ===================== */

static void buf_line(int x1,int y1,int x2,int y2,uint32_t c){
    int dx=abs(x2-x1), sx=x1<x2?1:-1;
    int dy=-abs(y2-y1), sy=y1<y2?1:-1;
    int err=dx+dy, e2;
    for(;;){
        buf_put(x1,y1,c);
        if (x1==x2 && y1==y2) break;
        e2=2*err; if (e2>=dy){ err+=dy; x1+=sx; } if (e2<=dx){ err+=dx; y1+=sy; }
    }
}
static void buf_rect(int x,int y,int w,int h,uint32_t c,bool fill){
    if (w<=0||h<=0) return;
    int x2=x+w-1, y2=y+h-1;
    if (fill){
        for(int yy=y; yy<=y2; yy++) for(int xx=x; xx<=x2; xx++) buf_put(xx,yy,c);
    }else{
        buf_line(x,y,x2,y,c); buf_line(x,y,x,y2,c); buf_line(x2,y,x2,y2,c); buf_line(x,y2,x2,y2,c);
    }
}
static void buf_circle(int cx,int cy,int r,uint32_t c,bool fill){
    if (r<=0) return;
    int x=r,y=0, err=1-r;
    if (fill){
        while (x>=y){
            for (int xx=cx-x; xx<=cx+x; xx++){ buf_put(xx,cy+y,c); buf_put(xx,cy-y,c); }
            for (int xx=cx-y; xx<=cx+y; xx++){ buf_put(xx,cy+x,c); buf_put(xx,cy-x,c); }
            y++; if (err<0) err+=2*y+1; else { x--; err+=2*(y-x+1); }
        }
    }else{
        while (x>=y){
            buf_put(cx+x,cy+y,c); buf_put(cx+y,cy+x,c); buf_put(cx-y,cy+x,c); buf_put(cx-x,cy+y,c);
            buf_put(cx-x,cy-y,c); buf_put(cx-y,cy-x,c); buf_put(cx+y,cy-x,c); buf_put(cx+x,cy-y,c);
            y++; if (err<0) err+=2*y+1; else { x--; err+=2*(y-x+1); }
        }
    }
}

/* ===================== Текст (кэш шрифтов) ===================== */

typedef struct { int size; HFONT font; } FontEnt;
#define FONT_CACHE_MAX 12
static FontEnt g_fonts[FONT_CACHE_MAX];
static int g_fonts_n = 0;

static HFONT font_for_size(HDC dc, int px){
    if (px<6) px=6;
    for (int i=0;i<g_fonts_n;i++) if (g_fonts[i].size==px) return g_fonts[i].font;
    int logpix = GetDeviceCaps(dc, LOGPIXELSY);
    int lfHeight = -MulDiv(px, logpix, 72);
    HFONT f = CreateFontW(lfHeight,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          ANTIALIASED_QUALITY, DEFAULT_PITCH|FF_DONTCARE, L"Segoe UI");
    if (!f) return NULL;
    if (g_fonts_n < FONT_CACHE_MAX){ g_fonts[g_fonts_n].size=px; g_fonts[g_fonts_n].font=f; g_fonts_n++; }
    return f;
}

typedef struct { int x,y,size; COLORREF rgb; wchar_t* w; } TextItem;
static TextItem* g_texts=NULL; static int g_t_len=0, g_t_cap=0;
static void texts_reset(void){ for(int i=0;i<g_t_len;i++) free(g_texts[i].w); g_t_len=0; }
static void texts_dispose(void){ texts_reset(); free(g_texts); g_texts=NULL; g_t_cap=0; }
static void fonts_dispose(void){ for(int i=0;i<g_fonts_n;i++) if (g_fonts[i].font) DeleteObject(g_fonts[i].font); g_fonts_n=0; }
static void texts_push(int x,int y,const char* s,int n,int size,int rgb){
    if (g_t_len+1>g_t_cap){
        int nc=g_t_cap?g_t_cap*2:16; TextItem* nt=(TextItem*)realloc(g_texts,(size_t)nc*sizeof(TextItem));
        if (!nt) return; g_texts=nt; g_t_cap=nc;
    }
    g_texts[g_t_len].x=x; g_texts[g_t_len].y=y; g_texts[g_t_len].size=(size>0?size:16);
    uint8_t r=(rgb>>16)&255, g=(rgb>>8)&255, b=rgb&255;
    g_texts[g_t_len].rgb = RGB(r,g,b);
    g_texts[g_t_len].w   = u8_to_w(s,n);
    if (g_texts[g_t_len].w) g_t_len++;
}

/* ===================== Окно/ввод/тайминг ===================== */

static HWND g_hwnd=NULL;
static uint32_t g_bg = 0;
static int g_mouse_x=0, g_mouse_y=0;
static int g_wheel_delta=0; // накопитель; выдаём и обнуляем в колесо_мыши()

// Состояние клавиатуры (по GetAsyncKeyState → edge детекция в живо_шаг)
static uint8_t g_key_down[256]={0};
static uint8_t g_key_prev[256]={0};
static int     g_last_key = -1; // последняя «сырая» клавиша (для читать_клавишу())

// FPS таргет
static double g_target_ms = 0.0; // 0 = без лимита
static LARGE_INTEGER g_qpf = {0}, g_last_tick = {0};

// Презентация кадра: blit DIB и все тексты
static void present(HDC dc){
    if (!dc || !g_buf.px) return;
    BITMAPINFO bi; ZeroMemory(&bi,sizeof(bi));
    bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth=g_buf.w; bi.bmiHeader.biHeight=-(int)g_buf.h; // top-down
    bi.bmiHeader.biPlanes=1; bi.bmiHeader.biBitCount=32; bi.bmiHeader.biCompression=BI_RGB;
    StretchDIBits(dc, 0,0,g_buf.w,g_buf.h, 0,0,g_buf.w,g_buf.h, g_buf.px, &bi, DIB_RGB_COLORS, SRCCOPY);

    SetBkMode(dc, TRANSPARENT);
    for (int i=0;i<g_t_len;i++){
        HFONT f = font_for_size(dc, g_texts[i].size);
        HFONT old=(HFONT)SelectObject(dc, f);
        SetTextColor(dc, g_texts[i].rgb);
        const wchar_t* ws = g_texts[i].w? g_texts[i].w : L"";
        TextOutW(dc, g_texts[i].x, g_texts[i].y, ws, (int)wcslen(ws));
        SelectObject(dc, old);
    }
}

static void update_key_arrays(void){
    for (int vk=0; vk<256; vk++){
        g_key_prev[vk] = g_key_down[vk];
        SHORT s = GetAsyncKeyState(vk);
        g_key_down[vk] = (s & 0x8000) ? 1 : 0;
    }
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l){
    switch (m){
        case WM_CREATE:
            return 0;
        case WM_SETCURSOR:
            SetCursor(LoadCursor(NULL, IDC_ARROW));
            return TRUE;
        case WM_SYSKEYDOWN: // системные (Esc/Alt+...)
        case WM_KEYDOWN:
            g_last_key = (int)w;
            return 0;
        case WM_MOUSEMOVE:
            g_mouse_x = GET_X_LPARAM(l);
            g_mouse_y = GET_Y_LPARAM(l);
            return 0;
        case WM_MOUSEWHEEL:
            g_wheel_delta += GET_WHEEL_DELTA_WPARAM(w);
            return 0;
        case WM_SIZE: {
            int cw = LOWORD(l), ch = HIWORD(l);
            if (cw>0 && ch>0){
                if (buf_create(cw,ch)) buf_clear(g_bg);
                // без лишних RedrawWindow — кадр обновится в живо_шаг()
            }
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC dc=BeginPaint(h,&ps);
            // Ничего не рисуем здесь — весь вывод делаем в живо_шаг()/present()
            EndPaint(h,&ps);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(h,m,w,l);
}

static bool create_window(int w,int h,const wchar_t* title){
    HINSTANCE hi = GetModuleHandleW(NULL);
    static const wchar_t* kClass=L"KirtonWin32DIB2";
    static ATOM atom=0;
    if (!atom){
        WNDCLASSW wc; ZeroMemory(&wc,sizeof(wc));
        wc.style=CS_HREDRAW|CS_VREDRAW|CS_OWNDC;
        wc.lpfnWndProc=WndProc; wc.hInstance=hi;
        wc.hCursor=LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
        wc.lpszClassName=kClass;
        atom=RegisterClassW(&wc);
        if (!atom) return false;
    }

    RECT r={0,0,w,h};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    int W=r.right-r.left, H=r.bottom-r.top;

    g_hwnd = CreateWindowW(kClass, title?title:L"Киртон",
                           WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, W,H,
                           NULL,NULL,hi,NULL);
    if (!g_hwnd) return false;

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    if (!buf_create(w,h)) return false;
    buf_clear(g_bg);

    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_last_tick);
    return true;
}

static bool pump_once(void){
    MSG msg;
    while (PeekMessageW(&msg,NULL,0,0,PM_REMOVE)){
        if (msg.message==WM_QUIT) return false;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (g_hwnd!=NULL);
}

static void window_process_cleanup(void){
    if (g_hwnd && IsWindow(g_hwnd)) DestroyWindow(g_hwnd);
    g_hwnd=NULL;
    texts_dispose();
    fonts_dispose();
    buf_destroy();
}

/* ===================== Измерение текста / метрики шрифта ===================== */

static bool with_dc_do(bool use_window, void (*fn)(HDC, void*), void* ud){
    HDC dc = NULL;
    if (use_window && g_hwnd){
        dc = GetDC(g_hwnd);
    }else{
        dc = GetDC(NULL); // экранный DC
    }
    if (!dc) return false;
    fn(dc, ud);
    if (use_window && g_hwnd) ReleaseDC(g_hwnd, dc); else ReleaseDC(NULL, dc);
    return true;
}

typedef struct {
    const wchar_t* ws;
    int px;
    SIZE sz;
    int ascent, descent, lineh, avew;
} MeasureCtx;

static void measure_text_do(HDC dc, void* u){
    MeasureCtx* m = (MeasureCtx*)u;
    HFONT f = font_for_size(dc, m->px);
    HFONT old = f ? (HFONT)SelectObject(dc, f) : NULL;

    SetBkMode(dc, TRANSPARENT);
    if (m->ws && m->ws[0]){
        GetTextExtentPoint32W(dc, m->ws, (int)wcslen(m->ws), &m->sz);
    }else{
        TEXTMETRICW tm; GetTextMetricsW(dc, &tm);
        m->sz.cx = 0;
        m->sz.cy = tm.tmHeight;
    }

    TEXTMETRICW tm; GetTextMetricsW(dc, &tm);
    m->ascent  = tm.tmAscent;
    m->descent = tm.tmDescent;
    m->avew    = tm.tmAveCharWidth;
    m->lineh   = tm.tmHeight + tm.tmExternalLeading;

    if (old) SelectObject(dc, old);
}

/* текст_ширина(строка, размер_px) -> число */
static Value k_текст_ширина(int n, const Value* a){
    if (n<2 || a[0].kind!=VAL_STR || a[1].kind!=VAL_NUM) return v_int(0);
    int px;
    if (!v_num_to_int_trunc(a[1], &px)) return v_int(0);
    wchar_t* ws = u8_to_w(a[0].as.str.s, a[0].as.str.n);
    MeasureCtx m = { ws, px, {0,0}, 0,0,0,0 };
    with_dc_do(true, measure_text_do, &m);
    if (ws) free(ws);
    return v_int(m.sz.cx);
}

/* текст_размер(строка, размер_px) -> [w,h] */
static Value k_текст_размер(int n, const Value* a){
    if (n<2 || a[0].kind!=VAL_STR || a[1].kind!=VAL_NUM) {
        Value L=v_list_new(); v_list_push(&L,v_int(0)); v_list_push(&L,v_int(0)); return L;
    }
    int px;
    if (!v_num_to_int_trunc(a[1], &px)) {
        Value L=v_list_new(); v_list_push(&L,v_int(0)); v_list_push(&L,v_int(0)); return L;
    }
    wchar_t* ws = u8_to_w(a[0].as.str.s, a[0].as.str.n);
    MeasureCtx m = { ws, px, {0,0}, 0,0,0,0 };
    with_dc_do(true, measure_text_do, &m);
    if (ws) free(ws);
    Value L = v_list_new();
    v_list_push(&L, v_int(m.sz.cx));
    v_list_push(&L, v_int(m.sz.cy));
    return L;
}

/* шрифт_метрики(размер_px) -> {высота, асцент, десцент, межстрочный, средняя_ширина} */
static Value k_шрифт_метрики(int n, const Value* a){
    if (n<1 || a[0].kind!=VAL_NUM) return v_dict_new();
    int px;
    if (!v_num_to_int_trunc(a[0], &px)) return v_dict_new();
    MeasureCtx m = { L"", px, {0,0}, 0,0,0,0 };
    with_dc_do(true, measure_text_do, &m);
    Value D = v_dict_new();
    v_dict_set(&D, "высота",         12, v_int(m.sz.cy));
    v_dict_set(&D, "асцент",         12, v_int(m.ascent));
    v_dict_set(&D, "десцент",        14, v_int(m.descent));
    v_dict_set(&D, "межстрочный",    22, v_int(m.lineh));
    v_dict_set(&D, "средняя_ширина", 26, v_int(m.avew));
    return D;
}

/* текст_метрики(размер_px) -> алиас шрифт_метрики */
static Value k_текст_метрики(int n, const Value* a){ return k_шрифт_метрики(n,a); }

/* ===================== Нативные функции языка ===================== */

// окно(w,h,title)
static Value k_окно(int n, const Value* a){
    if (n<3 || a[0].kind!=VAL_NUM || a[1].kind!=VAL_NUM || a[2].kind!=VAL_STR) return v_bool(false);
    int w, h;
    if (!v_num_to_int_trunc(a[0], &w) || !v_num_to_int_trunc(a[1], &h)) return v_bool(false);
    wchar_t* wt = u8_to_w(a[2].as.str.s, a[2].as.str.n);
    SetConsoleOutputCP(CP_UTF8); SetConsoleCP(CP_UTF8);
    bool ok = create_window(w,h,wt);
    if (wt) free(wt);
    return v_bool(ok);
}

// окно_открыто()
static Value k_окно_открыто(int n, const Value* a){ (void)n;(void)a; return v_bool(g_hwnd!=NULL); }

// очистить(...)
static Value k_очистить(int n, const Value* a){
    if (!g_buf.px) return v_bool(false);
    g_bg = color_from_args(n,a,0);
    buf_clear(g_bg);
    return v_bool(true);
}

// пиксель(x,y,цвет)
static Value k_пиксель(int n, const Value* a){
    if (!g_buf.px || n<3 || a[0].kind!=VAL_NUM || a[1].kind!=VAL_NUM) return v_bool(false);
    int x, y;
    if (!v_num_to_int_trunc(a[0], &x) || !v_num_to_int_trunc(a[1], &y)) return v_bool(false);
    uint32_t c=color_from_args(n,a,2); buf_put(x,y,c);
    return v_bool(true);
}

// линия(x1,y1,x2,y2,цвет)
static Value k_линия(int n, const Value* a){
    if (!g_buf.px || n<5) return v_bool(false);
    int x1, y1, x2, y2;
    if (!v_num_to_int_trunc(a[0], &x1) || !v_num_to_int_trunc(a[1], &y1) ||
        !v_num_to_int_trunc(a[2], &x2) || !v_num_to_int_trunc(a[3], &y2)) return v_bool(false);
    uint32_t c=color_from_args(n,a,4); buf_line(x1,y1,x2,y2,c);
    return v_bool(true);
}

// прямоугольник(x,y,w,h,цвет,[заливка=1])
static Value k_прямоугольник(int n, const Value* a){
    if (!g_buf.px || n<5) return v_bool(false);
    int x, y, w, h;
    if (!v_num_to_int_trunc(a[0], &x) || !v_num_to_int_trunc(a[1], &y) ||
        !v_num_to_int_trunc(a[2], &w) || !v_num_to_int_trunc(a[3], &h)) return v_bool(false);
    uint32_t c=color_from_args(n,a,4);
    bool fill=true;
    if (n>=6 && a[5].kind==VAL_NUM){ int value; if (!v_num_to_int_trunc(a[5], &value)) return v_bool(false); fill = value!=0; }
    buf_rect(x,y,w,h,c,fill);
    return v_bool(true);
}

// окружность(cx,cy,r,цвет,[заливка=0])
static Value k_окружность(int n, const Value* a){
    if (!g_buf.px || n<4) return v_bool(false);
    int cx, cy, r;
    if (!v_num_to_int_trunc(a[0], &cx) || !v_num_to_int_trunc(a[1], &cy) || !v_num_to_int_trunc(a[2], &r)) return v_bool(false);
    uint32_t c=color_from_args(n,a,3);
    bool fill=false;
    if (n>=5 && a[4].kind==VAL_NUM){ int value; if (!v_num_to_int_trunc(a[4], &value)) return v_bool(false); fill = value!=0; }
    buf_circle(cx,cy,r,c,fill);
    return v_bool(true);
}

// текст(x,y,str,size,colorRGB)
static Value k_текст(int n, const Value* a){
    if (!g_hwnd || n<5) return v_bool(false);
    if (a[0].kind!=VAL_NUM || a[1].kind!=VAL_NUM || a[2].kind!=VAL_STR || a[3].kind!=VAL_NUM || a[4].kind!=VAL_NUM)
        return v_bool(false);
    int x, y, px, color;
    if (!v_num_to_int_trunc(a[0], &x) || !v_num_to_int_trunc(a[1], &y) ||
        !v_num_to_int_trunc(a[3], &px) || !v_num_to_int_trunc(a[4], &color)) return v_bool(false);
    texts_push(x, y, a[2].as.str.s, a[2].as.str.n, px, color);
    return v_bool(true);
}

// живо_шаг(): сообщение → кадр → fps-сон → сброс «одноразового» текста
static Value k_живо_шаг(int n, const Value* a){
    (void)n;(void)a;
    if (!pump_once()){ texts_dispose(); fonts_dispose(); buf_destroy(); g_hwnd=NULL; return v_bool(false); }

    if (g_hwnd){
        HDC dc = GetDC(g_hwnd); present(dc); ReleaseDC(g_hwnd, dc);
    }

    // обновляем массивы клавиш (для edge-детекции)
    update_key_arrays();

    // FPS таргет
    if (g_target_ms > 0.0 && g_qpf.QuadPart){
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        double dt_ms = 1000.0*(now.QuadPart - g_last_tick.QuadPart)/(double)g_qpf.QuadPart;
        if (dt_ms < g_target_ms){
            DWORD ms = (DWORD)(g_target_ms - dt_ms);
            if (ms>0) Sleep(ms);
        }
        QueryPerformanceCounter(&g_last_tick);
    }else{
        Sleep(1);
    }

    // текст накапливался на кадр — освобождаем строки, чтобы не дублировать
    texts_reset();
    return v_bool(true);
}

// цель FPS
static Value k_FPS_цель(int n, const Value* a){
    if (n<1 || a[0].kind!=VAL_NUM){ g_target_ms=0.0; return v_bool(true); }
    double fps = a[0].as.num;
    if (fps <= 0.0){ g_target_ms = 0.0; }
    else { g_target_ms = 1000.0 / fps; }
    return v_bool(true);
}

// читать_клавишу() — блокирующая
static Value k_читать_клавишу(int n, const Value* a){
    (void)n;(void)a;
    for(;;){
        if (!pump_once()){ texts_dispose(); fonts_dispose(); buf_destroy(); g_hwnd=NULL; return v_int(-1); }
        if (g_last_key >= 0){ int k=g_last_key; g_last_key=-1; return v_int(k); }
        if (g_hwnd){ HDC dc=GetDC(g_hwnd); present(dc); ReleaseDC(g_hwnd,dc); }
        Sleep(1);
    }
}

// читать_клавишу_если_есть() — неблокирующая
static Value k_читать_клавишу_если_есть(int n, const Value* a){
    (void)n;(void)a;
    if (!pump_once()){ texts_dispose(); fonts_dispose(); buf_destroy(); g_hwnd=NULL; return v_int(-1); }
    int k=g_last_key; g_last_key=-1; return v_int(k<0?-1:k);
}

// клавиша_зажата(vk)
static Value k_клавиша_зажата(int n, const Value* a){
    if (n<1 || a[0].kind!=VAL_NUM) return v_bool(false);
    int vk;
    if (!v_num_to_int_trunc(a[0], &vk)) return v_bool(false);
    vk &= 255; return v_bool(g_key_down[vk]!=0);
}

// клавиша_нажата(vk) — фронт (edge) за последний кадр
static Value k_клавиша_нажата(int n, const Value* a){
    if (n<1 || a[0].kind!=VAL_NUM) return v_bool(false);
    int vk;
    if (!v_num_to_int_trunc(a[0], &vk)) return v_bool(false);
    vk &= 255; return v_bool(g_key_down[vk] && !g_key_prev[vk]);
}

// клавиша_отпущена(vk) — спад (edge)
static Value k_клавиша_отпущена(int n, const Value* a){
    if (n<1 || a[0].kind!=VAL_NUM) return v_bool(false);
    int vk;
    if (!v_num_to_int_trunc(a[0], &vk)) return v_bool(false);
    vk &= 255; return v_bool(!g_key_down[vk] && g_key_prev[vk]);
}

// курсор_позиция() -> [x,y]
static Value k_курсор_позиция(int n, const Value* a){
    (void)n;(void)a;
    Value L = v_list_new();
    v_list_push(&L, v_int(g_mouse_x));
    v_list_push(&L, v_int(g_mouse_y));
    return L;
}

// кнопка_мыши(vk) — VK_LBUTTON=1, VK_RBUTTON=2, VK_MBUTTON=4 ...
static Value k_кнопка_мыши(int n, const Value* a){
    if (n<1 || a[0].kind!=VAL_NUM) return v_bool(false);
    int vk;
    if (!v_num_to_int_trunc(a[0], &vk)) return v_bool(false);
    vk &= 255; SHORT s = GetAsyncKeyState(vk);
    return v_bool((s & 0x8000)!=0);
}

// колесо_мыши() -> int (накопленная дельта с прошлого вызова, в тиках WinAPI по 120)
static Value k_колесо_мыши(int n, const Value* a){
    (void)n;(void)a;
    int d = g_wheel_delta; g_wheel_delta=0;
    return v_int(d);
}

// размер_окна() -> [w,h]
static Value k_размер_окна(int n, const Value* a){
    (void)n;(void)a;
    RECT rc={0}; if (!g_hwnd) { Value L=v_list_new(); v_list_push(&L,v_int(0)); v_list_push(&L,v_int(0)); return L; }
    GetClientRect(g_hwnd,&rc);
    Value L=v_list_new(); v_list_push(&L,v_int(rc.right-rc.left)); v_list_push(&L,v_int(rc.bottom-rc.top)); return L;
}

// установить_заголовок(str)
static Value k_установить_заголовок(int n, const Value* a){
    if (n<1 || a[0].kind!=VAL_STR || !g_hwnd) return v_bool(false);
    wchar_t* wt = u8_to_w(a[0].as.str.s, a[0].as.str.n);
    if (!wt) return v_bool(false);
    SetWindowTextW(g_hwnd, wt); free(wt);
    return v_bool(true);
}

/* ===================== Регистрация ===================== */

static const KirNativeDef FUNCS[] = {
    {"окно",                 k_окно},
    {"окно_открыто",         k_окно_открыто},
    {"очистить",             k_очистить},
    {"пиксель",              k_пиксель},
    {"линия",                k_линия},
    {"прямоугольник",        k_прямоугольник},
    {"окружность",           k_окружность},
    {"текст",                k_текст},
    {"живо_шаг",             k_живо_шаг},
    {"FPS_цель",             k_FPS_цель},
    {"читать_клавишу",       k_читать_клавишу},
    {"читать_клавишу_если_есть", k_читать_клавишу_если_есть},
    {"клавиша_зажата",       k_клавиша_зажата},
    {"клавиша_нажата",       k_клавиша_нажата},
    {"клавиша_отпущена",     k_клавиша_отпущена},
    {"курсор_позиция",       k_курсор_позиция},
    {"курcор_позиция",       k_курсор_позиция}, // алиас с латинской 'c' для совместимости
    {"кнопка_мыши",          k_кнопка_мыши},
    {"колесо_мыши",          k_колесо_мыши},
    {"размер_окна",          k_размер_окна},
    {"установить_заголовок", k_установить_заголовок},

    // Новые функции метрик/измерений текста:
    {"текст_ширина",         k_текст_ширина},
    {"текст_размер",         k_текст_размер},
    {"шрифт_метрики",        k_шрифт_метрики},
    {"текст_метрики",        k_текст_метрики},
};

void window_init(void){
    static int cleanup_registered=0;
    if (!cleanup_registered){
        atexit(window_process_cleanup);
        cleanup_registered=1;
    }
    kir_native_register(FUNCS, (int)(sizeof(FUNCS)/sizeof(FUNCS[0])));
    kir_native_mark_bytecode_safe(FUNCS, sizeof(FUNCS)/sizeof(FUNCS[0]));
}

#endif // _WIN32
