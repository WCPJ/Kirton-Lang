/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
// src/mod_sys.c — системная информация для Киртона под Linux/Arch.
// Экспортирует:
//   система_инфо() -> DICT<string,string>

#include "kirton/native.h"
#include "native_internal.h"
#include "kirton/value.h"

#include <string.h>
#include <stdio.h>

#ifdef __linux__
    #include <sys/utsname.h>
#endif

static Value sys_info(int argc, const Value* argv){
    (void)argc; (void)argv;
    Value dict = v_dict_new();

#ifdef __linux__
    /* --- uname: ядро, система, архитектура --- */
    struct utsname u;
    if (uname(&u) == 0){
        v_dict_set(&dict, "ядро", strlen("ядро"),
                   v_str_copy(u.release, -1));
        v_dict_set(&dict, "система", strlen("система"),
                   v_str_copy(u.sysname, -1));
        v_dict_set(&dict, "архитектура", strlen("архитектура"),
                   v_str_copy(u.machine, -1));
    }

    /* --- /proc/meminfo: память --- */
    FILE* fm = fopen("/proc/meminfo", "r");
    if (fm){
        char line[256];
        long memTotalKB = 0;
        long memAvailKB = 0;

        while (fgets(line, sizeof(line), fm)){
            long v = 0;
            if (sscanf(line, "MemTotal: %ld kB", &v) == 1){
                memTotalKB = v;
            } else if (sscanf(line, "MemAvailable: %ld kB", &v) == 1){
                memAvailKB = v;
            }
        }
        fclose(fm);

        if (memTotalKB > 0){
            char buf[64];
            long mb = memTotalKB / 1024;
            snprintf(buf, sizeof(buf), "%ld", mb);
            v_dict_set(&dict, "память_всего_мб", strlen("память_всего_мб"),
                       v_str_copy(buf, -1));
        }
        if (memAvailKB > 0){
            char buf[64];
            long mb = memAvailKB / 1024;
            snprintf(buf, sizeof(buf), "%ld", mb);
            v_dict_set(&dict, "память_свободно_мб", strlen("память_свободно_мб"),
                       v_str_copy(buf, -1));
        }
    }

    /* --- /proc/uptime: аптайм --- */
    FILE* fu = fopen("/proc/uptime", "r");
    if (fu){
        double up = 0.0;
        if (fscanf(fu, "%lf", &up) == 1){
            char buf[64];
            /* округлим до целых секунд */
            snprintf(buf, sizeof(buf), "%.0f", up);
            v_dict_set(&dict, "аптайм_сек", strlen("аптайм_сек"),
                       v_str_copy(buf, -1));
        }
        fclose(fu);
    }
#else
    /* На не-Linux возвращаем просто пустой словарь */
    (void)dict;
#endif

    return dict;
}

static const KirNativeDef SYS_FUNCS[] = {
    { "система_инфо", sys_info },
};

void sys_init(void){
    kir_native_register_module("sys", SYS_FUNCS,
        (int)(sizeof(SYS_FUNCS)/sizeof(SYS_FUNCS[0])));
    kir_native_register(SYS_FUNCS,
        (int)(sizeof(SYS_FUNCS)/sizeof(SYS_FUNCS[0])));
    kir_native_mark_bytecode_module_safe("sys");
    kir_native_mark_bytecode_safe(SYS_FUNCS, sizeof(SYS_FUNCS)/sizeof(SYS_FUNCS[0]));
}
