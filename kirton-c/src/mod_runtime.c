/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
// src/mod_runtime.c
#include "kirton/native.h"
#include "native_internal.h"
#include "kirton/value.h"
#include "kirton/common.h"
#include <stdlib.h>
#include <string.h>

// ==== KIRTON PATCH BEGIN: доп. заголовки для окружения/консоли ====
#include <stdio.h>
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  extern char **environ; /* POSIX */
#endif
// ==== KIRTON PATCH END ====


/* ---------------- argv для языка ---------------- */

static Value g_argv_list;    /* VAL_LIST, элементы — строки */
static int   g_argv_ready = 0;

static void ensure_argv_inited(void){
    if (!g_argv_ready){
        g_argv_list = v_list_new();
        g_argv_ready = 1;
    }
}

/* Вызывается из main.c, чтобы прокинуть системный argv в язык.
   Формат: argv[0] — имя процесса, argv[1] — путь скрипта, argv[2..] — аргументы скрипта. */
void runtime_set_argv(int argc, char** argv){
    ensure_argv_inited();
    for (int i = 0; i < argc; ++i){
        Value s = v_str_copy(argv[i], -1);
        v_list_push(&g_argv_list, s);
    }
}

/* Доступ из скриптов: аргументы() -> список строк */
static Value fn_argv(int argc, const Value* a){
    (void)argc; (void)a;
    ensure_argv_inited();
    return g_argv_list; /* возврат по значению — ok, внутри struct хранит указатель */
}

/* (Опционально) имя программы progname() и имя скрипта scriptname() */
static Value fn_progname(int argc, const Value* a){
    (void)argc; (void)a;
    ensure_argv_inited();
    if (g_argv_list.kind == VAL_LIST && g_argv_list.as.list->len >= 1){
        return g_argv_list.as.list->data[0];
    }
    return v_str_copy("", 0);
}

static Value fn_scriptname(int argc, const Value* a){
    (void)argc; (void)a;
    ensure_argv_inited();
    if (g_argv_list.kind == VAL_LIST && g_argv_list.as.list->len >= 2){
        return g_argv_list.as.list->data[1];
    }
    return v_str_copy("", 0);
}


// ==== KIRTON PATCH BEGIN: тип(x), окружение(), переменная(k), ошибка(msg) ====

/* тип(x) -> "пусто"|"целое"|"число"|"строка"|"список"|"словарь"|"булево" */
static Value fn_typeof(int argc, const Value* a){
    const char* t = "пусто";
    if (argc >= 1){
        switch (a[0].kind){
            case VAL_NIL:  t = "пусто";   break;
            case VAL_NUM:  t = v_num_is_integer(a[0]) ? "целое" : "число"; break;
            case VAL_BOOL: t = "булево";  break;
            case VAL_STR:  t = "строка";  break;
            case VAL_LIST: t = "список";  break;
            case VAL_DICT: t = "словарь"; break;
            case VAL_ARENA: t = "область_памяти"; break;
            case VAL_BUFFER: t = "буфер"; break;
            case VAL_TYPED_ARRAY: t = "типизированный_массив"; break;
            case VAL_RECORD_LAYOUT: t = "структура_памяти"; break;
            case VAL_RECORD: t = "запись_памяти"; break;
            case VAL_NATIVE_RESOURCE: t = "внешний_ресурс"; break;
            case VAL_FUNCTION: t = "функция"; break;
            case VAL_USER_STRUCT: t = "структура"; break;
            case VAL_ENUM_VALUE: t = "перечисление"; break;
        }
    }
    return v_str_copy(t, -1);
}

#ifdef _WIN32
/* helper: wide -> utf8 heap string */
static char* w_to_u8(const wchar_t* w){
    if (!w) return NULL;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char* s = (char*)kir_oom_malloc((size_t)n);
    if (!s) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}
static wchar_t* u8_to_w(const char* s){
    if (!s) return NULL;
    int wn = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (wn <= 0) return NULL;
    size_t bytes=0;
    if (kir_mul_overflow_size((size_t)wn,sizeof(wchar_t),&bytes)) return NULL;
    wchar_t* w = (wchar_t*)kir_oom_malloc(bytes);
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, wn);
    return w;
}
#endif

/* окружение() -> DICT всех пар окружения (UTF-8) */
static Value fn_env(int argc, const Value* a){
    (void)argc; (void)a;
    Value d = v_dict_new();

#ifdef _WIN32
    /* Надёжно читаем в Unicode и конвертируем в UTF-8 */
    LPWCH block = GetEnvironmentStringsW();
    if (!block) return d;
    for (LPWCH p = block; *p; p += wcslen(p) + 1){
        /* Пропускаем строки вида "=C:=C:\..." (служебные для консоли) */
        if (p[0] == L'=') continue;
        char* u8 = w_to_u8(p);
        if (!u8) continue;
        char* eq = strchr(u8, '=');
        if (eq && eq != u8){
            *eq = 0;
            const char* k = u8;
            const char* v = eq + 1;
            v_dict_set(&d, k, strlen(k), v_str_copy(v, -1));
        }
        free(u8);
    }
    FreeEnvironmentStringsW(block);
#else
    if (environ){
        for (char** e = environ; *e; ++e){
            const char* kv = *e;
            const char* eq = strchr(kv, '=');
            if (!eq || eq == kv) continue;
            size_t kn = (size_t)(eq - kv);
            const char* v = eq + 1;
            v_dict_set(&d, kv, kn, v_str_copy(v, -1));
        }
    }
#endif
    return d;
}

/* переменная(ключ) -> STRING|"" */
static Value fn_getenv(int argc, const Value* a){
    if (argc < 1 || a[0].kind != VAL_STR) return v_str_copy("",0);
    const char* key = a[0].as.str.s;

#ifdef _WIN32
    wchar_t* wk = u8_to_w(key);
    if (!wk) return v_str_copy("",0);
    DWORD need = GetEnvironmentVariableW(wk, NULL, 0);
    if (need == 0){
        free(wk);
        return v_str_copy("",0);
    }
    size_t bytes=0;
    if (kir_mul_overflow_size((size_t)need,sizeof(wchar_t),&bytes)){
        free(wk);
        return v_str_copy("",0);
    }
    wchar_t* wv = (wchar_t*)kir_oom_malloc(bytes);
    if (!wv){ free(wk); return v_str_copy("",0); }
    DWORD got = GetEnvironmentVariableW(wk, wv, need);
    free(wk);
    if (got == 0){ free(wv); return v_str_copy("",0); }
    char* u8 = w_to_u8(wv);
    free(wv);
    if (!u8) return v_str_copy("",0);
    Value out = v_str_copy(u8, -1);
    free(u8);
    return out;
#else
    const char* v = getenv(key);
    if (!v) return v_str_copy("",0);
    return v_str_copy(v, -1);
#endif
}

/* ошибка(msg) -> DICT { "ок": ложь, "ошибка": msg } (единый формат ошибок в языке) */
static Value fn_error(int argc, const Value* a){
    const char* msg = (argc>=1 && a[0].kind==VAL_STR) ? a[0].as.str.s : "";
    Value d = v_dict_new();
    v_dict_set(&d, "ок",      strlen("ок"),      v_bool(false));
    v_dict_set(&d, "ошибка",  strlen("ошибка"),  v_str_copy(msg, -1));
    /* В stderr тоже напишем — помогает отладке CLI-скриптов */
    if (msg && msg[0]) fprintf(stderr, "kirton: ошибка: %s\n", msg);
    return d;
}

// ==== KIRTON PATCH END ====


/* Регистрация нативных символов рантайма */
static const KirNativeDef RUNTIME_FUNCS[] = {
    {"аргументы",   fn_argv},
    {"progname",    fn_progname},
    {"scriptname",  fn_scriptname},

    // ==== KIRTON PATCH BEGIN: экспорт новых функций ====
    {"тип",         fn_typeof},
    {"окружение",   fn_env},
    {"переменная",  fn_getenv},
    {"ошибка",      fn_error},
    // ==== KIRTON PATCH END ====
};

void runtime_init_mod(void){
    ensure_argv_inited();
    kir_native_register(RUNTIME_FUNCS, (int)(sizeof(RUNTIME_FUNCS)/sizeof(RUNTIME_FUNCS[0])));
    kir_native_mark_bytecode_safe(RUNTIME_FUNCS, sizeof(RUNTIME_FUNCS)/sizeof(RUNTIME_FUNCS[0]));

    /* ВАЖНО: НИКАКОГО автоматического вызова функций пользователя здесь больше нет.
       Точка входа выбирается только через флаг --entry (или -e/-m) в main.c,
       либо скрипт сам вызывает нужные функции на top-level. */
}
