// Renders every Loopback screen to PPM files with the same graphics and UI code the console runs.
//   ui_render <font.ttf> <outdir>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/gfx.h"
#include "../source/ui.h"

static double g_t;
static Canvas g_c;

static void ppm(const char* dir, const char* name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    FILE* f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", g_c.w, g_c.h);
    for (int y = 0; y < g_c.h; y++)
        for (int x = 0; x < g_c.w; x++) {
            uint32_t v = g_c.px[(size_t)y * g_c.stride + x];
            unsigned char rgb[3] = { (unsigned char)(v & 255), (unsigned char)((v >> 8) & 255), (unsigned char)((v >> 16) & 255) };
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

static void run(double secs) {
    for (double t = 0; t < secs; t += 1.0 / 60.0) { g_t += 1.0 / 60.0; uiDraw(&g_c, g_t); }
}

static UiModel model(UiState s, bool dark) {
    UiModel m;
    memset(&m, 0, sizeof(m));
    m.version = "1.0.0"; m.state = s; m.dark = dark; m.battery = 84; m.charging = true; m.pct = 17; m.files = 273;
    m.link = "High-Speed";
    snprintf(m.read_text, sizeof(m.read_text), "1.2 GiB");
    snprintf(m.written_text, sizeof(m.written_text), "482 MiB");
    m.errors = 0;
    m.log[0] = "READ 273 FILES"; m.log[1] = "HIGH-SPEED LINK"; m.log[2] = "NEW FOLDER b3_eject"; m.log[3] = "SAVED 5 FILES";
    m.n_log = 4;
    m.pending = (s == UI_PENDING || s == UI_GONE);
    return m;
}

static void keys(bool up, bool down, bool left, bool right, bool a, bool b, bool x, bool y, bool plus) {
    UiKeys k = { up, down, left, right, a, b, x, y, plus, false };
    uiKeys(&k);
}

static void settle(UiState s, bool dark, const char* dir, const char* name) {
    uiInit();
    UiModel m = model(UI_IDLE, dark);
    uiSetModel(&m);
    run(0.2);
    m = model(s, dark);
    uiSetModel(&m);
    run(4.0);
    ppm(dir, name);
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s font.ttf outdir\n", argv[0]); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* ttf = malloc((size_t)n);
    if (fread(ttf, 1, (size_t)n, f) != (size_t)n) return 1;
    fclose(f);
    if (!gfxFontInit(ttf)) { fprintf(stderr, "font parse failed\n"); return 1; }
    g_c.w = UI_W; g_c.h = UI_H; g_c.stride = UI_W;
    g_c.px = calloc((size_t)UI_W * UI_H, 4);
    const char* dir = argv[2];
    static const char* names[] = { "idle", "reading", "waiting", "mounted", "pending", "saving", "gone" };
    for (int d = 0; d < 2; d++)
        for (int s = 0; s < 7; s++) {
            char nm[64];
            snprintf(nm, sizeof(nm), "%s_%s", names[s], d ? "dark" : "light");
            settle((UiState)s, d != 0, dir, nm);
        }
    // other random pairs of real lines
    // a spread of seeds covers all three palettes and several orders
    static const unsigned seeds[] = { 22, 25, 12, 15, 7, 5 };
    for (unsigned si = 0; si < sizeof(seeds) / sizeof(seeds[0]); si++)
        for (int d = 0; d < 2; d++) {
            char nm[64];
            uiInit(); uiRandomizeLines(seeds[si]);
            UiModel lm = model(UI_IDLE, d); uiSetModel(&lm); run(0.2);
            lm = model(UI_MOUNTED, d); uiSetModel(&lm); run(4.0);
            snprintf(nm, sizeof(nm), "lines_%u_%s", seeds[si], d ? "dark" : "light");
            ppm(dir, nm);
            lm = model(UI_GONE, d); lm.pending = false; uiSetModel(&lm); run(4.0);
            snprintf(nm, sizeof(nm), "lines_%u_gone_%s", seeds[si], d ? "dark" : "light");
            ppm(dir, nm);
        }
    // mid-flight: Ready to Mounted after 0.35 s, light
    uiInit();
    UiModel m = model(UI_IDLE, false);
    uiSetModel(&m); run(0.2);
    m = model(UI_MOUNTED, false); uiSetModel(&m);
    run(0.38); ppm(dir, "flight_a");
    run(0.30); ppm(dir, "flight_b");
    // advanced: each section, light and dark
    for (int d = 0; d < 2; d++)
        for (int sec = 0; sec < 6; sec++) {
            char nm[64];
            uiInit(); m = model(UI_IDLE, d); uiSetModel(&m); run(0.3);
            keys(0, 0, 0, 0, 0, 0, 0, 1, 0); run(0.1);
            for (int i = 0; i < sec; i++) keys(0, 1, 0, 0, 0, 0, 0, 0, 0);
            if (sec == 0 || sec == 2 || sec == 3) keys(0, 0, 0, 1, 0, 0, 0, 0, 0);
            run(0.2);
            snprintf(nm, sizeof(nm), "adv%d_%s", sec, d ? "dark" : "light");
            ppm(dir, nm);
        }
    // the Updates pane in each state: the pane open, light and dark
    {
        struct { const char* name; UiUpd u; int pct; const char* latest; const char* msg; UiState st; } sc[] = {
            { "idle", UI_UPD_IDLE, 0, "", "", UI_IDLE },
            { "checking", UI_UPD_CHECKING, 0, "", "Checking for a newer version...", UI_IDLE },
            { "current", UI_UPD_CURRENT, 0, "1.0.0", "You have the latest version.", UI_IDLE },
            { "available", UI_UPD_AVAILABLE, 0, "1.0.1", "Version 1.0.1 is available.", UI_IDLE },
            { "available_mounted", UI_UPD_AVAILABLE, 0, "1.0.1", "Version 1.0.1 is available.", UI_MOUNTED },
            { "downloading", UI_UPD_DOWNLOADING, 62, "1.0.1", "Downloading version 1.0.1...", UI_IDLE },
            { "ready", UI_UPD_READY, 100, "1.0.1", "Updated to version 1.0.1. Restart Loopback to use it.", UI_IDLE },
            { "failed", UI_UPD_FAILED, 0, "", "The download did not match its checksum. Nothing was changed.", UI_IDLE },
            { "private", UI_UPD_FAILED, 0, "", "No release found. The repository may be private.", UI_IDLE },
        };
        for (int d = 0; d < 2; d++)
            for (unsigned i = 0; i < sizeof(sc) / sizeof(sc[0]); i++) {
                char nm[64];
                uiInit(); m = model(UI_IDLE, d); uiSetModel(&m); run(0.3);
                keys(0, 0, 0, 0, 0, 0, 0, 1, 0);
                for (int k = 0; k < 3; k++) keys(0, 1, 0, 0, 0, 0, 0, 0, 0);
                keys(0, 0, 0, 1, 0, 0, 0, 0, 0);
                m = model(sc[i].st, d); m.upd = sc[i].u; m.upd_pct = sc[i].pct;
                snprintf(m.upd_latest, sizeof(m.upd_latest), "%s", sc[i].latest);
                snprintf(m.upd_msg, sizeof(m.upd_msg), "%s", sc[i].msg);
                uiSetModel(&m); run(0.5);
                snprintf(nm, sizeof(nm), "upd_%s_%s", sc[i].name, d ? "dark" : "light");
                ppm(dir, nm);
            }
        // pressing the button: each state gives the action it should
        uiInit(); m = model(UI_IDLE, 0); uiSetModel(&m); run(0.3);
        keys(0, 0, 0, 0, 0, 0, 0, 1, 0);
        for (int k = 0; k < 3; k++) keys(0, 1, 0, 0, 0, 0, 0, 0, 0);
        keys(0, 0, 0, 1, 0, 0, 0, 0, 0);
        struct { UiUpd u; UiState st; UiAction want; } act[] = {
            { UI_UPD_IDLE, UI_IDLE, UIA_UPD_CHECK }, { UI_UPD_FAILED, UI_IDLE, UIA_UPD_CHECK }, { UI_UPD_CHECKING, UI_IDLE, UIA_UPD_CANCEL },
            { UI_UPD_AVAILABLE, UI_IDLE, UIA_UPD_INSTALL }, { UI_UPD_AVAILABLE, UI_MOUNTED, UIA_NONE },
            { UI_UPD_DOWNLOADING, UI_IDLE, UIA_UPD_CANCEL }, { UI_UPD_READY, UI_IDLE, UIA_UPD_RESTART }, { UI_UPD_READY, UI_PENDING, UIA_NONE },
        };
        int bad = 0;
        for (unsigned i = 0; i < sizeof(act) / sizeof(act[0]); i++) {
            m = model(act[i].st, 0); m.upd = act[i].u; snprintf(m.upd_latest, sizeof(m.upd_latest), "1.0.1");
            uiSetModel(&m); run(0.1);
            UiKeys k = { 0, 0, 0, 0, 1, 0, 0, 0, 0, 0 };
            UiAction got = uiKeys(&k);
            if (got != act[i].want) { printf("updates button %u: got %d, wanted %d\n", i, (int)got, (int)act[i].want); bad = 1; }
        }
        printf(bad ? "updates button: FAILED\n" : "updates button: ok\n");
    }
    // expert mode switched on
    uiInit(); m = model(UI_IDLE, false); uiSetModel(&m); run(0.3);
    keys(0, 0, 0, 0, 0, 0, 0, 1, 0);
    keys(0, 1, 0, 0, 0, 0, 0, 0, 0); keys(0, 1, 0, 0, 0, 0, 0, 0, 0);
    keys(0, 0, 0, 1, 0, 0, 0, 0, 0); keys(0, 0, 0, 0, 1, 0, 0, 0, 0); run(0.2);
    ppm(dir, "adv_expert_on");
    // connecting (cable in, PC not mounted yet) and a pending save with the Save button
    {   // pressing X on a mounted card: the sign lights up line by line
        uiInit(); m = model(UI_MOUNTED, 0); m.io_busy = false; uiSetModel(&m); run(3.0);
        keys(0, 0, 0, 0, 0, 0, 1, 0, 0); run(0.17); ppm(dir, "press_a");
        run(0.25); ppm(dir, "press_b");
        run(0.45); ppm(dir, "press_c");
    }
    for (int d = 0; d < 2; d++) {
        const char* sfx = d ? "dark" : "light";
        char nm[64];
        uiInit(); m = model(UI_MOUNTED, d); uiSetModel(&m); run(3.0);
        m = model(UI_GONE, d); m.pending = false; uiSetModel(&m); run(3.0);
        snprintf(nm, sizeof(nm), "ejected_%s", sfx); ppm(dir, nm);
        uiInit(); m = model(UI_IDLE, d); uiSetModel(&m); run(0.2);
        m = model(UI_WAITING, d); m.write_mbps = 118.0f; m.read_mbps = 3.2f; uiSetModel(&m); run(3.0);
        snprintf(nm, sizeof(nm), "act_wide_%s", sfx); ppm(dir, nm);
    }
    for (int d = 0; d < 2; d++) {
        const char* sfx = d ? "dark" : "light";
        char nm[64];
        uiInit(); m = model(UI_MOUNTED, d); m.write_mbps = 18.4f; m.read_mbps = 3.2f; m.io_busy = true; uiSetModel(&m); run(3.0);
        snprintf(nm, sizeof(nm), "act_busy_%s", sfx); ppm(dir, nm);
        uiInit(); m = model(UI_MOUNTED, d); uiSetModel(&m); run(3.0);
        snprintf(nm, sizeof(nm), "act_safe_%s", sfx); ppm(dir, nm);
        uiInit(); m = model(UI_PENDING, d); m.write_mbps = 120.0f; m.io_busy = true; uiSetModel(&m); run(3.0);
        snprintf(nm, sizeof(nm), "act_pending_%s", sfx); ppm(dir, nm);
    }
    uiInit(); m = model(UI_WAITING, false); m.connecting = true; uiSetModel(&m); run(3.0); ppm(dir, "connecting");
    uiInit(); m = model(UI_IDLE, false); uiSetModel(&m); run(0.2);
    m = model(UI_PENDING, false); uiSetModel(&m); run(3.0); keys(0, 0, 0, 1, 0, 0, 0, 0, 0); run(0.2); ppm(dir, "pending_buttons");
    // dialogs: allow writes (read+write on whole card, then Mount), guard, quit
    for (int d = 0; d < 2; d++) {
        const char* sfx = d ? "dark" : "light";
        char nm[64];
        uiInit(); m = model(UI_IDLE, d); uiSetModel(&m); run(0.3);
        keys(0, 0, 0, 1, 0, 0, 0, 0, 0);   // focus: the access pill
        keys(0, 0, 0, 0, 1, 0, 0, 0, 0);   // A flips it to Read and write
        keys(0, 0, 0, 0, 0, 0, 1, 0, 0);   // X: mount, which asks first
        run(0.6); snprintf(nm, sizeof(nm), "dialog_warn_%s", sfx); ppm(dir, nm);
        uiInit(); m = model(UI_SAVING, d); uiSetModel(&m); run(1.5);
        uiOpenGuard(60, 0, 0); run(0.6); snprintf(nm, sizeof(nm), "dialog_guard_%s", sfx); ppm(dir, nm);
        uiInit(); m = model(UI_PENDING, d); uiSetModel(&m); run(2.5);
        keys(0, 0, 0, 0, 0, 0, 0, 0, 1); run(0.6); snprintf(nm, sizeof(nm), "dialog_quit_%s", sfx); ppm(dir, nm);
    }
    printf("rendered\n");
    return 0;
}
