/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/native.h"
#include "kirton/value.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void bitmap_init(void);

int LLVMFuzzerInitialize(int* argc,char*** argv){
    (void)argc; (void)argv;
    bitmap_init();
    return 0;
}

int LLVMFuzzerTestOneInput(const unsigned char* data,size_t size){
    if (size>1024u*1024u) return 0;
    FILE* file=fopen("kirton_fuzz_input.bmp","wb");
    if (!file) return 0;
    if (size) (void)fwrite(data,1,size,file);
    fclose(file);
    Value path=v_nil(),result=v_nil();
    path.kind=VAL_STR;
    path.as.str.s="kirton_fuzz_input.bmp";
    path.as.str.n=(int)strlen(path.as.str.s);
    if (kir_native_call("картинка_загрузить",strlen("картинка_загрузить"),1,&path,&result) &&
        result.kind==VAL_NUM && result.integer_small>0){
        Value ignored=v_nil();
        (void)kir_native_call("картинка_освободить",
                              strlen("картинка_освободить"),1,&result,&ignored);
    }
    remove("kirton_fuzz_input.bmp");
    return 0;
}
