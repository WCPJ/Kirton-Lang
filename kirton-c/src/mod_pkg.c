/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
// src/mod_pkg.c — обёртка над pacman для Arch.
// Функции:
//   пакеты_установить(name1, name2, ...) -> BOOL
//   пакеты_удалить(name1, name2, ...)    -> BOOL
//   пакеты_поиск(query)                  -> LIST<STRING> (строки вывода pacman -Ss)
//
// Реализация использует system()/popen(), поэтому работать будет только под Linux/Arch.

#include "kirton/native.h"
#include "native_internal.h"
#include "kirton/value.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef __linux__

/* Собрать команду вида "pacman <opts> name1 name2 ..." */
static char* build_pacman_cmd(const char* base, int argc, const Value* argv){
    size_t len = strlen(base);
    for (int i = 0; i < argc; ++i){
        if (argv[i].kind != VAL_STR) continue;
        len += 1 + (size_t)argv[i].as.str.n;
    }
    char* cmd = (char*)malloc(len + 1);
    if (!cmd) return NULL;
    strcpy(cmd, base);
    for (int i = 0; i < argc; ++i){
        if (argv[i].kind != VAL_STR) continue;
        strcat(cmd, " ");
        strncat(cmd, argv[i].as.str.s, argv[i].as.str.n);
    }
    cmd[len] = '\0';
    return cmd;
}

/* пакеты_установить(...) */
static Value pkg_install(int argc, const Value* argv){
    if (argc < 1) return v_bool(false);
    char* cmd = build_pacman_cmd("pacman -S --noconfirm", argc, argv);
    if (!cmd) return v_bool(false);
    int rc = system(cmd);
    free(cmd);
    return v_bool(rc == 0);
}

/* пакеты_удалить(...) */
static Value pkg_remove(int argc, const Value* argv){
    if (argc < 1) return v_bool(false);
    char* cmd = build_pacman_cmd("pacman -Rns --noconfirm", argc, argv);
    if (!cmd) return v_bool(false);
    int rc = system(cmd);
    free(cmd);
    return v_bool(rc == 0);
}

/* пакеты_поиск(query) -> LIST строк (как pacman -Ss выводит) */
static Value pkg_search(int argc, const Value* argv){
    if (argc < 1 || argv[0].kind != VAL_STR) return v_list_new();

    /* команда: pacman -Ss <query> */
    size_t base_len = strlen("pacman -Ss ");
    size_t len = base_len + (size_t)argv[0].as.str.n;
    char* cmd = (char*)malloc(len + 1);
    if (!cmd) return v_list_new();
    memcpy(cmd, "pacman -Ss ", base_len);
    memcpy(cmd + base_len, argv[0].as.str.s, argv[0].as.str.n);
    cmd[len] = '\0';

    FILE* f = popen(cmd, "r");
    free(cmd);
    Value list = v_list_new();
    if (!f) return list;

    char line[1024];
    while (fgets(line, sizeof(line), f)){
        /* убираем конечный '\n' */
        size_t n = strlen(line);
        if (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r'))
            line[--n] = '\0';
        v_list_push(&list, v_str_copy(line, -1));
    }
    pclose(f);
    return list;
}

#else  /* !__linux__ */

static Value pkg_install(int argc, const Value* argv){
    (void)argc; (void)argv;
    return v_bool(false);
}
static Value pkg_remove(int argc, const Value* argv){
    (void)argc; (void)argv;
    return v_bool(false);
}
static Value pkg_search(int argc, const Value* argv){
    (void)argc; (void)argv;
    return v_list_new();
}

#endif /* __linux__ */

static const KirNativeDef PKG_FUNCS[] = {
    { "пакеты_установить", pkg_install },
    { "пакеты_удалить",    pkg_remove  },
    { "пакеты_поиск",      pkg_search  },
};

void pkg_init(void){
    kir_native_register_module("pkg", PKG_FUNCS,
        (int)(sizeof(PKG_FUNCS)/sizeof(PKG_FUNCS[0])));
    kir_native_register(PKG_FUNCS,
        (int)(sizeof(PKG_FUNCS)/sizeof(PKG_FUNCS[0])));
    kir_native_mark_bytecode_module_safe("pkg");
    kir_native_mark_bytecode_safe(PKG_FUNCS, sizeof(PKG_FUNCS)/sizeof(PKG_FUNCS[0]));
}
