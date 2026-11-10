/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
// src/native.c
// Универсальная таблица нативных функций + простой back-compat натив "str_len".
#include "kirton/native.h"
#include "native_internal.h"
#include "kirton/mod_path.h"
#include "kirton/value.h"
#include "kirton/diag.h"
#include "kirton/common.h"   /* kir_malloc / kir_xreallocn */
#include <string.h>
#include <stdio.h>

/* ===== Таблица нативных функций ===== */

typedef struct {
    const char*  name;  /* полное имя, например "fs.фс_список" или просто "печать" */
    size_t       n;     /* длина name */
    KirNativeFn  fn;    /* указатель на реализацию */
    int bytecode_safe;
} Slot;

static Slot*  g_slots = NULL;
static size_t g_len   = 0;
static size_t g_cap   = 0;

/* Безопасное расширение массива слотов.
   Используем kir_xreallocn: либо успех, либо аварийный выход при OOM. */
static void ensure_slots(size_t need){
    if (g_cap >= need) return;

    size_t nc = g_cap ? g_cap * 2 : 32;
    if (nc < need) nc = need;

    g_slots = (Slot*)kir_xreallocn(g_slots, nc, sizeof(Slot));
    g_cap   = nc;
}

/* Линейный поиск — таблица небольшая, а регистрируем нечасто. */
static int find_slot(const char* name, size_t n){
    for (size_t i = 0; i < g_len; ++i){
        if (g_slots[i].n == n &&
            memcmp(g_slots[i].name, name, n) == 0)
        {
            return (int)i;
        }
    }
    return -1;
}

/* Вставка слота. name — либо статический литерал, либо заранее выделенная heap-строка.
   В любом случае указатель живёт до конца процесса. */
static void add_slot_raw(const char* name, size_t n, KirNativeFn fn){
    if (!name || n == 0 || !fn) return;

    int at = find_slot(name, n);
    if (at >= 0){
        /* Перерегистрация — просто обновляем fn. */
        g_slots[at].fn = fn;
        return;
    }

    ensure_slots(g_len + 1);

    g_slots[g_len].name = name;
    g_slots[g_len].n    = n;
    g_slots[g_len].fn   = fn;
    g_slots[g_len].bytecode_safe = 0;
    g_len++;
}

void kir_native_mark_bytecode_safe(const KirNativeDef*defs,size_t count){
    if(!defs)return;
    for(size_t i=0;i<count;i++)if(defs[i].name){int at=find_slot(defs[i].name,strlen(defs[i].name));if(at>=0)g_slots[at].bytecode_safe=1;}
}
void kir_native_mark_bytecode_module_safe(const char*module){
    if(!module||!*module)return;size_t n=strlen(module);
    for(size_t i=0;i<g_len;i++)if(g_slots[i].n>n+1&&!memcmp(g_slots[i].name,module,n)&&g_slots[i].name[n]=='.')g_slots[i].bytecode_safe=1;
}
int kir_native_bytecode_module_is_safe(const char*module){
    if(!module||!*module)return 0;size_t n=strlen(module);
    for(size_t i=0;i<g_len;i++)if(g_slots[i].bytecode_safe&&g_slots[i].n>n+1&&!memcmp(g_slots[i].name,module,n)&&g_slots[i].name[n]=='.')return 1;
    return 0;
}
int kir_native_bytecode_slot(const char*name,size_t name_len){
    if(!name)return -1;if(!name_len)name_len=strlen(name);int at=find_slot(name,name_len);return at>=0&&g_slots[at].bytecode_safe?at:-1;
}
int kir_native_has_function(const char*name,size_t name_len){
    if(!name)return 0;if(!name_len)name_len=strlen(name);return find_slot(name,name_len)>=0;
}
int kir_native_call_bytecode_slot(int slot,int argc,const Value*argv,Value*out){
    if(slot<0||(size_t)slot>=g_len||!g_slots[slot].bytecode_safe)return 0;Value result=g_slots[slot].fn(argc,argv);if(out)*out=result;return 1;
}

/* Публичная API: регистрирует набор функций с ИМЕНАМИ КАК ЕСТЬ (без префикса модуля).
   Предполагается, что defs[i].name — статические литералы (мы их не копируем). */
void kir_native_register(const KirNativeDef* defs, size_t count){
    if (!defs || count == 0) return;

    for (size_t i = 0; i < count; ++i){
        const char* nm = defs[i].name;
        if (!nm || nm[0] == '\0' || !defs[i].fn) continue;

        size_t n = strlen(nm);
        add_slot_raw(nm, n, defs[i].fn);
    }
}

/* Регистрация с префиксом модуля: для каждой записи формируем "module.name".
   Здесь мы ВЫДЕЛЯЕМ новую строку (heap) и кладём её в таблицу навсегда (до завершения процесса). */
void kir_native_register_module(const char* module, const KirNativeDef* defs, size_t count){
    if (!module || !module[0] || !defs || count == 0) return;

    const size_t lm = strlen(module);

    for (size_t i = 0; i < count; ++i){
        const char* nm = defs[i].name;
        if (!nm || nm[0] == '\0' || !defs[i].fn) continue;

        const size_t ln    = strlen(nm);
        const size_t total = lm + 1 + ln; /* "mod" + '.' + "name" */

        char* fullname = (char*)kir_malloc(total + 1);
        memcpy(fullname, module, lm);
        fullname[lm] = '.';
        memcpy(fullname + lm + 1, nm, ln);
        fullname[total] = '\0';

        /* fullname — теперь собственность таблицы (живет до конца процесса). */
        add_slot_raw(fullname, total, defs[i].fn);
    }
}

/* Вызов нативной функции по полному имени.
   Возвращает 1 при успехе, 0 если не найдено. */
int kir_native_call(const char* name, size_t name_len,
                    int argc, const Value* argv, Value* out){
    if (!name) return 0;
    if (name_len == 0) name_len = strlen(name);

    for (size_t i = 0; i < g_len; ++i){
        if (g_slots[i].n == name_len &&
            memcmp(g_slots[i].name, name, name_len) == 0)
        {
            Value res = g_slots[i].fn(argc, argv);
            if (out) *out = res;
            return 1;
        }
    }
    return 0;
}

/* �������� ������� ��������� ������ �� �������� ("mod.*"). */
int kir_native_has_module(const char* module){
    if (!module || !module[0]) return 0;
    size_t lm = strlen(module);

    for (size_t i = 0; i < g_len; ++i){
        const char* nm = g_slots[i].name;
        size_t n = g_slots[i].n;
        if (n <= lm + 1) continue;          /* ������� "mod.x" */
        if (memcmp(nm, module, lm) != 0) continue;
        if (nm[lm] != '.') continue;        /* ����� �������� ������ ���� ����� */
        return 1;
    }
    return 0;
}

/* �������� ������: ��������� "alias.func" ��� ���� "module.func". */
void kir_native_alias_module(const char* module, const char* alias){
    if (!module || !module[0] || !alias || !alias[0]) return;

    size_t lm = strlen(module);
    size_t la = strlen(alias);

    for (size_t i = 0; i < g_len; ++i){
        const char* nm = g_slots[i].name;
        size_t n = g_slots[i].n;
        if (n <= lm + 1) continue;
        if (memcmp(nm, module, lm) != 0) continue;
        if (nm[lm] != '.') continue;

        const char* local = nm + lm + 1; /* "func" */
        size_t ll = n - (lm + 1);

        size_t total = la + 1 + ll;      /* "alias.func" */
        char* fullname = (char*)kir_malloc(total + 1);
        memcpy(fullname, alias, la);
        fullname[la] = '.';
        memcpy(fullname + la + 1, local, ll);
        fullname[total] = '\0';

        add_slot_raw(fullname, total, g_slots[i].fn);
    }
}

/* --------------------------
   Доп. нативы: совместимость
   -------------------------- */

/* str_len(s) -> number (length)  - добавлен для совместимости со скриптами, использующими "str_len" */
static Value native_str_len(int argc, const Value* argv){
    if (argc < 1) return v_num(0);
    if (argv[0].kind != VAL_STR) return v_num(0);
    /* длина строки хранится в argv[0].as.str.n */
    return v_int((int64_t)argv[0].as.str.n);
}

/* --------------------------
   Инициализация встроенных нативов
   -------------------------- */

/* Объявляем инициализаторы модулей (реализации в других единицах компиляции) */
void path_init(void);
void str_init(void);
void fs_init(void);
void os_init(void);

/* НОВЫЕ модули для KirtonOS на Linux/Arch */
void sys_init(void);  /* модуль sys: система_инфо() */
void pkg_init(void);
void memory_init(void);
void native_resource_init(void);
static Value foreign_stub(int argc,const Value* args){
    (void)argc; (void)args;
    kir_diag_error("<внешний>",1,1,
        "Сначала импортируйте модуль внешний: импорт внешний");
    return v_nil();
}
static void foreign_init(void){
    static const KirNativeDef defs[]={
        {"загрузить",foreign_stub}, {"вызвать",foreign_stub}
    };
    kir_native_register_module("внешний",defs,sizeof(defs)/sizeof(defs[0]));
    kir_native_mark_bytecode_module_safe("внешний");
}  /* модуль pkg: пакеты_* для pacman */

void native_init(void){
    path_init();  // path.*
    fs_init();    // fs.* и фс_*
    os_init();    // os.* и окружение/текущая_папка и т.п.

    /* системная инфа и pacman-обёртка */
    sys_init();   // sys.система_инфо и глобальная система_инфо
    memory_init();
    native_resource_init();
    foreign_init();
    pkg_init();   // pkg.пакеты_* и глобальные пакеты_*

    static const KirNativeDef extra[] = {
        { "str_len", native_str_len },
    };
    kir_native_register(extra, KIR_ARRAY_LEN(extra));
    kir_native_mark_bytecode_safe(extra, KIR_ARRAY_LEN(extra));

    str_init();   // строковые нативы
}
