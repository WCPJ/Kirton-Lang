/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
// src/mod_process.c — запуск внешних процессов, захват вывода
// Экспорт: процесс_выполнить(cmd, [cwd], [таймаут_мс]) -> {код, stdout, ошибка?}
// Windows capture limit: 64 MiB; timeout -> 124, capture OOM/limit -> 125.
// POSIX helper is experimental and currently ignores cwd and timeout_ms.
// Совместимостьные алиасы: процесс_запуск, процесс_запуск_и_ждать
// Доп. алиасы для старых скриптов: process_выполнить, exec

#include "kirton/native.h"
#include "native_internal.h"
#include "kirton/value.h"
#include "kirton/common.h"
#include <string.h>
#include <stdlib.h>
#include <limits.h>

#define PROCESS_CAPTURE_LIMIT (64u*1024u*1024u)

static char* process_strdup(const char* text){
    if (!text) text="";
    size_t length=strlen(text);
    if (length==SIZE_MAX) return NULL;
    char* copy=(char*)kir_oom_malloc(length+1);
    if (copy) memcpy(copy,text,length+1);
    return copy;
}

/* Returns 1 on success, 0 when the capture limit is exceeded, -1 on OOM. */
static int reserve_capture(char** data,size_t* capacity,size_t used,size_t added){
    if (used>PROCESS_CAPTURE_LIMIT || added>PROCESS_CAPTURE_LIMIT-used) return 0;
    size_t required=used+added+1;
    if (required<=*capacity) return 1;
    size_t next=*capacity ? *capacity : 8192;
    while (next<required){
        if (next>(PROCESS_CAPTURE_LIMIT+1)/2){
            next=PROCESS_CAPTURE_LIMIT+1;
            break;
        }
        next*=2;
    }
    if (next<required) return 0;
    char* grown=(char*)kir_oom_realloc(*data,next);
    if (!grown) return -1;
    *data=grown;
    *capacity=next;
    return 1;
}

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <wchar.h>   // WCHAR

/* ---------- UTF-8 <-> UTF-16 helpers ---------- */
static WCHAR* u8_to_w(const char* s){
    if (!s) return NULL;
    int wn = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (wn <= 0) return NULL;
    WCHAR* w = (WCHAR*)kir_oom_malloc(sizeof(WCHAR)*(size_t)wn);
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, wn);
    return w;
}
static char* oem_to_utf8(const char* oem, int len){
    if (!oem) return process_strdup("");
    if (len < 0) len = (int)strlen(oem);
    int wlen = MultiByteToWideChar(CP_OEMCP, 0, oem, len, NULL, 0);
    if (wlen <= 0) return process_strdup("");
    WCHAR* wbuf = (WCHAR*)kir_oom_malloc(sizeof(WCHAR)*((size_t)wlen+1));
    if (!wbuf) return process_strdup("");
    MultiByteToWideChar(CP_OEMCP, 0, oem, len, wbuf, wlen);
    wbuf[wlen] = 0;
    int u8len = WideCharToMultiByte(CP_UTF8, 0, wbuf, wlen, NULL, 0, NULL, NULL);
    if (u8len <= 0){ free(wbuf); return process_strdup(""); }
    char* out = (char*)kir_oom_malloc((size_t)u8len+1);
    if (!out){ free(wbuf); return process_strdup(""); }
    WideCharToMultiByte(CP_UTF8, 0, wbuf, wlen, out, u8len, NULL, NULL);
    out[u8len]=0; free(wbuf); return out;
}

/* ---------- общий раннер (W-API, UTF-16, таймаут) ---------- */
typedef struct {
    int   code;
    char* out_utf8;  /* stdout+stderr (UTF-8) */
    char* err;       /* текст ошибки или NULL */
} RunRes;

static RunRes run_process_utf8(const char* cmd_u8, const char* cwd_u8, int timeout_ms){
    RunRes rr; rr.code = -1; rr.out_utf8 = NULL; rr.err = NULL;

    SECURITY_ATTRIBUTES sa = {0}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE rd=NULL, wr=NULL;
    if (!CreatePipe(&rd,&wr,&sa,0)){ rr.err = process_strdup("CreatePipe"); return rr; }
    if (!SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0)){
        CloseHandle(rd); CloseHandle(wr); rr.err = process_strdup("SetHandleInformation"); return rr;
    }

    STARTUPINFOW si; PROCESS_INFORMATION pi;
    ZeroMemory(&si,sizeof(si)); ZeroMemory(&pi,sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = wr; si.hStdError = wr;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    WCHAR* cmd_w = u8_to_w(cmd_u8);
    WCHAR* cwd_w = cwd_u8 ? u8_to_w(cwd_u8) : NULL;
    if (!cmd_w){ CloseHandle(rd); CloseHandle(wr); if (cwd_w) free(cwd_w); rr.err = process_strdup("out of memory or invalid command"); return rr; }

    BOOL ok = CreateProcessW(NULL, cmd_w, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL, cwd_w, &si, &pi);
    free(cmd_w); if (cwd_w) free(cwd_w);
    CloseHandle(wr); /* критично закрыть у родителя */

    if (!ok){
        CloseHandle(rd);
        rr.err = process_strdup("CreateProcessW failed");
        return rr;
    }

    char* raw=NULL; size_t cap=0,len=0;
    DWORD elapsed = 0;

    for(;;){
        DWORD avail=0;
        if (!PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL)) break;
        if (avail){
            int reserve=reserve_capture(&raw,&cap,len,(size_t)avail);
            if (reserve!=1){
                TerminateProcess(pi.hProcess,125);
                WaitForSingleObject(pi.hProcess,INFINITE);
                CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CloseHandle(rd);
                rr.code=125;
                rr.out_utf8=raw ? oem_to_utf8(raw,(int)len) : process_strdup("");
                if (raw) free(raw);
                rr.err=process_strdup(reserve==0 ? "process output limit exceeded" :
                                      "out of memory while capturing process output");
                return rr;
            }
            DWORD rdn=0; if (!ReadFile(rd, raw+len, avail, &rdn, NULL)) break;
            len += rdn;
        }

        DWORD wait = WaitForSingleObject(pi.hProcess, 10);
        if (wait == WAIT_OBJECT_0){
            /* A process may exit between PeekNamedPipe and the wait while
               leaving captured bytes queued in the pipe. Drain them first. */
            DWORD pending=0;
            if (!PeekNamedPipe(rd,NULL,0,NULL,&pending,NULL) || pending==0) break;
            continue;
        }

        if (wait==WAIT_TIMEOUT && timeout_ms > 0){
            elapsed += 10;
            if (elapsed >= (DWORD)timeout_ms){
                TerminateProcess(pi.hProcess, 124);
                WaitForSingleObject(pi.hProcess,INFINITE);
                CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CloseHandle(rd);
                rr.code = 124;
                rr.out_utf8 = raw ? oem_to_utf8(raw,(int)len) : process_strdup("");
                if (raw) free(raw);
                rr.err = process_strdup("таймаут");
                return rr;
            }
        }
    }

    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code=0; GetExitCodeProcess(pi.hProcess,&code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    rr.code = (int)code;

    if (raw){
        rr.out_utf8 = oem_to_utf8(raw,(int)len);
        free(raw);
    } else {
        rr.out_utf8 = process_strdup("");
    }
    return rr;
}

#else /* ---------- POSIX ---------- */
  #include <stdio.h>
  #include <sys/wait.h>

typedef struct {
    int   code;
    char* out_utf8;
    char* err;
} RunRes;

static RunRes run_process_utf8(const char* cmd_u8, const char* cwd_u8, int timeout_ms){
    (void)cwd_u8; (void)timeout_ms;
    RunRes rr; rr.code=-1; rr.out_utf8=NULL; rr.err=NULL;

    /* Добавим сбор stderr -> stdout */
    size_t L = strlen(cmd_u8);
    if (L>SIZE_MAX-6){ rr.err=process_strdup("command size overflow"); return rr; }
    char* cmd2 = (char*)kir_oom_malloc(L + 6); /* " 2>&1" and NUL */
    if (!cmd2){ rr.err = process_strdup("out of memory"); return rr; }
    memcpy(cmd2, cmd_u8, L);
    memcpy(cmd2+L, " 2>&1", 5);
    cmd2[L+5] = 0;

    FILE* f = popen(cmd2, "r");
    free(cmd2);
    if (!f){ rr.err = process_strdup("popen failed"); return rr; }

    char buf[4096]; size_t len=0, cap=0;
    char* raw=NULL;
    int capture_status=1;

    while(!feof(f)){
        size_t r=fread(buf,1,sizeof(buf),f);
        if(!r) break;
        if (capture_status==1){
            capture_status=reserve_capture(&raw,&cap,len,r);
            if (capture_status!=1){
                free(raw);
                raw=NULL;
                cap=0;
                len=0;
            } else {
                memcpy(raw+len,buf,r);
                len+=r;
            }
        }
    }
    int rc = pclose(f);
    if (capture_status!=1){
        rr.code=-1;
        rr.out_utf8=process_strdup("");
        rr.err=process_strdup(capture_status==0 ?
                              "process output limit exceeded" :
                              "out of memory while capturing process output");
        return rr;
    }
    if (rc<0){
        free(raw);
        rr.err=process_strdup("pclose failed");
        return rr;
    }
    if (WIFEXITED(rc)) rr.code = WEXITSTATUS(rc);
    else if (WIFSIGNALED(rc)) rr.code = 128 + WTERMSIG(rc);
    else rr.code = rc;

    if (raw){ raw[len]=0; rr.out_utf8=raw; }
    else rr.out_utf8=process_strdup("");
    return rr;
}
#endif

/* ---------- конвертация RunRes -> словарь ---------- */
static Value res_to_dict(const RunRes* r){
    Value d = v_dict_new();
    v_dict_set(&d, "код",     6,  v_int(r->code));        // "код"
    v_dict_set(&d, "stdout",  6,  v_str_copy(r->out_utf8 ? r->out_utf8 : "", -1));
    if (r->err) v_dict_set(&d, "ошибка", 12, v_str_copy(r->err, -1)); // "ошибка"
    return d;
}

/* процесс_выполнить(cmd, [cwd], [таймаут_мс]) -> {код, stdout, ошибка?} */
static Value fn_exec_full(int argc, const Value* a){
    if (argc<1 || a[0].kind!=VAL_STR) return res_to_dict(&(RunRes){-1, NULL, "нет команды"});
    const char* cmd = a[0].as.str.s;
    const char* cwd = (argc>=2 && a[1].kind==VAL_STR) ? a[1].as.str.s : NULL;
    int timeout_ms = 0;
    if (argc >= 3 && a[2].kind == VAL_NUM){
        int64_t timeout_value = 0;
        if (v_num_fits_i64(a[2], &timeout_value) && timeout_value > 0 && timeout_value <= INT_MAX)
            timeout_ms = (int)timeout_value;
    }

    RunRes r = run_process_utf8(cmd, cwd, timeout_ms);
    Value d = res_to_dict(&r);
    if (r.out_utf8) free(r.out_utf8);
    if (r.err) free(r.err);
    return d;
}

/* Совместимостьные обёртки */
static Value fn_spawn_capture_old(int argc, const Value* a){
    return fn_exec_full(argc, a);
}
static Value fn_run_and_wait_old(int argc, const Value* a){
    Value d = fn_exec_full(argc, a);
    Value codeV = v_int(-1);
    int ok = (d.kind==VAL_DICT) && v_dict_get(&d, "код", 6, &codeV);
    int64_t code_value = -1;
    int code = (ok && codeV.kind==VAL_NUM && v_num_fits_i64(codeV, &code_value) &&
                code_value >= INT_MIN && code_value <= INT_MAX) ? (int)code_value : -1;
    return v_int(code);
}

/* ---------- регистрация ---------- */
static const KirNativeDef PROC_FUNCS[] = {
    /* основные */
    {"процесс_выполнить",        fn_exec_full},
    {"процесс_запуск",           fn_spawn_capture_old},
    {"процесс_запуск_и_ждать",   fn_run_and_wait_old},

    /* алиасы совместимости под старые скрипты */
    {"process_выполнить",        fn_exec_full}, /* латиница+кириллица, как у тебя в explorer.kir */
    {"exec",                     fn_exec_full}, /* использовалось в открыть_системой(...) */
};

void process_init(void){
    kir_native_register(PROC_FUNCS, (int)(sizeof(PROC_FUNCS)/sizeof(PROC_FUNCS[0])));
    kir_native_mark_bytecode_safe(PROC_FUNCS, sizeof(PROC_FUNCS)/sizeof(PROC_FUNCS[0]));
}
