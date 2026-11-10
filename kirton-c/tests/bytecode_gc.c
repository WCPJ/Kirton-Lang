#include "kirton/bytecode.h"
#include "kirton/diag.h"
#include "kirton/parser.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    FILE *file = fopen(argv[1], "rb");
    if (!file) return 3;
    if (fseek(file, 0, SEEK_END) != 0) return 4;
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) return 5;
    char *source = malloc((size_t)size + 1);
    if (!source) return 6;
    if (fread(source, 1, (size_t)size, file) != (size_t)size) return 7;
    fclose(file);
    source[size] = 0;
    kir_diag_reset_errors();
    AstProgram *program = parse_program(source, (size_t)size, argv[1]);
    if (!program || kir_diag_error_count()) return 8;
    KirtonBytecodeStats stats = {0};
    int rc = kirton_bytecode_run_with_stats(program, "main", &stats);
    ast_program_destroy(program);
    free(source);
    if (rc || stats.collections < 2 || stats.reclaimed_objects < 1000 || stats.peak_objects < stats.live_objects_at_end ||
        stats.live_objects_at_end > 128) {
        fprintf(stderr, "GC stats: rc=%d collections=%zu reclaimed=%zu peak=%zu live_end=%zu\n",
                rc, stats.collections, stats.reclaimed_objects, stats.peak_objects, stats.live_objects_at_end);
        return 9;
    }
    fprintf(stderr,"GC collections=%zu reclaimed=%zu peak=%zu live_end=%zu\n",
            stats.collections,stats.reclaimed_objects,stats.peak_objects,stats.live_objects_at_end);
    return 0;
}
