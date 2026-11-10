/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
// src/mod_timers.c — Таймеры для Киртона (монотонные, кроссплатформенные).
//
// Функции языка (все имена на кириллице):
//   текущее_время_мс()                 -> NUM    // монотонные миллисекунды (double)
//   таймер_после(миллисек)             -> NUM    // создаёт одноразовый таймер; возвращает id (>0) или 0 при ошибке
//   таймер_каждый(миллисек)            -> NUM    // создаёт циклический таймер (интервал); возвращает id (>0) или 0
//   таймер_отменить(id)                -> BOOL   // снять таймер
//   таймеры_шаг()                      -> LIST<NUM> // опрашивает таймеры, возвращает список ids сработавших таймеров
//   таймер_следующее_ожидание()        -> NUM    // мс до ближайшего срабатывания, -1 если таймеров нет
//   таймер_инфо(id)                    -> DICT   // {id, период_мс, повтор, активен, следующий_время}
//   таймеры_очистить()                 -> BOOL   // удалить все таймеры
//
// Замечание по модели исполнения:
//   Таймеры НЕ запускают пользовательские функции сами по себе. Скрипт должен периодически
//   вызывать таймеры_шаг() (например, внутри собственного цикла или перед/после window.живо_шаг()).
//   Получив список сработавших id, скрипт может сам вызвать нужные обработчики.
//
// Требует: kirton/native.h, kirton/value.h

#include "kirton/native.h"
#include "native_internal.h"
#include "kirton/value.h"
#include "kirton/common.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include <math.h>

/* ========================== монотонное время (мс) ========================== */

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
static double mono_ms(void){
    static LARGE_INTEGER qpf = {0};
    if (!qpf.QuadPart) QueryPerformanceFrequency(&qpf);
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    return 1000.0 * (double)now.QuadPart / (double)qpf.QuadPart;
}
#else
  #include <time.h>
  #include <sys/time.h>
static double mono_ms(void){
#if defined(CLOCK_MONOTONIC)
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
#else
    struct timeval tv; gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
#endif
}
#endif

/* =============================== структура таймера =============================== */

typedef struct {
    int     id;            // >0 если занят
    double  period_ms;     // >0
    int     repeat;        // 0=одноразовый, 1=интервальный
    int     active;        // 0/1
    double  next_due_ms;   // абсолютное монотонное время (мс), когда сработает
} KTimer;

static KTimer* g_timers = NULL;
static int     g_len = 0;
static int     g_cap = 0;
static int     g_next_id = 1;

static void ensure_cap(int need){
    if (need <= g_cap) return;
    int nc;
    if (!g_cap) nc=16;
    else if (g_cap<=INT_MAX/2) nc=g_cap*2;
    else nc=need;
    if (nc < need) nc = need;
    size_t bytes=0;
    if (kir_mul_overflow_size((size_t)nc,sizeof(KTimer),&bytes)) return;
    KTimer* nt = (KTimer*)kir_oom_realloc(g_timers,bytes);
    if (!nt) return;
    g_timers = nt; g_cap = nc;
}

static int idx_by_id(int id){
    for (int i=0;i<g_len;i++) if (g_timers[i].id == id) return i;
    return -1;
}

static int new_timer(double delay_ms, int repeat){
    if (!(delay_ms > 0.0)) return 0;
    if (g_len==INT_MAX) return 0;
    ensure_cap(g_len + 1);
    if (g_len >= g_cap) return 0;
    int id = g_next_id++;
    if (g_next_id <= 0) g_next_id = 1; // переполнение — перезапустим
    KTimer t;
    t.id = id;
    t.period_ms = delay_ms;
    t.repeat = repeat ? 1 : 0;
    t.active = 1;
    t.next_due_ms = mono_ms() + delay_ms;
    g_timers[g_len++] = t;
    return id;
}

static void remove_at(int i){
    if (i < 0 || i >= g_len) return;
    g_len--;
    if (i != g_len) g_timers[i] = g_timers[g_len];
}

/* =============================== экспортируемые функции языка =============================== */

// текущее_время_мс()
static Value fn_now_ms(int argc, const Value* a){
    (void)argc; (void)a;
    return v_num(mono_ms());
}

// таймер_после(ms) -> id
static Value fn_after(int argc, const Value* a){
    if (argc<1 || a[0].kind!=VAL_NUM) return v_int(0);
    double ms = a[0].as.num;
    int id = new_timer(ms, 0);
    return v_int(id);
}

// таймер_каждый(ms) -> id
static Value fn_every(int argc, const Value* a){
    if (argc<1 || a[0].kind!=VAL_NUM) return v_int(0);
    double ms = a[0].as.num;
    int id = new_timer(ms, 1);
    return v_int(id);
}

// таймер_отменить(id) -> bool
static Value fn_cancel(int argc, const Value* a){
    if (argc<1 || a[0].kind!=VAL_NUM) return v_bool(false);
    int64_t id64=0;
    if (!v_num_fits_i64(a[0], &id64) || id64 < 0 || id64 > INT_MAX) return v_bool(false);
    int id = (int)id64;
    int i = idx_by_id(id);
    if (i<0) return v_bool(false);
    remove_at(i);
    return v_bool(true);
}

// таймеры_шаг() -> LIST<id> (сработавшие)
// Для повторяющихся корректно «догоняем» пропущенные интервалы без накопления лавины.
static Value fn_poll(int argc, const Value* a){
    (void)argc; (void)a;
    Value L = v_list_new();
    double now = mono_ms();

    // Однопроходный обход: если сработал одноразовый — удаляем; если повторный — переносим next_due
    // Чтобы безопасно удалять, пройдёмся с конца к началу.
    for (int i = g_len - 1; i >= 0; --i){
        KTimer* t = &g_timers[i];
        if (!t->active) continue;
        if (now + 1e-9 >= t->next_due_ms){
            // Сработал
            v_list_push(&L, v_int(t->id));
            if (t->repeat){
                // Сместим на кратное количество периодов, чтобы «догнать» время и не залипнуть позади now
                if (t->period_ms <= 0.0) t->period_ms = 1.0;
                double behind = now - t->next_due_ms;
                double steps = floor(behind / t->period_ms) + 1.0; // минимум 1 шаг вперёд
                if (steps < 1.0) steps = 1.0;
                t->next_due_ms += steps * t->period_ms;
            } else {
                // Одноразовый — удаляем
                remove_at(i);
            }
        }
    }
    return L;
}

// таймер_следующее_ожидание() -> NUM (мс до ближайшего, -1 если нет таймеров)
static Value fn_next_wait(int argc, const Value* a){
    (void)argc; (void)a;
    if (g_len == 0) return v_num(-1);
    double now = mono_ms();
    double best = 1e300;
    int found = 0;
    for (int i=0;i<g_len;i++){
        if (!g_timers[i].active) continue;
        double d = g_timers[i].next_due_ms - now;
        if (d < 0.0) d = 0.0;
        if (!found || d < best){ best = d; found = 1; }
    }
    if (!found) return v_num(-1);
    return v_num(best);
}

// таймер_инфо(id) -> DICT
static Value fn_info(int argc, const Value* a){
    if (argc<1 || a[0].kind!=VAL_NUM){
        Value d = v_dict_new();
        v_dict_set(&d, "ошибка", 12, v_str_copy("нет id", -1));
        return d;
    }
    int64_t id64=0;
    if (!v_num_fits_i64(a[0], &id64) || id64 < 0 || id64 > INT_MAX){
        Value d = v_dict_new();
        v_dict_set(&d, "ошибка", 12, v_str_copy("неверный id", -1));
        return d;
    }
    int id = (int)id64;
    int i = idx_by_id(id);
    if (i<0){
        Value d = v_dict_new();
        v_dict_set(&d, "ошибка", 12, v_str_copy("не найден", -1));
        return d;
    }
    KTimer* t = &g_timers[i];
    Value d = v_dict_new();
    v_dict_set(&d, "id",              2,  v_int(t->id));
    v_dict_set(&d, "период_мс",       16, v_num(t->period_ms));
    v_dict_set(&d, "повтор",          12, v_bool(t->repeat?true:false));
    v_dict_set(&d, "активен",         12, v_bool(t->active?true:false));
    v_dict_set(&d, "следующий_время", 28, v_num(t->next_due_ms));
    return d;
}

// таймеры_очистить() -> BOOL
static Value fn_clear_all(int argc, const Value* a){
    (void)argc; (void)a;
    free(g_timers); g_timers=NULL; g_len=g_cap=0; g_next_id=1;
    return v_bool(true);
}

/* =============================== регистрация =============================== */

static const KirNativeDef TIMER_FUNCS[] = {
    {"текущее_время_мс",         fn_now_ms},
    {"таймер_после",             fn_after},
    {"таймер_каждый",            fn_every},
    {"таймер_отменить",          fn_cancel},
    {"таймеры_шаг",              fn_poll},
    {"таймер_следующее_ожидание",fn_next_wait},
    {"таймер_инфо",              fn_info},
    {"таймеры_очистить",         fn_clear_all},
};

void timers_init(void){
    kir_native_register(TIMER_FUNCS, (int)(sizeof(TIMER_FUNCS)/sizeof(TIMER_FUNCS[0])));
    kir_native_mark_bytecode_safe(TIMER_FUNCS, sizeof(TIMER_FUNCS)/sizeof(TIMER_FUNCS[0]));
}
