/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
// src/mod_time.c — модуль времени Киртона (кроссплатформенный)
// Кодировка: UTF-8 (без BOM)

#include "kirton/native.h"
#include "native_internal.h"
#include "kirton/value.h"

#include <time.h>
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <unistd.h>
  #include <sys/time.h>
  #include <errno.h>
#endif

/* ======================= ВНУТРЕННИЕ ХЕЛПЕРЫ ======================= */

// Текущая unix-метка в секундах (double)
static double now_unix_sec(void){
#if defined(_WIN32)
    // time() даёт секунды; точности достаточно для общего случая
    return (double)time(NULL);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec/1e6;
#endif
}

// Текущая unix-метка в миллисекундах (double)
static double now_unix_ms(void){
#if defined(_WIN32)
    // GetSystemTimePreciseAsFileTime доступен начиная с Win8; fallback на GetSystemTimeAsFileTime
    FILETIME ft;
    #if defined(NTDDI_WIN8) && (NTDDI_VERSION >= NTDDI_WIN8)
    GetSystemTimePreciseAsFileTime(&ft);
    #else
    GetSystemTimeAsFileTime(&ft);
    #endif
    ULARGE_INTEGER t; t.LowPart = ft.dwLowDateTime; t.HighPart = ft.dwHighDateTime;
    // FILETIME: 100-нс с 1601-01-01. Переведём в мс и вычтем смещение до 1970-01-01.
    const unsigned long long EPOCH_DIFF_100NS = 116444736000000000ULL;
    unsigned long long t100 = (t.QuadPart - EPOCH_DIFF_100NS); // 100ns с 1970
    return (double)(t100 / 10000ULL);
#else
    struct timespec ts;
    #ifdef CLOCK_REALTIME
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
    #else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1e3;
    #endif
#endif
}

// Монотонные часы: секунды (double)
static double mono_sec(void){
#if defined(_WIN32)
    static LARGE_INTEGER freq = {0};
    static int inited = 0;
    if (!inited){
        QueryPerformanceFrequency(&freq);
        inited = 1;
    }
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    #ifdef CLOCK_MONOTONIC
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec/1e9;
    #else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec/1e6; // нестрого монотонно, но переносимо
    #endif
#endif
}

// Монотонные часы: миллисекунды (double)
static double mono_ms(void){
    return mono_sec() * 1000.0;
}

// Короткий сон в миллисекундах
static void sleep_ms(double ms){
    if (ms <= 0) return;
#ifdef _WIN32
    DWORD d = (DWORD)(ms + 0.5);
    Sleep(d);
#else
    // nanosleep — точнее и не ограничен 1e6 микросекунд как usleep
    struct timespec req;
    req.tv_sec  = (time_t)(ms / 1000.0);
    req.tv_nsec = (long)((ms - (double)req.tv_sec*1000.0) * 1e6 * 1000.0);
    if (req.tv_nsec >= 1000000000L){ req.tv_sec += 1; req.tv_nsec -= 1000000000L; }
    while (nanosleep(&req, &req) == -1 && errno == EINTR) {}
#endif
}

/* ========================== ПОЛЕЗНЫЕ УТИЛИТЫ ========================== */

// форматировать_время(сек_с_эпохи, "strftime-паттерн", utc_ли)
// Примеры паттернов: "%Y-%m-%d", "%H:%M:%S", "%Y-%m-%dT%H:%M:%S"
static Value fn_форматировать_время(int argc, const Value* a){
    if (argc < 2 || a[0].kind != VAL_NUM || a[1].kind != VAL_STR) return v_str_copy("",0);
    double sec = a[0].as.num;
    const char* pat = a[1].as.str.s;
    int use_utc = 0;
    if (argc >= 3 && a[2].kind == VAL_NUM) use_utc = v_truthy(a[2]);

    time_t t = (time_t)sec;
    struct tm tmv;
#if defined(_WIN32)
    if (use_utc){
        gmtime_s(&tmv, &t);
    } else {
        localtime_s(&tmv, &t);
    }
#else
    if (use_utc){
        gmtime_r(&t, &tmv);
    } else {
        localtime_r(&t, &tmv);
    }
#endif
    char buf[128];
    size_t n = strftime(buf, sizeof(buf), pat, &tmv);
    if (n==0) return v_str_copy("",0);
    return v_str_copy(buf, (int)n);
}

/* ============================ ЭКСПОРТИРУЕМЫЕ ФУНКЦИИ ============================ */

// время_сейчас() -> локальная дата/время ISO "YYYY-MM-DDTHH:MM:SS"
static Value time_now(int argc, const Value* argv){
    (void)argc; (void)argv;
    time_t t = time(NULL);
    struct tm lt;
#if defined(_WIN32)
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    char buf[32];
    if (strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &lt)){
        return v_str_copy(buf, (int)strlen(buf));
    }
    return v_str_copy("",0);
}

// время_сейчас_utc() -> ISO UTC "YYYY-MM-DDTHH:MM:SSZ"
static Value time_now_utc(int argc, const Value* argv){
    (void)argc; (void)argv;
    time_t t = time(NULL);
    struct tm gt;
#if defined(_WIN32)
    gmtime_s(&gt, &t);
#else
    gmtime_r(&t, &gt);
#endif
    char buf[32];
    if (strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &gt)){
        return v_str_copy(buf, (int)strlen(buf));
    }
    return v_str_copy("",0);
}

// эпоха_сек() -> число (double) секунд с 1970-01-01T00:00:00Z
static Value fn_эпоха_сек(int argc, const Value* a){
    (void)argc; (void)a;
    return v_num(now_unix_sec());
}

// эпоха_мс() -> число (double) миллисекунд с 1970-01-01T00:00:00Z
static Value fn_эпоха_мс(int argc, const Value* a){
    (void)argc; (void)a;
    return v_num(now_unix_ms());
}

// монотонное_сек() -> число (double), только возрастает, не зависит от системного времени
static Value fn_монотонное_сек(int argc, const Value* a){
    (void)argc; (void)a;
    return v_num(mono_sec());
}

// монотонное_мс() -> число (double)
static Value fn_монотонное_мс(int argc, const Value* a){
    (void)argc; (void)a;
    return v_num(mono_ms());
}

// ждать(секунды) -> bool
static Value time_sleep(int argc, const Value* argv){
    if (argc<1 || argv[0].kind!=VAL_NUM) return v_bool(false);
    double s = argv[0].as.num;
#ifdef _WIN32
    Sleep((DWORD)(s*1000.0));
#else
    sleep_ms(s*1000.0);
#endif
    return v_bool(true);
}

// ждать_мс(миллисекунды) -> bool
static Value time_sleep_ms(int argc, const Value* argv){
    if (argc<1 || argv[0].kind!=VAL_NUM) return v_bool(false);
    double ms = argv[0].as.num;
    sleep_ms(ms);
    return v_bool(true);
}

// таймер_старт() -> метка_мс (double, монотонные мс)
static Value fn_таймер_старт(int argc, const Value* a){
    (void)argc; (void)a;
    return v_num(mono_ms());
}

// таймер_прошло(метка_мс) -> прошедшие_мс (double, монотонно)
static Value fn_таймер_прошло(int argc, const Value* a){
    if (argc<1 || a[0].kind!=VAL_NUM) return v_num(0);
    double start = a[0].as.num;
    double now = mono_ms();
    return v_num(now - start);
}

/* ============================ РЕГИСТРАЦИЯ ============================ */

static const KirNativeDef TIME_FUNCS[] = {
    // базовые
    {"время_сейчас",        time_now},
    {"время_сейчас_utc",    time_now_utc},
    {"эпоха_сек",           fn_эпоха_сек},
    {"эпоха_мс",            fn_эпоха_мс},

    // монотонные таймеры
    {"монотонное_сек",      fn_монотонное_сек},
    {"монотонное_мс",       fn_монотонное_мс},

    // задержки
    {"ждать",               time_sleep},
    {"ждать_мс",            time_sleep_ms},

    // простой «таймер»
    {"таймер_старт",        fn_таймер_старт},
    {"таймер_прошло",       fn_таймер_прошло},

    // форматирование времени
    {"форматировать_время", fn_форматировать_время},
};

void time_init(void){ // вызывается из main.c
    kir_native_register(TIME_FUNCS, (int)(sizeof(TIME_FUNCS)/sizeof(TIME_FUNCS[0])));
    kir_native_mark_bytecode_safe(TIME_FUNCS, (int)(sizeof(TIME_FUNCS)/sizeof(TIME_FUNCS[0])));
}

/* --- Совместимость: старое имя функции регистрации, если где-то ожидается time_init_mod --- */
void time_init_mod(void){
    time_init();
}
