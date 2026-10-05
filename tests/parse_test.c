// Host tool: extracts every file of an exFAT image with the commit parser, so the parser can be checked
// against volumes it did not generate (any cluster size, fragmented files, FAT chains).
// Usage: parse_test <image> <outdir>
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../source/exfat_parse.h"

static bool rd(void* user, uint64_t lba, uint32_t count, void* out) {
    FILE* f = user;
    if (fseeko(f, (off_t)(lba * 512), SEEK_SET) != 0) return false;
    return fread(out, 512, count, f) == count;
}

static bool countRun(void* user, uint32_t first, uint32_t count) {
    (void)first; (void)count;
    (*(uint32_t*)user)++;
    return true;
}

static void pathOf(const ExfatTree* t, uint32_t i, const char* out, char* buf, size_t cap) {
    if (i == 0) { snprintf(buf, cap, "%s", out); return; }
    char parent[4096];
    pathOf(t, t->nodes[i].parent, out, parent, sizeof(parent));
    snprintf(buf, cap, "%s/%s", parent, t->nodes[i].name);
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s <image> <outdir>\n", argv[0]); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    ExfatTree t;
    if (!exfatParse(&t, rd, f)) { fprintf(stderr, "parse failed: %s\n", t.err); return 1; }
    mkdir(argv[2], 0777);

    uint8_t* buf = malloc(1 << 20);
    uint32_t files = 0, dirs = 0, fragmented = 0;
    for (uint32_t i = 1; i < t.n_nodes; i++) {
        const ExfatNode* n = &t.nodes[i];
        if (n->ignored) continue;
        char path[4096];
        pathOf(&t, i, argv[2], path, sizeof(path));
        if (n->is_dir) { mkdir(path, 0777); dirs++; continue; }
        FILE* o = fopen(path, "wb");
        if (!o) { perror(path); return 1; }
        ExfatCursor cur = { 0, 0 };
        for (uint64_t off = 0; off < n->data_len;) {
            size_t m = n->data_len - off < (1 << 20) ? (size_t)(n->data_len - off) : (1 << 20);
            if (!exfatReadFile(&t, n, &cur, off, m, buf)) { fprintf(stderr, "read '%s': %s\n", n->name, t.err); return 1; }
            fwrite(buf, 1, m, o);
            off += m;
        }
        fclose(o);
        uint32_t runs = 0;
        if (!exfatForEachRun(&t, n, countRun, &runs)) { fprintf(stderr, "runs '%s': %s\n", n->name, t.err); return 1; }
        if (runs > 1) fragmented++;
        files++;
    }
    printf("parsed: %u dirs, %u files, %u fragmented, cluster %u bytes\n", dirs, files, fragmented, 512u << t.spc_shift);
    exfatFree(&t);
    return 0;
}
