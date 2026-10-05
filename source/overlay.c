#include "overlay.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Where the blocks live. Scattered writes go to slots in one scratch file (an in-RAM hash table maps block number to slot).
// Long runs of consecutive blocks that start on a cluster boundary, which is what a file copy looks like, go to their own
// segment file instead, laid out exactly like the volume, so the commit can rename a segment into place as the finished file.
#define SEG_MAX         64                 // segments per commit; later runs fall back to slots
#define SEG_MAX_OPEN    8                  // file handles kept open
#define SEG_MIN_BLOCKS  16                 // a run must be at least this long (64 KiB) to start a segment
#define SEG_MAX_BLOCKS  ((0xFFFFFFFFull) / OVL_BLOCK_BYTES)  // FAT32: a file is at most 4 GiB - 1
#define SEG_GROW_BYTES  (64ull * 1024 * 1024)
#define SEG_PREFIX      "ovlseg_"

typedef struct {
    uint64_t start;     // first volume block
    uint64_t nblk;      // blocks present, all consecutive from start
    uint64_t alloc;     // bytes the file has been grown to
    int      fd;        // -1 = closed
    uint32_t id;
    uint32_t lru;
    bool     dead;      // renamed away by the commit
} Seg;

struct Overlay {
    int       fd;
    char*     path;
    char*     dir;      // where segment files go (next to the scratch file)
    uint64_t* keys;     // block number + 1, 0 = empty slot (open addressing)
    uint32_t* slots;    // position of the block in the scratch file, in blocks
    uint32_t  cap;      // power of two
    uint32_t  used;
    uint64_t  alloc;    // bytes the scratch file has been grown to
    uint64_t* sorted;   // present block numbers in order, built lazily for range queries
    uint32_t  sorted_n;
    bool      sorted_valid;

    Seg       segs[SEG_MAX];
    int       nsegs;
    uint32_t  next_id, tick;
    uint64_t  seg_blocks;           // blocks held in live segments
    bool      seg_on;
    uint64_t  heap_blk, cluster_blks;
};

static uint32_t hashKey(uint64_t key, uint32_t cap) {
    return (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 32) & (cap - 1);
}

// Index of the table entry holding blk, or UINT32_MAX.
static uint32_t find(const Overlay* o, uint64_t blk) {
    uint64_t key = blk + 1;
    for (uint32_t i = hashKey(key, o->cap);; i = (i + 1) & (o->cap - 1)) {
        if (o->keys[i] == key) return i;
        if (o->keys[i] == 0) return UINT32_MAX;
    }
}

static bool insert(Overlay* o, uint64_t blk, uint32_t slot) {
    if ((uint64_t)(o->used + 1) * 10 > (uint64_t)o->cap * 7) {
        uint32_t ncap = o->cap * 2;
        uint64_t* nk = calloc(ncap, sizeof(uint64_t));
        uint32_t* ns = malloc((size_t)ncap * sizeof(uint32_t));
        if (!nk || !ns) { free(nk); free(ns); return false; }
        for (uint32_t i = 0; i < o->cap; i++) {
            if (!o->keys[i]) continue;
            uint32_t j = hashKey(o->keys[i], ncap);
            while (nk[j]) j = (j + 1) & (ncap - 1);
            nk[j] = o->keys[i];
            ns[j] = o->slots[i];
        }
        free(o->keys); free(o->slots);
        o->keys = nk; o->slots = ns; o->cap = ncap;
    }
    uint64_t key = blk + 1;
    uint32_t i = hashKey(key, o->cap);
    while (o->keys[i]) i = (i + 1) & (o->cap - 1);
    o->keys[i] = key;
    o->slots[i] = slot;
    o->used++;
    o->sorted_valid = false;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Segments

static void segPath(const Overlay* o, const Seg* s, char* out, size_t cap) {
    snprintf(out, cap, "%s/" SEG_PREFIX "%u.bin", o->dir, s->id);
}

// Index of the live segment holding blk, or -1.
static int segFind(const Overlay* o, uint64_t blk) {
    for (int i = 0; i < o->nsegs; i++) {
        const Seg* s = &o->segs[i];
        if (!s->dead && blk >= s->start && blk < s->start + s->nblk) return i;
    }
    return -1;
}

// Blocks from blk up to where the next live segment starts (UINT64_MAX when there is none).
static uint64_t segRoomBefore(const Overlay* o, uint64_t blk) {
    uint64_t best = UINT64_MAX;
    for (int i = 0; i < o->nsegs; i++) {
        const Seg* s = &o->segs[i];
        if (!s->dead && s->start > blk && s->start - blk < best) best = s->start - blk;
    }
    return best;
}

// The live segment that ends exactly at blk, or -1.
static int segEndingAt(const Overlay* o, uint64_t blk) {
    for (int i = 0; i < o->nsegs; i++) {
        const Seg* s = &o->segs[i];
        if (!s->dead && s->start + s->nblk == blk) return i;
    }
    return -1;
}

static void segClose(Seg* s) {
    if (s->fd >= 0) { close(s->fd); s->fd = -1; }
}

static int segFd(Overlay* o, int i) {
    Seg* s = &o->segs[i];
    s->lru = ++o->tick;
    if (s->fd >= 0) return s->fd;
    int open_n = 0, victim = -1;
    for (int k = 0; k < o->nsegs; k++) {
        if (o->segs[k].fd < 0) continue;
        open_n++;
        if (victim < 0 || o->segs[k].lru < o->segs[victim].lru) victim = k;
    }
    if (open_n >= SEG_MAX_OPEN && victim >= 0) segClose(&o->segs[victim]);
    char p[1100];
    segPath(o, s, p, sizeof(p));
    s->fd = open(p, O_RDWR);
    return s->fd;
}

static bool readAt(int fd, uint64_t off, void* out, size_t len) {
    if (lseek(fd, (off_t)off, SEEK_SET) < 0) return false;
    uint8_t* p = out;
    while (len) {
        ssize_t r = read(fd, p, len);
        if (r <= 0) return false;
        p += r; len -= (size_t)r;
    }
    return true;
}

static bool writeAt(int fd, uint64_t off, const void* in, size_t len) {
    if (lseek(fd, (off_t)off, SEEK_SET) < 0) return false;
    const uint8_t* p = in;
    while (len) {
        ssize_t w = write(fd, p, len);
        if (w <= 0) return false;
        p += w; len -= (size_t)w;
    }
    return true;
}

// Writes count blocks at blk, which is inside segment i or exactly at its end (the caller checked), growing the file in big steps.
static bool segWrite(Overlay* o, int i, uint64_t blk, uint32_t count, const void* in) {
    Seg* s = &o->segs[i];
    int fd = segFd(o, i);
    if (fd < 0) return false;
    uint64_t off = (blk - s->start) * OVL_BLOCK_BYTES, end = off + (uint64_t)count * OVL_BLOCK_BYTES;
    if (end > s->alloc) {
        uint64_t want = (end + SEG_GROW_BYTES - 1) / SEG_GROW_BYTES * SEG_GROW_BYTES;
        if (want > 0xFFFFFFFFull) want = 0xFFFFFFFFull;
        if (want >= end && ftruncate(fd, (off_t)want) == 0) s->alloc = want;
        else s->alloc = end;
    }
    if (!writeAt(fd, off, in, (size_t)count * OVL_BLOCK_BYTES)) return false;
    uint64_t top = blk + count - s->start;
    if (top > s->nblk) { o->seg_blocks += top - s->nblk; s->nblk = top; }
    return true;
}

static int segCreate(Overlay* o, uint64_t start) {
    if (o->nsegs >= SEG_MAX) return -1;
    Seg* s = &o->segs[o->nsegs];
    memset(s, 0, sizeof(*s));
    s->start = start;
    s->id = ++o->next_id;
    s->fd = -1;
    char p[1100];
    segPath(o, s, p, sizeof(p));
    int fd = open(p, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return -1;
    close(fd);
    o->nsegs++;
    return o->nsegs - 1;
}

static void segsDropAll(Overlay* o, bool remove_files) {
    for (int i = 0; i < o->nsegs; i++) {
        segClose(&o->segs[i]);
        if (remove_files && !o->segs[i].dead) {
            char p[1100];
            segPath(o, &o->segs[i], p, sizeof(p));
            unlink(p);
        }
    }
    o->nsegs = 0;
    o->seg_blocks = 0;
}

// Segment files from an earlier run (a crash, or the app was closed mid-copy) are never wanted again.
static void segsRemoveStale(const char* dir) {
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* de;
    char p[1100];
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, SEG_PREFIX, strlen(SEG_PREFIX)) != 0) continue;
        snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
        unlink(p);
    }
    closedir(d);
}

void overlaySetGeometry(Overlay* o, uint64_t heap_first_blk, uint32_t cluster_blocks) {
    if (!o) return;
    o->heap_blk = heap_first_blk;
    o->cluster_blks = cluster_blocks;
    o->seg_on = cluster_blocks > 0;
}

int overlayFindSegment(Overlay* o, uint64_t first_blk, uint64_t need_blocks, uint64_t max_blocks) {
    for (int i = 0; i < o->nsegs; i++) {
        const Seg* s = &o->segs[i];
        if (!s->dead && s->start == first_blk && s->nblk >= need_blocks && s->nblk <= max_blocks) return i;
    }
    return -1;
}

bool overlayTakeSegment(Overlay* o, int idx, const char* dest, uint64_t size) {
    if (idx < 0 || idx >= o->nsegs || o->segs[idx].dead) return false;
    Seg* s = &o->segs[idx];
    int fd = segFd(o, idx);
    if (fd < 0) return false;
    if (ftruncate(fd, (off_t)size) != 0) return false;  // cut the slack after the file; a shorter segment is zero-extended
    fsync(fd);
    segClose(s);                                        // Horizon will not rename a file that is open
    char p[1100];
    segPath(o, s, p, sizeof(p));
    if (rename(p, dest) != 0) return false;
    s->dead = true;
    o->seg_blocks -= s->nblk;
    return true;
}

void overlayCloseFiles(Overlay* o) {
    for (int i = 0; i < o->nsegs; i++) segClose(&o->segs[i]);
}

// ---------------------------------------------------------------------------------------------
// Lifetime

Overlay* overlayOpen(const char* path) {
    Overlay* o = calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->cap = 1024;
    o->keys = calloc(o->cap, sizeof(uint64_t));
    o->slots = malloc((size_t)o->cap * sizeof(uint32_t));
    o->path = strdup(path);
    o->dir = strdup(path);
    o->fd = -1;
    if (o->dir) {
        char* sl = strrchr(o->dir, '/');
        if (sl) *sl = 0; else { free(o->dir); o->dir = strdup("."); }
    }
    if (o->keys && o->slots && o->path && o->dir) {
        segsRemoveStale(o->dir);
        o->fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0666);
    }
    if (!o->keys || !o->slots || !o->path || !o->dir || o->fd < 0) {
        overlayClose(o, false);
        return NULL;
    }
    return o;
}

void overlayClose(Overlay* o, bool remove_file) {
    if (!o) return;
    segsDropAll(o, remove_file);
    if (o->fd >= 0) close(o->fd);
    if (remove_file && o->path) unlink(o->path);
    free(o->path); free(o->dir); free(o->keys); free(o->slots); free(o->sorted);
    free(o);
}

bool overlayReset(Overlay* o) {
    segsDropAll(o, true);
    memset(o->keys, 0, (size_t)o->cap * sizeof(uint64_t));
    o->used = 0;
    o->alloc = 0;
    o->sorted_valid = false;
    return ftruncate(o->fd, 0) == 0;
}

uint64_t overlayBlocks(const Overlay* o) { return (uint64_t)o->used + o->seg_blocks; }

bool overlayHas(const Overlay* o, uint64_t blk) { return find(o, blk) != UINT32_MAX || segFind(o, blk) >= 0; }

// ---------------------------------------------------------------------------------------------
// Reading and writing blocks

bool overlayReadBlocks(Overlay* o, uint64_t blk, uint32_t count, void* out_v) {
    uint8_t* out = out_v;
    while (count) {
        int si = segFind(o, blk);
        if (si >= 0) {
            const Seg* s = &o->segs[si];
            uint64_t left = s->start + s->nblk - blk;
            uint32_t run = left < count ? (uint32_t)left : count;
            int fd = segFd(o, si);
            if (fd < 0 || !readAt(fd, (blk - s->start) * OVL_BLOCK_BYTES, out, (size_t)run * OVL_BLOCK_BYTES)) return false;
            out += (size_t)run * OVL_BLOCK_BYTES;
            blk += run;
            count -= run;
            continue;
        }
        uint32_t i = find(o, blk);
        if (i == UINT32_MAX) return false;
        uint32_t first_slot = o->slots[i];
        uint32_t run = 1;
        while (run < count) {  // blocks that sit side by side in the file go out as one read
            uint32_t j = find(o, blk + run);
            if (j == UINT32_MAX || o->slots[j] != first_slot + run) break;
            run++;
        }
        if (!readAt(o->fd, (uint64_t)first_slot * OVL_BLOCK_BYTES, out, (size_t)run * OVL_BLOCK_BYTES)) return false;
        out += (size_t)run * OVL_BLOCK_BYTES;
        blk += run;
        count -= run;
    }
    return true;
}

// The scratch file grows in big steps: extending a FAT32 file on every small write costs far more than the write.
#define OVL_GROW_BYTES (64ull * 1024 * 1024)

static void reserve(Overlay* o, uint64_t end) {
    if (end <= o->alloc) return;
    uint64_t want = (end + OVL_GROW_BYTES - 1) / OVL_GROW_BYTES * OVL_GROW_BYTES;
    if (ftruncate(o->fd, (off_t)want) == 0) o->alloc = want;
    else o->alloc = end;  // could not preallocate: plain writes still extend the file
}

// Scattered blocks: into slots, overwriting in place where a block is already held.
static bool slotWrite(Overlay* o, uint64_t blk, uint32_t count, const uint8_t* in) {
    while (count) {
        uint32_t i = find(o, blk);
        uint32_t run = 1;
        uint32_t first_slot;
        if (i != UINT32_MAX) {  // overwrite in place; extend over neighbours that are consecutive in the file
            first_slot = o->slots[i];
            while (run < count) {
                uint32_t j = find(o, blk + run);
                if (j == UINT32_MAX || o->slots[j] != first_slot + run) break;
                run++;
            }
        } else {  // new blocks get consecutive slots at the end of the file
            first_slot = o->used;
            while (run < count && find(o, blk + run) == UINT32_MAX) run++;
        }
        reserve(o, ((uint64_t)first_slot + run) * OVL_BLOCK_BYTES);
        if (!writeAt(o->fd, (uint64_t)first_slot * OVL_BLOCK_BYTES, in, (size_t)run * OVL_BLOCK_BYTES)) return false;
        if (i == UINT32_MAX) {
            for (uint32_t k = 0; k < run; k++)
                if (!insert(o, blk + k, first_slot + k)) return false;
        }
        in += (size_t)run * OVL_BLOCK_BYTES;
        blk += run;
        count -= run;
    }
    return true;
}

// How many blocks from blk (up to count) are not held in slots.
static uint32_t slotFreeRun(const Overlay* o, uint64_t blk, uint32_t count) {
    uint32_t n = 0;
    while (n < count && find(o, blk + n) == UINT32_MAX) n++;
    return n;
}

bool overlayWriteBlocks(Overlay* o, uint64_t blk, uint32_t count, const void* in_v) {
    const uint8_t* in = in_v;
    while (count) {
        uint32_t n;
        int si = segFind(o, blk);
        if (si >= 0) {  // inside a segment: overwrite in place
            const Seg* s = &o->segs[si];
            uint64_t left = s->start + s->nblk - blk;
            n = left < count ? (uint32_t)left : count;
            if (!segWrite(o, si, blk, n, in)) return false;
        } else {
            uint64_t room = segRoomBefore(o, blk);  // never run into the next segment
            uint32_t lim = room < count ? (uint32_t)room : count;
            int ei = o->seg_on ? segEndingAt(o, blk) : -1;
            uint32_t free_run = o->seg_on ? slotFreeRun(o, blk, lim) : 0;
            n = 0;
            if (ei >= 0 && free_run > 0) {  // the next stretch of a file being copied
                uint64_t cap = SEG_MAX_BLOCKS - o->segs[ei].nblk;
                n = free_run < cap ? free_run : (uint32_t)cap;
                if (n && !segWrite(o, ei, blk, n, in)) return false;
            } else if (ei < 0 && free_run >= SEG_MIN_BLOCKS && blk >= o->heap_blk && o->nsegs < SEG_MAX) {
                // a long run: a new file, if it can start on a cluster boundary. Anything before the first boundary
                // (the tail of a folder's directory cluster, say) goes to slots.
                uint64_t into = (blk - o->heap_blk) % o->cluster_blks;
                uint32_t lead = into ? (uint32_t)(o->cluster_blks - into) : 0;
                if (lead == 0 && free_run >= SEG_MIN_BLOCKS) {
                    int ni = segCreate(o, blk);
                    if (ni >= 0) {
                        n = free_run < SEG_MAX_BLOCKS ? free_run : (uint32_t)SEG_MAX_BLOCKS;
                        if (!segWrite(o, ni, blk, n, in)) return false;
                    }
                } else if (lead > 0 && free_run >= lead + SEG_MIN_BLOCKS) {
                    n = lead;
                    if (!slotWrite(o, blk, n, in)) return false;
                }
            }
            if (!n) {  // not a segment write: slots
                n = lim;
                if (!slotWrite(o, blk, n, in)) return false;
            }
        }
        in += (size_t)n * OVL_BLOCK_BYTES;
        blk += n;
        count -= n;
    }
    return true;
}

static int cmp64(const void* a, const void* b) {
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return x < y ? -1 : x > y;
}

bool overlayAnyInRange(Overlay* o, uint64_t first_blk, uint64_t count) {
    if (!count) return false;
    for (int i = 0; i < o->nsegs; i++) {
        const Seg* s = &o->segs[i];
        if (!s->dead && s->start < first_blk + count && first_blk < s->start + s->nblk) return true;
    }
    if (!o->used) return false;
    if (!o->sorted_valid) {
        free(o->sorted);
        o->sorted = malloc((size_t)o->used * sizeof(uint64_t));
        if (!o->sorted) return true;  // cannot answer: be conservative, caller treats it as "changed"
        uint32_t n = 0;
        for (uint32_t i = 0; i < o->cap; i++)
            if (o->keys[i]) o->sorted[n++] = o->keys[i] - 1;
        qsort(o->sorted, n, sizeof(uint64_t), cmp64);
        o->sorted_n = n;
        o->sorted_valid = true;
    }
    uint32_t lo = 0, hi = o->sorted_n;  // first element >= first_blk
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (o->sorted[mid] < first_blk) lo = mid + 1; else hi = mid;
    }
    return lo < o->sorted_n && o->sorted[lo] - first_blk < count;
}

#ifdef OVL_DEBUG
void overlayDebug(Overlay* o) {
    for (int i = 0; i < o->nsegs; i++)
        fprintf(stderr, "  seg %d start=%llu nblk=%llu dead=%d\n", i, (unsigned long long)o->segs[i].start, (unsigned long long)o->segs[i].nblk, o->segs[i].dead);
}
#endif
