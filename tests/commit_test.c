// Host tool for the write path.
//
//   commit_test <tree> <workdir> dump
//       Builds the writable synthesized volume for <tree> and dumps it as <workdir>/vol.img, to be
//       mounted read-write by a real exFAT driver.
//
//   commit_test <tree> <workdir> commit <modified.img> [dry] [yes] [limit=N] [subset]
//       Builds the same volume again, feeds every sector that differs in <modified.img> through the
//       backend's write path (what the host would have sent), then runs synthCommit onto <tree>.
//       dry      plan only
//       yes      confirm the mass-change guard
//       limit=N  mass-change limit
//       subset   only forward every other changed run of sectors (a copy that was cut short)
//       pump     count pump callbacks and print the total
//       cancel   cancel from the progress callback after the first chunk of new data
//       hide=P   leave <tree>/P out of the volume (the app's own folder)
//       poke=P   append a byte to <tree>/P after the volume was built, as if the Switch changed it
//       nodata=N withhold the data sectors of the file named N from the replay: its folder entry arrives, its data does not
//       Writes <workdir>/post.img (the volume rescanned after the commit) when the card was changed.
//
// Exit codes: 0 committed (or nothing to do), 3 refused before touching anything, 4 applied with errors.
#define _FILE_OFFSET_BITS 64
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../source/exfat_parse.h"
#include "../source/exfat_synth.h"

#define CHUNK 2048  // sectors per I/O (1 MiB)

static Backend* openVolume(const char* tree, const char* workdir) {
    char overlay[1024], err[256] = "";
    snprintf(overlay, sizeof(overlay), "%s/overlay.bin", workdir);
    SynthOptions o = { .root = tree, .overlay_path = overlay, .free_bytes = 4ull << 30 };
    Backend* be = synthBackendCreateEx(&o, err, sizeof(err));
    if (!be) fprintf(stderr, "create failed: %s\n", err);
    return be;
}

static int dumpImage(Backend* be, const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) { perror(path); return 1; }
    unsigned char* buf = malloc(CHUNK * 512);
    unsigned char* zero = calloc(1, CHUNK * 512);
    for (uint64_t lba = 0; lba < be->block_count; lba += CHUNK) {
        uint32_t n = be->block_count - lba < CHUNK ? (uint32_t)(be->block_count - lba) : CHUNK;
        if (!be->read(be->ctx, lba, n, buf)) { fprintf(stderr, "read failed at %llu\n", (unsigned long long)lba); return 1; }
        if (memcmp(buf, zero, (size_t)n * 512) != 0) {
            fseeko(f, (off_t)(lba * 512), SEEK_SET);
            fwrite(buf, 512, n, f);
        }
    }
    fflush(f);
    if (ftruncate(fileno(f), (off_t)(be->block_count * 512)) != 0) perror("ftruncate");
    fclose(f);
    free(buf); free(zero);
    return 0;
}

// Sends every sector of img that differs from the volume through be->write. Consecutive changed sectors go out as one write
// however many read windows they span, the way a host copying a file sends one long run.
typedef struct { unsigned char* buf; size_t cap, len; uint64_t lba; bool active; } Run;

static int flushRun(Backend* be, Run* r, uint64_t* sectors) {
    if (!r->active || !r->len) { r->active = false; r->len = 0; return 0; }
    uint32_t n = (uint32_t)(r->len / 512);
    int rc = be->write(be->ctx, r->lba, n, r->buf) ? 0 : 1;
    if (rc) fprintf(stderr, "write failed\n");
    *sectors += n;
    r->active = false; r->len = 0;
    return rc;
}

// Sectors in [g_skip_lo, g_skip_hi) are treated as unchanged: the data of a file whose entry the host wrote but whose data it did not.
static uint64_t g_skip_lo, g_skip_hi;

static bool diffSector(uint64_t lba, const unsigned char* a, const unsigned char* b) {
    if (lba >= g_skip_lo && lba < g_skip_hi) return false;
    return memcmp(a, b, 512) != 0;
}

static bool imgRead(void* user, uint64_t lba, uint32_t count, void* out) {
    FILE* f = user;
    return fseeko(f, (off_t)(lba * 512), SEEK_SET) == 0 && fread(out, 512, count, f) == count;
}

// Finds the clusters of the file called name in the modified image.
static bool findFileSectors(const char* img, const char* name) {
    FILE* f = fopen(img, "rb");
    if (!f) { perror(img); return false; }
    ExfatTree t;
    memset(&t, 0, sizeof(t));
    bool ok = exfatParse(&t, imgRead, f);
    if (!ok) fprintf(stderr, "parse failed: %s\n", t.err);
    bool found = false;
    for (uint32_t i = 1; ok && i < t.n_nodes; i++) {
        const ExfatNode* n = &t.nodes[i];
        if (n->is_dir || n->ignored || strcmp(n->name, name) != 0 || n->first_cluster < 2) continue;
        uint64_t cluster_sectors = 1ull << t.spc_shift, clusters = (n->data_len + cluster_sectors * 512 - 1) / (cluster_sectors * 512);
        g_skip_lo = t.heap_off + ((uint64_t)(n->first_cluster - 2) << t.spc_shift);
        g_skip_hi = g_skip_lo + clusters * cluster_sectors;
        found = true;
        break;
    }
    exfatFree(&t);
    fclose(f);
    if (!found) fprintf(stderr, "nodata: no file named %s in the image\n", name);
    return found;
}

static int replayImage(Backend* be, const char* path, bool subset, uint64_t* sectors) {
    FILE* f = fopen(path, "rb");
    if (!f) { perror(path); return 1; }
    unsigned char* base = malloc(CHUNK * 512);
    unsigned char* img = malloc(CHUNK * 512);
    bool skip = false;
    Run run = { 0 };
    uint64_t run_end = UINT64_MAX;  // where the run being built (or dropped) would continue
    bool dropping = false;
    for (uint64_t lba = 0; lba < be->block_count; lba += CHUNK) {
        uint32_t n = be->block_count - lba < CHUNK ? (uint32_t)(be->block_count - lba) : CHUNK;
        if (!be->read(be->ctx, lba, n, base)) { fprintf(stderr, "base read failed\n"); return 1; }
        fseeko(f, (off_t)(lba * 512), SEEK_SET);
        if (fread(img, 512, n, f) != n) { fprintf(stderr, "short image read\n"); return 1; }
        for (uint32_t i = 0; i < n;) {
            if (!diffSector(lba + i, base + i * 512, img + i * 512)) { i++; continue; }
            uint32_t j = i;
            while (j < n && diffSector(lba + j, base + j * 512, img + j * 512)) j++;
            if (lba + i != run_end) {  // a new run
                if (flushRun(be, &run, sectors)) return 1;
                skip = subset && !skip;  // alternate: forward, drop, forward, ...
                dropping = subset && skip;
                run.lba = lba + i;
                run.active = !dropping;
            }
            if (!dropping) {
                size_t add = (size_t)(j - i) * 512;
                if (run.len + add > run.cap) {
                    run.cap = (run.len + add) * 2;
                    run.buf = realloc(run.buf, run.cap);
                    if (!run.buf) { fprintf(stderr, "out of memory\n"); return 1; }
                }
                memcpy(run.buf + run.len, img + (size_t)i * 512, add);
                run.len += add;
            }
            run_end = lba + j;
            i = j;
        }
    }
    if (flushRun(be, &run, sectors)) return 1;
    free(run.buf);
    fclose(f);
    free(base); free(img);
    return 0;
}

static unsigned g_pumps;
static void pumpCb(void* user) { (void)user; g_pumps++; }
static bool cancelCb(uint64_t done, uint64_t total, void* user) {
    (void)user;
    printf("  | progress %llu/%llu, cancelling\n", (unsigned long long)done, (unsigned long long)total);
    return false;
}

static void logLine(const char* line, void* user) {
    (void)user;
    printf("  | %s\n", line);
}

static bool confirmYes(const CommitReport* r, void* user) {
    (void)user;
    printf("  | confirming %u deletions/overwrites\n", r->files_deleted + r->dirs_deleted + r->files_rewritten);
    return true;
}

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s <tree> <workdir> dump|commit ...\n", argv[0]); return 2; }
    const char *tree = argv[1], *work = argv[2], *cmd = argv[3];
    char path[1024];

    static char hidden[1024];
    for (int i = 4; i < argc; i++) {
        if (!strncmp(argv[i], "hide=", 5)) {
            snprintf(hidden, sizeof(hidden), "%s/%s", tree, argv[i] + 5);
            synthSetHiddenPath(hidden);
        }
    }

    Backend* be = openVolume(tree, work);
    if (!be) return 1;

    if (!strcmp(cmd, "dump")) {
        snprintf(path, sizeof(path), "%s/vol.img", work);
        int rc = dumpImage(be, path);
        const SynthStats* st = synthBackendStats(be);
        printf("volume: %u dirs, %u files, %llu sectors\n", st->dirs, st->files, (unsigned long long)be->block_count);
        synthBackendDestroy(be);
        return rc;
    }

    if (strcmp(cmd, "commit") != 0 || argc < 5) { fprintf(stderr, "bad command\n"); return 2; }
    CommitOpts opts = { .log = logLine };
    bool subset = false;
    const char* poke = NULL;
    for (int i = 5; i < argc; i++) {
        if (!strncmp(argv[i], "nodata=", 7) && !findFileSectors(argv[4], argv[i] + 7)) return 1;
        if (!strcmp(argv[i], "dry")) opts.dry_run = true;
        else if (!strcmp(argv[i], "yes")) opts.confirm = confirmYes;
        else if (!strcmp(argv[i], "subset")) subset = true;
        else if (!strcmp(argv[i], "pump")) opts.pump = pumpCb;
        else if (!strcmp(argv[i], "cancel")) opts.progress = cancelCb;
        else if (!strncmp(argv[i], "poke=", 5)) poke = argv[i] + 5;
        else if (!strncmp(argv[i], "limit=", 6)) opts.mass_limit = (uint32_t)atoi(argv[i] + 6);
    }

    uint64_t sectors = 0;
    if (replayImage(be, argv[4], subset, &sectors)) return 1;
    printf("replayed %llu changed sectors, overlay holds %llu bytes\n", (unsigned long long)sectors,
           (unsigned long long)synthPendingBytes(be));

    if (poke) {
        snprintf(path, sizeof(path), "%s/%s", tree, poke);
        FILE* pf = fopen(path, "ab");
        if (!pf || fputc('x', pf) == EOF) { perror(path); return 1; }
        fclose(pf);
    }

    CommitReport r;
    bool ok = synthCommit(be, &opts, &r);
    printf("REPORT ok=%d applied=%d changes=%u dirs+=%u files+=%u rewritten=%u moved=%u del_files=%u del_dirs=%u "
           "ignored=%u bytes=%llu errors=%u err=[%s]\n",
           ok, r.applied, r.changes, r.dirs_created, r.files_created, r.files_rewritten, r.moved, r.files_deleted,
           r.dirs_deleted, r.ignored, (unsigned long long)r.bytes, r.errors, r.err);

    if (opts.pump) printf("PUMP calls=%u\n", g_pumps);
    int rc = ok ? 0 : r.applied ? 4 : 3;
    if (r.applied) {
        snprintf(path, sizeof(path), "%s/post.img", work);
        if (dumpImage(be, path)) rc = 1;
        printf("after commit: media_changed=%d pending=%llu\n", be->media_changed, (unsigned long long)synthPendingBytes(be));
    }
    synthBackendDestroy(be);
    return rc;
}
