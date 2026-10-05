#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_FIT 160

// ---------------------------------------------------------------------------------------------
// Palette. Line colours carry meaning: blue = working, green = mounted, orange = needs a look, red = destructive.

typedef struct {
    Col bg, ink, ink2, card, mute, stf, sdim;
    Col blue, green, orange, red, yellow, brown, purple, pink;
    Col black, amber, amber_dim, board_txt, scrim, out_stroke, faint, panel, shadowA, shadowB, chip;
} Pal;

static Pal PL, PD;
static const Pal* P;

static void initPalettes(void) {
    PL = (Pal){ COL_HEX(0xefefec), COL_HEX(0x111111), COL_HEX(0x5d5f66), COL_HEX(0xffffff), COL_HEX(0xd2d2cd), COL_HEX(0xffffff), COL_HEX(0x8d8f96),
                COL_HEX(0x00a1de), COL_HEX(0x009b3a), COL_HEX(0xf9461c), COL_HEX(0xc60c30), COL_HEX(0xf9e300), COL_HEX(0x62361b), COL_HEX(0x522398), COL_HEX(0xe27ea6),
                COL_HEX(0x0b0b0b), COL_HEX(0xffb000), COL_HEX(0x8a6a1a), COL_HEX(0xc9c9c9), COLA(239, 239, 236, 184), COL_HEX(0xc9cbd1), COLA(128, 128, 128, 72), COL_HEX(0x23262c), COLA(20, 26, 44, 30), COLA(20, 26, 44, 20), COL_HEX(0x23262c) };
    PD = (Pal){ COL_HEX(0x0e1012), COL_HEX(0xf4f4f2), COL_HEX(0xa3a6ad), COL_HEX(0x1b1e22), COL_HEX(0x33363b), COL_HEX(0x0e1012), COL_HEX(0x6c7078),
                COL_HEX(0x1fb6f0), COL_HEX(0x14b852), COL_HEX(0xff6a3c), COL_HEX(0xc60c30), COL_HEX(0xf9e300), COL_HEX(0x62361b), COL_HEX(0x7a4bd0), COL_HEX(0xe27ea6),
                COL_HEX(0x0b0b0b), COL_HEX(0xffb000), COL_HEX(0x8a6a1a), COL_HEX(0xc9c9c9), COLA(14, 16, 18, 184), COL_HEX(0x3a3e46), COLA(128, 128, 128, 72), COL_HEX(0x1d2025), COLA(0, 0, 0, 120), COLA(0, 0, 0, 80), COL_HEX(0x3d424b) };
    P = &PL;
}

// The L's lines, in their real colours. Brown, Purple, Blue and Red are lifted a little on the dark theme so they stay visible on a
// near-black ground. Pink is light enough that white text on it is hard to read, so text on it is dark.
#define NLINES 7
static const struct { const char* name; Col light, dark; bool dark_text; } LOOP_LINES[NLINES] = {
    { "Blue",   COL_HEX(0x00a1de), COL_HEX(0x1fb6f0), false },
    { "Brown",  COL_HEX(0x62361b), COL_HEX(0xb0723f), false },
    { "Green",  COL_HEX(0x009b3a), COL_HEX(0x14b852), false },
    { "Orange", COL_HEX(0xf9461c), COL_HEX(0xff6a3c), false },
    { "Pink",   COL_HEX(0xe27ea6), COL_HEX(0xe27ea6), true },
    { "Purple", COL_HEX(0x522398), COL_HEX(0x8f66dc), false },
    { "Red",    COL_HEX(0xc60c30), COL_HEX(0xe8344f), false },
};
static int g_line[3] = { 5, 6, 1 };  // the two lines on the map, and a third for the header dots

static Col lineCol(int slot) { return P == &PD ? LOOP_LINES[g_line[slot]].dark : LOOP_LINES[g_line[slot]].light; }
#define LA() lineCol(0)
#define LB() lineCol(1)
#define LC() lineCol(2)

// Each mount draws its lines from one palette: lines that belong together on the L. The first two are the lines on the map and the
// third colours the header dots and the Eject bar; which is which is shuffled.
static const int PALETTES[3][3] = {
    { 6, 5, 1 },  // North Side: Red, Purple, Brown
    { 0, 4, 2 },  // West Side: Blue, Pink, Green
    { 6, 2, 3 },  // South Side: Red, Green, Orange
};

void uiRandomizeLines(unsigned seed) {
    seed = seed * 2654435761u + 12345u;
    const int* pal = PALETTES[(seed >> 8) % 3];
    static const int PERM[6][3] = { { 0, 1, 2 }, { 0, 2, 1 }, { 1, 0, 2 }, { 1, 2, 0 }, { 2, 0, 1 }, { 2, 1, 0 } };
    const int* pm = PERM[(seed >> 16) % 6];
    g_line[0] = pal[pm[0]]; g_line[1] = pal[pm[1]]; g_line[2] = pal[pm[2]];
}

static const TextStyle T_REG = { 0, 0.0f, false };
static const TextStyle T_MED = { 1, 0.0f, false };
static const TextStyle T_BLD = { 2, 0.0f, false };

static Col stateColor(UiState s) {
    switch (s) {
    case UI_MOUNTED: case UI_PENDING: case UI_SAVING: return P->green;
    case UI_GONE: return P->orange;
    default: return P->blue;
    }
}

// ---------------------------------------------------------------------------------------------
// State

typedef enum { V_MAIN, V_ADV } View;
typedef enum { D_NONE, D_WARN, D_GUARD, D_QUIT, D_EJECT } Dlg;
enum { F_ADV, F_PRIMARY, F_SAVE, F_ACCESS, F_QUIT };
enum { H_ADV, H_PRIMARY, H_SAVE, H_RO, H_RW, H_QUIT, H_BACK, H_RAIL, H_OPT, H_DLG };

// Every control registers a rectangle as it is drawn, so a tap can be matched to what is under the finger.
typedef struct { float x, y, w, h; int id, arg; } Hit;
static Hit g_hits[64];
static int g_nhits;
static void addHit(float x, float y, float w, float h, int id, int arg) {
    if (g_nhits < 64) g_hits[g_nhits++] = (Hit){ x, y, w, h, id, arg };
}

static UiModel  g_m;
static bool     g_have_model;
static View     g_view = V_MAIN;
static Dlg      g_dlg = D_NONE;
static int      g_dlg_focus;
static double   g_dlg_t0;
static unsigned g_guard_del, g_guard_dirs, g_guard_rw;
static bool     g_rw, g_allowed_whole, g_expert;
static UiShare  g_share = UI_SHARE_WHOLE;
static UiTheme  g_theme = UI_THEME_AUTO;
static int      g_focus = F_PRIMARY;
static int      g_sec, g_opt;   // Advanced: the section on show, and the option focused inside it
static bool     g_in_pane;      // Advanced: focus is in the pane, not in the list of sections
static double   g_now, g_state_t0, g_change_t0;
static bool     g_have_now, g_state_changed, g_no_focus;
static UiState  g_prev_state = UI_IDLE;
static Canvas   g_back;
static bool     g_back_ok, g_back_alloc, g_back_shadow;
static double   g_pulse_t0 = -10;
static double   g_press_t0 = -10;   // when Mount or Eject was last pressed: the activity sign lights up, line by line
static bool     g_press_eject;
static UiState  g_pulse_to;

bool uiAccessRw(void) { return g_rw; }
UiShare uiShare(void) { return g_share; }
UiTheme uiTheme(void) { return g_theme; }
void uiSetTheme(UiTheme t) { g_theme = t; }
bool uiExpert(void) { return g_expert; }
bool uiDialogOpen(void) { return g_dlg != D_NONE; }

// ---------------------------------------------------------------------------------------------
// The map. As on the real L, the Loop is shared track with no line colour of its own. The blue line comes in from the west and meets it
// at a node on a straight edge, the orange line leaves it from another node, and Mounted, Changes waiting and Saving are stations on the
// Loop itself. Lines meet only at nodes on straight track; a diagonal only ever joins two straight pieces.

#define NB 4
#define NL 10
#define NO 4
static const float TB[NB][2] = { { 110, 350 }, { 210, 350 }, { 279, 281 }, { 600, 281 } };
static const float TL[NL][2] = { { 600, 281 }, { 600, 206 }, { 640, 166 }, { 840, 166 }, { 880, 206 }, { 880, 356 }, { 840, 396 }, { 640, 396 }, { 600, 356 }, { 600, 281 } };
static const float TO[NO][2] = { { 880, 340 }, { 980, 340 }, { 1040, 280 }, { 1160, 280 } };
static float Bc[NB], Lc[NL], Oc[NO];
static float g_Lb, g_Lp, g_Lo, g_uo;  // lengths of the blue and orange tracks, the Loop's perimeter, and where the orange node sits on the Loop

enum { Z_B, Z_L, Z_O };
typedef struct { float x, y; char lab, tag; float tdx; int z; float t; const char* name; } Station;
static Station ST[7] = {
    { 110, 350, 'b', 'a', 18, Z_B, 0, "Ready" },
    { 380, 281, 'b', 'a', 0, Z_B, 0, "Reading" },
    { 510, 281, 'b', 'a', -30, Z_B, 0, "Plug in" },
    { 740, 166, 'a', 'b', 0, Z_L, 0, "Mounted" },
    { 880, 236, 'c', 'b', 84, Z_L, 0, "Changes waiting" },
    { 740, 396, 'b', 'a', 0, Z_L, 0, "Saving" },
    { 1160, 280, 'b', 'a', -50, Z_O, 0, "Unplugged" },
};

static void cum(const float (*p)[2], int n, float* c) {
    c[0] = 0;
    for (int i = 1; i < n; i++) c[i] = c[i - 1] + hypotf(p[i][0] - p[i - 1][0], p[i][1] - p[i - 1][1]);
}

static float distOf(const float (*p)[2], const float* c, int n, float x, float y) {
    for (int i = 1; i < n; i++) {
        float ax = p[i - 1][0], ay = p[i - 1][1], bx = p[i][0], by = p[i][1];
        float L = hypotf(bx - ax, by - ay);
        if (L < 1e-6f) continue;
        float t = ((x - ax) * (bx - ax) + (y - ay) * (by - ay)) / (L * L);
        t = t < 0 ? 0 : t > 1 ? 1 : t;
        if (hypotf(ax + (bx - ax) * t - x, ay + (by - ay) * t - y) < 1.5f) return c[i - 1] + t * L;
    }
    return 0;
}

static void atPoly(const float (*p)[2], const float* c, int n, float d, float* x, float* y) {
    if (d < 0) d = 0;
    if (d > c[n - 1]) d = c[n - 1];
    for (int i = 1; i < n; i++)
        if (d <= c[i] + 1e-6f) {
            float L = c[i] - c[i - 1], t = L > 0 ? (d - c[i - 1]) / L : 1.0f;
            *x = p[i - 1][0] + (p[i][0] - p[i - 1][0]) * t;
            *y = p[i - 1][1] + (p[i][1] - p[i - 1][1]) * t;
            return;
        }
    *x = p[n - 1][0]; *y = p[n - 1][1];
}

// The part of a polyline between distances d0 <= d1: points as x,y pairs plus the distance of each.
static int subPoly(const float (*p)[2], const float* c, int n, float d0, float d1, float* xy, float* t, int cap) {
    int k = 0;
    atPoly(p, c, n, d0, &xy[0], &xy[1]); t[k++] = d0;
    for (int i = 1; i < n - 1 && k < cap - 1; i++)
        if (c[i] > d0 + 1e-6f && c[i] < d1 - 1e-6f) { xy[2 * k] = p[i][0]; xy[2 * k + 1] = p[i][1]; t[k++] = c[i]; }
    atPoly(p, c, n, d1, &xy[2 * k], &xy[2 * k + 1]); t[k++] = d1;
    return k;
}

typedef struct { float x, y; int z; float t; } RP;

static struct { float x, y; int z; float t; } mk;
static struct { RP p[64]; float cum[64]; int n; float len, pos, vel, w; UiState to; bool active; } mv;
static float g_carry;

static float wrapLoop(float u) { float w = fmodf(u, g_Lp); return w < 0 ? w + g_Lp : w; }

// Loop points between unwrapped parameters u0 and u1 (either order), oriented from u0 to u1.
static int loopSub(RP* out, int n, float u0, float u1) {
    float lo = u0 < u1 ? u0 : u1, hi = u0 < u1 ? u1 : u0;
    int base = n;
    float x, y;
    atPoly(TL, Lc, NL, wrapLoop(lo), &x, &y); out[n++] = (RP){ x, y, Z_L, lo };
    for (int m = -2; m <= 3; m++)
        for (int k = 0; k < NL - 1; k++) {
            float tt = Lc[k] + m * g_Lp;
            if (tt > lo + 1e-3f && tt < hi - 1e-3f && n < 60) out[n++] = (RP){ TL[k][0], TL[k][1], Z_L, tt };
        }
    atPoly(TL, Lc, NL, wrapLoop(hi), &x, &y); out[n++] = (RP){ x, y, Z_L, hi };
    if (u0 > u1) for (int i = 0; i < (n - base) / 2; i++) { RP tmp = out[base + i]; out[base + i] = out[n - 1 - i]; out[n - 1 - i] = tmp; }
    return n;
}

// Around the Loop the short way (clockwise on a tie, as a train would continue).
static int addLoop(RP* out, int n, float u0, float u1) {
    u0 = wrapLoop(u0); u1 = wrapLoop(u1);
    float dc = wrapLoop(u1 - u0), dd = g_Lp - dc;
    float end = (dc <= dd + 1.0f) ? u0 + dc : u0 - dd;
    return loopSub(out, n, u0, end);
}

static int addLine(RP* out, int n, int z, float t0, float t1) {
    const float (*p)[2] = z == Z_B ? TB : TO;
    const float* c = z == Z_B ? Bc : Oc;
    int np = z == Z_B ? NB : NO;
    float xy[32], t[16];
    float lo = t0 < t1 ? t0 : t1, hi = t0 < t1 ? t1 : t0;
    int k = subPoly(p, c, np, lo, hi, xy, t, 14), base = n;
    for (int i = 0; i < k; i++) out[n++] = (RP){ xy[2 * i], xy[2 * i + 1], z, t[i] };
    if (t0 > t1) for (int i = 0; i < k / 2; i++) { RP tmp = out[base + i]; out[base + i] = out[base + k - 1 - i]; out[base + k - 1 - i] = tmp; }
    return n;
}

// From a point on the Loop (parameter u0) to station b.
static int fromLoop(RP* out, int n, float u0, const Station* b) {
    if (b->z == Z_L) return addLoop(out, n, u0, b->t);
    if (b->z == Z_O) return addLine(out, addLoop(out, n, u0, g_uo), Z_O, 0, g_Lo);
    return addLine(out, addLoop(out, n, u0, 0), Z_B, g_Lb, b->t);
}

static void startMove(UiState to) {
    RP pts[64];
    int n = 0;
    const Station* b = &ST[to];
    if (mk.z == Z_B) {
        if (b->z == Z_B) n = addLine(pts, 0, Z_B, mk.t, b->t);
        else n = fromLoop(pts, addLine(pts, 0, Z_B, mk.t, g_Lb), 0, b);
    } else if (mk.z == Z_L) {
        n = fromLoop(pts, 0, mk.t, b);
    } else {
        if (b->z == Z_O) n = addLine(pts, 0, Z_O, mk.t, b->t);
        else n = fromLoop(pts, addLine(pts, 0, Z_O, mk.t, 0), g_uo, b);
    }
    float len = 0;
    mv.cum[0] = 0;
    for (int i = 0; i < n; i++) { mv.p[i] = pts[i]; if (i) { len += hypotf(pts[i].x - pts[i - 1].x, pts[i].y - pts[i - 1].y); mv.cum[i] = len; } }
    mv.n = n; mv.len = len; mv.to = to;
    if (len < 2.0f) { mk.x = b->x; mk.y = b->y; mk.z = b->z; mk.t = b->t; mv.active = false; return; }
    float carry = (mv.active && g_carry > 20.0f) ? g_carry : 0.0f;
    float resp = 0.4f + len / 2400.0f;
    resp = resp < 0.42f ? 0.42f : resp > 0.8f ? 0.8f : resp;
    mv.w = 6.2831853f / resp;
    mv.pos = 0; mv.vel = carry; mv.active = true;
}

static void placeMarker(void) {
    float d = mv.pos < 0 ? 0 : mv.pos > mv.len ? mv.len : mv.pos;
    for (int i = 1; i < mv.n; i++)
        if (d <= mv.cum[i] + 1e-6f || i == mv.n - 1) {
            float L = mv.cum[i] - mv.cum[i - 1], u = L > 0 ? (d - mv.cum[i - 1]) / L : 1.0f;
            u = u > 1 ? 1 : u;
            const RP *A = &mv.p[i - 1], *B2 = &mv.p[i];
            mk.x = A->x + (B2->x - A->x) * u; mk.y = A->y + (B2->y - A->y) * u;
            mk.z = (u >= 1.0f) ? B2->z : A->z;
            mk.t = A->t + (B2->t - A->t) * u;
            return;
        }
}

static void tickMarker(double dt) {
    if (!mv.active) return;
    float h = (float)dt / 2.0f;
    for (int i = 0; i < 2; i++) {
        float a = -mv.w * mv.w * (mv.pos - mv.len) - 2.0f * mv.w * mv.vel;
        mv.vel += a * h;
        mv.pos += mv.vel * h;
    }
    g_carry = mv.vel;
    placeMarker();
    if (fabsf(mv.len - mv.pos) < 0.6f && fabsf(mv.vel) < 6.0f) {
        const Station* s = &ST[mv.to];
        mk.x = s->x; mk.y = s->y; mk.z = s->z; mk.t = s->t;
        mv.active = false;
        g_pulse_t0 = g_now;
        g_pulse_to = mv.to;
    }
}

// ---------------------------------------------------------------------------------------------
// Text and small pieces

static void glyphBtn(Canvas* c, float cx, float cy, const char* ch, Col col) {
    gfxRing(c, cx, cy, 14.75f, 2.5f, col);
    gfxTextC(c, cx, cy + 6.0f, ch, 17, col, &T_BLD);
}

// One clean ring a few pixels outside the control. Controls are at least 28 px apart, so rings never touch a neighbour.
static void focusRing(Canvas* c, float x, float y, float w, float h, float r) {
    if (g_no_focus) return;
    float t = 0.5f + 0.5f * cosf((float)g_now * 3.5f);  // breathes a little
    gfxRRectStroke(c, x - 6, y - 6, w + 12, h + 12, r + 6, 3.0f, gfxWithAlpha(P->ink, 0.74f + 0.26f * t));
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
static void fitText(const char* s, int px, float maxw, const TextStyle* st, char* out, size_t cap) {
    snprintf(out, cap, "%s", s);
    if (gfxTextWidth(out, px, st) <= maxw) return;
    size_t n = strlen(out);
    while (n > 1) {
        n--;
        out[n] = 0;
        char t[200];
        snprintf(t, sizeof(t), "%s...", out);
        if (gfxTextWidth(t, px, st) <= maxw) { snprintf(out, cap, "%s", t); return; }
    }
}
#pragma GCC diagnostic pop

static const char* titleText(UiState s, bool connecting) {
    static const char* t[] = { "Ready", "Reading SD card", "Plug into your PC", "Mounted", "Changes waiting", "Saving", "Unplugged" };
    return (s == UI_WAITING && connecting) ? "Connecting" : t[s];
}

static void captionText(UiState s, bool connecting, char* out, size_t cap) {
    switch (s) {
    case UI_IDLE: snprintf(out, cap, "%s", g_m.note ? g_m.note : "Now boarding. Press Mount to share your SD card."); break;
    case UI_READING: snprintf(out, cap, "Next stop: Plug in. %u files so far.", g_m.files); break;
    case UI_WAITING:
        if (connecting) snprintf(out, cap, "Your PC is setting up the drive. Next stop: Mounted.");
        else snprintf(out, cap, "Next stop: Mounted. Connect the cable to your PC.");
        break;
    case UI_MOUNTED: snprintf(out, cap, "%s", g_m.note ? g_m.note : "Your PC has the drive. It is named SWITCH SD."); break;
    case UI_PENDING: snprintf(out, cap, "Your PC is still writing. Saving in a moment."); break;
    case UI_SAVING: snprintf(out, cap, "This only takes a moment. Keep the cable plugged in."); break;
    default:
        if (g_m.pending) snprintf(out, cap, "Cable out. What arrived may be incomplete.");
        else snprintf(out, cap, "Ejected. It is safe to pull the cable. Press Mount to ride again.");
        break;
    }
}

// The main button is Mount while ready and Eject once the card is shared; X does the same from the controller.
static const char* primaryLabel(UiState s, bool connecting) {
    switch (s) {
    case UI_IDLE: return "Mount";
    case UI_READING: return "Reading...";
    case UI_WAITING: return connecting ? "Eject" : "Cancel";
    case UI_SAVING: return "Cancel";
    case UI_GONE: return g_m.pending ? "Eject" : "Mount";
    default: return "Eject";
    }
}
// Nothing is mounted and nothing is waiting: Ready, or Unplugged after an eject. Mount, Access and Share can be used.
static bool atRest(UiState s) { return s == UI_IDLE || (s == UI_GONE && !g_m.pending); }
static bool primaryEnabled(UiState s) { return s != UI_READING; }
static bool saveVisible(UiState s) { return s == UI_PENDING || (s == UI_GONE && g_m.pending); }
static const char* saveLabel(UiState s) { return s == UI_GONE ? "Save what arrived" : "Save now"; }

// ---------------------------------------------------------------------------------------------
// Main screen

static void drawHeader(Canvas* c, const char* title, Col d1, Col d2, Col d3, bool advBtn, float* adv_x, float* adv_w) {
    gfxRRect(c, 0, 0, UI_W, 84, 0, P->panel);
    TextStyle ts = { 2, 0.9f, false };
    Col white = COL(255, 255, 255);
    gfxText(c, 64, 53, title, 30, white, &ts);
    float w = gfxTextWidth(title, 30, &ts), x = 64 + w + 24 + 9;
    gfxCircle(c, x, 42, 9, d1); gfxCircle(c, x + 26, 42, 9, d2); gfxCircle(c, x + 52, 42, 9, d3);
    char bat[40] = "";
    if (g_m.battery >= 0) snprintf(bat, sizeof(bat), "%d%% \xC2\xB7 %s", g_m.battery, g_m.charging ? "Charging" : "On battery");
    TextStyle tm = { 1, 0.0f, false };
    gfxTextR(c, 1216, 49, bat, 22, COL_HEX(0xd8d8d8), &tm);
    if (advBtn) {
        float bw = gfxTextWidth(bat, 22, &tm);
        float aw = gfxTextWidth("Advanced", 22, &T_BLD);
        float pw = 18 + 32 + 12 + aw + 18, px = 1216 - bw - 34 - pw;
        glyphBtn(c, px + 18 + 16, 42, "Y", white);
        gfxText(c, px + 18 + 32 + 12, 50, "Advanced", 22, white, &T_BLD);
        *adv_x = px; *adv_w = pw;
        addHit(px, 18, pw, 48, H_ADV, 0);
    }
}

static void flat(const float (*p)[2], int n, float* out) { for (int i = 0; i < n; i++) { out[2 * i] = p[i][0]; out[2 * i + 1] = p[i][1]; } }

// Offsets an open polyline sideways by d (positive = to the left of travel in screen space, which is outward for the Loop,
// since it is traced clockwise). Corners use a mitre so parallel stripes stay parallel around the 45 degree cuts.
static void offsetPoly(const float* xy, int n, float d, float* out) {
    for (int i = 0; i < n; i++) {
        float n1x = 0, n1y = 0, n2x = 0, n2y = 0;
        bool h1 = false, h2 = false;
        if (i > 0) { float dx = xy[2 * i] - xy[2 * i - 2], dy = xy[2 * i + 1] - xy[2 * i - 1], l = hypotf(dx, dy); if (l > 1e-6f) { n1x = dy / l; n1y = -dx / l; h1 = true; } }
        if (i < n - 1) { float dx = xy[2 * i + 2] - xy[2 * i], dy = xy[2 * i + 3] - xy[2 * i + 1], l = hypotf(dx, dy); if (l > 1e-6f) { n2x = dy / l; n2y = -dx / l; h2 = true; } }
        float mx, my;
        if (h1 && h2) { float k = 1.0f + n1x * n2x + n1y * n2y; if (k < 0.2f) k = 0.2f; mx = (n1x + n2x) / k; my = (n1y + n2y) / k; }
        else if (h1) { mx = n1x; my = n1y; }
        else { mx = n2x; my = n2y; }
        out[2 * i] = xy[2 * i] + d * mx;
        out[2 * i + 1] = xy[2 * i + 1] + d * my;
    }
}

static void drawNode(Canvas* c, float x, float y) {
    gfxCircle(c, x, y, 22, P->ink);
    gfxCircle(c, x, y, 16, P->stf);
}

// The marker and its banner wear the colour of the line they are on: the west tail and the Loop (where line A's stripe lights up) are
// line A, the east tail is line B. The colour eases across at the junction instead of jumping.
static int zoneSlot(int z) { return z == Z_O ? 1 : 0; }

static Col markerColor(double now) {
    static Col cur;
    static double t0 = -1;
    Col target = lineCol(zoneSlot(mk.z));
    if (t0 < 0) cur = target;
    else {
        float dt = (float)(now - t0);
        if (dt > 0.1f) dt = 0.1f;
        cur = gfxMix(cur, target, 1.0f - expf(-14.0f * dt));
    }
    t0 = now;
    return cur;
}

static void drawMap(Canvas* c, double now, UiState st) {
    float xy[160], tt[80], fl[2 * NL];
    // Track not yet ridden is a pale tint of the line that owns it. The Loop is two lines overlapped: the blue line's lap and the
    // orange line's lap run side by side as parallel stripes (a "6" and a "9" sharing one rectangle).
    Col tb = gfxMix(P->mute, LA(), 0.14f), to = gfxMix(P->mute, LB(), 0.14f);
    const float SW = 7.0f, OFF = 4.5f;
    float outer[2 * NL], inner[2 * NL];
    flat(TL, NL, fl);
    offsetPoly(fl, NL, OFF, outer);
    offsetPoly(fl, NL, -OFF, inner);
    flat(TB, NB, fl); gfxPoly(c, fl, NB, 16, tb);
    flat(TO, NO, fl); gfxPoly(c, fl, NO, 16, to);
    gfxPoly(c, outer, NL, SW, tb);
    gfxPoly(c, inner, NL, SW, to);
    TextStyle lw = { 2, 6.0f, false };
    gfxTextR(c, 572, 150, "THE LOOPBACK", 20, gfxWithAlpha(P->ink2, 0.7f), &lw);
    // ridden track: the blue tail and the blue lap, then the orange tail
    float bt = mk.z == Z_B ? mk.t : g_Lb;
    if (bt > 1.0f) { int k = subPoly(TB, Bc, NB, 0, bt, xy, tt, 14); gfxPoly(c, xy, k, 16, LA()); }
    float lt = mk.z == Z_L ? wrapLoop(mk.t) : mk.z == Z_O ? g_uo : 0.0f;
    if (lt > 1.0f) {
        RP rp[64];
        float lit[2 * 64], litOut[2 * 64];
        int k = loopSub(rp, 0, 0, lt);
        for (int i = 0; i < k; i++) { lit[2 * i] = rp[i].x; lit[2 * i + 1] = rp[i].y; }
        offsetPoly(lit, k, OFF, litOut);
        gfxPoly(c, litOut, k, SW, LA());
    }
    if (mk.z == Z_O && mk.t > 1.0f) { int k = subPoly(TO, Oc, NO, 0, mk.t, xy, tt, 14); gfxPoly(c, xy, k, 16, LB()); }
    // nodes where the lines meet the Loop
    drawNode(c, TB[NB - 1][0], TB[NB - 1][1]);
    drawNode(c, TO[0][0], TO[0][1]);
    // stations
    Col lcol[3] = { LA(), P->ink, LB() };
    int ci = (int)st, mi = UI_PENDING;
    for (int i = 0; i < 7; i++) {
        const Station* s = &ST[i];
        if (st == UI_GONE && !g_m.pending) mi = UI_MOUNTED;
        bool vis = st == UI_GONE ? (i == UI_GONE || i <= mi) : (i <= ci && i != UI_GONE), cur = (i == (int)st);
        if (!cur) {
            gfxCircle(c, s->x, s->y, 22, vis ? lcol[s->z] : P->sdim);
            gfxCircle(c, s->x, s->y, 16, P->stf);
        }
        const TextStyle* ts = cur ? &T_BLD : &T_MED;
        Col tc = (vis || cur) ? P->ink : P->ink2;
        if (s->lab == 'b') gfxTextC(c, s->x, s->y + 56, s->name, 26, tc, ts);
        else if (s->lab == 'a') gfxTextC(c, s->x, s->y - 32, s->name, 26, tc, ts);
        else if (s->lab == 'c') gfxTextC(c, 740, 289, s->name, 26, tc, ts);  // centred in the Loop, between the Mounted and Saving banners
        else {
            int px = 26;
            float room = (s->x - 34) - (TB[NB - 1][0] + 22 + 18);  // from just right of the west node to the station
            while (px > 18 && gfxTextWidth(s->name, px, ts) > room) px--;
            gfxTextR(c, s->x - 34, s->y + 9, s->name, px, tc, ts);
        }
    }
    // arrival pulse
    double pa = now - g_pulse_t0;
    if (pa >= 0 && pa < 0.65) {
        float u = (float)(pa / 0.65), e = 1.0f - (1.0f - u) * (1.0f - u) * (1.0f - u);
        gfxRing(c, ST[g_pulse_to].x, ST[g_pulse_to].y, 26.0f * (1.0f + 1.6f * e), 6, gfxWithAlpha(markerColor(now), 0.7f * (1.0f - e)));
    }
    // marker: halo, ring, dot
    Col sc = markerColor(now);
    float ph = (float)fmod(now, 2.2) / 2.2f, tg = 0.5f - 0.5f * cosf(6.2831853f * ph);
    gfxCircle(c, mk.x, mk.y, 24.0f + 22.0f * tg, gfxWithAlpha(sc, 0.4f * (1.0f - tg)));
    gfxCircle(c, mk.x, mk.y, 28.5f, COL(255, 255, 255));
    gfxCircle(c, mk.x, mk.y, 21.5f, sc);
    gfxCircle(c, mk.x, mk.y, 9, COL(255, 255, 255));
    if (!mv.active) {
        const Station* s = &ST[st];
        float cx = mk.x + s->tdx, cy = mk.y + (s->tag == 'a' ? -64.0f : 56.0f);
        float px = mk.x < cx - 84 ? cx - 84 : mk.x > cx + 84 ? cx + 84 : mk.x;
        // the pointer is drawn with the banner, in the same colour and overlapping it, so there is no seam between them
        gfxRRect(c, cx - 102.5f, cy - 22.5f, 205, 45, 10, P->bg);
        if (s->tag == 'a') gfxTriangle(c, px - 15, cy + 18, px + 15, cy + 18, px, cy + 36, P->bg);
        else gfxTriangle(c, px - 15, cy - 18, px + 15, cy - 18, px, cy - 36, P->bg);
        gfxRRect(c, cx - 100, cy - 20, 200, 40, 8, sc);
        if (s->tag == 'a') gfxTriangle(c, px - 12, cy + 16, px + 12, cy + 16, px, cy + 32, sc);
        else gfxTriangle(c, px - 12, cy - 16, px + 12, cy - 16, px, cy - 32, sc);
        TextStyle tgs = { 2, 1.5f, false };
        gfxTextC(c, cx, cy + 6, "YOU ARE HERE", 17, LOOP_LINES[g_line[zoneSlot(mk.z)]].dark_text ? COL_HEX(0x111111) : COL(255, 255, 255), &tgs);
    }
}

static void drawBoard(Canvas* c, double now, UiState st) {
    gfxRRect(c, 64, 488, 1152, 112, 16, P->panel);
    // title, caption and dot cross-fade together when the state changes
    double age = now - g_state_t0;
    UiState show = (age < 0.13) ? g_prev_state : st;
    bool cn = g_m.connecting && show == UI_WAITING;
    gfxCircle(c, 124, 544, 28, stateColor(show));
    float a = age < 0.13 ? (float)(1.0 - age / 0.13) : age < 0.26 ? (float)((age - 0.13) / 0.13) : 1.0f;
    float dy = age < 0.13 ? (float)(age / 0.13) * 8.0f : age < 0.26 ? (float)(1.0 - (age - 0.13) / 0.13) * 8.0f : 0.0f;
    char cap[120], fit[120];
    captionText(show, cn, cap, sizeof(cap));
    fitText(cap, 23, 960, &T_REG, fit, sizeof(fit));
    gfxText(c, 180, 543 + dy, titleText(show, cn), 42, gfxWithAlpha(COL(255, 255, 255), a), &T_BLD);
    gfxText(c, 180, 578 + dy, fit, 23, gfxWithAlpha(P->board_txt, a), &T_REG);
}

// ---------------------------------------------------------------------------------------------
// The arrivals board in the empty top-left corner: three lines, like the CTA sign that says what is coming and when.

#define ACT_X 64
#define ACT_Y 112
#define ACT_W 196  // stops short of the "you are here" tags, which hang from x=277 at the Reading and Plug in stops
#define ACT_H 114

// "WRITING 18 MB/s": a bright number and a small unit, or "-" when idle.
static void rateParts(float mbps, char* num, size_t cap) {
    if (mbps < 0.05f) snprintf(num, cap, "-");
    else if (mbps < 10.0f) snprintf(num, cap, "%.1f", mbps);
    else snprintf(num, cap, "%.0f", mbps);
}

// What to tell the user about pulling the cable or pressing Eject.
typedef enum { EJ_NONE, EJ_SAFE, EJ_WAIT, EJ_SAVE } EjectKind;
static EjectKind ejectKind(const UiModel* m) {
    switch (m->state) {
    case UI_MOUNTED: return m->io_busy ? EJ_WAIT : EJ_SAFE;
    case UI_PENDING: return EJ_SAVE;
    case UI_GONE: return m->pending ? EJ_SAVE : EJ_SAFE;
    case UI_SAVING: return EJ_WAIT;
    default: return EJ_NONE;
    }
}

// One bar of the board: a line-coloured strip with a name on the left and the figure on the right. It goes dim when idle.
static void arrivalBar(Canvas* c, float y, Col line, float level, const char* name, const char* num, const char* unit) {
    const float x = ACT_X + 5, w = ACT_W - 10, h = 32;
    // level 1 = the line colour, ~0.5 = a state to notice (Wait, Save first), ~0.2 = dim
    Col fill = gfxMix(P->panel, line, level);
    float it = level >= 0.5f ? 1.0f : 0.45f + (level - 0.22f) / 0.28f * 0.55f;
    if (it < 0.45f) it = 0.45f;
    Col ink = gfxMix(P->panel, COL(255, 255, 255), it);
    gfxRRect(c, x, y, w, h, 6, fill);
    gfxText(c, x + 10, y + 22, name, 20, ink, &T_BLD);
    float right = x + w - 10;
    if (unit[0]) {
        gfxTextR(c, right, y + 22, unit, 13, ink, &T_MED);
        right -= gfxTextWidth(unit, 13, &T_MED) + 4;
    }
    gfxTextR(c, right, y + 23, num, 22, ink, &T_BLD);
}

// 0..1: how much the press of Mount/Eject is lighting bar i. Each bar comes on a beat after the one above, holds, then settles.
static float pressGlow(int i) {
    double a = g_now - g_press_t0 - i * 0.12;
    if (a <= 0 || a > 1.5) return 0.0f;
    float up = a < 0.12 ? (float)(a / 0.12) : 1.0f;
    float down = a > 0.9 ? (float)(1.0 - (a - 0.9) / 0.6) : 1.0f;
    return up < down ? up : down;
}

static void drawActivity(Canvas* c) {
    gfxRRect(c, ACT_X, ACT_Y, ACT_W, ACT_H, 12, P->panel);
    char wv[16], rv[16];
    rateParts(g_m.write_mbps, wv, sizeof(wv));
    rateParts(g_m.read_mbps, rv, sizeof(rv));
    bool wl = g_m.write_mbps >= 0.05f, rl = g_m.read_mbps >= 0.05f;
    EjectKind ek = ejectKind(&g_m);
    float base[3] = { wl ? 1.0f : 0.22f, rl ? 1.0f : 0.22f, ek == EJ_SAFE ? 1.0f : ek == EJ_NONE ? 0.22f : 0.55f };
    float lv[3];
    for (int i = 0; i < 3; i++) { float g = pressGlow(i); lv[i] = base[i] + (1.0f - base[i]) * g; }
    const char* ev = ek == EJ_SAFE ? "Safe" : ek == EJ_WAIT ? "Wait" : ek == EJ_SAVE ? "Save first" : "-";
    if (g_press_eject && g_now - g_press_t0 < 1.2) ev = "Ejecting";
    arrivalBar(c, ACT_Y + 5, LA(), lv[0], "Writing", wv, wl ? "MB/s" : "");
    arrivalBar(c, ACT_Y + 5 + 36, LB(), lv[1], "Reading", rv, rl ? "MB/s" : "");
    arrivalBar(c, ACT_Y + 5 + 72, LC(), lv[2], "Eject", ev, "");
}

// ---------------------------------------------------------------------------------------------
// The row of controls under the board: [Mount or Eject  X] [Save now  R] [Read only | Read and write] ......... [Quit  +]

#define BTN_Y 624
#define BTN_H 72
#define BTN_GAP 32
#define QUIT_X 1036
#define QUIT_W 180

typedef struct { float px, pw, sx, sw, ax, aw, w1, w2; } Row;

static void selectorWidths(float* w1, float* w2) {
    *w1 = gfxTextWidth("Read only", 26, &T_BLD) + 52;
    *w2 = gfxTextWidth("Read and write", 26, &T_BLD) + 52;
}

static void layoutRow(UiState st, bool cn, Row* r) {
    float tw = gfxTextWidth(primaryLabel(st, cn), 30, &T_BLD);
    r->px = 64;
    r->pw = 28 + 28 + 14 + tw + 22 + 32 + 28;
    if (r->pw < 230) r->pw = 230;
    float x = r->px + r->pw + BTN_GAP;
    r->sx = x; r->sw = 0;
    if (saveVisible(st)) {
        r->sw = 28 + gfxTextWidth(saveLabel(st), 28, &T_BLD) + 22 + 32 + 28;
        x += r->sw + BTN_GAP;
    }
    selectorWidths(&r->w1, &r->w2);
    r->ax = x;
    r->aw = 8 + r->w1 + 4 + r->w2;
}

// ---------------------------------------------------------------------------------------------
// A cached background: the flat ground plus the soft shadows under every panel. Shadows cost a lot to draw, so this is built
// once and copied each frame; it is rebuilt when the theme, the screen or the state (which changes the controls) changes.

static void panelShadow(Canvas* c, float x, float y, float w, float h, float r) {
    gfxShadow(c, x, y, w, h, r, 22, 11, P->shadowA);
    gfxShadow(c, x, y, w, h, r, 4, 2, P->shadowB);
}

#define RAIL_X 64
#define RAIL_W 290
#define PANE_X 386
#define PANE_W 830
#define TOP 116
#define PANE_H 480

static float backWidth(void) { return 28 + 32 + 12 + gfxTextWidth("Back", 26, &T_BLD) + 28; }

static void buildBase(Canvas* c, int view, UiState st, bool cn) {
    gfxFill(c, P->bg);
    gfxShadow(c, 0, -60, UI_W, 144, 0, 12, 5, P->shadowA);  // under the header bar
    if (view == V_MAIN) {
        panelShadow(c, 64, 488, 1152, 112, 16);
        panelShadow(c, ACT_X, ACT_Y, ACT_W, ACT_H, 12);
        Row r;
        layoutRow(st, cn, &r);
        panelShadow(c, r.px, BTN_Y, r.pw, BTN_H, 12);
        if (r.sw > 0) panelShadow(c, r.sx, BTN_Y, r.sw, BTN_H, 12);
        panelShadow(c, r.ax, BTN_Y, r.aw, BTN_H, 12);
        panelShadow(c, QUIT_X, BTN_Y, QUIT_W, BTN_H, 12);
    } else {
        for (int i = 0; i < 5; i++) panelShadow(c, RAIL_X, TOP + i * 84, RAIL_W, 72, 14);
        panelShadow(c, PANE_X, TOP, PANE_W, PANE_H, 20);
        panelShadow(c, 64, 624, backWidth(), 64, 12);
    }
}

static Canvas g_base;
static bool   g_base_alloc, g_base_ok;
static int    g_base_key = -1;

static void useBase(Canvas* c, int view, UiState st, bool cn) {
    int key = view | (g_m.dark ? 2 : 0) | ((int)st << 2) | (cn ? 64 : 0) | (g_m.pending ? 128 : 0);
    if (!g_base_alloc) {
        g_base.px = malloc((size_t)UI_W * UI_H * 4);
        g_base.w = UI_W; g_base.h = UI_H; g_base.stride = UI_W;
        g_base_alloc = g_base.px != NULL;
    }
    if (!g_base_alloc) { gfxFill(c, P->bg); return; }
    if (!g_base_ok || g_base_key != key) { buildBase(&g_base, view, st, cn); g_base_ok = true; g_base_key = key; }
    gfxCopy(c, &g_base);
}

static void drawButtons(Canvas* c, UiState st, bool cn, const Row* r) {
    Col white = COL(255, 255, 255);
    // main button
    bool en = primaryEnabled(st);
    float al = en ? 1.0f : 0.7f;
    gfxRRect(c, r->px, BTN_Y, r->pw, BTN_H, 12, en ? P->panel : gfxMix(P->bg, P->panel, 0.62f));
    gfxCircle(c, r->px + 42, BTN_Y + 36, 14, gfxWithAlpha(stateColor(st), al));
    gfxText(c, r->px + 70, BTN_Y + 47, primaryLabel(st, cn), 30, gfxWithAlpha(white, al), &T_BLD);
    glyphBtn(c, r->px + r->pw - 28 - 16, BTN_Y + 36, "X", gfxWithAlpha(white, al * 0.9f));
    addHit(r->px, BTN_Y, r->pw, BTN_H, H_PRIMARY, 0);
    // save, only when something is waiting
    if (r->sw > 0) {
        gfxRRect(c, r->sx, BTN_Y, r->sw, BTN_H, 12, stateColor(st));
        gfxText(c, r->sx + 28, BTN_Y + 47, saveLabel(st), 28, white, &T_BLD);
        glyphBtn(c, r->sx + r->sw - 28 - 16, BTN_Y + 36, "R", white);
        addHit(r->sx, BTN_Y, r->sw, BTN_H, H_SAVE, 0);
    }
    // access: one pill, the chosen side filled; A (or a tap) flips it while nothing is mounted
    bool idle = atRest(st);
    gfxRRect(c, r->ax, BTN_Y, r->aw, BTN_H, 12, P->card);
    gfxRRectStroke(c, r->ax, BTN_Y, r->aw, BTN_H, 12, 2, P->out_stroke);
    float ox1 = r->ax + 4, ox2 = ox1 + r->w1 + 4;
    if (!g_rw) gfxRRect(c, ox1, BTN_Y + 4, r->w1, 64, 9, P->chip);
    else gfxRRect(c, ox2, BTN_Y + 4, r->w2, 64, 9, P->orange);
    Col off = idle ? P->ink : gfxWithAlpha(P->ink, 0.35f);
    gfxTextC(c, ox1 + r->w1 * 0.5f, BTN_Y + 45, "Read only", 26, !g_rw ? white : off, &T_BLD);
    gfxTextC(c, ox2 + r->w2 * 0.5f, BTN_Y + 45, "Read and write", 26, g_rw ? white : off, &T_BLD);
    addHit(ox1, BTN_Y, r->w1 + 4, BTN_H, H_RO, 0);
    addHit(ox2 - 4, BTN_Y, r->w2 + 8, BTN_H, H_RW, 0);
    // quit
    gfxRRect(c, QUIT_X, BTN_Y, QUIT_W, BTN_H, 12, P->card);
    gfxRRectStroke(c, QUIT_X, BTN_Y, QUIT_W, BTN_H, 12, 2, P->out_stroke);
    gfxText(c, QUIT_X + 28, BTN_Y + 47, "Quit", 30, P->ink, &T_BLD);
    glyphBtn(c, QUIT_X + QUIT_W - 28 - 16, BTN_Y + 36, "+", P->ink);
    addHit(QUIT_X, BTN_Y, QUIT_W, BTN_H, H_QUIT, 0);
}

static void drawMain(Canvas* c, double now) {
    UiState st = g_m.state;
    bool cn = g_m.connecting;
    useBase(c, V_MAIN, st, cn);
    float ax = 0, aw = 0;
    drawHeader(c, "LOOPBACK", LA(), LC(), LB(), true, &ax, &aw);
    drawMap(c, now, st);
    drawActivity(c);
    drawBoard(c, now, st);
    Row r;
    layoutRow(st, cn, &r);
    drawButtons(c, st, cn, &r);
    if (g_focus == F_SAVE && r.sw <= 0) g_focus = F_PRIMARY;
    switch (g_focus) {
    case F_ADV: focusRing(c, ax, 18, aw, 48, 10); break;
    case F_PRIMARY: focusRing(c, r.px, BTN_Y, r.pw, BTN_H, 12); break;
    case F_SAVE: focusRing(c, r.sx, BTN_Y, r.sw, BTN_H, 12); break;
    case F_ACCESS: focusRing(c, r.ax, BTN_Y, r.aw, BTN_H, 12); break;
    default: focusRing(c, QUIT_X, BTN_Y, QUIT_W, BTN_H, 12); break;
    }
}

// ---------------------------------------------------------------------------------------------
// Advanced: a list of sections on the left, one calm pane on the right.

static const char* const SEC_NAME[5] = { "Share", "Appearance", "Expert mode", "Connection", "Activity" };
static const char* const SEC_DESC[5] = {
    "What your PC sees when you mount. Eject first to change it.",
    "Colours for this screen.",
    "For when you know what you are doing. Off unless you turn it on.",
    "How your PC is talking to this Switch.",
    "What happened in this session.",
};
static const char* const SHARE_NAME[3] = { "Whole card", "Test folder", "RAM disk test" };
static const char* const SHARE_DESC[3] = { "Everything on the SD card.", "Only the nx-test folder. Safe for trying things.", "A small throwaway drive for checking the cable." };
static const char* const THEME_NAME[3] = { "Match console", "Light", "Dark" };
static const char* const THEME_DESC[3] = { "Follows the Switch's own light or dark setting.", "Always light.", "Always dark." };
static const char* const EXPERT_LINES[4] = {
    "Read and write is the default.",
    "No warning before allowing changes to the card.",
    "Large deletions apply without asking.",
    "Quitting or ejecting saves your changes first.",
};

static int secOptCount(int sec) { return sec <= 1 ? 3 : sec == 2 ? 1 : 0; }
static int secSelected(int sec) { return sec == 0 ? (int)g_share : sec == 1 ? (int)g_theme : 0; }

static void secValue(int sec, char* out, size_t cap) {
    switch (sec) {
    case 0: snprintf(out, cap, "%s", SHARE_NAME[g_share]); break;
    case 1: snprintf(out, cap, "%s", g_theme == UI_THEME_AUTO ? "Auto" : THEME_NAME[g_theme]); break;
    case 2: snprintf(out, cap, "%s", g_expert ? "On" : "Off"); break;
    case 3: snprintf(out, cap, "%s", g_m.link ? g_m.link : "-"); break;
    default: out[0] = 0; break;
    }
}

static void chevron(Canvas* c, float x, float y, Col col) {
    float p[6] = { x, y - 8, x + 8, y, x, y + 8 };
    gfxPoly(c, p, 3, 3.0f, col);
}

static void drawRail(Canvas* c) {
    for (int i = 0; i < 5; i++) {
        float y = TOP + i * 84;
        bool sel = i == g_sec;
        gfxRRect(c, RAIL_X, y, RAIL_W, 72, 14, sel ? P->chip : P->card);
        if (!sel) gfxRRectStroke(c, RAIL_X, y, RAIL_W, 72, 14, 2, P->out_stroke);
        Col ink = sel ? COL(255, 255, 255) : P->ink;
        Col ink2 = sel ? COL_HEX(0xb8bcc6) : P->ink2;
        char v[40];
        secValue(i, v, sizeof(v));
        gfxText(c, RAIL_X + 26, v[0] ? y + 33 : y + 45, SEC_NAME[i], 27, ink, &T_BLD);
        if (v[0]) gfxText(c, RAIL_X + 26, y + 59, v, 20, ink2, &T_REG);
        if (sel) chevron(c, RAIL_X + RAIL_W - 34, y + 36, ink2);
        addHit(RAIL_X, y, RAIL_W, 72, H_RAIL, i);
        if (sel && !g_in_pane) focusRing(c, RAIL_X, y, RAIL_W, 72, 14);
    }
}

static void drawOptionRow(Canvas* c, float x, float y, float w, const char* title, const char* desc, bool selected, bool enabled, int idx) {
    float al = enabled ? 1.0f : 0.45f;
    gfxRRect(c, x, y, w, 84, 14, selected ? gfxMix(P->bg, P->ink, 0.07f) : P->bg);
    float rx = x + 38, ry = y + 42;
    gfxRing(c, rx, ry, 14, 3, gfxWithAlpha(P->ink2, al));
    if (selected) gfxCircle(c, rx, ry, 7, gfxWithAlpha(P->ink, al));
    gfxText(c, x + 76, y + 37, title, 28, gfxWithAlpha(P->ink, al), &T_BLD);
    char fit[160];
    fitText(desc, 22, w - 110, &T_REG, fit, sizeof(fit));
    gfxText(c, x + 76, y + 66, fit, 22, gfxWithAlpha(P->ink2, al), &T_REG);
    addHit(x, y, w, 84, H_OPT, idx);
    if (g_in_pane && g_opt == idx) focusRing(c, x, y, w, 84, 14);
}

static void drawPane(Canvas* c) {
    bool dark = g_sec == 4;
    float x = PANE_X, y = TOP, w = PANE_W, h = PANE_H;
    gfxRRect(c, x, y, w, h, 20, dark ? P->panel : P->card);
    if (!dark) gfxRRectStroke(c, x, y, w, h, 20, 2, P->faint);
    Col ink = dark ? COL(255, 255, 255) : P->ink;
    Col ink2 = dark ? COL_HEX(0xb8bcc6) : P->ink2;
    gfxText(c, x + 40, y + 66, SEC_NAME[g_sec], 38, ink, &T_BLD);
    char fit[160];
    fitText(SEC_DESC[g_sec], 24, w - 80, &T_REG, fit, sizeof(fit));
    gfxText(c, x + 40, y + 106, fit, 24, ink2, &T_REG);
    float cy = y + 146;
    bool idle = atRest(g_m.state);
    switch (g_sec) {
    case 0:
        for (int i = 0; i < 3; i++) drawOptionRow(c, x + 32, cy + i * 96, w - 64, SHARE_NAME[i], SHARE_DESC[i], (int)g_share == i, idle, i);
        break;
    case 1:
        for (int i = 0; i < 3; i++) drawOptionRow(c, x + 32, cy + i * 96, w - 64, THEME_NAME[i], THEME_DESC[i], (int)g_theme == i, true, i);
        break;
    case 2: {
        float rx = x + 32, rw = w - 64;
        gfxRRect(c, rx, cy, rw, 84, 14, P->bg);
        gfxText(c, rx + 28, cy + 37, "Enable expert mode", 28, P->ink, &T_BLD);
        gfxText(c, rx + 28, cy + 66, g_expert ? "On" : "Off", 22, P->ink2, &T_REG);
        float sx = rx + rw - 28 - 88, sy = cy + 20;  // the switch
        gfxRRect(c, sx, sy, 88, 44, 22, g_expert ? P->green : P->mute);
        gfxCircle(c, g_expert ? sx + 88 - 22 : sx + 22, sy + 22, 17, COL(255, 255, 255));
        addHit(rx, cy, rw, 84, H_OPT, 0);
        if (g_in_pane) focusRing(c, rx, cy, rw, 84, 14);
        for (int i = 0; i < 4; i++) {
            float ly = cy + 140 + i * 46;
            gfxCircle(c, rx + 34, ly - 8, 5, P->ink2);
            gfxText(c, rx + 56, ly, EXPERT_LINES[i], 25, P->ink, &T_REG);
        }
        break;
    }
    case 3: {
        char errs[16];
        snprintf(errs, sizeof(errs), "%u", g_m.errors);
        const char* kv[5][2] = { { "Link", g_m.link ? g_m.link : "-" }, { "Access", g_rw ? "Read and write" : "Read only" },
                                 { "Read", g_m.read_text }, { "Written", g_m.written_text }, { "Errors", errs } };
        for (int i = 0; i < 5; i++) {
            float ly = cy + 40 + i * 62;
            gfxText(c, x + 40, ly, kv[i][0], 26, P->ink, &T_REG);
            gfxTextR(c, x + w - 40, ly, kv[i][1], 26, P->ink2, &T_REG);
            if (i < 4) gfxRRect(c, x + 40, ly + 18, w - 80, 1.5f, 0, P->mute);
        }
        break;
    }
    default: {
        int n = g_m.n_log;
        for (int i = 0; i < n && i < 8; i++) {
            char line[LOG_FIT];
            fitText(g_m.log[n - 1 - i], 22, w - 80, &T_REG, line, sizeof(line));
            gfxText(c, x + 40, cy + 30 + i * 40, line, 22, P->amber, &T_REG);
        }
        if (n == 0) gfxText(c, x + 40, cy + 30, "Nothing yet.", 22, P->amber_dim, &T_REG);
        break;
    }
    }
}

static void drawAdvanced(Canvas* c, double now) {
    (void)now;
    useBase(c, V_ADV, UI_IDLE, false);
    float d1;
    drawHeader(c, "ADVANCED", P->brown, P->purple, P->pink, false, &d1, &d1);
    drawRail(c);
    drawPane(c);
    float bw = backWidth();
    gfxRRect(c, 64, 624, bw, 64, 12, P->panel);
    glyphBtn(c, 64 + 28 + 16, 656, "B", COL(255, 255, 255));
    gfxText(c, 64 + 28 + 32 + 12, 665, "Back", 26, COL(255, 255, 255), &T_BLD);
    addHit(64, 624, bw, 64, H_BACK, 0);
    gfxText(c, 64 + bw + 36, 664, g_in_pane ? "Up and down to choose, A to select, left to go back." : "Up and down for sections, A or right to open.", 22, P->ink2, &T_REG);
}

// ---------------------------------------------------------------------------------------------
// Dialogs

typedef struct {
    char title[96], body[200];
    const char *band, *lb, *rb;
    Col bandc, rightc;
    int tl, bl;
    float W, H, x, y;
} DlgInfo;

static void dlgInfo(DlgInfo* d) {
    switch (g_dlg) {
    case D_WARN:
        d->band = "Caution"; d->bandc = P->orange; d->rightc = P->orange;
        snprintf(d->title, sizeof(d->title), "Allow changes to your whole card?");
        snprintf(d->body, sizeof(d->body), "Your PC will be able to change or delete anything on it. Back up first.");
        d->lb = "Not Now"; d->rb = "Allow Changes";
        break;
    case D_GUARD:
        d->band = "Heads up"; d->bandc = P->red; d->rightc = P->red;
        if (g_guard_dirs == 0 && g_guard_rw == 0) {
            snprintf(d->title, sizeof(d->title), "Delete %u files?", g_guard_del);
            snprintf(d->body, sizeof(d->body), "Your PC deleted %u files. Nothing has changed on the card yet.", g_guard_del);
            d->rb = "Delete";
        } else {
            snprintf(d->title, sizeof(d->title), "Apply these changes?");
            snprintf(d->body, sizeof(d->body), "Your PC deleted %u files and %u folders and overwrote %u files. Nothing has changed on the card yet.", g_guard_del, g_guard_dirs, g_guard_rw);
            d->rb = "Apply";
        }
        d->lb = "Keep Files";
        break;
    case D_EJECT:
        d->band = "Before you eject"; d->bandc = P->blue; d->rightc = P->blue;
        snprintf(d->title, sizeof(d->title), "Save changes before ejecting?");
        snprintf(d->body, sizeof(d->body), "Some changes from your PC aren't on the card yet.");
        d->lb = "Discard"; d->rb = "Save and Eject";
        break;
    default:
        d->band = "Before you go"; d->bandc = P->blue; d->rightc = P->blue;
        snprintf(d->title, sizeof(d->title), "Save changes before quitting?");
        snprintf(d->body, sizeof(d->body), "Some changes from your PC aren't on the card yet.");
        d->lb = "Discard"; d->rb = "Save and Quit";
        break;
    }
    const float tw = 680;
    d->tl = gfxTextWrapCount(tw, d->title, 38, &T_BLD);
    d->bl = gfxTextWrapCount(tw, d->body, 26, &T_REG);
    d->W = 760;
    d->H = 64 + 36 + d->tl * 42.6f + 14 + d->bl * 36.4f + 16 + 24 + 76 + 40;
    d->x = (UI_W - d->W) / 2;
    d->y = (UI_H - d->H) / 2;
}

static void drawDialog(Canvas* c, double now) {
    DlgInfo d;
    dlgInfo(&d);
    double age = now - g_dlg_t0;
    float e = age < 0.3 ? (float)(age / 0.3) : 1.0f;
    e = 1.0f - powf(1.0f - e, 3.0f);
    float x = d.x, y = d.y + (1.0f - e) * 16.0f, W = d.W, H = d.H;
    gfxRRect(c, x - 1, y - 1, W + 2, H + 2, 19, P->faint);
    gfxRRect(c, x, y, W, H, 18, P->card);
    // header band: rounded top only
    gfxRRect(c, x, y, W, 64, 18, d.bandc);
    gfxRRect(c, x, y + 30, W, 34, 0, d.bandc);
    gfxCircle(c, x + 40 + 12, y + 32, 12, COL(255, 255, 255));
    TextStyle bs = { 2, 3.7f, false };
    char up[40];
    size_t i;
    for (i = 0; d.band[i] && i < sizeof(up) - 1; i++) up[i] = (d.band[i] >= 'a' && d.band[i] <= 'z') ? (char)(d.band[i] - 32) : d.band[i];
    up[i] = 0;
    gfxText(c, x + 40 + 24 + 16, y + 40, up, 23, COL(255, 255, 255), &bs);
    const float tw = 680;
    float ty = y + 64 + 36 + 33;
    gfxTextWrap(c, x + 40, ty, tw, 42.6f, d.title, 38, P->ink, &T_BLD);
    float py = ty + (d.tl - 1) * 42.6f + 14 + 27;
    gfxTextWrap(c, x + 40, py, tw, 36.4f, d.body, 26, P->ink2, &T_REG);
    float by = y + H - 40 - 76, bw = (W - 80 - 20) / 2;
    float bx0 = x + 40, bx1 = bx0 + bw + 20;
    gfxRRect(c, bx0, by, bw, 76, 12, P->card);
    gfxRRectStroke(c, bx0, by, bw, 76, 12, 2.5f, P->out_stroke);
    gfxTextC(c, bx0 + bw / 2, by + 48, d.lb, 30, P->ink, &T_BLD);
    gfxRRect(c, bx1, by, bw, 76, 12, d.rightc);
    gfxTextC(c, bx1 + bw / 2, by + 48, d.rb, 30, COL(255, 255, 255), &T_BLD);
    addHit(bx0, by, bw, 76, H_DLG, 0);
    addHit(bx1, by, bw, 76, H_DLG, 1);
    if (g_dlg_focus == 0) focusRing(c, bx0, by, bw, 76, 12); else focusRing(c, bx1, by, bw, 76, 12);
}

// ---------------------------------------------------------------------------------------------
// Public

void uiInit(void) {
    initPalettes();
    cum(TB, NB, Bc); cum(TL, NL, Lc); cum(TO, NO, Oc);
    g_Lb = Bc[NB - 1]; g_Lp = Lc[NL - 1]; g_Lo = Oc[NO - 1];
    g_uo = distOf(TL, Lc, NL, TO[0][0], TO[0][1]);
    for (int i = 0; i < 7; i++) {
        Station* s = &ST[i];
        if (s->z == Z_B) s->t = distOf(TB, Bc, NB, s->x, s->y);
        else if (s->z == Z_L) s->t = distOf(TL, Lc, NL, s->x, s->y);
        else s->t = g_Lo;
    }
    mk.x = ST[UI_IDLE].x; mk.y = ST[UI_IDLE].y; mk.z = Z_B; mk.t = 0;
    mv.active = false;
    g_view = V_MAIN; g_dlg = D_NONE; g_focus = F_PRIMARY; g_sec = 0; g_opt = 0; g_in_pane = false;
    g_rw = false; g_allowed_whole = false; g_expert = false; g_share = UI_SHARE_WHOLE;
    g_have_model = false; g_have_now = false; g_back_ok = false; g_base_ok = false;
    memset(&g_m, 0, sizeof(g_m));
    g_m.state = UI_IDLE; g_m.battery = -1;
    g_prev_state = UI_IDLE; g_state_t0 = -10; g_change_t0 = -10; g_pulse_t0 = -10; g_press_t0 = -10; g_press_eject = false;
}

void uiSetModel(const UiModel* m) {
    bool theme = g_have_model && m->dark != g_m.dark;
    bool changed = g_have_model && m->state != g_m.state;
    UiState from = g_m.state;
    g_m = *m;
    P = g_m.dark ? &PD : &PL;
    if (!g_have_model) { g_have_model = true; return; }
    if (theme) g_back_ok = false;
    if (changed) {
        g_prev_state = from;
        g_state_changed = true;
        g_back_ok = false;
        startMove(m->state);
    }
}

void uiSetExpert(bool on) {
    g_expert = on;
    if (on) g_rw = true;
}

void uiOpenGuard(unsigned files_deleted, unsigned dirs_deleted, unsigned files_rewritten) {
    g_guard_del = files_deleted; g_guard_dirs = dirs_deleted; g_guard_rw = files_rewritten;
    g_dlg = D_GUARD; g_dlg_focus = 0; g_dlg_t0 = g_now; g_back_ok = false;
}

static void openDlg(Dlg d, int focus) { g_dlg = d; g_dlg_focus = focus; g_dlg_t0 = g_now; g_back_ok = false; }

// ---- what each control does, shared by the controller and the touch screen

static UiAction notePress(UiAction a, bool eject) {
    if (a != UIA_NONE) { g_press_t0 = g_now; g_press_eject = eject; }
    return a;
}

static UiAction pressPrimary(void) {
    UiState st = g_m.state;
    if (atRest(st)) {
        if (g_rw && g_share == UI_SHARE_WHOLE && !g_allowed_whole && !g_expert) { openDlg(D_WARN, 0); return UIA_NONE; }
        return notePress(UIA_MOUNT, false);
    }
    if (st == UI_READING) return UIA_NONE;
    if (st == UI_SAVING) return UIA_CANCEL;
    if (g_m.pending) {
        if (g_expert) return notePress(UIA_EJECT_SAVE, true);
        openDlg(D_EJECT, 1);
        return UIA_NONE;
    }
    return notePress(UIA_EJECT, g_m.state != UI_WAITING || g_m.connecting);
}

static UiAction pressSave(void) { return saveVisible(g_m.state) ? UIA_SAVE : UIA_NONE; }

static UiAction pressQuit(void) {
    if (g_m.pending) {
        if (g_expert) return UIA_QUIT_SAVE;
        openDlg(D_QUIT, 1);
        return UIA_NONE;
    }
    return UIA_QUIT;
}

static void setAccess(bool rw) { if (atRest(g_m.state)) g_rw = rw; }

static void openAdvanced(void) { g_view = V_ADV; g_in_pane = false; g_opt = 0; }

// pick: 0 left button, 1 right button, -1 dismissed with B
static UiAction dlgResolve(int pick) {
    Dlg d = g_dlg;
    g_dlg = D_NONE;
    g_back_ok = false;
    switch (d) {
    case D_WARN:
        if (pick == 1) { g_allowed_whole = true; return notePress(UIA_MOUNT, false); }
        g_rw = false;
        return UIA_NONE;
    case D_GUARD: return pick == 1 ? UIA_GUARD_APPLY : UIA_GUARD_REFUSE;
    case D_EJECT: return pick < 0 ? UIA_NONE : notePress(pick == 1 ? UIA_EJECT_SAVE : UIA_EJECT_DISCARD, true);
    default: return pick < 0 ? UIA_NONE : pick == 1 ? UIA_QUIT_SAVE : UIA_QUIT_DISCARD;
    }
}

static void applyOption(int sec, int opt) {
    if (sec == 0) { if (atRest(g_m.state)) g_share = (UiShare)opt; }
    else if (sec == 1) { g_theme = (UiTheme)opt; g_back_ok = false; }
    else if (sec == 2) {
        g_expert = !g_expert;
        if (atRest(g_m.state)) g_rw = g_expert;  // turning it on makes Read and write the default; off goes back to Read only
    }
}

static UiAction mainKeys(const UiKeys* k) {
    UiState st = g_m.state;
    if (k->y) { openAdvanced(); return UIA_NONE; }
    if (k->x) return pressPrimary();
    if (k->r) return pressSave();
    if (k->plus) return pressQuit();
    // focus order along the row; up goes to Advanced, down comes back
    int order[4], n = 0, cur = -1;
    order[n++] = F_PRIMARY;
    if (saveVisible(st)) order[n++] = F_SAVE;
    order[n++] = F_ACCESS;
    order[n++] = F_QUIT;
    for (int i = 0; i < n; i++) if (order[i] == g_focus) cur = i;
    if ((k->left || k->right) && cur >= 0) g_focus = order[(cur + (k->right ? 1 : n - 1)) % n];
    if (k->up) g_focus = F_ADV;
    if (k->down && g_focus == F_ADV) g_focus = F_PRIMARY;
    if (k->a) {
        switch (g_focus) {
        case F_ADV: openAdvanced(); break;
        case F_PRIMARY: return pressPrimary();
        case F_SAVE: return pressSave();
        case F_ACCESS: setAccess(!g_rw); break;
        default: return pressQuit();
        }
    }
    return UIA_NONE;
}

static UiAction advKeys(const UiKeys* k) {
    int count = secOptCount(g_sec);
    if (!g_in_pane) {
        if (k->b || k->y) { g_view = V_MAIN; return UIA_NONE; }
        if (k->up && g_sec > 0) g_sec--;
        if (k->down && g_sec < 4) g_sec++;
        if ((k->a || k->right) && count > 0) { g_in_pane = true; g_opt = secSelected(g_sec); }
        return UIA_NONE;
    }
    if (k->b || k->left) { g_in_pane = false; return UIA_NONE; }
    if (k->up && g_opt > 0) g_opt--;
    if (k->down && g_opt < count - 1) g_opt++;
    if (k->a) applyOption(g_sec, g_opt);
    return UIA_NONE;
}

UiAction uiKeys(const UiKeys* k) {
    if (g_dlg != D_NONE) {
        if (k->left || k->right) g_dlg_focus ^= 1;
        if (k->a) return dlgResolve(g_dlg_focus);
        if (k->b) return dlgResolve((g_dlg == D_WARN || g_dlg == D_GUARD) ? 0 : -1);
        return UIA_NONE;
    }
    return g_view == V_ADV ? advKeys(k) : mainKeys(k);
}

// A tap does what pressing A on that control would.
UiAction uiTouch(int x, int y) {
    for (int i = g_nhits - 1; i >= 0; i--) {
        const Hit* h = &g_hits[i];
        if (x < h->x || x >= h->x + h->w || y < h->y || y >= h->y + h->h) continue;
        bool dlg_hit = h->id == H_DLG;
        if (g_dlg != D_NONE) {
            if (!dlg_hit) continue;
            g_dlg_focus = h->arg;
            return dlgResolve(h->arg);
        }
        if (dlg_hit) continue;
        if (g_view == V_MAIN) {
            switch (h->id) {
            case H_ADV: g_focus = F_ADV; openAdvanced(); return UIA_NONE;
            case H_PRIMARY: g_focus = F_PRIMARY; return pressPrimary();
            case H_SAVE: g_focus = F_SAVE; return pressSave();
            case H_RO: g_focus = F_ACCESS; setAccess(false); return UIA_NONE;
            case H_RW: g_focus = F_ACCESS; setAccess(true); return UIA_NONE;
            case H_QUIT: g_focus = F_QUIT; return pressQuit();
            default: continue;
            }
        } else {
            switch (h->id) {
            case H_BACK: g_view = V_MAIN; return UIA_NONE;
            case H_RAIL: g_sec = h->arg; g_in_pane = false; return UIA_NONE;
            case H_OPT: g_in_pane = true; g_opt = h->arg; applyOption(g_sec, h->arg); return UIA_NONE;
            default: continue;
            }
        }
    }
    return UIA_NONE;
}

bool uiMarkerTravelling(void) { return *(volatile bool*)&mv.active; }

bool uiAnimating(void) {
    double age = g_now - g_state_t0;
    return mv.active || age < 0.3 || (g_now - g_pulse_t0) < 0.7 || (g_now - g_dlg_t0) < 0.35 || (g_now - g_change_t0) < 0.4 || (g_now - g_press_t0) < 1.6 || g_state_changed;
}

void uiDraw(Canvas* c, double now) {
    double dt = g_have_now ? now - g_now : 0.0;
    if (dt < 0) dt = 0;
    if (dt > 0.05) dt = 0.05;
    g_now = now;
    g_have_now = true;
    g_nhits = 0;
    if (g_state_changed) { g_state_t0 = now; g_change_t0 = now; g_state_changed = false; }
    tickMarker(dt);

    if (g_dlg != D_NONE) {
        if (!g_back_alloc) {
            g_back.px = malloc((size_t)UI_W * UI_H * 4);
            g_back.w = UI_W; g_back.h = UI_H; g_back.stride = UI_W;
            g_back_alloc = g_back.px != NULL;
        }
        if (!g_back_ok) {
            Canvas tmp = *c;
            g_no_focus = true;
            drawMain(c, now);
            g_no_focus = false;
            g_nhits = 0;  // the controls behind a dialog are not touchable
            if (g_back_alloc) {
                gfxBlurCopy(&g_back, &tmp);
                gfxTint(&g_back, P->scrim);
                g_back_shadow = false;
                g_back_ok = true;
            }
        }
        if (g_back_ok) {
            if (!g_back_shadow) {
                DlgInfo d;
                dlgInfo(&d);
                gfxShadow(&g_back, d.x, d.y, d.W, d.H, 18, 34, 16, P->shadowA);
                gfxShadow(&g_back, d.x, d.y, d.W, d.H, 18, 6, 3, P->shadowB);
                g_back_shadow = true;
            }
            gfxCopy(c, &g_back);
        }
        drawDialog(c, now);
        return;
    }
    if (g_view == V_ADV) drawAdvanced(c, now);
    else drawMain(c, now);
}
