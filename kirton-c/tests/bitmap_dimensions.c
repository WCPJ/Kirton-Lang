/*
 * SPDX-License-Identifier: MPL-2.0
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
#include "kirton/native.h"
#include "kirton/value.h"
#include "kirton/common.h"
#include <stdint.h>
#include <stdio.h>
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
    uint16_t type;
    uint32_t size;
    uint16_t reserved1,reserved2;
    uint32_t pixel_offset;
} TestBmpFileHeader;
typedef struct {
    uint32_t size;
    int32_t width,height;
    uint16_t planes,bpp;
    uint32_t compression,image_size;
    int32_t xppm,yppm;
    uint32_t colors,important;
} TestBmpInfoHeader;
#pragma pack(pop)

void bitmap_init(void);

static int write_bad_bmp(const char* path,int32_t width,int32_t height){
    FILE* file=fopen(path,"wb");
    if (!file) return 0;
    TestBmpFileHeader fh={0x4D42,sizeof(fh)+sizeof(TestBmpInfoHeader),0,0,
                          sizeof(fh)+sizeof(TestBmpInfoHeader)};
    TestBmpInfoHeader ih={sizeof(ih),width,height,1,32,0,0,0,0,0,0};
    int ok=fwrite(&fh,sizeof(fh),1,file)==1 &&
           fwrite(&ih,sizeof(ih),1,file)==1;
    fclose(file);
    return ok;
}

static int write_good_bmp(const char* path){
    FILE* file=fopen(path,"wb");
    if (!file) return 0;
    TestBmpFileHeader fh={0x4D42,sizeof(fh)+sizeof(TestBmpInfoHeader)+4,0,0,
                          sizeof(fh)+sizeof(TestBmpInfoHeader)};
    TestBmpInfoHeader ih={sizeof(ih),1,1,1,24,0,4,0,0,0,0};
    const uint8_t pixel[4]={3,2,1,0};
    int ok=fwrite(&fh,sizeof(fh),1,file)==1 &&
           fwrite(&ih,sizeof(ih),1,file)==1 &&
           fwrite(pixel,sizeof(pixel),1,file)==1;
    fclose(file);
    return ok;
}

static int rejects_image(const char* path){
    Value argument=v_str_copy(path,-1),result=v_nil();
    int called=kir_native_call("картинка_загрузить",strlen("картинка_загрузить"),
                               1,&argument,&result);
    int rejected=called && result.kind==VAL_NUM && result.integer_small==0;
    remove(path);
    return rejected;
}

int main(void){
    test_heap=v_value_heap_new();
    CHECK(test_heap!=NULL);
    previous_heap=v_value_heap_set_current(test_heap);
    bitmap_init();
    CHECK(write_bad_bmp("bad_bmp_height.bmp",1,INT32_MIN));
    CHECK(rejects_image("bad_bmp_height.bmp"));
    CHECK(write_bad_bmp("bad_bmp_width.bmp",INT32_MAX,1));
    CHECK(rejects_image("bad_bmp_width.bmp"));
    CHECK(write_good_bmp("oom_bmp.bmp"));
    Value path=v_str_copy("oom_bmp.bmp",-1),result=v_nil();
    kir_allocator_test_fail_after(0);
    CHECK(kir_native_call("картинка_загрузить",strlen("картинка_загрузить"),1,&path,&result));
    CHECK(result.kind==VAL_NUM && result.integer_small==0);
    kir_allocator_test_fail_after(1);
    CHECK(kir_native_call("картинка_загрузить",strlen("картинка_загрузить"),1,&path,&result));
    CHECK(result.kind==VAL_NUM && result.integer_small==0);
    kir_allocator_test_fail_after(2);
    CHECK(kir_native_call("картинка_загрузить",strlen("картинка_загрузить"),1,&path,&result));
    CHECK(result.kind==VAL_NUM && result.integer_small>0);
    Value bitmap_id=result;
    CHECK(kir_native_call("картинка_освободить",strlen("картинка_освободить"),1,&bitmap_id,&result));
    CHECK(result.kind==VAL_BOOL && result.as.boolean);
    remove("oom_bmp.bmp");
    cleanup_values();
    return 0;
}
