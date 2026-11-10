#include "kirton/bytecode.h"
#include "kirton/diag.h"
#include "kirton/eval.h"
#include "kirton/parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

void native_init(void);
void list_init(void);
void dict_init(void);
void math_init(void);
void math3d_init(void);
void json_init(void);
void regex_init(void);

static int compare_double(const void *a, const void *b) {
    double x=*(const double*)a,y=*(const double*)b;
    return (x>y)-(x<y);
}
static double median(double *values,size_t n) {
    qsort(values,n,sizeof(*values),compare_double);
    return values[n/2];
}
static double now_ms(void) {
#ifdef _WIN32
    LARGE_INTEGER counter,frequency;QueryPerformanceCounter(&counter);QueryPerformanceFrequency(&frequency);
    return (double)counter.QuadPart*1000.0/(double)frequency.QuadPart;
#else
    struct timespec ts;timespec_get(&ts,TIME_UTC);return (double)ts.tv_sec*1000.0+(double)ts.tv_nsec/1000000.0;
#endif
}
int main(int argc,char **argv) {
    if(argc!=5)return 2;
    native_init();list_init();dict_init();math_init();math3d_init();json_init();regex_init();
    int bytecode=strcmp(argv[1],"bytecode")==0;
    if(!bytecode&&strcmp(argv[1],"ast")!=0)return 2;
    char *end=NULL;long runs=strtol(argv[3],&end,10);
    if(!end||*end||runs<1||runs>101)return 2;
    FILE *in=fopen(argv[2],"rb");if(!in)return 3;
    if(fseek(in,0,SEEK_END)!=0)return 4;
    long length=ftell(in);if(length<0||fseek(in,0,SEEK_SET)!=0)return 4;
    char *source=malloc((size_t)length+1);if(!source)return 5;
    if(fread(source,1,(size_t)length,in)!=(size_t)length)return 6;
    fclose(in);source[length]=0;
    kir_diag_reset_errors();
    AstProgram *program=parse_program(source,(size_t)length,argv[2]);
    if(!program||kir_diag_error_count())return 7;
    eval_set_entry("main");
    double *times=calloc((size_t)runs,sizeof(double));
    double *compile=calloc((size_t)runs,sizeof(double));
    if(!times||!compile)return 8;
    for(long i=0;i<runs;i++){
        kir_diag_reset_errors();
        if(bytecode){
            KirtonBytecodeStats stats={0};
            if(kirton_bytecode_run_with_stats(program,"main",&stats))return 9;
            times[i]=stats.execute_ms;compile[i]=stats.compile_ms;
        }else{
            double begin=now_ms();int rc=eval_program(program);double finish=now_ms();
            if(rc)return 10;
            times[i]=finish-begin;
        }
    }
    FILE *out=fopen(argv[4],"wb");if(!out)return 11;
    fprintf(out,"mode=%s runs=%ld parse_ms=excluded compile_median_ms=%.3f execute_median_ms=%.3f\n",
        bytecode?"bytecode":"ast",runs,bytecode?median(compile,(size_t)runs):0.0,median(times,(size_t)runs));
    fclose(out);
    free(times);free(compile);ast_program_destroy(program);free(source);
    return 0;
}
