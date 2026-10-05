// exFAT volume synthesized from a directory tree, optionally writable through an overlay.
//
// Scans a directory tree once (snapshot), assigns every file and directory a contiguous cluster
// range, and generates the exFAT metadata (boot region, FAT, bitmap, upcase table, directory
// entry sets) on demand. File data is read straight from the real files.
//
// Pure C + POSIX (opendir/stat/fopen), so it builds on the Switch ("sdmc:/") and on the PC.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "backend.h"

typedef struct {
    uint32_t dirs;
    uint32_t files;
    uint32_t skipped;    // unreadable dirs/entries, names we cannot represent
    uint64_t data_bytes; // sum of file sizes
} SynthStats;

// Called during the scan, roughly every few hundred entries. May be NULL.
typedef void (*SynthProgress)(const SynthStats* st, void* user);

// Called for every entry the scan leaves out, with its path and the reason. Optional.
typedef void (*SynthSkipLog)(const char* path, const char* why);
void synthSetSkipLog(SynthSkipLog fn);

typedef struct {
    const char*   root;           // directory to expose, no trailing slash ("sdmc:" or "/tmp/tree")
    SynthProgress progress;       // optional
    void*         progress_user;
    // Write support. NULL keeps the volume read-only. Otherwise host writes go to this scratch file
    // and reach the card only in synthCommit.
    const char*   overlay_path;
    uint64_t      free_bytes;     // real free space; 0 = ask the OS. Bounds the free space the host sees.
} SynthOptions;

// Returns NULL and fills err on failure.
Backend* synthBackendCreateEx(const SynthOptions* opts, char* err, size_t err_len);
// Read-only volume, as before.
Backend* synthBackendCreate(const char* root, SynthProgress cb, void* user, char* err, size_t err_len);
void     synthBackendDestroy(Backend* be);
const SynthStats* synthBackendStats(const Backend* be);

// A path (as used for the scan, e.g. "sdmc:/switch/loopback") that is left out of the volume entirely, so the
// PC can neither see nor change the app's own scratch files. NULL = nothing hidden.
void synthSetHiddenPath(const char* path);

// Bytes the host has written since the last commit (0 for a read-only volume).
uint64_t synthPendingBytes(const Backend* be);

// One line describing the volume layout (heap start, cluster size, ...), for the log.
void     synthLayoutText(const Backend* be, char* out, size_t cap);

// ---------------------------------------------------------------------------------------------
// Commit: apply the host's changes to the real files.

typedef struct {
    bool     applied;         // the card was modified (even if some operations failed)
    bool     needs_confirm;   // more deletions/overwrites than the mass-change guard allows
    uint32_t changes;         // operations planned
    uint32_t dirs_created, files_created, files_rewritten, moved, files_deleted, dirs_deleted;
    uint32_t ignored;         // Windows housekeeping not copied to the card
    uint64_t bytes;           // file data to write
    uint32_t errors;          // operations that failed while applying
    char     err[200];        // why nothing was applied, when it returns false before applying
} CommitReport;

typedef struct {
    bool     dry_run;         // plan and log only, touch nothing
    uint32_t mass_limit;      // deletions + overwrites allowed without confirmation; 0 = 50
    bool   (*confirm)(const CommitReport* planned, void* user);  // asked when over the limit; NULL = refuse
    void   (*log)(const char* line, void* user);                  // one line per operation; may be NULL
    void*    user;
    void   (*pump)(void* user);  // called often while committing, so the caller can keep the USB host answered; may be NULL
    bool   (*progress)(uint64_t done, uint64_t total, void* user);  // while new file data is copied; false cancels (card untouched); may be NULL
} CommitOpts;

// Parses what the host wrote, compares it with the card and applies the difference. On success the
// volume is rescanned and be->media_changed is set so the host remounts it.
// Returns false (with the card untouched) if the image is inconsistent or the plan is refused, and
// false (with report->applied) if some operations failed; either way the report says what happened.
// Returns true with nothing applied if the host only changed housekeeping.
bool synthCommit(Backend* be, const CommitOpts* opts, CommitReport* report);
