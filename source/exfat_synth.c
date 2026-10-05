#include "synth_priv.h"

#include "exfat_parse.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(__has_include)
#  if __has_include(<sys/statvfs.h>)
#    include <sys/statvfs.h>
#  endif
#endif
#include <time.h>
#include <unistd.h>

// Standard up-case table. Windows mounts a volume read-only if it carries anything else.
#include "upcase_table.h"
#define UPCASE_BYTES ((uint32_t)sizeof(UPCASE_TABLE))

static const char VOLUME_LABEL[] = "SWITCH SD";

// ---------------------------------------------------------------------------------------------
// Checksums

static uint32_t checksum32(const uint8_t* d, size_t n, size_t skip_a, size_t skip_b, size_t skip_c, uint32_t sum) {
    for (size_t i = 0; i < n; i++) {
        if (i == skip_a || i == skip_b || i == skip_c) continue;
        sum = ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) + d[i];
    }
    return sum;
}

// The table expanded to one entry per UTF-16 unit, for hashing names.
static uint16_t g_upcase[65536];
static bool     g_upcase_ready;

static void upcaseInit(void) {
    uint32_t at = 0;
    for (uint32_t i = 0; i < sizeof(UPCASE_TABLE) / 2 && at < 65536; i++) {
        if (UPCASE_TABLE[i] == 0xFFFF && i + 1 < sizeof(UPCASE_TABLE) / 2) {  // run of identity mappings
            for (uint32_t n = UPCASE_TABLE[++i]; n && at < 65536; n--, at++) g_upcase[at] = (uint16_t)at;
        } else {
            g_upcase[at++] = UPCASE_TABLE[i];
        }
    }
    for (; at < 65536; at++) g_upcase[at] = (uint16_t)at;
    g_upcase_ready = true;
}

static uint16_t upcaseChar(uint16_t c) {
    if (!g_upcase_ready) upcaseInit();
    return g_upcase[c];
}

static uint16_t nameHash(const uint16_t* name, size_t n) {
    uint16_t h = 0;
    for (size_t i = 0; i < n; i++) {
        uint16_t c = upcaseChar(name[i]);
        h = (uint16_t)(((h & 1) ? 0x8000 : 0) + (h >> 1) + (c & 0xFF));
        h = (uint16_t)(((h & 1) ? 0x8000 : 0) + (h >> 1) + (c >> 8));
    }
    return h;
}

// ---------------------------------------------------------------------------------------------
// UTF-8 -> UTF-16. Returns the number of code units, or -1 if invalid. out may be NULL.

int synthUtf8to16(const char* s, uint16_t* out, int max) {
    int n = 0;
    const uint8_t* p = (const uint8_t*)s;
    while (*p) {
        uint32_t cp;
        int extra;
        if (*p < 0x80)                { cp = *p; extra = 0; }
        else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; extra = 1; }
        else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; extra = 2; }
        else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; extra = 3; }
        else return -1;
        p++;
        for (int i = 0; i < extra; i++, p++) {
            if ((*p & 0xC0) != 0x80) return -1;
            cp = (cp << 6) | (*p & 0x3F);
        }
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) return -1;
        int units = cp >= 0x10000 ? 2 : 1;
        if (n + units > max) return -1;
        if (out) {
            if (units == 2) {
                cp -= 0x10000;
                out[n] = (uint16_t)(0xD800 + (cp >> 10));
                out[n + 1] = (uint16_t)(0xDC00 + (cp & 0x3FF));
            } else {
                out[n] = (uint16_t)cp;
            }
        }
        n += units;
    }
    return n;
}

// exFAT timestamp from time_t. UTC offset byte is set to "valid, +0" by the caller.
uint32_t synthDosTime(time_t t) {
    struct tm tm;
    if (t <= 0 || !gmtime_r(&t, &tm) || tm.tm_year < 80) return (1u << 21) | (1u << 16);  // 1980-01-01
    if (tm.tm_year > 207) tm.tm_year = 207;
    return ((uint32_t)(tm.tm_year - 80) << 25) | ((uint32_t)(tm.tm_mon + 1) << 21) |
           ((uint32_t)tm.tm_mday << 16) | ((uint32_t)tm.tm_hour << 11) |
           ((uint32_t)tm.tm_min << 5) | (uint32_t)(tm.tm_sec / 2);
}

// ---------------------------------------------------------------------------------------------
// Scan

static bool addNode(Synth* s, const char* name, uint32_t parent, bool is_dir, uint64_t size,
                    uint32_t mtime, int name16, uint32_t* out_id) {
    if (s->n_nodes == s->cap_nodes) {
        uint32_t cap = s->cap_nodes ? s->cap_nodes * 2 : 1024;
        Node* n = realloc(s->nodes, (size_t)cap * sizeof(Node));
        if (!n) return false;
        s->nodes = n;
        s->cap_nodes = cap;
    }
    size_t len = strlen(name) + 1;
    if (s->pool_len + len > s->pool_cap) {
        size_t cap = s->pool_cap ? s->pool_cap * 2 : 65536;
        while (cap < s->pool_len + len) cap *= 2;
        char* p = realloc(s->pool, cap);
        if (!p) return false;
        s->pool = p;
        s->pool_cap = cap;
    }
    memcpy(s->pool + s->pool_len, name, len);
    Node* nd = &s->nodes[s->n_nodes];
    memset(nd, 0, sizeof(*nd));
    nd->name_off = (uint32_t)s->pool_len;
    nd->parent = parent;
    nd->size = size;
    nd->mtime = mtime;
    nd->name_len16 = (uint16_t)name16;
    nd->is_dir = is_dir;
    s->pool_len += len;
    *out_id = s->n_nodes++;
    return true;
}

// Builds "<root>/<a>/<b>" for a node into buf. Returns false if it does not fit.
bool synthNodePath(const Synth* s, uint32_t id, char* buf, size_t cap) {
    uint32_t chain[256];
    int depth = 0;
    for (uint32_t i = id; i != 0; i = s->nodes[i].parent) {
        if (depth == 256) return false;
        chain[depth++] = i;
    }
    size_t n = strlen(s->root);
    if (n + 1 >= cap) return false;
    memcpy(buf, s->root, n);
    while (depth--) {
        const char* name = s->pool + s->nodes[chain[depth]].name_off;
        size_t l = strlen(name);
        if (n + 1 + l + 1 > cap) return false;
        buf[n++] = '/';
        memcpy(buf + n, name, l);
        n += l;
    }
    buf[n] = 0;
    if (id == 0) {  // the root itself needs a trailing slash for "sdmc:"
        if (n + 2 > cap) return false;
        buf[n++] = '/';
        buf[n] = 0;
    }
    return true;
}

static SynthSkipLog g_skip_log;

void synthSetSkipLog(SynthSkipLog fn) { g_skip_log = fn; }

static const char* g_hidden_path;
void synthSetHiddenPath(const char* path) { g_hidden_path = path; }

static void skipLog(const char* path, const char* why) {
    if (g_skip_log) g_skip_log(path, why);
}

static void skipLog2(const char* dir, const char* name, const char* why) {
    char p[MAX_PATH_LEN];
    snprintf(p, sizeof(p), "%.*s/%.*s", (int)(sizeof(p) / 2 - 1), dir, (int)(sizeof(p) / 2 - 1), name);
    skipLog(p, why);
}

static bool scan(Synth* s, SynthProgress cb, void* user, char* err, size_t err_len) {
    uint32_t root;
    if (!addNode(s, "", 0, true, 0, 0, 0, &root)) { snprintf(err, err_len, "out of memory"); return false; }
    s->stats.dirs = 1;

    char dpath[MAX_PATH_LEN], fpath[MAX_PATH_LEN];
    uint32_t since_cb = 0;
    for (uint32_t d = 0; d < s->n_nodes; d++) {
        if (!s->nodes[d].is_dir) continue;
        if (!synthNodePath(s, d, dpath, sizeof(dpath))) { s->stats.skipped++; skipLog("(directory)", "path too long"); continue; }
        DIR* dir = opendir(dpath);
        if (!dir) {
            if (d == 0) { snprintf(err, err_len, "cannot open %s", dpath); return false; }
            s->stats.skipped++; skipLog(dpath, "cannot open dir");
            continue;
        }
        size_t dl = strlen(dpath);
        if (dl && dpath[dl - 1] == '/') dpath[--dl] = 0;

        uint32_t first = s->n_nodes;
        struct dirent* de;
        while ((de = readdir(dir))) {
            const char* name = de->d_name;
            if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
            int n16 = synthUtf8to16(name, NULL, MAX_NAME16);
            if (n16 <= 0 || dl + 1 + strlen(name) + 1 > sizeof(fpath)) { s->stats.skipped++; skipLog2(dpath, name, "bad or too-long name"); continue; }
            snprintf(fpath, sizeof(fpath), "%s/%s", dpath, name);
            if (g_hidden_path && !strcmp(fpath, g_hidden_path)) continue;
            struct stat st;
            if (stat(fpath, &st) != 0) {
                // Some entries (in-use files, special save/placeholder files) refuse stat on Horizon.
                // The dirent type is still reliable, so recover the size by opening the file.
                int stat_errno = errno;
                memset(&st, 0, sizeof(st));
                bool recovered = false;
                if (de->d_type == DT_DIR) {
                    st.st_mode = S_IFDIR;
                    recovered = true;
                } else if (de->d_type == DT_REG) {
                    FILE* f = fopen(fpath, "rb");
                    if (f) {
                        if (fseeko(f, 0, SEEK_END) == 0) {
                            off_t end = ftello(f);
                            if (end >= 0) { st.st_mode = S_IFREG; st.st_size = end; recovered = true; }
                        }
                        fclose(f);
                    }
                }
                if (!recovered) {
                    char why[48];
                    snprintf(why, sizeof(why), "stat failed (errno %d, type %d)", stat_errno, de->d_type);
                    s->stats.skipped++;
                    skipLog2(dpath, name, why);
                    continue;
                }
            }
            bool is_dir = S_ISDIR(st.st_mode);
            if (!is_dir && !S_ISREG(st.st_mode)) { s->stats.skipped++; skipLog2(dpath, name, "not a regular file or dir"); continue; }
            uint32_t id;
            uint64_t size = is_dir ? 0 : (uint64_t)st.st_size;
            if (!addNode(s, name, d, is_dir, size, synthDosTime(st.st_mtime), n16, &id)) {
                closedir(dir);
                snprintf(err, err_len, "out of memory after %u entries", s->n_nodes);
                return false;
            }
            if (is_dir) s->stats.dirs++;
            else { s->stats.files++; s->stats.data_bytes += size; }
            if (cb && ++since_cb >= 256) { since_cb = 0; cb(&s->stats, user); }
        }
        closedir(dir);
        s->nodes[d].child_start = first;
        s->nodes[d].child_count = s->n_nodes - first;
    }
    if (cb) cb(&s->stats, user);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Layout

static uint32_t entrySetBytes(const Node* n) { return (2u + (n->name_len16 + 14u) / 15u) * 32u; }

static uint32_t ceilDiv64(uint64_t a, uint32_t b) { return (uint32_t)((a + b - 1) / b); }

// Writes the directory entry set for node n into out. Returns bytes written.
static uint32_t writeEntrySet(const Synth* s, const Node* n, uint8_t* out) {
    uint16_t name16[MAX_NAME16 + 1];
    int len = synthUtf8to16(s->pool + n->name_off, name16, MAX_NAME16);
    uint32_t names = ((uint32_t)len + 14) / 15;
    uint32_t total = (2 + names) * 32;
    memset(out, 0, total);

    out[0] = ENT_FILE;
    out[1] = (uint8_t)(1 + names);
    put16(out + 4, n->is_dir ? 0x10 : 0x20);
    put32(out + 8, n->mtime);   // created
    put32(out + 12, n->mtime);  // modified
    put32(out + 16, n->mtime);  // accessed
    out[22] = out[23] = out[24] = 0x80;  // UTC offset valid, +0

    uint8_t* st = out + 32;
    st[0] = ENT_STREAM;
    st[1] = n->first_cluster ? 0x03 : 0x01;  // AllocationPossible | NoFatChain
    st[3] = (uint8_t)len;
    put16(st + 4, nameHash(name16, (size_t)len));
    uint64_t data_len = n->is_dir ? (uint64_t)n->n_clusters * CLUSTER_BYTES : n->size;
    put64(st + 8, data_len);   // ValidDataLength
    put32(st + 20, n->first_cluster);
    put64(st + 24, data_len);  // DataLength

    for (uint32_t i = 0; i < names; i++) {
        uint8_t* e = out + 64 + i * 32;
        e[0] = ENT_NAME;
        for (uint32_t j = 0; j < 15; j++) {
            uint32_t k = i * 15 + j;
            if (k < (uint32_t)len) put16(e + 2 + j * 2, name16[k]);
        }
    }
    put16(out + 2, exfatEntrySetChecksum(out, total));
    return total;
}

static void buildDirImage(const Synth* s, uint32_t d, uint8_t* img) {
    const Node* dn = &s->nodes[d];
    uint32_t off = 0;
    if (d == 0) {
        uint8_t* e = img;
        e[0] = ENT_LABEL;
        e[1] = (uint8_t)(sizeof(VOLUME_LABEL) - 1);
        for (size_t i = 0; i < sizeof(VOLUME_LABEL) - 1; i++) put16(e + 2 + i * 2, (uint8_t)VOLUME_LABEL[i]);
        e = img + 32;
        e[0] = ENT_BITMAP;
        put32(e + 20, s->bitmap_first);
        put64(e + 24, s->bitmap_bytes);
        e = img + 64;
        e[0] = ENT_UPCASE;
        put32(e + 4, s->upcase_checksum);
        put32(e + 20, s->upcase_first);
        put64(e + 24, UPCASE_BYTES);
        off = 96;
    }
    for (uint32_t c = 0; c < dn->child_count; c++)
        off += writeEntrySet(s, &s->nodes[dn->child_start + c], img + off);
}

static void buildBootRegion(Synth* s) {
    uint8_t* main = s->boot;
    memset(s->boot, 0, sizeof(s->boot));

    main[0] = 0xEB; main[1] = 0x76; main[2] = 0x90;
    memcpy(main + 3, "EXFAT   ", 8);
    put64(main + 72, s->vol_sectors);
    put32(main + 80, BOOT_SECTORS);
    put32(main + 84, s->fat_sectors);
    put32(main + 88, s->heap_off);
    put32(main + 92, s->cluster_count);
    put32(main + 96, s->nodes[0].first_cluster);
    put32(main + 100, SYNTH_SERIAL);
    put16(main + 104, 0x0100);
    main[108] = 9;
    main[109] = SPC_SHIFT;
    main[110] = 1;
    main[111] = 0x80;
    main[112] = 0xFF;
    main[510] = 0x55; main[511] = 0xAA;

    for (int i = 1; i <= 8; i++) {  // extended boot sectors carry the 0xAA550000 signature
        uint8_t* e = main + i * SECTOR_BYTES;
        e[SECTOR_BYTES - 2] = 0x55; e[SECTOR_BYTES - 1] = 0xAA;
    }

    uint32_t sum = checksum32(main, 11 * SECTOR_BYTES, 106, 107, 112, 0);
    for (uint32_t i = 0; i < SECTOR_BYTES / 4; i++) put32(main + 11 * SECTOR_BYTES + i * 4, sum);

    memcpy(main + 12 * SECTOR_BYTES, main, 12 * SECTOR_BYTES);  // backup boot region
}

static bool layout(Synth* s, char* err, size_t err_len) {
    // Directory sizes.
    uint64_t data_clusters = 0;
    for (uint32_t d = 0; d < s->n_nodes; d++) {
        Node* n = &s->nodes[d];
        if (n->is_dir) {
            uint64_t bytes = d == 0 ? 96 : 0;
            for (uint32_t c = 0; c < n->child_count; c++) bytes += entrySetBytes(&s->nodes[n->child_start + c]);
            n->size = bytes;
            n->n_clusters = bytes ? ceilDiv64(bytes, CLUSTER_BYTES) : 1;
        } else {
            n->n_clusters = ceilDiv64(n->size, CLUSTER_BYTES);
        }
        data_clusters += n->n_clusters;
    }
    data_clusters += 1;  // upcase table

    uint64_t headroom = data_clusters / 20;
    if (headroom < MIN_HEADROOM_CLUSTERS) headroom = MIN_HEADROOM_CLUSTERS;
    if (s->ovl && s->free_bytes) {
        // Everything the host writes lands twice on the card (overlay, then the committed file), so
        // offer at most half of the real free space, less some slack.
        uint64_t usable = s->free_bytes / 2;
        uint64_t slack = 256ull << 20;
        usable = usable > slack ? usable - slack : 0;
        headroom = usable / CLUSTER_BYTES;
        if (headroom < 1) headroom = 1;
    }
    if (s->ovl) {
        // The overlay is one file on a FAT32 card, so it cannot pass 4 GiB. Offering less than that
        // keeps a single large copy from failing half way; the host just reports a full disk.
        uint64_t cap = (3584ull << 20) / CLUSTER_BYTES;
        if (headroom > cap) headroom = cap;
    }

    uint32_t bm_clusters = 1;
    uint64_t total;
    for (;;) {
        total = data_clusters + headroom + bm_clusters;
        uint32_t need = ceilDiv64((total + 7) / 8, CLUSTER_BYTES);
        if (need <= bm_clusters) break;
        bm_clusters = need;
    }
    if (total > 0xFFFFFFF5u - 2) {
        snprintf(err, err_len, "tree too large for exFAT (%llu clusters)", (unsigned long long)total);
        return false;
    }
    s->cluster_count = (uint32_t)total;
    s->bitmap_bytes = (total + 7) / 8;
    s->bitmap_clusters = bm_clusters;

    // Allocation order: bitmap, upcase, then every node in scan order.
    s->ext = calloc((size_t)s->n_nodes + 2, sizeof(Extent));
    if (!s->ext) { snprintf(err, err_len, "out of memory"); return false; }
    uint32_t next = 2;
    s->bitmap_first = next;
    s->ext[s->n_ext++] = (Extent){ next, bm_clusters, EXT_BITMAP, 0 };
    next += bm_clusters;
    s->upcase_first = next;
    s->ext[s->n_ext++] = (Extent){ next, 1, EXT_UPCASE, 0 };
    next += 1;
    for (uint32_t i = 0; i < s->n_nodes; i++) {
        Node* n = &s->nodes[i];
        if (!n->n_clusters) continue;
        n->first_cluster = next;
        s->ext[s->n_ext++] = (Extent){ next, n->n_clusters, EXT_NODE, i };
        next += n->n_clusters;
    }
    s->alloc_clusters = next - 2;

    s->fat_sectors = ceilDiv64((uint64_t)(s->cluster_count + 2) * 4, SECTOR_BYTES);
    s->heap_off = ((BOOT_SECTORS + s->fat_sectors + HEAP_ALIGN - 1) / HEAP_ALIGN) * HEAP_ALIGN;
    s->vol_sectors = (uint64_t)s->heap_off + (uint64_t)s->cluster_count * SPC;

    s->upcase_checksum = checksum32((const uint8_t*)UPCASE_TABLE, UPCASE_BYTES, (size_t)-1, (size_t)-1, (size_t)-1, 0);

    for (uint32_t d = 0; d < s->n_nodes; d++) {
        Node* n = &s->nodes[d];
        if (!n->is_dir) continue;
        n->dir_img = calloc(1, n->size ? n->size : 32);
        if (!n->dir_img) { snprintf(err, err_len, "out of memory building directories"); return false; }
        buildDirImage(s, d, n->dir_img);
    }
    buildBootRegion(s);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Reading

static uint32_t fatEntry(const Synth* s, uint64_t i) {
    if (i == 0) return 0xFFFFFFF8u;
    if (i == 1) return 0xFFFFFFFFu;
    // Bitmap, upcase and the root directory are FAT-chained (contiguous); everything else is NoFatChain.
    uint32_t c = (uint32_t)i;
    const Node* root = &s->nodes[0];
    uint32_t spans[3][2] = {
        { s->bitmap_first, s->bitmap_clusters },
        { s->upcase_first, 1 },
        { root->first_cluster, root->n_clusters },
    };
    for (int k = 0; k < 3; k++) {
        if (c >= spans[k][0] && c < spans[k][0] + spans[k][1])
            return c == spans[k][0] + spans[k][1] - 1 ? 0xFFFFFFFFu : c + 1;
    }
    return 0;
}

static void genFat(const Synth* s, uint64_t sector, uint32_t count, uint8_t* out) {
    for (uint32_t k = 0; k < count; k++) {
        for (uint32_t j = 0; j < SECTOR_BYTES / 4; j++)
            put32(out + k * SECTOR_BYTES + j * 4, fatEntry(s, (sector + k) * (SECTOR_BYTES / 4) + j));
    }
}

static void genBitmap(const Synth* s, uint64_t off, size_t len, uint8_t* out) {
    for (size_t k = 0; k < len; k++) {
        uint64_t first_bit = (off + k) * 8;
        uint64_t alloc = s->alloc_clusters;
        uint8_t b;
        if (first_bit + 8 <= alloc) b = 0xFF;
        else if (first_bit >= alloc) b = 0;
        else b = (uint8_t)((1u << (alloc - first_bit)) - 1);
        out[k] = b;
    }
}

static bool readFile(Synth* s, uint32_t id, uint64_t off, size_t len, uint8_t* out) {
    const Node* n = &s->nodes[id];
    memset(out, 0, len);
    if (off >= n->size) return true;
    size_t want = len;
    if (off + want > n->size) want = (size_t)(n->size - off);

    if (s->fd < 0 || s->fd_node != id) {
        if (s->fd >= 0) { close(s->fd); s->fd = -1; }
        char path[MAX_PATH_LEN];
        if (!synthNodePath(s, id, path, sizeof(path))) return false;
        s->fd = open(path, O_RDONLY);
        if (s->fd < 0) return false;
        s->fd_node = id;
    }
    // Raw read(2) goes straight to the filesystem; stdio would copy through its own small buffer.
    if (lseek(s->fd, (off_t)off, SEEK_SET) < 0) return false;
    size_t done = 0;
    while (done < want) {
        ssize_t r = read(s->fd, out + done, want - done);
        if (r <= 0) return false;
        done += (size_t)r;
    }
    return true;
}

static bool readExtent(Synth* s, const Extent* e, uint64_t off, size_t len, uint8_t* out) {
    switch (e->kind) {
    case EXT_BITMAP:
        genBitmap(s, off, len, out);
        return true;
    case EXT_UPCASE:
        memset(out, 0, len);
        if (off < UPCASE_BYTES) {
            size_t n = UPCASE_BYTES - off < len ? (size_t)(UPCASE_BYTES - off) : len;
            memcpy(out, (const uint8_t*)UPCASE_TABLE + off, n);
        }
        return true;
    default: {
        const Node* n = &s->nodes[e->node];
        if (n->is_dir) {
            memset(out, 0, len);
            if (off < n->size) {
                size_t m = n->size - off < len ? (size_t)(n->size - off) : len;
                memcpy(out, n->dir_img + off, m);
            }
            return true;
        }
        return readFile(s, e->node, off, len, out);
    }
    }
}

// Last extent with first <= cluster, or n_ext if none.
uint32_t synthFindExtent(const Synth* s, uint32_t cluster) {
    uint32_t lo = 0, hi = s->n_ext;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (s->ext[mid].first <= cluster) lo = mid + 1; else hi = mid;
    }
    return lo ? lo - 1 : s->n_ext;
}

// The generated volume as scanned, before any host writes.
static bool baseRead(Synth* s, uint64_t lba, uint32_t count, void* out_v) {
    uint8_t* out = out_v;
    while (count) {
        uint64_t n;
        if (lba >= s->vol_sectors) return false;
        if (lba < BOOT_SECTORS) {
            n = BOOT_SECTORS - lba < count ? BOOT_SECTORS - lba : count;
            memcpy(out, s->boot + lba * SECTOR_BYTES, n * SECTOR_BYTES);
        } else if (lba < (uint64_t)BOOT_SECTORS + s->fat_sectors) {
            uint64_t end = (uint64_t)BOOT_SECTORS + s->fat_sectors;
            n = end - lba < count ? end - lba : count;
            genFat(s, lba - BOOT_SECTORS, (uint32_t)n, out);
        } else if (lba < s->heap_off) {
            n = s->heap_off - lba < count ? s->heap_off - lba : count;
            memset(out, 0, n * SECTOR_BYTES);
        } else {
            uint64_t rel = lba - s->heap_off;
            uint32_t cl = (uint32_t)(rel / SPC) + 2;
            uint32_t ei = synthFindExtent(s, cl);  // never n_ext: the first extent starts at cluster 2
            const Extent* e = &s->ext[ei];
            if (cl < e->first + e->count) {
                uint64_t ext_start = (uint64_t)(e->first - 2) * SPC;
                uint64_t ext_end = ext_start + (uint64_t)e->count * SPC;
                n = ext_end - rel < count ? ext_end - rel : count;
                if (!readExtent(s, e, (rel - ext_start) * SECTOR_BYTES, (size_t)n * SECTOR_BYTES, out)) return false;
            } else {  // unallocated space between extents or after the last one
                uint64_t gap_end = ei + 1 < s->n_ext ? (uint64_t)(s->ext[ei + 1].first - 2) * SPC
                                                     : (uint64_t)s->cluster_count * SPC;
                n = gap_end - rel < count ? gap_end - rel : count;
                memset(out, 0, (size_t)n * SECTOR_BYTES);
            }
        }
        out += n * SECTOR_BYTES;
        lba += n;
        count -= (uint32_t)n;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Host view: the generated volume with the overlay on top

bool synthViewRead(Synth* s, uint64_t lba, uint32_t count, void* out_v) {
    uint8_t* out = out_v;
    if (!s->ovl || overlayBlocks(s->ovl) == 0) return baseRead(s, lba, count, out);
    while (count) {
        uint64_t blk = lba / OVL_BLOCK_SECTORS;
        uint32_t in_blk = (uint32_t)(lba % OVL_BLOCK_SECTORS);
        uint32_t n;
        if (overlayHas(s->ovl, blk)) {
            if (in_blk == 0 && count >= OVL_BLOCK_SECTORS) {
                uint32_t k = 1;
                while ((k + 1) * OVL_BLOCK_SECTORS <= count && overlayHas(s->ovl, blk + k)) k++;
                if (!overlayReadBlocks(s->ovl, blk, k, out)) return false;
                n = k * OVL_BLOCK_SECTORS;
            } else {
                uint8_t tmp[OVL_BLOCK_BYTES];
                if (!overlayReadBlocks(s->ovl, blk, 1, tmp)) return false;
                n = OVL_BLOCK_SECTORS - in_blk < count ? OVL_BLOCK_SECTORS - in_blk : count;
                memcpy(out, tmp + in_blk * SECTOR_BYTES, (size_t)n * SECTOR_BYTES);
            }
        } else {
            // Longest run of sectors whose blocks are not in the overlay: one base read.
            n = OVL_BLOCK_SECTORS - in_blk;
            while (n < count && !overlayHas(s->ovl, (lba + n) / OVL_BLOCK_SECTORS)) n += OVL_BLOCK_SECTORS;
            if (n > count) n = count;
            if (!baseRead(s, lba, n, out)) return false;
        }
        out += (size_t)n * SECTOR_BYTES;
        lba += n;
        count -= n;
    }
    return true;
}

static bool synthRead(void* ctx, uint64_t lba, uint32_t count, void* out) {
    return synthViewRead(ctx, lba, count, out);
}

// Host writes never touch the card. They land in the overlay and reach the real files in synthCommit.
static bool synthWrite(void* ctx, uint64_t lba, uint32_t count, const void* in_v) {
    Synth* s = ctx;
    const uint8_t* in = in_v;
    if (!s->ovl) return false;  // read-only volume
    if (lba >= s->vol_sectors || count > s->vol_sectors - lba) return false;
    while (count) {
        uint64_t blk = lba / OVL_BLOCK_SECTORS;
        uint32_t in_blk = (uint32_t)(lba % OVL_BLOCK_SECTORS);
        uint32_t n;
        if (in_blk == 0 && count >= OVL_BLOCK_SECTORS) {
            n = (count / OVL_BLOCK_SECTORS) * OVL_BLOCK_SECTORS;
            if (!overlayWriteBlocks(s->ovl, blk, n / OVL_BLOCK_SECTORS, in)) return false;
        } else {  // partial block: read-modify-write against what the host currently sees
            uint8_t tmp[OVL_BLOCK_BYTES];
            if (!synthViewRead(s, blk * OVL_BLOCK_SECTORS, OVL_BLOCK_SECTORS, tmp)) return false;
            n = OVL_BLOCK_SECTORS - in_blk < count ? OVL_BLOCK_SECTORS - in_blk : count;
            memcpy(tmp + in_blk * SECTOR_BYTES, in, (size_t)n * SECTOR_BYTES);
            if (!overlayWriteBlocks(s->ovl, blk, 1, tmp)) return false;
        }
        in += (size_t)n * SECTOR_BYTES;
        lba += n;
        count -= n;
    }
    return true;
}

static bool synthFlush(void* ctx) { (void)ctx; return true; }

// ---------------------------------------------------------------------------------------------

void synthVolumeFree(Synth* s) {
    if (s->fd >= 0) { close(s->fd); s->fd = -1; }
    for (uint32_t i = 0; i < s->n_nodes; i++) free(s->nodes[i].dir_img);
    free(s->nodes);
    free(s->pool);
    free(s->ext);
    s->nodes = NULL; s->pool = NULL; s->ext = NULL;
    s->n_nodes = s->cap_nodes = s->n_ext = 0;
    s->pool_len = s->pool_cap = 0;
    memset(&s->stats, 0, sizeof(s->stats));
}

bool synthVolumeBuild(Synth* s, char* err, size_t err_len) {
    if (!scan(s, s->progress, s->progress_user, err, err_len) || !layout(s, err, err_len)) return false;
    s->be.block_count = s->vol_sectors;
    if (s->ovl) overlaySetGeometry(s->ovl, s->heap_off / OVL_BLOCK_SECTORS, SPC / OVL_BLOCK_SECTORS);
    return true;
}

uint64_t synthRealFreeBytes(const char* root) {
#if defined(__has_include)
#  if __has_include(<sys/statvfs.h>)
    struct statvfs sv;
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/", root);
    if (statvfs(path, &sv) == 0) return (uint64_t)sv.f_bavail * sv.f_frsize;
#  endif
#endif
    (void)root;
    return 0;
}

Backend* synthBackendCreateEx(const SynthOptions* o, char* err, size_t err_len) {
    Synth* s = calloc(1, sizeof(*s));
    if (!s) { snprintf(err, err_len, "out of memory"); return NULL; }
    s->fd = -1;
    snprintf(s->root, sizeof(s->root), "%s", o->root);
    s->progress = o->progress;
    s->progress_user = o->progress_user;

    if (o->overlay_path) {
        snprintf(s->overlay_path, sizeof(s->overlay_path), "%s", o->overlay_path);
        s->ovl = overlayOpen(s->overlay_path);
        if (!s->ovl) {
            snprintf(err, err_len, "cannot create %s", s->overlay_path);
            synthBackendDestroy(&s->be);
            return NULL;
        }
        s->free_bytes = o->free_bytes ? o->free_bytes : synthRealFreeBytes(s->root);
    }

    if (!synthVolumeBuild(s, err, err_len)) {
        synthBackendDestroy(&s->be);
        return NULL;
    }
    s->be.name = s->ovl ? "SD card (read-write)" : "SD card (read-only snapshot)";
    s->be.block_size = SECTOR_BYTES;
    s->be.read_only = s->ovl == NULL;
    s->be.ctx = s;
    s->be.read = synthRead;
    s->be.write = synthWrite;
    s->be.flush = synthFlush;
    return &s->be;
}

Backend* synthBackendCreate(const char* root, SynthProgress cb, void* user, char* err, size_t err_len) {
    SynthOptions o = { .root = root, .progress = cb, .progress_user = user };
    return synthBackendCreateEx(&o, err, err_len);
}

void synthBackendDestroy(Backend* be) {
    if (!be) return;
    Synth* s = (Synth*)be;
    synthVolumeFree(s);
    if (s->ovl) overlayClose(s->ovl, true);
    free(s);
}

const SynthStats* synthBackendStats(const Backend* be) { return &((const Synth*)be)->stats; }

void synthLayoutText(const Backend* be, char* out, size_t cap) {
    const Synth* s = (const Synth*)be;
    snprintf(out, cap, "volume: heap_off=%u sectors/cluster=%u fat_sectors=%u bitmap_first=%u upcase_first=%u clusters=%u used=%u",
             s->heap_off, SPC, s->fat_sectors, s->bitmap_first, s->upcase_first, s->cluster_count, s->alloc_clusters);
}

uint64_t synthPendingBytes(const Backend* be) {
    const Synth* s = (const Synth*)be;
    return s->ovl ? overlayBlocks(s->ovl) * OVL_BLOCK_BYTES : 0;
}
