#include "gfx.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#pragma GCC diagnostic pop

// ---------------------------------------------------------------------------------------------
// Pixels

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float minf(float a, float b) { return a < b ? a : b; }
static inline float maxf(float a, float b) { return a > b ? a : b; }

// Coverage (0..255) of a pixel whose centre is d pixels outside the shape edge (negative = inside).
static inline int covFromDist(float d) {
    float c = 0.5f - d;
    if (c <= 0.0f) return 0;
    if (c >= 1.0f) return 255;
    return (int)(c * 255.0f + 0.5f);
}

static inline void blendPx(Canvas* c, int x, int y, Col src, int cov) {
    if (cov <= 0 || (unsigned)x >= (unsigned)c->w || (unsigned)y >= (unsigned)c->h) return;
    int a = (COL_ALPHA(src) * cov + 127) / 255;
    if (a <= 0) return;
    uint32_t* d = &c->px[(size_t)y * c->stride + x];
    if (a >= 255) { *d = src | 0xFF000000u; return; }
    uint32_t dv = *d;
    int ia = 255 - a;
    uint32_t r = ((src & 255) * a + (dv & 255) * ia + 127) / 255;
    uint32_t g = (((src >> 8) & 255) * a + ((dv >> 8) & 255) * ia + 127) / 255;
    uint32_t b = (((src >> 16) & 255) * a + ((dv >> 16) & 255) * ia + 127) / 255;
    *d = 0xFF000000u | (b << 16) | (g << 8) | r;
}

static void spanFill(Canvas* c, int y, int x0, int x1, Col col) {  // x1 exclusive, opaque or translucent
    if ((unsigned)y >= (unsigned)c->h) return;
    x0 = clampi(x0, 0, c->w);
    x1 = clampi(x1, 0, c->w);
    if (x1 <= x0) return;
    if (COL_ALPHA(col) >= 255) {
        uint32_t* d = &c->px[(size_t)y * c->stride];
        Col v = col | 0xFF000000u;
        for (int x = x0; x < x1; x++) d[x] = v;
    } else {
        for (int x = x0; x < x1; x++) blendPx(c, x, y, col, 255);
    }
}

Col gfxWithAlpha(Col c, float a) {
    int na = (int)(COL_ALPHA(c) * (a < 0 ? 0 : a > 1 ? 1 : a) + 0.5f);
    return (c & 0x00FFFFFFu) | ((uint32_t)na << 24);
}

Col gfxMix(Col a, Col b, float t) {
    float it = 1.0f - t;
    int r = (int)((a & 255) * it + (b & 255) * t + 0.5f);
    int g = (int)(((a >> 8) & 255) * it + ((b >> 8) & 255) * t + 0.5f);
    int bl = (int)(((a >> 16) & 255) * it + ((b >> 16) & 255) * t + 0.5f);
    return COL(r, g, bl);
}

void gfxFill(Canvas* c, Col col) {
    for (int y = 0; y < c->h; y++) spanFill(c, y, 0, c->w, col | 0xFF000000u);
}

void gfxCopy(Canvas* dst, const Canvas* src) {
    int h = dst->h < src->h ? dst->h : src->h, w = dst->w < src->w ? dst->w : src->w;
    for (int y = 0; y < h; y++) memcpy(&dst->px[(size_t)y * dst->stride], &src->px[(size_t)y * src->stride], (size_t)w * 4);
}

void gfxTint(Canvas* c, Col overlay) {
    for (int y = 0; y < c->h; y++) {
        uint32_t* d = &c->px[(size_t)y * c->stride];
        int a = COL_ALPHA(overlay), ia = 255 - a;
        int sr = overlay & 255, sg = (overlay >> 8) & 255, sb = (overlay >> 16) & 255;
        for (int x = 0; x < c->w; x++) {
            uint32_t v = d[x];
            uint32_t r = (sr * a + (v & 255) * ia + 127) / 255;
            uint32_t g = (sg * a + ((v >> 8) & 255) * ia + 127) / 255;
            uint32_t b = (sb * a + ((v >> 16) & 255) * ia + 127) / 255;
            d[x] = 0xFF000000u | (b << 16) | (g << 8) | r;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Shapes

// Signed distance to a rounded rectangle centred at (cx, cy) with half sizes (hw, hh) and corner radius r.
static inline float sdRRect(float px, float py, float cx, float cy, float hw, float hh, float r) {
    float qx = fabsf(px - cx) - (hw - r), qy = fabsf(py - cy) - (hh - r);
    float ox = maxf(qx, 0.0f), oy = maxf(qy, 0.0f);
    return sqrtf(ox * ox + oy * oy) + minf(maxf(qx, qy), 0.0f) - r;
}

void gfxRRect(Canvas* c, float x, float y, float w, float h, float r, Col col) {
    if (w <= 0 || h <= 0) return;
    r = minf(r, minf(w, h) * 0.5f);
    float cx = x + w * 0.5f, cy = y + h * 0.5f, hw = w * 0.5f, hh = h * 0.5f;
    int y0 = clampi((int)floorf(y) - 1, 0, c->h), y1 = clampi((int)ceilf(y + h) + 1, 0, c->h);
    int xa = (int)floorf(x) - 1, xb = (int)ceilf(x + w) + 1;
    for (int py = y0; py < y1; py++) {
        float fy = py + 0.5f;
        float qy = fabsf(fy - cy) - (hh - r);
        if (qy <= 0.0f && fy > y + 1.0f && fy < y + h - 1.0f) {
            // side band: only the left and right fringes need the distance function
            int ix0 = (int)ceilf(x), ix1 = (int)floorf(x + w);  // pixels [ix0, ix1) are fully covered
            for (int px = xa; px < ix0 + 0 && px < xb; px++) blendPx(c, px, py, col, covFromDist(sdRRect(px + 0.5f, fy, cx, cy, hw, hh, r)));
            spanFill(c, py, ix0, ix1, col);
            for (int px = ix1; px < xb; px++) blendPx(c, px, py, col, covFromDist(sdRRect(px + 0.5f, fy, cx, cy, hw, hh, r)));
        } else {
            for (int px = xa; px < xb; px++) blendPx(c, px, py, col, covFromDist(sdRRect(px + 0.5f, fy, cx, cy, hw, hh, r)));
        }
    }
}

void gfxRRectStroke(Canvas* c, float x, float y, float w, float h, float r, float sw, Col col) {
    if (w <= 0 || h <= 0) return;
    r = minf(r, minf(w, h) * 0.5f);
    float cx = x + w * 0.5f, cy = y + h * 0.5f, hw = w * 0.5f, hh = h * 0.5f;
    int y0 = clampi((int)floorf(y) - 1, 0, c->h), y1 = clampi((int)ceilf(y + h) + 1, 0, c->h);
    int xa = (int)floorf(x) - 1, xb = (int)ceilf(x + w) + 1;
    int band = (int)ceilf(sw) + 3;
    for (int py = y0; py < y1; py++) {
        float fy = py + 0.5f;
        bool mid = fy > y + sw + 1.5f && fy < y + h - sw - 1.5f && (fabsf(fy - cy) - (hh - r)) <= 0.0f;
        for (int px = xa; px < xb; px++) {
            if (mid && px == xa + band) px = xb - band;  // skip the empty middle of the row
            float d = sdRRect(px + 0.5f, fy, cx, cy, hw, hh, r);
            float dd = fabsf(d + sw * 0.5f) - sw * 0.5f;
            blendPx(c, px, py, col, covFromDist(dd));
        }
    }
}

void gfxCircle(Canvas* c, float cx, float cy, float r, Col col) {
    int y0 = clampi((int)floorf(cy - r) - 1, 0, c->h), y1 = clampi((int)ceilf(cy + r) + 1, 0, c->h);
    int xa = (int)floorf(cx - r) - 1, xb = (int)ceilf(cx + r) + 1;
    for (int py = y0; py < y1; py++) {
        float dy = py + 0.5f - cy;
        for (int px = xa; px < xb; px++) {
            float dx = px + 0.5f - cx;
            blendPx(c, px, py, col, covFromDist(sqrtf(dx * dx + dy * dy) - r));
        }
    }
}

void gfxRing(Canvas* c, float cx, float cy, float r, float sw, Col col) {
    float ro = r + sw * 0.5f;
    int y0 = clampi((int)floorf(cy - ro) - 1, 0, c->h), y1 = clampi((int)ceilf(cy + ro) + 1, 0, c->h);
    int xa = (int)floorf(cx - ro) - 1, xb = (int)ceilf(cx + ro) + 1;
    for (int py = y0; py < y1; py++) {
        float dy = py + 0.5f - cy;
        for (int px = xa; px < xb; px++) {
            float dx = px + 0.5f - cx;
            blendPx(c, px, py, col, covFromDist(fabsf(sqrtf(dx * dx + dy * dy) - r) - sw * 0.5f));
        }
    }
}

void gfxShadow(Canvas* c, float x, float y, float w, float h, float r, float blur, float dy, Col col) {
    const int N = 14;
    float a = COL_ALPHA(col) / 255.0f;
    float per = 1.0f - powf(1.0f - a, 1.0f / N);  // so the layers pile up to about `a` in the middle
    Col lc = gfxWithAlpha(col | 0xFF000000u, per);
    for (int i = N; i >= 1; i--) {
        float e = blur * (float)i / N;
        gfxRRect(c, x - e, y + dy - e, w + 2 * e, h + 2 * e, r + e, lc);
    }
}

static void capsule(Canvas* c, float x0, float y0, float x1, float y1, float w, Col col) {
    float hw = w * 0.5f;
    int ya = clampi((int)floorf(minf(y0, y1) - hw) - 1, 0, c->h), yb = clampi((int)ceilf(maxf(y0, y1) + hw) + 1, 0, c->h);
    int xa = (int)floorf(minf(x0, x1) - hw) - 1, xb = (int)ceilf(maxf(x0, x1) + hw) + 1;
    float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
    for (int py = ya; py < yb; py++) {
        float fy = py + 0.5f;
        for (int px = xa; px < xb; px++) {
            float fx = px + 0.5f;
            float t = len2 > 0 ? ((fx - x0) * dx + (fy - y0) * dy) / len2 : 0.0f;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            float qx = x0 + dx * t - fx, qy = y0 + dy * t - fy;
            blendPx(c, px, py, col, covFromDist(sqrtf(qx * qx + qy * qy) - hw));
        }
    }
}

void gfxPoly(Canvas* c, const float* xy, int n, float width, Col col) {
    for (int i = 1; i < n; i++) capsule(c, xy[2 * i - 2], xy[2 * i - 1], xy[2 * i], xy[2 * i + 1], width, col);
}

void gfxTriangle(Canvas* c, float x0, float y0, float x1, float y1, float x2, float y2, Col col) {
    float ax[3] = { x0, x1, x2 }, ay[3] = { y0, y1, y2 };
    // make the winding consistent so the inside is on one side of every edge
    float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    float s = area < 0 ? -1.0f : 1.0f;
    int ya = clampi((int)floorf(minf(y0, minf(y1, y2))) - 1, 0, c->h), yb = clampi((int)ceilf(maxf(y0, maxf(y1, y2))) + 1, 0, c->h);
    int xa = (int)floorf(minf(x0, minf(x1, x2))) - 1, xb = (int)ceilf(maxf(x0, maxf(x1, x2))) + 1;
    for (int py = ya; py < yb; py++) {
        for (int px = xa; px < xb; px++) {
            float fx = px + 0.5f, fy = py + 0.5f, d = -1e9f;
            for (int e = 0; e < 3; e++) {
                int f = (e + 1) % 3;
                float ex = ax[f] - ax[e], ey = ay[f] - ay[e], l = sqrtf(ex * ex + ey * ey);
                float dist = s * ((fx - ax[e]) * ey - (fy - ay[e]) * ex) / (l > 0 ? l : 1.0f);  // positive outside for this winding
                d = maxf(d, dist);
            }
            blendPx(c, px, py, col, covFromDist(d));
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Text

static stbtt_fontinfo g_font;
static bool           g_font_ok;

bool gfxFontInit(const void* ttf) {
    const unsigned char* d = ttf;
    int off = stbtt_GetFontOffsetForIndex(d, 0);
    g_font_ok = off >= 0 && stbtt_InitFont(&g_font, d, off) != 0;
    return g_font_ok;
}

typedef struct {
    uint32_t key;  // 0 = empty
    int      w, h, xoff, yoff;
    float    adv;
    uint8_t* bm;
} Glyph;

#define GLYPH_SLOTS 16384
static Glyph g_glyphs[GLYPH_SLOTS];

static float emboldenFor(int px, int weight) { return weight <= 0 ? 0.0f : px * (weight == 1 ? 0.010f : 0.026f); }

static const Glyph* glyphFor(uint32_t cp, int px, int weight, int phase) {
    uint32_t key = 0x80000000u | ((uint32_t)(phase & 3) << 26) | ((uint32_t)(weight & 3) << 24) | ((uint32_t)(px & 255) << 16) | (cp & 0xFFFF);
    uint32_t i = (key * 2654435761u) >> 18;
    for (int probe = 0; probe < GLYPH_SLOTS; probe++, i++) {
        Glyph* g = &g_glyphs[i % GLYPH_SLOTS];
        if (g->key == key) return g;
        if (g->key != 0) continue;
        // build it
        float sc = stbtt_ScaleForPixelHeight(&g_font, (float)px), e = emboldenFor(px, weight);
        int adv = 0, lsb = 0, x0, y0, x1, y1;
        stbtt_GetCodepointHMetrics(&g_font, (int)cp, &adv, &lsb);
        stbtt_GetCodepointBitmapBox(&g_font, (int)cp, sc, sc, &x0, &y0, &x1, &y1);
        int extra = (int)ceilf(e) + 2;
        int bw = (x1 - x0) + extra + 3, bh = (y1 - y0) + 2;
        if (bw < 1) bw = 1;
        if (bh < 1) bh = 1;
        g->key = key;
        g->w = bw; g->h = bh; g->xoff = x0; g->yoff = y0;
        g->adv = adv * sc + e * 0.6f;
        g->bm = calloc((size_t)bw * bh, 1);
        if (g->bm && x1 > x0 && y1 > y0) {
            int layers = e > 0.01f ? 3 : 1;
            for (int l = 0; l < layers; l++) {
                // stb places a shifted bitmap by its own bounding box, so ask for that box and offset by it
                float sh = (layers > 1 ? e * l / (layers - 1) : 0.0f) + phase * 0.25f;
                int lx0, ly0, lx1, ly1;
                stbtt_GetCodepointBitmapBoxSubpixel(&g_font, (int)cp, sc, sc, sh, 0.0f, &lx0, &ly0, &lx1, &ly1);
                int lw = lx1 - lx0, lh = ly1 - ly0;
                if (lw <= 0 || lh <= 0) continue;
                uint8_t* tmp = calloc((size_t)lw * lh, 1);
                if (!tmp) continue;
                stbtt_MakeCodepointBitmapSubpixel(&g_font, tmp, lw, lh, lw, sc, sc, sh, 0.0f, (int)cp);
                int ox = lx0 - x0, oy = ly0 - y0;
                for (int yy = 0; yy < lh; yy++) {
                    int dy = yy + oy;
                    if (dy < 0 || dy >= bh) continue;
                    for (int xx = 0; xx < lw; xx++) {
                        int dx = xx + ox;
                        if (dx < 0 || dx >= bw) continue;
                        uint8_t v = tmp[yy * lw + xx];
                        if (v > g->bm[dy * bw + dx]) g->bm[dy * bw + dx] = v;
                    }
                }
                free(tmp);
            }
        }
        return g;
    }
    return NULL;
}

static uint32_t nextCp(const char** s) {
    const unsigned char* p = (const unsigned char*)*s;
    uint32_t c = *p;
    if (c < 0x80) { (*s)++; return c; }
    if ((c & 0xE0) == 0xC0 && p[1]) { *s += 2; return ((c & 0x1F) << 6) | (p[1] & 0x3F); }
    if ((c & 0xF0) == 0xE0 && p[1] && p[2]) { *s += 3; return ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); }
    (*s)++;
    return '?';
}

static const TextStyle k_plain = { 0, 0.0f, false };

float gfxTextWidth(const char* s, int px, const TextStyle* st) {
    if (!g_font_ok) return 0;
    if (!st) st = &k_plain;
    float w = 0;
    int n = 0;
    while (*s) {
        uint32_t cp = nextCp(&s);
        const Glyph* gw = st->mono ? NULL : glyphFor(cp, px, st->weight, 0);
        w += st->mono ? px * 0.6f : (gw ? gw->adv : 0);
        w += st->spacing;
        n++;
    }
    return n ? w - st->spacing : 0;  // no spacing after the last character
}

void gfxText(Canvas* c, float x, float y, const char* s, int px, Col col, const TextStyle* st) {
    if (!g_font_ok) return;
    if (!st) st = &k_plain;
    while (*s) {
        uint32_t cp = nextCp(&s);
        const Glyph* g0 = glyphFor(cp, px, st->weight, 0);
        if (!g0) continue;
        float adv = st->mono ? px * 0.6f : g0->adv;
        float gx = st->mono ? x + (adv - g0->adv) * 0.5f : x;
        int ix = (int)floorf(gx), ph = (int)((gx - ix) * 4.0f + 0.5f);
        if (ph >= 4) { ix++; ph = 0; }
        const Glyph* g = ph ? glyphFor(cp, px, st->weight, ph) : g0;
        if (!g) g = g0;
        int ox = ix + g->xoff, oy = (int)floorf(y + 0.5f) + g->yoff;
        if (g->bm) {
            for (int yy = 0; yy < g->h; yy++) {
                int ty = oy + yy;
                if ((unsigned)ty >= (unsigned)c->h) continue;
                for (int xx = 0; xx < g->w; xx++) {
                    int v = g->bm[yy * g->w + xx];
                    if (v) blendPx(c, ox + xx, ty, col, v);
                }
            }
        }
        x += adv + st->spacing;
    }
}

void gfxTextC(Canvas* c, float cx, float y, const char* s, int px, Col col, const TextStyle* st) {
    gfxText(c, cx - gfxTextWidth(s, px, st) * 0.5f, y, s, px, col, st);
}

void gfxTextR(Canvas* c, float rx, float y, const char* s, int px, Col col, const TextStyle* st) {
    gfxText(c, rx - gfxTextWidth(s, px, st), y, s, px, col, st);
}

int gfxFontAscent(int px) {
    if (!g_font_ok) return px;
    int a, d, g;
    stbtt_GetFontVMetrics(&g_font, &a, &d, &g);
    return (int)(a * stbtt_ScaleForPixelHeight(&g_font, (float)px) + 0.5f);
}

// Splits s into lines no wider than maxw; calls emit(line, len) for each.
static int wrapLines(const char* s, float maxw, int px, const TextStyle* st, Canvas* c, float x, float y, float lineh, Col col) {
    char line[256];
    int lines = 0, len = 0;
    const char* p = s;
    while (*p) {
        const char* w = p;
        while (*w && *w != ' ') w++;
        int wl = (int)(w - p);
        char trial[256];
        int tl = len ? len + 1 + wl : wl;
        if (tl >= (int)sizeof(trial) - 1) break;
        if (len) { memcpy(trial, line, (size_t)len); trial[len] = ' '; memcpy(trial + len + 1, p, (size_t)wl); }
        else memcpy(trial, p, (size_t)wl);
        trial[tl] = 0;
        if (len && gfxTextWidth(trial, px, st) > maxw) {
            line[len] = 0;
            if (c) gfxText(c, x, y + lines * lineh, line, px, col, st);
            lines++;
            len = 0;
            continue;  // retry this word on a fresh line
        }
        memcpy(line, trial, (size_t)tl + 1);
        len = tl;
        p = *w ? w + 1 : w;
    }
    if (len) {
        line[len] = 0;
        if (c) gfxText(c, x, y + lines * lineh, line, px, col, st);
        lines++;
    }
    return lines;
}

int gfxTextWrap(Canvas* c, float x, float y, float maxw, float lineh, const char* s, int px, Col col, const TextStyle* st) {
    return wrapLines(s, maxw, px, st ? st : &k_plain, c, x, y, lineh, col);
}

int gfxTextWrapCount(float maxw, const char* s, int px, const TextStyle* st) {
    return wrapLines(s, maxw, px, st ? st : &k_plain, NULL, 0, 0, 0, 0);
}

// ---------------------------------------------------------------------------------------------
// Blur

static void boxPass(uint8_t* buf, int w, int h, int ch, int rad, bool horiz) {
    int n = horiz ? w : h, lines = horiz ? h : w;
    uint8_t* tmp = malloc((size_t)n * ch);
    if (!tmp) return;
    for (int l = 0; l < lines; l++) {
        for (int k = 0; k < ch; k++) {
            int sum = 0, cnt = 0;
            for (int i = -rad; i <= rad; i++) { int j = clampi(i, 0, n - 1); sum += horiz ? buf[((size_t)l * w + j) * ch + k] : buf[((size_t)j * w + l) * ch + k]; cnt++; }
            for (int i = 0; i < n; i++) {
                tmp[i * ch + k] = (uint8_t)(sum / cnt);
                int add = clampi(i + rad + 1, 0, n - 1), sub = clampi(i - rad, 0, n - 1);
                sum += (horiz ? buf[((size_t)l * w + add) * ch + k] : buf[((size_t)add * w + l) * ch + k]);
                sum -= (horiz ? buf[((size_t)l * w + sub) * ch + k] : buf[((size_t)sub * w + l) * ch + k]);
            }
        }
        for (int i = 0; i < n; i++)
            for (int k = 0; k < ch; k++) { if (horiz) buf[((size_t)l * w + i) * ch + k] = tmp[i * ch + k]; else buf[((size_t)i * w + l) * ch + k] = tmp[i * ch + k]; }
    }
    free(tmp);
}

void gfxBlurCopy(Canvas* dst, const Canvas* src) {
    const int F = 4;
    int sw = src->w / F, sh = src->h / F;
    uint8_t* small = malloc((size_t)sw * sh * 3);
    if (!small) { gfxCopy(dst, src); return; }
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++) {
            int r = 0, g = 0, b = 0;
            for (int dy = 0; dy < F; dy++)
                for (int dx = 0; dx < F; dx++) {
                    uint32_t v = src->px[(size_t)(y * F + dy) * src->stride + x * F + dx];
                    r += v & 255; g += (v >> 8) & 255; b += (v >> 16) & 255;
                }
            small[((size_t)y * sw + x) * 3 + 0] = (uint8_t)(r / (F * F));
            small[((size_t)y * sw + x) * 3 + 1] = (uint8_t)(g / (F * F));
            small[((size_t)y * sw + x) * 3 + 2] = (uint8_t)(b / (F * F));
        }
    for (int i = 0; i < 2; i++) { boxPass(small, sw, sh, 3, 2, true); boxPass(small, sw, sh, 3, 2, false); }
    for (int y = 0; y < dst->h && y < src->h; y++) {
        float fy = (y + 0.5f) / F - 0.5f;
        int y0 = clampi((int)floorf(fy), 0, sh - 1), y1 = clampi(y0 + 1, 0, sh - 1);
        float ty = fy - floorf(fy);
        ty = ty < 0 ? 0 : ty;
        for (int x = 0; x < dst->w && x < src->w; x++) {
            float fx = (x + 0.5f) / F - 0.5f;
            int x0 = clampi((int)floorf(fx), 0, sw - 1), x1 = clampi(x0 + 1, 0, sw - 1);
            float tx = fx - floorf(fx);
            tx = tx < 0 ? 0 : tx;
            int ch[3];
            for (int k = 0; k < 3; k++) {
                float a = small[((size_t)y0 * sw + x0) * 3 + k] * (1 - tx) + small[((size_t)y0 * sw + x1) * 3 + k] * tx;
                float b = small[((size_t)y1 * sw + x0) * 3 + k] * (1 - tx) + small[((size_t)y1 * sw + x1) * 3 + k] * tx;
                ch[k] = (int)(a * (1 - ty) + b * ty + 0.5f);
            }
            dst->px[(size_t)y * dst->stride + x] = COL(ch[0], ch[1], ch[2]);
        }
    }
    free(small);
}
