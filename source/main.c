// Loopback: shares the Switch's SD card with a PC as a USB drive, without rebooting into a payload.
//
// Two threads. The UI thread owns the screen, the controller and all of ui.c, and runs at a steady frame rate. The main thread
// owns the USB bus, the card scan and the commits, which can stall for tens of milliseconds, so they must never share a thread
// with drawing. They talk through a few single-writer flags (the "shared" block below).
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <switch.h>

#include "backend.h"
#include "exfat_synth.h"
#include "gfx.h"
#include "log.h"
#include "ui.h"
#include "update.h"
#include "usb_msc.h"

#ifndef APP_VERSION_STR
#define APP_VERSION_STR "dev"  // the Makefile passes APP_VERSION
#endif
#define DEFAULT_NRO     "sdmc:/switch/loopback.nro"

#define ROOT_WHOLE_CARD "sdmc:"
#define ROOT_TEST_DIR   "sdmc:/nx-test"
#define APP_DIR         "sdmc:/switch/loopback"  // log and overlay live here; never shown to the PC
#define OVERLAY_PATH    APP_DIR "/overlay.bin"
#define SETTINGS_PATH   APP_DIR "/settings.txt"
#define OLD_THEME_PATH  APP_DIR "/theme.txt"
#define STAGE_PREFIX    ".nxusb-stage"

// ---------------------------------------------------------------------------------------------
// Shared between the threads. The main thread writes the state, progress and note; the UI thread writes the action, the
// cancel request and the guard answer. Each is a plain int or flag, read and written with the atomic builtins.

static volatile int      g_s_state = UI_IDLE;  // what the main thread says the app is doing (UiState)
static volatile int      g_s_pending;          // the PC wrote data that is not on the card yet
static volatile int      g_s_connecting;       // the cable is in and the PC is setting the drive up, but has not mounted it
static volatile int      g_pct;                // saving progress
static volatile unsigned g_files_seen;         // files found so far while reading
static volatile int      g_act;                // an action the UI thread wants the main thread to carry out (UiAction)
static volatile int      g_cancel;             // the UI thread asks the running save to stop
static volatile int      g_exit;               // the main thread asks the UI thread to finish
static volatile int      g_guard_req;          // 1: main asks for the mass-change prompt, 2: it is on screen
static volatile int      g_guard_res;          // 1: apply, 2: refuse
static unsigned          g_guard_del, g_guard_dirs, g_guard_rw;
static char              g_note[160];          // a line of caption text for Ready and Mounted
static volatile int      g_restart;            // quit into the freshly installed version instead of the Homebrew Menu
static char              g_nro_path[256];      // the NRO this run was started from, which an update replaces

_Static_assert((int)UPD_IDLE == (int)UI_UPD_IDLE && (int)UPD_CHECKING == (int)UI_UPD_CHECKING && (int)UPD_CURRENT == (int)UI_UPD_CURRENT &&
               (int)UPD_AVAILABLE == (int)UI_UPD_AVAILABLE && (int)UPD_DOWNLOADING == (int)UI_UPD_DOWNLOADING &&
               (int)UPD_READY == (int)UI_UPD_READY && (int)UPD_FAILED == (int)UI_UPD_FAILED, "UiUpd and UpdState must list the same states");

static int  ld(volatile int* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void st(volatile int* p, int v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }

static double nowSec(void) { return (double)armTicksToNs(armGetSystemTick()) / 1e9; }

// ---------------------------------------------------------------------------------------------
// UI thread: model, frame, input

static Framebuffer g_fb;
static bool        g_fb_ok;
static PadState    g_pad;
static int         g_battery = -1;
static bool        g_charging, g_dark;
static u64         g_sys_tick;
static char        g_log_lines[8][40];

static const char* linkName(UsbDeviceSpeed s) {
    switch (s) {
    case UsbDeviceSpeed_Low:   return "Low-Speed";
    case UsbDeviceSpeed_Full:  return "Full-Speed";
    case UsbDeviceSpeed_High:  return "High-Speed";
    case UsbDeviceSpeed_Super: return "SuperSpeed";
    default:                   return "Not connected";
    }
}

static void sizeText(char* out, size_t cap, u64 bytes) {
    if (bytes >= (1ull << 30)) snprintf(out, cap, "%.1f GiB", bytes / 1073741824.0);
    else snprintf(out, cap, "%.0f MiB", bytes / 1048576.0);
}

// Battery and light/dark setting change slowly; look at them every few seconds.
static void pollSystem(void) {
    u64 now = armGetSystemTick();
    if (g_sys_tick && armTicksToNs(now - g_sys_tick) < 3000000000ULL) return;
    g_sys_tick = now;
    u32 pct = 0;
    PsmChargerType ch = PsmChargerType_Unconnected;
    if (R_SUCCEEDED(psmGetBatteryChargePercentage(&pct))) g_battery = (int)pct;
    if (R_SUCCEEDED(psmGetChargerType(&ch))) g_charging = ch != PsmChargerType_Unconnected;
    ColorSetId cs;
    if (R_SUCCEEDED(setsysGetColorSetId(&cs))) g_dark = cs == ColorSetId_Dark;
}

// Transfer rates for the activity board: bytes moved since the last sample, averaged over at least half a second and smoothed.
static void sampleRates(UiModel* m) {
    static u64 prev_r, prev_w, prev_t;
    static float r_rate, w_rate;
    const MscStats* s = mscStats();
    u64 now = armGetSystemTick();
    if (!prev_t) { prev_t = now; prev_r = s->bytes_read; prev_w = s->bytes_written; }
    double dt = armTicksToNs(now - prev_t) / 1e9;
    if (dt >= 0.5) {
        double dr = s->bytes_read >= prev_r ? (double)(s->bytes_read - prev_r) : 0.0;  // the counters restart on each mount
        double dw = s->bytes_written >= prev_w ? (double)(s->bytes_written - prev_w) : 0.0;
        r_rate = r_rate * 0.4f + (float)(dr / dt / 1048576.0) * 0.6f;
        w_rate = w_rate * 0.4f + (float)(dw / dt / 1048576.0) * 0.6f;
        if (r_rate < 0.05f && dr == 0.0) r_rate = 0;
        if (w_rate < 0.05f && dw == 0.0) w_rate = 0;
        prev_t = now; prev_r = s->bytes_read; prev_w = s->bytes_written;
    }
    m->read_mbps = r_rate;
    m->write_mbps = w_rate;
    m->io_busy = s->last_write_tick && armTicksToNs(now - s->last_write_tick) < 2000000000ULL;
}

static void fillModel(UiModel* m) {
    memset(m, 0, sizeof(*m));
    const MscStats* s = mscStats();
    m->state = (UiState)ld(&g_s_state);
    UiTheme th = uiTheme();
    m->dark = th == UI_THEME_DARK || (th == UI_THEME_AUTO && g_dark);
    m->pending = ld(&g_s_pending) != 0;
    sampleRates(m);
    m->connecting = ld(&g_s_connecting) != 0;
    m->pct = ld(&g_pct);
    m->files = g_files_seen;
    m->battery = g_battery;
    m->charging = g_charging;
    m->note = g_note[0] ? g_note : NULL;
    m->version = APP_VERSION_STR;
    UpdStatus us;
    updGet(&us);
    m->upd = (UiUpd)us.state;
    m->upd_pct = us.pct;
    snprintf(m->upd_latest, sizeof(m->upd_latest), "%s", us.latest);
    snprintf(m->upd_msg, sizeof(m->upd_msg), "%s", us.msg);
    m->link = linkName(s->speed);
    sizeText(m->read_text, sizeof(m->read_text), s->bytes_read);
    sizeText(m->written_text, sizeof(m->written_text), s->bytes_written);
    m->errors = s->errors;
    // the last few log lines that mean something to a person
    char all[LOG_RING_LINES][LOG_LINE_LEN];
    int n = 0;
    for (int i = 0; i < LOG_RING_LINES; i++) {
        char l[LOG_LINE_LEN];
        lgCopyLine(i, l, sizeof(l));
        if (!l[0] || !strncmp(l, "op ", 3) || !strncmp(l, "setup ", 6) || !strncmp(l, "CSW", 3) || !strncmp(l, "ui:", 3)) continue;
        memcpy(all[n++], l, sizeof(l));
    }
    int first = n > 8 ? n - 8 : 0;
    for (int i = first; i < n; i++) {
        char* d = g_log_lines[i - first];
        size_t k = 0;
        for (; all[i][k] && k < sizeof(g_log_lines[0]) - 1; k++) d[k] = (all[i][k] >= 'a' && all[i][k] <= 'z') ? (char)(all[i][k] - 32) : all[i][k];
        d[k] = 0;
        m->log[m->n_log++] = d;
    }
}

static UiKeys readKeys(void) {
    padUpdate(&g_pad);
    u64 d = padGetButtonsDown(&g_pad);
    UiKeys k;
    k.up = d & HidNpadButton_AnyUp;
    k.down = d & HidNpadButton_AnyDown;
    k.left = d & HidNpadButton_AnyLeft;
    k.right = d & HidNpadButton_AnyRight;
    k.a = d & HidNpadButton_A;
    k.b = d & HidNpadButton_B;
    k.x = d & HidNpadButton_X;
    k.y = d & HidNpadButton_Y;
    k.plus = d & HidNpadButton_Plus;
    k.r = d & HidNpadButton_R;
    return k;
}

// Settings chosen in Advanced are kept in a small file next to the log: "theme=auto|light|dark" and "expert=0|1".
static void loadSettings(void) {
    FILE* f = fopen(SETTINGS_PATH, "r");
    if (f) {
        char line[64];
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "theme=", 6)) uiSetTheme(!strncmp(line + 6, "dark", 4) ? UI_THEME_DARK : !strncmp(line + 6, "light", 5) ? UI_THEME_LIGHT : UI_THEME_AUTO);
            else if (!strncmp(line, "expert=", 7)) uiSetExpert(line[7] == '1');
        }
        fclose(f);
        return;
    }
    f = fopen(OLD_THEME_PATH, "r");  // the first version kept only the theme
    if (!f) return;
    char w[16] = "";
    if (fscanf(f, "%15s", w) == 1) uiSetTheme(!strcmp(w, "dark") ? UI_THEME_DARK : !strcmp(w, "light") ? UI_THEME_LIGHT : UI_THEME_AUTO);
    fclose(f);
}

static void saveSettings(UiTheme t, bool expert) {
    FILE* f = fopen(SETTINGS_PATH, "w");
    if (!f) return;
    fprintf(f, "theme=%s\nexpert=%d\n", t == UI_THEME_DARK ? "dark" : t == UI_THEME_LIGHT ? "light" : "auto", expert ? 1 : 0);
    fclose(f);
}

// What an action from the UI means for the UI thread itself; the rest go to the main thread.
static void routeAction(UiAction a) {
    if (a == UIA_CANCEL) st(&g_cancel, 1);
    else if (a == UIA_GUARD_APPLY) st(&g_guard_res, 1);
    else if (a == UIA_GUARD_REFUSE) st(&g_guard_res, 2);
    else if (a == UIA_UPD_CHECK) updCheckAsync();
    else if (a == UIA_UPD_CANCEL) updCancel();
    else if (a == UIA_UPD_INSTALL) {
        // the app file is replaced, so nothing may be mounted (the screen only offers this when the card is at rest; this is the backstop)
        int s = ld(&g_s_state);
        if (s == UI_IDLE || (s == UI_GONE && !ld(&g_s_pending))) updInstallAsync();
    }
    else if (a != UIA_NONE) {
        if (a == UIA_MOUNT) uiRandomizeLines((unsigned)armGetSystemTick());  // a fresh pair of real L lines for each mount
        st(&g_act, (int)a);
    }
}

#define FADE_SEC 0.4   // fade in from black at launch
#define FADE_RATE 6.0  // how sharply it front-loads: higher = steeper start

static void uiThread(void* arg) {
    (void)arg;
    UiTheme saved_theme = uiTheme();
    bool saved_expert = uiExpert();
    bool touching = false;
    u64 stat_t0 = armGetSystemTick();
    u64 fade_t0 = 0;  // set once the first (black) frame is presented
    bool fade_on = false, fade_done = false;
    unsigned frames = 0;
    double draw_ms = 0, present_ms = 0, draw_max = 0;
    while (!ld(&g_exit)) {
        u64 frame_start = armGetSystemTick();

        UiKeys k = readKeys();
        routeAction(uiKeys(&k));
        if (k.b && ld(&g_s_state) == UI_SAVING) st(&g_cancel, 1);
        // touch: a tap acts when the finger goes down
        HidTouchScreenState ts;
        if (hidGetTouchScreenStates(&ts, 1) && ts.count > 0) {
            if (!touching) { touching = true; routeAction(uiTouch((int)ts.touches[0].x, (int)ts.touches[0].y)); }
        } else {
            touching = false;
        }
        if (ld(&g_guard_req) == 1 && !uiDialogOpen()) {
            uiOpenGuard(g_guard_del, g_guard_dirs, g_guard_rw);
            st(&g_guard_req, 2);
        }
        if (uiTheme() != saved_theme || uiExpert() != saved_expert) {
            saved_theme = uiTheme(); saved_expert = uiExpert();
            saveSettings(saved_theme, saved_expert);
        }

        pollSystem();
        UiModel m;
        fillModel(&m);
        uiSetModel(&m);

        if (g_fb_ok) {
            u64 t0 = armGetSystemTick();
            u32 stride = 0;
            u32* buf = framebufferBegin(&g_fb, &stride);
            Canvas c = { buf, UI_W, UI_H, (int)(stride / 4) };
            u64 t1 = armGetSystemTick();
            uiDraw(&c, nowSec());
            // The startup logo is off in the forwarder, so the app fades in from black itself. The first frame is black and the clock
            // starts once it is on screen, so building the cached backgrounds does not eat into the fade.
            if (!fade_on) {
                memset(buf, 0, (size_t)c.stride * UI_H * 4);
            } else if (fade_t0) {
                double ft = armTicksToNs(armGetSystemTick() - fade_t0) / 1e9;
                if (ft >= FADE_SEC) fade_done = true;
                else {
                    // reverse exponential: most of the brightness arrives in the first moments, then it settles in
                    double u = (1.0 - exp(-FADE_RATE * ft / FADE_SEC)) / (1.0 - exp(-FADE_RATE));
                    unsigned k = (unsigned)(u * 256.0);
                    for (int y = 0; y < UI_H; y++) {
                        u32* row = buf + (size_t)y * c.stride;
                        for (int x = 0; x < UI_W; x++) {
                            u32 p = row[x];
                            u32 rb = ((p & 0x00FF00FFu) * k >> 8) & 0x00FF00FFu;
                            u32 ga = (((p >> 8) & 0x00FF00FFu) * k) & 0xFF00FF00u;
                            row[x] = rb | ga;
                        }
                    }
                }
            }
            u64 t2 = armGetSystemTick();
            framebufferEnd(&g_fb);
            if (!fade_on) { fade_on = true; fade_t0 = armGetSystemTick(); }
            u64 t3 = armGetSystemTick();
            double dms = armTicksToNs(t2 - t1) / 1e6;
            draw_ms += dms;
            if (dms > draw_max) draw_max = dms;
            present_ms += armTicksToNs((t1 - t0) + (t3 - t2)) / 1e6;
        }
        frames++;
        double span = armTicksToNs(armGetSystemTick() - stat_t0) / 1e9;
        if (span >= 10.0) {
            lg("ui: %.1f fps, draw avg %.1f max %.1f ms, wait+present avg %.1f ms", frames / span, draw_ms / frames, draw_max, present_ms / frames);
            stat_t0 = armGetSystemTick();
            frames = 0; draw_ms = present_ms = draw_max = 0;
        }

        // hold a steady rate: 30 fps while something moves, a gentler 20 otherwise
        u64 target_ns = (fade_on && !fade_done) ? 0ULL : uiAnimating() ? 33000000ULL : 50000000ULL;  // the fade runs at the display's pace
        u64 spent = armTicksToNs(armGetSystemTick() - frame_start);
        if (spent < target_ns) svcSleepThread((s64)(target_ns - spent));
    }
}

// ---------------------------------------------------------------------------------------------
// Main thread: write mode, mounting, USB

#define IDLE_COMMIT_NS 5000000000ULL  // no writes for this long
#define EXPERT_COMMIT_NS 2000000000ULL  // expert mode saves as soon as a copy looks finished
#define SYNC_QUIET_NS   500000000ULL  // after a flush from the PC, once writes have stopped this long

typedef struct {
    u64  baseline;      // overlay bytes that need no commit (housekeeping the last commit found nothing in)
    bool refused;       // the last commit was refused; do not retry until the PC writes more
    u64  refused_at;
    u32  seen_sync, seen_eject;
    bool fatal;         // the volume could not be rebuilt after a commit
} WriteState;

static void commitLogLine(const char* line, void* user) {
    (void)user;
    lg("%s", line);
}

// The mass-change guard: the PC deleted or overwrote a lot. The UI thread shows the prompt and reports the answer.
static bool confirmCommit(const CommitReport* r, void* user) {
    (void)user;
    if (uiExpert()) { lg("expert mode: applying %u deletions/overwrites without asking", r->files_deleted + r->dirs_deleted + r->files_rewritten); return true; }
    g_guard_del = r->files_deleted; g_guard_dirs = r->dirs_deleted; g_guard_rw = r->files_rewritten;
    st(&g_guard_res, 0);
    st(&g_guard_req, 1);
    while (appletMainLoop()) {
        int res = ld(&g_guard_res);
        if (res) { st(&g_guard_req, 0); st(&g_guard_res, 0); return res == 1; }
        mscPoll(16ULL * 1000000ULL);  // the commit is busy: the PC gets NOT READY while we wait
    }
    return false;
}

static void commitPump(void* user) {
    (void)user;
    mscPoll(1000000ULL);
}

// While new file data is copied to the card (minutes for a large file) the UI thread shows progress and B cancels;
// nothing on the card has been touched at that point.
static bool commitProgress(u64 done, u64 total, void* user) {
    (void)user;
    st(&g_pct, total ? (int)(done * 100 / total) : 0);
    return !ld(&g_cancel);
}

static void runCommit(Backend* be, WriteState* w, const char* why) {
    mscLogWriteRun();
    bool stored = mscFlush();  // every write the PC was told is done must be in the overlay before it is committed
    lg("commit (%s)%s", why, stored ? "" : " (a write failed earlier)");
    {
        const MscStats* ms = mscStats();
        lg("writes so far: %u cmds, max %u KiB, %.0f MiB, usb out %.1f s, backend write %.1f s",
           ms->write_cmds, ms->max_write_bytes / 1024, ms->bytes_written / 1048576.0, ms->out_ns / 1e9, ms->write_ns / 1e9);
    }
    st(&g_cancel, 0);
    st(&g_pct, 0);
    st(&g_s_state, UI_SAVING);
    st(&g_s_pending, 1);

    CommitOpts opts = { .confirm = confirmCommit, .log = commitLogLine, .progress = commitProgress, .pump = commitPump };
    CommitReport r;
    u64 t0 = armGetSystemTick();
    mscSetBusy(true);  // the PC is answered NOT READY until the volume has been rebuilt
    bool ok = synthCommit(be, &opts, &r);
    mscSetBusy(false);
    double secs = (double)armTicksToNs(armGetSystemTick() - t0) / 1e9;

    if (r.applied) {
        lg("committed in %.1f s: +%u dirs +%u files, %u rewritten, %u moved, -%u files -%u dirs, %u errors", secs,
           r.dirs_created, r.files_created, r.files_rewritten, r.moved, r.files_deleted, r.dirs_deleted, r.errors);
        if (r.err[0]) { lg("FATAL: %s", r.err); w->fatal = true; }
    } else if (!ok) {
        lg("commit refused: %s", r.err);
        w->refused = true;
        w->refused_at = synthPendingBytes(be);
    } else {
        lg("nothing to commit (%u ignored)", r.ignored);
    }
    // A refused commit leaves the PC's changes pending: keep them counted, so Save and the quit dialog still see them.
    if (ok) w->baseline = synthPendingBytes(be);
}

// Commit on its own when the PC is done writing. With the PC gone nothing finishes a half-done copy, so no automatic
// commit starts then, except after an eject (Safely Remove).
static void writeTriggers(Backend* be, WriteState* w) {
    const MscStats* s = mscStats();
    u64 pending = synthPendingBytes(be);
    if (pending == w->baseline) {
        w->seen_sync = s->sync_cmds;
        w->seen_eject = s->eject_cmds;
        return;
    }
    if (w->refused && pending == w->refused_at) return;  // nothing new since the refusal
    w->refused = false;

    u64 idle = armTicksToNs(armGetSystemTick() - s->last_write_tick);
    const char* why = NULL;
    bool attached = s->state == UsbState_Configured;
    if (s->eject_cmds != w->seen_eject)                                          why = "eject";
    else if (attached && s->sync_cmds != w->seen_sync && idle > SYNC_QUIET_NS)  why = "flush";
    else if (attached && idle > (uiExpert() ? EXPERT_COMMIT_NS : IDLE_COMMIT_NS)) why = "idle";
    if (!why) return;
    runCommit(be, w, why);
    w->seen_sync = s->sync_cmds;
    w->seen_eject = s->eject_cmds;
}

static bool stageLeftovers(const char* root) {
    DIR* d = opendir(root);
    if (!d) return false;
    bool found = false;
    struct dirent* de;
    while (!found && (de = readdir(d))) found = strncmp(de->d_name, STAGE_PREFIX, strlen(STAGE_PREFIX)) == 0;
    closedir(d);
    return found;
}

static u64 sdFreeBytes(void) {
    FsFileSystem* fs = fsdevGetDeviceFileSystem("sdmc");
    s64 free_space = 0;
    if (fs && R_SUCCEEDED(fsFsGetFreeSpace(fs, "/", &free_space)) && free_space > 0) return (u64)free_space;
    return 0;
}

static void scanProgress(const SynthStats* s, void* user) {
    (void)user;
    g_files_seen = s->files;
}

static void logSkip(const char* path, const char* why) { lg("skip %s: %s", why, path); }

// Reads the card (or builds the RAM disk) and starts the USB drive. Returns the backend, or NULL with g_note set.
static Backend* startMount(UiShare share, bool rw, bool* write_out, Result* init_rc) {
    g_note[0] = 0;
    g_files_seen = 0;
    st(&g_s_state, UI_READING);
    *write_out = false;
    Backend* be = NULL;
    char err[128] = "";
    if (share == UI_SHARE_RAM) {
        be = ramBackendCreate();
        if (!be) snprintf(err, sizeof(err), "out of memory");
    } else {
        const char* root = ROOT_WHOLE_CARD;
        if (share == UI_SHARE_TEST) {
            if (mkdir(ROOT_TEST_DIR, 0777) != 0 && errno != EEXIST) lg("mkdir " ROOT_TEST_DIR " failed: errno %d", errno);
            root = ROOT_TEST_DIR;
        }
        lg("scan start: %s", root);
        synthSetSkipLog(logSkip);
        synthSetHiddenPath(APP_DIR);
        SynthOptions so = { .root = root, .progress = scanProgress };
        if (rw) {
            so.overlay_path = OVERLAY_PATH;
            so.free_bytes = sdFreeBytes();
            lg("write mode: %.1f GiB free on the card", so.free_bytes / 1073741824.0);
            if (stageLeftovers(root)) {
                lg("WARNING: " STAGE_PREFIX "* in %s holds files from an interrupted commit", root);
                snprintf(g_note, sizeof(g_note), "Files from a stopped save are in %s on the card.", STAGE_PREFIX);
            }
        }
        be = synthBackendCreateEx(&so, err, sizeof(err));
        if (be) {
            const SynthStats* ss = synthBackendStats(be);
            lg("scan done: %u dirs, %u files, %u skipped", ss->dirs, ss->files, ss->skipped);
            *write_out = rw;
        }
    }
    if (!be) {
        lg("mount failed: %s", err);
        snprintf(g_note, sizeof(g_note), "Could not read the SD card: %s", err);
        st(&g_s_state, UI_IDLE);
        return NULL;
    }
    if (share != UI_SHARE_RAM) {
        char lay[200];
        synthLayoutText(be, lay, sizeof(lay));
        lg("%s", lay);
    }
    *init_rc = mscInit(be);
    if (R_FAILED(*init_rc)) {
        lg("usb init failed: %08x", *init_rc);
        snprintf(g_note, sizeof(g_note), "The USB connection could not start (%08x).", *init_rc);
    }
    return be;
}

// ---------------------------------------------------------------------------------------------

typedef struct {
    Backend* be;
    UiShare  share;
    bool     write, running, sleep_off, playback_hint;
    bool     ejected;  // stopped on purpose with the cable (or the PC) still there: the screen says Unplugged, not Ready
    WriteState ws;
} App;

// Stops sharing the card and goes back to Ready: the USB drive disappears from the PC and everything is released, so Mount
// can be pressed again.
static void stopMount(App* app) {
    if (!app->running) return;
    lg("eject");
    mscExit();
    if (app->share == UI_SHARE_RAM) ramBackendDestroy(app->be);
    else synthBackendDestroy(app->be);
    app->be = NULL;
    app->running = false;
    app->write = false;
    memset(&app->ws, 0, sizeof(app->ws));
    if (app->sleep_off) { appletSetAutoSleepDisabled(false); app->sleep_off = false; }
    if (app->playback_hint) { appletSetMediaPlaybackState(false); app->playback_hint = false; }
    st(&g_s_state, app->ejected ? UI_GONE : UI_IDLE);
    st(&g_s_pending, 0);
    st(&g_s_connecting, 0);
}

// Which file an update replaces: the NRO this run was started from, if the loader says so and it is there, else the usual place.
static void resolveNro(int argc, char** argv) {
    snprintf(g_nro_path, sizeof(g_nro_path), "%s", DEFAULT_NRO);
    if (argc < 1 || !argv || !argv[0]) return;
    char p[256];
    if (!strncmp(argv[0], "sdmc:/", 6)) snprintf(p, sizeof(p), "%s", argv[0]);
    else if (argv[0][0] == '/') snprintf(p, sizeof(p), "sdmc:%s", argv[0]);
    else return;
    size_t n = strlen(p);
    struct stat sb;
    if (n > 10 && n < sizeof(p) - 1 && !strcasecmp(p + n - 4, ".nro") && stat(p, &sb) == 0) snprintf(g_nro_path, sizeof(g_nro_path), "%s", p);
}

int main(int argc, char** argv) {
    lgInit();
    resolveNro(argc, argv);
    updInit(APP_VERSION_STR, g_nro_path, APP_DIR);
    lg("loopback %s, app file %s", APP_VERSION_STR, g_nro_path);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    hidInitializeTouchScreen();

    psmInitialize();
    setsysInitialize();
    plInitialize(PlServiceType_User);
    PlFontData font;
    bool font_ok = R_SUCCEEDED(plGetSharedFontByType(&font, PlSharedFontType_Standard)) && gfxFontInit(font.address);
    if (!font_ok) lg("system font unavailable");
    NWindow* win = nwindowGetDefault();
    g_fb_ok = R_SUCCEEDED(framebufferCreate(&g_fb, win, UI_W, UI_H, PIXEL_FORMAT_RGBA_8888, 2));
    if (g_fb_ok) framebufferMakeLinear(&g_fb);
    uiInit();
    uiRandomizeLines((unsigned)armGetSystemTick());
    loadSettings();

    Thread ui_thread;
    bool ui_started = false;
    if (R_SUCCEEDED(threadCreate(&ui_thread, uiThread, NULL, NULL, 0x40000, 0x2C, 2))) ui_started = R_SUCCEEDED(threadStart(&ui_thread));
    if (!ui_started) lg("could not start the UI thread");

    App app;
    memset(&app, 0, sizeof(app));
    Result init_rc = 0;
    bool host_mounted = false;  // latched once the PC has mounted the drive, until the cable comes out

    // Expert mode mounts as soon as the app opens (Read and write, whole card, as that mode defaults to).
    if (uiExpert()) __atomic_store_n(&g_act, UIA_MOUNT, __ATOMIC_RELEASE);

    while (appletMainLoop()) {
        int act = __atomic_exchange_n(&g_act, UIA_NONE, __ATOMIC_ACQ_REL);
        bool want_quit = false;
        switch ((UiAction)act) {
        case UIA_UPD_RESTART:
            if (!app.running) { g_restart = 1; want_quit = true; }
            break;
        case UIA_MOUNT:
            if (!app.running) {
                UpdStatus us;
                updGet(&us);
                if (us.state == UPD_DOWNLOADING) break;  // the app file is being replaced; the card is scanned once that is done
                app.ejected = false;
                app.share = uiShare();
                app.be = startMount(app.share, uiAccessRw(), &app.write, &init_rc);
                if (app.be) {
                    app.running = true;
                    memset(&app.ws, 0, sizeof(app.ws));
                    host_mounted = false;
                    // The Switch dropping into sleep mid-transfer yanks the drive from the PC.
                    Result sr = appletSetAutoSleepDisabled(true);
                    app.sleep_off = R_SUCCEEDED(sr);
                    if (!app.sleep_off) {
                        app.playback_hint = R_SUCCEEDED(appletSetMediaPlaybackState(true));
                        lg("auto-sleep disable failed %08x, playback hint %s", sr, app.playback_hint ? "on" : "failed");
                    } else lg("auto-sleep disabled");
                }
            }
            break;
        case UIA_SAVE:
            if (app.running && app.write) runCommit(app.be, &app.ws, "button");
            break;
        case UIA_EJECT_SAVE:
            if (app.running && app.write && synthPendingBytes(app.be) != app.ws.baseline) runCommit(app.be, &app.ws, "eject");
            app.ejected = ld(&g_s_state) != UI_WAITING || ld(&g_s_connecting);
            stopMount(&app);
            break;
        case UIA_EJECT:
        case UIA_EJECT_DISCARD:
            // Cancelling before any cable is plugged in goes back to Ready; stopping a drive the PC has goes to Unplugged.
            app.ejected = ld(&g_s_state) != UI_WAITING || ld(&g_s_connecting);
            stopMount(&app);
            break;
        case UIA_QUIT_SAVE:
            if (app.running && app.write) runCommit(app.be, &app.ws, "quit");
            want_quit = true;
            break;
        case UIA_QUIT:
        case UIA_QUIT_DISCARD:
            want_quit = true;
            break;
        default: break;
        }
        if (want_quit) break;

        if (app.running) {
            mscPoll(20ULL * 1000000ULL);
            if (app.write) {
                writeTriggers(app.be, &app.ws);
                if (app.ws.fatal) break;
            }
        } else {
            svcSleepThread(10ULL * 1000000ULL);
        }

        // what the UI should show now
        bool pend = app.running && app.write && synthPendingBytes(app.be) != app.ws.baseline;
        UiState s = app.ejected ? UI_GONE : UI_IDLE;
        bool connecting = false;
        if (app.running) {
            const MscStats* ms = mscStats();
            bool attached = ms->state == UsbState_Configured;
            if (!attached) host_mounted = false;
            else if (ms->host_locked || (ms->read_cmds - ms->reads_at_config) >= 40) host_mounted = true;
            if (!attached) s = pend ? UI_GONE : UI_WAITING;
            else if (!host_mounted && !pend) { s = UI_WAITING; connecting = true; }  // cable in, the PC is still setting the drive up
            else s = pend ? UI_PENDING : UI_MOUNTED;
        }
        st(&g_s_state, (int)s);
        st(&g_s_pending, pend ? 1 : 0);
        st(&g_s_connecting, connecting ? 1 : 0);
    }

    {
        const MscStats* ms = mscStats();
        lg("totals: read %.0f MiB, write %.0f MiB, backend read %.1f s, usb wait %.1f s",
           ms->bytes_read / 1048576.0, ms->bytes_written / 1048576.0, ms->read_ns / 1e9, ms->wait_ns / 1e9);
        lg("writes: %u cmds, max %u KiB, usb out %.1f s, backend write %.1f s",
           ms->write_cmds, ms->max_write_bytes / 1024, ms->out_ns / 1e9, ms->write_ns / 1e9);
    }

    // Quitting a mounted drive: take it away first and let the marker reach Unplugged, so the screen shows it before closing.
    if (app.running) {
        app.ejected = true;
        stopMount(&app);
        if (ui_started) {
            svcSleepThread(120ULL * 1000000ULL);  // the UI thread picks up the new state and starts the marker
            for (int i = 0; i < 100 && uiMarkerTravelling(); i++) svcSleepThread(20ULL * 1000000ULL);  // at most 2 s
            svcSleepThread(300ULL * 1000000ULL);  // a beat on Unplugged
        }
    }

    st(&g_exit, 1);
    if (ui_started) { threadWaitForExit(&ui_thread); threadClose(&ui_thread); }
    stopMount(&app);  // the PC sees the drive go away, nothing stays locked
    updShutdown();    // a download in progress is cancelled and its partial file removed at the next start
    (void)init_rc;
    // Quit goes back to the Homebrew Menu instead of leaving to Home (needed when launched from the forwarder). After an update it
    // starts Loopback again instead, now the new version.
    if (envHasNextLoad()) {
        const char* next = g_restart ? g_nro_path : "sdmc:/hbmenu.nro";
        char nargv[300];
        snprintf(nargv, sizeof(nargv), "\"%s\"", next);
        Result nr = envSetNextLoad(next, nargv);
        lg("next load %s: %08x", next, nr);
    } else if (g_restart) lg("restart asked for, but there is no loader to start it");
    if (g_fb_ok) framebufferClose(&g_fb);
    plExit();
    setsysExit();
    psmExit();
    lgExit();
    return 0;
}
