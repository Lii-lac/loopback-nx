// Self-update from the GitHub releases of this project.
//
// A check asks github.com where /releases/latest points (a redirect to /releases/tag/vX.Y.Z), so it needs no API token and no JSON.
// An install downloads loopback.nro and loopback.nro.sha256 from that tag, checks the hash and the NRO header, and only then swaps the
// file in, keeping the old one as loopback.nro.bak. Nothing on the card changes unless every check passes.
//
// The engine below is synchronous and also builds on a PC (tests/update_test.c). The app runs it on a worker thread through the
// Async functions.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifndef UPD_REPO_URL
#define UPD_REPO_URL "https://github.com/Lii-lac/loopback-nx"
#endif
#define UPD_ASSET "loopback.nro"

typedef enum { UPD_IDLE, UPD_CHECKING, UPD_CURRENT, UPD_AVAILABLE, UPD_DOWNLOADING, UPD_READY, UPD_FAILED } UpdState;

typedef struct {
    UpdState state;
    int      pct;         // download progress 0..100
    char     latest[24];  // newest version found, "1.0.1" (empty until a check has succeeded)
    char     msg[120];    // one sentence for the screen
} UpdStatus;

// ---- pure helpers (no network, no console)

// Compares dotted versions with an optional leading "v" ("v1.0.1" vs "1.0.0"). Returns -1, 0, 1, or -2 when either is not a version.
int  updVersionCmp(const char* a, const char* b);
// ".../releases/tag/v1.2.3" -> "v1.2.3". False when the URL does not name a release tag or the tag is not a version.
bool updTagFromUrl(const char* url, char* tag, size_t cap);
// First 64 hex digits of a "sha256sum" style file, lowercased into hex[65].
bool updParseSha(const char* text, char hex[65]);
// SHA-256 of a file as lowercase hex.
bool updSha256File(const char* path, char hex[65]);
// Puts `tmp` in place of `target`, keeping the old target as `bak`. If the second step fails the old file is put back.
// Returns NULL on success or a short reason.
const char* updSwapIn(const char* tmp, const char* target, const char* bak);

// ---- engine

// `running` is this build's version, `target` the NRO that is replaced, `work_dir` holds the download and the backup.
void updInit(const char* running, const char* target, const char* work_dir);
void updCheckNow(void);
void updInstallNow(void);   // needs a successful check first
void updCancel(void);       // stops a running check or download at the next progress tick
void updGet(UpdStatus* out);
bool updBusy(void);         // a check or install is running

#ifdef __SWITCH__
void updCheckAsync(void);
void updInstallAsync(void);
void updShutdown(void);     // cancels and waits for the worker
#endif
