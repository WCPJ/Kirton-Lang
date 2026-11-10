#include "../src/eval_internal.h"
#include "kirton/common.h"
#include <stdio.h>

int main(int argc,char**argv){
    if(argc!=2)return 2;
    kir_allocator_test_fail_after(0);
    size_t length=0;
    char*source=kir_read_text_file(argv[1],&length);
    if(source){kir_free(source);fprintf(stderr,"module source allocation unexpectedly succeeded\n");return 1;}
    return 0;
}
