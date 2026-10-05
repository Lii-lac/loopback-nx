// Write overlay: host sector writes go here instead of to the card.
//
// Storage is a scratch file, addressed in 4 KiB blocks (8 sectors). An in-RAM hash table maps a block
// number to its slot in the file, so RAM use is about 12 bytes per written 4 KiB.
//
// Long runs of consecutive blocks that begin on a cluster boundary (a file being copied in) are kept in separate
// "segment" files instead, laid out exactly like the volume. A segment that holds exactly one finished file can be
// renamed into place by the commit, so the file's data is never copied.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define OVL_BLOCK_SECTORS 8u
#define OVL_BLOCK_BYTES   4096u

typedef struct Overlay Overlay;

// Creates (or truncates) the scratch file. NULL on failure.
Overlay* overlayOpen(const char* path);
void     overlayClose(Overlay* o, bool remove_file);

// Drops every block and truncates the scratch file.
bool     overlayReset(Overlay* o);

uint64_t overlayBlocks(const Overlay* o);
bool     overlayHas(const Overlay* o, uint64_t blk);

// Whole-block I/O. Reads require every block to be present. Coalesces blocks that sit next to each
// other in the scratch file into one read or write.
bool     overlayReadBlocks(Overlay* o, uint64_t blk, uint32_t count, void* out);
bool     overlayWriteBlocks(Overlay* o, uint64_t blk, uint32_t count, const void* in);

// True if any block in [first_blk, first_blk + count) is present.
bool     overlayAnyInRange(Overlay* o, uint64_t first_blk, uint64_t count);

// Tells the overlay where the cluster heap starts and how big a cluster is, in blocks. Segments only begin on cluster
// boundaries inside the heap. Call it again whenever the volume layout is rebuilt.
void     overlaySetGeometry(Overlay* o, uint64_t heap_first_blk, uint32_t cluster_blocks);

// A live segment that starts exactly at first_blk, holds at least need_blocks and at most max_blocks; its index, or -1.
int      overlayFindSegment(Overlay* o, uint64_t first_blk, uint64_t need_blocks, uint64_t max_blocks);
// Cuts the segment to size bytes and renames it to dest (same file system). The segment is gone from the overlay afterwards.
// On failure the segment is still intact.
bool     overlayTakeSegment(Overlay* o, int idx, const char* dest, uint64_t size);
// Closes every open segment file (before directories are renamed).
void     overlayCloseFiles(Overlay* o);
