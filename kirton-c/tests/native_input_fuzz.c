/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/native.h"
#include "kirton/value.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static KirValueHeap* test_heap;
static KirValueHeap* previous_heap;

static void cleanup_values(void){
    v_value_heap_set_current(previous_heap);
    v_value_heap_destroy(test_heap);
    test_heap=NULL;
}

#define CHECK(x) do { if (!(x)) { int line=__LINE__; cleanup_values(); return line; } } while (0)

#pragma pack(push,1)
typedef struct {
    uint16_t type; uint32_t size; uint16_t r1,r2; uint32_t offset;
} FuzzBmpFileHeader;
typedef struct {
    uint32_t size; int32_t width,height; uint16_t planes,bpp;
    uint32_t compression,image_size; int32_t xppm,yppm;
    uint32_t colors,important;
} FuzzBmpInfoHeader;
#pragma pack(pop)

void json_init(void);
void bitmap_init(void);

static uint32_t next_random(uint32_t* state){
    uint32_t x=*state;
    x^=x<<13; x^=x>>17; x^=x<<5;
    *state=x;
    return x;
}

int main(void){
    test_heap=v_value_heap_new();
    CHECK(test_heap!=NULL);
    previous_heap=v_value_heap_set_current(test_heap);
    uint32_t random_state=0x4B495254u;
    json_init();
    bitmap_init();
    for (int iteration=0;iteration<128;++iteration){
        unsigned char bytes[256];
        size_t length=next_random(&random_state)%sizeof(bytes);
        for (size_t i=0;i<length;++i) bytes[i]=(unsigned char)next_random(&random_state);
        Value json=v_str_copy((const char*)bytes,(int)length),valid=v_nil();
        CHECK(kir_native_call("json_валиден",strlen("json_валиден"),1,&json,&valid));
        CHECK(valid.kind==VAL_BOOL);

        FILE* file=fopen("native_fuzz.bmp","wb");
        CHECK(file!=NULL);
        static const int32_t dimensions[]={INT32_MIN,-1,0,1,INT32_MAX};
        FuzzBmpFileHeader fh={0x4D42,54,0,0,54};
        FuzzBmpInfoHeader ih={40,
            dimensions[next_random(&random_state)%5],
            dimensions[next_random(&random_state)%5],
            1,(uint16_t)next_random(&random_state),
            next_random(&random_state),0,0,0,0,0};
        fwrite(&fh,sizeof(fh),1,file);
        fwrite(&ih,sizeof(ih),1,file);
        unsigned char tail[128];
        for (size_t i=0;i<sizeof(tail);++i) tail[i]=(unsigned char)next_random(&random_state);
        fwrite(tail,sizeof(tail),1,file);
        fclose(file);

        Value path=v_str_copy("native_fuzz.bmp",-1),loaded=v_nil();
        CHECK(kir_native_call("картинка_загрузить",strlen("картинка_загрузить"),
                              1,&path,&loaded));
        CHECK(loaded.kind==VAL_NUM);
        if (loaded.integer_small>0){
            Value release=v_nil();
            CHECK(kir_native_call("картинка_освободить",
                                  strlen("картинка_освободить"),1,&loaded,&release));
        }
        v_value_heap_mark(test_heap,NULL,json);
        v_value_heap_mark(test_heap,NULL,valid);
        v_value_heap_mark(test_heap,NULL,path);
        v_value_heap_mark(test_heap,NULL,loaded);
        (void)v_value_heap_sweep(test_heap);
        CHECK(v_value_heap_count(test_heap)<=2);
        remove("native_fuzz.bmp");
    }
    cleanup_values();
    return 0;
}
