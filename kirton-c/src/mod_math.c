/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
// src/mod_math.c — Математический модуль Киртона (расширенный)
// Кодировка: UTF-8 (без BOM)

#include "kirton/native.h"
#include "native_internal.h"
#include "kirton/value.h"
#include "kirton/common.h"

#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <float.h>

/* ========================== ВНУТРЕННИЕ ХЕЛПЕРЫ ========================== */

// PI
static inline double k_pi(void) { return acos(-1.0); }

// log2 / log1p / expm1 / cbrt — совместимые обёртки
static inline double k_log2(double x) {
#if defined(log2)
    return log2(x);
#else
    return log(x) / 0.6931471805599453094; // ln2
#endif
}
static inline double k_log1p(double x) {
#if defined(log1p)
    return log1p(x);
#else
    return log(1.0 + x);
#endif
}
static inline double k_expm1(double x) {
#if defined(expm1)
    return expm1(x);
#else
    return exp(x) - 1.0;
#endif
}
static inline double k_cbrt(double x) {
#if defined(cbrt)
    return cbrt(x);
#else
    return (x >= 0.0) ? pow(x, 1.0/3.0) : -pow(-x, 1.0/3.0);
#endif
}

// hypot
#if defined(_MSC_VER)
  #define k_hypot _hypot
#else
  #define k_hypot hypot
#endif

// tgamma / lgamma — переносимые обёртки
static inline double k_tgamma(double x){
#if defined(tgamma)
    return tgamma(x);
#else
    // минимальная переносимость — не все CRT поддерживают.
    // Можно заменить грубой аппроксимацией, но оставим как есть.
    return tgamma(x);
#endif
}
static inline double k_lgamma(double x){
#if defined(lgamma)
    return lgamma(x);
#else
    // если lgamma недоступна — используем log(tgamma(x))
    double g = k_tgamma(x);
    return (g>0) ? log(g) : NAN;
#endif
}

// erf / erfc — переносимые: используем аппроксимацию Абрамовиц–Стиган (7.1.26), при наличии erf/erfc — напрямую
static inline double k_erf(double x){
#if defined(erf)
    return erf(x);
#else
    // erf(x) ≈ sign * (1 - (((((a5 t + a4)t + a3)t + a2)t + a1)t) * e^{-x^2}))
    // t = 1/(1 + p |x|)
    const double p = 0.3275911;
    const double a1 = 0.254829592, a2 = -0.284496736, a3 = 1.421413741, a4 = -1.453152027, a5 = 1.061405429;
    double s = x < 0 ? -1.0 : 1.0;
    double ax = fabs(x);
    double t = 1.0 / (1.0 + p*ax);
    double y = (((((a5*t + a4)*t + a3)*t + a2)*t + a1)*t);
    return s * (1.0 - y * exp(-ax*ax));
#endif
}
static inline double k_erfc(double x){
#if defined(erfc)
    return erfc(x);
#else
    return 1.0 - k_erf(x);
#endif
}

// fma — если нет аппаратного, делаем обычное
static inline double k_fma(double a, double b, double c){
#if defined(fma)
    return fma(a,b,c);
#else
    return a*b + c;
#endif
}

// RNG
static int seeded = 0;
static void ensure_seed(void){ if(!seeded){ srand((unsigned)time(NULL)); seeded=1; } }

static uint64_t rand_u64(void){
    const unsigned int chunk_bits = RAND_MAX >= 0x7FFFFFFF ? 31u : 15u;
    uint64_t result = 0;
    unsigned int filled = 0;
    while (filled < 64){
        unsigned int take = 64 - filled;
        if (take > chunk_bits) take = chunk_bits;
        uint64_t mask = (1ULL << take) - 1ULL;
        result = (result << take) | ((uint64_t)rand() & mask);
        filled += take;
    }
    return result;
}
/* равномерное double в [0,1) */
static inline double k_urand01(void){
    return (double)rand() / ((double)RAND_MAX + 1.0);
}

/* ============================ БАЗОВАЯ МАТЕМАТИКА ============================ */
/* --- прежние имена/поведение (сохраняем) --- */
static Value m_sin   (int n, const Value* a){ return (n<1||a[0].kind!=VAL_NUM)? v_num(0): v_num(sin(a[0].as.num)); }
static Value m_cos   (int n, const Value* a){ return (n<1||a[0].kind!=VAL_NUM)? v_num(0): v_num(cos(a[0].as.num)); }
static Value m_tan   (int n, const Value* a){ return (n<1||a[0].kind!=VAL_NUM)? v_num(0): v_num(tan(a[0].as.num)); }
static Value m_sqrt  (int n, const Value* a){ return (n<1||a[0].kind!=VAL_NUM)? v_num(0): v_num(sqrt(a[0].as.num)); }
static Value m_pow   (int n, const Value* a){ return (n<2||a[0].kind!=VAL_NUM||a[1].kind!=VAL_NUM)? v_num(0): v_num(pow(a[0].as.num,a[1].as.num)); }
static Value m_abs(int n, const Value* a){
    if (n<1 || a[0].kind!=VAL_NUM) return v_int(0);
    if (v_num_is_integer(a[0])){
        int cmp=0; (void)v_num_compare(a[0],v_int(0),&cmp);
        return cmp<0 ? v_num_neg(a[0]) : a[0];
    }
    return v_num(fabs(a[0].as.num));
}
static Value m_floor(int n, const Value* a){
    if (n<1 || a[0].kind!=VAL_NUM) return v_int(0);
    return v_num_is_integer(a[0]) ? a[0] : v_int_from_double(floor(a[0].as.num));
}
static Value m_ceil(int n, const Value* a){
    if (n<1 || a[0].kind!=VAL_NUM) return v_int(0);
    return v_num_is_integer(a[0]) ? a[0] : v_int_from_double(ceil(a[0].as.num));
}

/* --- расширение тригонометрии --- */
static Value m_asin  (int n, const Value* a){ return (n<1||a[0].kind!=VAL_NUM)? v_num(0): v_num(asin(a[0].as.num)); }
static Value m_acos  (int n, const Value* a){ return (n<1||a[0].kind!=VAL_NUM)? v_num(0): v_num(acos(a[0].as.num)); }
static Value m_atan  (int n, const Value* a){ return (n<1||a[0].kind!=VAL_NUM)? v_num(0): v_num(atan(a[0].as.num)); }
static Value m_atan2 (int n, const Value* a){
    if (n<2 || a[0].kind!=VAL_NUM || a[1].kind!=VAL_NUM) return v_num(0);
    return v_num(atan2(a[0].as.num, a[1].as.num)); // (y, x)
}

/* --- гиперболические --- */
static Value m_sinh  (int n, const Value* a){ return (n<1||a[0].kind!=VAL_NUM)? v_num(0): v_num(sinh(a[0].as.num)); }
static Value m_cosh  (int n, const Value* a){ return (n<1||a[0].kind!=VAL_NUM)? v_num(0): v_num(cosh(a[0].as.num)); }
static Value m_tanh  (int n, const Value* a){ return (n<1||a[0].kind!=VAL_NUM)? v_num(0): v_num(tanh(a[0].as.num)); }
static Value m_asinh (int n, const Value* a){ return (n<1||a[0].kind!=VAL_NUM)? v_num(0): v_num(asinh(a[0].as.num)); }
static Value m_acosh (int n, const Value* a){ return (n<1||a[0].kind!=VAL_NUM)? v_num(0): v_num(acosh(a[0].as.num)); }
static Value m_atanh (int n, const Value* a){ return (n<1||a[0].kind!=VAL_NUM)? v_num(0): v_num(atanh(a[0].as.num)); }

/* --- показательные и логарифмы --- */
static Value m_exp   (int n, const Value* a){ return (n<1||a[0].kind!=VAL_NUM)? v_num(0): v_num(exp(a[0].as.num)); }
static Value m_expm1 (int n, const Value* a){ return (n<1||a[0].kind!=VAL_NUM)? v_num(0): v_num(k_expm1(a[0].as.num)); }
static Value m_log   (int n, const Value* a){ return (n<1||a[0].kind!=VAL_NUM)? v_num(0): v_num(log(a[0].as.num)); }
static Value m_log1p (int n, const Value* a){ return (n<1||a[0].kind!=VAL_NUM)? v_num(0): v_num(k_log1p(a[0].as.num)); }
static Value m_log10 (int n, const Value* a){ return (n<1||a[0].kind!=VAL_NUM)? v_num(0): v_num(log10(a[0].as.num)); }
static Value m_log2  (int n, const Value* a){ return (n<1||a[0].kind!=VAL_NUM)? v_num(0): v_num(k_log2(a[0].as.num)); }

/* --- корни/степени/модуль --- */
static Value m_cbrt  (int n, const Value* a){ return (n<1||a[0].kind!=VAL_NUM)? v_num(0): v_num(k_cbrt(a[0].as.num)); }

/* --- округления и разбиение --- */
static Value m_round(int n, const Value* a){
    if (n<1 || a[0].kind!=VAL_NUM) return v_int(0);
    return v_num_is_integer(a[0]) ? a[0] : v_int_from_double(round(a[0].as.num));
}
static Value m_trunc(int n, const Value* a){
    if (n<1 || a[0].kind!=VAL_NUM) return v_int(0);
    return v_num_is_integer(a[0]) ? a[0] : v_int_from_double(trunc(a[0].as.num));
}
static Value m_intpart (int n, const Value* a){
    if (n<1||a[0].kind!=VAL_NUM) return v_int(0);
    if (v_num_is_integer(a[0])) return a[0];
    double i; (void)modf(a[0].as.num, &i);
    return v_int_from_double(i);
}
static Value m_fracpart (int n, const Value* a){
    if (n<1||a[0].kind!=VAL_NUM) return v_num(0);
    double i; double f = modf(a[0].as.num, &i);
    return v_num(f);
}

/* --- остатки, сравнения, длина/геометрия --- */
static Value m_fmod  (int n, const Value* a){
    if (n<2 || a[0].kind!=VAL_NUM || a[1].kind!=VAL_NUM) return v_num(0);
    return v_num(fmod(a[0].as.num, a[1].as.num));
}
static Value m_min   (int n, const Value* a){
    if (n<2 || a[0].kind!=VAL_NUM || a[1].kind!=VAL_NUM) return v_int(0);
    int cmp=0; (void)v_num_compare(a[0],a[1],&cmp);
    return cmp <= 0 ? a[0] : a[1];
}
static Value m_max   (int n, const Value* a){
    if (n<2 || a[0].kind!=VAL_NUM || a[1].kind!=VAL_NUM) return v_int(0);
    int cmp=0; (void)v_num_compare(a[0],a[1],&cmp);
    return cmp >= 0 ? a[0] : a[1];
}
static Value m_clamp (int n, const Value* a){
    if (n<3 || a[0].kind!=VAL_NUM || a[1].kind!=VAL_NUM || a[2].kind!=VAL_NUM) return v_int(0);
    Value lo=a[1], hi=a[2];
    int cmp=0;
    (void)v_num_compare(lo,hi,&cmp);
    if (cmp > 0){ Value t=lo; lo=hi; hi=t; }
    (void)v_num_compare(a[0],lo,&cmp);
    if (cmp < 0) return lo;
    (void)v_num_compare(a[0],hi,&cmp);
    if (cmp > 0) return hi;
    return a[0];
}
static Value m_hypot2 (int n, const Value* a){
    if (n<2 || a[0].kind!=VAL_NUM || a[1].kind!=VAL_NUM) return v_num(0);
    return v_num(k_hypot(a[0].as.num, a[1].as.num));
}
static Value m_hypot3 (int n, const Value* a){
    if (n<3 || a[0].kind!=VAL_NUM || a[1].kind!=VAL_NUM || a[2].kind!=VAL_NUM) return v_num(0);
    double x=a[0].as.num, y=a[1].as.num, z=a[2].as.num;
    return v_num(sqrt(x*x + y*y + z*z));
}

/* --- знаки и проверки --- */
static Value m_sign  (int n, const Value* a){
    if (n<1||a[0].kind!=VAL_NUM) return v_num(0);
    double x=a[0].as.num;
    if (x==0.0) return v_num(0);
    return v_num(signbit(x) ? -1.0 : 1.0);
}
static Value m_copysign (int n, const Value* a){
    if (n<2 || a[0].kind!=VAL_NUM || a[1].kind!=VAL_NUM) return v_num(0);
    return v_num(copysign(a[0].as.num, a[1].as.num));
}
static Value m_isfinite (int n, const Value* a){
    if (n<1||a[0].kind!=VAL_NUM) return v_bool(false);
    return v_bool( isfinite(a[0].as.num) ? true : false );
}
static Value m_isinf (int n, const Value* a){
    if (n<1||a[0].kind!=VAL_NUM) return v_bool(false);
    return v_bool( isinf(a[0].as.num) ? true : false );
}
static Value m_isnan (int n, const Value* a){
    if (n<1||a[0].kind!=VAL_NUM) return v_bool(false);
    return v_bool( isnan(a[0].as.num) ? true : false );
}

/* --- константы как 0-аргументные функции --- */
static Value m_pi   (int n, const Value* a){ (void)n;(void)a; return v_num(k_pi()); }
static Value m_tau  (int n, const Value* a){ (void)n;(void)a; return v_num(2.0*k_pi()); }
static Value m_e    (int n, const Value* a){ (void)n;(void)a; return v_num(exp(1.0)); }
static Value m_inf  (int n, const Value* a){ (void)n;(void)a; return v_num(INFINITY); }
static Value m_nan  (int n, const Value* a){ (void)n;(void)a; return v_num(NAN); }

/* --- углы и утилиты --- */
static Value m_to_rad (int n, const Value* a){
    if (n<1||a[0].kind!=VAL_NUM) return v_num(0);
    return v_num(a[0].as.num * (k_pi()/180.0));
}
static Value m_to_deg (int n, const Value* a){
    if (n<1||a[0].kind!=VAL_NUM) return v_num(0);
    return v_num(a[0].as.num * (180.0/k_pi()));
}
static Value m_lerp (int n, const Value* a){
    if (n<3 || a[0].kind!=VAL_NUM || a[1].kind!=VAL_NUM || a[2].kind!=VAL_NUM) return v_num(0);
    double A=a[0].as.num, B=a[1].as.num, t=a[2].as.num;
    return v_num(A + (B - A) * t);
}
static Value m_remap (int n, const Value* a){
    if (n<5) return v_num(0);
    if (a[0].kind!=VAL_NUM || a[1].kind!=VAL_NUM || a[2].kind!=VAL_NUM || a[3].kind!=VAL_NUM || a[4].kind!=VAL_NUM) return v_num(0);
    double x=a[0].as.num, a1=a[1].as.num, b1=a[2].as.num, a2=a[3].as.num, b2=a[4].as.num;
    double denom = (b1 - a1);
    if (denom == 0.0) return v_num(a2); // избежим деления на 0
    return v_num(a2 + (x - a1) * (b2 - a2) / denom);
}
static Value m_smoothstep (int n, const Value* a){
    if (n<1||a[0].kind!=VAL_NUM) return v_num(0);
    double t=a[0].as.num;
    if (t<0.0) t=0.0; else if (t>1.0) t=1.0;
    return v_num(t*t*(3.0 - 2.0*t));
}
static Value m_smootherstep (int n, const Value* a){
    if (n<1||a[0].kind!=VAL_NUM) return v_num(0);
    double t=a[0].as.num;
    if (t<0.0) t=0.0; else if (t>1.0) t=1.0;
    return v_num(t*t*t*(t*(6.0*t - 15.0) + 10.0));
}
static Value m_norm_angle_rad (int n, const Value* a){
    if (n<1||a[0].kind!=VAL_NUM) return v_num(0);
    double x=a[0].as.num;
    double twopi = 2.0*k_pi();
    x = fmod(x, twopi);
    if (x <= -k_pi()) x += twopi;
    else if (x > k_pi()) x -= twopi;
    return v_num(x);
}
static Value m_norm_angle_deg (int n, const Value* a){
    if (n<1||a[0].kind!=VAL_NUM) return v_num(0);
    double x=a[0].as.num;
    x = fmod(x, 360.0);
    if (x <= -180.0) x += 360.0;
    else if (x > 180.0) x -= 360.0;
    return v_num(x);
}

/* --- дополнительные утилиты --- */
static Value m_равно_с_точностью (int n, const Value* a){
    if (n<3 || a[0].kind!=VAL_NUM || a[1].kind!=VAL_NUM || a[2].kind!=VAL_NUM) return v_bool(false);
    double x=a[0].as.num, y=a[1].as.num, eps=fabs(a[2].as.num);
    return v_bool(fabs(x-y) <= eps);
}
static Value m_обернуть_в (int n, const Value* a){
    // обернуть значение x в полуинтервал [a, b) (wrap)
    if (n<3 || a[0].kind!=VAL_NUM || a[1].kind!=VAL_NUM || a[2].kind!=VAL_NUM) return v_num(0);
    double x=a[0].as.num, lo=a[1].as.num, hi=a[2].as.num;
    if (hi==lo) return v_num(lo);
    if (hi<lo){ double t=lo; lo=hi; hi=t; }
    double w = hi - lo;
    double t = fmod(x - lo, w);
    if (t < 0) t += w;
    return v_num(lo + t);
}
static Value m_между_ли (int n, const Value* a){
    if (n<3 || a[0].kind!=VAL_NUM || a[1].kind!=VAL_NUM || a[2].kind!=VAL_NUM) return v_bool(false);
    double x=a[0].as.num, lo=a[1].as.num, hi=a[2].as.num;
    if (lo>hi){ double t=lo; lo=hi; hi=t; }
    return v_bool(x>=lo && x<=hi);
}
static Value m_округлить_до (int n, const Value* a){
    if (n<2 || a[0].kind!=VAL_NUM || a[1].kind!=VAL_NUM) return v_num(0);
    double x=a[0].as.num, step=fabs(a[1].as.num);
    if (step==0.0) return v_num(x);
    return v_num(round(x/step)*step);
}
static Value m_пол_до (int n, const Value* a){
    if (n<2 || a[0].kind!=VAL_NUM || a[1].kind!=VAL_NUM) return v_num(0);
    double x=a[0].as.num, step=fabs(a[1].as.num);
    if (step==0.0) return v_num(x);
    return v_num(floor(x/step)*step);
}
static Value m_потолок_до (int n, const Value* a){
    if (n<2 || a[0].kind!=VAL_NUM || a[1].kind!=VAL_NUM) return v_num(0);
    double x=a[0].as.num, step=fabs(a[1].as.num);
    if (step==0.0) return v_num(x);
    return v_num(ceil(x/step)*step);
}

/* =========================== СЛУЧАЙНЫЕ ВЕЛИЧИНЫ =========================== */

static Value m_rand (int n, const Value* a){ (void)n;(void)a; ensure_seed(); return v_num( k_urand01() ); }
static Value m_randint(int n, const Value* a){
    if (n<2 || a[0].kind!=VAL_NUM || a[1].kind!=VAL_NUM) return v_num(0);
    ensure_seed();
    int64_t lo=0, hi=0;
    if (!v_num_fits_i64(a[0], &lo) || !v_num_fits_i64(a[1], &hi)) return v_int(0);
    if (hi < lo){ int64_t t=lo; lo=hi; hi=t; }
    if (hi == lo) return v_int(lo);

    uint64_t span = (uint64_t)hi - (uint64_t)lo + 1ULL;
    uint64_t offset;
    if (span == 0){ /* весь диапазон int64 */
        offset = rand_u64();
    } else {
        uint64_t threshold = (0ULL - span) % span;
        uint64_t random;
        do { random = rand_u64(); } while (random < threshold);
        offset = random % span;
    }
    uint64_t result_bits = (uint64_t)lo + offset;
    int64_t res;
    memcpy(&res, &result_bits, sizeof(res));
    return v_int(res);
}
static Value m_seed(int n, const Value* a){
    if(n>=1 && a[0].kind==VAL_NUM){ srand((unsigned)a[0].as.num); seeded=1; }
    return v_bool(true);
}
static Value m_randrange(int n, const Value* a){
    if (n<2 || a[0].kind!=VAL_NUM || a[1].kind!=VAL_NUM) return v_num(0);
    ensure_seed();
    double lo=a[0].as.num, hi=a[1].as.num;
    if (lo==hi) return v_num(lo);
    if (lo>hi){ double t=lo; lo=hi; hi=t; }
    return v_num(lo + (hi - lo) * k_urand01());
}
static Value m_randnorm(int n, const Value* a){
    if (n<2 || a[0].kind!=VAL_NUM || a[1].kind!=VAL_NUM) return v_num(0);
    ensure_seed();
    double mu=a[0].as.num, sigma=a[1].as.num;
    if (sigma==0.0) return v_num(mu);
    double u1 = ((double)rand() + 1.0) / ((double)RAND_MAX + 2.0); // в (0,1]
    double u2 = k_urand01(); // [0,1)
    double r = sqrt(-2.0 * log(u1));
    double z = r * cos(2.0 * k_pi() * u2); // одно из пары
    return v_num(mu + sigma * z);
}

/* =============================== ПАРСИНГ =============================== */

static Value m_num_parse(int n, const Value* a){
    if (n<1 || a[0].kind!=VAL_STR) return v_int(0);
    const char* s = a[0].as.str.s;
    size_t len = a[0].as.str.n;
    if (!s || len>(size_t)INT_MAX || len==SIZE_MAX) return v_int(0);
    char* tmp = (char*)kir_oom_malloc(len+1);
    if (!tmp) return v_int(0);
    for (size_t i=0;i<len;i++){ char c=s[i]; tmp[i] = (c==',')?'.':c; }
    tmp[len]=0;
    char* start=tmp;
    while (*start && isspace((unsigned char)*start)) ++start;
    char* end=tmp+len;
    while (end>start && isspace((unsigned char)end[-1])) --end;
    *end=0;
    if (end==start){ free(tmp); return v_int(0); }
    size_t value_len=(size_t)(end-start);
    const char* digits = (*start=='+' || *start=='-') ? start+1 : start;
    int is_hex = end-digits >= 3 && digits[0]=='0' && (digits[1]=='x' || digits[1]=='X');
    int is_integer = is_hex || (strchr(start, '.') == NULL && strchr(start, 'e') == NULL && strchr(start, 'E') == NULL);
    if (is_integer){
        Value result = v_int_parse(start, value_len, is_hex ? 16u : 10u);
        free(tmp);
        return result;
    }
    char* parsed_end=NULL;
    double x = strtod(start, &parsed_end);
    int valid = parsed_end == end;
    free(tmp);
    return valid ? v_num(x) : v_int(0);
}

/* ========================== ГАММА / ОШИБКА / FMA ========================== */

static Value m_гамма (int n, const Value* a){
    if (n<1 || a[0].kind!=VAL_NUM) return v_num(NAN);
    return v_num(k_tgamma(a[0].as.num));
}
static Value m_лог_гамма (int n, const Value* a){
    if (n<1 || a[0].kind!=VAL_NUM) return v_num(NAN);
    return v_num(k_lgamma(a[0].as.num));
}
static Value m_ерф (int n, const Value* a){
    if (n<1 || a[0].kind!=VAL_NUM) return v_num(0);
    return v_num(k_erf(a[0].as.num));
}
static Value m_ерфс (int n, const Value* a){
    if (n<1 || a[0].kind!=VAL_NUM) return v_num(0);
    return v_num(k_erfc(a[0].as.num));
}
static Value m_фма (int n, const Value* a){
    if (n<3 || a[0].kind!=VAL_NUM || a[1].kind!=VAL_NUM || a[2].kind!=VAL_NUM) return v_num(0);
    return v_num(k_fma(a[0].as.num, a[1].as.num, a[2].as.num));
}

/* ============================== СТАТИСТИКА ============================== */

static Value m_сумма (int n, const Value* a){
    if (n<1 || a[0].kind!=VAL_LIST) return v_int(0);
    const VList* L = a[0].as.list;
    Value sum = v_int(0);
    for (size_t i=0;i<L->len;i++) if (L->data[i].kind==VAL_NUM) sum = v_num_add(sum, L->data[i]);
    return sum;
}
static Value m_произведение (int n, const Value* a){
    if (n<1 || a[0].kind!=VAL_LIST) return v_int(1);
    const VList* L = a[0].as.list;
    Value product = v_int(1);
    for (size_t i=0;i<L->len;i++) if (L->data[i].kind==VAL_NUM) product = v_num_mul(product, L->data[i]);
    return product;
}
static Value m_среднее (int n, const Value* a){
    if (n<1 || a[0].kind!=VAL_LIST) return v_num(0);
    const VList* L = a[0].as.list;
    double s=0.0; size_t cnt=0;
    for (size_t i=0;i<L->len;i++) if (L->data[i].kind==VAL_NUM){ s += L->data[i].as.num; cnt++; }
    return v_num(cnt? s/cnt : 0.0);
}
static Value m_дисперсия (int n, const Value* a){
    if (n<1 || a[0].kind!=VAL_LIST) return v_num(0);
    const VList* L = a[0].as.list;
    double s=0.0; size_t cnt=0;
    for (size_t i=0;i<L->len;i++) if (L->data[i].kind==VAL_NUM){ s += L->data[i].as.num; cnt++; }
    if (!cnt) return v_num(0);
    double m = s / cnt;
    double ss=0.0;
    for (size_t i=0;i<L->len;i++) if (L->data[i].kind==VAL_NUM){ double d=L->data[i].as.num - m; ss += d*d; }
    return v_num(ss / cnt); // популяционная дисперсия
}
static Value m_ско (int n, const Value* a){
    Value v = m_дисперсия(n,a);
    return v_num( sqrt(v.as.num) );
}
static Value m_минимум_списка (int n, const Value* a){
    if (n<1 || a[0].kind!=VAL_LIST) return v_int(0);
    const VList* L = a[0].as.list;
    Value best=v_int(0); bool any=false;
    for (size_t i=0;i<L->len;i++) if (L->data[i].kind==VAL_NUM){
        int cmp=0;
        if (!any || (v_num_compare(L->data[i],best,&cmp) && cmp<0)){ best=L->data[i]; any=true; }
    }
    return best;
}
static Value m_максимум_списка (int n, const Value* a){
    if (n<1 || a[0].kind!=VAL_LIST) return v_int(0);
    const VList* L = a[0].as.list;
    Value best=v_int(0); bool any=false;
    for (size_t i=0;i<L->len;i++) if (L->data[i].kind==VAL_NUM){
        int cmp=0;
        if (!any || (v_num_compare(L->data[i],best,&cmp) && cmp>0)){ best=L->data[i]; any=true; }
    }
    return best;
}

/* ============================ РЕГИСТРАЦИЯ ============================ */

static const KirNativeDef MATH_FUNCS[] = {
    /* базовые + тригонометрия */
    {"син",    m_sin},   {"кос",       m_cos},    {"тан",        m_tan},
    {"арксин", m_asin},  {"арккос",    m_acos},   {"арктан",     m_atan},
    {"арктан2",m_atan2},

    /* гиперболические */
    {"синх",   m_sinh},  {"косх",      m_cosh},   {"танх",       m_tanh},
    {"арксинх",m_asinh}, {"арккосх",   m_acosh},  {"арктанх",    m_atanh},

    /* корни/степени/модуль */
    {"корень", m_sqrt},  {"кубический_корень", m_cbrt},
    {"степень",m_pow},   {"модуль",    m_abs},

    /* эксп/лог */
    {"эксп",   m_exp},   {"экспм1",    m_expm1},
    {"лог",    m_log},   {"лог1п",     m_log1p},
    {"лог10",  m_log10}, {"лог2",      m_log2},

    /* округления и разбиение */
    {"пол",       m_floor}, {"потолок",    m_ceil},
    {"округлить", m_round}, {"усечь",      m_trunc},
    {"целая_часть", m_intpart}, {"дробная_часть", m_fracpart},
    {"округлить_до", m_округлить_до}, {"пол_до", m_пол_до}, {"потолок_до", m_потолок_до},

    /* остатки, сравнения, геометрия */
    {"остаток", m_fmod}, {"минимум", m_min}, {"максимум", m_max},
    {"ограничить", m_clamp}, {"гипот", m_hypot2}, {"гипот3", m_hypot3},
    {"между_ли", m_между_ли}, {"обернуть_в", m_обернуть_в},
    {"равно_с_точностью", m_равно_с_точностью},

    /* знаки и проверки */
    {"знак", m_sign}, {"копировать_знак", m_copysign},
    {"число_ли", m_isfinite}, {"бесконечность_ли", m_isinf}, {"нечисло_ли", m_isnan},

    /* константы */
    {"пи", m_pi}, {"тау", m_tau}, {"е", m_e}, {"беск", m_inf}, {"нан", m_nan},

    /* углы и утилиты */
    {"в_радианы", m_to_rad}, {"в_градусы", m_to_deg},
    {"линейная_интерполяция", m_lerp}, {"перенести_диапазон", m_remap},
    {"сгладить", m_smoothstep}, {"сгладить2", m_smootherstep},
    {"норм_угол_рад", m_norm_angle_rad}, {"норм_угол_град", m_norm_angle_deg},

    /* случайные */
    {"случайное", m_rand}, {"случайное_целое", m_randint},
    {"случайное_в", m_randrange}, {"случайное_нормальное", m_randnorm},
    {"задать_зерно", m_seed}, {"задасть_зерно", m_seed}, /* алиас для совместимости */

    /* парсинг */
    {"число_из_строки",  m_num_parse},

    /* гамма/ошибка/fma */
    {"гамма", m_гамма}, {"лог_гамма", m_лог_гамма},
    {"ерф", m_ерф}, {"ерфс", m_ерфс}, {"фма", m_фма},

    /* статистика по спискам */
    {"сумма", m_сумма}, {"произведение", m_произведение},
    {"среднее", m_среднее}, {"дисперсия", m_дисперсия}, {"ско", m_ско},
    {"минимум_списка", m_минимум_списка}, {"максимум_списка", m_максимум_списка},
};

void math_init(void){
    kir_native_register(MATH_FUNCS, (int)(sizeof(MATH_FUNCS)/sizeof(MATH_FUNCS[0])));
    kir_native_mark_bytecode_safe(MATH_FUNCS, sizeof(MATH_FUNCS)/sizeof(MATH_FUNCS[0]));
}
