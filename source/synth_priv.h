// Internals of the synthesized exFAT volume, shared by exfat_synth.c (read side) and synth_commit.c
// (write side). Not part of the public API.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "backend.h"
#include "exfat_synth.h"
#include "overlay.h"

// ---------------------------------------------------------------------------------------------
// Geometry

#define SECTOR_BYTES  512u
#define SPC_SHIFT     6u                       // 64 sectors per cluster = 32 KiB
#define SPC           (1u << SPC_SHIFT)
#define CLUSTER_BYTES (SECTOR_BYTES * SPC)
#define BOOT_SECTORS  24u                      // main (12) + backup (12) boot regions
#define HEAP_ALIGN    4096u                    // cluster heap starts on a 2 MiB boundary
#define MAX_PATH_LEN  1024
#define MAX_NAME16    255
#define SYNTH_SERIAL  0x4E585553u              // volume serial; a host reformat would change it
#define MIN_HEADROOM_CLUSTERS 4096u            // 128 MiB of free space shown to the host

#define ENT_LABEL   0x83
#define ENT_BITMAP  0x81
#define ENT_UPCASE  0x82
#define ENT_FILE    0x85
#define ENT_STREAM  0xC0
#define ENT_NAME    0xC1

// ---------------------------------------------------------------------------------------------
// Data model

typedef struct {
    uint32_t name_off;     // into Synth.pool (NUL-terminated UTF-8)
    uint32_t parent;
    uint32_t child_start;
    uint32_t child_count;
    uint64_t size;         // file: bytes. dir: bytes of directory entries
    uint32_t mtime;        // exFAT timestamp
    uint32_t first_cluster;
    uint32_t n_clusters;
    uint16_t name_len16;   // UTF-16 code units
    uint8_t  is_dir;
    uint8_t* dir_img;      // generated directory entries (dirs only)
} Node;

typedef enum { EXT_BITMAP, EXT_UPCASE, EXT_NODE } ExtKind;

typedef struct {
    uint32_t first;        // first cluster (>= 2)
    uint32_t count;
    uint32_t kind;
    uint32_t node;
} Extent;

typedef struct {
    Backend       be;      // must be first: Backend* <-> Synth*
    SynthStats    stats;
    char          root[MAX_PATH_LEN];
    SynthProgress progress;
    void*         progress_user;

    Node*         nodes;
    uint32_t      n_nodes, cap_nodes;
    char*         pool;
    size_t        pool_len, pool_cap;
    Extent*       ext;
    uint32_t      n_ext;

    uint32_t      cluster_count;     // total clusters in the heap
    uint32_t      alloc_clusters;    // clusters in use from cluster 2 (bitmap bits set)
    uint32_t      fat_sectors;
    uint32_t      heap_off;          // sector
    uint64_t      vol_sectors;
    uint32_t      bitmap_first, bitmap_clusters;
    uint64_t      bitmap_bytes;
    uint32_t      upcase_first;
    uint32_t      upcase_checksum;
    uint8_t       boot[BOOT_SECTORS * SECTOR_BYTES];

    int           fd;                // single-entry file handle cache (-1 = none), raw POSIX fd to skip stdio buffering
    uint32_t      fd_node;

    // Write support. ovl == NULL means a read-only volume.
    Overlay*      ovl;
    char          overlay_path[MAX_PATH_LEN];
    uint64_t      free_bytes;        // real free space at mount time, 0 = unknown
    uint32_t      commits;
} Synth;

static inline void put16(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void put32(uint8_t* p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static inline void put64(uint8_t* p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

// ---------------------------------------------------------------------------------------------
// Shared helpers (exfat_synth.c)

int      synthUtf8to16(const char* s, uint16_t* out, int max);
uint32_t synthDosTime(time_t t);
bool     synthNodePath(const Synth* s, uint32_t id, char* buf, size_t cap);
uint32_t synthFindExtent(const Synth* s, uint32_t cluster);

// Reads through the overlay (what the host sees).
bool     synthViewRead(Synth* s, uint64_t lba, uint32_t count, void* out);

uint64_t synthRealFreeBytes(const char* root);

// Throws away the scan and rebuilds it from the real tree. Used after a commit.
void     synthVolumeFree(Synth* s);
bool     synthVolumeBuild(Synth* s, char* err, size_t err_len);
