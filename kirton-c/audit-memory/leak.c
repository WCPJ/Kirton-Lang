#include "kirton/bytecode.h"
#include "kirton/eval.h"
#include "kirton/parser.h"
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <string.h>
#if defined(_DEBUG)
#define _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#endif
void native_init(void);
int main(int argc, char **argv) {
  const char *src="import memory\n";
  (void)src;
  native_init();
  const char *mode=argc>1?argv[1]:"";
  const char *path=!strcmp(mode,"bigint")?"audit-memory/bigint.kir":
                   !strcmp(mode,"bigint-control")?"audit-memory/bigint-control.kir":"audit-memory/leak.kir";
  int iterations=argc>2?atoi(argv[2]):12; if(iterations<1)return 2;
  FILE *f=fopen(path,"rb"); if(!f)return 2;
  char buf[1024];size_t n=fread(buf,1,sizeof(buf),f);fclose(f);
  AstProgram *p=parse_program(buf,n,path); if(!p || p->parse_errors)return 3;
  int ast=!strcmp(mode,"ast");
  for(int i=0;i<iterations;i++) {
#if defined(_DEBUG)
    _CrtMemState before={0},after={0},difference={0};
    _CrtMemCheckpoint(&before);
#endif
    int rc=ast ? eval_program(p) : kirton_bytecode_run(p,NULL);
#if defined(_DEBUG)
    _CrtMemCheckpoint(&after);
    (void)_CrtMemDifference(&difference,&before,&after);
    long crt_count=0,crt_bytes=0;
    for(int kind=0;kind<_MAX_BLOCKS;++kind){crt_count+=difference.lCounts[kind];crt_bytes+=difference.lSizes[kind];}
#endif
    PROCESS_MEMORY_COUNTERS_EX m={0};m.cb=sizeof(m);GetProcessMemoryInfo(GetCurrentProcess(),(PROCESS_MEMORY_COUNTERS*)&m,sizeof(m));
#if defined(_DEBUG)
    printf("%d rc=%d private=%zu crt_live_delta=%ld crt_bytes_delta=%ld\n",i,rc,(size_t)m.PrivateUsage,crt_count,crt_bytes);
#else
    printf("%d rc=%d private=%zu\n",i,rc,(size_t)m.PrivateUsage);
#endif
    if(rc)return 4;
  }
  ast_program_destroy(p);
  return 0;
}
