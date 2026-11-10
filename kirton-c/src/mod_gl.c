/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
// src/mod_gl.c
// Minimal OpenGL 1.1 bindings for Kirton on Windows: create window + context,
// clear/present, basic immediate-mode drawing and transforms, simple input.

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "kirton/native.h"
#include "native_internal.h"
#include "kirton/value.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "../third_party/stb_image.h"
#include <wchar.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <GL/gl.h>
#ifdef _MSC_VER
#pragma comment(lib, "opengl32.lib")
#endif

static HWND  g_hwnd_gl   = NULL;
static HDC   g_hdc_gl    = NULL;
static HGLRC g_glrc      = NULL;  // the single context selected for this window
static bool  g_gl_is_core = false;
static bool  g_gl_in_begin = false;
static int   g_gl_w      = 0;
static int   g_gl_h      = 0;
static bool  g_gl_ready  = false;
static int   g_mouse_x   = 0;
static bool  g_gl_cleanup_ok = true;
static int   g_mouse_y   = 0;
static int   g_wheel     = 0;
static int   g_last_key  = -1;
static uint8_t g_key_down[256] = {0};
static uint8_t g_key_prev[256] = {0};

// WGL extensions (lazy loaded)
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB  0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
#define GL_CONTEXT_PROFILE_MASK 0x9126
#define GL_CONTEXT_CORE_PROFILE_BIT 0x00000001
typedef HGLRC (WINAPI *PFNWGLCREATECONTEXTATTRIBSARBPROC)(HDC, HGLRC, const int*);
static PFNWGLCREATECONTEXTATTRIBSARBPROC p_wglCreateContextAttribsARB = NULL;

// Modern GL function pointers
typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef unsigned int GLuint;
typedef int GLint;
typedef unsigned int GLenum;
typedef unsigned char GLboolean;
#ifndef GL_VERTEX_SHADER
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW 0x88E4
#define GL_FLOAT 0x1406
#define GL_TEXTURE0 0x84C0
#endif
typedef void (*PFNGLGENVERTEXARRAYSPROC)(GLsizei, GLuint*);
typedef void (*PFNGLBINDVERTEXARRAYPROC)(GLuint);
typedef GLuint (*PFNGLCREATESHADERPROC)(GLenum);
typedef void (*PFNGLSHADERSOURCEPROC)(GLuint, GLsizei, const GLchar* const*, const GLint*);
typedef void (*PFNGLCOMPILESHADERPROC)(GLuint);
typedef void (*PFNGLGETSHADERIVPROC)(GLuint, GLenum, GLint*);
typedef void (*PFNGLGETSHADERINFOLOGPROC)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef void (*PFNGLDELETESHADERPROC)(GLuint);
typedef GLuint (*PFNGLCREATEPROGRAMPROC)(void);
typedef void (*PFNGLATTACHSHADERPROC)(GLuint, GLuint);
typedef void (*PFNGLLINKPROGRAMPROC)(GLuint);
typedef void (*PFNGLGETPROGRAMIVPROC)(GLuint, GLenum, GLint*);
typedef void (*PFNGLGETPROGRAMINFOLOGPROC)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef void (*PFNGLUSEPROGRAMPROC)(GLuint);
typedef void (*PFNGLDELETEPROGRAMPROC)(GLuint);
typedef void (*PFNGLGENBUFFERSPROC)(GLsizei, GLuint*);
typedef void (*PFNGLBINDBUFFERPROC)(GLenum, GLuint);
typedef void (*PFNGLBUFFERDATAPROC)(GLenum, GLsizeiptr, const void*, GLenum);
typedef void (*PFNGLDELETEBUFFERSPROC)(GLsizei, const GLuint*);
typedef void (*PFNGLENABLEVERTEXATTRIBARRAYPROC)(GLuint);
typedef void (*PFNGLVERTEXATTRIBPOINTERPROC)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
typedef void (*PFNGLDRAWELEMENTSPROC)(GLenum, GLsizei, GLenum, const void*);
typedef GLint (*PFNGLGETUNIFORMLOCATIONPROC)(GLuint, const GLchar*);
typedef void (*PFNGLUNIFORM1IPROC)(GLint, GLint);
typedef void (*PFNGLUNIFORM1FPROC)(GLint, float);
typedef void (*PFNGLACTIVETEXTUREPROC)(GLenum);
typedef void (*PFNGLGENTEXTURESPROC)(GLsizei, GLuint*);
typedef void (*PFNGLBINDTEXTUREPROC)(GLenum, GLuint);
typedef void (*PFNGLTEXPARAMETERIPROC)(GLenum, GLenum, GLint);
typedef void (*PFNGLTEXIMAGE2DPROC)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
typedef void (*PFNGLDELETETEXTURESPROC)(GLsizei, const GLuint*);
typedef void (*PFNGLDELETEVERTEXARRAYSPROC)(GLsizei, const GLuint*);
typedef void (*PFNGLBINDATTRIBLOCATIONPROC)(GLuint, GLuint, const GLchar*);

static PFNGLGENVERTEXARRAYSPROC      p_glGenVertexArrays = NULL;
static PFNGLBINDVERTEXARRAYPROC      p_glBindVertexArray = NULL;
static PFNGLCREATESHADERPROC         p_glCreateShader = NULL;
static PFNGLSHADERSOURCEPROC         p_glShaderSource = NULL;
static PFNGLCOMPILESHADERPROC        p_glCompileShader = NULL;
static PFNGLGETSHADERIVPROC          p_glGetShaderiv = NULL;
static PFNGLGETSHADERINFOLOGPROC     p_glGetShaderInfoLog = NULL;
static PFNGLDELETESHADERPROC         p_glDeleteShader = NULL;
static PFNGLCREATEPROGRAMPROC        p_glCreateProgram = NULL;
static PFNGLATTACHSHADERPROC         p_glAttachShader = NULL;
static PFNGLLINKPROGRAMPROC          p_glLinkProgram = NULL;
static PFNGLGETPROGRAMIVPROC         p_glGetProgramiv = NULL;
static PFNGLGETPROGRAMINFOLOGPROC    p_glGetProgramInfoLog = NULL;
static PFNGLUSEPROGRAMPROC           p_glUseProgram = NULL;
static PFNGLDELETEPROGRAMPROC        p_glDeleteProgram = NULL;
static PFNGLGENBUFFERSPROC           p_glGenBuffers = NULL;
static PFNGLBINDBUFFERPROC           p_glBindBuffer = NULL;
static PFNGLBUFFERDATAPROC           p_glBufferData = NULL;
static PFNGLDELETEBUFFERSPROC        p_glDeleteBuffers = NULL;
static PFNGLENABLEVERTEXATTRIBARRAYPROC p_glEnableVertexAttribArray = NULL;
static PFNGLVERTEXATTRIBPOINTERPROC  p_glVertexAttribPointer = NULL;
static PFNGLDRAWELEMENTSPROC         p_glDrawElements = NULL;
static PFNGLGETUNIFORMLOCATIONPROC   p_glGetUniformLocation = NULL;
static PFNGLUNIFORM1IPROC            p_glUniform1i = NULL;
static PFNGLUNIFORM1FPROC            p_glUniform1f = NULL;
static PFNGLACTIVETEXTUREPROC        p_glActiveTexture = NULL;
static PFNGLGENTEXTURESPROC          p_glGenTextures = NULL;
static PFNGLBINDTEXTUREPROC          p_glBindTexture = NULL;
static PFNGLTEXPARAMETERIPROC        p_glTexParameteri = NULL;
static PFNGLTEXIMAGE2DPROC           p_glTexImage2D = NULL;
static PFNGLDELETETEXTURESPROC       p_glDeleteTextures = NULL;
static PFNGLDELETEVERTEXARRAYSPROC   p_glDeleteVertexArrays = NULL;
static PFNGLBINDATTRIBLOCATIONPROC   p_glBindAttribLocation = NULL;

static bool g_gl_core_ok = false;
static bool g_gl_glsl_ok = false;

typedef struct { GLuint id; GLint sampler_loc; } GlProgram;
typedef struct { GLuint id; int stride; int count; GLenum target; } GlBuffer;
typedef struct { GLuint id; int w,h; } GlTexture;

#define GL_MAX_PROGS   128
#define GL_MAX_BUFFERS 256
#define GL_MAX_TEX     256
static GlProgram g_gl_progs[GL_MAX_PROGS];
static GlBuffer  g_gl_bufs[GL_MAX_BUFFERS];
static GlTexture g_gl_tex[GL_MAX_TEX];
static int g_gl_tex_bound = 0;
static GLuint g_gl_vao = 0;

static int gl_alloc_program(void){
    for (int i=0;i<GL_MAX_PROGS;i++) if (g_gl_progs[i].id==0) return i+1;
    return 0;
}
static int gl_alloc_buffer(void){
    for (int i=0;i<GL_MAX_BUFFERS;i++) if (g_gl_bufs[i].id==0) return i+1;
    return 0;
}
static int gl_alloc_tex(void){
    for (int i=0;i<GL_MAX_TEX;i++) if (g_gl_tex[i].id==0) return i+1;
    return 0;
}

static void gl_update_keys(void){
    for (int vk=0; vk<256; ++vk){
        g_key_prev[vk] = g_key_down[vk];
        SHORT s = GetAsyncKeyState(vk);
        g_key_down[vk] = (s & 0x8000) ? 1 : 0;
    }
}

static LRESULT CALLBACK gl_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp){
    switch (msg){
    case WM_CLOSE:
        // Defer actual destruction to gl_destroy_all so the WGL context and
        // device context are released while the HWND is still valid.
        g_gl_ready = false;
        return 0;
    case WM_DESTROY:
        g_gl_ready = false;
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
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void gl_destroy_all(void){
    g_gl_cleanup_ok = true;
    if (g_glrc){
        bool current = g_hdc_gl && wglMakeCurrent(g_hdc_gl, g_glrc);
        if (!current){
            fprintf(stderr, "OpenGL: wglMakeCurrent during cleanup failed (error %lu)\n", (unsigned long)GetLastError());
            g_gl_cleanup_ok = false;
        }
        // Dispose GL objects
        if (current && g_gl_core_ok){
            for (int i=0;i<GL_MAX_PROGS;i++){ if (g_gl_progs[i].id && p_glDeleteProgram){ p_glDeleteProgram(g_gl_progs[i].id); g_gl_progs[i].id=0; } }
            for (int i=0;i<GL_MAX_BUFFERS;i++){ if (g_gl_bufs[i].id && p_glDeleteBuffers){ GLuint id=g_gl_bufs[i].id; p_glDeleteBuffers(1, &id); g_gl_bufs[i].id=0; } }
            for (int i=0;i<GL_MAX_TEX;i++){ if (g_gl_tex[i].id && p_glDeleteTextures){ GLuint id=g_gl_tex[i].id; p_glDeleteTextures(1, &id); g_gl_tex[i].id=0; } }
            if (g_gl_vao && p_glDeleteVertexArrays){ GLuint id=g_gl_vao; p_glDeleteVertexArrays(1,&id); g_gl_vao=0; }
        }
        if (wglGetCurrentContext() == g_glrc) wglMakeCurrent(NULL, NULL);
        if (!wglDeleteContext(g_glrc)){
            fprintf(stderr, "OpenGL: wglDeleteContext failed (error %lu)\n", (unsigned long)GetLastError());
            g_gl_cleanup_ok = false;
        }
        g_glrc = NULL;
    }
    if (g_hwnd_gl && g_hdc_gl && IsWindow(g_hwnd_gl) && ReleaseDC(g_hwnd_gl, g_hdc_gl) == 0){
        fprintf(stderr, "OpenGL: ReleaseDC failed (error %lu)\n", (unsigned long)GetLastError());
        g_gl_cleanup_ok = false;
    }
    g_hdc_gl = NULL;
    if (g_hwnd_gl){
        if (IsWindow(g_hwnd_gl) && !DestroyWindow(g_hwnd_gl)){
            fprintf(stderr, "OpenGL: DestroyWindow failed (error %lu)\n", (unsigned long)GetLastError());
            g_gl_cleanup_ok = false;
        }
        g_hwnd_gl = NULL;
    }
    g_gl_ready = false;
    g_gl_w = g_gl_h = 0;
    g_gl_is_core = false;
    g_gl_in_begin = false;
    g_gl_core_ok = false;
    g_gl_glsl_ok = false;
    g_gl_vao = 0;
    g_gl_tex_bound = 0;
    memset(g_gl_progs, 0, sizeof(g_gl_progs));
    memset(g_gl_bufs, 0, sizeof(g_gl_bufs));
    memset(g_gl_tex, 0, sizeof(g_gl_tex));
}

static bool gl_make_current(void){
    if (!g_hwnd_gl || !g_glrc || !g_hdc_gl) return false;
    if (wglGetCurrentContext() != g_glrc){
        if (!wglMakeCurrent(g_hdc_gl, g_glrc)) {
            fprintf(stderr, "OpenGL: wglMakeCurrent failed (error %lu, context=%p, dc=%p)\n",
                    (unsigned long)GetLastError(), (void*)g_glrc, (void*)g_hdc_gl);
            return false;
        }
    }
    if (wglGetCurrentContext() != g_glrc || wglGetCurrentDC() != g_hdc_gl) {
        fprintf(stderr, "OpenGL: current WGL context/DC does not match Kirton window\n");
        return false;
    }
    return true;
}

static bool gl_make_current_core(void){
    if (!g_gl_is_core) {
        fprintf(stderr, "OpenGL: this operation requires a window created with gl.create_window_core\n");
        return false;
    }
    return gl_make_current();
}

static bool gl_make_current_compat(void){
    if (g_gl_is_core) {
        fprintf(stderr, "OpenGL: fixed-function/immediate-mode calls require compatibility profile\n");
        return false;
    }
    return gl_make_current();
}

static GLenum gl_report_error(const char* operation){
    GLenum error = glGetError();
    if (error != GL_NO_ERROR)
        fprintf(stderr, "OpenGL: %s failed with glGetError=0x%04x\n", operation, (unsigned)error);
    return error;
}

static void* gl_load(const char* name){
    void* p = (void*)wglGetProcAddress(name);
    if (p == (void*)1 || p == (void*)2 || p == (void*)3 || p == (void*)-1) p = NULL;
    if (!p){
        HMODULE mod = GetModuleHandleA("opengl32.dll");
        if (mod) p = (void*)GetProcAddress(mod, name);
    }
    return p;
}

static void gl_load_core_functions(void){
    if (g_gl_core_ok) return;
    p_glGenVertexArrays = (PFNGLGENVERTEXARRAYSPROC)gl_load("glGenVertexArrays");
    p_glBindVertexArray = (PFNGLBINDVERTEXARRAYPROC)gl_load("glBindVertexArray");
    p_glCreateShader    = (PFNGLCREATESHADERPROC)gl_load("glCreateShader");
    p_glShaderSource    = (PFNGLSHADERSOURCEPROC)gl_load("glShaderSource");
    p_glCompileShader   = (PFNGLCOMPILESHADERPROC)gl_load("glCompileShader");
    p_glGetShaderiv     = (PFNGLGETSHADERIVPROC)gl_load("glGetShaderiv");
    p_glGetShaderInfoLog= (PFNGLGETSHADERINFOLOGPROC)gl_load("glGetShaderInfoLog");
    p_glDeleteShader    = (PFNGLDELETESHADERPROC)gl_load("glDeleteShader");
    p_glCreateProgram   = (PFNGLCREATEPROGRAMPROC)gl_load("glCreateProgram");
    p_glAttachShader    = (PFNGLATTACHSHADERPROC)gl_load("glAttachShader");
    p_glLinkProgram     = (PFNGLLINKPROGRAMPROC)gl_load("glLinkProgram");
    p_glGetProgramiv    = (PFNGLGETPROGRAMIVPROC)gl_load("glGetProgramiv");
    p_glGetProgramInfoLog=(PFNGLGETPROGRAMINFOLOGPROC)gl_load("glGetProgramInfoLog");
    p_glUseProgram      = (PFNGLUSEPROGRAMPROC)gl_load("glUseProgram");
    p_glDeleteProgram   = (PFNGLDELETEPROGRAMPROC)gl_load("glDeleteProgram");
    p_glGenBuffers      = (PFNGLGENBUFFERSPROC)gl_load("glGenBuffers");
    p_glBindBuffer      = (PFNGLBINDBUFFERPROC)gl_load("glBindBuffer");
    p_glBufferData      = (PFNGLBUFFERDATAPROC)gl_load("glBufferData");
    p_glDeleteBuffers   = (PFNGLDELETEBUFFERSPROC)gl_load("glDeleteBuffers");
    p_glEnableVertexAttribArray = (PFNGLENABLEVERTEXATTRIBARRAYPROC)gl_load("glEnableVertexAttribArray");
    p_glVertexAttribPointer = (PFNGLVERTEXATTRIBPOINTERPROC)gl_load("glVertexAttribPointer");
    p_glDrawElements    = (PFNGLDRAWELEMENTSPROC)gl_load("glDrawElements");
    p_glGetUniformLocation = (PFNGLGETUNIFORMLOCATIONPROC)gl_load("glGetUniformLocation");
    p_glUniform1i       = (PFNGLUNIFORM1IPROC)gl_load("glUniform1i");
    p_glUniform1f       = (PFNGLUNIFORM1FPROC)gl_load("glUniform1f");
    p_glActiveTexture   = (PFNGLACTIVETEXTUREPROC)gl_load("glActiveTexture");
    p_glGenTextures     = (PFNGLGENTEXTURESPROC)gl_load("glGenTextures");
    p_glBindTexture     = (PFNGLBINDTEXTUREPROC)gl_load("glBindTexture");
    p_glTexParameteri   = (PFNGLTEXPARAMETERIPROC)gl_load("glTexParameteri");
    p_glTexImage2D      = (PFNGLTEXIMAGE2DPROC)gl_load("glTexImage2D");
    p_glDeleteTextures  = (PFNGLDELETETEXTURESPROC)gl_load("glDeleteTextures");
    p_glDeleteVertexArrays = (PFNGLDELETEVERTEXARRAYSPROC)gl_load("glDeleteVertexArrays");
    p_glBindAttribLocation = (PFNGLBINDATTRIBLOCATIONPROC)gl_load("glBindAttribLocation");

    g_gl_core_ok = p_glGenVertexArrays && p_glBindVertexArray &&
                   p_glCreateShader && p_glShaderSource && p_glCompileShader &&
                   p_glGetShaderiv && p_glGetShaderInfoLog && p_glDeleteShader &&
                   p_glCreateProgram && p_glAttachShader && p_glLinkProgram &&
                   p_glGetProgramiv && p_glGetProgramInfoLog && p_glUseProgram &&
                   p_glDeleteProgram && p_glGenBuffers && p_glBindBuffer &&
                   p_glBufferData && p_glDeleteBuffers &&
                   p_glEnableVertexAttribArray && p_glVertexAttribPointer &&
                   p_glDrawElements && p_glGetUniformLocation && p_glUniform1i &&
                   p_glUniform1f && p_glGenTextures && p_glBindTexture &&
                   p_glTexParameteri && p_glTexImage2D && p_glDeleteTextures &&
                   p_glDeleteVertexArrays && p_glBindAttribLocation;

    g_gl_glsl_ok = false;
    if (g_gl_core_ok){
        const char* ver = (const char*)glGetString(GL_VERSION);
        int maj=0,min=0;
        if (ver && sscanf(ver, "%d.%d", &maj, &min) == 2){
            if (maj > 3 || (maj==3 && min>=3)){
                g_gl_glsl_ok = true;
            }
        }
        if (!g_gl_glsl_ok) g_gl_core_ok = false;
    }
}

static bool gl_extract_bytes(const Value* v, uint8_t** out, size_t* n, bool* need_free){
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
            int val;
            if (!v_num_to_int_trunc(*it, &val)){ free(buf); return false; }
            if (val < 0) val = 0; if (val > 255) val = 255;
            buf[i] = (uint8_t)val;
        }
        *out = buf; *n = len; *need_free=true; return true;
    }
    return false;
}

static bool gl_extract_floats(const Value* v, float** out, size_t* n, bool* need_free){
    *out=NULL; *n=0; *need_free=false;
    if (!v) return false;
    if (v->kind == VAL_STR){
        *out = (float*)v->as.str.s;
        *n   = v->as.str.n / sizeof(float);
        return true;
    }
    if (v->kind == VAL_LIST){
        size_t len = v->as.list->len;
        float* buf = (float*)malloc(len * sizeof(float));
        if (!buf) return false;
        for (size_t i=0;i<len;i++){
            Value* it = &v->as.list->data[i];
            if (it->kind != VAL_NUM){ free(buf); return false; }
            buf[i] = (float)it->as.num;
        }
        *out=buf; *n=len; *need_free=true; return true;
    }
    return false;
}

static bool gl_create_window(int w, int h, const char* title, int title_n, bool request_core){
    if (w <= 0 || h <= 0) return false;
    gl_destroy_all();

    WNDCLASSW wc; ZeroMemory(&wc, sizeof(wc));
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc   = gl_wndproc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"KirtonGLWindow";
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS){
        fprintf(stderr, "OpenGL: RegisterClassW failed (error %lu)\n", (unsigned long)GetLastError());
        return false;
    }

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
    if (!wtitle){ wtitle = _wcsdup(L"Kirton GL"); }

    RECT rc = {0, 0, w, h};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd_gl = CreateWindowW(L"KirtonGLWindow", wtitle, WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              rc.right - rc.left, rc.bottom - rc.top,
                              NULL, NULL, wc.hInstance, NULL);
    free(wtitle);
    if (!g_hwnd_gl){ fprintf(stderr, "OpenGL: CreateWindowW failed (error %lu)\n", (unsigned long)GetLastError()); return false; }

    g_hdc_gl = GetDC(g_hwnd_gl);
    if (!g_hdc_gl){ fprintf(stderr, "OpenGL: GetDC failed (error %lu)\n", (unsigned long)GetLastError()); gl_destroy_all(); return false; }

    PIXELFORMATDESCRIPTOR pfd;
    ZeroMemory(&pfd, sizeof(pfd));
    pfd.nSize      = sizeof(pfd);
    pfd.nVersion   = 1;
    pfd.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits= 8;
    pfd.iLayerType = PFD_MAIN_PLANE;

    int fmt = ChoosePixelFormat(g_hdc_gl, &pfd);
    if (fmt == 0){ fprintf(stderr, "OpenGL: ChoosePixelFormat failed (error %lu)\n", (unsigned long)GetLastError()); gl_destroy_all(); return false; }
    if (!SetPixelFormat(g_hdc_gl, fmt, &pfd)){
        fprintf(stderr, "OpenGL: SetPixelFormat failed (error %lu)\n", (unsigned long)GetLastError());
        gl_destroy_all(); return false;
    }

    g_glrc = wglCreateContext(g_hdc_gl);
    if (!g_glrc){
        fprintf(stderr, "OpenGL: wglCreateContext failed (error %lu)\n", (unsigned long)GetLastError());
        gl_destroy_all(); return false;
    }
    if (!wglMakeCurrent(g_hdc_gl, g_glrc)){
        fprintf(stderr, "OpenGL: activating temporary WGL context failed (error %lu)\n", (unsigned long)GetLastError());
        gl_destroy_all(); return false;
    }

    // A temporary legacy context is required to load WGL extensions. Replace it
    // with the requested context before exposing the window to Kirton code.
    if (request_core){
        p_wglCreateContextAttribsARB = (PFNWGLCREATECONTEXTATTRIBSARBPROC)gl_load("wglCreateContextAttribsARB");
        if (!p_wglCreateContextAttribsARB){
            fprintf(stderr, "OpenGL: WGL_ARB_create_context is unavailable; cannot create OpenGL 3.3 core context\n");
            gl_destroy_all(); return false;
        }
        const int attribs[] = {
            WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
            WGL_CONTEXT_MINOR_VERSION_ARB, 3,
            WGL_CONTEXT_PROFILE_MASK_ARB,  WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
            0
        };
        HGLRC core = p_wglCreateContextAttribsARB(g_hdc_gl, NULL, attribs);
        if (!core){
            fprintf(stderr, "OpenGL: creating OpenGL 3.3 core context failed (error %lu)\n", (unsigned long)GetLastError());
            gl_destroy_all(); return false;
        }
        if (!wglMakeCurrent(NULL, NULL) || !wglDeleteContext(g_glrc)){
            fprintf(stderr, "OpenGL: replacing temporary WGL context failed (error %lu)\n", (unsigned long)GetLastError());
            wglDeleteContext(core); gl_destroy_all(); return false;
        }
        g_glrc = core;
        if (!wglMakeCurrent(g_hdc_gl, g_glrc)){
            fprintf(stderr, "OpenGL: activating OpenGL 3.3 core context failed (error %lu)\n", (unsigned long)GetLastError());
            gl_destroy_all(); return false;
        }
        g_gl_is_core = true;
    }

    gl_load_core_functions();
    const char* vendor = (const char*)glGetString(GL_VENDOR);
    const char* renderer = (const char*)glGetString(GL_RENDERER);
    const char* version = (const char*)glGetString(GL_VERSION);
    const char* glsl = (const char*)glGetString(0x8B8C /* GL_SHADING_LANGUAGE_VERSION */);
    GLint profile = 0;
    if (request_core && version) glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile);
    fprintf(stderr, "OpenGL vendor: %s\nOpenGL renderer: %s\nOpenGL version: %s\nGLSL version: %s\nOpenGL profile: %s (mask=0x%x)\n",
            vendor ? vendor : "(unavailable)", renderer ? renderer : "(unavailable)",
            version ? version : "(unavailable)", glsl ? glsl : "(unavailable)",
            request_core ? "core" : "compatibility", (unsigned)profile);
    if (!vendor || !renderer || !version || (request_core && (!g_gl_core_ok || !g_gl_glsl_ok))){
        fprintf(stderr, "OpenGL: requested renderer capabilities are unavailable\n");
        gl_destroy_all(); return false;
    }

    ShowWindow(g_hwnd_gl, SW_SHOW);
    UpdateWindow(g_hwnd_gl);

    g_gl_w = w; g_gl_h = h;
    g_gl_ready = true;

    if (!request_core){
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
    }
    glViewport(0, 0, w, h);
    gl_report_error("initializing window state");
    return true;
}

static bool gl_pump(void){
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)){
        if (msg.message == WM_QUIT){ g_gl_ready = false; return false; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_hwnd_gl && g_glrc && gl_make_current()){
        RECT client;
        if (GetClientRect(g_hwnd_gl, &client)){
            int w = client.right - client.left, h = client.bottom - client.top;
            if (w > 0 && h > 0 && (w != g_gl_w || h != g_gl_h)){
                g_gl_w = w; g_gl_h = h;
                glViewport(0, 0, w, h);
                gl_report_error("resizing viewport");
            }
        }
    }
    gl_update_keys();
    return g_gl_ready && g_hwnd_gl != NULL;
}

/* --------- Kirton bindings --------- */

static Value k_gl_create_window(int n, const Value* a){
    if (n < 2) return v_bool(false);
    if (a[0].kind != VAL_NUM || a[1].kind != VAL_NUM) return v_bool(false);
    int width, height;
    if (!v_num_to_int_trunc(a[0], &width) || !v_num_to_int_trunc(a[1], &height)) return v_bool(false);
    const char* title = (n >= 3 && a[2].kind == VAL_STR) ? a[2].as.str.s : "Kirton GL";
    int title_n = (n >= 3 && a[2].kind == VAL_STR) ? (int)a[2].as.str.n : -1;
    bool ok = gl_create_window(width, height, title, title_n, false);
    return v_bool(ok);
}

static Value k_gl_create_window_core(int n, const Value* a){
    if (n < 2 || a[0].kind != VAL_NUM || a[1].kind != VAL_NUM) return v_bool(false);
    int width, height;
    if (!v_num_to_int_trunc(a[0], &width) || !v_num_to_int_trunc(a[1], &height)) return v_bool(false);
    const char* title = (n >= 3 && a[2].kind == VAL_STR) ? a[2].as.str.s : "Kirton GL Core";
    int title_n = (n >= 3 && a[2].kind == VAL_STR) ? (int)a[2].as.str.n : -1;
    return v_bool(gl_create_window(width, height, title, title_n, true));
}

static Value k_gl_destroy(int n, const Value* a){
    (void)n; (void)a;
    gl_destroy_all();
    return v_bool(g_gl_cleanup_ok);
}

static Value k_gl_poll(int n, const Value* a){
    (void)n; (void)a;
    bool ok = gl_pump();
    if (!ok) gl_destroy_all();
    return v_bool(ok);
}

static Value k_gl_swap(int n, const Value* a){
    (void)n; (void)a;
    if (!g_gl_ready || !gl_make_current()) return v_bool(false);
    if (!SwapBuffers(g_hdc_gl)){
        fprintf(stderr, "OpenGL: SwapBuffers failed (error %lu)\n", (unsigned long)GetLastError());
        return v_bool(false);
    }
    return v_bool(true);
}

static Value k_gl_clear(int n, const Value* a){
    if (!g_gl_ready || !gl_make_current()) return v_bool(false);
    float r=0, g=0, b=0, al=1;
    if (n > 0 && a[0].kind == VAL_NUM) r = (float)a[0].as.num;
    if (n > 1 && a[1].kind == VAL_NUM) g = (float)a[1].as.num;
    if (n > 2 && a[2].kind == VAL_NUM) b = (float)a[2].as.num;
    if (n > 3 && a[3].kind == VAL_NUM) al = (float)a[3].as.num;
    glClearColor(r, g, b, al);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    return v_bool(gl_report_error("clear") == GL_NO_ERROR);
}

static Value k_gl_viewport(int n, const Value* a){
    if (!g_gl_ready || !gl_make_current()) return v_bool(false);
    if (n < 2 || a[0].kind != VAL_NUM || a[1].kind != VAL_NUM) return v_bool(false);
    int w, h;
    if (!v_num_to_int_trunc(a[0], &w) || !v_num_to_int_trunc(a[1], &h)) return v_bool(false);
    glViewport(0, 0, w, h);
    g_gl_w = w; g_gl_h = h;
    return v_bool(true);
}

static Value k_gl_enable_depth(int n, const Value* a){
    if (!g_gl_ready || !gl_make_current()) return v_bool(false);
    bool on = true;
    if (n > 0 && a[0].kind == VAL_BOOL) on = a[0].as.boolean;
    else if (n > 0 && a[0].kind == VAL_NUM) on = (a[0].as.num != 0.0);
    if (on) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    return v_bool(true);
}

static Value k_gl_enable_alpha(int n, const Value* a){
    if (!g_gl_ready || !gl_make_current()) return v_bool(false);
    bool on = true;
    if (n > 0 && a[0].kind == VAL_BOOL) on = a[0].as.boolean;
    else if (n > 0 && a[0].kind == VAL_NUM) on = (a[0].as.num != 0.0);
    if (on){
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }
    return v_bool(true);
}

static Value k_gl_enable_cull(int n, const Value* a){
    if (!g_gl_ready || !gl_make_current()) return v_bool(false);
    bool on = true;
    if (n > 0 && a[0].kind == VAL_BOOL) on = a[0].as.boolean;
    else if (n > 0 && a[0].kind == VAL_NUM) on = (a[0].as.num != 0.0);
    if (on){ glEnable(GL_CULL_FACE); glCullFace(GL_BACK); }
    else { glDisable(GL_CULL_FACE); }
    return v_bool(true);
}

static Value k_gl_begin(int n, const Value* a){
    if (!g_gl_ready || !gl_make_current_compat() || g_gl_in_begin) return v_bool(false);
    int mode = GL_TRIANGLES;
    if (n > 0 && a[0].kind == VAL_NUM && !v_num_to_int_trunc(a[0], &mode)) return v_bool(false);
    glBegin(mode);
    g_gl_in_begin = true;
    return v_bool(true);
}

static Value k_gl_end(int n, const Value* a){
    (void)n; (void)a;
    if (!g_gl_ready || !g_gl_in_begin || !gl_make_current_compat()) return v_bool(false);
    glEnd();
    g_gl_in_begin = false;
    return v_bool(gl_report_error("glEnd/immediate-mode batch") == GL_NO_ERROR);
}

static Value k_gl_vertex3(int n, const Value* a){
    if (!g_gl_ready || !g_gl_in_begin || !gl_make_current_compat()) return v_bool(false);
    if (n < 3 || a[0].kind != VAL_NUM || a[1].kind != VAL_NUM || a[2].kind != VAL_NUM) return v_bool(false);
    float x=(float)a[0].as.num, y=(float)a[1].as.num, z=(float)a[2].as.num;
    glVertex3f(x, y, z);
    return v_bool(true);
}

static Value k_gl_color3(int n, const Value* a){
    if (!g_gl_ready || !g_gl_in_begin || !gl_make_current_compat()) return v_bool(false);
    if (n < 3 || a[0].kind != VAL_NUM || a[1].kind != VAL_NUM || a[2].kind != VAL_NUM) return v_bool(false);
    float r=(float)a[0].as.num, g=(float)a[1].as.num, b=(float)a[2].as.num;
    float a4 = (n>3 && a[3].kind==VAL_NUM) ? (float)a[3].as.num : 1.0f;
    glColor4f(r, g, b, a4);
    return v_bool(true);
}

static Value k_gl_texcoord2(int n, const Value* a){
    if (!g_gl_ready || !g_gl_in_begin || !gl_make_current_compat()) return v_bool(false);
    if (n < 2 || a[0].kind != VAL_NUM || a[1].kind != VAL_NUM) return v_bool(false);
    float u=(float)a[0].as.num, v=(float)a[1].as.num;
    glTexCoord2f(u, v);
    return v_bool(true);
}

static Value k_gl_normal3(int n, const Value* a){
    if (!g_gl_ready || !g_gl_in_begin || !gl_make_current_compat()) return v_bool(false);
    if (n < 3 || a[0].kind != VAL_NUM || a[1].kind != VAL_NUM || a[2].kind != VAL_NUM) return v_bool(false);
    float x=(float)a[0].as.num, y=(float)a[1].as.num, z=(float)a[2].as.num;
    glNormal3f(x, y, z);
    return v_bool(true);
}

static Value k_gl_matrix_mode_projection(int n, const Value* a){
    (void)n; (void)a;
    if (!g_gl_ready || !gl_make_current_compat()) return v_bool(false);
    glMatrixMode(GL_PROJECTION);
    return v_bool(true);
}

static Value k_gl_matrix_mode_modelview(int n, const Value* a){
    (void)n; (void)a;
    if (!g_gl_ready || !gl_make_current_compat()) return v_bool(false);
    glMatrixMode(GL_MODELVIEW);
    return v_bool(true);
}

static Value k_gl_load_identity(int n, const Value* a){
    (void)n; (void)a;
    if (!g_gl_ready || !gl_make_current_compat()) return v_bool(false);
    glLoadIdentity();
    return v_bool(true);
}

static Value k_gl_translate(int n, const Value* a){
    if (!g_gl_ready || !gl_make_current_compat()) return v_bool(false);
    if (n < 3) return v_bool(false);
    glTranslatef((float)a[0].as.num, (float)a[1].as.num, (float)a[2].as.num);
    return v_bool(true);
}

static Value k_gl_rotate_deg(int n, const Value* a){
    if (!g_gl_ready || !gl_make_current_compat()) return v_bool(false);
    if (n < 4) return v_bool(false);
    glRotatef((float)a[0].as.num, (float)a[1].as.num, (float)a[2].as.num, (float)a[3].as.num);
    return v_bool(true);
}

static Value k_gl_scale(int n, const Value* a){
    if (!g_gl_ready || !gl_make_current_compat()) return v_bool(false);
    if (n < 3) return v_bool(false);
    glScalef((float)a[0].as.num, (float)a[1].as.num, (float)a[2].as.num);
    return v_bool(true);
}

static Value k_gl_perspective(int n, const Value* a){
    if (!g_gl_ready || !gl_make_current_compat()) return v_bool(false);
    if (n < 4) return v_bool(false);
    float fov = (float)a[0].as.num;
    float aspect = (float)a[1].as.num;
    float zn = (float)a[2].as.num;
    float zf = (float)a[3].as.num;
    if (aspect <= 0.0f || zn <= 0.0f || zf <= zn) return v_bool(false);
    float t = tanf(fov * (float)M_PI / 360.0f);
    float top = zn * t;
    float right = top * aspect;
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-right, right, -top, top, zn, zf);
    glMatrixMode(GL_MODELVIEW);
    return v_bool(true);
}

static Value k_gl_modes_triangles(int n, const Value* a){ (void)n;(void)a; return v_int(GL_TRIANGLES); }
static Value k_gl_modes_lines(int n, const Value* a){ (void)n;(void)a; return v_int(GL_LINES); }
static Value k_gl_modes_triangle_strip(int n, const Value* a){ (void)n;(void)a; return v_int(GL_TRIANGLE_STRIP); }
static Value k_gl_modes_triangle_fan(int n, const Value* a){ (void)n;(void)a; return v_int(GL_TRIANGLE_FAN); }
static Value k_gl_modes_quads(int n, const Value* a){ (void)n;(void)a; return v_int(GL_QUADS); }

static Value k_gl_mouse(int n, const Value* a){
    (void)n;(void)a;
    Value L = v_list_new();
    v_list_push(&L, v_int(g_mouse_x));
    v_list_push(&L, v_int(g_mouse_y));
    return L;
}

static Value k_gl_mouse_wheel(int n, const Value* a){
    (void)n;(void)a;
    int d = g_wheel; g_wheel = 0;
    return v_int(d);
}

static Value k_gl_key_down_fn(int n, const Value* a){
    if (n < 1 || a[0].kind != VAL_NUM) return v_bool(false);
    int vk;
    if (!v_num_to_int_trunc(a[0], &vk)) return v_bool(false);
    vk &= 255;
    return v_bool(g_key_down[vk] != 0);
}

static Value k_gl_key_pressed_fn(int n, const Value* a){
    if (n < 1 || a[0].kind != VAL_NUM) return v_bool(false);
    int vk;
    if (!v_num_to_int_trunc(a[0], &vk)) return v_bool(false);
    vk &= 255;
    return v_bool(g_key_down[vk] && !g_key_prev[vk]);
}

static Value k_gl_key_released_fn(int n, const Value* a){
    if (n < 1 || a[0].kind != VAL_NUM) return v_bool(false);
    int vk;
    if (!v_num_to_int_trunc(a[0], &vk)) return v_bool(false);
    vk &= 255;
    return v_bool(!g_key_down[vk] && g_key_prev[vk]);
}

static Value k_gl_last_key_fn(int n, const Value* a){
    (void)n;(void)a;
    int k = g_last_key; g_last_key = -1;
    return v_int(k);
}

static Value k_gl_window_size(int n, const Value* a){
    (void)n;(void)a;
    Value L = v_list_new();
    v_list_push(&L, v_int(g_gl_w));
    v_list_push(&L, v_int(g_gl_h));
    return L;
}

/* ===================== Modern GL pipeline ===================== */

static char* gl_sanitize_shader(const char* src, int len, int* out_len){
    if (!src || len <= 0) return NULL;
    const unsigned char* us = (const unsigned char*)src;
    int i = 0;
    // Skip UTF-8 BOM if present
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

static GLuint gl_compile(GLenum type, const char* src, int len){
    int clean_len = len;
    char* clean = gl_sanitize_shader(src, len, &clean_len);
    const GLchar* s = (const GLchar*)(clean ? clean : src);
    GLint l = (GLint)clean_len;
    GLuint sh = p_glCreateShader(type);
    if (!sh){ if (clean) free(clean); return 0; }
    p_glShaderSource(sh, 1, &s, &l);
    p_glCompileShader(sh);
    GLint ok=0; p_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (clean) free(clean);
    if (!ok){
        char log[512]={0}; GLsizei out=0;
        p_glGetShaderInfoLog(sh, (GLsizei)sizeof(log)-1, &out, log);
        (void)out;
        fprintf(stderr, "GL shader compile failed: %s\n", log);
        p_glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static Value k_gl_create_program(int n, const Value* a){
    if (!g_gl_ready || !gl_make_current_core()) return v_num(0);
    gl_load_core_functions();
    if (!g_gl_core_ok || !g_gl_glsl_ok){
        fprintf(stderr, "GLSL not available (need OpenGL 3.3+ core context)\n");
        return v_num(0);
    }
    if (n < 2 || a[0].kind != VAL_STR || a[1].kind != VAL_STR) return v_num(0);
    GLuint vs = gl_compile(GL_VERTEX_SHADER, a[0].as.str.s, (int)a[0].as.str.n);
    GLuint fs = gl_compile(GL_FRAGMENT_SHADER, a[1].as.str.s, (int)a[1].as.str.n);
    if (!vs || !fs){
        if (vs) p_glDeleteShader(vs);
        if (fs) p_glDeleteShader(fs);
        return v_num(0);
    }
    GLuint prog = p_glCreateProgram();
    p_glAttachShader(prog, vs);
    p_glAttachShader(prog, fs);
    if (p_glBindAttribLocation){
        p_glBindAttribLocation(prog, 0, "aPos");
        p_glBindAttribLocation(prog, 1, "aCol");
        p_glBindAttribLocation(prog, 2, "aUV");
    }
    p_glLinkProgram(prog);
    GLint ok=0; p_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    p_glDeleteShader(vs);
    p_glDeleteShader(fs);
    if (!ok){
        char log[512]={0}; GLsizei out=0;
        p_glGetProgramInfoLog(prog, (GLsizei)sizeof(log)-1, &out, log);
        fprintf(stderr, "GL program link failed: %s\n", log);
        p_glDeleteProgram(prog);
        return v_num(0);
    }
    int handle = gl_alloc_program();
    if (!handle){ p_glDeleteProgram(prog); return v_num(0); }
    g_gl_progs[handle-1].id = prog;
    g_gl_progs[handle-1].sampler_loc = p_glGetUniformLocation ? p_glGetUniformLocation(prog, "uTex") : -1;
    return v_int(handle);
}

static Value k_gl_destroy_program(int n, const Value* a){
    if (!g_gl_ready || !gl_make_current_core() || !g_gl_core_ok) return v_bool(false);
    if (n < 1 || a[0].kind != VAL_NUM) return v_bool(false);
    int h;
    if (!v_num_to_int_trunc(a[0], &h)) return v_bool(false);
    if (h<=0 || h>GL_MAX_PROGS) return v_bool(false);
    if (g_gl_progs[h-1].id && p_glDeleteProgram){
        p_glDeleteProgram(g_gl_progs[h-1].id);
    }
    g_gl_progs[h-1].id=0; g_gl_progs[h-1].sampler_loc=-1;
    return v_bool(true);
}

static Value k_gl_create_vb(int n, const Value* a){
    if (!g_gl_ready || !gl_make_current_core()) return v_num(0);
    gl_load_core_functions();
    if (!g_gl_core_ok) return v_num(0);
    if (n < 3 || a[1].kind != VAL_NUM || a[2].kind != VAL_NUM) return v_num(0);
    float* data=NULL; size_t count=0; bool need_free=false;
    if (!gl_extract_floats(&a[0], &data, &count, &need_free)) return v_num(0);
    int stride, vcount;
    if (!v_num_to_int_trunc(a[1], &stride) || !v_num_to_int_trunc(a[2], &vcount) || stride <= 0 || vcount <= 0){
        if (need_free) free(data);
        return v_num(0);
    }
    if ((size_t)vcount > SIZE_MAX / (size_t)stride){ if (need_free) free(data); return v_num(0); }
    size_t bytes = (size_t)stride * (size_t)vcount;
    if (bytes == 0 || bytes > (size_t)PTRDIFF_MAX || count > SIZE_MAX / sizeof(float) || bytes > count*sizeof(float)){ if (need_free) free(data); return v_num(0); }
    GLuint id=0; p_glGenBuffers(1, &id);
    if (!id){ if (need_free) free(data); return v_num(0); }
    p_glBindBuffer(GL_ARRAY_BUFFER, id);
    p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)bytes, data, GL_STATIC_DRAW);
    if (need_free) free(data);
    if (gl_report_error("create_vertex_buffer") != GL_NO_ERROR){ GLuint tmp=id; p_glDeleteBuffers(1,&tmp); return v_num(0); }
    int handle = gl_alloc_buffer();
    if (!handle){ GLuint tmp=id; p_glDeleteBuffers(1,&tmp); return v_num(0); }
    g_gl_bufs[handle-1].id = id;
    g_gl_bufs[handle-1].stride = stride;
    g_gl_bufs[handle-1].count = vcount;
    g_gl_bufs[handle-1].target = GL_ARRAY_BUFFER;
    return v_int(handle);
}

static Value k_gl_create_ib(int n, const Value* a){
    if (!g_gl_ready || !gl_make_current_core()) return v_num(0);
    gl_load_core_functions();
    if (!g_gl_core_ok) return v_num(0);
    if (n < 2 || a[1].kind != VAL_NUM) return v_num(0);
    uint8_t* raw=NULL; size_t bytes=0; bool need_free=false;
    if (a[0].kind == VAL_LIST){
        size_t len = a[0].as.list->len;
        if (len > SIZE_MAX / sizeof(uint32_t)) return v_num(0);
        uint32_t* buf = (uint32_t*)malloc(len * sizeof(uint32_t));
        if (!buf) return v_num(0);
        for (size_t i=0;i<len;i++){
            Value* it = &a[0].as.list->data[i];
            if (it->kind != VAL_NUM){ free(buf); return v_num(0); }
            int v;
            if (!v_num_to_int_trunc(*it, &v) || v < 0){ free(buf); return v_num(0); }
            uint32_t exact = (uint32_t)v;
            buf[i]=exact;
        }
        raw = (uint8_t*)buf;
        bytes = len * sizeof(uint32_t);
        need_free = true;
    } else {
        if (!gl_extract_bytes(&a[0], &raw, &bytes, &need_free)) return v_num(0);
    }
    int icount;
    if (!v_num_to_int_trunc(a[1], &icount) || icount <= 0){ if (need_free) free(raw); return v_num(0); }
    size_t needed = (size_t)icount * sizeof(uint32_t);
    if (bytes < needed){ if (need_free) free(raw); return v_num(0); }
    GLuint id=0; p_glGenBuffers(1, &id);
    if (!id){ if (need_free) free(raw); return v_num(0); }
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, id);
    p_glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)needed, raw, GL_STATIC_DRAW);
    if (need_free) free(raw);
    if (gl_report_error("create_index_buffer") != GL_NO_ERROR){ GLuint tmp=id; p_glDeleteBuffers(1,&tmp); return v_num(0); }
    int handle = gl_alloc_buffer();
    if (!handle){ GLuint tmp=id; p_glDeleteBuffers(1,&tmp); return v_num(0); }
    g_gl_bufs[handle-1].id = id;
    g_gl_bufs[handle-1].stride = sizeof(uint32_t);
    g_gl_bufs[handle-1].count = icount;
    g_gl_bufs[handle-1].target = GL_ELEMENT_ARRAY_BUFFER;
    return v_int(handle);
}

static bool gl_prepare_vao(void){
    gl_load_core_functions();
    if (!g_gl_core_ok || !p_glGenVertexArrays) return false;
    if (g_gl_vao) return true;
    p_glGenVertexArrays(1, &g_gl_vao);
    return g_gl_vao != 0;
}

static Value k_gl_draw_indexed(int n, const Value* a){
    if (!g_gl_ready || !gl_make_current_core()) return v_bool(false);
    gl_load_core_functions();
    if (!g_gl_core_ok) return v_bool(false);
    if (n < 3) return v_bool(false);
    int hvb, hib, hp, mode=GL_TRIANGLES;
    if (a[0].kind!=VAL_NUM || a[1].kind!=VAL_NUM || a[2].kind!=VAL_NUM ||
        !v_num_to_int_trunc(a[0], &hvb) || !v_num_to_int_trunc(a[1], &hib) || !v_num_to_int_trunc(a[2], &hp)) return v_bool(false);
    if (n>=4 && a[3].kind==VAL_NUM && !v_num_to_int_trunc(a[3], &mode)) return v_bool(false);
    if (hvb<=0||hvb>GL_MAX_BUFFERS||hib<=0||hib>GL_MAX_BUFFERS||hp<=0||hp>GL_MAX_PROGS) return v_bool(false);
    GlBuffer* vb = &g_gl_bufs[hvb-1];
    GlBuffer* ib = &g_gl_bufs[hib-1];
    GlProgram* pr = &g_gl_progs[hp-1];
    if (!vb->id || !ib->id || !pr->id) return v_bool(false);
    if (!gl_prepare_vao()) return v_bool(false);
    p_glBindVertexArray(g_gl_vao);
    p_glUseProgram(pr->id);

    if (g_gl_tex_bound>0 && g_gl_tex_bound<=GL_MAX_TEX && g_gl_tex[g_gl_tex_bound-1].id){
        if (p_glActiveTexture) p_glActiveTexture(GL_TEXTURE0);
        p_glBindTexture(GL_TEXTURE_2D, g_gl_tex[g_gl_tex_bound-1].id);
        if (pr->sampler_loc >= 0 && p_glUniform1i) p_glUniform1i(pr->sampler_loc, 0);
    }else{
        p_glBindTexture(GL_TEXTURE_2D, 0);
    }

    p_glBindBuffer(GL_ARRAY_BUFFER, vb->id);
    p_glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib->id);
    int stride = vb->stride;
    if (stride < (int)(sizeof(float)*9)) return v_bool(false);
    p_glEnableVertexAttribArray(0);
    p_glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (const void*)0);
    p_glEnableVertexAttribArray(1);
    p_glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, (const void*)(sizeof(float)*3));
    p_glEnableVertexAttribArray(2);
    p_glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (const void*)(sizeof(float)*7));
    p_glDrawElements(mode, ib->count, GL_UNSIGNED_INT, (const void*)0);
    return v_bool(gl_report_error("draw_indexed") == GL_NO_ERROR);
}

static Value k_gl_load_texture(int n, const Value* a){
    if (!g_gl_ready || !gl_make_current_core()) return v_num(0);
    gl_load_core_functions();
    if (!g_gl_core_ok) return v_num(0);
    if (n<1 || a[0].kind!=VAL_STR) return v_num(0);
    int w=0,h=0,comp=0;
    unsigned char* px = stbi_load(a[0].as.str.s, &w, &h, &comp, 4);
    if (!px){
        fprintf(stderr, "gl.load_texture failed: %s\n", stbi_failure_reason());
        return v_num(0);
    }
    GLuint id=0; p_glGenTextures(1,&id);
    if (!id){ stbi_image_free(px); return v_num(0); }
    p_glBindTexture(GL_TEXTURE_2D, id);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    stbi_image_free(px);
    int handle = gl_alloc_tex();
    if (!handle){ GLuint tmp=id; p_glDeleteTextures(1,&tmp); return v_num(0); }
    g_gl_tex[handle-1].id=id; g_gl_tex[handle-1].w=w; g_gl_tex[handle-1].h=h;
    return v_int(handle);
}

static Value k_gl_bind_texture(int n, const Value* a){
    if (n<1 || a[0].kind!=VAL_NUM) return v_bool(false);
    int h;
    if (!v_num_to_int_trunc(a[0], &h)) return v_bool(false);
    if (h<0 || h>GL_MAX_TEX) return v_bool(false);
    g_gl_tex_bound = h;
    return v_bool(true);
}

static Value k_gl_set_uniform1f(int n, const Value* a){
    if (!g_gl_ready || !gl_make_current_core()) return v_bool(false);
    gl_load_core_functions();
    if (!g_gl_core_ok) return v_bool(false);
    if (n<3 || a[0].kind!=VAL_NUM || a[1].kind!=VAL_STR || a[2].kind!=VAL_NUM) return v_bool(false);
    int hp;
    if (!v_num_to_int_trunc(a[0], &hp)) return v_bool(false);
    if (hp<=0 || hp>GL_MAX_PROGS) return v_bool(false);
    if (!g_gl_progs[hp-1].id || !p_glGetUniformLocation || !p_glUniform1f) return v_bool(false);
    p_glUseProgram(g_gl_progs[hp-1].id);
    GLint loc = p_glGetUniformLocation(g_gl_progs[hp-1].id, (const GLchar*)a[1].as.str.s);
    if (loc<0) return v_bool(false);
    p_glUniform1f(loc, (float)a[2].as.num);
    return v_bool(gl_report_error("set_uniform1f") == GL_NO_ERROR);
}

static const KirNativeDef FUNCS[] = {
    {"create_window", k_gl_create_window},
    {"create_window_core", k_gl_create_window_core},
    {"destroy",       k_gl_destroy},
    {"poll",          k_gl_poll},
    {"swap",          k_gl_swap},
    {"clear",         k_gl_clear},
    {"viewport",      k_gl_viewport},
    {"enable_depth",  k_gl_enable_depth},
    {"enable_alpha",  k_gl_enable_alpha},
    {"enable_cull",   k_gl_enable_cull},
    {"begin",         k_gl_begin},
    {"end",           k_gl_end},
    {"vertex3",       k_gl_vertex3},
    {"color3",        k_gl_color3},
    {"texcoord2",     k_gl_texcoord2},
    {"normal3",       k_gl_normal3},
    {"matrix_projection", k_gl_matrix_mode_projection},
    {"matrix_modelview",  k_gl_matrix_mode_modelview},
    {"load_identity", k_gl_load_identity},
    {"translate",     k_gl_translate},
    {"rotate_deg",    k_gl_rotate_deg},
    {"scale",         k_gl_scale},
    {"perspective",   k_gl_perspective},
    {"mode_triangles",k_gl_modes_triangles},
    {"mode_lines",    k_gl_modes_lines},
    {"mode_triangle_strip", k_gl_modes_triangle_strip},
    {"mode_triangle_fan",   k_gl_modes_triangle_fan},
    {"mode_quads",    k_gl_modes_quads},
    {"mouse_pos",     k_gl_mouse},
    {"mouse_wheel",   k_gl_mouse_wheel},
    {"key_down",      k_gl_key_down_fn},
    {"key_pressed",   k_gl_key_pressed_fn},
    {"key_released",  k_gl_key_released_fn},
    {"last_key",      k_gl_last_key_fn},
    {"window_size",   k_gl_window_size},

    // Modern pipeline
    {"create_shader_program", k_gl_create_program},
    {"destroy_shader_program", k_gl_destroy_program},
    {"create_vertex_buffer", k_gl_create_vb},
    {"create_index_buffer", k_gl_create_ib},
    {"draw_indexed", k_gl_draw_indexed},
    {"load_texture", k_gl_load_texture},
    {"bind_texture", k_gl_bind_texture},
    {"set_uniform1f", k_gl_set_uniform1f},
};

void gl_init(void){
    static int cleanup_registered=0;
    if (!cleanup_registered){
        atexit(gl_destroy_all);
        cleanup_registered=1;
    }
    kir_native_register_module("gl", FUNCS, sizeof(FUNCS)/sizeof(FUNCS[0]));
    kir_native_mark_bytecode_module_safe("gl");
}

#else

void gl_init(void){}

#endif /* _WIN32 */
