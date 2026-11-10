/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
// src/mod_d3d11.c
// Direct3D11 bindings: create window + device/swapchain, clear/present, basic input.

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "kirton/native.h"
#include "native_internal.h"
#include "kirton/value.h"
#include "../third_party/stb_image.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdio.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#ifndef ARRAYSIZE
#define ARRAYSIZE(a) (sizeof(a)/sizeof((a)[0]))
#endif
#ifdef _MSC_VER
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#endif

static HWND   g_hwnd = NULL;
static int    g_w = 0, g_h = 0;
static bool   g_ready = false;
static bool   g_vsync = true;
static D3D_FEATURE_LEVEL g_feature_level = 0;

static ID3D11Device*           g_dev  = NULL;
static ID3D11DeviceContext*    g_ctx  = NULL;
static IDXGISwapChain*         g_swap = NULL;
static ID3D11RenderTargetView* g_rtv  = NULL;
static ID3D11Texture2D*        g_ds   = NULL;
static ID3D11DepthStencilView* g_dsv  = NULL;
static ID3D11RasterizerState*  g_rs_ccw = NULL;
static ID3D11RasterizerState*  g_rs_cull_back = NULL;
static ID3D11DepthStencilState* g_dss_enabled = NULL;
static ID3D11DepthStencilState* g_dss_disabled = NULL;
static ID3D11BlendState*       g_bs_alpha = NULL;
static bool   g_depth_enabled = true;
static bool   g_cull_enabled = false;
static bool   g_alpha_enabled = false;

static int g_mouse_x = 0, g_mouse_y = 0;
static int g_wheel = 0;
static int g_last_key = -1;
static uint8_t g_key_down[256] = {0};
static uint8_t g_key_prev[256] = {0};

typedef struct {
    ID3D11VertexShader* vs;
    ID3D11PixelShader*  ps;
    ID3D11InputLayout*  layout;
    ID3DBlob*           vs_blob;
} D3DShaderPair;

typedef struct {
    ID3D11Buffer* buf;
    UINT stride;
    UINT count;
} D3DVertexBuffer;

typedef struct {
    ID3D11ShaderResourceView* srv;
    ID3D11Texture2D* tex;
    ID3D11SamplerState* sampler;
    UINT w,h;
} D3DTexture;

typedef struct {
    ID3D11Buffer* buf;
    UINT count;
    DXGI_FORMAT fmt;
} D3DIndexBuffer;

typedef struct {
    ID3D11Buffer* buf;
    UINT size;
} D3DConstantBuffer;

#define D3D_MAX_SHADERS 128
#define D3D_MAX_VB      256
#define D3D_MAX_TEX     256
#define D3D_MAX_IB      256
#define D3D_MAX_CB      128
static D3DShaderPair g_d3d_shaders[D3D_MAX_SHADERS];
static D3DVertexBuffer g_d3d_vb[D3D_MAX_VB];
static D3DTexture g_d3d_tex[D3D_MAX_TEX];
static D3DIndexBuffer g_d3d_ib[D3D_MAX_IB];
static D3DConstantBuffer g_d3d_cb[D3D_MAX_CB];
static int g_d3d_tex_bound = 0;
static int g_cb_mvp = 0;

typedef HRESULT (WINAPI *PFN_D3DCOMPILE)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
static PFN_D3DCOMPILE p_D3DCompile = NULL;

static int d3d_alloc_shader(void){ for (int i=0;i<D3D_MAX_SHADERS;i++) if (!g_d3d_shaders[i].vs && !g_d3d_shaders[i].ps) return i+1; return 0; }
static int d3d_alloc_vb(void){ for (int i=0;i<D3D_MAX_VB;i++) if (!g_d3d_vb[i].buf) return i+1; return 0; }
static int d3d_alloc_tex(void){ for (int i=0;i<D3D_MAX_TEX;i++) if (!g_d3d_tex[i].srv) return i+1; return 0; }
static int d3d_alloc_ib(void){ for (int i=0;i<D3D_MAX_IB;i++) if (!g_d3d_ib[i].buf) return i+1; return 0; }
static int d3d_alloc_cb(void){ for (int i=0;i<D3D_MAX_CB;i++) if (!g_d3d_cb[i].buf) return i+1; return 0; }

#define SAFE_RELEASE(x) do{ if (x){ (x)->lpVtbl->Release(x); (x)=NULL; } }while(0)

static bool d3d_resize(int w, int h);

static const char* d3d_feature_level_str(D3D_FEATURE_LEVEL fl){
    switch (fl){
    case D3D_FEATURE_LEVEL_11_1: return "11.1";
    case D3D_FEATURE_LEVEL_11_0: return "11.0";
    case D3D_FEATURE_LEVEL_10_1: return "10.1";
    case D3D_FEATURE_LEVEL_10_0: return "10.0";
    default: return "unknown";
    }
}

typedef struct {
    const char* vs;
    const char* ps;
} D3DShaderProfiles;

static D3DShaderProfiles d3d_choose_profiles(void){
    if (g_feature_level == D3D_FEATURE_LEVEL_11_1 || g_feature_level == D3D_FEATURE_LEVEL_11_0){
        D3DShaderProfiles p = {"vs_5_0", "ps_5_0"};
        return p;
    }
    if (g_feature_level == D3D_FEATURE_LEVEL_10_1 || g_feature_level == D3D_FEATURE_LEVEL_10_0){
        D3DShaderProfiles p = {"vs_4_0", "ps_4_0"};
        return p;
    }
    D3DShaderProfiles p = {NULL, NULL};
    return p;
}

static void d3d_update_keys(void){
    for (int vk=0; vk<256; ++vk){
        g_key_prev[vk] = g_key_down[vk];
        SHORT s = GetAsyncKeyState(vk);
        g_key_down[vk] = (s & 0x8000) ? 1 : 0;
    }
}

static bool d3d_load_compiler(void){
    if (p_D3DCompile) return true;
    HMODULE mod = LoadLibraryA("d3dcompiler_47.dll");
    if (!mod) mod = LoadLibraryA("d3dcompiler_43.dll");
    if (!mod) return false;
    p_D3DCompile = (PFN_D3DCOMPILE)GetProcAddress(mod, "D3DCompile");
    return p_D3DCompile != NULL;
}

static bool d3d_extract_bytes(const Value* v, uint8_t** out, size_t* n, bool* need_free){
    *out=NULL; *n=0; *need_free=false;
    if (!v) return false;
    if (v->kind == VAL_STR){
        *out = (uint8_t*)v->as.str.s;
        *n   = v->as.str.n;
        return true;
    }
    if (v->kind == VAL_LIST){
        size_t len = v->as.list->len;
        uint8_t* buf = (uint8_t*)malloc(len);
        if (!buf) return false;
        for (size_t i=0;i<len;i++){
            Value* it = &v->as.list->data[i];
            if (it->kind != VAL_NUM){ free(buf); return false; }
            int val, compare;
            if (v_num_compare(*it, v_int(0), &compare) && compare < 0){ buf[i]=0; continue; }
            if (v_num_compare(*it, v_int(255), &compare) && compare > 0){ buf[i]=255; continue; }
            if (!v_num_to_int_trunc(*it, &val)){ free(buf); return false; }
            if (val<0) val=0;
            if (val>255) val=255;
            buf[i]=(uint8_t)val;
        }
        *out=buf; *n=len; *need_free=true; return true;
    }
    return false;
}

static bool d3d_extract_floats(const Value* v, float** out, size_t* n, bool* need_free){
    *out=NULL; *n=0; *need_free=false;
    if (!v) return false;
    if (v->kind == VAL_STR){
        *out = (float*)v->as.str.s;
        *n   = v->as.str.n / sizeof(float);
        return true;
    }
    if (v->kind == VAL_LIST){
        size_t len = v->as.list->len;
        float* buf = (float*)malloc(len*sizeof(float));
        if (!buf) return false;
        for (size_t i=0;i<len;i++){
            Value* it = &v->as.list->data[i];
            if (it->kind != VAL_NUM){
                fprintf(stderr, "d3d_extract_floats: list[%zu] not num (kind=%d)\n", i, it->kind);
                free(buf);
                return false;
            }
            buf[i] = (float)it->as.num;
        }
        *out=buf; *n=len; *need_free=true; return true;
    }
    fprintf(stderr, "d3d_extract_floats: value kind %d not list/str\n", v->kind);
    return false;
}

static bool d3d_extract_bytes_or_floats(const Value* v, uint8_t** out, size_t* n, bool* need_free){
    if (!v) return false;
    if (v->kind == VAL_LIST){
        float* f=NULL; size_t fn=0; bool nf=false;
        if (d3d_extract_floats(v, &f, &fn, &nf)){
            *out = (uint8_t*)f;
            *n   = fn * sizeof(float);
            *need_free = nf;
            return true;
        }
    }
    return d3d_extract_bytes(v, out, n, need_free);
}

static UINT d3d_align16(UINT v){ return (v + 15u) & ~15u; }

static LRESULT CALLBACK d3d_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp){
    switch (msg){
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_ready = false;
        g_hwnd = NULL;
        return 0;
    case WM_MOUSEMOVE:
        g_mouse_x = GET_X_LPARAM(lp);
        g_mouse_y = GET_Y_LPARAM(lp);
        break;
    case WM_MOUSEWHEEL:
        g_wheel += GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        break;
    case WM_KEYDOWN:
        g_last_key = ((int)wp) & 255;
        break;
    case WM_SIZE: {
        int nw = LOWORD(lp);
        int nh = HIWORD(lp);
        if (nw > 0 && nh > 0 && g_ready && g_swap){
            if (!d3d_resize(nw, nh)){
                fprintf(stderr, "WM_SIZE: не удалось пересоздать рендер-таргеты для %dx%d\n", nw, nh);
            }
        }
        break;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static bool d3d_ensure_states(void){
    if (!g_dev) return false;

    if (!g_rs_ccw){
        D3D11_RASTERIZER_DESC rd; ZeroMemory(&rd, sizeof(rd));
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.FrontCounterClockwise = TRUE;
        rd.DepthClipEnable = TRUE;
        ID3D11RasterizerState* rs = NULL;
        if (SUCCEEDED(g_dev->lpVtbl->CreateRasterizerState(g_dev, &rd, &rs))){
            g_rs_ccw = rs;
        }
    }
    if (!g_rs_cull_back){
        D3D11_RASTERIZER_DESC rd; ZeroMemory(&rd, sizeof(rd));
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_BACK;
        rd.FrontCounterClockwise = TRUE;
        rd.DepthClipEnable = TRUE;
        ID3D11RasterizerState* rs = NULL;
        if (SUCCEEDED(g_dev->lpVtbl->CreateRasterizerState(g_dev, &rd, &rs))){
            g_rs_cull_back = rs;
        }
    }
    if (!g_dss_enabled){
        D3D11_DEPTH_STENCIL_DESC dd; ZeroMemory(&dd, sizeof(dd));
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        ID3D11DepthStencilState* ds = NULL;
        if (SUCCEEDED(g_dev->lpVtbl->CreateDepthStencilState(g_dev, &dd, &ds))){
            g_dss_enabled = ds;
        }
    }
    if (!g_dss_disabled){
        D3D11_DEPTH_STENCIL_DESC dd; ZeroMemory(&dd, sizeof(dd));
        dd.DepthEnable = FALSE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
        ID3D11DepthStencilState* ds = NULL;
        if (SUCCEEDED(g_dev->lpVtbl->CreateDepthStencilState(g_dev, &dd, &ds))){
            g_dss_disabled = ds;
        }
    }
    return true;
}

static bool d3d_ensure_blend_states(void){
    if (!g_dev) return false;
    if (!g_bs_alpha){
        D3D11_BLEND_DESC bd; ZeroMemory(&bd, sizeof(bd));
        bd.AlphaToCoverageEnable = FALSE;
        bd.IndependentBlendEnable = FALSE;
        D3D11_RENDER_TARGET_BLEND_DESC rt; ZeroMemory(&rt, sizeof(rt));
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt.DestBlendAlpha = D3D11_BLEND_ZERO;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        bd.RenderTarget[0] = rt;
        ID3D11BlendState* bs = NULL;
        if (SUCCEEDED(g_dev->lpVtbl->CreateBlendState(g_dev, &bd, &bs))){
            g_bs_alpha = bs;
        }
    }
    return true;
}

static void d3d_apply_states(void){
    if (!g_ctx) return;
    ID3D11RasterizerState* rs = g_cull_enabled ? g_rs_cull_back : g_rs_ccw;
    if (rs) g_ctx->lpVtbl->RSSetState(g_ctx, rs);
    ID3D11DepthStencilState* ds = g_depth_enabled ? g_dss_enabled : g_dss_disabled;
    if (ds) g_ctx->lpVtbl->OMSetDepthStencilState(g_ctx, ds, 0);
    ID3D11BlendState* bs = NULL;
    if (g_alpha_enabled){
        d3d_ensure_blend_states();
        bs = g_bs_alpha;
    }
    const FLOAT bf[4] = {0,0,0,0};
    g_ctx->lpVtbl->OMSetBlendState(g_ctx, bs, bf, 0xFFFFFFFFu);
}

static void d3d_destroy_all(void){
    if (g_rs_ccw){ g_rs_ccw->lpVtbl->Release(g_rs_ccw); g_rs_ccw=NULL; }
    if (g_rs_cull_back){ g_rs_cull_back->lpVtbl->Release(g_rs_cull_back); g_rs_cull_back=NULL; }
    if (g_dss_enabled){ g_dss_enabled->lpVtbl->Release(g_dss_enabled); g_dss_enabled=NULL; }
    if (g_dss_disabled){ g_dss_disabled->lpVtbl->Release(g_dss_disabled); g_dss_disabled=NULL; }
    if (g_bs_alpha){ g_bs_alpha->lpVtbl->Release(g_bs_alpha); g_bs_alpha=NULL; }
    for (int i=0;i<D3D_MAX_SHADERS;i++){
        if (g_d3d_shaders[i].layout){ g_d3d_shaders[i].layout->lpVtbl->Release(g_d3d_shaders[i].layout); }
        if (g_d3d_shaders[i].vs){ g_d3d_shaders[i].vs->lpVtbl->Release(g_d3d_shaders[i].vs); }
        if (g_d3d_shaders[i].ps){ g_d3d_shaders[i].ps->lpVtbl->Release(g_d3d_shaders[i].ps); }
        if (g_d3d_shaders[i].vs_blob){ g_d3d_shaders[i].vs_blob->lpVtbl->Release(g_d3d_shaders[i].vs_blob); }
        g_d3d_shaders[i].vs=NULL;
        g_d3d_shaders[i].ps=NULL;
        g_d3d_shaders[i].layout=NULL;
        g_d3d_shaders[i].vs_blob=NULL;
    }
    for (int i=0;i<D3D_MAX_VB;i++){
        if (g_d3d_vb[i].buf){ g_d3d_vb[i].buf->lpVtbl->Release(g_d3d_vb[i].buf); g_d3d_vb[i].buf=NULL; }
    }
    for (int i=0;i<D3D_MAX_IB;i++){
        if (g_d3d_ib[i].buf){ g_d3d_ib[i].buf->lpVtbl->Release(g_d3d_ib[i].buf); g_d3d_ib[i].buf=NULL; }
        g_d3d_ib[i].count = 0; g_d3d_ib[i].fmt = DXGI_FORMAT_UNKNOWN;
    }
    for (int i=0;i<D3D_MAX_CB;i++){
        if (g_d3d_cb[i].buf){ g_d3d_cb[i].buf->lpVtbl->Release(g_d3d_cb[i].buf); g_d3d_cb[i].buf=NULL; }
        g_d3d_cb[i].size = 0;
    }
    for (int i=0;i<D3D_MAX_TEX;i++){
        if (g_d3d_tex[i].srv){ g_d3d_tex[i].srv->lpVtbl->Release(g_d3d_tex[i].srv); g_d3d_tex[i].srv=NULL; }
        if (g_d3d_tex[i].tex){ g_d3d_tex[i].tex->lpVtbl->Release(g_d3d_tex[i].tex); g_d3d_tex[i].tex=NULL; }
        if (g_d3d_tex[i].sampler){ g_d3d_tex[i].sampler->lpVtbl->Release(g_d3d_tex[i].sampler); g_d3d_tex[i].sampler=NULL; }
    }
    SAFE_RELEASE(g_dsv);
    SAFE_RELEASE(g_ds);
    SAFE_RELEASE(g_rtv);
    SAFE_RELEASE(g_swap);
    SAFE_RELEASE(g_ctx);
    SAFE_RELEASE(g_dev);
    if (g_hwnd){ DestroyWindow(g_hwnd); g_hwnd = NULL; }
    g_ready = false;
    g_w = g_h = 0;
    g_cull_enabled = false;
    g_depth_enabled = true;
    g_alpha_enabled = false;
    g_d3d_tex_bound = 0;
    g_cb_mvp = 0;
    g_feature_level = 0;
}

static bool d3d_create_views(int w, int h){
    SAFE_RELEASE(g_rtv);
    SAFE_RELEASE(g_dsv);
    SAFE_RELEASE(g_ds);

    ID3D11Texture2D* backbuf = NULL;
    if (FAILED(g_swap->lpVtbl->GetBuffer(g_swap, 0, &IID_ID3D11Texture2D, (void**)&backbuf))){
        return false;
    }
    HRESULT hr = g_dev->lpVtbl->CreateRenderTargetView(g_dev, (ID3D11Resource*)backbuf, NULL, &g_rtv);
    backbuf->lpVtbl->Release(backbuf);
    if (FAILED(hr)) return false;

    D3D11_TEXTURE2D_DESC ds_desc;
    ZeroMemory(&ds_desc, sizeof(ds_desc));
    ds_desc.Width = (UINT)w;
    ds_desc.Height= (UINT)h;
    ds_desc.MipLevels = 1;
    ds_desc.ArraySize = 1;
    ds_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    ds_desc.SampleDesc.Count = 1;
    ds_desc.Usage = D3D11_USAGE_DEFAULT;
    ds_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    hr = g_dev->lpVtbl->CreateTexture2D(g_dev, &ds_desc, NULL, &g_ds);
    if (FAILED(hr)) return false;
    hr = g_dev->lpVtbl->CreateDepthStencilView(g_dev, (ID3D11Resource*)g_ds, NULL, &g_dsv);
    if (FAILED(hr)) return false;

    g_ctx->lpVtbl->OMSetRenderTargets(g_ctx, 1, &g_rtv, g_dsv);

    D3D11_VIEWPORT vp;
    vp.TopLeftX = 0.0f; vp.TopLeftY = 0.0f;
    vp.Width = (float)w; vp.Height = (float)h;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    g_ctx->lpVtbl->RSSetViewports(g_ctx, 1, &vp);

    d3d_ensure_states();
    d3d_apply_states();
    return true;
}

static bool d3d_resize(int w, int h){
    if (!g_swap) return false;
    if (w <= 0 || h <= 0) return false;
    SAFE_RELEASE(g_dsv);
    SAFE_RELEASE(g_ds);
    SAFE_RELEASE(g_rtv);
    HRESULT hr = g_swap->lpVtbl->ResizeBuffers(g_swap, 0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)){
        fprintf(stderr, "d3d_resize: ResizeBuffers для %dx%d вернул hr=0x%08lx\n", w, h, (unsigned long)hr);
        return false;
    }
    bool ok = d3d_create_views(w, h);
    if (ok){
        g_w = w;
        g_h = h;
    } else {
        fprintf(stderr, "d3d_resize: d3d_create_views не удалось для %dx%d\n", w, h);
    }
    return ok;
}

static bool d3d_create_window_and_device(int w, int h, const char* title, int title_n){
    if (w <= 0 || h <= 0) return false;
    d3d_destroy_all();

    WNDCLASSW wc; ZeroMemory(&wc, sizeof(wc));
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = d3d_wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"KirtonD3D11Window";
    RegisterClassW(&wc);

    wchar_t* wtitle = NULL;
    if (title && title_n >= 0){
        int len = MultiByteToWideChar(CP_UTF8, 0, title, title_n, NULL, 0);
        if (len > 0){
            wtitle = (wchar_t*)malloc(((size_t)len+1)*sizeof(wchar_t));
            if (wtitle){
                MultiByteToWideChar(CP_UTF8, 0, title, title_n, wtitle, len);
                wtitle[len] = 0;
            }
        }
    }
    if (!wtitle) wtitle = _wcsdup(L"Kirton D3D11");

    RECT rc = {0, 0, w, h};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowW(L"KirtonD3D11Window", wtitle, WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           rc.right - rc.left, rc.bottom - rc.top,
                           NULL, NULL, wc.hInstance, NULL);
    free(wtitle);
    if (!g_hwnd) return false;

    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = (UINT)w;
    sd.BufferDesc.Height= (UINT)h;
    sd.BufferDesc.Format= DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = g_hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got = 0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        &sd, &g_swap, &g_dev, &got, &g_ctx);
    if (FAILED(hr)){
        hr = D3D11CreateDeviceAndSwapChain(
            NULL, D3D_DRIVER_TYPE_WARP, NULL, flags,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
            &sd, &g_swap, &g_dev, &got, &g_ctx);
        if (FAILED(hr)){ d3d_destroy_all(); return false; }
    }
    g_feature_level = got;

    g_w = w; g_h = h;
    if (!d3d_create_views(w, h)){ d3d_destroy_all(); return false; }

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
    g_ready = true;
    return true;
}

static bool d3d_pump(void){
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)){
        if (msg.message == WM_QUIT){ g_ready = false; return false; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    d3d_update_keys();
    return g_ready && g_hwnd != NULL;
}

/* ------- Kirton bindings ------- */

static Value k_d3d_create(int n, const Value* a){
    if (n < 2) return v_bool(false);
    if (a[0].kind != VAL_NUM || a[1].kind != VAL_NUM) return v_bool(false);
    int width, height;
    if (!v_num_to_int_trunc(a[0], &width) || !v_num_to_int_trunc(a[1], &height)) return v_bool(false);
    const char* title = (n >= 3 && a[2].kind == VAL_STR) ? a[2].as.str.s : "Kirton D3D11";
    int title_n = (n >= 3 && a[2].kind == VAL_STR) ? (int)a[2].as.str.n : -1;
    bool ok = d3d_create_window_and_device(width, height, title, title_n);
    return v_bool(ok);
}

static Value k_d3d_destroy(int n, const Value* a){
    (void)n;(void)a;
    d3d_destroy_all();
    return v_bool(true);
}

static Value k_d3d_poll(int n, const Value* a){
    (void)n;(void)a;
    bool ok = d3d_pump();
    if (!ok) d3d_destroy_all();
    return v_bool(ok);
}

static Value k_d3d_present(int n, const Value* a){
    bool vs = g_vsync;
    if (n > 0 && a[0].kind == VAL_BOOL) vs = a[0].as.boolean;
    else if (n > 0 && a[0].kind == VAL_NUM) vs = (a[0].as.num != 0.0);
    g_vsync = vs;
    if (!g_ready || !g_swap) return v_bool(false);
    g_swap->lpVtbl->Present(g_swap, vs ? 1 : 0, 0);
    return v_bool(true);
}

static Value k_d3d_clear(int n, const Value* a){
    if (!g_ready || !g_ctx || !g_rtv) return v_bool(false);
    float r=0, g=0, b=0, al=1;
    if (n > 0 && a[0].kind == VAL_NUM) r = (float)a[0].as.num;
    if (n > 1 && a[1].kind == VAL_NUM) g = (float)a[1].as.num;
    if (n > 2 && a[2].kind == VAL_NUM) b = (float)a[2].as.num;
    if (n > 3 && a[3].kind == VAL_NUM) al = (float)a[3].as.num;
    float c[4] = {r, g, b, al};
    g_ctx->lpVtbl->ClearRenderTargetView(g_ctx, g_rtv, c);
    if (g_dsv) g_ctx->lpVtbl->ClearDepthStencilView(g_ctx, g_dsv, D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL, 1.0f, 0);
    return v_bool(true);
}

static Value k_d3d_viewport(int n, const Value* a){
    if (!g_ready || !g_ctx) return v_bool(false);
    if (n < 2 || a[0].kind != VAL_NUM || a[1].kind != VAL_NUM) return v_bool(false);
    int w, h;
    if (!v_num_to_int_trunc(a[0], &w) || !v_num_to_int_trunc(a[1], &h)) return v_bool(false);
    if (w <= 0 || h <= 0) return v_bool(false);
    D3D11_VIEWPORT vp;
    vp.TopLeftX = 0.0f; vp.TopLeftY = 0.0f;
    vp.Width = (float)w; vp.Height = (float)h;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    g_ctx->lpVtbl->RSSetViewports(g_ctx, 1, &vp);
    g_w = w; g_h = h;
    return v_bool(true);
}

static Value k_d3d_resize(int n, const Value* a){
    if (!g_ready || !g_swap) return v_bool(false);
    if (n < 2 || a[0].kind != VAL_NUM || a[1].kind != VAL_NUM) return v_bool(false);
    int w, h;
    if (!v_num_to_int_trunc(a[0], &w) || !v_num_to_int_trunc(a[1], &h)) return v_bool(false);
    if (w <= 0 || h <= 0) return v_bool(false);
    bool ok = d3d_resize(w, h);
    if (ok){ g_w = w; g_h = h; }
    return v_bool(ok);
}

static Value k_d3d_window_size(int n, const Value* a){
    (void)n;(void)a;
    Value L = v_list_new();
    v_list_push(&L, v_int(g_w));
    v_list_push(&L, v_int(g_h));
    return L;
}

/* ===================== Modern pipeline ===================== */

static char* d3d_sanitize_shader(const char* src, int len, int* out_len){
    if (!src || len <= 0) return NULL;
    const unsigned char* us = (const unsigned char*)src;
    int i = 0;
    if (len >= 3 && us[0]==0xEF && us[1]==0xBB && us[2]==0xBF){
        i = 3;
    }
    char* out = (char*)malloc((size_t)len + 1);
    if (!out) return NULL;
    int j = 0;
    for (; i < len; ++i){
        if (src[i] == '\\' && i+1 < len){
            char n = src[i+1];
            if (n == 'n'){ out[j++] = '\n'; i++; continue; }
            if (n == 'r'){ out[j++] = '\r'; i++; continue; }
            if (n == 't'){ out[j++] = '\t'; i++; continue; }
        }
        out[j++] = src[i];
    }
    out[j] = 0;
    if (out_len) *out_len = j;
    return out;
}

static Value k_d3d_create_shader_pair(int n, const Value* a){
    if (!g_ready || !g_dev || !g_ctx) return v_num(0);
    if (n < 2 || a[0].kind != VAL_STR || a[1].kind != VAL_STR) return v_num(0);
    if (!d3d_load_compiler()) return v_num(0);

    D3DShaderProfiles prof = d3d_choose_profiles();
    if (!prof.vs || !prof.ps){
        fprintf(stderr, "create_shader_pair: неподдерживаемый feature level %s\n", d3d_feature_level_str(g_feature_level));
        return v_num(0);
    }

    int vs_len = (int)a[0].as.str.n, ps_len = (int)a[1].as.str.n;
    char* vs_src = d3d_sanitize_shader(a[0].as.str.s, vs_len, &vs_len);
    char* ps_src = d3d_sanitize_shader(a[1].as.str.s, ps_len, &ps_len);

    ID3DBlob *vsb=NULL, *psb=NULL, *err=NULL;
    HRESULT hr = p_D3DCompile(vs_src ? vs_src : a[0].as.str.s, vs_len, NULL, NULL, NULL, "main", prof.vs, 0, 0, &vsb, &err);
    if (FAILED(hr)){
        fprintf(stderr, "D3DCompile(VS) не прошёл (FL %s, профиль %s), hr=0x%08lx\n",
                d3d_feature_level_str(g_feature_level), prof.vs, (unsigned long)hr);
        if (err){ fwrite(err->lpVtbl->GetBufferPointer(err), 1, err->lpVtbl->GetBufferSize(err), stderr); err->lpVtbl->Release(err); err=NULL; }
        if (vsb) vsb->lpVtbl->Release(vsb);
        if (vs_src) free(vs_src);
        if (ps_src) free(ps_src);
        return v_num(0);
    }
    if (err){ err->lpVtbl->Release(err); err=NULL; }
    hr = p_D3DCompile(ps_src ? ps_src : a[1].as.str.s, ps_len, NULL, NULL, NULL, "main", prof.ps, 0, 0, &psb, &err);
    if (FAILED(hr)){
        fprintf(stderr, "D3DCompile(PS) не прошёл (FL %s, профиль %s), hr=0x%08lx\n",
                d3d_feature_level_str(g_feature_level), prof.ps, (unsigned long)hr);
        if (err){ fwrite(err->lpVtbl->GetBufferPointer(err), 1, err->lpVtbl->GetBufferSize(err), stderr); err->lpVtbl->Release(err); err=NULL; }
        vsb->lpVtbl->Release(vsb);
        if (psb) psb->lpVtbl->Release(psb);
        if (vs_src) free(vs_src);
        if (ps_src) free(ps_src);
        return v_num(0);
    }
    if (err){ err->lpVtbl->Release(err); err=NULL; }
    if (vs_src) free(vs_src);
    if (ps_src) free(ps_src);

    ID3D11VertexShader* vs=NULL;
    ID3D11PixelShader*  ps=NULL;
    hr = g_dev->lpVtbl->CreateVertexShader(g_dev, vsb->lpVtbl->GetBufferPointer(vsb), vsb->lpVtbl->GetBufferSize(vsb), NULL, &vs);
    if (FAILED(hr)){
        fprintf(stderr, "CreateVertexShader не прошёл (FL %s, профиль %s), hr=0x%08lx\n",
                d3d_feature_level_str(g_feature_level), prof.vs, (unsigned long)hr);
        vsb->lpVtbl->Release(vsb); psb->lpVtbl->Release(psb); return v_num(0);
    }
    hr = g_dev->lpVtbl->CreatePixelShader(g_dev, psb->lpVtbl->GetBufferPointer(psb), psb->lpVtbl->GetBufferSize(psb), NULL, &ps);
    if (FAILED(hr)){
        fprintf(stderr, "CreatePixelShader не прошёл (FL %s, профиль %s), hr=0x%08lx\n",
                d3d_feature_level_str(g_feature_level), prof.ps, (unsigned long)hr);
        vs->lpVtbl->Release(vs); vsb->lpVtbl->Release(vsb); psb->lpVtbl->Release(psb); return v_num(0);
    }

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION",0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,   D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR",   0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD",0, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    ID3D11InputLayout* il = NULL;
    hr = g_dev->lpVtbl->CreateInputLayout(g_dev, layout, ARRAYSIZE(layout),
                                          vsb->lpVtbl->GetBufferPointer(vsb),
                                          vsb->lpVtbl->GetBufferSize(vsb),
                                          &il);
    if (FAILED(hr)){ vs->lpVtbl->Release(vs); ps->lpVtbl->Release(ps); vsb->lpVtbl->Release(vsb); psb->lpVtbl->Release(psb); return v_num(0); }

    int h = d3d_alloc_shader();
    if (!h){ il->lpVtbl->Release(il); vs->lpVtbl->Release(vs); ps->lpVtbl->Release(ps); vsb->lpVtbl->Release(vsb); psb->lpVtbl->Release(psb); return v_num(0); }
    g_d3d_shaders[h-1].vs = vs;
    g_d3d_shaders[h-1].ps = ps;
    g_d3d_shaders[h-1].layout = il;
    g_d3d_shaders[h-1].vs_blob = vsb;
    psb->lpVtbl->Release(psb); // not needed after PS creation
    return v_int(h);
}

static Value k_d3d_create_vb_fn(int n, const Value* a){
    if (!g_ready || !g_dev || !g_ctx){
        fprintf(stderr, "create_vertex_buffer: device not ready\n");
        return v_num(0);
    }
    if (n < 3 || a[1].kind != VAL_NUM || a[2].kind != VAL_NUM){
        fprintf(stderr, "create_vertex_buffer: bad args\n");
        return v_num(0);
    }
    float* fdata=NULL; size_t fcount=0; bool need_free=false;
    if (!d3d_extract_floats(&a[0], &fdata, &fcount, &need_free)){
        fprintf(stderr, "create_vertex_buffer: extract_floats failed (arg0 kind=%d)\n", a[0].kind);
        return v_num(0);
    }
    int stride_value, count_value;
    if (!v_num_to_int_trunc(a[1], &stride_value) || !v_num_to_int_trunc(a[2], &count_value) ||
        stride_value <= 0 || count_value <= 0){
        if (need_free) free(fdata);
        return v_num(0);
    }
    UINT stride = (UINT)stride_value;
    UINT count  = (UINT)count_value;
    size_t need = (size_t)stride * (size_t)count;
    if (need == 0 || fcount * sizeof(float) < need){
        if (need_free) free(fdata);
        fprintf(stderr, "create_vertex_buffer: data too small or zero (stride=%u count=%u fbytes=%zu need=%zu)\n",
                stride, count, fcount*sizeof(float), need);
        return v_num(0);
    }

    D3D11_BUFFER_DESC bd; ZeroMemory(&bd, sizeof(bd));
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.ByteWidth = (UINT)need;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sd; ZeroMemory(&sd, sizeof(sd));
    sd.pSysMem = fdata;
    ID3D11Buffer* buf = NULL;
    HRESULT hr = g_dev->lpVtbl->CreateBuffer(g_dev, &bd, &sd, &buf);
    if (need_free) free(fdata);
    if (FAILED(hr)){
        fprintf(stderr, "CreateBuffer VB failed hr=0x%08lx (stride=%u count=%u need=%zu)\n",
                (unsigned long)hr, stride, count, need);
        return v_num(0);
    }
    int h = d3d_alloc_vb();
    if (!h){ buf->lpVtbl->Release(buf); return v_num(0); }
    g_d3d_vb[h-1].buf=buf; g_d3d_vb[h-1].stride=stride; g_d3d_vb[h-1].count=count;
    return v_int(h);
}

static Value k_d3d_update_vb_fn(int n, const Value* a){
    if (!g_ready || !g_dev || !g_ctx) { fprintf(stderr,"update_vb: not ready\n"); return v_bool(false); }
    if (n < 2 || a[0].kind != VAL_NUM) { fprintf(stderr,"update_vb: bad args\n"); return v_bool(false); }
    int hvb;
    if (!v_num_to_int_trunc(a[0], &hvb)) return v_bool(false);
    if (hvb<=0 || hvb>D3D_MAX_VB){ fprintf(stderr,"update_vb: handle out of range\n"); return v_bool(false); }
    D3DVertexBuffer* vb = &g_d3d_vb[hvb-1];
    if (!vb->buf || vb->stride==0 || vb->count==0){ fprintf(stderr,"update_vb: vb invalid\n"); return v_bool(false); }

    float* fdata=NULL; size_t fcount=0; bool need_free=false;
    if (!d3d_extract_floats(&a[1], &fdata, &fcount, &need_free)){ fprintf(stderr,"update_vb: extract failed\n"); return v_bool(false); }
    size_t need = (size_t)vb->stride * (size_t)vb->count;
    if (fcount * sizeof(float) < need){ if (need_free) free(fdata); fprintf(stderr,"update_vb: data too small (%zu bytes, need %zu)\n", fcount*sizeof(float), need); return v_bool(false); }

    g_ctx->lpVtbl->UpdateSubresource(g_ctx, (ID3D11Resource*)vb->buf, 0, NULL, fdata, 0, 0);
    if (need_free) free(fdata);
    return v_bool(true);
}

static Value k_d3d_create_ib_fn(int n, const Value* a){
    if (!g_ready || !g_dev || !g_ctx){
        fprintf(stderr, "create_index_buffer: device not ready\n");
        return v_num(0);
    }
    if (n < 2 || a[1].kind != VAL_NUM){
        fprintf(stderr, "create_index_buffer: bad args\n");
        return v_num(0);
    }
    uint8_t* raw=NULL; size_t bytes=0; bool need_free=false;
    if (a[0].kind == VAL_LIST){
        size_t len = a[0].as.list->len;
        uint32_t* tmp = (uint32_t*)malloc(len * sizeof(uint32_t));
        if (!tmp) return v_num(0);
        for (size_t i=0;i<len;i++){
            Value* it = &a[0].as.list->data[i];
            if (it->kind != VAL_NUM){ free(tmp); return v_num(0); }
            int value;
            if (!v_num_to_int_trunc(*it, &value) || value < 0){ free(tmp); return v_num(0); }
            tmp[i] = (uint32_t)value;
        }
        raw = (uint8_t*)tmp;
        bytes = len * sizeof(uint32_t);
        need_free = true;
    } else {
        if (!d3d_extract_bytes(&a[0], &raw, &bytes, &need_free)) return v_num(0);
    }
    int count_value;
    if (!v_num_to_int_trunc(a[1], &count_value) || count_value <= 0){ if (need_free) free(raw); return v_num(0); }
    UINT count = (UINT)count_value;
    size_t needed = (size_t)count * sizeof(uint32_t);
    if (count == 0 || bytes < needed){
        if (need_free) free(raw);
        fprintf(stderr, "create_index_buffer: data too small or zero (count=%u bytes=%zu need=%zu)\n",
                count, bytes, needed);
        return v_num(0);
    }

    D3D11_BUFFER_DESC bd; ZeroMemory(&bd, sizeof(bd));
    bd.ByteWidth = (UINT)needed;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    bd.StructureByteStride = sizeof(uint32_t);
    D3D11_SUBRESOURCE_DATA sd; ZeroMemory(&sd, sizeof(sd));
    sd.pSysMem = raw;
    ID3D11Buffer* buf=NULL;
    HRESULT hr = g_dev->lpVtbl->CreateBuffer(g_dev, &bd, &sd, &buf);
    if (need_free) free(raw);
    if (FAILED(hr)){
        fprintf(stderr, "CreateBuffer IB failed hr=0x%08lx (count=%u need=%zu)\n",
                (unsigned long)hr, count, needed);
        return v_num(0);
    }
    int h = d3d_alloc_ib();
    if (!h){ buf->lpVtbl->Release(buf); return v_num(0); }
    g_d3d_ib[h-1].buf = buf;
    g_d3d_ib[h-1].count = count;
    g_d3d_ib[h-1].fmt = DXGI_FORMAT_R32_UINT;
    return v_int(h);
}

static Value k_d3d_update_ib_fn(int n, const Value* a){
    if (!g_ready || !g_dev || !g_ctx) return v_bool(false);
    if (n < 2 || a[0].kind != VAL_NUM) return v_bool(false);
    int hib;
    if (!v_num_to_int_trunc(a[0], &hib)) return v_bool(false);
    if (hib<=0 || hib>D3D_MAX_IB) return v_bool(false);
    D3DIndexBuffer* ib = &g_d3d_ib[hib-1];
    if (!ib->buf || ib->count==0) return v_bool(false);

    uint8_t* raw=NULL; size_t bytes=0; bool need_free=false;
    if (a[1].kind == VAL_LIST){
        size_t len = a[1].as.list->len;
        uint32_t* tmp = (uint32_t*)malloc(len * sizeof(uint32_t));
        if (!tmp) return v_bool(false);
        for (size_t i=0;i<len;i++){
            Value* it = &a[1].as.list->data[i];
            if (it->kind != VAL_NUM){ free(tmp); return v_bool(false); }
            int value;
            if (!v_num_to_int_trunc(*it, &value) || value < 0){ free(tmp); return v_bool(false); }
            tmp[i] = (uint32_t)value;
        }
        raw = (uint8_t*)tmp;
        bytes = len * sizeof(uint32_t);
        need_free = true;
    } else {
        if (!d3d_extract_bytes(&a[1], &raw, &bytes, &need_free)) return v_bool(false);
    }
    size_t needed = (size_t)ib->count * ((ib->fmt==DXGI_FORMAT_R16_UINT)?2:4);
    if (bytes < needed){ if (need_free) free(raw); return v_bool(false); }

    g_ctx->lpVtbl->UpdateSubresource(g_ctx, (ID3D11Resource*)ib->buf, 0, NULL, raw, 0, 0);
    if (need_free) free(raw);
    return v_bool(true);
}

static Value k_d3d_draw(int n, const Value* a){
    if (!g_ready || !g_ctx || !g_dev || !g_rtv) return v_bool(false);
    if (n < 2 || a[0].kind != VAL_NUM || a[1].kind != VAL_NUM) return v_bool(false);
    int hvb, hsh;
    if (!v_num_to_int_trunc(a[0], &hvb) || !v_num_to_int_trunc(a[1], &hsh)) return v_bool(false);
    if (hvb<=0||hvb>D3D_MAX_VB||hsh<=0||hsh>D3D_MAX_SHADERS) return v_bool(false);
    D3DVertexBuffer* vb = &g_d3d_vb[hvb-1];
    D3DShaderPair* sh = &g_d3d_shaders[hsh-1];
    if (!vb->buf || !sh->vs || !sh->ps || !sh->layout) return v_bool(false);

    D3D_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    if (n >= 3 && a[2].kind == VAL_NUM){
        int m;
        if (!v_num_to_int_trunc(a[2], &m)) return v_bool(false);
        if (m == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP) topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        else topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }

    UINT stride = vb->stride, offset=0;
    g_ctx->lpVtbl->OMSetRenderTargets(g_ctx, 1, &g_rtv, g_dsv);
    d3d_ensure_states();
    d3d_apply_states();
    g_ctx->lpVtbl->IASetInputLayout(g_ctx, sh->layout);
    g_ctx->lpVtbl->IASetPrimitiveTopology(g_ctx, topo);
    g_ctx->lpVtbl->IASetVertexBuffers(g_ctx, 0, 1, &vb->buf, &stride, &offset);
    g_ctx->lpVtbl->VSSetShader(g_ctx, sh->vs, NULL, 0);
    g_ctx->lpVtbl->PSSetShader(g_ctx, sh->ps, NULL, 0);

    int tex_idx = g_d3d_tex_bound;
    if (tex_idx <= 0){
        // fallback: first available texture
        for (int i=0;i<D3D_MAX_TEX;i++){ if (g_d3d_tex[i].srv){ tex_idx = i+1; break; } }
    }
    if (tex_idx>0 && tex_idx<=D3D_MAX_TEX && g_d3d_tex[tex_idx-1].srv){
        g_ctx->lpVtbl->PSSetShaderResources(g_ctx, 0, 1, &g_d3d_tex[tex_idx-1].srv);
        g_ctx->lpVtbl->PSSetSamplers(g_ctx, 0, 1, &g_d3d_tex[tex_idx-1].sampler);
    } else {
        // Bind a null SRV only once to avoid stale texture bindings
        ID3D11ShaderResourceView* null_srv=NULL;
        ID3D11SamplerState* null_smp=NULL;
        g_ctx->lpVtbl->PSSetShaderResources(g_ctx, 0, 1, &null_srv);
        g_ctx->lpVtbl->PSSetSamplers(g_ctx, 0, 1, &null_smp);
    }

    g_ctx->lpVtbl->Draw(g_ctx, vb->count, 0);
    return v_bool(true);
}

static Value k_d3d_draw_indexed(int n, const Value* a){
    if (!g_ready || !g_ctx || !g_dev || !g_rtv) return v_bool(false);
    if (n < 3 || a[0].kind != VAL_NUM || a[1].kind != VAL_NUM || a[2].kind != VAL_NUM) return v_bool(false);
    int hvb, hib, hsh;
    if (!v_num_to_int_trunc(a[0], &hvb) || !v_num_to_int_trunc(a[1], &hib) || !v_num_to_int_trunc(a[2], &hsh)) return v_bool(false);
    if (hvb<=0||hvb>D3D_MAX_VB||hib<=0||hib>D3D_MAX_IB||hsh<=0||hsh>D3D_MAX_SHADERS) return v_bool(false);
    D3DVertexBuffer* vb = &g_d3d_vb[hvb-1];
    D3DIndexBuffer* ib = &g_d3d_ib[hib-1];
    D3DShaderPair* sh = &g_d3d_shaders[hsh-1];
    if (!vb->buf || !ib->buf || ib->fmt==DXGI_FORMAT_UNKNOWN || !sh->vs || !sh->ps || !sh->layout) return v_bool(false);

    D3D_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    if (n >= 4 && a[3].kind == VAL_NUM){
        int m;
        if (!v_num_to_int_trunc(a[3], &m)) return v_bool(false);
        if (m == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP) topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        else topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }

    UINT stride = vb->stride, offset=0;
    g_ctx->lpVtbl->OMSetRenderTargets(g_ctx, 1, &g_rtv, g_dsv);
    d3d_ensure_states();
    d3d_apply_states();
    g_ctx->lpVtbl->IASetInputLayout(g_ctx, sh->layout);
    g_ctx->lpVtbl->IASetPrimitiveTopology(g_ctx, topo);
    g_ctx->lpVtbl->IASetVertexBuffers(g_ctx, 0, 1, &vb->buf, &stride, &offset);
    g_ctx->lpVtbl->IASetIndexBuffer(g_ctx, ib->buf, ib->fmt, 0);
    g_ctx->lpVtbl->VSSetShader(g_ctx, sh->vs, NULL, 0);
    g_ctx->lpVtbl->PSSetShader(g_ctx, sh->ps, NULL, 0);

    int tex_idx = g_d3d_tex_bound;
    if (tex_idx <= 0){
        for (int i=0;i<D3D_MAX_TEX;i++){ if (g_d3d_tex[i].srv){ tex_idx = i+1; break; } }
    }
    if (tex_idx>0 && tex_idx<=D3D_MAX_TEX && g_d3d_tex[tex_idx-1].srv){
        g_ctx->lpVtbl->PSSetShaderResources(g_ctx, 0, 1, &g_d3d_tex[tex_idx-1].srv);
        g_ctx->lpVtbl->PSSetSamplers(g_ctx, 0, 1, &g_d3d_tex[tex_idx-1].sampler);
    } else {
        ID3D11ShaderResourceView* null_srv=NULL;
        ID3D11SamplerState* null_smp=NULL;
        g_ctx->lpVtbl->PSSetShaderResources(g_ctx, 0, 1, &null_srv);
        g_ctx->lpVtbl->PSSetSamplers(g_ctx, 0, 1, &null_smp);
    }

    g_ctx->lpVtbl->DrawIndexed(g_ctx, ib->count, 0, 0);
    return v_bool(true);
}

static Value k_d3d_load_tex(int n, const Value* a){
    if (!g_ready || !g_dev || !g_ctx) return v_num(0);
    if (n<1 || a[0].kind!=VAL_STR) return v_num(0);
    int w=0,h=0,c=0;
    unsigned char* px = stbi_load(a[0].as.str.s, &w, &h, &c, 4);
    bool from_file = true;
    if (!px){
        fprintf(stderr, "d3d11.load_texture failed: %s\n", stbi_failure_reason());
        // Fallback: tiny 2x2 checker magenta/cyan
        w = h = 2;
        unsigned char fallback[] = {
            255, 0, 255, 255,   0, 255, 255, 255,
            0, 255, 255, 255,   255, 0, 255, 255
        };
        px = fallback;
        from_file = false;
    }

    D3D11_TEXTURE2D_DESC desc; ZeroMemory(&desc,sizeof(desc));
    desc.Width = (UINT)w; desc.Height = (UINT)h;
    desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd; ZeroMemory(&sd,sizeof(sd));
    sd.pSysMem = px;
    sd.SysMemPitch = (UINT)(w*4);
    ID3D11Texture2D* tex=NULL;
    HRESULT hr = g_dev->lpVtbl->CreateTexture2D(g_dev, &desc, &sd, &tex);
    if (from_file) stbi_image_free(px);
    if (FAILED(hr)) return v_num(0);

    ID3D11ShaderResourceView* srv=NULL;
    hr = g_dev->lpVtbl->CreateShaderResourceView(g_dev, (ID3D11Resource*)tex, NULL, &srv);
    if (FAILED(hr)){ tex->lpVtbl->Release(tex); return v_num(0); }

    D3D11_SAMPLER_DESC smp; ZeroMemory(&smp,sizeof(smp));
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    smp.MipLODBias = 0.0f;
    smp.MaxAnisotropy = 1;
    smp.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    smp.BorderColor[0] = smp.BorderColor[1] = smp.BorderColor[2] = smp.BorderColor[3] = 0.0f;
    smp.MinLOD = 0.0f;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11SamplerState* samp=NULL;
    hr = g_dev->lpVtbl->CreateSamplerState(g_dev, &smp, &samp);
    if (FAILED(hr)){
        fprintf(stderr, "CreateSamplerState не прошёл, hr=0x%08lx\n", (unsigned long)hr);
        srv->lpVtbl->Release(srv); tex->lpVtbl->Release(tex); return v_num(0);
    }

    int hdl = d3d_alloc_tex();
    if (!hdl){ samp->lpVtbl->Release(samp); srv->lpVtbl->Release(srv); tex->lpVtbl->Release(tex); return v_num(0); }
    g_d3d_tex[hdl-1].srv=srv; g_d3d_tex[hdl-1].tex=tex; g_d3d_tex[hdl-1].sampler=samp; g_d3d_tex[hdl-1].w=w; g_d3d_tex[hdl-1].h=h;
    return v_int(hdl);
}

static Value k_d3d_bind_tex(int n, const Value* a){
    if (n<1 || a[0].kind!=VAL_NUM) return v_bool(false);
    int h;
    if (!v_num_to_int_trunc(a[0], &h)) return v_bool(false);
    if (h<0 || h>D3D_MAX_TEX) return v_bool(false);
    g_d3d_tex_bound = h;
    return v_bool(true);
}

static Value k_d3d_create_cb(int n, const Value* a){
    if (!g_ready || !g_dev || !g_ctx) return v_num(0);
    if (n < 1) return v_num(0);
    uint8_t* raw=NULL; size_t bytes=0; bool need_free=false;
    UINT requested = 0;

    if (a[0].kind == VAL_NUM){
        int requested_value;
        if (!v_num_to_int_trunc(a[0], &requested_value) || requested_value < 0) return v_num(0);
        requested = (UINT)requested_value;
    } else {
        if (!d3d_extract_bytes_or_floats(&a[0], &raw, &bytes, &need_free)) return v_num(0);
    }
    if (n >= 2 && a[1].kind == VAL_NUM){
        int requested_value;
        if (!v_num_to_int_trunc(a[1], &requested_value) || requested_value < 0){ if (need_free) free(raw); return v_num(0); }
        requested = (UINT)requested_value;
    }

    size_t base = requested ? (size_t)requested : bytes;
    if (base == 0){ if (need_free) free(raw); return v_num(0); }
    UINT aligned = d3d_align16((UINT)base);
    if (aligned == 0) aligned = 16;

    uint8_t* init_data = NULL;
    D3D11_SUBRESOURCE_DATA sd; ZeroMemory(&sd, sizeof(sd));
    D3D11_SUBRESOURCE_DATA* psd = NULL;
    init_data = (uint8_t*)malloc(aligned);
    if (!init_data){ if (need_free) free(raw); return v_num(0); }
    memset(init_data, 0, aligned);
    if (raw && bytes>0){
        size_t to_copy = bytes < aligned ? bytes : aligned;
        memcpy(init_data, raw, to_copy);
    }
    sd.pSysMem = init_data;
    psd = &sd;

    D3D11_BUFFER_DESC bd; ZeroMemory(&bd, sizeof(bd));
    bd.ByteWidth = aligned;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    ID3D11Buffer* buf=NULL;
    HRESULT hr = g_dev->lpVtbl->CreateBuffer(g_dev, &bd, psd, &buf);
    if (need_free) free(raw);
    if (init_data) free(init_data);
    if (FAILED(hr)) return v_num(0);
    int h = d3d_alloc_cb();
    if (!h){ buf->lpVtbl->Release(buf); return v_num(0); }
    g_d3d_cb[h-1].buf = buf;
    g_d3d_cb[h-1].size = aligned;
    return v_int(h);
}

static Value k_d3d_update_cb(int n, const Value* a){
    if (!g_ready || !g_dev || !g_ctx) return v_bool(false);
    if (n < 2 || a[0].kind != VAL_NUM) return v_bool(false);
    int hcb;
    if (!v_num_to_int_trunc(a[0], &hcb)) return v_bool(false);
    if (hcb<=0 || hcb>D3D_MAX_CB) return v_bool(false);
    D3DConstantBuffer* cb = &g_d3d_cb[hcb-1];
    if (!cb->buf || cb->size==0) return v_bool(false);

    uint8_t* raw=NULL; size_t bytes=0; bool need_free=false;
    if (!d3d_extract_bytes_or_floats(&a[1], &raw, &bytes, &need_free)) return v_bool(false);
    if (bytes == 0){ if (need_free) free(raw); return v_bool(false); }

    uint8_t* tmp = (uint8_t*)malloc(cb->size);
    if (!tmp){ if (need_free) free(raw); return v_bool(false); }
    memset(tmp, 0, cb->size);
    size_t to_copy = bytes < cb->size ? bytes : cb->size;
    memcpy(tmp, raw, to_copy);
    g_ctx->lpVtbl->UpdateSubresource(g_ctx, (ID3D11Resource*)cb->buf, 0, NULL, tmp, 0, 0);
    free(tmp);
    if (need_free) free(raw);
    return v_bool(true);
}

static Value k_d3d_bind_cb(int n, const Value* a){
    if (!g_ready || !g_dev || !g_ctx) return v_bool(false);
    if (n < 2 || a[0].kind != VAL_NUM || a[1].kind != VAL_NUM) return v_bool(false);
    int hcb, slot_value;
    if (!v_num_to_int_trunc(a[0], &hcb) || !v_num_to_int_trunc(a[1], &slot_value) || slot_value < 0) return v_bool(false);
    if (hcb<=0 || hcb>D3D_MAX_CB) return v_bool(false);
    UINT slot = (UINT)slot_value;
    int stage = 2; // 0=VS,1=PS,2=both
    if (n >= 3 && a[2].kind == VAL_NUM && !v_num_to_int_trunc(a[2], &stage)) return v_bool(false);
    D3DConstantBuffer* cb = &g_d3d_cb[hcb-1];
    if (!cb->buf) return v_bool(false);
    bool set_vs = (stage == 0 || stage == 2);
    bool set_ps = (stage == 1 || stage == 2);
    if (stage < 0 || stage > 2){ set_vs = true; set_ps = true; }
    if (set_vs) g_ctx->lpVtbl->VSSetConstantBuffers(g_ctx, slot, 1, &cb->buf);
    if (set_ps) g_ctx->lpVtbl->PSSetConstantBuffers(g_ctx, slot, 1, &cb->buf);
    return v_bool(true);
}

static bool d3d_set_mvp_internal(const Value* v){
    if (!g_ready || !g_dev || !g_ctx || !v) return false;
    float* fdata=NULL; size_t fcount=0; bool need_free=false;
    if (!d3d_extract_floats(v, &fdata, &fcount, &need_free)) return false;
    if (fcount < 16){ if (need_free) free(fdata); return false; }

    if (g_cb_mvp <= 0 || g_cb_mvp > D3D_MAX_CB || !g_d3d_cb[g_cb_mvp-1].buf){
        D3D11_BUFFER_DESC bd; ZeroMemory(&bd, sizeof(bd));
        bd.ByteWidth = d3d_align16(16*sizeof(float));
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        D3D11_SUBRESOURCE_DATA sd; ZeroMemory(&sd,sizeof(sd));
        sd.pSysMem = fdata;
        ID3D11Buffer* buf=NULL;
        HRESULT hr = g_dev->lpVtbl->CreateBuffer(g_dev, &bd, &sd, &buf);
        if (FAILED(hr)){ if (need_free) free(fdata); return false; }
        int h = d3d_alloc_cb();
        if (!h){ buf->lpVtbl->Release(buf); if (need_free) free(fdata); return false; }
        g_d3d_cb[h-1].buf = buf;
        g_d3d_cb[h-1].size = bd.ByteWidth;
        g_cb_mvp = h;
    } else {
        D3DConstantBuffer* cb = &g_d3d_cb[g_cb_mvp-1];
        if (cb->size < 16*sizeof(float)){
            if (need_free) free(fdata);
            return false;
        }
        g_ctx->lpVtbl->UpdateSubresource(g_ctx, (ID3D11Resource*)cb->buf, 0, NULL, fdata, 0, 0);
    }
    if (need_free) free(fdata);
    if (g_cb_mvp>0 && g_cb_mvp<=D3D_MAX_CB && g_d3d_cb[g_cb_mvp-1].buf){
        g_ctx->lpVtbl->VSSetConstantBuffers(g_ctx, 0, 1, &g_d3d_cb[g_cb_mvp-1].buf);
    }
    return true;
}

static Value k_d3d_set_mvp(int n, const Value* a){
    if (n < 1) return v_bool(false);
    bool ok = d3d_set_mvp_internal(&a[0]);
    return v_bool(ok);
}

static Value k_d3d_enable_alpha(int n, const Value* a){
    if (!g_ready || !g_dev || !g_ctx) return v_bool(false);
    bool on = true;
    if (n > 0 && a[0].kind == VAL_BOOL) on = a[0].as.boolean;
    else if (n > 0 && a[0].kind == VAL_NUM) on = (a[0].as.num != 0.0);
    g_alpha_enabled = on;
    if (on){
        d3d_ensure_blend_states();
        if (!g_bs_alpha) return v_bool(false);
    }
    d3d_apply_states();
    return v_bool(true);
}

static Value k_d3d_enable_depth(int n, const Value* a){
    if (!g_ready || !g_dev || !g_ctx) return v_bool(false);
    bool on = true;
    if (n > 0 && a[0].kind == VAL_BOOL) on = a[0].as.boolean;
    else if (n > 0 && a[0].kind == VAL_NUM) on = (a[0].as.num != 0.0);
    g_depth_enabled = on;
    d3d_ensure_states();
    d3d_apply_states();
    return v_bool(true);
}

static Value k_d3d_enable_cull(int n, const Value* a){
    if (!g_ready || !g_dev || !g_ctx) return v_bool(false);
    bool on = true;
    if (n > 0 && a[0].kind == VAL_BOOL) on = a[0].as.boolean;
    else if (n > 0 && a[0].kind == VAL_NUM) on = (a[0].as.num != 0.0);
    g_cull_enabled = on;
    d3d_ensure_states();
    d3d_apply_states();
    return v_bool(true);
}

static Value k_d3d_mouse(int n, const Value* a){
    (void)n;(void)a;
    Value L = v_list_new();
    v_list_push(&L, v_int(g_mouse_x));
    v_list_push(&L, v_int(g_mouse_y));
    return L;
}

static Value k_d3d_wheel(int n, const Value* a){
    (void)n;(void)a;
    int d = g_wheel; g_wheel = 0;
    return v_int(d);
}

static Value k_d3d_key_down(int n, const Value* a){
    if (n < 1 || a[0].kind != VAL_NUM) return v_bool(false);
    int vk;
    if (!v_num_to_int_trunc(a[0], &vk)) return v_bool(false);
    vk &= 255;
    return v_bool(g_key_down[vk] != 0);
}

static Value k_d3d_key_pressed(int n, const Value* a){
    if (n < 1 || a[0].kind != VAL_NUM) return v_bool(false);
    int vk;
    if (!v_num_to_int_trunc(a[0], &vk)) return v_bool(false);
    vk &= 255;
    return v_bool(g_key_down[vk] && !g_key_prev[vk]);
}

static Value k_d3d_key_released(int n, const Value* a){
    if (n < 1 || a[0].kind != VAL_NUM) return v_bool(false);
    int vk;
    if (!v_num_to_int_trunc(a[0], &vk)) return v_bool(false);
    vk &= 255;
    return v_bool(!g_key_down[vk] && g_key_prev[vk]);
}

static Value k_d3d_last_key(int n, const Value* a){
    (void)n;(void)a;
    int k = g_last_key; g_last_key = -1;
    return v_int(k);
}

static const KirNativeDef FUNCS[] = {
    {"create_window", k_d3d_create},
    {"destroy",       k_d3d_destroy},
    {"poll",          k_d3d_poll},
    {"present",       k_d3d_present},
    {"clear",         k_d3d_clear},
    {"viewport",      k_d3d_viewport},
    {"resize",        k_d3d_resize},
    {"window_size",   k_d3d_window_size},
    {"create_shader_pair", k_d3d_create_shader_pair},
    {"create_vertex_buffer", k_d3d_create_vb_fn},
    {"update_vertex_buffer", k_d3d_update_vb_fn},
    {"create_index_buffer", k_d3d_create_ib_fn},
    {"update_index_buffer", k_d3d_update_ib_fn},
    {"draw",          k_d3d_draw},
    {"draw_indexed",  k_d3d_draw_indexed},
    {"create_constant_buffer", k_d3d_create_cb},
    {"update_constant_buffer", k_d3d_update_cb},
    {"bind_constant_buffer",   k_d3d_bind_cb},
    {"set_mvp",        k_d3d_set_mvp},
    {"enable_alpha",  k_d3d_enable_alpha},
    {"enable_depth",  k_d3d_enable_depth},
    {"enable_cull",   k_d3d_enable_cull},
    {"load_texture",  k_d3d_load_tex},
    {"bind_texture",  k_d3d_bind_tex},
    {"mouse_pos",     k_d3d_mouse},
    {"mouse_wheel",   k_d3d_wheel},
    {"key_down",      k_d3d_key_down},
    {"key_pressed",   k_d3d_key_pressed},
    {"key_released",  k_d3d_key_released},
    {"last_key",      k_d3d_last_key},
};

void d3d11_init(void){
    static int cleanup_registered=0;
    if (!cleanup_registered){
        atexit(d3d_destroy_all);
        cleanup_registered=1;
    }
    kir_native_register_module("d3d11", FUNCS, sizeof(FUNCS)/sizeof(FUNCS[0]));
    kir_native_mark_bytecode_module_safe("d3d11");
}

#else

void d3d11_init(void){}

#endif /* _WIN32 */
