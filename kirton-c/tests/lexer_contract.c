/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/lexer.h"

#include <stdio.h>
#include <string.h>

static int expect_id(KirLexer* lexer, const char* expected){
    KirToken token = kir_lexer_next(lexer);
    size_t length = strlen(expected);
    return token.kind == TK_ID &&
        (size_t)(token.end - token.start) == length &&
        memcmp(token.start, expected, length) == 0;
}

static int expect_error(const char* source, size_t length){
    KirLexer lexer;
    kir_lexer_init(&lexer, source, length);
    return kir_lexer_next(&lexer).kind == TK_ERROR;
}

int main(void){
    static const char identifiers[] =
        "foo Foo кир Кир nameA name7_part _private имя́\n// comment at EOF";
    KirLexer lexer;
    kir_lexer_init(&lexer, identifiers, sizeof(identifiers) - 1);

    if (!expect_id(&lexer, "foo") || !expect_id(&lexer, "Foo") ||
        !expect_id(&lexer, "кир") || !expect_id(&lexer, "Кир") ||
        !expect_id(&lexer, "nameA") || !expect_id(&lexer, "name7_part") ||
        !expect_id(&lexer, "_private") || !expect_id(&lexer, "имя́") ||
        kir_lexer_next(&lexer).kind != TK_EOF){
        fprintf(stderr, "LEXER_CONTRACT_FAIL: identifier ranges or EOF comment\n");
        return 1;
    }

    static const char comment_forms[] = "# hash comment\n/* closed block */name";
    kir_lexer_init(&lexer, comment_forms, sizeof(comment_forms) - 1);
    if (!expect_id(&lexer, "name") || kir_lexer_next(&lexer).kind != TK_EOF){
        fprintf(stderr, "LEXER_CONTRACT_FAIL: supported comment forms\n");
        return 1;
    }

    static const char unterminated_comment[] = "/* unterminated";
    if (!expect_error(unterminated_comment, sizeof(unterminated_comment) - 1)){
        fprintf(stderr, "LEXER_CONTRACT_FAIL: unterminated block comment accepted\n");
        return 1;
    }

    static const char unsupported_identifier[] = "name🙂";
    kir_lexer_init(&lexer, unsupported_identifier, sizeof(unsupported_identifier) - 1);
    if (!expect_id(&lexer, "name") || kir_lexer_next(&lexer).kind != TK_ERROR){
        fprintf(stderr, "LEXER_CONTRACT_FAIL: emoji accepted as identifier continuation\n");
        return 1;
    }

    static const char unsupported_start[] = "🙂";
    kir_lexer_init(&lexer, unsupported_start, sizeof(unsupported_start) - 1);
    if (kir_lexer_next(&lexer).kind != TK_ERROR){
        fprintf(stderr, "LEXER_CONTRACT_FAIL: emoji accepted as identifier start\n");
        return 1;
    }

    static const char combining_start[] = {
        (char)0xcc, (char)0x81, 'n', 'a', 'm', 'e'
    };
    if (!expect_error(combining_start, sizeof(combining_start))){
        fprintf(stderr, "LEXER_CONTRACT_FAIL: combining mark accepted as identifier start\n");
        return 1;
    }

    static const char invalid_overlong[] = {'"', (char)0xc0, (char)0xaf, '"'};
    static const char invalid_continuation[] = {'"', (char)0x80, '"'};
    static const char invalid_surrogate[] = {'"', (char)0xed, (char)0xa0, (char)0x80, '"'};
    static const char invalid_out_of_range[] = {'"', (char)0xf4, (char)0x90, (char)0x80, (char)0x80, '"'};
    static const char raw_tab[] = {'"', 'a', '\t', 'b', '"'};
    if (!expect_error(invalid_overlong, sizeof(invalid_overlong)) ||
        !expect_error(invalid_continuation, sizeof(invalid_continuation)) ||
        !expect_error(invalid_surrogate, sizeof(invalid_surrogate)) ||
        !expect_error(invalid_out_of_range, sizeof(invalid_out_of_range)) ||
        !expect_error(raw_tab, sizeof(raw_tab))){
        fprintf(stderr, "LEXER_CONTRACT_FAIL: malformed UTF-8 or raw control accepted in string\n");
        return 1;
    }

    puts("LEXER_CONTRACT_OK");
    return 0;
}
