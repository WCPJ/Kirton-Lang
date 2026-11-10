/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
// src/mod_os.c — окружение и OS-утилиты для Киртона.
// Контракты:
//   окружение()                      -> DICT всех переменных
//   окружение(name: STRING)         -> STRING значения или nil
//   установить_окружение(name,value)-> BOOL
//   текущая_папка()                 -> STRING (UTF-8 путь)

#include "kirton/native.h"
#include "native_internal.h"
#include "kirton/value.h"
#include "kirton/common.h"   /* kir_malloc, kir_xreallocn и др. */

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <direct.h>
#else
  #include <unistd.h>
  #include <limits.h>
  extern char **environ;
#endif

/* ==================== helpers ==================== */

static char* dupn(const char* s, size_t n){
    if (n==SIZE_MAX) return NULL;
    char* out = (char*)kir_oom_malloc(n + 1);
    if (!out) return NULL;
    if (n) memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

#ifdef _WIN32
static wchar_t* u8_to_w(const char* u8, int n){
    if (!u8) return NULL;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, u8, n >= 0 ? n : -1, NULL, 0);
    if (wlen <= 0) return NULL;

    /* ВАЖНО: (wlen + 1) * sizeof(wchar_t), а не wlen + sizeof(wchar_t) */
    size_t bytes=0;
    if (kir_mul_overflow_size((size_t)wlen+1u,sizeof(wchar_t),&bytes)) return NULL;
    wchar_t* w = (wchar_t*)kir_oom_malloc(bytes);
    if (!w) return NULL;

    MultiByteToWideChar(CP_UTF8, 0, u8, n >= 0 ? n : -1, w, wlen);
    w[wlen] = 0;
    return w;
}

static char* w_to_u8(const wchar_t* w, int wn){
    if (!w) return dupn("", 0);
    int u8len = WideCharToMultiByte(CP_UTF8, 0, w, wn >= 0 ? wn : -1,
                                    NULL, 0, NULL, NULL);
    if (u8len <= 0) return dupn("", 0);
    char* out = (char*)kir_oom_malloc((size_t)u8len + 1);
    if (!out) return dupn("", 0);
    WideCharToMultiByte(CP_UTF8, 0, w, wn >= 0 ? wn : -1,
                        out, u8len, NULL, NULL);
    out[u8len] = 0;
    return out;
}
#endif

static Value make_nil(void){
    Value v;
    memset(&v, 0, sizeof(v));
    v.kind = VAL_NIL;
    return v;
}

/* ==================== окружение ==================== */

/* окружение(name?) */
static Value os_env(int argc, const Value* argv){
    /* Без аргументов: вернуть DICT со всем окружением */
    if (argc == 0){
        Value dict = v_dict_new();

#ifdef _WIN32
        LPWCH block = GetEnvironmentStringsW();
        if (!block) return dict;

        const wchar_t* p = block;
        while (*p){
            const wchar_t* start = p;
            /* строка вида L"NAME=VALUE" */
            while (*p) ++p;
            size_t len = (size_t)(p - start);
            if (len > 0){
                char* u8 = w_to_u8(start, (int)len);
                if (u8){
                    char* eq = strchr(u8, '=');
                    if (eq){
                        size_t kn = (size_t)(eq - u8);
                        const char* vs = eq + 1;
                        size_t vn = strlen(vs);
                        if (kn > 0){
                            v_dict_set(&dict, u8, kn, v_str_copy(vs, (int)vn));
                        }
                    }
                    free(u8);
                }
            }
            ++p; /* перейти на следующую wide-строку */
        }
        FreeEnvironmentStringsW(block);
#else
        if (environ){
            for (char** e = environ; *e; ++e){
                char* s = *e;
                char* eq = strchr(s, '=');
                if (!eq) continue;
                size_t kn = (size_t)(eq - s);
                const char* vs = eq + 1;
                size_t vn = strlen(vs);
                if (kn == 0) continue;
                v_dict_set(&dict, s, kn, v_str_copy(vs, (int)vn));
            }
        }
#endif
        return dict;
    }

    /* Один аргумент: окружение("NAME") -> строка или nil */
    if (argc >= 1 && argv[0].kind == VAL_STR){
        const char* name = argv[0].as.str.s;

#ifdef _WIN32
        if (argv[0].as.str.n>(size_t)INT_MAX) return make_nil();
        wchar_t* wname = u8_to_w(name, (int)argv[0].as.str.n);
        if (!wname) return make_nil();

        /* сначала узнаём длину */
        DWORD need = GetEnvironmentVariableW(wname, NULL, 0);
        if (need == 0){
            free(wname);
            return make_nil(); /* переменной нет */
        }

        size_t bytes=0;
        if (kir_mul_overflow_size((size_t)need,sizeof(wchar_t),&bytes)){
            free(wname);
            return make_nil();
        }
        wchar_t* wval = (wchar_t*)kir_oom_malloc(bytes);
        if (!wval){
            free(wname);
            return make_nil();
        }

        DWORD got = GetEnvironmentVariableW(wname, wval, need);
        free(wname);
        if (got == 0){
            free(wval);
            return make_nil();
        }

        char* u8 = w_to_u8(wval, (int)got);
        free(wval);
        if (!u8) return make_nil();
        Value res = v_str_copy(u8, -1);
        free(u8);
        return res;
#else
        const char* v = getenv(name);
        if (!v) return make_nil();
        return v_str_copy(v, -1);
#endif
    }

    return make_nil();
}

/* установить_окружение(name, value) -> bool */
static Value os_setenv(int argc, const Value* argv){
    if (argc < 2 || argv[0].kind != VAL_STR || argv[1].kind != VAL_STR)
        return v_bool(false);

    const char* name = argv[0].as.str.s;
    const char* val  = argv[1].as.str.s;

#ifdef _WIN32
    if (argv[0].as.str.n>(size_t)INT_MAX || argv[1].as.str.n>(size_t)INT_MAX)
        return v_bool(false);
    wchar_t* wname = u8_to_w(name, (int)argv[0].as.str.n);
    wchar_t* wval  = u8_to_w(val,  (int)argv[1].as.str.n);
    if (!wname || !wval){
        if (wname) free(wname);
        if (wval)  free(wval);
        return v_bool(false);
    }
    BOOL ok = SetEnvironmentVariableW(wname, wval);
    free(wname);
    free(wval);
    return v_bool(ok != 0);
#else
    int rc = setenv(name, val, 1);
    return v_bool(rc == 0);
#endif
}

/* ==================== текущая_папка ==================== */

static Value os_cwd(int argc, const Value* argv){
    (void)argc; (void)argv;
#ifdef _WIN32
    DWORD need = GetCurrentDirectoryW(0, NULL);
    if (need == 0) return v_str_copy("", 0);

    size_t bytes=0;
    if (kir_mul_overflow_size((size_t)need,sizeof(wchar_t),&bytes))
        return v_str_copy("",0);
    wchar_t* wbuf = (wchar_t*)kir_oom_malloc(bytes);
    if (!wbuf) return v_str_copy("", 0);

    DWORD got = GetCurrentDirectoryW(need, wbuf);
    if (got == 0){
        free(wbuf);
        return v_str_copy("", 0);
    }

    char* u8 = w_to_u8(wbuf, (int)got);
    free(wbuf);
    if (!u8) return v_str_copy("", 0);
    Value v = v_str_copy(u8, -1);
    free(u8);
    return v;
#else
    char buf[PATH_MAX];
    char* r = getcwd(buf, sizeof(buf));
    if (!r) return v_str_copy("", 0);
    return v_str_copy(r, -1);
#endif
}

/* ==================== регистрация ==================== */

static const KirNativeDef OS_FUNCS[] = {
    { "окружение",             os_env },
    { "установить_окружение",  os_setenv },
    { "текущая_папка",         os_cwd },
};

void os_init(void){
    /* модуль "os" */
    kir_native_register_module("os", OS_FUNCS,
        (int)(sizeof(OS_FUNCS)/sizeof(OS_FUNCS[0])));
    /* глобальные функции */
    kir_native_register(OS_FUNCS,
        (int)(sizeof(OS_FUNCS)/sizeof(OS_FUNCS[0])));
    kir_native_mark_bytecode_module_safe("os");
    kir_native_mark_bytecode_safe(OS_FUNCS, (int)(sizeof(OS_FUNCS)/sizeof(OS_FUNCS[0])));
}
