/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
// src/main.c — точка входа Киртона (обновлён под все добавленные модули)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "kirton/parser.h"
#include "kirton/eval.h"
#include "kirton/bytecode.h"
#include "kirton/native_compiler.h"
#include "kirton/native.h"   // native_init()
#include "kirton/diag.h"

/* --- Прототипы инициализации нативных модулей (реализация в соответствующих src/mod_*.c) --- */

/* ядро коллекций/строк и прочих нативов */
void time_init(void);
void math_init(void);
void math3d_init(void);
void http_init(void);
void process_init(void);
void os_init(void);      // инициализируется внутри native_init, здесь прототип лишним не будет
void path_init(void);    // то же
void str_init(void);     // то же
void list_init(void);
void dict_init(void);
void json_init(void);
void regex_init(void);
void clipboard_init(void);
void timers_init(void);
void native_init(void);
void bitmap_init(void);   // модуль картинок/текстур
void sound_init(void);    // звуковой модуль (многоканальный)
void gl_init(void);       // OpenGL (WGL) биндинги
void d3d11_init(void);    // Direct3D 11 биндинги

/* опциональный старый графический модуль */
#ifdef KIRTON_LEGACY_DRAW2D
void draw2d_init(void);   // старый буфер/BMP (если включён флагом сборки)
#endif

#ifdef _WIN32
void window_init(void);   // оконный модуль (Windows)
#endif

/* runtime: argv и базовый рантайм */
void runtime_init_mod(void);
void runtime_set_argv(int argc, char** argv);

/* -------------------- утилиты -------------------- */

static void* xmalloc(size_t n){
    void* p = malloc(n);
    if (!p){ fprintf(stderr, "kirton: OOM\n"); exit(70); }
    return p;
}

static FILE* fopen_utf8(const char* path, const char* mode){
#ifdef _WIN32
    int path_len=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,NULL,0);
    int mode_len=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,mode,-1,NULL,0);
    if (path_len<=0 || mode_len<=0) return NULL;
    wchar_t* wide_path=(wchar_t*)xmalloc((size_t)path_len*sizeof(*wide_path));
    wchar_t* wide_mode=(wchar_t*)xmalloc((size_t)mode_len*sizeof(*wide_mode));
    if (!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,wide_path,path_len) ||
        !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,mode,-1,wide_mode,mode_len)){
        free(wide_path);
        free(wide_mode);
        return NULL;
    }
    FILE* file=_wfopen(wide_path,wide_mode);
    free(wide_path);
    free(wide_mode);
    return file;
#else
    return fopen(path,mode);
#endif
}

static char* read_file_utf8(const char* path, size_t* out_len){
    FILE* f = fopen_utf8(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0){ fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0){ fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0){ fclose(f); return NULL; }
    char* buf = (char*)xmalloc((size_t)sz + 1);
    size_t r = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (r != (size_t)sz){ free(buf); return NULL; }
    buf[sz] = 0;
    if (out_len) *out_len = (size_t)sz;
    return buf;
}

static void usage(const char* prog){
    fprintf(stderr,
        "Использование: %s [опции] <скрипт.кир> [--] [args...]\n"
        "Опции:\n"
        "  -e, --entry <имя>   Явно вызвать указанную функцию после top-level init\n"
        "  -m <имя>            То же, что --entry (совместимость)\n"
        "  --allow-ffi          Разрешить загрузку доверенных C-библиотек\n"
        "  --bytecode           Выполнить экспериментальный байткодовый прототип\n"
        "  --emit-llvm          [experimental] вывести LLVM IR (требует -o)\n"
        "  --native             [experimental] собрать executable (требует -o и Clang)\n"
        "  --version            Показать версию интерпретатора\n"
        "  -h, --help          Показать эту справку\n"
        "\n"
        "По умолчанию выполняется только верхнеуровневый код файла (без автозапуска функций).\n",
        prog);
}

static void version(void){
#ifndef KIRTON_VERSION
#define KIRTON_VERSION "1.3.0"
#endif
    printf("Kirton %s\n", KIRTON_VERSION);
}

static char** build_lang_argv(int* out_argc, const char* prog, const char* script, int restc, char** restv){
    int n = 2 + restc;
    char** v = (char**)xmalloc((size_t)n * sizeof(char*));
    v[0] = (char*)prog;
    v[1] = (char*)script;
    for (int i = 0; i < restc; ++i) v[2 + i] = restv[i];
    *out_argc = n;
    return v;
}

#ifdef _WIN32
/* ---- Основной вход с поддержкой Unicode/UTF-8 ---- */
int wmain(int argc, wchar_t** wargv){
    /* Включаем UTF-8 кодировки консоли */
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    /* Перекодируем wide argv в UTF-8 */
    char** argv = (char**)xmalloc(argc * sizeof(char*));
    for (int i = 0; i < argc; ++i){
        int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        argv[i] = (char*)xmalloc((size_t)len);
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, argv[i], len, NULL, NULL);
    }
#else
int main(int argc, char** argv){
#endif
    if (argc < 2){ usage(argv[0]); return 64; }

    const char* entry = NULL;
    const char* script = NULL;
    int allow_ffi = 0;
    int use_bytecode = 0;
    enum { MODE_RUN, MODE_EMIT_LLVM, MODE_NATIVE } mode = MODE_RUN;
    const char* native_output = NULL;

    int i = 1;
    for (; i < argc; ++i){
        const char* a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0){
            usage(argv[0]);
            return 0;
        } else if (strcmp(a, "--version") == 0){
            version();
            return 0;
        } else if ((strcmp(a, "-e") == 0) || (strcmp(a, "--entry") == 0) || (strcmp(a, "-m") == 0)){
            if (i + 1 >= argc){
                fprintf(stderr, "Ошибка: %s требует имя функции.\n", a);
                return 64;
            }
            entry = argv[++i];
        } else if (strcmp(a, "--allow-ffi") == 0){
            allow_ffi = 1;
        } else if (strcmp(a, "--bytecode") == 0){
            use_bytecode = 1;
        } else if (strcmp(a, "--emit-llvm") == 0){
            mode = MODE_EMIT_LLVM;
        } else if (strcmp(a, "--native") == 0){
            mode = MODE_NATIVE;
        } else if (strcmp(a, "-o") == 0){
            if (i + 1 >= argc){
                fprintf(stderr, "Ошибка: -o требует путь результата.\n");
                return 64;
            }
            native_output = argv[++i];
        } else if (strcmp(a, "--") == 0){
            ++i; // дальше — аргументы скрипта
            break;
        } else if (a[0] == '-'){
            fprintf(stderr, "Неизвестная опция: %s\n", a);
            return 64;
        } else {
            script = a;
            ++i;
            break;
        }
    }

    if (!script){
        if (i < argc){ script = argv[i++]; }
        if (!script){ usage(argv[0]); return 64; }
    }
    if (mode != MODE_RUN){
        if (!native_output && i < argc && strcmp(argv[i], "-o") == 0 && i + 1 < argc){
            native_output = argv[i + 1];
            i += 2;
        }
        if (!native_output){
            fprintf(stderr, "Ошибка: Native-команды требуют -o <путь>.\n");
            return 64;
        }
        if (i < argc){
            fprintf(stderr, "Ошибка: Native-команды не принимают аргументы программы.\n");
            return 64;
        }
    }
    if (i < argc && strcmp(argv[i], "--") == 0) ++i;

    size_t len = 0;
    char* src = read_file_utf8(script, &len);
    if (!src){
        fprintf(stderr, "Ошибка: не удалось открыть файл: %s\n", script);
        return 66;
    }

    /* --- Инициализация всех нативных модулей (порядок устойчив) --- */

    // Базовые коллекции
    list_init();
    dict_init();

    // Базовые нативы: строки, пути, FS, OS, sys, pkg, str_len и т.п.
    // Всё это делается внутри native_init().
    native_init();

    // Процессы (может использовать os/fs)
    process_init();

    // Время/таймеры
    time_init();
    timers_init();

    // Математика / JSON / Regex / Буфер обмена / HTTP
    math_init();
    math3d_init();
    json_init();
    regex_init();
#ifdef _WIN32
    clipboard_init();
#endif
    http_init();

    // Графика/ресурсы
    bitmap_init();      // загрузка/чтение картинок (BMP)

#ifdef KIRTON_LEGACY_DRAW2D
    draw2d_init();      // опциональный старый 2D
#endif

#ifdef _WIN32
    window_init();      // окно и 2D-рендер через DIB
    gl_init();          // OpenGL 1.1 биндинги
    d3d11_init();       // Direct3D 11 биндинги
#endif

    // Звук (на не-Windows — заглушка, на Windows — полноценный микшер)
    sound_init();

    // Рантайм (argv/entry-политика и т.п.)
    runtime_init_mod();

    // Прокинем скриптовые аргументы в язык: [0]=prog, [1]=script, [2..]=args
    int lang_argc = 0;
    char** lang_argv = build_lang_argv(&lang_argc, argv[0], script, argc - i, &argv[i]);
    runtime_set_argv(lang_argc, lang_argv);
    free(lang_argv); /* runtime_set_argv копирует аргументы */

    // Парсинг и исполнение
    kir_diag_reset_errors();
    AstProgram* progAst = parse_program(src, len, script);
    if (!progAst || kir_diag_error_count() != 0){
        fprintf(stderr, "Ошибка: не удалось распарсить %s\n", script);
        ast_program_destroy(progAst);
        free(src);
        return 65;
    }

    if (mode != MODE_RUN){
        char error[512];
        char* llvm = NULL;
        int needs_runtime=0;
        if (!kirton_llvm_emit_options(progAst, &llvm, &needs_runtime,
                                      allow_ffi, error, sizeof(error))){
            fprintf(stderr, "%s\n", error[0] ? error : "native compiler: unsupported program");
            ast_program_destroy(progAst);
            free(src);
            return 65;
        }
        char* llvm_path = NULL;
        if (mode == MODE_NATIVE){
            size_t n = strlen(native_output);
            llvm_path = (char*)xmalloc(n + sizeof(".kirton-native.ll"));
            memcpy(llvm_path, native_output, n);
            memcpy(llvm_path + n, ".kirton-native.ll", sizeof(".kirton-native.ll"));
        } else {
            llvm_path = (char*)native_output;
        }
        FILE* ir_file = fopen_utf8(llvm_path, "wb");
        int write_ok = ir_file && fwrite(llvm, 1, strlen(llvm), ir_file) == strlen(llvm);
        if (ir_file && fclose(ir_file) != 0) write_ok = 0;
        if (!write_ok){
            fprintf(stderr, "Ошибка: не удалось записать LLVM IR: %s\n", llvm_path);
            if (mode == MODE_NATIVE) free(llvm_path);
            free(llvm);
            ast_program_destroy(progAst);
            free(src);
            return 73;
        }
        int ok = 1;
        if (mode == MODE_NATIVE){
            ok = kirton_native_compile_ex(llvm_path,native_output,needs_runtime,
                                          error,sizeof(error));
            if (!ok) fprintf(stderr, "%s\n", error[0] ? error : "native compiler: Clang failed");
            free(llvm_path);
        }
        free(llvm);
        ast_program_destroy(progAst);
        free(src);
        return ok ? 0 : 69;
    }

    // Явная точка входа (если передана через -e/--entry/-m); иначе — без автозапуска
    eval_set_entry((entry && entry[0]) ? entry : NULL);
    eval_set_ffi_enabled(allow_ffi);

    int rc;
    if (use_bytecode) rc = kirton_bytecode_run_with_ffi(progAst, entry, allow_ffi);
    else rc = eval_program(progAst);
    ast_program_destroy(progAst);

    free(src);
    return rc;
}
