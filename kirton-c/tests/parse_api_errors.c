/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/eval.h"
#include "kirton/parser.h"

#include <string.h>

int main(void){
    const char source[] = "1 + * 2";
    AstProgram* program = parse_program(source, sizeof(source) - 1, "<api-test>");
    int result=eval_program(program);
    ast_program_destroy(program);
    return result == 1 ? 0 : 1;
}
