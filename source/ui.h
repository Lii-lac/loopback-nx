// Loopback's screens: the Chicago-L route map, the status board, the signs, Advanced, and the dialogs.
// Pure drawing and input logic on top of gfx.h. The application feeds it a model each frame and acts on the
// UiAction it returns, so the same code is rendered to images in the PC tests.
#pragma once
#include <stdbool.h>
#include "gfx.h"

#define UI_W 1280
#define UI_H 720

typedef enum { UI_IDLE, UI_READING, UI_WAITING, UI_MOUNTED, UI_PENDING, UI_SAVING, UI_GONE } UiState;
typedef enum { UI_SHARE_WHOLE, UI_SHARE_TEST, UI_SHARE_RAM } UiShare;
typedef enum { UI_THEME_AUTO, UI_THEME_LIGHT, UI_THEME_DARK } UiTheme;  // AUTO follows the console's own setting

// Same order as UpdState in update.h (main.c converts); ui.c does not depend on the update engine.
typedef enum { UI_UPD_IDLE, UI_UPD_CHECKING, UI_UPD_CURRENT, UI_UPD_AVAILABLE, UI_UPD_DOWNLOADING, UI_UPD_READY, UI_UPD_FAILED } UiUpd;

typedef struct {
    UiState     state;
    bool        dark;
    const char* version;      // this build's version, "1.0.0"
    UiUpd       upd;          // Advanced > Updates
    int         upd_pct;      // download progress 0..100
    char        upd_latest[24];
    char        upd_msg[120];
    bool        pending;      // the PC wrote data that is not on the card yet
    bool        connecting;   // the cable is in and the PC is setting the drive up, but has not mounted it yet
    int         pct;          // saving progress 0..100
    unsigned    files;        // files found so far while reading
    int         battery;      // percent, or -1 when unknown
    bool        charging;
    const char* note;         // replaces the caption while Ready or Mounted, or NULL
    const char* link;         // "High-Speed", ...
    char        read_text[24], written_text[24];
    float       read_mbps, write_mbps;  // current transfer rates, MB/s (0 = idle)
    bool        io_busy;      // the PC has read or written very recently, so pulling the cable now would cut a transfer
    unsigned    errors;
    const char* log[8];       // newest last
    int         n_log;
} UiModel;

typedef struct { bool up, down, left, right, a, b, x, y, plus, r; } UiKeys;  // edges: true once per press

typedef enum {
    UIA_NONE,
    UIA_MOUNT,         // start: read the card, then wait for the PC
    UIA_SAVE,          // commit what the PC wrote
    UIA_CANCEL,        // cancel the running save
    UIA_QUIT,          // quit now (nothing pending)
    UIA_QUIT_SAVE,     // save, then quit
    UIA_QUIT_DISCARD,  // quit without saving
    UIA_EJECT,         // stop sharing and go back to Ready
    UIA_EJECT_SAVE,    // save, then eject
    UIA_EJECT_DISCARD, // eject without saving
    UIA_GUARD_APPLY,   // the mass-change prompt: apply
    UIA_GUARD_REFUSE,  // the mass-change prompt: refuse
    UIA_UPD_CHECK,     // look for a newer release
    UIA_UPD_INSTALL,   // download and install the release the check found (only offered when nothing is mounted)
    UIA_UPD_CANCEL,    // stop the check or download
    UIA_UPD_RESTART    // quit and start Loopback again, now the new version
} UiAction;

void     uiInit(void);
// Picks which real L lines this run draws. Only lines that really run on the Loop are used (Brown, Green, Orange, Pink, Purple),
// in their real colours; call it each time the card is mounted. The same seed always gives the same lines.
void     uiRandomizeLines(unsigned seed);
void     uiSetModel(const UiModel* m);
UiAction uiKeys(const UiKeys* k);
UiAction uiTouch(int x, int y);       // a tap at screen coordinates (0..1279, 0..719); does what A would on that control
bool     uiExpert(void);              // expert mode: Read and write by default, no warnings, changes saved without asking
void     uiSetExpert(bool on);
bool     uiAccessRw(void);
UiTheme  uiTheme(void);              // the choice made in Advanced; the caller turns it into UiModel.dark
void     uiSetTheme(UiTheme t);
UiShare  uiShare(void);

// The mass-change prompt, asked from inside a commit. The next uiKeys() returns GUARD_APPLY or GUARD_REFUSE.
void     uiOpenGuard(unsigned files_deleted, unsigned dirs_deleted, unsigned files_rewritten);
bool     uiDialogOpen(void);

// Draws one frame. `now` is seconds on any steady clock.
void     uiDraw(Canvas* c, double now);
// True while something is moving, so a caller can pace its frame rate.
bool     uiAnimating(void);
// True while the marker is still travelling between stations (read from another thread, so quitting can let it arrive).
bool     uiMarkerTravelling(void);
