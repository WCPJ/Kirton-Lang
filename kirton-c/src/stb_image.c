/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "../third_party/stb_image.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")
#endif

#include <stdlib.h>
#include <string.h>

static const char* g_stbi_err = NULL;
static void stbi_set_error(const char* msg){ g_stbi_err = msg; }

const char* stbi_failure_reason(void){ return g_stbi_err ? g_stbi_err : "unknown"; }

#ifdef _WIN32

static IWICImagingFactory* wic_factory(void){
    static IWICImagingFactory* f = NULL;
    if (f) return f;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    (void)hr;
    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IWICImagingFactory, (void**)&f);
    if (FAILED(hr)) return NULL;
    return f;
}

static unsigned char* wic_decode(IWICBitmapDecoder* dec, int* ox, int* oy, int* oc){
    if (!dec) return NULL;
    IWICBitmapFrameDecode* frame = NULL;
    if (FAILED(dec->lpVtbl->GetFrame(dec, 0, &frame))){ stbi_set_error("frame"); return NULL; }
    UINT w=0,h=0; frame->lpVtbl->GetSize(frame, &w, &h);
    if (w==0 || h==0){ frame->lpVtbl->Release(frame); stbi_set_error("size"); return NULL; }

    IWICFormatConverter* conv = NULL;
    IWICImagingFactory* f = wic_factory();
    if (!f){ frame->lpVtbl->Release(frame); stbi_set_error("factory"); return NULL; }
    HRESULT hr = f->lpVtbl->CreateFormatConverter(f, &conv);
    if (FAILED(hr)){ frame->lpVtbl->Release(frame); stbi_set_error("converter"); return NULL; }

    hr = conv->lpVtbl->Initialize(conv, (IWICBitmapSource*)frame,
                                  &GUID_WICPixelFormat32bppRGBA,
                                  WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
    frame->lpVtbl->Release(frame);
    if (FAILED(hr)){ conv->lpVtbl->Release(conv); stbi_set_error("conv_init"); return NULL; }

    const size_t stride = (size_t)w * 4;
    unsigned char* data = (unsigned char*)malloc((size_t)h * stride);
    if (!data){ conv->lpVtbl->Release(conv); stbi_set_error("oom"); return NULL; }
    hr = conv->lpVtbl->CopyPixels(conv, NULL, (UINT)stride, (UINT)((size_t)h*stride), data);
    conv->lpVtbl->Release(conv);
    if (FAILED(hr)){ free(data); stbi_set_error("copy"); return NULL; }
    if (ox) *ox = (int)w;
    if (oy) *oy = (int)h;
    if (oc) *oc = 4;
    return data;
}

unsigned char *stbi_load(char const *filename, int *x, int *y, int *comp, int req_comp){
    (void)req_comp; g_stbi_err = NULL;
    IWICImagingFactory* f = wic_factory();
    if (!f){ stbi_set_error("factory"); return NULL; }
    wchar_t wpath[MAX_PATH];
    int wlen = MultiByteToWideChar(CP_UTF8, 0, filename, -1, wpath, MAX_PATH);
    if (wlen == 0){ stbi_set_error("path"); return NULL; }
    IWICBitmapDecoder* dec = NULL;
    HRESULT hr = f->lpVtbl->CreateDecoderFromFilename(f, wpath, NULL, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnLoad, &dec);
    if (FAILED(hr)){ stbi_set_error("decoder"); return NULL; }
    unsigned char* out = wic_decode(dec, x, y, comp);
    dec->lpVtbl->Release(dec);
    return out;
}

unsigned char *stbi_load_from_memory(const unsigned char *buffer, int len, int *x, int *y, int *comp, int req_comp){
    (void)req_comp; g_stbi_err = NULL;
    IWICImagingFactory* f = wic_factory();
    if (!f){ stbi_set_error("factory"); return NULL; }
    IWICStream* stream = NULL;
    HRESULT hr = f->lpVtbl->CreateStream(f, &stream);
    if (FAILED(hr)){ stbi_set_error("stream"); return NULL; }
    hr = stream->lpVtbl->InitializeFromMemory(stream, (BYTE*)buffer, (DWORD)len);
    if (FAILED(hr)){ stream->lpVtbl->Release(stream); stbi_set_error("stream_init"); return NULL; }
    IWICBitmapDecoder* dec = NULL;
    hr = f->lpVtbl->CreateDecoderFromStream(f, (IStream*)stream, NULL, WICDecodeMetadataCacheOnLoad, &dec);
    stream->lpVtbl->Release(stream);
    if (FAILED(hr)){ stbi_set_error("decoder"); return NULL; }
    unsigned char* out = wic_decode(dec, x, y, comp);
    dec->lpVtbl->Release(dec);
    return out;
}

int stbi_info(char const *filename, int *x, int *y, int *comp){
    int c=0; int w=0,h=0;
    unsigned char* p = stbi_load(filename, &w, &h, &c, 0);
    if (p){ stbi_image_free(p); if (x) *x=w; if (y) *y=h; if (comp) *comp=c; return 1; }
    return 0;
}

int stbi_info_from_memory(const unsigned char *buffer, int len, int *x, int *y, int *comp){
    int c=0; int w=0,h=0;
    unsigned char* p = stbi_load_from_memory(buffer, len, &w, &h, &c, 0);
    if (p){ stbi_image_free(p); if (x) *x=w; if (y) *y=h; if (comp) *comp=c; return 1; }
    return 0;
}

float *stbi_loadf(char const *filename, int *x, int *y, int *comp, int req_comp){
    (void)req_comp;
    int w=0,h=0,c=0;
    unsigned char* d = stbi_load(filename, &w,&h,&c,0);
    if (!d) return NULL;
    size_t n = (size_t)w*h*c;
    float* out = (float*)malloc(n*sizeof(float));
    if (!out){ stbi_image_free(d); return NULL; }
    for (size_t i=0;i<n;i++) out[i] = d[i]/255.0f;
    stbi_image_free(d);
    if (x) *x=w; if (y) *y=h; if (comp) *comp=c;
    return out;
}

float *stbi_loadf_from_memory(const unsigned char *buffer, int len, int *x, int *y, int *comp, int req_comp){
    (void)req_comp;
    int w=0,h=0,c=0;
    unsigned char* d = stbi_load_from_memory(buffer, len, &w,&h,&c,0);
    if (!d) return NULL;
    size_t n = (size_t)w*h*c;
    float* out = (float*)malloc(n*sizeof(float));
    if (!out){ stbi_image_free(d); return NULL; }
    for (size_t i=0;i<n;i++) out[i] = d[i]/255.0f;
    stbi_image_free(d);
    if (x) *x=w; if (y) *y=h; if (comp) *comp=c;
    return out;
}

#else

// Non-Windows stub
unsigned char *stbi_load(char const *filename, int *x, int *y, int *comp, int req_comp){
    (void)filename;(void)x;(void)y;(void)comp;(void)req_comp; stbi_set_error("stb_image stubbed"); return NULL;
}
unsigned char *stbi_load_from_memory(const unsigned char *buffer, int len, int *x, int *y, int *comp, int req_comp){
    (void)buffer;(void)len;(void)x;(void)y;(void)comp;(void)req_comp; stbi_set_error("stb_image stubbed"); return NULL;
}
int stbi_info(char const *filename, int *x, int *y, int *comp){ (void)filename;(void)x;(void)y;(void)comp; return 0; }
int stbi_info_from_memory(const unsigned char *buffer, int len, int *x, int *y, int *comp){ (void)buffer;(void)len;(void)x;(void)y;(void)comp; return 0; }
float *stbi_loadf(char const *filename, int *x, int *y, int *comp, int req_comp){ (void)filename;(void)x;(void)y;(void)comp;(void)req_comp; return NULL; }
float *stbi_loadf_from_memory(const unsigned char *buffer, int len, int *x, int *y, int *comp, int req_comp){ (void)buffer;(void)len;(void)x;(void)y;(void)comp;(void)req_comp; return NULL; }

#endif

void stbi_image_free(void *retval_from_stbi_load){
    free(retval_from_stbi_load);
}
