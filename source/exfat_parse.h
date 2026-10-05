// Reads the directory tree out of an exFAT volume image, given only a sector read callback.
//
// Used on the write path: the host (Windows) has modified the volume through the overlay, and this
// turns the resulting raw sectors back into a tree of names, sizes and cluster chains.
//
// The parser is strict. Anything inconsistent (bad checksum, truncated entry set, cross-linked
// directory, chain running off the heap) fails the whole parse with a message, so a half-written
// image is never mistaken for a deliberate change.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef bool (*ExfatRead)(void* user, uint64_t lba, uint32_t count, void* out);

typedef struct {
    char*    name;          // UTF-8, owned by the tree
    uint32_t parent;        // index of the parent directory (the root is its own parent)
    uint32_t first_cluster; // 0 for an empty file
    uint64_t data_len;      // file size in bytes
    uint64_t valid_len;     // bytes actually written; the rest of the file reads as zero
    uint32_t mtime;         // exFAT timestamp
    uint16_t attrs;
    bool     is_dir;
    bool     no_fat_chain;  // clusters are contiguous from first_cluster
    bool     ignored;       // Windows housekeeping at the root; listed but not descended into
    uint32_t child_start;   // children are consecutive in the nodes array
    uint32_t child_count;
} ExfatNode;

typedef struct {
    ExfatRead rd;
    void*     user;

    uint32_t  spc_shift;    // sectors per cluster, as a shift
    uint32_t  fat_off, fat_len, heap_off, cluster_count, root_cluster;
    uint32_t  serial;
    uint64_t  vol_len;      // sectors

    ExfatNode* nodes;       // [0] is the root; parents come before their children
    uint32_t  n_nodes, cap_nodes;

    uint8_t   fat_sector[512];
    uint64_t  fat_lba;      // sector held in fat_sector, UINT64_MAX = none
    char      err[160];
} ExfatTree;

// Checksum stored in the first entry of a directory entry set (bytes 2 and 3 are skipped).
uint16_t exfatEntrySetChecksum(const uint8_t* d, size_t n);

// Names the host creates at the root for its own bookkeeping ("System Volume Information", "$RECYCLE.BIN").
// The parser lists them as ignored and does not descend; the commit leaves any such folder on the card alone.
bool exfatIsHostHousekeeping(const char* name);

// Parses the volume. On failure returns false with t->err set; free with exfatFree either way.
bool exfatParse(ExfatTree* t, ExfatRead rd, void* user);
void exfatFree(ExfatTree* t);

// Position inside a file's cluster chain, for sequential reads.
typedef struct {
    uint32_t index;         // cluster index within the file
    uint32_t cluster;       // cluster number at that index; 0 = not positioned yet
} ExfatCursor;

// Reads len bytes of a file starting at off (off + len <= data_len). Bytes past valid_len are zero.
// Sequential reads are cheap; seeking backwards restarts the walk from the first cluster.
bool exfatReadFile(ExfatTree* t, const ExfatNode* n, ExfatCursor* cur, uint64_t off, size_t len, uint8_t* out);

// Calls fn for each run of consecutive clusters in the file's chain (enough to hold data_len).
// Stops early if fn returns false. Returns false on a broken chain, with t->err set.
typedef bool (*ExfatRunFn)(void* user, uint32_t first_cluster, uint32_t count);
bool exfatForEachRun(ExfatTree* t, const ExfatNode* n, ExfatRunFn fn, void* user);
