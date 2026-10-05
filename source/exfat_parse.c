#include "exfat_parse.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR_BYTES   512u
#define FAT_ENTRIES    (SECTOR_BYTES / 4)
#define FAT_BAD        0xFFFFFFF7u
#define FAT_EOC        0xFFFFFFF8u
#define MAX_DIR_BYTES  (64u << 20)
#define MAX_NODES      4000000u

#define ENT_FILE       0x85
#define ENT_STREAM     0xC0
#define ENT_NAME       0xC1

static uint32_t get16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static uint32_t get32(const uint8_t* p) { return get16(p) | (get16(p + 2) << 16); }
static uint64_t get64(const uint8_t* p) { return get32(p) | ((uint64_t)get32(p + 4) << 32); }

static bool fail(ExfatTree* t, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static bool fail(ExfatTree* t, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t->err, sizeof(t->err), fmt, ap);
    va_end(ap);
    return false;
}

uint16_t exfatEntrySetChecksum(const uint8_t* d, size_t n) {
    uint16_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        if (i == 2 || i == 3) continue;
        sum = (uint16_t)(((sum & 1) ? 0x8000 : 0) + (sum >> 1) + d[i]);
    }
    return sum;
}

// ---------------------------------------------------------------------------------------------
// Clusters and the FAT

static uint32_t clusterBytes(const ExfatTree* t) { return SECTOR_BYTES << t->spc_shift; }

static uint64_t clusterLba(const ExfatTree* t, uint32_t c) {
    return t->heap_off + ((uint64_t)(c - 2) << t->spc_shift);
}

static bool clusterOk(const ExfatTree* t, uint32_t c) { return c >= 2 && c - 2 < t->cluster_count; }

static bool fatNext(ExfatTree* t, uint32_t c, uint32_t* next) {
    if (!clusterOk(t, c)) return fail(t, "cluster %u outside the heap", c);
    uint32_t sec = c / FAT_ENTRIES;
    if (sec >= t->fat_len) return fail(t, "cluster %u beyond the FAT", c);
    uint64_t lba = (uint64_t)t->fat_off + sec;
    if (t->fat_lba != lba) {
        if (!t->rd(t->user, lba, 1, t->fat_sector)) return fail(t, "read error in the FAT");
        t->fat_lba = lba;
    }
    *next = get32(t->fat_sector + (c % FAT_ENTRIES) * 4);
    return true;
}

// Reads n bytes at an absolute byte offset in the volume.
static bool readBytes(ExfatTree* t, uint64_t off, size_t n, uint8_t* out) {
    uint8_t tmp[SECTOR_BYTES];
    uint64_t lba = off / SECTOR_BYTES;
    size_t head = (size_t)(off % SECTOR_BYTES);
    if (head) {
        if (!t->rd(t->user, lba, 1, tmp)) return fail(t, "read error at sector %llu", (unsigned long long)lba);
        size_t m = SECTOR_BYTES - head < n ? SECTOR_BYTES - head : n;
        memcpy(out, tmp + head, m);
        out += m; n -= m; lba++;
    }
    size_t whole = n / SECTOR_BYTES;
    if (whole) {
        if (!t->rd(t->user, lba, (uint32_t)whole, out)) return fail(t, "read error at sector %llu", (unsigned long long)lba);
        out += whole * SECTOR_BYTES; n -= whole * SECTOR_BYTES; lba += whole;
    }
    if (n) {
        if (!t->rd(t->user, lba, 1, tmp)) return fail(t, "read error at sector %llu", (unsigned long long)lba);
        memcpy(out, tmp, n);
    }
    return true;
}

// Reads a whole directory (all of its clusters) into a malloc'd buffer.
static bool readDir(ExfatTree* t, const ExfatNode* d, uint8_t** out, size_t* out_len) {
    uint32_t cb = clusterBytes(t);
    uint32_t max_clusters = MAX_DIR_BYTES / cb;
    uint8_t* buf = NULL;
    uint32_t n = 0;
    if (d->no_fat_chain) {
        n = (uint32_t)((d->data_len + cb - 1) / cb);
        if (!n || n > max_clusters || !clusterOk(t, d->first_cluster) || !clusterOk(t, d->first_cluster + n - 1))
            return fail(t, "directory '%s' has a bad extent", d->name);
        buf = malloc((size_t)n * cb);
        if (!buf) return fail(t, "out of memory");
        if (!t->rd(t->user, clusterLba(t, d->first_cluster), n << t->spc_shift, buf)) {
            free(buf);
            return fail(t, "read error in directory '%s'", d->name);
        }
    } else {
        uint32_t c = d->first_cluster;
        for (;;) {
            if (!clusterOk(t, c) || n == max_clusters) { free(buf); return fail(t, "directory '%s' has a broken chain", d->name); }
            uint8_t* nb = realloc(buf, (size_t)(n + 1) * cb);
            if (!nb) { free(buf); return fail(t, "out of memory"); }
            buf = nb;
            if (!t->rd(t->user, clusterLba(t, c), 1u << t->spc_shift, buf + (size_t)n * cb)) {
                free(buf);
                return fail(t, "read error in directory '%s'", d->name);
            }
            n++;
            uint32_t nx;
            if (!fatNext(t, c, &nx)) { free(buf); return false; }
            if (nx >= FAT_EOC) break;
            c = nx;
        }
    }
    *out = buf;
    *out_len = (size_t)n * cb;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Names

// UTF-16 to UTF-8 into out (cap bytes). Returns the length, or -1 for a lone surrogate or overflow.
static int utf16to8(const uint16_t* in, int n, char* out, int cap) {
    int o = 0;
    for (int i = 0; i < n; i++) {
        uint32_t cp = in[i];
        if (cp >= 0xD800 && cp < 0xDC00) {
            if (i + 1 >= n || in[i + 1] < 0xDC00 || in[i + 1] >= 0xE000) return -1;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (in[++i] - 0xDC00u);
        } else if (cp >= 0xDC00 && cp < 0xE000) {
            return -1;
        }
        int need = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
        if (o + need >= cap) return -1;
        if (need == 1) out[o++] = (char)cp;
        else if (need == 2) { out[o++] = (char)(0xC0 | (cp >> 6)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
        else if (need == 3) {
            out[o++] = (char)(0xE0 | (cp >> 12)); out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[o++] = (char)(0xF0 | (cp >> 18)); out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[o] = 0;
    return o;
}

static char foldAscii(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int cmpFold(const char* a, const char* b) {
    for (;; a++, b++) {
        char x = foldAscii(*a), y = foldAscii(*b);
        if (x != y) return (unsigned char)x < (unsigned char)y ? -1 : 1;
        if (!x) return 0;
    }
}

// Housekeeping Windows creates at the root. The host owns it; none of it belongs on the card.
bool exfatIsHostHousekeeping(const char* name) {
    return !cmpFold(name, "System Volume Information") || !cmpFold(name, "$RECYCLE.BIN");
}

// ---------------------------------------------------------------------------------------------
// Tree

static bool addNode(ExfatTree* t, ExfatNode** out) {
    if (t->n_nodes == MAX_NODES) return fail(t, "too many directory entries");
    if (t->n_nodes == t->cap_nodes) {
        uint32_t cap = t->cap_nodes ? t->cap_nodes * 2 : 256;
        ExfatNode* n = realloc(t->nodes, (size_t)cap * sizeof(ExfatNode));
        if (!n) return fail(t, "out of memory");
        t->nodes = n;
        t->cap_nodes = cap;
    }
    ExfatNode* nd = &t->nodes[t->n_nodes++];
    memset(nd, 0, sizeof(*nd));
    *out = nd;
    return true;
}

static int cmpSiblings(const void* a, const void* b) {
    const ExfatNode* const* x = a;
    const ExfatNode* const* y = b;
    return cmpFold((*x)->name, (*y)->name);
}

static bool checkDuplicates(ExfatTree* t, uint32_t dir) {
    const ExfatNode* d = &t->nodes[dir];
    if (d->child_count < 2) return true;
    const ExfatNode** v = malloc((size_t)d->child_count * sizeof(*v));
    if (!v) return fail(t, "out of memory");
    for (uint32_t i = 0; i < d->child_count; i++) v[i] = &t->nodes[d->child_start + i];
    qsort(v, d->child_count, sizeof(*v), cmpSiblings);
    for (uint32_t i = 1; i < d->child_count; i++) {
        if (!cmpFold(v[i - 1]->name, v[i]->name)) {
            fail(t, "duplicate name '%s' in '%s'", v[i]->name, d->name);
            free(v);
            return false;
        }
    }
    free(v);
    return true;
}

static const char* dirName(const ExfatTree* t, uint32_t i) { return t->nodes[i].name[0] ? t->nodes[i].name : "/"; }

static bool parseDir(ExfatTree* t, uint32_t dir_idx) {
    uint8_t* buf = NULL;
    size_t len = 0;
    if (!readDir(t, &t->nodes[dir_idx], &buf, &len)) return false;

    uint32_t child_start = t->n_nodes;
    size_t n_ent = len / 32;
    for (size_t i = 0; i < n_ent; i++) {
        const uint8_t* e = buf + i * 32;
        if (e[0] == 0x00) break;                  // end of directory
        if (!(e[0] & 0x80) || e[0] != ENT_FILE) continue;  // deleted, or a primary entry we do not need

        unsigned sec = e[1];
        if (sec < 2 || sec > 18 || i + sec >= n_ent) { free(buf); return fail(t, "truncated entry set in '%s'", dirName(t, dir_idx)); }
        if (exfatEntrySetChecksum(e, (sec + 1) * 32) != get16(e + 2)) {
            free(buf);
            return fail(t, "bad entry checksum in '%s'", dirName(t, dir_idx));
        }
        const uint8_t* st = e + 32;
        if (st[0] != ENT_STREAM) { free(buf); return fail(t, "entry set without stream in '%s'", dirName(t, dir_idx)); }
        unsigned name_len = st[3];
        unsigned name_ents = (name_len + 14) / 15;
        if (!name_len || name_ents > sec - 1) { free(buf); return fail(t, "bad name length in '%s'", dirName(t, dir_idx)); }

        uint16_t name16[255 + 15];
        for (unsigned k = 0; k < name_ents; k++) {
            const uint8_t* ne = e + 64 + k * 32;
            if (ne[0] != ENT_NAME) { free(buf); return fail(t, "bad name entry in '%s'", dirName(t, dir_idx)); }
            for (unsigned j = 0; j < 15; j++) name16[k * 15 + j] = (uint16_t)get16(ne + 2 + j * 2);
        }
        char name8[255 * 4 + 1];
        if (utf16to8(name16, (int)name_len, name8, sizeof(name8)) < 0) {
            free(buf);
            return fail(t, "unusable name in '%s'", dirName(t, dir_idx));
        }

        ExfatNode* nd;
        if (!addNode(t, &nd)) { free(buf); return false; }
        nd->name = strdup(name8);
        nd->parent = dir_idx;
        nd->attrs = (uint16_t)get16(e + 4);
        nd->mtime = get32(e + 12);
        nd->is_dir = (nd->attrs & 0x10) != 0;
        nd->no_fat_chain = (st[1] & 0x02) != 0;
        nd->valid_len = get64(st + 8);
        nd->first_cluster = get32(st + 20);
        nd->data_len = get64(st + 24);
        if (!nd->name) { free(buf); return fail(t, "out of memory"); }

        if (dir_idx == 0 && exfatIsHostHousekeeping(nd->name)) {
            nd->ignored = true;
        } else {
            if (nd->first_cluster ? !clusterOk(t, nd->first_cluster) : (nd->data_len != 0 || nd->is_dir))
                { free(buf); return fail(t, "'%s' has a bad first cluster", nd->name); }
            if (nd->valid_len > nd->data_len) { free(buf); return fail(t, "'%s' has valid length above size", nd->name); }
            if (nd->data_len / clusterBytes(t) > t->cluster_count) { free(buf); return fail(t, "'%s' is larger than the volume", nd->name); }
        }
        i += sec;
    }
    free(buf);
    t->nodes[dir_idx].child_start = child_start;
    t->nodes[dir_idx].child_count = t->n_nodes - child_start;
    return checkDuplicates(t, dir_idx);
}

bool exfatParse(ExfatTree* t, ExfatRead rd, void* user) {
    memset(t, 0, sizeof(*t));
    t->rd = rd;
    t->user = user;
    t->fat_lba = UINT64_MAX;

    uint8_t b[SECTOR_BYTES];
    if (!rd(user, 0, 1, b)) return fail(t, "cannot read the boot sector");
    if (memcmp(b + 3, "EXFAT   ", 8) != 0 || b[510] != 0x55 || b[511] != 0xAA) return fail(t, "boot sector is not exFAT");
    if (b[108] != 9) return fail(t, "unsupported sector size (shift %u)", b[108]);
    if (b[109] > 12) return fail(t, "unsupported cluster size (shift %u)", b[109]);
    t->spc_shift = b[109];
    t->vol_len = get64(b + 72);
    t->serial = get32(b + 100);
    t->fat_off = get32(b + 80);
    t->fat_len = get32(b + 84);
    t->heap_off = get32(b + 88);
    t->cluster_count = get32(b + 92);
    t->root_cluster = get32(b + 96);
    if (!t->cluster_count || t->cluster_count > 0xFFFFFFF5u - 2) return fail(t, "bad cluster count");
    if ((uint64_t)t->fat_len * FAT_ENTRIES < (uint64_t)t->cluster_count + 2) return fail(t, "FAT too small");
    if (t->heap_off + ((uint64_t)t->cluster_count << t->spc_shift) > t->vol_len) return fail(t, "heap runs past the volume");
    if (!clusterOk(t, t->root_cluster)) return fail(t, "bad root directory cluster");

    uint8_t* seen = calloc(t->cluster_count / 8 + 1, 1);  // directory first clusters already visited
    if (!seen) return fail(t, "out of memory");

    ExfatNode* root;
    if (!addNode(t, &root)) { free(seen); return false; }
    root->name = strdup("");
    root->is_dir = true;
    root->first_cluster = t->root_cluster;
    if (!root->name) { free(seen); return fail(t, "out of memory"); }

    bool ok = true;
    for (uint32_t i = 0; i < t->n_nodes && ok; i++) {
        const ExfatNode* d = &t->nodes[i];
        if (!d->is_dir || d->ignored) continue;
        uint32_t bit = d->first_cluster - 2;
        if (seen[bit / 8] & (1u << (bit % 8))) { ok = fail(t, "directory '%s' shares clusters with another", d->name); break; }
        seen[bit / 8] |= (uint8_t)(1u << (bit % 8));
        ok = parseDir(t, i);
    }
    free(seen);
    return ok;
}

void exfatFree(ExfatTree* t) {
    for (uint32_t i = 0; i < t->n_nodes; i++) free(t->nodes[i].name);
    free(t->nodes);
    t->nodes = NULL;
    t->n_nodes = t->cap_nodes = 0;
}

// ---------------------------------------------------------------------------------------------
// File data

bool exfatForEachRun(ExfatTree* t, const ExfatNode* n, ExfatRunFn fn, void* user) {
    if (!n->first_cluster) return true;
    uint32_t cb = clusterBytes(t);
    uint64_t need = (n->data_len + cb - 1) / cb;
    if (!need) return true;
    if (need > t->cluster_count) return fail(t, "'%s' is larger than the volume", n->name);
    if (n->no_fat_chain) {
        if (!clusterOk(t, n->first_cluster) || !clusterOk(t, n->first_cluster + (uint32_t)need - 1))
            return fail(t, "'%s' runs past the heap", n->name);
        fn(user, n->first_cluster, (uint32_t)need);
        return true;
    }
    uint32_t c = n->first_cluster, run_start = c, run_len = 1;
    for (uint64_t k = 1; k < need; k++) {
        uint32_t nx;
        if (!fatNext(t, c, &nx)) return false;
        if (nx == 0 || nx >= FAT_BAD || !clusterOk(t, nx)) return fail(t, "chain of '%s' ends early", n->name);
        if (nx == c + 1) {
            run_len++;
        } else {
            if (!fn(user, run_start, run_len)) return true;
            run_start = nx;
            run_len = 1;
        }
        c = nx;
    }
    fn(user, run_start, run_len);
    return true;
}

// Cluster number at index idx of the file's chain. Advances the cursor on FAT chains.
static bool clusterAt(ExfatTree* t, const ExfatNode* n, ExfatCursor* cur, uint32_t idx, uint32_t* out) {
    if (n->no_fat_chain) {
        *out = n->first_cluster + idx;
        return clusterOk(t, *out) ? true : fail(t, "'%s' runs past the heap", n->name);
    }
    if (!cur->cluster || idx < cur->index) { cur->cluster = n->first_cluster; cur->index = 0; }
    while (cur->index < idx) {
        uint32_t nx;
        if (!fatNext(t, cur->cluster, &nx)) return false;
        if (nx == 0 || nx >= FAT_BAD || !clusterOk(t, nx)) return fail(t, "chain of '%s' ends early", n->name);
        cur->cluster = nx;
        cur->index++;
    }
    *out = cur->cluster;
    return true;
}

bool exfatReadFile(ExfatTree* t, const ExfatNode* n, ExfatCursor* cur, uint64_t off, size_t len, uint8_t* out) {
    if (!len) return true;
    if (off + len > n->data_len) return fail(t, "read past the end of '%s'", n->name);
    uint32_t cb = clusterBytes(t);
    size_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint32_t idx = (uint32_t)(pos / cb), in = (uint32_t)(pos % cb);
        uint32_t first = 0;
        if (!clusterAt(t, n, cur, idx, &first)) return false;

        // Extend the read over following clusters that sit right after this one on the volume.
        uint32_t last = first, run = 1;
        uint64_t covered = cb - in;
        while (covered < len - done) {
            uint32_t nx;
            if (n->no_fat_chain) {
                nx = last + 1;
            } else if (!fatNext(t, last, &nx)) {
                return false;
            }
            if (nx != last + 1 || !clusterOk(t, nx)) break;
            last = nx;
            run++;
            covered += cb;
            if (!n->no_fat_chain) { cur->cluster = last; cur->index = idx + run - 1; }
        }
        size_t take = covered < len - done ? (size_t)covered : len - done;
        if (!readBytes(t, clusterLba(t, first) * SECTOR_BYTES + in, take, out + done)) return false;
        done += take;
    }
    if (off + len > n->valid_len) {
        size_t from = n->valid_len > off ? (size_t)(n->valid_len - off) : 0;
        memset(out + from, 0, len - from);
    }
    return true;
}
