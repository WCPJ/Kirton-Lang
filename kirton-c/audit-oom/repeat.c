#include "kirton/parser.h"
#include "kirton/bytecode.h"
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    const char* source = "пусть x = \"abcdefghijklmnopqrstuvwxyz\"\n";
    AstProgram* p = parse_program(source, strlen(source), "repeat.kir");
    if (!p || p->parse_errors) return 2;
    for (int i = 0; i < 100000; ++i) {
        if (kirton_bytecode_run(p, NULL)) return 3;
        if (i % 25000 == 0) {
            PROCESS_MEMORY_COUNTERS_EX m = {0};
            m.cb = sizeof(m);
            if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&m, sizeof(m))) return 4;
            printf("%d %zu\n", i, (size_t)m.PrivateUsage);
        }
    }
    ast_program_destroy(p);
    return 0;
}
