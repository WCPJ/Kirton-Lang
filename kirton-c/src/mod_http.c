/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
// src/mod_http.c — HTTP для Киртона (WinHTTP; GET и общий запрос)
// API:
//   http_получить(url: str, [таймаут_мс: num=15000]) -> {код, тело, заголовки, заголовки_сырые, ошибка?}
//   http_запрос(метод: str, url: str, [заголовки: список пар], [тело: str],
//               [таймаут_мс: num=15000], [непроверять_TLS: num=0]) -> {...как выше...}
// Алиасы:
//   веб_получить(...), веб_запрос(...)
//
// Примечания:
// - заголовки: список пар вида [ ["Header","Value"], ... ]
// - "заголовки" в ответе — словарь (ключи как в ответе сервера), "заголовки_сырые" — одна строка с CRLF.
// - На Windows включены: авто-редиректы и авто-декомпрессия (gzip/deflate).
// - На POSIX без внешних зависимостей — аккуратные заглушки.

#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include "kirton/native.h"
#include "native_internal.h"
#include "kirton/value.h"
#include "kirton/common.h"
#include <string.h>
#include <stdlib.h>
#include <limits.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <winhttp.h>
  #include <wchar.h>
  #pragma comment(lib, "winhttp.lib")

/* ----------------- helpers ----------------- */

#define HTTP_RESPONSE_LIMIT (64u*1024u*1024u)

/* Returns 1 on success, 0 when the response limit is exceeded, -1 on OOM. */
static int http_reserve_body(char** body,size_t* capacity,size_t used,size_t added){
    if (used>HTTP_RESPONSE_LIMIT || added>HTTP_RESPONSE_LIMIT-used) return 0;
    size_t required=used+added+1;
    if (required<=*capacity) return 1;
    size_t next=*capacity ? *capacity : 4096;
    while (next<required){
        if (next>(HTTP_RESPONSE_LIMIT+1)/2){
            next=HTTP_RESPONSE_LIMIT+1;
            break;
        }
        next*=2;
    }
    if (next<required) return 0;
    char* grown=(char*)kir_oom_realloc(*body,next);
    if (!grown) return -1;
    *body=grown;
    *capacity=next;
    return 1;
}

#if defined(KIRTON_ALLOC_TESTING)
int kir_http_test_reserve_body(char** body,size_t* capacity,size_t used,size_t added){
    return http_reserve_body(body,capacity,used,added);
}
#endif

static wchar_t* u8_to_w(const char* u8, int n){
    if (!u8) return NULL;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, u8, n>=0?n:-1, NULL, 0);
    if (wlen <= 0) return NULL;
    size_t bytes=0;
    if (kir_mul_overflow_size((size_t)wlen+1,sizeof(wchar_t),&bytes)) return NULL;
    wchar_t* w = (wchar_t*)kir_oom_malloc(bytes);
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, u8, n>=0?n:-1, w, wlen);
    w[wlen] = 0;
    return w;
}

static char* w_to_u8(const wchar_t* w, int wn){
    if (!w) return NULL;
    int u8len = WideCharToMultiByte(CP_UTF8, 0, w, wn>=0?wn:-1, NULL, 0, NULL, NULL);
    if (u8len <= 0) return NULL;
    char* out = (char*)kir_oom_malloc((size_t)u8len+1);
    if (!out) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, wn>=0?wn:-1, out, u8len, NULL, NULL);
    out[u8len] = 0;
    return out;
}

/* ключи словаря (длины — байты UTF-8) */
#define K_COD_UTF8_LEN        6   /* "код" */
#define K_TELO_UTF8_LEN       8   /* "тело" */
#define K_OSHIBKA_UTF8_LEN    12  /* "ошибка" */
#define K_HDRS_UTF8_LEN       18  /* "заголовки" */
#define K_HDRS_RAW_UTF8_LEN   29  /* "заголовки_сырые" */

static Value make_resp_base(void){
    Value d = v_dict_new();
    v_dict_set(&d, "код", K_COD_UTF8_LEN, v_num(-1));
    v_dict_set(&d, "тело", K_TELO_UTF8_LEN, v_str_copy("",0));
    return d;
}

static Value make_resp_err(const char* err){
    Value d = make_resp_base();
    v_dict_set(&d, "ошибка", K_OSHIBKA_UTF8_LEN, v_str_copy(err?err:"ошибка",-1));
    return d;
}

static void dict_set_str(Value* d, const char* k, int klen, const char* v){
    v_dict_set(d, k, klen, v_str_copy(v ? v : "", v ? (int)strlen(v) : 0));
}

/* Разбор CRLF-сырых заголовков в словарь {Name: Value} */
static Value parse_headers_to_dict(const wchar_t* rawW){
    Value D = v_dict_new();
    if (!rawW) return D;
    char* raw = w_to_u8(rawW, -1);
    if (!raw) return D;
    const char* s = raw;
    while (*s){
        const char* line = s;
        const char* e = strstr(s, "\r\n");
        if (!e) e = s + strlen(s);
        const char* colon = NULL;
        for (const char* p=line; p<e; ++p){ if (*p==':'){ colon=p; break; } }
        if (colon){
            const char* name = line;
            int nlen = (int)(colon - name);
            const char* val = colon + 1;
            while (val < e && (*val==' ' || *val=='\t')) val++;
            int vlen = (int)(e - val);
            v_dict_set(&D, name, nlen, v_str_copy(val, vlen));
        }
        s = (*e) ? (e + 2) : e;
    }
    free(raw);
    return D;
}

/* Общий исполнитель запроса */
static Value http_request_core(const char* method_u8,
                               const char* url_u8,
                               const Value* headers_opt, /* список пар или nil */
                               const Value* body_opt,    /* строка или nil */
                               int timeout_ms,
                               int insecure_tls)         /* 0/1 */
{
    if (!url_u8) return make_resp_err("ожидался URL");
    if (body_opt && body_opt->kind==VAL_STR && body_opt->as.str.n>MAXDWORD)
        return make_resp_err("request body exceeds WinHTTP size limit");
    wchar_t* wurl = u8_to_w(url_u8, -1);
    if (!wurl) return make_resp_err("out of memory or invalid UTF-8 URL");

    URL_COMPONENTSW uc; ZeroMemory(&uc, sizeof(uc)); uc.dwStructSize = sizeof(uc);
    wchar_t host[1025]; wchar_t path[4096]; wchar_t extra[4096];
    size_t host_capacity = sizeof(host) / sizeof(host[0]);
    uc.lpszHostName = host;  uc.dwHostNameLength  = (DWORD)(host_capacity - 1);
    uc.lpszUrlPath  = path;  uc.dwUrlPathLength   = (DWORD)(sizeof(path)/sizeof(path[0]));
    uc.lpszExtraInfo= extra; uc.dwExtraInfoLength = (DWORD)(sizeof(extra)/sizeof(extra[0]));

    if (!WinHttpCrackUrl(wurl, 0, 0, &uc)){
        free(wurl);
        return make_resp_err("WinHttpCrackUrl");
    }
    if ((size_t)uc.dwHostNameLength >= host_capacity){
        free(wurl);
        return make_resp_err("URL host exceeds supported length");
    }
    host[uc.dwHostNameLength] = L'\0';
    BOOL secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    INTERNET_PORT port = uc.nPort ? uc.nPort : (secure ? 443 : 80);

    HINTERNET hSession = WinHttpOpen(L"Kirton/1.0",
                                     WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession){ free(wurl); return make_resp_err("WinHttpOpen"); }

    if (timeout_ms > 0){
        if (!WinHttpSetTimeouts(hSession, timeout_ms, timeout_ms, timeout_ms, timeout_ms)){
            WinHttpCloseHandle(hSession);
            free(wurl);
            return make_resp_err("WinHttpSetTimeouts");
        }
    }

    /* авторедиректы (если доступно) */
    #ifdef WINHTTP_OPTION_REDIRECT_POLICY
    DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hSession, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof(redir));
    #endif

    HINTERNET hConnect = WinHttpConnect(hSession, host, port, 0);
    if (!hConnect){
        WinHttpCloseHandle(hSession); free(wurl);
        return make_resp_err("WinHttpConnect");
    }

    /* WinHTTP returns path and query as length-delimited spans, not C strings. */
    wchar_t fullPath[4096+4096+4];
    size_t full_capacity = sizeof(fullPath) / sizeof(fullPath[0]);
    size_t full_length = 0;
    if (uc.lpszUrlPath && uc.dwUrlPathLength){
        full_length = (size_t)uc.dwUrlPathLength;
        if (full_length >= full_capacity){
            WinHttpCloseHandle(hConnect);WinHttpCloseHandle(hSession);free(wurl);
            return make_resp_err("URL path exceeds supported length");
        }
        memcpy(fullPath,uc.lpszUrlPath,full_length*sizeof(*fullPath));
    } else {
        fullPath[full_length++] = L'/';
    }
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength){
        size_t extra_length=(size_t)uc.dwExtraInfoLength;
        if(extra_length>full_capacity-full_length-1){
            WinHttpCloseHandle(hConnect);WinHttpCloseHandle(hSession);free(wurl);
            return make_resp_err("URL query exceeds supported length");
        }
        memcpy(fullPath+full_length,uc.lpszExtraInfo,
               extra_length*sizeof(*fullPath));
        full_length+=extra_length;
    }
    fullPath[full_length] = L'\0';

    wchar_t* wmethod = u8_to_w(method_u8 ? method_u8 : "GET", -1);
    if (!wmethod){
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        free(wurl);
        return make_resp_err("out of memory or invalid HTTP method");
    }

    HINTERNET hReq = WinHttpOpenRequest(hConnect, wmethod, fullPath, NULL,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        secure ? WINHTTP_FLAG_SECURE : 0);
    if (wmethod) free(wmethod);
    if (!hReq){
        WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); free(wurl);
        return make_resp_err("WinHttpOpenRequest");
    }
    if (timeout_ms > 0 && !WinHttpSetTimeouts(hReq,timeout_ms,timeout_ms,timeout_ms,timeout_ms)){
        WinHttpCloseHandle(hReq);WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);free(wurl);
        return make_resp_err("WinHttpSetTimeouts(request)");
    }

    /* авто-декомпрессия (если доступно) */
    #ifdef WINHTTP_OPTION_DECOMPRESSION
    DWORD decomp = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
    WinHttpSetOption(hReq, WINHTTP_OPTION_DECOMPRESSION, &decomp, sizeof(decomp));
    #endif

    /* Разрешить игнор TLS-ошибок при insecure_tls
       Набор флагов для WinHTTP — без REVOCATION (в некоторых SDK его нет). */
    if (secure && insecure_tls){
        DWORD flags =
            SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
            SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
            SECURITY_FLAG_IGNORE_UNKNOWN_CA |
            SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        #ifdef SECURITY_FLAG_IGNORE_REVOCATION
        flags |= SECURITY_FLAG_IGNORE_REVOCATION;
        #endif
        WinHttpSetOption(hReq, WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
    }

    /* Пользовательские заголовки: список пар */
    if (headers_opt && headers_opt->kind == VAL_LIST){
        for (int i=0; i < headers_opt->as.list->len; ++i){
            Value pair = headers_opt->as.list->data[i];
            if (pair.kind == VAL_LIST && pair.as.list->len >= 2){
                Value k = pair.as.list->data[0];
                Value v = pair.as.list->data[1];
                if (k.kind==VAL_STR && v.kind==VAL_STR){
                    if(k.as.str.n>INT_MAX || v.as.str.n>INT_MAX) continue;
                    int key_bytes=(int)k.as.str.n;
                    int value_bytes=(int)v.as.str.n;
                    int nameW = MultiByteToWideChar(CP_UTF8,0,k.as.str.s,key_bytes,NULL,0);
                    int valW  = MultiByteToWideChar(CP_UTF8,0,v.as.str.s,value_bytes,NULL,0);
                    if (nameW>0 && valW>0){
                        size_t chars=(size_t)nameW+2u+(size_t)valW+1u;
                        size_t bytes=0;
                        wchar_t* line = kir_mul_overflow_size(chars,sizeof(wchar_t),&bytes)
                            ? NULL : (wchar_t*)kir_oom_malloc(bytes);
                        if (line){
                            MultiByteToWideChar(CP_UTF8,0,k.as.str.s,key_bytes,line,nameW);
                            line[nameW]=L':'; line[nameW+1]=L' ';
                            MultiByteToWideChar(CP_UTF8,0,v.as.str.s,value_bytes,line+nameW+2,valW);
                            line[nameW+2+valW]=0;
                            WinHttpAddRequestHeaders(hReq, line, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
                            free(line);
                        }
                    }
                }
            }
        }
    }

    /* Тело */
    const void* body_ptr = NULL; DWORD body_len = 0;
    if (body_opt && body_opt->kind == VAL_STR){
        body_ptr = (const void*)body_opt->as.str.s;
        body_len = (DWORD)body_opt->as.str.n;
    }

    BOOL ok = WinHttpSendRequest(hReq,
                                 WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 (LPVOID)body_ptr, body_len,
                                 body_len, 0);
    if (ok) ok = WinHttpReceiveResponse(hReq, NULL);
    if (!ok){
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); free(wurl);
        return make_resp_err("WinHttpReceiveResponse");
    }

    /* Код ответа */
    DWORD code = 0, code_len = sizeof(code);
    WinHttpQueryHeaders(hReq,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &code, &code_len, WINHTTP_NO_HEADER_INDEX);

    /* Сырые заголовки */
    DWORD needW = 0;
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, NULL, &needW, WINHTTP_NO_HEADER_INDEX);
    wchar_t* rawW = NULL;
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && needW){
        rawW = (wchar_t*)kir_oom_malloc(needW);
        if (rawW){
            if (!WinHttpQueryHeaders(hReq, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, rawW, &needW, WINHTTP_NO_HEADER_INDEX)){
                free(rawW); rawW=NULL;
            }
        }
    }

    /* Тело */
    char* body = NULL; size_t cap = 0, len = 0;
    const char* body_error=NULL;
    for (;;){
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hReq, &avail)){
            body_error="WinHttpQueryDataAvailable failed";
            break;
        }
        if (avail == 0) break;
        int reserve=http_reserve_body(&body,&cap,len,(size_t)avail);
        if (reserve!=1){
            body_error=reserve==0 ? "HTTP response exceeds the 64 MiB limit" :
                                    "out of memory while reading HTTP response";
            free(body); body=NULL; cap=0; len=0;
            break;
        }
        DWORD rd = 0;
        if (!WinHttpReadData(hReq, body + len, avail, &rd)){
            body_error="WinHttpReadData failed";
            free(body); body=NULL; cap=0; len=0;
            break;
        }
        if (!rd) break;
        len += rd;
    }
    if (body) body[len] = 0;

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    free(wurl);

    /* Формирование ответа */
    Value resp = make_resp_base();
    v_dict_set(&resp, "код", K_COD_UTF8_LEN, v_int((int)code));
    if (body){
        v_dict_set(&resp, "тело", K_TELO_UTF8_LEN, v_str_copy(body, (int)len));
        free(body);
    }
    if (body_error)
        v_dict_set(&resp,"ошибка",K_OSHIBKA_UTF8_LEN,v_str_copy(body_error,-1));
    if (rawW){
        char* rawU8 = w_to_u8(rawW, -1);
        if (rawU8){
            v_dict_set(&resp, "заголовки_сырые", K_HDRS_RAW_UTF8_LEN, v_str_copy(rawU8, -1));
            free(rawU8);
        }
        Value H = parse_headers_to_dict(rawW);
        v_dict_set(&resp, "заголовки", K_HDRS_UTF8_LEN, H);
        free(rawW);
    } else {
        v_dict_set(&resp, "заголовки", K_HDRS_UTF8_LEN, v_dict_new());
    }

    return resp;
}

/* ----------------- совместимая GET-обёртка ----------------- */

static Value http_get(int argc, const Value* argv){
    if (argc < 1 || argv[0].kind != VAL_STR) return make_resp_err("ожидался url (строка)");
    int timeout_ms = 15000;
    if (argc >= 2 && argv[1].kind == VAL_NUM){
        int64_t timeout_value = 0;
        if (v_num_fits_i64(argv[1], &timeout_value) && timeout_value > 0 && timeout_value <= INT_MAX)
            timeout_ms = (int)timeout_value;
    }
    return http_request_core("GET", argv[0].as.str.s, NULL, NULL, timeout_ms, 0);
}

/* ----------------- общий запрос ----------------- */
// http_запрос(метод, url, [заголовки-пары], [тело], [таймаут_мс], [непроверять_TLS])
static Value http_request_fn(int argc, const Value* a){
    if (argc < 2 || a[0].kind!=VAL_STR || a[1].kind!=VAL_STR)
        return make_resp_err("ожидались метод и url (строки)");
    const Value* headers = (argc>=3 && a[2].kind!=VAL_NIL) ? &a[2] : NULL;
    const Value* body    = (argc>=4 && a[3].kind==VAL_STR) ? &a[3] : NULL;
    int timeout_ms = 15000;
    int insecure_tls = 0;
    int64_t numeric_option = 0;
    if (argc>=5 && a[4].kind==VAL_NUM && v_num_fits_i64(a[4], &numeric_option) && numeric_option > 0 && numeric_option <= INT_MAX)
        timeout_ms = (int)numeric_option;
    if (argc>=6 && a[5].kind==VAL_NUM && v_num_fits_i64(a[5], &numeric_option))
        insecure_tls = numeric_option != 0;
    return http_request_core(a[0].as.str.s, a[1].as.str.s, headers, body, timeout_ms, insecure_tls);
}

#else /* !_WIN32 */

static Value http_get(int argc, const Value* argv){
    (void)argc; (void)argv;
    Value d = v_dict_new();
    v_dict_set(&d, "код",    6, v_int(-1));
    v_dict_set(&d, "тело",   8, v_str_copy("", 0));
    v_dict_set(&d, "ошибка", 12, v_str_copy("http не поддержан на этой платформе", -1));
    return d;
}

static Value http_request_fn(int argc, const Value* a){
    (void)argc; (void)a;
    Value d = v_dict_new();
    v_dict_set(&d, "код",    6, v_int(-1));
    v_dict_set(&d, "тело",   8, v_str_copy("", 0));
    v_dict_set(&d, "ошибка", 12, v_str_copy("http не поддержан на этой платформе", -1));
    return d;
}


#endif /* _WIN32 */

/* ----------------- регистрация ----------------- */

static const KirNativeDef HTTP_FUNCS[] = {
    {"http_получить", http_get},
    {"веб_получить",  http_get},
    {"http_запрос",   http_request_fn},
    {"веб_запрос",    http_request_fn}
};

void http_init(void){
    kir_native_register(HTTP_FUNCS, (int)(sizeof(HTTP_FUNCS)/sizeof(HTTP_FUNCS[0])));
    kir_native_mark_bytecode_safe(HTTP_FUNCS, sizeof(HTTP_FUNCS)/sizeof(HTTP_FUNCS[0]));
}
