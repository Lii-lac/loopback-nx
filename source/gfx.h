// Small software renderer: anti-aliased flat shapes and text on a 32-bit canvas. No platform dependencies, so the
// same code runs on the console and in the PC render tests.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Pixels are 0xAABBGGRR as a little-endian u32, which is the byte order R,G,B,A that the console framebuffer wants.
typedef uint32_t Col;
#define COLA(r, g, b, a) ((Col)(((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r)))
#define COL(r, g, b)     COLA(r, g, b, 255)
#define COL_HEX(h)       COL(((h) >> 16) & 255, ((h) >> 8) & 255, (h) & 255)
#define COL_ALPHA(c)     ((int)((c) >> 24))

typedef struct {
    uint32_t* px;
    int       w, h;
    int       stride;  // pixels per row
} Canvas;

// The font: TrueType data that stays valid for the life of the program. Returns false if it cannot be parsed.
bool gfxFontInit(const void* ttf);

Col  gfxWithAlpha(Col c, float a);              // c with its alpha scaled by a (0..1)
Col  gfxMix(Col a, Col b, float t);             // linear mix, opaque result
void gfxFill(Canvas* c, Col col);
void gfxCopy(Canvas* dst, const Canvas* src);
void gfxTint(Canvas* c, Col overlay);           // blend a translucent colour over the whole canvas

// Coordinates are in pixels; fractional values are anti-aliased.
void gfxRRect(Canvas* c, float x, float y, float w, float h, float r, Col col);
void gfxRRectStroke(Canvas* c, float x, float y, float w, float h, float r, float sw, Col col);  // stroke lies inside the edge
void gfxCircle(Canvas* c, float cx, float cy, float r, Col col);
void gfxRing(Canvas* c, float cx, float cy, float r, float sw, Col col);                         // sw is centred on radius r
void gfxPoly(Canvas* c, const float* xy, int n, float width, Col col);                            // open polyline, round joins and caps
// A soft drop shadow under a rounded rectangle: stacked translucent shapes, so it is meant to be drawn once into a cached
// background, not every frame. `blur` is how far it spreads, `dy` how far it is pushed down, col's alpha its strength.
void gfxShadow(Canvas* c, float x, float y, float w, float h, float r, float blur, float dy, Col col);
void gfxTriangle(Canvas* c, float x0, float y0, float x1, float y1, float x2, float y2, Col col);

typedef struct {
    int   weight;   // 0 regular, 1 medium, 2 bold
    float spacing;  // extra pixels after each character
    bool  mono;     // fixed 0.6 em cells
} TextStyle;

float gfxTextWidth(const char* s, int px, const TextStyle* st);
// x is the left edge (or centre / right edge), y the baseline.
void  gfxText(Canvas* c, float x, float y, const char* s, int px, Col col, const TextStyle* st);
void  gfxTextC(Canvas* c, float cx, float y, const char* s, int px, Col col, const TextStyle* st);
void  gfxTextR(Canvas* c, float rx, float y, const char* s, int px, Col col, const TextStyle* st);
// Word-wraps into maxw pixels. Returns the number of lines drawn.
int   gfxTextWrap(Canvas* c, float x, float y, float maxw, float lineh, const char* s, int px, Col col, const TextStyle* st);
int   gfxTextWrapCount(float maxw, const char* s, int px, const TextStyle* st);
int   gfxFontAscent(int px);

// dst = a blurred, quarter-size-and-back copy of src (same size). Cheap enough to do once when a dialog opens.
void gfxBlurCopy(Canvas* dst, const Canvas* src);
