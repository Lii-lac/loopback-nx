// Turns what the host wrote to the synthesized volume into real file operations on the card.
//
// The host only ever sees a raw exFAT image. Its writes sit in the overlay until a commit:
//   1. Parse the overlay-applied image (exfat_parse.c) into a tree.
//   2. Match tree nodes to the nodes from the scan by cluster identity.
//   3. Plan: what was created, deleted, moved or rewritten. Everything that can fail is checked here
//      and a failure aborts with the card untouched.
//   4. Apply, in an order that never overwrites anything in place (see apply()).
//   5. Rescan the card and start a fresh volume. The host is told the media changed.
#include "synth_priv.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "exfat_parse.h"

#define NONE              0xFFFFFFFFu
#define DEFAULT_MASS_LIMIT 50u
#define MAX_FILE_BYTES     0xFFFFFFFFull     // FAT32: a file is at most 4 GiB - 1
#define COPY_CHUNK         (2u * 1024u * 1024u)
#define MAX_DEPTH          256

enum { F_MOVED = 1, F_MODIFIED = 2, F_STALE = 4 };  // F_STALE: the host's entry is out of date; the file on the card stays as it is

typedef struct {
    Synth*            s;
    ExfatTree         t;
    const CommitOpts* o;
    CommitReport*     r;

    uint32_t* match;     // per tree node: the scan node it is, or NONE
    uint32_t* rmatch;    // per scan node: the tree node it became, or NONE (= deleted)
    uint8_t*  flags;     // per tree node
    uint8_t*  parked;    // per scan node: moved into the stage directory
    uint64_t  stage_total, stage_done;  // bytes of new file data to stage, and staged so far
    bool      cancelled;                // the progress callback asked to stop
    uint8_t*  prot;      // per scan node: host housekeeping already on the card; never touched
    uint8_t*  buf;       // copy buffer
    uint64_t  read_ns, write_ns, sync_ns;  // where copying time went, for the log
    int*      zc;        // per tree node: the overlay segment that already holds the whole file (renamed into place, never copied), or -1
    uint64_t  zc_bytes;  // bytes in such files
    uint32_t  zc_files;
    char      stage[MAX_PATH_LEN];
} Commit;

static void commitLog(Commit* c, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void commitLog(Commit* c, const char* fmt, ...) {
    if (!c->o->log) return;
    char line[MAX_PATH_LEN + 64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    c->o->log(line, c->o->user);
}

static bool abortWith(Commit* c, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static bool abortWith(Commit* c, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->r->err, sizeof(c->r->err), fmt, ap);
    va_end(ap);
    return false;
}

static bool viewCb(void* user, uint64_t lba, uint32_t count, void* out) {
    return synthViewRead(user, lba, count, out);
}

// ---------------------------------------------------------------------------------------------
// Paths

static const char* oldName(const Commit* c, uint32_t id) { return c->s->pool + c->s->nodes[id].name_off; }

// Where a scan node is right now, taking nodes that were moved to the stage directory into account.
static bool oldPathNow(const Commit* c, uint32_t id, char* buf, size_t cap) {
    uint32_t chain[MAX_DEPTH];
    int depth = 0;
    uint32_t i = id;
    for (;; i = c->s->nodes[i].parent) {
        if (c->parked[i]) break;
        if (i == 0) break;
        if (depth == MAX_DEPTH) return false;
        chain[depth++] = i;
    }
    int n;
    if (c->parked[i]) n = snprintf(buf, cap, "%s/p%u", c->stage, i);
    else              n = snprintf(buf, cap, "%s", c->s->root);
    if (n < 0 || (size_t)n >= cap) return false;
    size_t len = (size_t)n;
    while (depth--) {
        const char* name = oldName(c, chain[depth]);
        size_t l = strlen(name);
        if (len + 1 + l + 1 > cap) return false;
        buf[len++] = '/';
        memcpy(buf + len, name, l);
        len += l;
    }
    buf[len] = 0;
    return true;
}

// Where a tree node belongs in the finished card.
static bool newPath(const Commit* c, uint32_t idx, char* buf, size_t cap) {
    uint32_t chain[MAX_DEPTH];
    int depth = 0;
    for (uint32_t i = idx; i != 0; i = c->t.nodes[i].parent) {
        if (depth == MAX_DEPTH) return false;
        chain[depth++] = i;
    }
    size_t len = strlen(c->s->root);
    if (len + 1 >= cap) return false;
    memcpy(buf, c->s->root, len);
    while (depth--) {
        const char* name = c->t.nodes[chain[depth]].name;
        size_t l = strlen(name);
        if (len + 1 + l + 1 > cap) return false;
        buf[len++] = '/';
        memcpy(buf + len, name, l);
        len += l;
    }
    buf[len] = 0;
    return true;
}

static void stagePath(const Commit* c, char* buf, char kind, uint32_t id) {
    int n = snprintf(buf, MAX_PATH_LEN, "%s/%c%u", c->stage, kind, id);
    if (n < 0 || n >= MAX_PATH_LEN) buf[0] = 0;  // cannot happen: the stage path is short; fails cleanly if it does
}

// What the card (FAT32) accepts as a name.
static bool nameOk(const char* n) {
    size_t l = strlen(n);
    if (!l || n[l - 1] == ' ' || n[l - 1] == '.') return false;  // also rejects "." and ".."
    for (const char* p = n; *p; p++) {
        if ((unsigned char)*p < 0x20 || strchr("\"*/:<>?\\|", *p)) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Matching

typedef struct { Synth* s; uint32_t old_first; uint32_t runs; bool dirty; } DirtyCtx;

static bool runCb(void* user, uint32_t first, uint32_t count) {
    DirtyCtx* d = user;
    if (d->runs++ || first != d->old_first) { d->dirty = true; return false; }  // not the original contiguous run
    uint64_t sector = d->s->heap_off + ((uint64_t)(first - 2) << SPC_SHIFT);
    if (overlayAnyInRange(d->s->ovl, sector / OVL_BLOCK_SECTORS, (uint64_t)count * SPC / OVL_BLOCK_SECTORS))
        d->dirty = true;
    return !d->dirty;
}

// A matched file counts as rewritten if its size changed, its clusters moved, or the host wrote to them.
static bool isModified(Commit* c, const ExfatNode* n, const Node* on, bool* modified) {
    *modified = true;
    if (n->data_len != on->size || n->valid_len != n->data_len) return true;
    if (!n->data_len) { *modified = false; return true; }
    DirtyCtx d = { c->s, on->first_cluster, 0, false };
    if (!exfatForEachRun(&c->t, n, runCb, &d)) return abortWith(c, "%s", c->t.err);
    *modified = d.dirty;
    return true;
}

static bool unchangedOnCard(Commit* c, uint32_t old_id) {
    char path[MAX_PATH_LEN];
    struct stat st;
    if (!synthNodePath(c->s, old_id, path, sizeof(path)) || stat(path, &st) != 0) return false;
    const Node* on = &c->s->nodes[old_id];
    return S_ISREG(st.st_mode) && (uint64_t)st.st_size == on->size && synthDosTime(st.st_mtime) == on->mtime;
}

// Whether the host actually wrote a file's data. A file the host creates always sends its data through the overlay, so a new entry
// whose data is not there is not a new file: either the host has not got to the data yet (it writes the folder entry first), or
// it is an out-of-date copy of a folder block that the host wrote back after a commit had already moved everything to new places.
// Reading such an entry would create a file full of zeros, or of another file's bytes.
typedef struct { Synth* s; uint64_t remaining; bool missing; } WrittenCtx;

static bool writtenRunCb(void* user, uint32_t first, uint32_t count) {
    WrittenCtx* w = user;
    uint64_t bytes = (uint64_t)count * CLUSTER_BYTES;
    if (bytes > w->remaining) bytes = w->remaining;
    uint64_t blk = (w->s->heap_off + ((uint64_t)(first - 2) << SPC_SHIFT)) / OVL_BLOCK_SECTORS;
    uint64_t n = (bytes + OVL_BLOCK_BYTES - 1) / OVL_BLOCK_BYTES;
    for (uint64_t b = 0; b < n; b++)
        if (!overlayHas(w->s->ovl, blk + b)) { w->missing = true; return false; }
    w->remaining -= bytes;
    return w->remaining > 0;
}

static bool dataMissing(Commit* c, const ExfatNode* n, bool* missing) {
    *missing = false;
    if (!n->data_len) return true;
    if (!n->valid_len || n->first_cluster < 2) { *missing = true; return true; }
    WrittenCtx w = { c->s, n->valid_len, false };
    if (!exfatForEachRun(&c->t, n, writtenRunCb, &w)) return abortWith(c, "%s", c->t.err);
    *missing = w.missing;
    return true;
}

typedef struct { uint32_t depth, id; } DepthId;

static int cmpDeepFirst(const void* a, const void* b) {
    const DepthId* x = a;
    const DepthId* y = b;
    if (x->depth != y->depth) return x->depth > y->depth ? -1 : 1;
    return x->id < y->id ? -1 : x->id > y->id;
}

static uint32_t oldDepth(const Commit* c, uint32_t id) {
    uint32_t d = 0;
    for (; id != 0; id = c->s->nodes[id].parent) d++;
    return d;
}

static bool plan(Commit* c) {
    Synth* s = c->s;
    ExfatTree* t = &c->t;
    CommitReport* r = c->r;

    if (t->vol_len != s->vol_sectors || t->heap_off != s->heap_off || t->fat_off != BOOT_SECTORS ||
        t->fat_len != s->fat_sectors || t->serial != SYNTH_SERIAL || t->cluster_count != s->cluster_count || t->spc_shift != SPC_SHIFT)
        return abortWith(c, "the host changed the volume layout (reformatted?)");

    c->match = malloc((size_t)t->n_nodes * sizeof(uint32_t));
    c->rmatch = malloc((size_t)s->n_nodes * sizeof(uint32_t));
    c->flags = calloc(t->n_nodes, 1);
    c->parked = calloc(s->n_nodes, 1);
    c->prot = calloc(s->n_nodes, 1);
    c->buf = malloc(COPY_CHUNK);
    if (!c->match || !c->rmatch || !c->flags || !c->parked || !c->prot || !c->buf) return abortWith(c, "out of memory");
    for (uint32_t i = 0; i < t->n_nodes; i++) c->match[i] = NONE;
    for (uint32_t i = 0; i < s->n_nodes; i++) c->rmatch[i] = NONE;
    c->match[0] = c->rmatch[0] = 0;

    // The card may already hold folders like "System Volume Information" (from other PCs). The host's
    // copy of those is ignored, so the card's must not count as deleted. Scan order puts parents first.
    for (uint32_t i = 1; i < s->n_nodes; i++) {
        uint32_t par = s->nodes[i].parent;
        c->prot[i] = par == 0 ? exfatIsHostHousekeeping(oldName(c, i)) : c->prot[par];
    }

    // Identity: a node is the same one if it still starts at the same cluster. Empty files have no
    // cluster, so those match by name inside the matched parent.
    for (uint32_t i = 1; i < t->n_nodes; i++) {
        const ExfatNode* n = &t->nodes[i];
        if (n->ignored) { r->ignored++; continue; }
        uint32_t old = NONE;
        if (n->first_cluster) {
            uint32_t ei = synthFindExtent(s, n->first_cluster);
            if (ei < s->n_ext && s->ext[ei].kind == EXT_NODE && s->ext[ei].first == n->first_cluster) {
                uint32_t cand = s->ext[ei].node;
                if (cand != 0 && (bool)s->nodes[cand].is_dir == n->is_dir) old = cand;
            }
        } else if (!n->is_dir) {
            uint32_t po = c->match[n->parent];
            if (po != NONE) {
                const Node* pn = &s->nodes[po];
                for (uint32_t k = 0; k < pn->child_count; k++) {
                    uint32_t id = pn->child_start + k;
                    const Node* cn = &s->nodes[id];
                    if (!cn->is_dir && !cn->size && c->rmatch[id] == NONE && !strcasecmp(oldName(c, id), n->name)) { old = id; break; }
                }
            }
        }
        if (old == NONE) continue;
        if (c->rmatch[old] != NONE) return abortWith(c, "'%s' shares clusters with another entry", n->name);
        c->match[i] = old;
        c->rmatch[old] = i;
    }

    // A created file whose data never arrived (see dataMissing). If the card already has a file with that name in that folder, the
    // entry is stale and the file stays as it is. Otherwise the host has not finished the copy: refuse for now, so the next
    // commit, after the data has arrived, picks it up whole.
    for (uint32_t i = 1; i < t->n_nodes; i++) {
        const ExfatNode* n = &t->nodes[i];
        if (n->ignored || n->is_dir || c->match[i] != NONE) continue;
        bool missing;
        if (!dataMissing(c, n, &missing)) return false;
        if (!missing) continue;
        uint32_t po = c->match[n->parent], old = NONE;
        if (po != NONE) {
            const Node* pn = &s->nodes[po];
            for (uint32_t k = 0; k < pn->child_count; k++) {
                uint32_t id = pn->child_start + k;
                if (!s->nodes[id].is_dir && c->rmatch[id] == NONE && !strcmp(oldName(c, id), n->name)) { old = id; break; }
            }
        }
        if (old == NONE) return abortWith(c, "'%s' has no data yet: the PC has not finished writing it", n->name);
        c->match[i] = old;
        c->rmatch[old] = i;
        c->flags[i] |= F_STALE;
        commitLog(c, "kept '%s' as it is: the PC's entry for it is out of date and carries no data", n->name);
    }

    // Classify, and check everything that could fail later.
    char path[MAX_PATH_LEN];
    for (uint32_t i = 1; i < t->n_nodes; i++) {
        const ExfatNode* n = &t->nodes[i];
        if (n->ignored) continue;
        if (!nameOk(n->name)) return abortWith(c, "name not allowed on the card: '%s'", n->name);
        if (!newPath(c, i, path, sizeof(path))) return abortWith(c, "path too long: '%s'", n->name);

        if (c->flags[i] & F_STALE) continue;
        uint32_t old = c->match[i];
        bool rewrite = false;
        if (old == NONE) {
            if (n->is_dir) { r->dirs_created++; continue; }
            r->files_created++;
            rewrite = n->data_len > 0;
        } else {
            const Node* on = &s->nodes[old];
            uint32_t po = c->match[n->parent];
            if (on->parent != po || strcmp(oldName(c, old), n->name) != 0) { c->flags[i] |= F_MOVED; r->moved++; }
            if (!n->is_dir) {
                bool modified;
                if (!isModified(c, n, on, &modified)) return false;
                if (modified) { c->flags[i] |= F_MODIFIED; r->files_rewritten++; rewrite = true; }
            }
        }
        if (rewrite) {
            if (n->data_len > MAX_FILE_BYTES) return abortWith(c, "'%s' would be %llu bytes; the card cannot hold a file over 4 GiB", n->name, (unsigned long long)n->data_len);
            r->bytes += n->data_len;
        }
    }
    for (uint32_t i = 1; i < s->n_nodes; i++) {
        if (c->rmatch[i] != NONE || c->prot[i]) continue;
        if (s->nodes[i].is_dir) r->dirs_deleted++; else r->files_deleted++;
    }

    // Anything the Switch changed since the scan must not be moved, replaced or deleted from under it.
    uint32_t conflicts = 0;
    for (uint32_t i = 1; i < s->n_nodes; i++) {
        if (s->nodes[i].is_dir || c->prot[i]) continue;
        uint32_t ni = c->rmatch[i];
        bool touched = ni == NONE || (c->flags[ni] & (F_MOVED | F_MODIFIED));
        if (!touched || unchangedOnCard(c, i)) continue;
        if (conflicts++ < 5 && synthNodePath(s, i, path, sizeof(path))) commitLog(c, "changed on the Switch since the scan: %s", path);
    }
    if (conflicts) return abortWith(c, "%u file(s) changed on the Switch since the scan; reconnect to refresh", conflicts);

    r->changes = r->dirs_created + r->files_created + r->files_rewritten + r->moved + r->files_deleted + r->dirs_deleted;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Apply

static bool writeAll(int fd, const uint8_t* p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

static void pump(const Commit* c) {
    if (c->o->pump) c->o->pump(c->o->user);
}

static uint64_t nowNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// Writes the finished contents of tree node idx to its stage file.
static bool materialize(Commit* c, uint32_t idx) {
    const ExfatNode* n = &c->t.nodes[idx];
    char dst[MAX_PATH_LEN];
    stagePath(c, dst, 'n', idx);
    int fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) { commitLog(c, "cannot create %s (errno %d)", dst, errno); return false; }
    // Reserve the whole file first: a FAT32 file that grows 2 MiB at a time pays to find and chain free clusters on every write.
    if (n->data_len > COPY_CHUNK && ftruncate(fd, (off_t)n->data_len) != 0) lseek(fd, 0, SEEK_SET);  // not reserved: plain growth still works
    bool ok = true;
    {
        ExfatCursor cur = { 0, 0 };
        for (uint64_t off = 0; off < n->data_len && ok;) {
            size_t m = n->data_len - off < COPY_CHUNK ? (size_t)(n->data_len - off) : COPY_CHUNK;
            uint64_t t0 = nowNs();
            bool rd = exfatReadFile(&c->t, n, &cur, off, m, c->buf);
            uint64_t t1 = nowNs();
            c->read_ns += t1 - t0;
            if (!rd) { commitLog(c, "read failed for '%s': %s", n->name, c->t.err); ok = false; }
            else if (!writeAll(fd, c->buf, m)) { commitLog(c, "write failed for '%s' (errno %d)", n->name, errno); ok = false; }
            c->write_ns += nowNs() - t1;
            off += m;
            c->stage_done += m;
            pump(c);
            if (ok && c->o->progress && !c->o->progress(c->stage_done, c->stage_total, c->o->user)) {
                commitLog(c, "cancelled while copying '%s'", n->name);
                c->cancelled = true;
                ok = false;
            }
        }
    }
    uint64_t ts = nowNs();
    if (ok) fsync(fd);
    if (close(fd) != 0 && ok) { commitLog(c, "close failed for '%s'", n->name); ok = false; }
    c->sync_ns += nowNs() - ts;
    if (!ok) unlink(dst);
    return ok;
}

static void fail(Commit* c, const char* what, const char* path) {
    c->r->errors++;
    commitLog(c, "ERROR %s %s (errno %d)", what, path, errno);
}

// Files whose contents have to be written out: created ones, and ones the host changed.
static bool needsContents(const Commit* c, uint32_t i) {
    const ExfatNode* n = &c->t.nodes[i];
    return !n->ignored && !n->is_dir && ((c->flags[i] & F_MODIFIED) || c->match[i] == NONE);
}

// Step 0 of the commit: write the new contents of every created or rewritten file into the stage
// directory. Nothing on the card has changed yet, so a failure here (card full, read error) aborts cleanly.
static bool stageContents(Commit* c) {
    c->stage_total = c->stage_done = 0;
    for (uint32_t i = 1; i < c->t.n_nodes; i++) {
        if (needsContents(c, i) && c->zc[i] < 0) c->stage_total += c->t.nodes[i].data_len;
    }
    for (uint32_t i = 1; i < c->t.n_nodes; i++) {
        if (needsContents(c, i) && c->zc[i] < 0 && !materialize(c, i)) return false;
    }
    if (c->zc_files)
        commitLog(c, "%u file(s), %.0f MiB, are already in place and only get renamed", c->zc_files, (double)c->zc_bytes / 1048576.0);
    if (c->stage_total >= (64u << 20))
        commitLog(c, "copied %.0f MiB: read %.1f s, write %.1f s, flush %.1f s", (double)c->stage_total / 1048576.0, (double)c->read_ns / 1e9,
                  (double)c->write_ns / 1e9, (double)c->sync_ns / 1e9);
    return true;
}

// A file copied in by the host sits in the overlay as one segment that begins at the file's first cluster. When the file is
// contiguous, whole and fully written, and nothing else shares its segment, the segment simply is the file: it is cut to size
// and renamed into place by apply(), and none of its bytes are read or written again.
#define ZC_MIN_BYTES (4ull << 20)

typedef struct { uint32_t runs; uint64_t clusters; } RunCount;
static bool countRun(void* user, uint32_t first, uint32_t count) {
    (void)first;
    RunCount* rc = user;
    rc->runs++;
    rc->clusters += count;
    return true;
}

static void pickZeroCopy(Commit* c) {
    for (uint32_t i = 1; i < c->t.n_nodes; i++) {
        if (!needsContents(c, i)) continue;
        const ExfatNode* n = &c->t.nodes[i];
        if (n->data_len < ZC_MIN_BYTES || n->first_cluster < 2 || n->valid_len != n->data_len) continue;
        uint64_t clusters = (n->data_len + CLUSTER_BYTES - 1) / CLUSTER_BYTES;
        if (!n->no_fat_chain) {
            RunCount rc = { 0, 0 };
            if (!exfatForEachRun(&c->t, n, countRun, &rc) || rc.runs != 1 || rc.clusters < clusters) continue;
        }
        uint64_t first_blk = ((uint64_t)c->s->heap_off + (uint64_t)(n->first_cluster - 2) * SPC) / OVL_BLOCK_SECTORS;
        uint64_t need = (n->data_len + OVL_BLOCK_BYTES - 1) / OVL_BLOCK_BYTES;
        int seg = overlayFindSegment(c->s->ovl, first_blk, need, UINT64_MAX);  // a segment that runs on into the next file is cut to size
#ifdef OVL_DEBUG
        fprintf(stderr, "  zc? %s first_blk=%llu need=%llu max=%llu -> %d\n", n->name, (unsigned long long)first_blk, (unsigned long long)need, 0ull, seg);
        { extern void overlayDebug(Overlay*); overlayDebug(c->s->ovl); }
#endif
        if (seg < 0) continue;
        c->zc[i] = seg;
        c->zc_bytes += n->data_len;
        c->zc_files++;
    }
}

static void discardStage(Commit* c) {
    char p[MAX_PATH_LEN];
    for (uint32_t i = 1; i < c->t.n_nodes; i++) {
        if (!needsContents(c, i)) continue;
        stagePath(c, p, 'n', i);
        unlink(p);
    }
    rmdir(c->stage);
}

// Order matters, and nothing is ever overwritten in place:
//   0. stageContents() has already written new file contents into the stage directory.
//   1. Park every moved or rewritten scan node in the stage directory, deepest first.
//   2. Delete scan nodes that are gone: files, then directories deepest first.
//   3. Walk the finished tree top-down: make directories, move parked nodes into place, move the
//      new files in.
//   4. Remove the stage directory. Anything still in it is a file the commit could not place.
// A commit that dies half way leaves files in the stage directory rather than losing them.
static void apply(Commit* c) {
    Synth* s = c->s;
    ExfatTree* t = &c->t;
    char a[MAX_PATH_LEN], b[MAX_PATH_LEN];

    // Horizon refuses to rename or remove a directory while anything below it is open (TargetLocked,
    // seen as EIO). The volume view keeps the last file the host read open, so let go of it first.
    if (s->fd >= 0) { close(s->fd); s->fd = -1; }
    overlayCloseFiles(s->ovl);

    DepthId* order = malloc((size_t)s->n_nodes * sizeof(DepthId));
    if (!order) { c->r->errors++; commitLog(c, "out of memory"); return; }
    uint32_t n_order = 0;
    for (uint32_t i = 1; i < t->n_nodes; i++) {
        if (t->nodes[i].ignored || c->match[i] == NONE || !(c->flags[i] & (F_MOVED | F_MODIFIED))) continue;
        order[n_order++] = (DepthId){ oldDepth(c, c->match[i]), c->match[i] };
    }
    qsort(order, n_order, sizeof(DepthId), cmpDeepFirst);
    for (uint32_t k = 0; k < n_order; k++) {
        pump(c);
        uint32_t id = order[k].id;
        if (!oldPathNow(c, id, a, sizeof(a))) { c->r->errors++; commitLog(c, "ERROR path too long for %s", oldName(c, id)); continue; }
        stagePath(c, b, 'p', id);
        if (rename(a, b) != 0) { fail(c, "park", a); continue; }
        c->parked[id] = 1;
    }

    n_order = 0;
    for (uint32_t i = 1; i < s->n_nodes; i++) {
        pump(c);
        if (c->rmatch[i] != NONE || c->prot[i] || s->nodes[i].is_dir) continue;
        if (!oldPathNow(c, i, a, sizeof(a))) { c->r->errors++; continue; }
        commitLog(c, "delete %s", a);
        if (unlink(a) != 0) fail(c, "delete", a);
    }
    for (uint32_t i = 1; i < s->n_nodes; i++) {
        if (c->rmatch[i] == NONE && !c->prot[i] && s->nodes[i].is_dir) order[n_order++] = (DepthId){ oldDepth(c, i), i };
    }
    qsort(order, n_order, sizeof(DepthId), cmpDeepFirst);
    for (uint32_t k = 0; k < n_order; k++) {
        pump(c);
        if (!oldPathNow(c, order[k].id, a, sizeof(a))) { c->r->errors++; continue; }
        commitLog(c, "rmdir %s", a);
        if (rmdir(a) != 0) fail(c, "rmdir", a);
    }
    free(order);

    for (uint32_t i = 1; i < t->n_nodes; i++) {
        pump(c);
        const ExfatNode* n = &t->nodes[i];
        if (n->ignored) continue;
        uint32_t old = c->match[i];
        uint8_t fl = c->flags[i];
        if (old != NONE && !(fl & (F_MOVED | F_MODIFIED))) continue;
        if (!newPath(c, i, b, sizeof(b))) { c->r->errors++; continue; }

        if (old == NONE && n->is_dir) {
            commitLog(c, "mkdir %s", b);
            if (mkdir(b, 0777) != 0 && errno != EEXIST) fail(c, "mkdir", b);
        } else if (old == NONE || (fl & F_MODIFIED)) {
            if (old != NONE && !c->parked[old]) continue;  // parking failed, already reported; the old file stays
            stagePath(c, a, 'n', i);
            commitLog(c, "%s %s (%llu bytes)", old == NONE ? "create" : "rewrite", b, (unsigned long long)n->data_len);
            if (c->zc[i] >= 0) {
                if (!overlayTakeSegment(s->ovl, c->zc[i], b, n->data_len)) {  // cannot rename it (another file system?): copy it
                    commitLog(c, "cannot rename the copied-in data into place, copying it");
                    if (!materialize(c, i)) { fail(c, "place", b); continue; }
                    if (rename(a, b) != 0) { fail(c, "place", b); continue; }
                }
            } else if (rename(a, b) != 0) { fail(c, "place", b); continue; }
            if (old != NONE) {
                stagePath(c, a, 'p', old);
                if (unlink(a) != 0) fail(c, "remove old copy of", b);
            }
        } else {  // moved, content unchanged
            if (!c->parked[old]) continue;  // parking failed, already reported
            stagePath(c, a, 'p', old);
            commitLog(c, "move %s", b);
            if (rename(a, b) != 0) fail(c, "move to", b);
        }
    }

    if (rmdir(c->stage) != 0) {
        c->r->errors++;
        commitLog(c, "ERROR files left in %s for recovery", c->stage);
    }
}

// ---------------------------------------------------------------------------------------------

static void logPlan(Commit* c) {
    const ExfatTree* t = &c->t;
    char a[MAX_PATH_LEN], b[MAX_PATH_LEN];
    for (uint32_t i = 1; i < c->s->n_nodes; i++) {
        if (c->rmatch[i] == NONE && !c->prot[i] && synthNodePath(c->s, i, a, sizeof(a))) commitLog(c, "would delete %s", a);
    }
    for (uint32_t i = 1; i < t->n_nodes; i++) {
        if (t->nodes[i].ignored || !newPath(c, i, b, sizeof(b))) continue;
        uint32_t old = c->match[i];
        if (old == NONE) {
            commitLog(c, t->nodes[i].is_dir ? "would mkdir %s" : "would create %s", b);
        } else if (c->flags[i] & F_MOVED) {
            synthNodePath(c->s, old, a, sizeof(a));
            commitLog(c, "would move %s -> %s", a, b);
        }
        if (old != NONE && (c->flags[i] & F_MODIFIED)) commitLog(c, "would rewrite %s", b);
    }
}

static void freeCommit(Commit* c) {
    exfatFree(&c->t);
    free(c->match); free(c->rmatch); free(c->flags); free(c->parked); free(c->prot); free(c->buf); free(c->zc);
}

bool synthCommit(Backend* be, const CommitOpts* opts, CommitReport* report) {
    Synth* s = (Synth*)be;
    CommitReport* r = report;
    memset(r, 0, sizeof(*r));
    if (!s->ovl) { snprintf(r->err, sizeof(r->err), "volume is read-only"); return false; }

    Commit c;
    memset(&c, 0, sizeof(c));
    c.s = s;
    c.o = opts;
    c.r = r;

    if (!exfatParse(&c.t, viewCb, s)) {
        snprintf(r->err, sizeof(r->err), "cannot read the volume yet: %s", c.t.err);
        freeCommit(&c);
        return false;
    }
    if (!plan(&c)) { freeCommit(&c); return false; }

    if (!r->changes) {  // only housekeeping (timestamps, dirty flag, ignored folders): nothing to put on the card
        freeCommit(&c);
        return true;
    }

    uint32_t limit = opts->mass_limit ? opts->mass_limit : DEFAULT_MASS_LIMIT;
    uint32_t destructive = r->files_deleted + r->dirs_deleted + r->files_rewritten;
    if (destructive > limit) {
        r->needs_confirm = true;
        if (!opts->dry_run && !(opts->confirm && opts->confirm(r, opts->user))) {
            snprintf(r->err, sizeof(r->err), "%u deletions/overwrites need confirmation (limit %u)", destructive, limit);
            freeCommit(&c);
            return false;
        }
    }

    if (opts->dry_run) {
        logPlan(&c);
        freeCommit(&c);
        return true;
    }

    c.zc = malloc((size_t)c.t.n_nodes * sizeof(int));
    if (!c.zc) { snprintf(r->err, sizeof(r->err), "out of memory"); freeCommit(&c); return false; }
    for (uint32_t i = 0; i < c.t.n_nodes; i++) c.zc[i] = -1;
    {   // renaming a segment into place only works within one file system
        struct stat sa, sb;
        char root[MAX_PATH_LEN + 2], dir[MAX_PATH_LEN + 2];
        snprintf(root, sizeof(root), "%s/", s->root);
        snprintf(dir, sizeof(dir), "%s", s->overlay_path);
        char* sl = strrchr(dir, '/');
        if (sl) sl[1] = 0; else snprintf(dir, sizeof(dir), "./");
        if (stat(root, &sa) == 0 && stat(dir, &sb) == 0 && sa.st_dev == sb.st_dev) pickZeroCopy(&c);
    }

    // New contents are written out before anything is moved or deleted, so the card needs room for all of it
    // (files that are already in place need none).
    uint64_t room = synthRealFreeBytes(s->root);
    uint64_t need_bytes = r->bytes > c.zc_bytes ? r->bytes - c.zc_bytes : 0;
    if (room && need_bytes + (8ull << 20) > room) {
        snprintf(r->err, sizeof(r->err), "not enough free space on the card: need %llu MiB, have %llu MiB",
                 (unsigned long long)(need_bytes >> 20) + 8, (unsigned long long)(room >> 20));
        freeCommit(&c);
        return false;
    }

    // Stage directory: a name not in use at the root, because an earlier crashed commit may have left one.
    bool made = false;
    for (int i = 0; i < 100 && !made; i++) {
        int n = i ? snprintf(c.stage, sizeof(c.stage), "%s/.nxusb-stage-%d", s->root, i)
                  : snprintf(c.stage, sizeof(c.stage), "%s/.nxusb-stage", s->root);
        if (n < 0 || (size_t)n >= sizeof(c.stage)) break;
        made = mkdir(c.stage, 0777) == 0;
        if (!made && errno != EEXIST) break;
    }
    if (!made) {
        snprintf(r->err, sizeof(r->err), "cannot create the stage directory (errno %d)", errno);
        freeCommit(&c);
        return false;
    }

    if (!stageContents(&c)) {
        discardStage(&c);
        if (c.cancelled) snprintf(r->err, sizeof(r->err), "cancelled while copying; the card is unchanged");
        else snprintf(r->err, sizeof(r->err), "could not stage the new file contents (card full?); the card is unchanged");
        freeCommit(&c);
        return false;
    }

    r->applied = true;
    apply(&c);
    freeCommit(&c);

    // The card is now the truth. Rescan it and start a fresh volume; the host must remount.
    s->free_bytes = synthRealFreeBytes(s->root);
    synthVolumeFree(s);
    char err[160];
    if (!synthVolumeBuild(s, err, sizeof(err))) {
        snprintf(r->err, sizeof(r->err), "rescan after commit failed: %s", err);
        r->errors++;
        return false;
    }
    overlayReset(s->ovl);
    s->be.media_changed = true;
    s->commits++;
    return r->errors == 0;
}
