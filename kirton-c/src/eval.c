/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
// src/eval.c (closures, import cache, memory fixes)
// Исполнение AST, окружения, вызовы функций и стабильная поддержка include/import.
// По умолчанию выполняется только top-level init. Любая функция может быть выбрана
// явным параметром через eval_set_entry()/--entry.

#include "kirton/eval.h"
#include "eval_internal.h"
#include "kirton/ffi_internal.h"
#include "kirton/native.h"
#include "kirton/diag.h"
#include "kirton/parser.h"
#include "kirton/value.h"
#include "kirton/common.h"
#include "kirton/utf8.h"
#include "kirton/callstack.h"
#include "kirton/source_cache.h"
#include "native_internal.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <errno.h>
#include <limits.h>

#ifdef _WIN32
#  include <io.h>
#  include <direct.h>
#  include <windows.h>
#  define KIR_PATH_SEP '\\'
#  define KIR_PATH_SEP_STR "\\"
#else
#  include <unistd.h>
#  include <limits.h>
#  define KIR_PATH_SEP '/'
#  define KIR_PATH_SEP_STR "/"
#endif

/* -------- точка входа, задаётся main.c через eval_set_entry -------- */
static const char* g_entry = NULL;
static int g_ffi_cli_enabled = 0;
void eval_set_ffi_enabled(int enabled){ g_ffi_cli_enabled = enabled!=0; }
static unsigned int g_eval_error_baseline;
static unsigned int g_eval_handled_error_count;
static unsigned int g_eval_error_stack_count;
static KirFrame* g_eval_error_frames;
static size_t g_eval_error_frame_count;
void eval_set_entry(const char* name_or_null){ g_entry = name_or_null; }

static int eval_has_error(void){
    unsigned int errors=kir_diag_error_count();
    if (errors<=g_eval_handled_error_count) return 0;
    if (g_eval_error_stack_count!=errors){
        size_t count=cs_snapshot(NULL,0);
        if (count>g_eval_error_frame_count){
            g_eval_error_frames=(KirFrame*)kir_xreallocn(
                g_eval_error_frames,count,sizeof(*g_eval_error_frames));
            g_eval_error_frame_count=count;
        }
        cs_snapshot(g_eval_error_frames,g_eval_error_frame_count);
        g_eval_error_stack_count=errors;
    }
    return 1;
}

/* -------- окружение (лексическая цепочка словарей) -------- */
typedef struct ModuleBinding ModuleBinding;
typedef struct KirPinnedValue KirPinnedValue;

typedef struct KirEnv {
    Value dict;          /* VAL_DICT */
    struct KirEnv* up;   /* родитель */
    ModuleBinding* modules; /* импорты, видимые из этого окружения */
    struct KirEnv* owned_next; /* владение памятью VM */
    size_t closure_refs;
} KirEnv;

struct ModuleBinding {
    char* alias;
    size_t alias_len;
    char* native_module; /* NULL для пользовательского модуля */
    char* file;
    AstProgram* program;
    KirEnv* closure;
    ModuleBinding* next;
};

/* -------- функции и VM (с замыканиями) -------- */
typedef struct {
    const char* name; size_t nn;
    const char* file; int line;
    AstFunc* f;
    KirEnv*  closure;      /* окружение определения функции */
} FuncEntry;

typedef struct {
    char* key;
    Value dict;
    int init_done;
    AstProgram* prog;
    KirEnv* env;
    char* module_name;
} ModRec;

typedef struct {
    AstUserType* type;
    KirEnv* closure;
} UserTypeEntry;

struct KirPinnedValue {
    Value value;
    KirPinnedValue* next;
};

struct KirMemoryBorrow {
    Value value;
    KirPinnedValue* pin;
    const void* data;
    size_t bytes;
    bool writable;
    struct KirMemoryBorrow* next;
};

struct KirNativeResourceLease {
    Value value;
    KirPinnedValue* pin;
    char* type_tag;
    bool visiting;
    struct KirNativeResourceLease* next;
};

typedef struct VM {
    FuncEntry* ftab; size_t fn, fc;
    UserTypeEntry* types; size_t types_n, types_c;
    KirEnv*    root_env;   /* корень программы/модуля */
    ModRec* mods; size_t mods_n, mods_c;
    char** loading; size_t loading_n, loading_c;
    KirBigIntHeap* bigint_heap;
    KirValueHeap* value_heap;
    KirEnv* owned_envs;
    KirPinnedValue* pinned;
    KirMemoryBorrow* borrows;
    size_t borrow_count;
    size_t native_visits;
    KirNativeResourceLease* resource_leases;
    size_t resource_lease_count;
    KirFfiState* ffi;
    size_t ffi_calls;
    int ffi_enabled;
    AstProgram** owned_programs;
    size_t owned_programs_n, owned_programs_c;
    Value** gc_roots; size_t gc_roots_n, gc_roots_c;
    KirEnv** gc_envs; size_t gc_envs_n, gc_envs_c;
    const Value* external_roots; size_t external_roots_n;
    size_t gc_threshold, gc_collections, gc_reclaimed, gc_peak;
    size_t gc_arena_threshold;
} VM;

/* --- UTF-8 friendly fopen for Windows --- */
#ifdef _WIN32
static FILE* fopen_utf8(const char* path, const char* mode){
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    int mlen = MultiByteToWideChar(CP_UTF8, 0, mode, -1, NULL, 0);
    if (wlen<=0 || mlen<=0) return NULL;
    wchar_t* wpath = (wchar_t*)kir_oom_malloc((size_t)wlen * sizeof(wchar_t));
    wchar_t* wmode = (wchar_t*)kir_oom_malloc((size_t)mlen * sizeof(wchar_t));
    if (!wpath || !wmode){ kir_free(wpath); kir_free(wmode); return NULL; }
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen);
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, mlen);
    FILE* f = _wfopen(wpath, wmode);
    kir_free(wpath);
    kir_free(wmode);
    return f;
}
#else
static FILE* fopen_utf8(const char* path, const char* mode){
    return fopen(path, mode);
}
#endif

/* ==================== УТИЛИТЫ ПАМЯТИ/СТРОК ==================== */

static char* k_strdup(const char* s){
    size_t n = strlen(s);
    char* r = (char*)kir_malloc(n+1);
    memcpy(r, s, n); r[n]=0;
    return r;
}

static char* k_strdupn(const char* s, size_t n){
    char* r = (char*)kir_malloc(n + 1);
    if (n) memcpy(r, s, n);
    r[n] = 0;
    return r;
}

/* чтение файла полностью (UTF-8 как есть) */
char* kir_read_text_file(const char* path, size_t* out_len){
    FILE* f = fopen_utf8(path, "rb");
    if(!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0){ fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0){ fclose(f); return NULL; }
    rewind(f);
    if ((uintmax_t)sz >= (uintmax_t)SIZE_MAX){ fclose(f); return NULL; }
    char* buf = (char*)kir_oom_malloc((size_t)sz + 1);
    if (!buf){ fclose(f); return NULL; }
    size_t r = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (r != (size_t)sz){ kir_free(buf); return NULL; }
    buf[sz] = 0;
    if (out_len) *out_len = (size_t)sz;
    return buf;
}

/* ========================= Module cache (по разрешённому пути) ========================= */

static int mod_find(VM* vm, const char* key){
    for (size_t i=0;i<vm->mods_n;i++){
        if (strcmp(vm->mods[i].key, key)==0) return (int)i;
    }
    return -1;
}
static int mod_push_new(VM* vm, const char* key){
    if (vm->mods_n+1 > vm->mods_c){
        size_t nc = vm->mods_c? vm->mods_c*2 : 16;
        vm->mods = (ModRec*)kir_realloc(vm->mods, nc*sizeof(ModRec));
        vm->mods_c = nc;
    }
    ModRec* rec=&vm->mods[vm->mods_n];
    rec->key=k_strdup(key);
    rec->dict=v_dict_new();
    rec->init_done=0;
    rec->prog=NULL;
    rec->env=NULL;
    rec->module_name=NULL;
    return (int)vm->mods_n++;
}

/* Стек загрузки для детекта циклов импорта (по ключу) */
static int loading_has(VM* vm, const char* key){
    for (size_t i=0;i<vm->loading_n;i++)
        if (strcmp(vm->loading[i], key)==0) return 1;
    return 0;
}
static void loading_push(VM* vm, const char* key){
    if (vm->loading_n+1 > vm->loading_c){
        size_t nc = vm->loading_c? vm->loading_c*2 : 16;
        vm->loading = (char**)kir_realloc(vm->loading, nc*sizeof(char*));
        vm->loading_c = nc;
    }
    vm->loading[vm->loading_n++] = k_strdup(key);
}
static void loading_pop(VM* vm){
    if (vm->loading_n==0) return;
    char* s = vm->loading[--vm->loading_n];
    kir_free(s);
}

/* ==================== PATH УТИЛИТЫ ДЛЯ ИМПОРТА ==================== */

static int file_exists(const char* p){
    FILE* f = fopen_utf8(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static int is_abs_path(const char* p){
    if (!p || !*p) return 0;
#ifdef _WIN32
    if ((strlen(p) > 2 && p[1]==':' && (p[2]=='\\' || p[2]=='/')) ||
        (p[0]=='\\' && p[1]=='\\')) return 1;
    return 0;
#else
    return p[0]=='/';
#endif
}

static void replace_dots_with_sep(char* s){
    for (; *s; ++s) if (*s=='.') *s = KIR_PATH_SEP;
}

static int has_suffix(const char* value, const char* suffix){
    size_t value_len=strlen(value), suffix_len=strlen(suffix);
    return value_len>=suffix_len &&
           memcmp(value+value_len-suffix_len,suffix,suffix_len)==0;
}

/* Безопасное склеивание base + sep + leaf в out */
static void path_join2(char* out, size_t outsz, const char* base, const char* leaf){
    if (!base || !*base){
        snprintf(out, outsz, "%s", leaf ? leaf : "");
        return;
    }
    size_t lb = strlen(base);
    int need_sep = (lb>0 && base[lb-1]!=KIR_PATH_SEP);
    if (need_sep)
        snprintf(out, outsz, "%s" KIR_PATH_SEP_STR "%s", base, leaf?leaf:"");
    else
        snprintf(out, outsz, "%s%s", base, leaf?leaf:"");
}

static char* kirton_module_search_path_utf8(void){
#ifdef _WIN32
    DWORD wide_length=GetEnvironmentVariableW(L"KIRTON_PATH",NULL,0);
    if(!wide_length)return NULL;
    if((size_t)wide_length>SIZE_MAX/sizeof(wchar_t))return NULL;
    wchar_t* wide=(wchar_t*)malloc((size_t)wide_length*sizeof(*wide));
    if(!wide)return NULL;
    DWORD copied=GetEnvironmentVariableW(L"KIRTON_PATH",wide,wide_length);
    if(!copied || copied>=wide_length){free(wide);return NULL;}
    int utf8_length=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide,-1,
                                        NULL,0,NULL,NULL);
    if(utf8_length<=0){free(wide);return NULL;}
    char* utf8=(char*)malloc((size_t)utf8_length);
    if(!utf8){free(wide);return NULL;}
    if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,wide,-1,
                            utf8,utf8_length,NULL,NULL)){
        free(utf8);
        utf8=NULL;
    }
    free(wide);
    return utf8;
#else
    const char* value=getenv("KIRTON_PATH");
    if(!value || !*value)return NULL;
    size_t length=strlen(value)+1;
    char* copy=(char*)malloc(length);
    if(copy)memcpy(copy,value,length);
    return copy;
#endif
}

/* Возвращает 1 и заполняет out_path, если файл найден. Иначе 0.
   ПОДДЕРЖИВАЕТ .кир и .kir (ASCII), а также имя без расширения. */
int kir_resolve_module_path(const char* name_utf8, const char* from_dir, char* out_path, size_t outsz){
    int explicit_path = strchr(name_utf8, '/') || strchr(name_utf8, '\\') ||
                        is_abs_path(name_utf8) || has_suffix(name_utf8,".кир") ||
                        has_suffix(name_utf8,".kir");
    char modbuf[512];
    size_t n = strlen(name_utf8);
    if (n >= sizeof(modbuf)) n = sizeof(modbuf)-1;
    memcpy(modbuf, name_utf8, n); modbuf[n] = 0;
    if (!explicit_path) replace_dots_with_sep(modbuf);

    char candidate[1024];

    /* 1) name как путь */
    if (explicit_path){
        snprintf(candidate, sizeof(candidate), "%s.кир", modbuf);
        if (file_exists(candidate)){ snprintf(out_path, outsz, "%s", candidate); return 1; }
        snprintf(candidate, sizeof(candidate), "%s.kir", modbuf);
        if (file_exists(candidate)){ snprintf(out_path, outsz, "%s", candidate); return 1; }
        if (file_exists(modbuf)){ snprintf(out_path, outsz, "%s", modbuf); return 1; }
    }

    /* 2) from_dir */
    if (from_dir && *from_dir){
        char leaf_cyr[600], leaf_lat[600];
        snprintf(leaf_cyr, sizeof(leaf_cyr), "%s.кир", modbuf);
        snprintf(leaf_lat, sizeof(leaf_lat), "%s.kir", modbuf);

        path_join2(candidate, sizeof(candidate), from_dir, leaf_cyr);
        if (file_exists(candidate)){ snprintf(out_path, outsz, "%s", candidate); return 1; }

        path_join2(candidate, sizeof(candidate), from_dir, leaf_lat);
        if (file_exists(candidate)){ snprintf(out_path, outsz, "%s", candidate); return 1; }

        path_join2(candidate, sizeof(candidate), from_dir, modbuf);
        if (file_exists(candidate)){ snprintf(out_path, outsz, "%s", candidate); return 1; }
    }

    /* 3) KIRTON_PATH */
    char* env_path=kirton_module_search_path_utf8();
    const char* envp=env_path;
    if (envp && *envp){
#ifdef _WIN32
        const char sep = ';';
#else
        const char sep = ':';
#endif
        const char* it = envp;
        while (*it){
            const char* seg_start = it;
            while (*it && *it != sep) ++it;
            size_t seg_len = (size_t)(it - seg_start);
            char seg[512];
            if (seg_len >= sizeof(seg)) seg_len = sizeof(seg)-1;
            memcpy(seg, seg_start, seg_len); seg[seg_len] = 0;

            char leaf_cyr[600], leaf_lat[600];
            snprintf(leaf_cyr, sizeof(leaf_cyr), "%s.кир", modbuf);
            snprintf(leaf_lat, sizeof(leaf_lat), "%s.kir", modbuf);

            path_join2(candidate, sizeof(candidate), seg, leaf_cyr);
            if (file_exists(candidate)){ snprintf(out_path, outsz, "%s", candidate); free(env_path); return 1; }

            path_join2(candidate, sizeof(candidate), seg, leaf_lat);
            if (file_exists(candidate)){ snprintf(out_path, outsz, "%s", candidate); free(env_path); return 1; }

            path_join2(candidate, sizeof(candidate), seg, modbuf);
            if (file_exists(candidate)){ snprintf(out_path, outsz, "%s", candidate); free(env_path); return 1; }

            if (*it == sep) ++it;
        }
    }
    free(env_path);

    /* 4) Текущая директория */
    snprintf(candidate, sizeof(candidate), "%s.кир", modbuf);
    if (file_exists(candidate)){ snprintf(out_path, outsz, "%s", candidate); return 1; }
    snprintf(candidate, sizeof(candidate), "%s.kir", modbuf);
    if (file_exists(candidate)){ snprintf(out_path, outsz, "%s", candidate); return 1; }
    if (file_exists(modbuf)){ snprintf(out_path, outsz, "%s", modbuf); return 1; }

    return 0;
}

/* Нормализация пути для ключа кэша */
void kir_normalize_module_key(const char* in, char* out, size_t outsz){
#ifdef _WIN32
    char* full = _fullpath(NULL, in, 0);
    if (full){ snprintf(out, outsz, "%s", full); free(full); return; }
#else
    char realbuf[PATH_MAX];
    if (realpath(in, realbuf)){ snprintf(out, outsz, "%s", realbuf); return; }
#endif
    snprintf(out, outsz, "%s", in);
}

/* Имя модуля = последний компонент пути без расширения. */
void kir_module_name_from_path(const char* path, char* out, size_t outsz){
    if (!path || !*path || outsz==0){ if (outsz) out[0]=0; return; }
    size_t n = strlen(path);
    const char* base = path;
    for (size_t i=0;i<n;i++){
        char c = path[i];
        if (c=='/' || c=='\\') base = path + i + 1;
    }
    const char* end = path + n;
    const char* dot = NULL;
    for (const char* p = end; p>base;){
        --p;
        if (*p=='.'){ dot = p; break; }
        if (*p=='/' || *p=='\\') break;
    }
    size_t len = (dot ? (size_t)(dot - base) : strlen(base));
    if (len == 0) len = strlen(base);
    if (len >= outsz) len = outsz - 1;
    memcpy(out, base, len);
    out[len] = '\0';
}

/* ==================== ВСПОМОГАТЕЛЬНЫЕ УТИЛИТЫ ОКРУЖЕНИЯ/VM ==================== */

static KirEnv* env_new(VM* vm, KirEnv* up){
    KirEnv* e = (KirEnv*)kir_malloc(sizeof(KirEnv));
    KirValueHeap* previous = v_value_heap_set_current(vm->value_heap);
    e->dict = v_dict_new();
    v_value_heap_set_current(previous);
    e->up = up;
    e->modules = NULL;
    e->closure_refs = 0;
    e->owned_next = vm->owned_envs;
    vm->owned_envs = e;
    return e;
}

static void env_free_struct(KirEnv* env){
    ModuleBinding* binding=env->modules;
    while (binding){
        ModuleBinding* next=binding->next;
        kir_free(binding->alias);
        kir_free(binding->native_module);
        kir_free(binding->file);
        kir_free(binding);
        binding=next;
    }
    kir_free(env);
}

/* Временные окружения вызовов не должны расти пропорционально числу
   вызовов. Сохраняем кадр только если он стал замыканием функции/модуля. */
static void env_release_temporary(VM* vm, KirEnv* env){
    if (!env || env==vm->root_env || env->closure_refs) return;
    for (size_t i=0;i<vm->fn;++i)
        for (KirEnv* e=vm->ftab[i].closure;e;e=e->up)
            if (e==env) return;
    for (size_t i=0;i<vm->mods_n;++i)
        for (KirEnv* e=vm->mods[i].env;e;e=e->up)
            if (e==env) return;
    KirEnv** link=&vm->owned_envs;
    while (*link && *link!=env) link=&(*link)->owned_next;
    if (*link){ *link=env->owned_next; env_free_struct(env); }
}

static int env_get(KirEnv* e, const char* k, size_t kn, Value* out){
    for (KirEnv* cur=e; cur; cur=cur->up){
        if (v_dict_get(&cur->dict, k, kn, out)) return 1;
    }
    return 0;
}

static void env_unbind_module(KirEnv* env, const char* alias, size_t alias_len){
    ModuleBinding** link = &env->modules;
    while (*link){
        ModuleBinding* binding = *link;
        if (binding->alias_len == alias_len && memcmp(binding->alias, alias, alias_len) == 0){
            *link = binding->next;
            kir_free(binding->alias);
            kir_free(binding->native_module);
            kir_free(binding->file);
            kir_free(binding);
            return;
        }
        link = &binding->next;
    }
}

static void env_declare(KirEnv* e, const char* k, size_t kn, Value v){
    env_unbind_module(e, k, kn);
    v_dict_set(&e->dict, k, kn, v);
}

static int env_assign(KirEnv* e, const char* k, size_t kn, Value v){
    KirEnv* cur = e;
    while (cur){
        if (v_dict_find(&cur->dict, k, kn) >= 0){
            env_unbind_module(cur, k, kn);
            v_dict_set(&cur->dict, k, kn, v);
            return 1;
        }
        cur = cur->up;
    }
    return 0;
}

/* A loop target reuses the nearest binding, or declares one in this scope. */
static void env_assign_or_declare(KirEnv* e, const char* k, size_t kn, Value v){
    if (!env_assign(e,k,kn,v)) env_declare(e,k,kn,v);
}

static Value make_function_value(VM* vm,AstFunc* function,KirEnv* closure);

static void hoist_function_statements(VM* vm,KirEnv* env,AstStmt** body,size_t len){
    for (size_t i=0;i<len;++i){
        AstStmt* statement=body[i];
        if (!statement) continue;
        switch (statement->kind){
            case STMT_FUNCTION: {
                AstFunc* function=statement->as.function;
                Value value=make_function_value(vm,function,env);
                env_declare(env,function->name_start,function->name_len,value);
            } break;
            case STMT_IF:
                hoist_function_statements(vm,env,statement->as.if_.then_b,
                                          statement->as.if_.then_len);
                hoist_function_statements(vm,env,statement->as.if_.else_b,
                                          statement->as.if_.else_len);
                break;
            case STMT_WHILE:
                hoist_function_statements(vm,env,statement->as.wh.body,
                                          statement->as.wh.body_len);
                break;
            case STMT_FOREACH:
                hoist_function_statements(vm,env,statement->as.fe.body,
                                          statement->as.fe.body_len);
                break;
            default: break;
        }
    }
}

/* Мутация списка/словаря меняет само значение, сохраняя его import binding. */
static void env_write_back(KirEnv* e, const char* k, size_t kn, Value v){
    if (!e) return;
    for (KirEnv* cur = e; cur; cur = cur->up){
        if (v_dict_find(&cur->dict, k, kn) >= 0){
            v_dict_set(&cur->dict, k, kn, v);
            return;
        }
    }
    v_dict_set(&e->dict, k, kn, v);
}

static void env_bind_module(KirEnv* env, const char* alias, size_t alias_len,
                            const char* native_module, AstProgram* program,
                            KirEnv* closure, const char* file){
    for (ModuleBinding* binding = env->modules; binding; binding = binding->next){
        if (binding->alias_len == alias_len && memcmp(binding->alias, alias, alias_len) == 0){
            kir_free(binding->native_module);
            kir_free(binding->file);
            binding->native_module = native_module ? k_strdup(native_module) : NULL;
            binding->file = file ? k_strdup(file) : NULL;
            binding->program = program;
            binding->closure = closure;
            return;
        }
    }

    ModuleBinding* binding = (ModuleBinding*)kir_malloc(sizeof(ModuleBinding));
    binding->alias = k_strdupn(alias, alias_len);
    binding->alias_len = alias_len;
    binding->native_module = native_module ? k_strdup(native_module) : NULL;
    binding->file = file ? k_strdup(file) : NULL;
    binding->program = program;
    binding->closure = closure;
    binding->next = env->modules;
    env->modules = binding;
}

static ModuleBinding* env_find_module(KirEnv* env, const char* alias, size_t alias_len){
    for (KirEnv* current = env; current; current = current->up){
        for (ModuleBinding* binding = current->modules; binding; binding = binding->next){
            if (binding->alias_len == alias_len &&
                memcmp(binding->alias, alias, alias_len) == 0) return binding;
        }
        /* Обычная переменная затеняет import binding во внешнем окружении. */
        if (v_dict_find(&current->dict, alias, alias_len) >= 0) return NULL;
    }
    return NULL;
}

static AstFunc* module_find_function(AstProgram* program, const char* name, size_t name_len){
    if (!program) return NULL;
    for (size_t i = program->func_count; i>0; --i){
        AstFunc* func = program->funcs[i-1];
        if (func->name_len == name_len && memcmp(func->name_start, name, name_len) == 0) return func;
    }
    return NULL;
}

static FuncEntry* vm_add_func(VM* vm, const char* ns, size_t nn, AstFunc* f, const char* file, int line, KirEnv* closure){
    /* Повторное объявление заменяет функцию только в том же окружении. */
    for (size_t i = 0; i < vm->fn; ++i){
        FuncEntry* existing = &vm->ftab[i];
        if (existing->closure == closure && existing->nn == nn && memcmp(existing->name, ns, nn) == 0){
            existing->f = f;
            existing->file = file;
            existing->line = line;
            existing->closure = closure;
            return existing;
        }
    }

    if (vm->fn+1 > vm->fc){
        size_t nc = vm->fc? vm->fc*2 : 32;
        vm->ftab = (FuncEntry*)kir_realloc(vm->ftab, nc*sizeof(FuncEntry));
        vm->fc = nc;
    }
    FuncEntry* fe = &vm->ftab[vm->fn++];
    fe->name = k_strdup(ns);
    fe->nn = nn;
    fe->f = f; fe->file=file; fe->line=line; fe->closure = closure;
    return fe;
}

static FuncEntry* vm_find_func(VM* vm, const char* ns, size_t nn){
    for (size_t i=vm->fn;i>0;i--){
        if (vm->ftab[i-1].nn==nn && memcmp(vm->ftab[i-1].name, ns, nn)==0) return &vm->ftab[i-1];
    }
    return NULL;
}

static FuncEntry* vm_find_func_scoped(VM* vm, KirEnv* env, const char* ns, size_t nn){
    for (KirEnv* scope=env; scope; scope=scope->up){
        for (size_t i=vm->fn; i>0; --i){
            FuncEntry* entry=&vm->ftab[i-1];
            if (entry->closure==scope && entry->nn==nn && memcmp(entry->name,ns,nn)==0) return entry;
        }
    }
    return NULL;
}

static AstUserType* vm_find_type_scoped(VM* vm,KirEnv* env,
                                       const char* name,size_t name_len,
                                       AstUserTypeKind kind){
    for (KirEnv* scope=env;scope;scope=scope->up){
        for (size_t i=vm->types_n;i>0;--i){
            UserTypeEntry* entry=&vm->types[i-1];
            if (entry->closure==scope && entry->type->kind==kind &&
                entry->type->name_len==name_len &&
                memcmp(entry->type->name,name,name_len)==0) return entry->type;
        }
    }
    return NULL;
}

static AstUserType* vm_find_any_type_scoped(VM* vm,KirEnv* env,
                                            const char* name,size_t name_len){
    AstUserType* type=vm_find_type_scoped(vm,env,name,name_len,AST_TYPE_STRUCT);
    return type?type:vm_find_type_scoped(vm,env,name,name_len,AST_TYPE_ENUM);
}

static int value_matches_type(VM* vm,KirEnv* env,const char* name,size_t len,Value value){
    if (len==strlen("пусто") && !memcmp(name,"пусто",len)) return value.kind==VAL_NIL;
    if (len==strlen("булево") && !memcmp(name,"булево",len)) return value.kind==VAL_BOOL;
    if (len==strlen("целое") && !memcmp(name,"целое",len))
        return value.kind==VAL_NUM && v_num_is_integer(value);
    if (len==strlen("число") && !memcmp(name,"число",len))
        return value.kind==VAL_NUM && !v_num_is_integer(value);
    if (len==strlen("строка") && !memcmp(name,"строка",len)) return value.kind==VAL_STR;
    if (len==strlen("список") && !memcmp(name,"список",len)) return value.kind==VAL_LIST;
    if (len==strlen("словарь") && !memcmp(name,"словарь",len)) return value.kind==VAL_DICT;
    AstUserType* type=vm_find_any_type_scoped(vm,env,name,len);
    if (!type) return 0;
    return type->kind==AST_TYPE_STRUCT ?
           v_user_struct_type(value)==type : v_enum_value_type(value)==type;
}

static AstTypeField* user_type_field(AstUserType* type,const char* name,size_t len){
    if (!type || type->kind!=AST_TYPE_STRUCT) return NULL;
    for (size_t i=0;i<type->field_count;++i)
        if (type->fields[i].name_len==len &&
            memcmp(type->fields[i].name,name,len)==0) return &type->fields[i];
    return NULL;
}

/* ==================== ВЫЧИСЛЕНИЕ ВЫРАЖЕНИЙ ==================== */

typedef enum {
    FLOW_NONE,
    FLOW_RETURN,
    FLOW_BREAK,
    FLOW_CONTINUE
} FlowKind;

typedef struct { FlowKind flow; Value v; } Ret;

static void vm_own_program(VM* vm, AstProgram* p){
    if (!p) return;
    if (vm->owned_programs_n==vm->owned_programs_c){
        size_t cap=vm->owned_programs_c ? vm->owned_programs_c*2 : 8;
        vm->owned_programs=(AstProgram**)kir_xreallocn(
            vm->owned_programs,cap,sizeof(*vm->owned_programs));
        vm->owned_programs_c=cap;
    }
    vm->owned_programs[vm->owned_programs_n++]=p;
}

static void gc_root_push(VM* vm, Value* value){
    if (!vm->bigint_heap) return;
    if (vm->gc_roots_n==vm->gc_roots_c){
        size_t cap=vm->gc_roots_c ? vm->gc_roots_c*2 : 32;
        vm->gc_roots=(Value**)kir_xreallocn(vm->gc_roots,cap,sizeof(*vm->gc_roots));
        vm->gc_roots_c=cap;
    }
    vm->gc_roots[vm->gc_roots_n++]=value;
}

static void gc_env_push(VM* vm, KirEnv* env){
    if (!vm->bigint_heap) return;
    if (vm->gc_envs_n==vm->gc_envs_c){
        size_t cap=vm->gc_envs_c ? vm->gc_envs_c*2 : 16;
        vm->gc_envs=(KirEnv**)kir_xreallocn(vm->gc_envs,cap,sizeof(*vm->gc_envs));
        vm->gc_envs_c=cap;
    }
    vm->gc_envs[vm->gc_envs_n++]=env;
}

static void gc_mark_value(VM* vm, Value value){
    v_value_heap_mark(vm->value_heap,vm->bigint_heap,value);
}

static void gc_mark_env(VM* vm, KirEnv* env){
    for (KirEnv* current=env; current; current=current->up)
        gc_mark_value(vm,current->dict);
}

static void gc_trace_function_closure(void* context,void* closure){
    gc_mark_env((VM*)context,(KirEnv*)closure);
}

static void gc_release_function_closure(void* context,void* closure){
    VM* vm=(VM*)context;
    KirEnv* env=(KirEnv*)closure;
    if (!vm || !env || !env->closure_refs) return;
    --env->closure_refs;
    env_release_temporary(vm,env);
}

static Value make_function_value(VM* vm,AstFunc* function,KirEnv* closure){
    KirValueHeap* previous=v_value_heap_set_current(vm->value_heap);
    Value value=v_function_new(function,closure,gc_trace_function_closure,
                                gc_release_function_closure,vm);
    v_value_heap_set_current(previous);
    if (value.kind==VAL_FUNCTION && closure) ++closure->closure_refs;
    return value;
}

static Value module_export_dict(VM* vm,AstProgram* program,KirEnv* closure){
    Value exports=v_dict_new();
    if (!program) return exports;
    for (size_t i=0;i<program->func_count;++i){
        AstFunc* function=program->funcs[i];
        if (!function) continue;
        Value value=make_function_value(vm,function,closure);
        v_dict_set(&exports,function->name_start,function->name_len,value);
    }
    return exports;
}

static void gc_collect(VM* vm, int external_only){
    if (!vm || !vm->bigint_heap || v_value_heap_is_finalizing(vm->value_heap)) return;
    size_t before=kir_bigint_heap_count(vm->bigint_heap)+v_value_heap_count(vm->value_heap);
    if (before>vm->gc_peak) vm->gc_peak=before;
    for (size_t i=0; i<vm->external_roots_n; ++i)
        gc_mark_value(vm,vm->external_roots[i]);
    for (KirPinnedValue* pin=vm->pinned;pin;pin=pin->next)
        gc_mark_value(vm,pin->value);
    if (!external_only){
        gc_mark_env(vm,vm->root_env);
        for (size_t i=0; i<vm->gc_envs_n; ++i) gc_mark_env(vm,vm->gc_envs[i]);
        for (size_t i=0; i<vm->fn; ++i) gc_mark_env(vm,vm->ftab[i].closure);
        for (size_t i=0; i<vm->mods_n; ++i){
            gc_mark_env(vm,vm->mods[i].env);
            gc_mark_value(vm,vm->mods[i].dict);
        }
        for (size_t i=0; i<vm->gc_roots_n; ++i)
            gc_mark_value(vm,*vm->gc_roots[i]);
    }
    vm->gc_reclaimed += v_value_heap_sweep(vm->value_heap);
    vm->gc_reclaimed += kir_bigint_heap_sweep(vm->bigint_heap);
    ++vm->gc_collections;
    size_t live=kir_bigint_heap_count(vm->bigint_heap)+v_value_heap_count(vm->value_heap);
    vm->gc_threshold=live > SIZE_MAX-256 ? SIZE_MAX : live+256;
    /* Large native arena reservations must not wait for 256 tiny Values. */
    size_t reserved=v_value_heap_arena_bytes(vm->value_heap);
    const size_t step=16u*1024u*1024u;
    vm->gc_arena_threshold=reserved>SIZE_MAX-step ? SIZE_MAX : reserved+step;
}

static Value eval_expr(VM* vm, KirEnv* env, AstExpr* e); /* fwd */
static Ret   exec_stmt(VM* vm, KirEnv* env, AstStmt* s); /* fwd */
static Ret   exec_block(VM* vm, KirEnv* env, AstStmt** body, size_t blen); /* fwd */

static void dict_set_text(Value* dict,const char* key,const char* value){
    Value text=v_str_copy(value?value:"",-1);
    v_dict_set(dict,key,strlen(key),text);
}

static Value make_error_value(void){
    KirDiagnostic diagnostic={0};
    kir_diag_last_error(&diagnostic);
    Value error=v_dict_new();
    dict_set_text(&error,"код","runtime");
    dict_set_text(&error,"сообщение",diagnostic.message);
    dict_set_text(&error,"файл",diagnostic.file);
    v_dict_set(&error,"строка",strlen("строка"),
               diagnostic.line>0?v_int(diagnostic.line):v_nil());
    v_dict_set(&error,"столбец",strlen("столбец"),
               diagnostic.col>0?v_int(diagnostic.col):v_nil());
    Value stack=v_list_new();
    size_t count=g_eval_error_stack_count==kir_diag_error_count()
                 ? g_eval_error_frame_count : 0;
    for (size_t i=0;i<count;++i){
        Value frame=v_dict_new();
        dict_set_text(&frame,"функция",g_eval_error_frames[i].func);
        dict_set_text(&frame,"файл",g_eval_error_frames[i].file);
        v_dict_set(&frame,"строка",strlen("строка"),v_int(g_eval_error_frames[i].line));
        v_list_push(&stack,frame);
    }
    v_dict_set(&error,"стек",strlen("стек"),stack);
    return error;
}

typedef struct {
    const void* left;
    const void* right;
    ValKind kind;
} ValuePair;

typedef struct {
    ValuePair* items;
    size_t len;
    size_t cap;
} ValuePairStack;

static int value_pair_is_active(const ValuePairStack* stack, const void* left,
                                const void* right, ValKind kind){
    for (size_t i = 0; i < stack->len; ++i){
        const ValuePair* pair = &stack->items[i];
        if (pair->kind == kind &&
            ((pair->left == left && pair->right == right) ||
             (pair->left == right && pair->right == left))) return 1;
    }
    return 0;
}

static void value_pair_push(ValuePairStack* stack, const void* left,
                            const void* right, ValKind kind){
    if (stack->len == stack->cap){
        size_t next_cap = stack->cap ? stack->cap * 2 : 16;
        stack->items = (ValuePair*)kir_xreallocn(stack->items, next_cap, sizeof(ValuePair));
        stack->cap = next_cap;
    }
    stack->items[stack->len++] = (ValuePair){left, right, kind};
}

static bool values_equal(Value a, Value b, ValuePairStack* stack){
    if (a.kind != b.kind) return false;
    switch (a.kind){
        case VAL_NIL:  return true;
        case VAL_NUM:  return v_num_equal(a, b);
        case VAL_BOOL: return a.as.boolean == b.as.boolean;
        case VAL_ARENA: return a.as.arena == b.as.arena;
        case VAL_BUFFER: return a.as.buffer == b.as.buffer;
        case VAL_TYPED_ARRAY: return a.as.typed_array == b.as.typed_array;
        case VAL_RECORD_LAYOUT: return a.as.record_layout == b.as.record_layout;
        case VAL_RECORD: return a.as.record == b.as.record;
        case VAL_NATIVE_RESOURCE: return a.as.native_resource == b.as.native_resource &&
                                         a.integer_small == b.integer_small;
        case VAL_USER_STRUCT: return a.as.user_struct==b.as.user_struct &&
                                     a.integer_small==b.integer_small;
        case VAL_ENUM_VALUE: {
            const char* an=NULL; const char* bn=NULL;
            size_t al=0,bl=0;
            return v_enum_value_type(a)==v_enum_value_type(b) &&
                   v_enum_value_variant(a,&an,&al) &&
                   v_enum_value_variant(b,&bn,&bl) && al==bl &&
                   (al==0 || memcmp(an,bn,al)==0);
        }
        case VAL_FUNCTION: return a.as.function == b.as.function &&
                                   a.integer_small == b.integer_small;
        case VAL_STR:
            return a.as.str.n == b.as.str.n &&
                   (a.as.str.n == 0 || memcmp(a.as.str.s, b.as.str.s, a.as.str.n) == 0);
        case VAL_LIST: {
            if (a.as.list->len != b.as.list->len) return false;
            if (a.as.list->len == 0 || a.as.list == b.as.list) return true;
            const void* left = a.as.list;
            const void* right = b.as.list;
            if (value_pair_is_active(stack, left, right, VAL_LIST)) return true;
            size_t mark = stack->len;
            value_pair_push(stack, left, right, VAL_LIST);
            bool equal = true;
            for (size_t i = 0; i < a.as.list->len; ++i){
                if (!values_equal(a.as.list->data[i], b.as.list->data[i], stack)){
                    equal = false;
                    break;
                }
            }
            stack->len = mark;
            return equal;
        }
        case VAL_DICT: {
            if (a.as.dict->len != b.as.dict->len) return false;
            if (a.as.dict->len == 0 || a.as.dict == b.as.dict) return true;
            const void* left = a.as.dict;
            const void* right = b.as.dict;
            if (value_pair_is_active(stack, left, right, VAL_DICT)) return true;
            size_t mark = stack->len;
            value_pair_push(stack, left, right, VAL_DICT);
            bool equal = true;
            for (size_t i = 0; i < a.as.dict->len; ++i){
                int at = v_dict_find(&b, a.as.dict->keys[i], a.as.dict->kn[i]);
                if (at < 0 || !values_equal(a.as.dict->vals[i], b.as.dict->vals[at], stack)){
                    equal = false;
                    break;
                }
            }
            stack->len = mark;
            return equal;
        }
    }
    return false;
}

bool eval_values_equal(Value left, Value right){
    ValuePairStack stack={0};
    bool equal=values_equal(left,right,&stack);
    kir_free(stack.items);
    return equal;
}

static Value eval_binop(const char* op, Value a, Value b, const AstPos* pos){
    if (op[0]=='+'){
        if (a.kind==VAL_NUM && b.kind==VAL_NUM) return v_num_add(a, b);
        if (a.kind==VAL_STR && b.kind==VAL_STR){
            size_t na=a.as.str.n, nb=b.as.str.n;
            char* s=(char*)kir_malloc(na+nb+1);
            memcpy(s,a.as.str.s,na); memcpy(s+na,b.as.str.s,nb); s[na+nb]=0;
            Value ret = v_str_copy(s, -1);
            kir_free(s);
            return ret;
        }
    } else if (op[0]=='-'){
        if (a.kind==VAL_NUM && b.kind==VAL_NUM) return v_num_sub(a, b);
    } else if (op[0]=='*'){
        if (a.kind==VAL_NUM && b.kind==VAL_NUM) return v_num_mul(a, b);
    } else if (strcmp(op, "нацело") == 0 || op[0]=='%'){
        if (a.kind!=VAL_NUM || b.kind!=VAL_NUM){
            kir_diag_error(pos && pos->file ? pos->file : "<выражение>",
                           pos ? pos->line : 1, pos ? pos->col : 1,
                           "Операция '%s' требует два числа", op);
            return v_nil();
        }
        int compare=1;
        if (v_num_compare(b, v_int(0), &compare) && compare==0){
            kir_diag_error(pos && pos->file ? pos->file : "<выражение>",
                           pos ? pos->line : 1, pos ? pos->col : 1,
                           "Деление на ноль в операции '%s'", op);
            return v_nil();
        }
        return strcmp(op, "нацело") == 0 ? v_num_floor_div(a,b) : v_num_mod(a,b);
    } else if (op[0]=='&' || op[0]=='|' || op[0]=='^'){
        if (!v_num_is_integer(a) || !v_num_is_integer(b)){
            kir_diag_error(pos && pos->file ? pos->file : "<выражение>",
                           pos ? pos->line : 1, pos ? pos->col : 1,
                           "Побитовая операция '%s' требует два целых числа", op);
            return v_nil();
        }
        return v_num_bitwise(a,b,op[0]);
    } else if ((op[0]=='<' && op[1]=='<') || (op[0]=='>' && op[1]=='>')){
        int64_t shift;
        if (!v_num_is_integer(a) || !v_num_is_integer(b) ||
            !v_num_fits_i64(b,&shift) || shift<0 || (uint64_t)shift>(uint64_t)SIZE_MAX){
            kir_diag_error(pos && pos->file ? pos->file : "<выражение>",
                           pos ? pos->line : 1, pos ? pos->col : 1,
                           "Сдвиг требует целое число и неотрицательную целую величину сдвига в диапазоне size_t");
            return v_nil();
        }
        return v_num_shift(a,(size_t)shift,op[0]=='<');
    } else if (op[0]=='/'){
        if (a.kind==VAL_NUM && b.kind==VAL_NUM){
            double denominator = v_num_to_double(b);
            if (denominator==0){
                kir_diag_error(pos && pos->file ? pos->file : "<выражение>",
                               pos ? pos->line : 1, pos ? pos->col : 1,
                               "Деление на ноль");
                return v_nil();
            }
            return v_num(v_num_to_double(a) / denominator);
        }
    } else if (op[0]=='=' && op[1]=='='){
        ValuePairStack stack = {0};
        bool equal = values_equal(a, b, &stack);
        kir_free(stack.items);
        return v_bool(equal);
    } else if (op[0]=='!' && op[1]=='='){
        Value eq = eval_binop("==", a, b, pos);
        return v_bool(!eq.as.boolean);
    } else if (op[0]=='<' && op[1]=='='){
        if (a.kind==VAL_NUM && b.kind==VAL_NUM){ int cmp; return v_bool(v_num_compare(a,b,&cmp) && cmp<=0); }
    } else if (op[0]=='>' && op[1]=='='){
        if (a.kind==VAL_NUM && b.kind==VAL_NUM){ int cmp; return v_bool(v_num_compare(a,b,&cmp) && cmp>=0); }
    } else if (op[0]=='<'){
        if (a.kind==VAL_NUM && b.kind==VAL_NUM){ int cmp; return v_bool(v_num_compare(a,b,&cmp) && cmp<0); }
    } else if (op[0]=='>'){
        if (a.kind==VAL_NUM && b.kind==VAL_NUM){ int cmp; return v_bool(v_num_compare(a,b,&cmp) && cmp>0); }
    }
    return v_nil();
}

static Value eval_expr_inner(VM* vm, KirEnv* env, AstExpr* e);

static Value eval_expr(VM* vm, KirEnv* env, AstExpr* e){
    size_t roots_before=vm->gc_roots_n;
    Value result=eval_expr_inner(vm,env,e);
    vm->gc_roots_n=roots_before;
    return result;
}

static Value eval_expr_inner(VM* vm, KirEnv* env, AstExpr* e){
    if (eval_has_error()) return v_nil();
    switch (e->kind){
        case AST_NUM:   return e->as.num.value;
        case AST_BOOL:  return v_bool(e->as.boolean.value);
        case AST_NIL:   return v_nil();
        case AST_STR:   return v_str_copy(e->as.str.start, (int)e->as.str.len);
        case AST_VAR: {
            Value v;
            if (env_get(env, e->as.var.name_start, e->as.var.name_len, &v)) return v;
            kir_diag_error(e->pos.file, e->pos.line, e->pos.col,
                           "Неизвестная переменная '%.*s'",
                           (int)e->as.var.name_len, e->as.var.name_start);
            return v_nil();
        }
        case AST_UNARY: {
            Value x = eval_expr(vm, env, e->as.unary.e);
            if (eval_has_error()) return v_nil();
            if (e->as.unary.op[0]=='-'){ if (x.kind==VAL_NUM) return v_num_neg(x); }
            if (e->as.unary.op[0]=='~'){
                if (!v_num_is_integer(x)){
                    kir_diag_error(e->pos.file,e->pos.line,e->pos.col,
                                   "Побитовое отрицание '~' требует целое число");
                    return v_nil();
                }
                return v_num_bitnot(x);
            }
            if (e->as.unary.op[0]=='!'){ return v_bool(!v_truthy(x)); }
            return v_nil();
        }
        case AST_BIN: {
            const char* op = e->as.bin.op;

            /* Короткое замыкание для логических "и"/"или" */
            if (op[0]=='&' && op[1]=='&' && op[2]=='\0'){
                Value a = eval_expr(vm, env, e->as.bin.a);
                if (eval_has_error()) return v_nil();
                if (!v_truthy(a)) return v_bool(false);   /* правую часть не считаем */
                Value b = eval_expr(vm, env, e->as.bin.b);
                if (eval_has_error()) return v_nil();
                return v_bool(v_truthy(b));
            }
            if (op[0]=='|' && op[1]=='|' && op[2]=='\0'){
                Value a = eval_expr(vm, env, e->as.bin.a);
                if (eval_has_error()) return v_nil();
                if (v_truthy(a)) return v_bool(true);      /* правую часть не считаем */
                Value b = eval_expr(vm, env, e->as.bin.b);
                if (eval_has_error()) return v_nil();
                return v_bool(v_truthy(b));
            }

            /* Остальные бинарные операции — как раньше */
            Value a = eval_expr(vm, env, e->as.bin.a);
            if (eval_has_error()) return v_nil();
            gc_root_push(vm,&a);
            Value b = eval_expr(vm, env, e->as.bin.b);
            if (eval_has_error()) return v_nil();
            return eval_binop(op, a, b, &e->pos);
        }

        case AST_CALL: {
            const char* ns = NULL;
            size_t nn = 0;
            const char* module_alias = NULL;
            size_t module_alias_len = 0;
            const char* module_member = NULL;
            size_t module_member_len = 0;
            char fullname[256]; /* буфер под "модуль.функция" */
            AstFunc* closure_function=NULL;
            KirEnv* function_closure=NULL;
            AstUserType* constructor_type=NULL;
            FuncEntry selected_user_func;
            FuncEntry selected_module_func;
            int has_user_func=0;
            int direct_native=0;
            int has_module_func=0;
            int module_native_call=0;
            int module_ffi=0;
            Value callee_value=v_nil();
            char native_fullname[512];
            size_t native_fullname_len=0;
            ModuleBinding* module=NULL;

            AstExpr* cal = e->as.call.callee;

            if (cal->kind == AST_VAR) {
                /* обычный вызов: имя(...) */
                ns = cal->as.var.name_start;
                nn = cal->as.var.name_len;
                Value callable=v_nil();
                if (env_get(env,ns,nn,&callable)){
                    if (callable.kind!=VAL_FUNCTION ||
                        !v_function_is_owned(vm->value_heap,callable)||
                        !v_function_ast(callable)){
                        kir_diag_error(e->pos.file,e->pos.line,e->pos.col,
                                       "Значение '%.*s' не является функцией",
                                       (int)nn,ns);
                        return v_nil();
                    }
                    callee_value=callable;
                    gc_root_push(vm,&callee_value);
                    closure_function=v_function_ast(callable);
                    function_closure=(KirEnv*)v_function_closure(callable);
                } else {
                    constructor_type=vm_find_type_scoped(vm,env,ns,nn,AST_TYPE_STRUCT);
                    if (!constructor_type){
                        direct_native=kir_native_has_function(ns,nn);
                        if (!direct_native){
                            FuncEntry* found=vm_find_func_scoped(vm,env,ns,nn);
                            if(found){selected_user_func=*found;has_user_func=1;}
                            else {
                                kir_diag_error(e->pos.file,e->pos.line,e->pos.col,
                                               "Нет функции/нативной: %.*s",(int)nn,ns);
                                return v_nil();
                            }
                        }
                    }
                }
            } else if (cal->kind == AST_ATTR) {
                AstExpr* obj = cal->as.attr.obj;

                if (obj->kind == AST_VAR) {
                    const char* ms = obj->as.var.name_start;
                    size_t      mn = obj->as.var.name_len;
                    size_t      an = cal->as.attr.n;
                    Value object=v_nil();
                    module=env_find_module(env,ms,mn);
                    int has_object=env_get(env,ms,mn,&object);
                    int native_module=!module&&!has_object&&kir_native_has_module(ms);
                    if(module||native_module){
                        module_alias=ms;module_alias_len=mn;
                        module_member=cal->as.attr.name;module_member_len=an;
                        if(mn+1+an<sizeof(fullname)){
                            memcpy(fullname,ms,mn);fullname[mn]='.';
                            memcpy(fullname+mn+1,cal->as.attr.name,an);
                            nn=mn+1+an;fullname[nn]='\0';ns=fullname;
                        }else{ns=cal->as.attr.name;nn=an;}
                        if(module){
                            if(module->program&&!module_find_function(module->program,module_member,module_member_len)){
                                kir_diag_error(e->pos.file,e->pos.line,e->pos.col,
                                    "В модуле '%.*s' нет функции '%.*s'",
                                    (int)module_alias_len,module_alias,
                                    (int)module_member_len,module_member);
                                return v_nil();
                            }
                            if(module->program){
                                AstFunc*func=module_find_function(module->program,module_member,module_member_len);
                                selected_module_func.name=module_member;
                                selected_module_func.nn=module_member_len;
                                selected_module_func.file=module->program->source_file;
                                selected_module_func.line=func->pos.line>0?func->pos.line:1;
                                selected_module_func.f=func;
                                selected_module_func.closure=module->closure;
                                has_module_func=1;
                            }else if(module->native_module&&strcmp(module->native_module,"внешний")==0){
                                module_ffi=1;
                            }else if(module->native_module){
                                size_t native_len=strlen(module->native_module);
                                native_fullname_len=native_len+1+module_member_len;
                                if(native_fullname_len>=sizeof(native_fullname)){
                                    kir_diag_error(e->pos.file,e->pos.line,e->pos.col,
                                                   "Слишком длинное имя функции модуля");
                                    return v_nil();
                                }
                                memcpy(native_fullname,module->native_module,native_len);
                                native_fullname[native_len]='.';
                                memcpy(native_fullname+native_len+1,module_member,module_member_len);
                                native_fullname[native_fullname_len]='\0';
                                int exists=kir_native_has_function(native_fullname,native_fullname_len);
                                if(!exists){
                                    kir_diag_error(e->pos.file,e->pos.line,e->pos.col,
                                        "В модуле '%.*s' нет функции '%.*s'",
                                        (int)module_alias_len,module_alias,
                                        (int)module_member_len,module_member);
                                    return v_nil();
                                }
                            }
                            if(module->native_module&&strcmp(module->native_module,"внешний")!=0)
                                module_native_call=1;
                        }else if(native_module){
                            native_fullname_len=mn+1+an;
                            if(native_fullname_len>=sizeof(native_fullname)){
                                kir_diag_error(e->pos.file,e->pos.line,e->pos.col,
                                               "Слишком длинное имя функции модуля");
                                return v_nil();
                            }
                            memcpy(native_fullname,ms,mn);
                            native_fullname[mn]='.';
                            memcpy(native_fullname+mn+1,cal->as.attr.name,an);
                            native_fullname[native_fullname_len]='\0';
                            if(!kir_native_has_function(native_fullname,native_fullname_len)){
                                kir_diag_error(e->pos.file,e->pos.line,e->pos.col,
                                               "Нет функции '%.*s' в модуле '%.*s'",
                                               (int)an,cal->as.attr.name,(int)mn,ms);
                                return v_nil();
                            }
                            module_native_call=1;
                        }
                    }else{
                        callee_value=eval_expr(vm,env,cal);
                        if(eval_has_error())return v_nil();
                        if(callee_value.kind!=VAL_FUNCTION||
                           !v_function_is_owned(vm->value_heap,callee_value)||
                           !v_function_ast(callee_value)){
                            kir_diag_error(e->pos.file,e->pos.line,e->pos.col,
                                           "Значение атрибута не является функцией");
                            return v_nil();
                        }
                        gc_root_push(vm,&callee_value);
                        closure_function=v_function_ast(callee_value);
                        function_closure=(KirEnv*)v_function_closure(callee_value);
                    }
                } else {
                    callee_value=eval_expr(vm,env,cal);
                    if(eval_has_error())return v_nil();
                    if(callee_value.kind!=VAL_FUNCTION||
                       !v_function_is_owned(vm->value_heap,callee_value)||
                       !v_function_ast(callee_value)){
                        kir_diag_error(e->pos.file,e->pos.line,e->pos.col,
                                       "Значение выражения не является функцией");
                        return v_nil();
                    }
                    gc_root_push(vm,&callee_value);
                    closure_function=v_function_ast(callee_value);
                    function_closure=(KirEnv*)v_function_closure(callee_value);
                }
            } else {
                callee_value=eval_expr(vm,env,cal);
                if(eval_has_error())return v_nil();
                if(callee_value.kind!=VAL_FUNCTION||
                   !v_function_is_owned(vm->value_heap,callee_value)||
                   !v_function_ast(callee_value)){
                    kir_diag_error(e->pos.file,e->pos.line,e->pos.col,
                                   "Значение выражения не является функцией");
                    return v_nil();
                }
                gc_root_push(vm,&callee_value);
                closure_function=v_function_ast(callee_value);
                function_closure=(KirEnv*)v_function_closure(callee_value);
            }

            int argc = (int)e->as.call.argc;
            if (constructor_type){
                if ((size_t)argc!=constructor_type->field_count){
                    kir_diag_error(e->pos.file,e->pos.line,e->pos.col,
                        "Структура '%.*s' требует %zu полей, получено %d",
                        (int)constructor_type->name_len,constructor_type->name,
                        constructor_type->field_count,argc);
                    return v_nil();
                }
                Value fields=v_dict_new();
                gc_root_push(vm,&fields);
                for (int i=0;i<argc;++i){
                    const char* field_name=e->as.call.arg_names?
                        e->as.call.arg_names[i]:NULL;
                    size_t field_name_len=e->as.call.arg_name_lens?
                        e->as.call.arg_name_lens[i]:0;
                    AstTypeField* field=field_name?
                        user_type_field(constructor_type,field_name,field_name_len):NULL;
                    if (!field){
                        kir_diag_error(e->pos.file,e->pos.line,e->pos.col,
                                       "Неизвестное поле '%.*s' структуры",
                                       (int)field_name_len,field_name?field_name:"");
                        return v_nil();
                    }
                    if (v_dict_find(&fields,field_name,field_name_len)>=0){
                        kir_diag_error(e->pos.file,e->pos.line,e->pos.col,
                                       "Повторное поле '%.*s' структуры",
                                       (int)field_name_len,field_name);
                        return v_nil();
                    }
                    Value field_value=eval_expr(vm,env,e->as.call.args[i]);
                    if (eval_has_error()) return v_nil();
                    if (!value_matches_type(vm,env,field->type_name,
                                            field->type_name_len,field_value)){
                        kir_diag_error(e->pos.file,e->pos.line,e->pos.col,
                                       "Неверный тип поля '%.*s' структуры '%.*s'",
                                       (int)field->name_len,field->name,
                                       (int)constructor_type->name_len,
                                       constructor_type->name);
                        return v_nil();
                    }
                    v_dict_set(&fields,field_name,field_name_len,field_value);
                }
                Value instance=v_user_struct_new(constructor_type,fields);
                if (instance.kind!=VAL_USER_STRUCT){
                    kir_diag_error(e->pos.file,e->pos.line,e->pos.col,
                                   "Не удалось создать экземпляр структуры");
                    return v_nil();
                }
                return instance;
            }
            for (int i=0;i<argc;++i){
                if (e->as.call.arg_names && e->as.call.arg_names[i]){
                    kir_diag_error(e->pos.file,e->pos.line,e->pos.col,
                                   "Именованные аргументы не поддерживаются для этой функции");
                    return v_nil();
                }
            }
            Value* argv = (Value*)kir_malloc(sizeof(Value) * (size_t)argc);
            for (int i=0; i<argc; ++i){ argv[i]=v_nil(); gc_root_push(vm,&argv[i]); }
            for (int i = 0; i < argc; i++) {
                argv[i] = eval_expr(vm, env, e->as.call.args[i]);
                if (eval_has_error()){
                    kir_free(argv);
                    return v_nil();
                }
            }

            Value out = v_nil();
            /* The opt-in foreign module has a VM-scoped dispatcher; it is
               deliberately not implemented through process-global natives. */
            if (!closure_function && module_ffi){
                ModuleBinding*current_module=env_find_module(env,module_alias,module_alias_len);
                if(!current_module||!current_module->native_module||
                   strcmp(current_module->native_module,"внешний")!=0){
                    kir_free(argv);
                    kir_diag_error(e->pos.file,e->pos.line,e->pos.col,
                                   "Внешний модуль больше не привязан к этому имени");
                    return v_nil();
                }
                int done=0;
                int is_load=0;
                int valid_args=0;
                int recognized=0;
                if (module_member_len==strlen("загрузить") &&
                    !memcmp(module_member,"загрузить",module_member_len)){
                    is_load=1;
                    is_load=1;
                    recognized=1;
                    if (argc==1 && argv[0].kind==VAL_STR &&
                        !memchr(argv[0].as.str.s,0,argv[0].as.str.n)){
                        char* path=k_strdupn(argv[0].as.str.s,argv[0].as.str.n);
                        valid_args=1;
                        done=vm_ffi_load(vm,path);
                        kir_free(path);
                    }
                } else if (module_member_len==strlen("вызвать") &&
                           !memcmp(module_member,"вызвать",module_member_len)){
                    recognized=1;
                    if (argc>=2 && argv[0].kind==VAL_STR && argv[1].kind==VAL_STR &&
                        !memchr(argv[0].as.str.s,0,argv[0].as.str.n) &&
                        !memchr(argv[1].as.str.s,0,argv[1].as.str.n)){
                        char* mod=k_strdupn(argv[0].as.str.s,argv[0].as.str.n);
                        char* func=k_strdupn(argv[1].as.str.s,argv[1].as.str.n);
                        valid_args=1;
                        done=vm_ffi_call(vm,mod,func,argc-2,argv+2,&out);
                        kir_free(mod);
                        kir_free(func);
                    }
                }
                kir_free(argv);
                if (!done){
                    if (!recognized)
                        kir_diag_error(e->pos.file,e->pos.line,e->pos.col,
                                       "В модуле внешний нет такой функции");
                    else if (!valid_args)
                        kir_diag_error(e->pos.file,e->pos.line,e->pos.col,
                                       "Неверные аргументы внешнего вызова: ожидаются имена-строки без NUL");
                    else
                        kir_diag_error(e->pos.file,e->pos.line,e->pos.col,
                            "Ошибка вызова внешней библиотеки: %s",
                            vm_ffi_last_error(vm));
                    return v_nil();
                }
                return is_load ? v_bool(true) : out;
            }

            if (!closure_function && module_native_call){
                int called = kir_native_call(native_fullname,native_fullname_len,argc,argv,&out);
                kir_free(argv);
                if (called) return out;
                kir_diag_error("<вызов>", 1, 1,
                               "Нет функции '%.*s' в модуле '%.*s'",
                               (int)module_member_len, module_member,
                               (int)module_alias_len, module_alias);
                return v_nil();
            }

            /* Нативный модуль без локального alias binding. */
            if (!closure_function && direct_native &&
                kir_native_call(ns, nn, argc, argv, &out)){
                kir_free(argv);
                return out;
            }

            /* Пользовательский модуль ищем через ближайшее лексическое окружение. */
            FuncEntry closure_entry;
            FuncEntry* fe = NULL;
            if (closure_function){
                closure_entry.name=closure_function->name_start;
                closure_entry.nn=closure_function->name_len;
                closure_entry.file=closure_function->pos.file;
                closure_entry.line=closure_function->pos.line>0 ?
                                   closure_function->pos.line : 1;
                closure_entry.f=closure_function;
                closure_entry.closure=function_closure;
                fe=&closure_entry;
            } else if (has_module_func){
                fe = &selected_module_func;
            } else {
                fe = has_user_func ? &selected_user_func : NULL;
            }
            if (fe) {
                FuncEntry selected = *fe;
                fe = &selected;
                if (e->as.call.argc != fe->f->param_count){
                    kir_diag_error(e->pos.file, e->pos.line, e->pos.col,
                                   "Функция '%.*s' ожидает %zu аргументов, получено %zu",
                                   (int)nn, ns, fe->f->param_count, e->as.call.argc);
                    kir_free(argv);
                    return v_nil();
                }
                KirEnv* callenv = env_new(vm,fe->closure ? fe->closure : env);
                size_t envs_before=vm->gc_envs_n;
                gc_env_push(vm,callenv);
                size_t pc = fe->f->param_count;
                for (size_t pi = 0; pi < pc; ++pi) {
                    const char* ps = fe->f->params_start[pi];
                    size_t      pn = fe->f->params_len[pi];
                    v_dict_set(&callenv->dict, ps, pn, argv[pi]);
                }
                hoist_function_statements(vm,callenv,fe->f->body,fe->f->body_len);
                cs_push(fe->f->name_start,fe->file ? fe->file : "<скрипт>",fe->line);
                Ret r = (Ret){0};
                for (size_t bi = 0; bi < fe->f->body_len; ++bi) {
                    r = exec_stmt(vm, callenv, fe->f->body[bi]);
                    if (r.flow == FLOW_RETURN) { out = r.v; break; }
                    if (r.flow != FLOW_NONE){
                        kir_diag_error(e->pos.file, e->pos.line, e->pos.col,
                                       "Управление циклом не может выходить из функции");
                        break;
                    }
                    if (eval_has_error()) break;
                }
                cs_pop();
                vm->gc_envs_n=envs_before;
                env_release_temporary(vm,callenv);
                kir_free(argv);
                return out;
            }

            kir_free(argv);
            kir_diag_error(e->pos.file, e->pos.line, e->pos.col,
                           "Нет функции/нативной: %.*s", (int)nn, ns);
            return v_nil();
        }

        case AST_LIST: {
            Value L = v_list_new();
            gc_root_push(vm,&L);
            for (size_t i=0;i<e->as.list.len;i++){
                Value item = eval_expr(vm, env, e->as.list.items[i]);
                if (eval_has_error()) return v_nil();
                v_list_push(&L, item);
            }
            return L;
        }
        case AST_DICT: {
            Value D = v_dict_new();
            gc_root_push(vm,&D);
            for (size_t i=0;i<e->as.dict.len;i++){
                Value item = eval_expr(vm, env, e->as.dict.vals[i]);
                if (eval_has_error()) return v_nil();
                v_dict_set(&D, e->as.dict.keys_s[i], e->as.dict.keys_n[i], item);
            }
            return D;
        }
        case AST_INDEX: {
            Value base = eval_expr(vm, env, e->as.index.base);
            if (eval_has_error()) return v_nil();
            gc_root_push(vm,&base);
            Value idxv = eval_expr(vm, env, e->as.index.index);
            if (eval_has_error()) return v_nil();
            if (base.kind==VAL_LIST && idxv.kind==VAL_NUM){
                int64_t i = 0;
                if (!v_num_fits_i64(idxv, &i)){
                    kir_diag_error(e->pos.file, e->pos.line, e->pos.col,
                                   "Индекс списка должен быть целым в диапазоне int64");
                    return v_nil();
                }
                if (i>=0 && (uint64_t)i<(uint64_t)base.as.list->len) return base.as.list->data[(size_t)i];
                kir_diag_error(e->pos.file, e->pos.line, e->pos.col, "Индекс списка вне диапазона");
                return v_nil();
            }
            if (base.kind==VAL_STR && idxv.kind==VAL_NUM){
                int64_t index=0;
                if (!v_num_fits_i64(idxv,&index) || index<0){
                    kir_diag_error(e->pos.file,e->pos.line,e->pos.col,
                                   "Индекс строки должен быть неотрицательным целым");
                    return v_nil();
                }
                const char* p=base.as.str.s;
                const char* end=p+base.as.str.n;
                int64_t current=0;
                while (p<end){
                    uint32_t cp=0;
                    int width=kir_utf8_decode(p,end,&cp);
                    if (width<=0){
                        kir_diag_error(e->pos.file,e->pos.line,e->pos.col,
                                       "Строка содержит недопустимый UTF-8");
                        return v_nil();
                    }
                    if (current==index) return v_str_copy(p,width);
                    p+=width;
                    ++current;
                }
                kir_diag_error(e->pos.file,e->pos.line,e->pos.col,
                               "Индекс строки вне диапазона");
                return v_nil();
            }
            if (base.kind==VAL_TYPED_ARRAY){
                int64_t index=0;
                Value result=v_nil();
                if (!v_num_fits_i64(idxv,&index) || index<0 ||
                    !v_typed_array_get(base,(size_t)index,&result)){
                    kir_diag_error(e->pos.file,e->pos.line,e->pos.col,
                                   "Недействительный типизированный массив или индекс вне диапазона");
                    return v_nil();
                }
                return result;
            }
            if (base.kind==VAL_RECORD){
                Value result=v_nil();
                if (idxv.kind!=VAL_STR ||
                    !v_record_get(base,idxv.as.str.s,idxv.as.str.n,&result)){
                    kir_diag_error(e->pos.file,e->pos.line,e->pos.col,
                                   "Неизвестное поле или недействительная запись");
                    return v_nil();
                }
                return result;
            }
            if (base.kind==VAL_DICT && idxv.kind==VAL_STR){
                Value out; if (v_dict_get(&base, idxv.as.str.s, idxv.as.str.n, &out)) return out;
            }
            return v_nil();
        }
        case AST_ATTR: {
            AstExpr* base_expr=e->as.attr.obj;
            Value shadow_value=v_nil();
            if (base_expr->kind==AST_VAR &&
                !env_get(env,base_expr->as.var.name_start,
                         base_expr->as.var.name_len,&shadow_value)){
                AstUserType* enum_type=vm_find_type_scoped(vm,env,
                    base_expr->as.var.name_start,base_expr->as.var.name_len,
                    AST_TYPE_ENUM);
                if (enum_type){
                    for (size_t i=0;i<enum_type->variant_count;++i){
                        if (enum_type->variant_lens[i]==e->as.attr.n &&
                            memcmp(enum_type->variants[i],e->as.attr.name,e->as.attr.n)==0)
                            return v_enum_value_new(enum_type,enum_type->variants[i],
                                                    enum_type->variant_lens[i]);
                    }
                    kir_diag_error(e->pos.file,e->pos.line,e->pos.col,
                                   "Неизвестный вариант перечисления '%.*s'",
                                   (int)e->as.attr.n,e->as.attr.name);
                    return v_nil();
                }
            }
            Value obj = eval_expr(vm, env, e->as.attr.obj);
            if (eval_has_error()) return v_nil();
            if (obj.kind==VAL_USER_STRUCT){
                Value fields=v_user_struct_fields(obj),result=v_nil();
                if (!v_dict_get(&fields,e->as.attr.name,e->as.attr.n,&result))
                    kir_diag_error(e->pos.file,e->pos.line,e->pos.col,
                                   "Неизвестное поле структуры");
                return result;
            }
            if (obj.kind==VAL_RECORD){
                Value result=v_nil();
                if (!v_record_get(obj,e->as.attr.name,e->as.attr.n,&result))
                    kir_diag_error(e->pos.file,e->pos.line,e->pos.col,
                                   "Неизвестное поле или недействительная запись");
                return result;
            }
            if (obj.kind==VAL_DICT){
                Value out; if (v_dict_get(&obj, e->as.attr.name, e->as.attr.n, &out)) return out;
            }
            return v_nil();
        }
    }
    return v_nil();
}

/* helper: сделать нуль-терминированную копию из слайса исходника */
static char* dupz(const char* s, size_t n){
    char* r = (char*)kir_malloc(n + 1);
    if (n) memcpy(r, s, n);
    r[n] = 0;
    return r;
}

/* ==================== ИСПОЛНЕНИЕ ОПЕРАТОРОВ ==================== */

static Ret exec_stmt_inner(VM* vm, KirEnv* env, AstStmt* s){
    if (eval_has_error()) return (Ret){0};
    switch (s->kind){
        case STMT_PRINT: {
            for (size_t i=0;i<s->as.print.argc;i++){
                Value v = eval_expr(vm, env, s->as.print.args[i]);
                if (eval_has_error()) return (Ret){0};
                v_print(v);
                if (i+1<s->as.print.argc) printf(" ");
            }
            printf("\n");
            return (Ret){0};
        }
        case STMT_EXPR: {
            (void)eval_expr(vm, env, s->as.expr);
            return (Ret){0};
        }
        case STMT_FUNCTION:
            /* Function bindings are installed before the enclosing body runs. */
            return (Ret){0};
        case STMT_TRY: {
            kir_diag_silence_push();
            Ret result=exec_block(vm,env,s->as.try_.body,s->as.try_.body_len);
            /* A return value must survive all allocations and GC cycles in
               the catch/finally blocks. exec_stmt restores gc_roots_n when
               the whole try statement returns, including early flow exits. */
            gc_root_push(vm,&result.v);
            if (eval_has_error() && s->as.try_.has_catch){
                Value error=make_error_value();
                g_eval_handled_error_count=kir_diag_error_count();
                KirEnv* catch_env=env_new(vm,env);
                size_t envs_before=vm->gc_envs_n;
                gc_env_push(vm,catch_env);
                env_declare(catch_env,s->as.try_.catch_name,
                            s->as.try_.catch_name_len,error);
                result=exec_block(vm,catch_env,s->as.try_.catch_body,
                                  s->as.try_.catch_body_len);
                vm->gc_envs_n=envs_before;
                env_release_temporary(vm,catch_env);
            }
            if (s->as.try_.has_finally){
                int pending_error=eval_has_error();
                unsigned int handled_before_finally=g_eval_handled_error_count;
                if (pending_error)
                    g_eval_handled_error_count=kir_diag_error_count();
                Ret final_result=exec_block(vm,env,s->as.try_.finally_body,
                                            s->as.try_.finally_body_len);
                int final_error=eval_has_error();
                if (final_error){
                    kir_diag_silence_pop();
                    kir_diag_replay_last_error();
                    return (Ret){0};
                }
                if (final_result.flow!=FLOW_NONE){
                    if (pending_error)
                        g_eval_handled_error_count=kir_diag_error_count();
                    kir_diag_silence_pop();
                    return final_result;
                }
                if (pending_error)
                    g_eval_handled_error_count=handled_before_finally;
            }
            kir_diag_silence_pop();
            if (eval_has_error()) kir_diag_replay_last_error();
            return result;
        }
        case STMT_LET: {
            Value v = eval_expr(vm, env, s->as.let_.value);
            if (eval_has_error()) return (Ret){0};
            env_declare(env, s->as.let_.ns, s->as.let_.nn, v);
            return (Ret){0};
        }
        case STMT_SET: {
            Value v = eval_expr(vm, env, s->as.set.value);
            if (eval_has_error()) return (Ret){0};
            if (!env_assign(env, s->as.set.ns, s->as.set.nn, v)){
                kir_diag_error(s->pos.file, s->pos.line, s->pos.col,
                               "Нельзя присвоить необъявленной переменной '%.*s'",
                               (int)s->as.set.nn, s->as.set.ns);
                return (Ret){0};
            }
            return (Ret){0};
        }
        case STMT_RETURN: {
            Value v = eval_expr(vm, env, s->as.ret.value);
            if (eval_has_error()) return (Ret){0};
            return (Ret){FLOW_RETURN, v};
        }
        case STMT_BREAK:
            return (Ret){FLOW_BREAK, v_nil()};
        case STMT_CONTINUE:
            return (Ret){FLOW_CONTINUE, v_nil()};
        case STMT_IF: {
            Value c = eval_expr(vm, env, s->as.if_.cond);
            if (eval_has_error()) return (Ret){0};
            if (v_truthy(c)){
                return exec_block(vm, env, s->as.if_.then_b, s->as.if_.then_len);
            } else if (s->as.if_.has_else){
                return exec_block(vm, env, s->as.if_.else_b, s->as.if_.else_len);
            }
            return (Ret){0};
        }
        case STMT_WHILE: {
            for (;;){
                Value c = eval_expr(vm, env, s->as.wh.cond);
                if (eval_has_error()) return (Ret){0};
                if (!v_truthy(c)) break;
                Ret r = exec_block(vm, env, s->as.wh.body, s->as.wh.body_len);
                if (r.flow == FLOW_RETURN) return r;
                if (r.flow == FLOW_BREAK) break;
                if (r.flow == FLOW_CONTINUE) continue;
                if (eval_has_error()) return (Ret){0};
            }
            return (Ret){0};
        }
        case STMT_FOREACH: {
            Value seq = eval_expr(vm, env, s->as.fe.seq);
            if (eval_has_error()) return (Ret){0};
            gc_root_push(vm,&seq);
            if (seq.kind==VAL_LIST){
                for (size_t i=0;i<seq.as.list->len;i++){
                    env_assign_or_declare(env, s->as.fe.it, s->as.fe.in, seq.as.list->data[i]);
                    Ret r = exec_block(vm, env, s->as.fe.body, s->as.fe.body_len);
                    if (r.flow == FLOW_RETURN) return r;
                    if (r.flow == FLOW_BREAK) break;
                    if (r.flow == FLOW_CONTINUE) continue;
                    if (eval_has_error()) return (Ret){0};
                }
            } else if (seq.kind==VAL_TYPED_ARRAY){
                size_t count=0;
                if (!v_typed_array_length(seq,&count)){
                    kir_diag_error(s->pos.file,s->pos.line,s->pos.col,
                                   "Нельзя перебрать недействительный типизированный массив");
                    return (Ret){0};
                }
                for (size_t i=0;i<count;++i){
                    Value item=v_nil();
                    if (!v_typed_array_get(seq,i,&item)){
                        kir_diag_error(s->pos.file,s->pos.line,s->pos.col,
                                       "Типизированный массив был закрыт или сброшен во время перебора");
                        return (Ret){0};
                    }
                    env_assign_or_declare(env,s->as.fe.it,s->as.fe.in,item);
                    Ret r=exec_block(vm,env,s->as.fe.body,s->as.fe.body_len);
                    if (r.flow==FLOW_RETURN) return r;
                    if (r.flow==FLOW_BREAK) break;
                    if (eval_has_error()) return (Ret){0};
                    if (r.flow==FLOW_CONTINUE) continue;
                }
            } else if (seq.kind==VAL_STR){
                const char* p   = seq.as.str.s;
                const char* end = p + seq.as.str.n;
                while (p<end){
                    uint32_t cp=0; int adv = kir_utf8_decode(p, end, &cp);
                    if (adv<=0) adv=1;
                    Value ch = v_str_copy(p, adv);
                    env_assign_or_declare(env, s->as.fe.it, s->as.fe.in, ch);
                    Ret r = exec_block(vm, env, s->as.fe.body, s->as.fe.body_len);
                    p += adv;
                    if (r.flow == FLOW_RETURN) return r;
                    if (r.flow == FLOW_BREAK) break;
                    if (r.flow == FLOW_CONTINUE) continue;
                    if (eval_has_error()) return (Ret){0};
                }
            }
            return (Ret){0};
        }
        case STMT_SETINDEX: {
            AstExpr* base_expr = s->as.sidx.base;
            Value base = eval_expr(vm, env, base_expr);
            if (eval_has_error()) return (Ret){0};
            gc_root_push(vm,&base);
            Value idxv = eval_expr(vm, env, s->as.sidx.idx);
            if (eval_has_error()) return (Ret){0};
            gc_root_push(vm,&idxv);
            Value val  = eval_expr(vm, env, s->as.sidx.value);
            if (eval_has_error()) return (Ret){0};

            if (base.kind==VAL_LIST && idxv.kind==VAL_NUM){
                int64_t i=0;
                if (!v_num_fits_i64(idxv, &i)){
                    kir_diag_error(s->pos.file, s->pos.line, s->pos.col,
                                   "Индекс списка должен быть целым в диапазоне int64");
                } else if (i>=0 && (uint64_t)i<(uint64_t)base.as.list->len){
                    base.as.list->data[(size_t)i]=val;
                } else {
                    kir_diag_error(s->pos.file, s->pos.line, s->pos.col,
                                   "Индекс списка вне диапазона");
                }
            } else if (base.kind==VAL_TYPED_ARRAY){
                int64_t index=0;
                if (!v_num_fits_i64(idxv,&index) || index<0 ||
                    !v_typed_array_set(base,(size_t)index,val))
                    kir_diag_error(s->pos.file,s->pos.line,s->pos.col,
                                   "Неверный тип, индекс или недействительный типизированный массив");
            } else if (base.kind==VAL_RECORD){
                if (idxv.kind!=VAL_STR ||
                    !v_record_set(base,idxv.as.str.s,idxv.as.str.n,val))
                    kir_diag_error(s->pos.file,s->pos.line,s->pos.col,
                                   "Неверный тип, поле или недействительная запись");
            } else if (base.kind==VAL_DICT && idxv.kind==VAL_STR){
                v_dict_set(&base, idxv.as.str.s, idxv.as.str.n, val);
            } else {
                kir_diag_error(s->pos.file, s->pos.line, s->pos.col,
                               "Неверные типы для присваивания по индексу");
            }

            /* Если базовое выражение — просто переменная, записываем обновлённый Value обратно. */
            if (base_expr->kind == AST_VAR &&
                (base.kind == VAL_LIST || base.kind == VAL_DICT)){
                const char* ns = base_expr->as.var.name_start;
                size_t      nn = base_expr->as.var.name_len;
                env_write_back(env, ns, nn, base);
            }

            return (Ret){0};
        }
        case STMT_SETATTR: {
            AstExpr* obj_expr = s->as.sattr.obj;
            Value obj = eval_expr(vm, env, obj_expr);
            if (eval_has_error()) return (Ret){0};
            if (obj.kind != VAL_DICT && obj.kind != VAL_RECORD &&
                obj.kind != VAL_USER_STRUCT){
                kir_diag_error(s->pos.file, s->pos.line, s->pos.col,
                               "Присваивание полей возможно только для словаря или записи");
                return (Ret){0};
            }
            gc_root_push(vm,&obj);
            Value val = eval_expr(vm, env, s->as.sattr.value);
            if (eval_has_error()) return (Ret){0};
            if (obj.kind==VAL_RECORD){
                if (!v_record_set(obj,s->as.sattr.name,s->as.sattr.n,val))
                    kir_diag_error(s->pos.file,s->pos.line,s->pos.col,
                                   "Неверный тип, поле или недействительная запись");
                return (Ret){0};
            }
            if (obj.kind==VAL_USER_STRUCT){
                AstUserType* type=v_user_struct_type(obj);
                AstTypeField* field=user_type_field(type,s->as.sattr.name,s->as.sattr.n);
                if (!field || !value_matches_type(vm,env,field->type_name,
                                                  field->type_name_len,val)){
                    kir_diag_error(s->pos.file,s->pos.line,s->pos.col,
                                   "Неверный тип или поле пользовательской структуры");
                    return (Ret){0};
                }
                Value fields=v_user_struct_fields(obj);
                v_dict_set(&fields,s->as.sattr.name,s->as.sattr.n,val);
                return (Ret){0};
            }
            v_dict_set(&obj, s->as.sattr.name, s->as.sattr.n, val);

            /* Для простого случая: имя.поле = ...  — возвращаем изменённый словарь в окружение. */
            if (obj_expr->kind == AST_VAR){
                const char* ns = obj_expr->as.var.name_start;
                size_t      nn = obj_expr->as.var.name_len;
                env_write_back(env, ns, nn, obj);
            }

            return (Ret){0};
        }
        case STMT_INCLUDE: {
            char* path_z = dupz(s->as.inc.path, s->as.inc.pn);

            size_t fl=0; char* txt = kir_read_text_file(path_z, &fl);
            if (!txt){
                kir_diag_error(path_z, 1, 1, "Не удалось открыть для включения");
                kir_free(path_z);
                return (Ret){0};
            }
            sc_cache_file(path_z, txt, (int)fl);
            unsigned int errors_before = kir_diag_error_count();
            KirBigIntHeap* prev_heap=v_bigint_heap_set_current(NULL);
            AstProgram* sub = parse_program(txt, fl, path_z);
            vm_own_program(vm,sub);
            v_bigint_heap_set_current(prev_heap);
            if (kir_diag_error_count() == errors_before){
                (void)vm_eval_program_into((VM*)vm, sub, env);
            }

            kir_free(path_z);
            kir_free(txt);
            return (Ret){0};
        }
        case STMT_IMPORT: {
            char* name_z = dupz(s->as.imp.name,  s->as.imp.nn);
            char* alias_z = NULL;
            char* from_dir_z = NULL;

            if (s->as.imp.alias && s->as.imp.an)
                alias_z = dupz(s->as.imp.alias, s->as.imp.an);
#ifdef KIRTON_AST_IMPORT_HAS_FROM
            if (s->as.imp.from_dir && s->as.imp.from_dir_len)
                from_dir_z = dupz(s->as.imp.from_dir, s->as.imp.from_dir_len);
#endif
            Value dict = v_nil();
            (void)vm_import((VM*)vm, name_z, alias_z, from_dir_z, env, &dict);

            kir_free(name_z);
            if (alias_z)    kir_free(alias_z);
            if (from_dir_z) kir_free(from_dir_z);
            return (Ret){0};
        }
    }
    return (Ret){0};
}

static Ret exec_stmt(VM* vm, KirEnv* env, AstStmt* s){
    size_t roots_before=vm->gc_roots_n;
    Ret r=exec_stmt_inner(vm,env,s);
    vm->gc_roots_n=roots_before;
    return r;
}

static Ret exec_block(VM* vm, KirEnv* env, AstStmt** body, size_t blen){
    for (size_t i=0;i<blen;i++){
        if (eval_has_error()) break;
        Ret r = exec_stmt(vm, env, body[i]);
        if (r.flow != FLOW_NONE) return r;
        if (eval_has_error()) break;
        /* Collect only after a completed statement. Enclosing frames and
           intermediate expressions have registered their live roots. */
        if (vm->bigint_heap &&
            (kir_bigint_heap_count(vm->bigint_heap)+v_value_heap_count(vm->value_heap)
                >= vm->gc_threshold ||
             v_value_heap_arena_bytes(vm->value_heap)>=vm->gc_arena_threshold))
            gc_collect(vm,0);
    }
    return (Ret){0};
}

/* ==================== ПУБЛИЧНЫЙ API ==================== */

int eval_program(AstProgram* p){
    if (!p || p->parse_errors) return 1;
    unsigned int errors_before = kir_diag_error_count();
    g_eval_error_baseline = errors_before;
    g_eval_handled_error_count=errors_before;
    VM* vm = vm_new();
    if(!vm){
        kir_diag_error(p->source_file,1,1,"Не удалось создать AST Runtime: недостаточно памяти");
        return 1;
    }
    if (g_ffi_cli_enabled) vm_ffi_set_enabled(vm,1);
    KirEnv* root = env_new(vm,NULL);
    vm->root_env = root;
    (void)vm_eval_program_into(vm, p, root);

    if (!eval_has_error() && g_entry && *g_entry){
        if (!vm_has_func(vm, g_entry)){
            kir_diag_error("<вход>", 1, 1, "Неизвестная функция входа '%s'", g_entry);
        } else {
            Value out=v_nil();
            (void)vm_call_by_name(vm, g_entry, 0, NULL, &out);
        }
    }
    int rc=eval_has_error() ? 1 : 0;
    vm_free(vm);
    return rc;
}

VM* vm_new(void){
    VM* vm = (VM*)kir_malloc(sizeof(VM));
    memset(vm, 0, sizeof(*vm));
    vm->bigint_heap=kir_bigint_heap_new();
    vm->value_heap=v_value_heap_new();
    vm->ffi=kir_ffi_state_create();
    if(!vm->ffi){
        v_value_heap_destroy(vm->value_heap);
        kir_bigint_heap_destroy(vm->bigint_heap);
        kir_free(vm);
        return NULL;
    }
    vm->gc_threshold=256;
    vm->gc_arena_threshold=16u*1024u*1024u;
    return vm;
}

Value vm_value_list_new(VM* vm){
    if (!vm || v_value_heap_is_finalizing(vm->value_heap)) return v_nil();
    KirValueHeap* previous=v_value_heap_set_current(vm->value_heap);
    Value value=v_list_new();
    v_value_heap_set_current(previous);
    return value;
}

Value vm_value_dict_new(VM* vm){
    if (!vm || v_value_heap_is_finalizing(vm->value_heap)) return v_nil();
    KirValueHeap* previous=v_value_heap_set_current(vm->value_heap);
    Value value=v_dict_new();
    v_value_heap_set_current(previous);
    return value;
}

Value vm_value_user_struct_new(VM* vm,AstUserType* type,Value fields){
    if (!vm || !type || v_value_heap_is_finalizing(vm->value_heap)) return v_nil();
    KirValueHeap* previous=v_value_heap_set_current(vm->value_heap);
    Value value=v_user_struct_new(type,fields);
    v_value_heap_set_current(previous);
    return value;
}

Value vm_value_enum_new(VM* vm,AstUserType* type,const char* variant,size_t len){
    if (!vm || !type || v_value_heap_is_finalizing(vm->value_heap)) return v_nil();
    KirValueHeap* previous=v_value_heap_set_current(vm->value_heap);
    Value value=v_enum_value_new(type,variant,len);
    v_value_heap_set_current(previous);
    return value;
}

int vm_value_native_call(VM* vm,const char* name,size_t name_length,
                         int argc,const Value* argv,Value* out){
    if (!vm || !name || argc<0 || (argc && !argv) || !out ||
        v_value_heap_is_finalizing(vm->value_heap)) return 0;
    KirValueHeap* previous_values=v_value_heap_set_current(vm->value_heap);
    KirBigIntHeap* previous_bigints=v_bigint_heap_set_current(vm->bigint_heap);
    int result=kir_native_call(name,name_length,argc,argv,out);
    v_bigint_heap_set_current(previous_bigints);
    v_value_heap_set_current(previous_values);
    return result;
}

typedef struct {
    void* callback_descriptor;
    Value captures;
} KirCompiledClosure;

static void gc_trace_compiled_closure(void* context,void* closure){
    VM* vm=(VM*)context;
    KirCompiledClosure* cc=(KirCompiledClosure*)closure;
    if(vm && cc) v_value_heap_mark(vm->value_heap,vm->bigint_heap,cc->captures);
}

static void gc_release_compiled_closure(void* context,void* closure){
    (void)context;
    kir_free(closure);
}

void* vm_value_compiled_descriptor(Value function){
    if(function.kind!=VAL_FUNCTION)return NULL;
    KirCompiledClosure* cc=(KirCompiledClosure*)v_function_closure(function);
    return cc ? cc->callback_descriptor:NULL;
}

Value vm_value_compiled_captures(Value function){
    if(function.kind!=VAL_FUNCTION)return v_nil();
    KirCompiledClosure* cc=(KirCompiledClosure*)v_function_closure(function);
    return cc ? cc->captures:v_nil();
}

int vm_value_is_compiled_function(VM* vm,Value function){
    return vm && v_function_is_owned(vm->value_heap,function);
}

Value vm_value_compiled_function_new(VM* vm,AstFunc* identity,
                                     void* descriptor,Value captures){
    if (!vm || !identity || !descriptor ||
        v_value_heap_is_finalizing(vm->value_heap)) return v_nil();
    KirCompiledClosure* cc=(KirCompiledClosure*)kir_malloc(sizeof(*cc));
    cc->callback_descriptor=descriptor;
    cc->captures=captures;
    KirValueHeap* previous=v_value_heap_set_current(vm->value_heap);
    Value function=v_function_new(identity,cc,gc_trace_compiled_closure,
                                  gc_release_compiled_closure,vm);
    v_value_heap_set_current(previous);
    if(function.kind!=VAL_FUNCTION)kir_free(cc);
    return function;
}

Value vm_value_string_new(VM* vm,const char* utf8,size_t length){
    if (!vm || (!utf8 && length) || length>(size_t)INT_MAX ||
        v_value_heap_is_finalizing(vm->value_heap)) return v_nil();
    KirValueHeap* previous_values=v_value_heap_set_current(vm->value_heap);
    KirBigIntHeap* previous_bigints=v_bigint_heap_set_current(vm->bigint_heap);
    Value value=v_str_copy(utf8 ? utf8 : "",(int)length);
    v_bigint_heap_set_current(previous_bigints);
    v_value_heap_set_current(previous_values);
    return value;
}

int vm_value_number_parse(VM* vm,const char* text,size_t length,
                          int is_integer,Value* out){
    if (!vm || !out || (!text && length) || length>SIZE_MAX-1 ||
        v_value_heap_is_finalizing(vm->value_heap)) return 0;
    KirValueHeap* previous_values=v_value_heap_set_current(vm->value_heap);
    KirBigIntHeap* previous_bigints=v_bigint_heap_set_current(vm->bigint_heap);
    int ok=1;
    Value value=v_nil();
    if (is_integer){
        value=v_int_parse(text,length,10);
    } else {
        char* terminated=(char*)kir_malloc(length+1);
        if (length) memcpy(terminated,text,length);
        terminated[length]='\0';
        char* end=NULL;
        double number=strtod(terminated,&end);
        ok=end==terminated+length;
        kir_free(terminated);
        if (ok) value=v_num(number);
    }
    v_bigint_heap_set_current(previous_bigints);
    v_value_heap_set_current(previous_values);
    if (!ok) return 0;
    *out=value;
    return 1;
}

int vm_value_numeric_binary(VM* vm,KirNumericOperation operation,
                            Value left,Value right,Value* out){
    if (!vm || !out || left.kind!=VAL_NUM || right.kind!=VAL_NUM ||
        v_value_heap_is_finalizing(vm->value_heap)) return KIR_NUMERIC_INVALID;
    KirValueHeap* previous_values=v_value_heap_set_current(vm->value_heap);
    KirBigIntHeap* previous_bigints=v_bigint_heap_set_current(vm->bigint_heap);
    Value result=v_nil();
    int status=KIR_NUMERIC_OK;
    int comparison=0;
    switch (operation){
        case KIR_NUM_ADD: result=v_num_add(left,right); break;
        case KIR_NUM_SUB: result=v_num_sub(left,right); break;
        case KIR_NUM_MUL: result=v_num_mul(left,right); break;
        case KIR_NUM_DIV: {
            double denominator=v_num_to_double(right);
            if (denominator==0.0) status=KIR_NUMERIC_DIVIDE_BY_ZERO;
            else result=v_num(v_num_to_double(left)/denominator);
            break;
        }
        case KIR_NUM_FLOOR_DIV:
        case KIR_NUM_MOD:
            if (v_num_compare(right,v_int(0),&comparison) && comparison==0)
                status=KIR_NUMERIC_DIVIDE_BY_ZERO;
            else result=operation==KIR_NUM_FLOOR_DIV ?
                v_num_floor_div(left,right) : v_num_mod(left,right);
            break;
        case KIR_NUM_BITAND:
        case KIR_NUM_BITOR:
        case KIR_NUM_BITXOR:
            if (!v_num_is_integer(left) || !v_num_is_integer(right))
                status=KIR_NUMERIC_INVALID;
            else result=v_num_bitwise(left,right,
                operation==KIR_NUM_BITAND ? '&' :
                operation==KIR_NUM_BITOR ? '|' : '^');
            break;
        case KIR_NUM_SHL:
        case KIR_NUM_SHR: {
            int64_t shift=0;
            if (!v_num_is_integer(left) || !v_num_is_integer(right) ||
                !v_num_fits_i64(right,&shift) || shift<0 ||
                (uint64_t)shift>(uint64_t)SIZE_MAX)
                status=KIR_NUMERIC_INVALID;
            else result=v_num_shift(left,(size_t)shift,operation==KIR_NUM_SHL);
            break;
        }
        case KIR_NUM_EQ: result=v_bool(v_num_equal(left,right)); break;
        case KIR_NUM_NE: result=v_bool(!v_num_equal(left,right)); break;
        case KIR_NUM_LT:
        case KIR_NUM_LE:
        case KIR_NUM_GT:
        case KIR_NUM_GE: {
            int comparable=v_num_compare(left,right,&comparison);
            result=v_bool(comparable &&
                (operation==KIR_NUM_LT ? comparison<0 :
                 operation==KIR_NUM_LE ? comparison<=0 :
                 operation==KIR_NUM_GT ? comparison>0 : comparison>=0));
            break;
        }
        default: status=KIR_NUMERIC_INVALID; break;
    }
    v_bigint_heap_set_current(previous_bigints);
    v_value_heap_set_current(previous_values);
    if (status==KIR_NUMERIC_OK) *out=result;
    return status;
}

int vm_value_numeric_neg(VM* vm,Value input,Value* out){
    if (!vm || !out || input.kind!=VAL_NUM ||
        v_value_heap_is_finalizing(vm->value_heap)) return 0;
    KirValueHeap* previous_values=v_value_heap_set_current(vm->value_heap);
    KirBigIntHeap* previous_bigints=v_bigint_heap_set_current(vm->bigint_heap);
    *out=v_num_neg(input);
    v_bigint_heap_set_current(previous_bigints);
    v_value_heap_set_current(previous_values);
    return 1;
}


int vm_value_numeric_bitnot(VM* vm,Value input,Value* out){
    if (!vm || !out || !v_num_is_integer(input) ||
        v_value_heap_is_finalizing(vm->value_heap)) return 0;
    KirValueHeap* previous_values=v_value_heap_set_current(vm->value_heap);
    KirBigIntHeap* previous_bigints=v_bigint_heap_set_current(vm->bigint_heap);
    *out=v_num_bitnot(input);
    v_bigint_heap_set_current(previous_bigints);
    v_value_heap_set_current(previous_values);
    return 1;
}

int vm_ffi_set_enabled(VM* vm,int enabled){
    if (!vm || vm->ffi_calls || v_value_heap_is_finalizing(vm->value_heap)) return 0;
    vm->ffi_enabled=(enabled!=0);
    return 1;
}
const char* vm_ffi_last_error(const VM* vm){
    if (!vm || !vm->ffi_enabled) return "Внешние библиотеки отключены (нужно --allow-ffi)";
    return kir_ffi_state_error(vm->ffi);
}
int vm_ffi_load(VM* vm, const char* path){
    if (!vm || !vm->ffi_enabled || !vm->ffi || !path || vm->ffi_calls ||
        v_value_heap_is_finalizing(vm->value_heap)) return 0;
    ++vm->ffi_calls;
    int ok=kir_ffi_state_load(vm->ffi,path);
    --vm->ffi_calls;
    return ok;
}
int vm_ffi_call(VM* vm,const char* module,const char* function,
                int argc,const Value* argv,Value* out){
    if (!vm || !vm->ffi_enabled || !vm->ffi || !module || !function || !out ||
        argc<0 || (argc && !argv) || vm->ffi_calls ||
        v_value_heap_is_finalizing(vm->value_heap)) return 0;
    for (int i=0;i<argc;++i){
        if (!v_value_heap_accepts_value(vm->value_heap,vm->bigint_heap,argv[i])) return 0;
    }
    /* Plugin invocation cannot re-enter the VM or destroy it. */
    ++vm->ffi_calls;
    KirValueHeap* prev=v_value_heap_set_current(vm->value_heap);
    KirBigIntHeap* prev_int=v_bigint_heap_set_current(vm->bigint_heap);
    int ok=kir_ffi_state_call(vm->ffi,vm->value_heap,module,function,argc,argv,out);
    v_bigint_heap_set_current(prev_int);
    v_value_heap_set_current(prev);
    --vm->ffi_calls;
    return ok;
}

size_t vm_gc_collect(VM* vm){
    if (!vm || vm->ffi_calls) return 0;
    size_t previous=vm->gc_reclaimed;
    gc_collect(vm,0);
    return vm->gc_reclaimed-previous;
}

size_t vm_gc_tracked(const VM* vm){
    return vm ? kir_bigint_heap_count(vm->bigint_heap)+v_value_heap_count(vm->value_heap) : 0;
}

size_t vm_gc_collections(const VM* vm){ return vm ? vm->gc_collections : 0; }
size_t vm_memory_arena_bytes(const VM* vm){
    return vm ? v_value_heap_arena_bytes(vm->value_heap) : 0;
}
int vm_memory_set_arena_limit(VM* vm, size_t maximum_bytes){
    return vm && !v_value_heap_is_finalizing(vm->value_heap) ?
           v_value_heap_set_arena_limit(vm->value_heap,maximum_bytes) : 0;
}

/* Native resource handles intentionally do not expose persistent raw
   pointers. The visiting callback is synchronous and strongly rooted. */
Value vm_native_resource_adopt(VM* vm, void* handle, const char* tag,
                               KirNativeResourceDestroy destroy, void* user_data){
    if (!vm || v_value_heap_is_finalizing(vm->value_heap)) return v_nil();
    return v_native_resource_adopt(vm->value_heap,handle,tag,destroy,user_data);
}

int vm_native_resource_is_open(VM* vm, Value resource, const char* tag){
    return vm && !v_value_heap_is_finalizing(vm->value_heap) &&
           v_native_resource_is_open(vm->value_heap,resource,tag);
}

int vm_native_resource_close(VM* vm, Value resource){
    return vm && !v_value_heap_is_finalizing(vm->value_heap) &&
           v_native_resource_close(vm->value_heap,resource);
}

int vm_native_resource_visit(VM* vm, Value resource, const char* tag,
                             KirNativeResourceVisit visitor, void* user_data){
    if (!vm || !visitor || !tag ||
        !vm_native_resource_is_open(vm,resource,tag)) return 0;
    KirPinnedValue* pin=vm_value_pin(vm,resource);
    if (!pin) return 0;
    ++vm->native_visits;
    int result=v_native_resource_visit(vm->value_heap,resource,tag,visitor,user_data);
    --vm->native_visits;
    vm_value_unpin(vm,pin);
    return result;
}

KirNativeResourceLease* vm_native_resource_lease_begin(VM* vm, Value resource,
                                                       const char* type_tag){
    if (!vm || !type_tag || !*type_tag || vm->ffi_calls ||
        v_value_heap_is_finalizing(vm->value_heap) ||
        !vm_native_resource_is_open(vm,resource,type_tag) ||
        vm->resource_lease_count==SIZE_MAX) return NULL;
    KirPinnedValue* pin=vm_value_pin(vm,resource);
    if (!pin) return NULL;
    if (!v_native_resource_lease_start(vm->value_heap,resource,type_tag)){
        vm_value_unpin(vm,pin);
        return NULL;
    }
    KirNativeResourceLease* lease=(KirNativeResourceLease*)kir_malloc(sizeof(*lease));
    *lease=(KirNativeResourceLease){resource,pin,k_strdup(type_tag),false,vm->resource_leases};
    vm->resource_leases=lease;
    ++vm->resource_lease_count;
    return lease;
}

int vm_native_resource_lease_visit(VM* vm, KirNativeResourceLease* lease,
                                  KirNativeResourceVisit visitor, void* user_data){
    if (!vm || !lease || !visitor ||
        v_value_heap_is_finalizing(vm->value_heap)) return 0;
    KirNativeResourceLease* current=vm->resource_leases;
    while (current && current!=lease) current=current->next;
    if (!current || current->visiting) return 0;
    current->visiting=true;
    int ok=vm_native_resource_visit(vm,current->value,current->type_tag,
                                    visitor,user_data);
    current->visiting=false;
    return ok;
}

int vm_native_resource_lease_release(VM* vm, KirNativeResourceLease* lease){
    if (!vm || !lease || v_value_heap_is_finalizing(vm->value_heap)) return 0;
    KirNativeResourceLease** link=&vm->resource_leases;
    while (*link && *link!=lease) link=&(*link)->next;
    if (!*link || lease->visiting) return 0;
    if (!v_native_resource_lease_end(vm->value_heap,lease->value)) return 0;
    *link=lease->next;
    --vm->resource_lease_count;
    vm_value_unpin(vm,lease->pin);
    kir_free(lease->type_tag);
    kir_free(lease);
    return 1;
}
size_t vm_native_resource_lease_count(const VM* vm){
    return vm ? vm->resource_lease_count : 0;
}

/* VM-owned byte copies are deliberately O(n): no untracked raw pointer
   escapes to foreign code, and preflight ensures invalid spans are atomic. */
static int vm_memory_accepts(VM* vm, Value value, ValKind expected){
    return vm && !v_value_heap_is_finalizing(vm->value_heap) && value.kind==expected &&
           v_value_heap_accepts_memory_handle(vm->value_heap,value);
}

int vm_memory_buffer_read(VM* vm, Value buffer, size_t offset,
                          void* destination, size_t bytes){
    size_t length=0;
    if (!vm_memory_accepts(vm,buffer,VAL_BUFFER) ||
        !v_buffer_length(buffer,&length) || offset>length ||
        bytes>length-offset || (bytes && !destination)) return 0;
    uint8_t* dst=(uint8_t*)destination;
    for (size_t i=0;i<bytes;++i){
        if (!v_buffer_get_u8(buffer,offset+i,&dst[i])) return 0;
    }
    return 1;
}

int vm_memory_buffer_write(VM* vm, Value buffer, size_t offset,
                           const void* source, size_t bytes){
    size_t length=0;
    if (!vm_memory_accepts(vm,buffer,VAL_BUFFER) ||
        !v_buffer_length(buffer,&length) || offset>length ||
        bytes>length-offset || (bytes && !source)) return 0;
    const uint8_t* src=(const uint8_t*)source;
    for (size_t i=0;i<bytes;++i){
        if (!v_buffer_set_u8(buffer,offset+i,src[i])) return 0;
    }
    return 1;
}

int vm_memory_array_get(VM* vm, Value array, size_t index, Value* out){
    return vm_memory_accepts(vm,array,VAL_TYPED_ARRAY) &&
           v_typed_array_get(array,index,out);
}
int vm_memory_array_set(VM* vm, Value array, size_t index, Value number){
    return vm_memory_accepts(vm,array,VAL_TYPED_ARRAY) &&
           v_typed_array_set(array,index,number);
}
int vm_memory_record_get(VM* vm, Value record, const char* field,
                         size_t field_length, Value* out){
    return vm_memory_accepts(vm,record,VAL_RECORD) && field &&
           v_record_get(record,field,field_length,out);
}
int vm_memory_record_set(VM* vm, Value record, const char* field,
                         size_t field_length, Value number){
    return vm_memory_accepts(vm,record,VAL_RECORD) && field &&
           v_record_set(record,field,field_length,number);
}

KirMemoryBorrow* vm_memory_borrow(VM* vm, Value value, KirBorrowMode mode){
    /* Filter unsupported kinds before calling the general deep validator:
       a stale non-memory C Value must not cause traversal of a raw list. */
    if (!vm || v_value_heap_is_finalizing(vm->value_heap) ||
        (mode!=KIR_BORROW_READ && mode!=KIR_BORROW_WRITE) ||
        (value.kind!=VAL_BUFFER && value.kind!=VAL_TYPED_ARRAY &&
         value.kind!=VAL_RECORD) ||
        !v_value_heap_accepts_memory_handle(vm->value_heap,value)) return NULL;
    const void* data=NULL;
    size_t bytes=0;
    bool write=mode==KIR_BORROW_WRITE;
    if (!v_memory_borrow_begin(vm->value_heap,value,write,&data,&bytes)) return NULL;
    KirPinnedValue* pin=vm_value_pin(vm,value);
    if (!pin){
        (void)v_memory_borrow_end(vm->value_heap,value,write);
        return NULL;
    }
    KirMemoryBorrow* borrow=(KirMemoryBorrow*)kir_malloc(sizeof(*borrow));
    *borrow=(KirMemoryBorrow){value,pin,data,bytes,write,vm->borrows};
    vm->borrows=borrow;
    ++vm->borrow_count;
    return borrow;
}

const void* vm_memory_borrow_data(const KirMemoryBorrow* borrow){
    return borrow ? borrow->data : NULL;
}
void* vm_memory_borrow_mutable_data(const KirMemoryBorrow* borrow){
    return borrow && borrow->writable ? (void*)borrow->data : NULL;
}
size_t vm_memory_borrow_size(const KirMemoryBorrow* borrow){
    return borrow ? borrow->bytes : 0;
}
size_t vm_memory_borrow_count(const VM* vm){
    return vm ? vm->borrow_count : 0;
}
int vm_memory_borrow_release(VM* vm, KirMemoryBorrow* borrow){
    if (!vm || !borrow) return 0;
    KirMemoryBorrow** link=&vm->borrows;
    while (*link && *link!=borrow) link=&(*link)->next;
    if (!*link) return 0;
    if (!v_memory_borrow_end(vm->value_heap,borrow->value,borrow->writable)) return 0;
    *link=borrow->next;
    vm_value_unpin(vm,borrow->pin);
    kir_free(borrow);
    --vm->borrow_count;
    return 1;
}

KirPinnedValue* vm_value_pin(VM* vm, Value value){
    if (!vm || v_value_heap_is_finalizing(vm->value_heap) ||
        !v_value_heap_accepts_value(vm->value_heap,vm->bigint_heap,value)) return NULL;
    KirPinnedValue* pin=(KirPinnedValue*)kir_malloc(sizeof(*pin));
    pin->value=value;
    pin->next=vm->pinned;
    vm->pinned=pin;
    return pin;
}

Value vm_value_pinned(const KirPinnedValue* pin){ return pin ? pin->value : v_nil(); }

void vm_value_unpin(VM* vm, KirPinnedValue* pin){
    if (!vm || !pin) return;
    KirPinnedValue** link=&vm->pinned;
    while (*link && *link!=pin) link=&(*link)->next;
    if (*link){ *link=pin->next; kir_free(pin); }
}

int vm_try_free(VM* vm){
    if (!vm) return 1;
    if (vm->borrow_count || vm->native_visits || vm->resource_lease_count || vm->ffi_calls ||
        v_value_heap_is_finalizing(vm->value_heap)) return 0;
    while (vm->pinned){
        KirPinnedValue* next=vm->pinned->next;
        kir_free(vm->pinned);
        vm->pinned=next;
    }
    /* Keep plugin code loaded until native-resource finalizers have run.
       A host may have adopted a handle whose cleanup callback lives in a
       loaded plugin. Unloading first would call unmapped code at VM exit. */
    for (size_t i=0;i<vm->fn;++i) kir_free((void*)vm->ftab[i].name);
    for (size_t i=0;i<vm->mods_n;++i){
        kir_free(vm->mods[i].key);
        kir_free(vm->mods[i].module_name);
    }
    for (size_t i=0;i<vm->loading_n;++i) kir_free(vm->loading[i]);
    /* Function finalizers release their captured environments. */
    v_value_heap_destroy(vm->value_heap);
    while (vm->owned_envs){
        KirEnv* next=vm->owned_envs->owned_next;
        env_free_struct(vm->owned_envs);
        vm->owned_envs=next;
    }
    kir_free(vm->ftab);
    kir_free(vm->types);
    kir_free(vm->mods);
    kir_free(vm->loading);
    kir_free(vm->gc_roots);
    kir_free(vm->gc_envs);
    for (size_t i=0;i<vm->owned_programs_n;++i)
        ast_program_destroy(vm->owned_programs[i]);
    kir_free(vm->owned_programs);
    kir_ffi_state_destroy(vm->ffi);
    /* Указатель текущей кучи не должен переживать удаление VM. */
    KirBigIntHeap* active=v_bigint_heap_set_current(NULL);
    if (active && active!=vm->bigint_heap)
        v_bigint_heap_set_current(active);
    kir_bigint_heap_destroy(vm->bigint_heap);
    kir_free(vm);
    return 1;
}

void vm_free(VM* vm){
    if (!vm_try_free(vm))
        fprintf(stderr, "Kirton: vm_free отклонён — есть активные заимствования или использование нативного ресурса\n");
}

KirEnv* vm_run_root(VM* vm, const char* path_utf8){
    if (!vm || !path_utf8 || vm->ffi_calls ||
        v_value_heap_is_finalizing(vm->value_heap)) return NULL;
    size_t n=0; char* txt = kir_read_text_file(path_utf8, &n);
    if (!txt){
        kir_diag_error(path_utf8, 1, 1, "Не удалось открыть файл");
        return NULL;
    }
    sc_cache_file(path_utf8, txt, (int)n);
    unsigned int errors_before = kir_diag_error_count();
    g_eval_error_baseline = errors_before;
    g_eval_handled_error_count=errors_before;
    AstProgram* p = parse_program(txt, n, path_utf8);
    vm_own_program(vm,p);
    if (kir_diag_error_count() != errors_before){
        kir_free(txt);
        return NULL;
    }
    KirEnv* root = env_new(vm,NULL);
    vm->root_env = root;
    int rc = vm_eval_program_into(vm, p, root);
    kir_free(txt);
    return rc == 0 ? root : NULL;
}

int vm_eval_program_into(VM* vm, AstProgram* p, KirEnv* env){
    if (!vm || !env || !p || p->parse_errors || vm->ffi_calls ||
        v_value_heap_is_finalizing(vm->value_heap)) return 1;
    unsigned int errors_before = kir_diag_error_count();
    g_eval_error_baseline = errors_before;
    if (vm->gc_envs_n==0) g_eval_handled_error_count=errors_before;
    for (size_t i=0;i<p->type_count;++i){
        AstUserType* type=p->types[i];
        bool replaced=false;
        for (size_t j=0;j<vm->types_n;++j){
            if (vm->types[j].closure==env &&
                vm->types[j].type->name_len==type->name_len &&
                memcmp(vm->types[j].type->name,type->name,type->name_len)==0){
                vm->types[j].type=type;
                replaced=true;
                break;
            }
        }
        if (!replaced){
            if (vm->types_n==vm->types_c){
                size_t cap=vm->types_c?vm->types_c*2:8;
                vm->types=(UserTypeEntry*)kir_xreallocn(vm->types,cap,sizeof(*vm->types));
                vm->types_c=cap;
            }
            vm->types[vm->types_n++]=(UserTypeEntry){type,env};
        }
    }
    for (size_t i = 0; i < p->func_count; ++i){
        AstFunc* f = p->funcs[i];
        vm_add_func(vm, f->name_start, f->name_len, f,
                    f->pos.file ? f->pos.file : "<скрипт>",
                    f->pos.line > 0 ? f->pos.line : 1, env);
    }
    KirBigIntHeap* previous_bigints=v_bigint_heap_set_current(vm->bigint_heap);
    KirValueHeap* previous_values=v_value_heap_set_current(vm->value_heap);
    size_t envs_before=vm->gc_envs_n;
    gc_env_push(vm,env);
    (void)exec_block(vm, env, p->init, p->init_len);
    vm->gc_envs_n=envs_before;
    v_value_heap_set_current(previous_values);
    v_bigint_heap_set_current(previous_bigints);
    return eval_has_error() ? 1 : 0;
}

int vm_has_func(VM* vm, const char* name){
    if (!vm || !name || v_value_heap_is_finalizing(vm->value_heap)) return 0;
    return vm_find_func(vm, name, strlen(name)) != NULL;
}

int vm_call_by_name(VM* vm, const char* name, int argc, const Value* argv, Value* out){
    if (!vm || vm->ffi_calls) return 0;
    if (!vm || !name || argc<0 || (argc && !argv) ||
        v_value_heap_is_finalizing(vm->value_heap)) return 0;
    g_eval_error_baseline = kir_diag_error_count();
    g_eval_handled_error_count=g_eval_error_baseline;
    FuncEntry* fe = vm_find_func_scoped(vm, vm->root_env, name, strlen(name));
    if (!fe) return 0;
    FuncEntry selected = *fe;
    fe = &selected;
    if (argc < 0 || (size_t)argc != fe->f->param_count){
        kir_diag_error("<вызов>", 1, 1,
                       "Функция '%s' ожидает %zu аргументов, получено %d",
                       name, fe->f->param_count, argc);
        return 0;
    }
    /* Reject foreign memory handles, including those nested in collections. */
    for (int i=0;i<argc;++i){
        if (!v_value_heap_accepts_value(vm->value_heap,vm->bigint_heap,argv[i])){
            kir_diag_error("<вызов>",1,1,"Нельзя передать управляемое значение другой VM (в том числе внутри коллекции)");
            return 0;
        }
    }
    size_t roots_before=vm->gc_roots_n;
    for (int i=0;i<argc;++i) gc_root_push(vm,(Value*)&argv[i]);
    KirEnv* callenv = env_new(vm,fe->closure ? fe->closure : vm->root_env);
    size_t envs_before=vm->gc_envs_n;
    gc_env_push(vm,callenv);
    size_t pc = fe->f->param_count;
    for (size_t pi=0; pi<pc; ++pi){
        const char* ps = fe->f->params_start[pi];
        size_t      pn = fe->f->params_len[pi];
        v_dict_set(&callenv->dict, ps, pn, argv[pi]);
    }
    hoist_function_statements(vm,callenv,fe->f->body,fe->f->body_len);
    KirBigIntHeap* previous_bigints=v_bigint_heap_set_current(vm->bigint_heap);
    KirValueHeap* previous_values=v_value_heap_set_current(vm->value_heap);
    cs_push(fe->f->name_start,fe->file ? fe->file : "<скрипт>",fe->line);
    for (size_t bi=0; bi<fe->f->body_len; ++bi){
        Ret r = exec_stmt(vm, callenv, fe->f->body[bi]);
        if (r.flow == FLOW_RETURN){
            if (out) *out = r.v;
            cs_pop();
            v_value_heap_set_current(previous_values);
            v_bigint_heap_set_current(previous_bigints);
            vm->gc_envs_n=envs_before;
            vm->gc_roots_n=roots_before;
            env_release_temporary(vm,callenv);
            return 1;
        }
        if (r.flow != FLOW_NONE){
            kir_diag_error(fe->file, fe->line, 1, "Управление циклом не может выходить из функции");
            break;
        }
        if (eval_has_error()) break;
    }
    cs_pop();
    v_value_heap_set_current(previous_values);
    v_bigint_heap_set_current(previous_bigints);
    if (out) *out = v_nil();
    vm->gc_envs_n=envs_before;
    vm->gc_roots_n=roots_before;
    env_release_temporary(vm,callenv);
    return eval_has_error() ? 0 : 1;
}

/* ======== IMPORT: поиск по from_dir, KIRTON_PATH, точечные имена, кэш по пути ======== */

static int vm_import_inner(VM* vm, const char* name, const char* alias,
                           const char* from_dir, KirEnv* into_env, Value* out_module_dict)
{
    char found[1024];
    if (!kir_resolve_module_path(name, from_dir, found, sizeof(found))){
        /* Файл не нашли — пробуем чисто нативный модуль по префиксу. */
        if (kir_native_has_module(name)){
            const char* eff_alias = (alias && alias[0]) ? alias : name;

            /* В окружение кладём пустой словарь-заглушку, чтобы имя модуля существовало. */
            if (into_env && eff_alias && eff_alias[0]){
                Value dummy = v_dict_new();
                v_dict_set(&into_env->dict, eff_alias, strlen(eff_alias), dummy);
                env_bind_module(into_env, eff_alias, strlen(eff_alias), name, NULL, NULL, NULL);
            }

            if (out_module_dict){
                *out_module_dict = v_dict_new();
            }

            return 1;
        }

        if (from_dir && *from_dir){
            kir_diag_error("<импорт>",1,1,"Не найден модуль '%s' (из \"%s\")", name, from_dir);
        } else {
            kir_diag_error("<импорт>",1,1,"Не найден модуль '%s' (попробуйте из \"...\" или KIRTON_PATH)", name);
        }
        return 0;
    }

    char key[1024];
    kir_normalize_module_key(found, key, sizeof(key));
    char module_name_buf[256];
    kir_module_name_from_path(found, module_name_buf, sizeof(module_name_buf));

    if (loading_has(vm, key)){
        kir_diag_error("<импорт>", 1, 1, "Циклический импорт: %s", key);
        return 0;
    }

    int idx = mod_find(vm, key);
    ModRec* rec = NULL;
    if (idx >= 0 && vm->mods[idx].init_done){
        rec = &vm->mods[idx];
        const char* eff_alias = (alias && alias[0]) ? alias
                                  : (rec->module_name ? rec->module_name : module_name_buf);
        size_t an = eff_alias ? strlen(eff_alias) : 0;
        if (eff_alias && an){
            v_dict_set(&into_env->dict, eff_alias, an, rec->dict);
            env_bind_module(into_env, eff_alias, an, NULL, rec->prog, rec->env, rec->key);
        }
        if (rec->module_name && (!eff_alias || strcmp(eff_alias, rec->module_name)!=0) && rec->prog && rec->env){
            env_bind_module(into_env, rec->module_name, strlen(rec->module_name),
                            NULL, rec->prog, rec->env, rec->key);
        }
        if (out_module_dict) *out_module_dict = rec->dict;
        return 1;
    }

    if (idx < 0){
        KirValueHeap* prev=v_value_heap_set_current(vm->value_heap);
        idx=mod_push_new(vm,key);
        v_value_heap_set_current(prev);
    }
    rec = &vm->mods[idx];
    if (!rec->module_name) rec->module_name = k_strdup(module_name_buf);

    size_t fl=0; char* txt = kir_read_text_file(found, &fl);
    if (!txt){
        kir_diag_error("<импорт>",1,1,"Не удалось открыть модуль: %s", found);
        return 0;
    }
    sc_cache_file(found, txt, (int)fl);

    loading_push(vm, key);
    unsigned int errors_before = kir_diag_error_count();
    KirBigIntHeap* prev_heap=v_bigint_heap_set_current(NULL);
    AstProgram* p = parse_program(txt, fl, found);
    vm_own_program(vm,p);
    v_bigint_heap_set_current(prev_heap);
    if (kir_diag_error_count() != errors_before){
        kir_free(txt);
        loading_pop(vm);
        return 0;
    }

    if (p->module_name && p->module_name_len){
        kir_free(rec->module_name);
        rec->module_name = k_strdup(p->module_name);
    }

    KirEnv* m_env = env_new(vm,NULL);
    rec->env = m_env;
    rec->prog = p;
    env_bind_module(m_env, rec->module_name, strlen(rec->module_name),
                    NULL, p, m_env, rec->key);
    int init_rc = vm_eval_program_into(vm, p, m_env);
    /* Вложенный импорт мог переместить массив кэша через realloc. */
    rec = &vm->mods[idx];
    if (init_rc != 0){
        rec->init_done = 0;
        kir_free(txt);
        loading_pop(vm);
        return 0;
    }

    rec->dict = module_export_dict(vm,p,m_env);
    rec->init_done = 1;

    const char* eff_alias = (alias && alias[0]) ? alias : rec->module_name;
    size_t an = eff_alias ? strlen(eff_alias) : 0;
    if (rec->module_name && (!eff_alias || strcmp(eff_alias, rec->module_name)!=0)){
        env_bind_module(into_env, rec->module_name, strlen(rec->module_name),
                        NULL, p, m_env, rec->key);
    }
    if (eff_alias && an){
        v_dict_set(&into_env->dict, eff_alias, an, rec->dict);
        env_bind_module(into_env, eff_alias, an, NULL, p, m_env, rec->key);
    }

    if (out_module_dict) *out_module_dict = rec->dict;

    kir_free(txt);
    loading_pop(vm);
    return 1;
}

int vm_import(VM* vm, const char* name, const char* alias,
              const char* from_dir, KirEnv* into_env, Value* out_module_dict){
    if (!vm || !name || v_value_heap_is_finalizing(vm->value_heap)) return 0;
    KirBigIntHeap* old_bigints=v_bigint_heap_set_current(vm->bigint_heap);
    KirValueHeap* old_values=v_value_heap_set_current(vm->value_heap);
    int result=vm_import_inner(vm,name,alias,from_dir,into_env,out_module_dict);
    v_value_heap_set_current(old_values);
    v_bigint_heap_set_current(old_bigints);
    return result;
}

int vm_include(VM* vm, const char* path, KirEnv* into_env){
    if (!vm || !path || !into_env || v_value_heap_is_finalizing(vm->value_heap)) return 0;
    size_t fl=0; char* txt = kir_read_text_file(path, &fl);
    if (!txt){
        kir_diag_error("<включить>",1,1,"Не удалось открыть файл: %s", path);
        return 0;
    }
    sc_cache_file(path, txt, (int)fl);
    unsigned int errors_before = kir_diag_error_count();
    g_eval_error_baseline = errors_before;
    g_eval_handled_error_count=errors_before;
    KirBigIntHeap* prev_heap=v_bigint_heap_set_current(NULL);
    AstProgram* p = parse_program(txt, fl, path);
    vm_own_program(vm,p);
    v_bigint_heap_set_current(prev_heap);
    if (kir_diag_error_count() != errors_before){
        kir_free(txt);
        return 0;
    }
    int rc = vm_eval_program_into(vm, p, into_env);
    kir_free(txt);
    return rc == 0 ? 1 : 0;
}
