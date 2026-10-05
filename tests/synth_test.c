// Host tool: build the synthesized exFAT volume for a directory and dump it as a (sparse) image.
// Usage: synth_test <dir> <out.img>
// Validate the result with fsck.exfat and by mounting it (see tests/run_synth_test.sh).
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "../source/exfat_synth.h"

static void progress(const SynthStats* st, void* user) {
    (void)user;
    fprintf(stderr, "  scanned: %u dirs, %u files\n", st->dirs, st->files);
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <dir> <out.img>\n", argv[0]); return 2; }
    char err[256] = "";
    Backend* be = synthBackendCreate(argv[1], progress, NULL, err, sizeof(err));
    if (!be) { fprintf(stderr, "create failed: %s\n", err); return 1; }
    const SynthStats* st = synthBackendStats(be);
    printf("dirs=%u files=%u skipped=%u bytes=%llu sectors=%llu (%.2f GiB volume)\n", st->dirs, st->files,
           st->skipped, (unsigned long long)st->data_bytes, (unsigned long long)be->block_count,
           be->block_count * 512.0 / (1 << 30));

    FILE* f = fopen(argv[2], "wb");
    if (!f) { perror("fopen"); return 1; }
    enum { CHUNK = 2048 };  // sectors per read (1 MiB)
    unsigned char* buf = malloc(CHUNK * 512);
    unsigned char* zero = calloc(1, CHUNK * 512);
    for (uint64_t lba = 0; lba < be->block_count; lba += CHUNK) {
        uint32_t n = be->block_count - lba < CHUNK ? (uint32_t)(be->block_count - lba) : CHUNK;
        if (!be->read(be->ctx, lba, n, buf)) { fprintf(stderr, "read failed at lba %llu\n", (unsigned long long)lba); return 1; }
        if (memcmp(buf, zero, (size_t)n * 512) != 0) {
            fseeko(f, (off_t)(lba * 512), SEEK_SET);
            fwrite(buf, 512, n, f);
        }
    }
    fflush(f);
    if (ftruncate(fileno(f), (off_t)(be->block_count * 512)) != 0) perror("ftruncate");
    fclose(f);
    synthBackendDestroy(be);
    return 0;
}
