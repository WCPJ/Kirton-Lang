/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
// src/mod_fs.c — файловая система для Киртона (кроссплатформенно, UTF-8, wide WinAPI).
// Контракты функций:
//   фс_прочитать_текст(path)      -> STRING | "" при ошибке
//   фс_записать_текст(path,text)  -> BOOL
//   фс_есть(path)                 -> BOOL
//   фс_список(path)               -> LIST<STRING> (без "." и "..")
//   фс_создать_папку(path)        -> BOOL (mkdir -p)
//   удалить_файл(path)            -> BOOL
//   удалить_папка(path)           -> BOOL (пустую папку)
//
// Экспорт и как модуль (fs.*), и как глобальные алиасы (фс_* / удалить_*).

#include "kirton/native.h"
#include "native_internal.h"
#include "kirton/value.h"
#include "kirton/common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <io.h>
  #include <direct.h>
#else
  #include <dirent.h>
  #include <unistd.h>
  #include <sys/stat.h>
  #include <sys/types.h>
#endif

/* ==================== Helpers ==================== */

static char* dupn(const char* s, size_t n){
    char* out = (char*)kir_oom_malloc(n+1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = 0;
    return out;
}

#ifdef _WIN32
static wchar_t* u8_to_w(const char* u8, int n){
    if (!u8) return NULL;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, u8, n>=0?n:-1, NULL, 0);
    if (wlen <= 0) return NULL;
    wchar_t* w = (wchar_t*)kir_oom_malloc(((size_t)wlen+1)*sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, u8, n>=0?n:-1, w, wlen);
    w[wlen] = 0;
    return w;
}
static char* w_to_u8(const wchar_t* w, int wn){
    if (!w) return dupn("",0);
    int u8len = WideCharToMultiByte(CP_UTF8, 0, w, wn>=0?wn:-1, NULL, 0, NULL, NULL);
    if (u8len <= 0) return dupn("",0);
    char* out = (char*)kir_oom_malloc((size_t)u8len+1);
    if (!out) return dupn("",0);
    WideCharToMultiByte(CP_UTF8, 0, w, wn>=0?wn:-1, out, u8len, NULL, NULL);
    out[u8len] = 0;
    return out;
}

/* Разобрать Windows-префикс для mkdir -p:
   - UNC: \\server\share\...
   - Диск: C:\...
   Вернём индекс, с которого начинаются обычные сегменты. */
static size_t win_prefix_end(const char* s, size_t n){
    if (n >= 2 && s[0]=='\\' && s[1]=='\\'){
        size_t i = 2, segs = 0;
        while (i<n && (s[i]=='\\' || s[i]=='/')) i++;
        while (i<n){
            if (s[i]=='\\' || s[i]=='/'){
                segs++;
                while (i<n && (s[i]=='\\'||s[i]=='/')) i++;
                if (segs>=2) break; // после \\server\share
            } else i++;
        }
        while (i<n && (s[i]=='\\'||s[i]=='/')) i++;
        return i;
    }
    if (n >= 2 &&
        ((s[0]>='A'&&s[0]<='Z')||(s[0]>='a'&&s[0]<='z')) &&
        s[1]==':'){
        size_t i = 2;
        if (i<n && (s[i]=='\\'||s[i]=='/')) i++;
        return i;
    }
    if (n>0 && (s[0]=='\\'||s[0]=='/')) return 1; // корень \...
    return 0;
}
#endif

/* ==================== Read/Write ==================== */

static Value fs_read(int argc, const Value* argv){
    if (argc<1 || argv[0].kind!=VAL_STR) return v_str_copy("",0);

#ifdef _WIN32
    wchar_t* w = u8_to_w(argv[0].as.str.s, (int)argv[0].as.str.n);
    if (!w) return v_str_copy("",0);
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (h==INVALID_HANDLE_VALUE) return v_str_copy("",0);
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)){ CloseHandle(h); return v_str_copy("",0); }
    if (sz.QuadPart > 64ll*1024*1024){ CloseHandle(h); return v_str_copy("",0); } // 64 МБ лимит
    DWORD to_read = (DWORD)sz.QuadPart;
    char* buf = (char*)kir_oom_malloc((size_t)to_read + 1);
    if (!buf){ CloseHandle(h); return v_str_copy("",0); }
    DWORD rd=0;
    if (!ReadFile(h, buf, to_read, &rd, NULL)){ free(buf); CloseHandle(h); return v_str_copy("",0); }
    CloseHandle(h);
    buf[rd]=0;
    Value v = v_str_copy(buf, (int)rd);
    free(buf);
    return v;
#else
    const char* p = argv[0].as.str.s;
    FILE* f = fopen(p, "rb");
    if (!f) return v_str_copy("",0);
    if (fseek(f, 0, SEEK_END)!=0){ fclose(f); return v_str_copy("",0); }
    long sz = ftell(f);
    if (sz < 0){ fclose(f); return v_str_copy("",0); }
    if (fseek(f, 0, SEEK_SET)!=0){ fclose(f); return v_str_copy("",0); }
    char* buf = (char*)kir_oom_malloc((size_t)sz + 1);
    if (!buf){ fclose(f); return v_str_copy("",0); }
    size_t rd = fread(buf,1,(size_t)sz,f);
    fclose(f);
    buf[rd]=0;
    Value v = v_str_copy(buf, (int)rd);
    free(buf);
    return v;
#endif
}

static Value fs_write(int argc, const Value* argv){
    if (argc<2 || argv[0].kind!=VAL_STR || argv[1].kind!=VAL_STR) return v_bool(false);

#ifdef _WIN32
    wchar_t* wp = u8_to_w(argv[0].as.str.s, (int)argv[0].as.str.n);
    if (!wp) return v_bool(false);
    HANDLE h = CreateFileW(wp, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    free(wp);
    if (h==INVALID_HANDLE_VALUE) return v_bool(false);
    DWORD wr=0;
    BOOL ok = WriteFile(h, argv[1].as.str.s, (DWORD)argv[1].as.str.n, &wr, NULL);
    CloseHandle(h);
    return v_bool(ok && wr == (DWORD)argv[1].as.str.n);
#else
    FILE* f = fopen(argv[0].as.str.s, "wb");
    if (!f) return v_bool(false);
    size_t wr = fwrite(argv[1].as.str.s, 1, argv[1].as.str.n, f);
    int ec = fclose(f);
    return v_bool(ec==0 && wr==argv[1].as.str.n);
#endif
}

/* ==================== Exists ==================== */

static Value fs_exists(int argc, const Value* argv){
    if (argc<1 || argv[0].kind!=VAL_STR) return v_bool(false);
#ifdef _WIN32
    wchar_t* w = u8_to_w(argv[0].as.str.s, (int)argv[0].as.str.n);
    if (!w) return v_bool(false);
    DWORD attr = GetFileAttributesW(w);
    free(w);
    return v_bool(attr != INVALID_FILE_ATTRIBUTES);
#else
    return v_bool(access(argv[0].as.str.s, F_OK) == 0);
#endif
}

/* ==================== List directory ==================== */

static Value fs_list(int argc, const Value* argv){
    if (argc<1 || argv[0].kind!=VAL_STR) return v_list_new();
    Value L = v_list_new();
#ifdef _WIN32
    // Построим маску <path>\*
    size_t n = argv[0].as.str.n;
    if (n>SIZE_MAX-3) return L;
    char* pat = (char*)kir_oom_malloc(n + 3);
    if (!pat) return L;
    memcpy(pat, argv[0].as.str.s, n);
    // нормализуем на обратный слэш
    for (size_t i=0;i<n;i++) if (pat[i]=='/') pat[i]='\\';
    if (n>0 && (pat[n-1]=='\\')){ pat[n++]='*'; pat[n]=0; }
    else { pat[n++]='\\'; pat[n++]='*'; pat[n]=0; }

    wchar_t* wpat = u8_to_w(pat, (int)n);
    free(pat);
    if (!wpat) return L;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpat, &fd);
    free(wpat);
    if (h==INVALID_HANDLE_VALUE) return L;
    do{
        if (wcscmp(fd.cFileName, L".")==0 || wcscmp(fd.cFileName, L"..")==0) continue;
        char* name = w_to_u8(fd.cFileName, -1);
        if (name){
            v_list_push(&L, v_str_copy(name, -1));
            free(name);
        }
    }while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(argv[0].as.str.s);
    if (!d) return L;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL){
        if (strcmp(ent->d_name, ".")==0 || strcmp(ent->d_name, "..")==0) continue;
        v_list_push(&L, v_str_copy(ent->d_name, -1));
    }
    closedir(d);
#endif
    return L;
}

/* ==================== mkdir -p ==================== */

static Value fs_mkdirs(int argc, const Value* argv){
    if (argc<1 || argv[0].kind!=VAL_STR) return v_bool(false);
    const char* p = argv[0].as.str.s;
    size_t n = argv[0].as.str.n;
    if (!p || n==0) return v_bool(false);

    if (n==SIZE_MAX) return v_bool(false);
    char* tmp = (char*)kir_oom_malloc(n+1);
    if (!tmp) return v_bool(false);
    memcpy(tmp, p, n); tmp[n]=0;

#ifdef _WIN32
    // Приведём слэши к '\'
    for (size_t i=0;i<n;i++) if (tmp[i]=='/') tmp[i]='\\';

    size_t start = win_prefix_end(tmp, n);
    bool ok = true;

    // Создадим головной префикс целиком, если это UNC или диск
    if (start > 0){
        // Для UNC/диска: ничего не создаём до start, идём дальше по сегментам
    }

    for (size_t i=start; i<=n; ++i){
        if (i==n || tmp[i]=='\\'){
            char c = tmp[i]; tmp[i]=0;
            if (tmp[0]!=0){ // непустой префикс
                wchar_t* w = u8_to_w(tmp, -1);
                if (w){
                    if (!CreateDirectoryW(w, NULL)){
                        DWORD err = GetLastError();
                        if (err != ERROR_ALREADY_EXISTS) ok=false;
                    }
                    free(w);
                } else ok=false;
            }
            tmp[i]=c;
            if (!ok) break;
        }
    }
#else
    const char sep = '/';
    bool ok = true;
    // пропустим ведущий '/'
    size_t start = (tmp[0]==sep)? 1 : 0;

    for (size_t i=start; i<=n; ++i){
        if (i==n || tmp[i]==sep){
            char c = tmp[i]; tmp[i]=0;
            if (tmp[0]!=0){
                if (mkdir(tmp, 0777)!=0 && errno!=EEXIST) ok=false;
            }
            tmp[i]=c;
            if (!ok) break;
        }
    }
#endif
    free(tmp);
    return v_bool(ok);
}

/* ==================== delete file / remove dir ==================== */

static Value fs_delete_file(int argc, const Value* argv){
    if (argc<1 || argv[0].kind!=VAL_STR) return v_bool(false);
#ifdef _WIN32
    wchar_t* w = u8_to_w(argv[0].as.str.s, (int)argv[0].as.str.n);
    if (!w) return v_bool(false);
    BOOL ok = DeleteFileW(w);
    free(w);
    return v_bool(ok!=0);
#else
    return v_bool(unlink(argv[0].as.str.s) == 0);
#endif
}

static Value fs_remove_dir(int argc, const Value* argv){
    if (argc<1 || argv[0].kind!=VAL_STR) return v_bool(false);
#ifdef _WIN32
    wchar_t* w = u8_to_w(argv[0].as.str.s, (int)argv[0].as.str.n);
    if (!w) return v_bool(false);
    BOOL ok = RemoveDirectoryW(w);
    free(w);
    return v_bool(ok!=0);
#else
    return v_bool(rmdir(argv[0].as.str.s) == 0);
#endif
}

/* ==================== Register ==================== */

static const KirNativeDef FS_FUNCS[] = {
    {"фс_прочитать_текст", fs_read},
    {"фс_записать_текст",  fs_write},
    {"фс_есть",            fs_exists},
    {"фс_список",          fs_list},
    {"фс_создать_папку",   fs_mkdirs},
    {"удалить_файл",       fs_delete_file},
    {"удалить_папка",      fs_remove_dir},
};

void fs_init(void){
    // 1) модульные имена: fs.фс_*
    kir_native_register_module("fs", FS_FUNCS, (int)(sizeof(FS_FUNCS)/sizeof(FS_FUNCS[0])));
    // 2) глобальные алиасы: фс_* (и удалить_*)
    kir_native_register(FS_FUNCS, (int)(sizeof(FS_FUNCS)/sizeof(FS_FUNCS[0])));
    kir_native_mark_bytecode_module_safe("fs");
    kir_native_mark_bytecode_safe(FS_FUNCS, (int)(sizeof(FS_FUNCS)/sizeof(FS_FUNCS[0])));
}
