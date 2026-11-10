/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------------
 * Общие макросы
 * ---------------------------------------------------------------------- */

#define KIR_UNUSED(x) (void)(x)

/* Предсказатели ветвлений (микрооптимизация, не критично) */
#if defined(__GNUC__) || defined(__clang__)
  #define KIR_LIKELY(x)   (__builtin_expect(!!(x), 1))
  #define KIR_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#else
  #define KIR_LIKELY(x)   (x)
  #define KIR_UNLIKELY(x) (x)
#endif

/* Удобный макрос длины статического массива */
#define KIR_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* Fatal allocator contract: kir_malloc/kir_realloc/kir_x* report and exit.
   Call kir_oom_* only where the caller can propagate NULL safely. */
static inline void kir_panic_oom(const char* what, size_t n){
    fprintf(stderr, "[kirton] OOM (%s %zu)\n", what ? what : "alloc", n);
    /* Здесь можно добавить cs_dump() при желании:
       cs_dump(); */
    exit(1);
}

static inline void kir_panic_overflow(size_t a, size_t b){
    fprintf(stderr, "[kirton] size overflow (%zu * %zu)\n", a, b);
    exit(1);
}

#if defined(KIRTON_ALLOC_TESTING)
void kir_allocator_test_fail_after(size_t successful_allocations);
int kir_allocator_test_should_fail(void);
#endif

static inline void* kir_oom_malloc(size_t size){
#if defined(KIRTON_ALLOC_TESTING)
    if (kir_allocator_test_should_fail()) return NULL;
#endif
    return malloc(size?size:1);
}

static inline void* kir_oom_realloc(void* memory,size_t size){
#if defined(KIRTON_ALLOC_TESTING)
    if (kir_allocator_test_should_fail()) return NULL;
#endif
    return realloc(memory,size);
}

/* ----------------------------------------------------------------------
 * Fatal allocators. OOM or arithmetic overflow terminates the process.
 *
 * Правила:
 *  - kir_malloc(0) возвращает уникальный валидный указатель (минимум 1 байт).
 *  - kir_realloc(p,0) освобождает p и возвращает NULL.
 * Это позволяет безопасно оперировать «пустыми» массивами.
 * ---------------------------------------------------------------------- */

static inline void* kir_malloc(size_t n) {
    size_t need = (n == 0) ? 1 : n;
#if defined(KIRTON_ALLOC_TESTING)
    void* p = kir_allocator_test_should_fail() ? NULL : malloc(need);
#else
    void* p = malloc(need);
#endif
    if (!p) kir_panic_oom("malloc", n);
    return p;
}

static inline void* kir_realloc(void* p, size_t n) {
    if (n == 0) {
        free(p);
        return NULL;
    }
#if defined(KIRTON_ALLOC_TESTING)
    void* r = kir_allocator_test_should_fail() ? NULL : realloc(p,n);
#else
    void* r = realloc(p,n);
#endif
    if (!r) kir_panic_oom("realloc", n);
    return r;
}

static inline void kir_free(void* p) {
    free(p);
}

/* ----------------------------------------------------------------------
 * Overflow-checked allocators. kir_x* are still fatal on OOM/overflow;
 * kir_oom_calloc is fallible and returns NULL for overflow or OOM.
 * ---------------------------------------------------------------------- */

static inline int kir_mul_overflow_size(size_t a, size_t b, size_t* out){
#if defined(__has_builtin)
#  if __has_builtin(__builtin_mul_overflow)
    return __builtin_mul_overflow(a, b, out);
#  endif
#endif
    if (a == 0 || b == 0) {
        *out = 0;
        return 0;
    }
    if (a > SIZE_MAX / b) return 1;
    *out = a * b;
    return 0;
}

/* Fallible zero-initializing allocation for subsystems with an explicit
   error return path. Unlike kir_callocn, this never terminates the process. */
static inline void* kir_oom_calloc(size_t count,size_t elem_size){
    size_t bytes=0;
    if (kir_mul_overflow_size(count,elem_size,&bytes)) return NULL;
    if (!bytes) bytes=1;
#if defined(KIRTON_ALLOC_TESTING)
    if (kir_allocator_test_should_fail()) return NULL;
#endif
    return calloc(1,bytes);
}

/* Выделяет count * elem_size байт, с проверкой переполнения. */
static inline void* kir_xmallocn(size_t count, size_t elem_size){
    size_t bytes;
    if (kir_mul_overflow_size(count, elem_size, &bytes)){
        kir_panic_overflow(count, elem_size);
    }
    /* соблюдаем правило про нулевые выделения */
    return kir_malloc(bytes);
}

static inline void* kir_xreallocn(void* p, size_t count, size_t elem_size){
    size_t bytes;
    if (kir_mul_overflow_size(count, elem_size, &bytes)){
        kir_panic_overflow(count, elem_size);
    }
    return kir_realloc(p, bytes);
}

/* Эквивалент calloc(count, elem_size) с проверкой переполнения.
   Гарантирует нулевую инициализацию. */
static inline void* kir_callocn(size_t count, size_t elem_size){
    size_t bytes;
    if (kir_mul_overflow_size(count, elem_size, &bytes)){
        kir_panic_overflow(count, elem_size);
    }
    /* calloc(0, x) по стандарту может вернуть NULL — нормализуем как malloc(1)+memset */
    if (bytes == 0) {
        void* p = kir_malloc(1);
        memset(p, 0, 1);
        return p;
    }
#if defined(KIRTON_ALLOC_TESTING)
    void* p = kir_allocator_test_should_fail() ? NULL : calloc(1,bytes);
#else
    void* p = calloc(1, bytes);
#endif
    if (!p) kir_panic_oom("calloc", bytes);
    return p;
}

/* ----------------------------------------------------------------------
 * Удобные дупликаторы
 * ---------------------------------------------------------------------- */

static inline char* kir_strdup(const char* s){
    if (!s) return NULL;
    size_t n = strlen(s);
    char* r = (char*)kir_malloc(n + 1);
    memcpy(r, s, n + 1);
    return r;
}

/* Дубликатор с длиной (не требует NUL-терминатора у src) */
static inline char* kir_strdupn(const char* s, size_t n){
    if (!s && n) return NULL; /* защитимся от явного UB */
    char* r = (char*)kir_malloc(n + 1);
    if (n) memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

static inline void* kir_memdup(const void* src, size_t n){
    if (!src && n) return NULL;
    void* dst = kir_malloc(n);
    if (n) memcpy(dst, src, n);
    return dst;
}

/* ----------------------------------------------------------------------
 * Прочее
 * ---------------------------------------------------------------------- */

#if defined(_MSC_VER)
  #define kir_unreachable() __assume(0)
#elif defined(__GNUC__) || defined(__clang__)
  #define kir_unreachable() __builtin_unreachable()
#else
  #define kir_unreachable() do{}while(0)
#endif

#ifdef __cplusplus
}
#endif
