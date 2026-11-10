/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "kirton/native.h"
#include "kirton/value.h"
#include "kirton/bytecode.h"
#include "kirton/diag.h"
#include "kirton/eval.h"
#include "kirton/parser.h"
#include "../src/native_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "HTTP_LOOPBACK_FAIL:%d: %s\n", __LINE__, #condition); return __LINE__; \
} } while (0)

void http_init(void);
void native_init(void);

typedef struct HttpTestServer {
    SOCKET listener;
    int requests_served;
    int methods_valid;
    int errors_valid;
} HttpTestServer;

static int response_is_expected(Value* response);

static int send_all(SOCKET socket, const char* data, int length){
    int sent = 0;
    while (sent < length){
        int n = send(socket, data + sent, length - sent, 0);
        if (n <= 0) return 0;
        sent += n;
    }
    return 1;
}

static DWORD WINAPI serve_requests(LPVOID argument){
    HttpTestServer* server = (HttpTestServer*)argument;
    static const char response[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "X-Kirton-Test: yes\r\n"
        "Connection: close\r\n\r\n"
        "hello";

    for (int i = 0; i < 6; ++i){
        SOCKET client = accept(server->listener, NULL, NULL);
        if (client == INVALID_SOCKET) return 1;

        char request[8192];
        int used = 0;
        while (used < (int)sizeof(request) - 1){
            int n = recv(client, request + used,
                         (int)sizeof(request) - used - 1, 0);
            if (n <= 0) break;
            used += n;
            request[used] = '\0';
            if (strstr(request, "\r\n\r\n")) break;
        }

        if ((strncmp(request, "GET /", 5) == 0) ||
            (strncmp(request, "POST /", 6) == 0))
            server->methods_valid++;
        const char* reply=response;
        size_t reply_len=sizeof(response)-1;
        static const char not_found[] =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Length: 7\r\n"
            "Connection: close\r\n\r\n"
            "missing";
        if (strstr(request, "missing HTTP/")){
            reply=not_found;
            reply_len=sizeof(not_found)-1;
            server->errors_valid++;
        }
        if (!send_all(client, reply, (int)reply_len)){
            closesocket(client);
            return 2;
        }
        shutdown(client, SD_BOTH);
        closesocket(client);
        server->requests_served++;
    }
    return 0;
}

static int response_is_expected(Value* response){
    Value status = v_nil(), body = v_nil(), headers = v_nil(), header = v_nil();
    Value raw = v_nil(), error = v_nil();
    int64_t status_code = 0;

    if (response->kind != VAL_DICT ||
        !v_dict_get(response, "код", strlen("код"), &status) ||
        !v_num_fits_i64(status, &status_code) || status_code != 200 ||
        !v_dict_get(response, "тело", strlen("тело"), &body) ||
        body.kind != VAL_STR || body.as.str.n != 5 ||
        memcmp(body.as.str.s, "hello", 5) != 0 ||
        !v_dict_get(response, "заголовки", strlen("заголовки"), &headers) ||
        headers.kind != VAL_DICT ||
        !v_dict_get(&headers, "X-Kirton-Test", strlen("X-Kirton-Test"), &header) ||
        header.kind != VAL_STR || header.as.str.n != 3 ||
        memcmp(header.as.str.s, "yes", 3) != 0 ||
        !v_dict_get(response, "заголовки_сырые", strlen("заголовки_сырые"), &raw) ||
        raw.kind != VAL_STR || raw.as.str.n == 0)
        return 0;

    return !v_dict_get(response, "ошибка", strlen("ошибка"), &error);
}

static int response_has_status_and_body(Value* response,int expected_code,const char* expected_body){
    Value status=v_nil(),body=v_nil();
    int64_t code=0;
    size_t body_len=strlen(expected_body);
    return response->kind==VAL_DICT &&
        v_dict_get(response,"код",strlen("код"),&status) &&
        v_num_fits_i64(status,&code) && code==expected_code &&
        v_dict_get(response,"тело",strlen("тело"),&body) &&
        body.kind==VAL_STR && body.as.str.n==body_len &&
        memcmp(body.as.str.s,expected_body,body_len)==0;
}

static int http_assertions;
static int http_error_assertions;
static int http_status_assertions;

static Value assert_http_response(int argc, const Value* argv){
    if (argc != 1 || !response_is_expected((Value*)&argv[0])){
        kir_diag_error("<http_test>", 1, 1, "Unexpected HTTP response in AST/Bytecode test");
        return v_bool(false);
    }
    ++http_assertions;
    return v_bool(true);
}

static Value assert_http_error(int argc, const Value* argv){
    Value status = v_nil(), error = v_nil();
    int64_t code = 0;
    if (argc != 1 || argv[0].kind != VAL_DICT ||
        !v_dict_get((Value*)&argv[0], "код", strlen("код"), &status) ||
        !v_num_fits_i64(status, &code) || code != -1 ||
        !v_dict_get((Value*)&argv[0], "ошибка", strlen("ошибка"), &error) ||
        error.kind != VAL_STR){
        kir_diag_error("<http_test>", 1, 1, "Invalid HTTP arguments did not return the AST error value");
        return v_bool(false);
    }
    ++http_error_assertions;
    return v_bool(true);
}

static Value assert_http_status(int argc,const Value* argv){
    int64_t expected=0;
    if(argc!=3 || !v_num_fits_i64(argv[1],&expected) ||
       !response_has_status_and_body((Value*)&argv[0],(int)expected,
                                      argv[2].kind==VAL_STR?argv[2].as.str.s:"")){
        kir_diag_error("<http_test>",1,1,"Unexpected HTTP status/body in AST/Bytecode test");
        return v_bool(false);
    }
    ++http_status_assertions;
    return v_bool(true);
}

static AstProgram* read_test_program(const char* path){
    FILE* file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0){ fclose(file); return NULL; }
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0){ fclose(file); return NULL; }
    char* source = (char*)malloc((size_t)size + 1);
    if (!source){ fclose(file); return NULL; }
    if (fread(source, 1, (size_t)size, file) != (size_t)size){ free(source); fclose(file); return NULL; }
    fclose(file);
    AstProgram* program = parse_program(source, (size_t)size, path);
    free(source);
    return program;
}

int main(int argc, char** argv){
    CHECK(argc == 2);
    WSADATA winsock;
    CHECK(WSAStartup(MAKEWORD(2, 2), &winsock) == 0);

    HttpTestServer server = {0};
    server.listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(server.listener != INVALID_SOCKET);

    struct sockaddr_in address;
    ZeroMemory(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    CHECK(bind(server.listener, (struct sockaddr*)&address, sizeof(address)) == 0);
    CHECK(listen(server.listener, 4) == 0);

    int address_length = (int)sizeof(address);
    CHECK(getsockname(server.listener, (struct sockaddr*)&address,
                      &address_length) == 0);
    u_short server_port=ntohs(address.sin_port);

    char url[128], closed_url[128];
    int url_length = snprintf(url, sizeof(url), "http://127.0.0.1:%u/", (unsigned)server_port);
    CHECK(url_length > 0 && (size_t)url_length < sizeof(url));

    SOCKET unused=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    CHECK(unused!=INVALID_SOCKET);
    struct sockaddr_in closed_address;
    ZeroMemory(&closed_address,sizeof(closed_address));
    closed_address.sin_family=AF_INET;
    closed_address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    CHECK(bind(unused,(struct sockaddr*)&closed_address,sizeof(closed_address))==0);
    int closed_address_length=(int)sizeof(closed_address);
    CHECK(getsockname(unused,(struct sockaddr*)&closed_address,&closed_address_length)==0);
    int closed_len=snprintf(closed_url,sizeof(closed_url),"http://127.0.0.1:%u/closed",(unsigned)ntohs(closed_address.sin_port));
    CHECK(closed_len>0 && (size_t)closed_len<sizeof(closed_url));
    closesocket(unused);

    HANDLE thread = CreateThread(NULL, 0, serve_requests, &server, 0, NULL);
    CHECK(thread != NULL);

    CHECK(_putenv_s("KIRTON_TEST_HTTP_URL", url) == 0);
    CHECK(_putenv_s("KIRTON_TEST_HTTP_CLOSED_URL",closed_url)==0);
    native_init();
    http_init();
    static const KirNativeDef test_functions[] = {
        {"http_test_assert", assert_http_response},
        {"http_test_assert_error", assert_http_error},
        {"http_test_assert_status",assert_http_status}
    };
    kir_native_register(test_functions, 3);
    kir_native_mark_bytecode_safe(test_functions, 3);

    AstProgram* program = read_test_program(argv[1]);
    CHECK(program != NULL && program->parse_errors == 0);
    eval_set_entry("main");
    kir_diag_reset_errors();
    fputs("HTTP_LOOPBACK_AST_BEGIN\n", stderr);
    CHECK(eval_program(program) == 0);
    CHECK(http_assertions == 2 && http_status_assertions==1 && http_error_assertions == 3);
    kir_diag_reset_errors();
    fputs("HTTP_LOOPBACK_BYTECODE_BEGIN\n", stderr);
    CHECK(kirton_bytecode_run(program, "main") == 0);
    CHECK(http_assertions == 4 && http_status_assertions==2 && http_error_assertions == 6);
    ast_program_destroy(program);

    CHECK(WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0);
    DWORD thread_result = 1;
    CHECK(GetExitCodeThread(thread, &thread_result) && thread_result == 0);
    CHECK(server.requests_served == 6 && server.methods_valid == 6);
    CHECK(server.errors_valid == 2);

    CloseHandle(thread);
    closesocket(server.listener);

    WSACleanup();
    puts("HTTP_AST_BYTECODE_LOOPBACK_OK");
    return 0;
}
