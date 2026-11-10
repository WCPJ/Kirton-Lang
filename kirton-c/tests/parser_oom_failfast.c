/* Deterministically document the parser allocator failure contract. */
#include "kirton/common.h"
#include "kirton/parser.h"
#include <stdlib.h>
#include <string.h>

int main(int argc,char** argv){
    if (argc!=2) return 2;
    char* end=NULL;
    unsigned long long fail_after=strtoull(argv[1],&end,10);
    if (!end || *end) return 2;
    static const char source[]="функция main() { вернуть 1 }";
    kir_allocator_test_fail_after((size_t)fail_after);
    AstProgram* program=parse_program(source,sizeof(source)-1,"parser_oom.kir");
    if (!program || program->parse_errors) return 3;
    ast_program_destroy(program);
    return 0;
}
