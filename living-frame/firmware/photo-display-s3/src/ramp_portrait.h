// ---------------------------------------------------------------------------
//  ramp_portrait.h -- the diagnostic ramp, portrait-native, for rung 3b (0180).
//
//  Same diagnostic content as src/ramp_screen.h (rung 3a): per channel the four
//  ladder levels in the order 0 / LSB only / MSB only / both, a combined grey
//  ramp where a per-channel fault shows up as a tint, and a dithered
//  black->white gradient that shows what the quantiser does to a smooth
//  surface. What changes is the *canvas* and the *pixel unit*:
//
//    * canvas is 480x640 portrait, laid out for a monitor hanging portrait, so
//      every row still runs across the viewer's width (nothing is rotated on
//      the screen -- the whole 480x640 content is rotated once, below, into the
//      640x480 raster the LCD_CAM scans out);
//    * a pixel is a 4-bit PALETTE INDEX, not a raw (B<<4)|(G<<2)|R byte, since
//      the full-resolution mode's frame only has 16 colours per photo. The index
//      map is fixed and documented below so a photograph can be read against it.
//
//  PALETTE (the ramp phase's own 16 entries; printed at boot as well):
//     0  black        4  black            8  black
//     1  R LSB        5  G LSB            9  B LSB
//     2  R MSB        6  G MSB           10  B MSB
//     3  R both       7  G both          11  B both
//    12  grey LSB    13  grey MSB       14  grey both      15  black
//  Black is repeated three times so that a channel's four patches sit at
//  consecutive indices (base 0 / 4 / 8) and "level 0" is black in every row
//  without a branch -- the same reason the 64-colour packing put R in the low
//  bits.
//
//  ROTATION. A portrait canvas is written to the landscape raster with
//      raster(x, y) = canvas(x_c = y, y_c = PH-1-x)      (90 deg clockwise)
//  so the canvas's top-left pixel lands at the raster's top-right. The picture
//  therefore reads upright on a monitor pivoted 90 deg COUNTER-CLOCKWISE
//  (rotate-left). tools/render16.cpp writes both the raster and the
//  rotate-back view, and the header's own marker assertion in
//  tools/convert_images16.py pins the direction.
// ---------------------------------------------------------------------------
#pragma once

#include <stdint.h>

#include "ladder64.h"      // ladderQuant + BAYER8 + LADDER_MODE_PERCEPTUAL

#define RP_PW 480          // portrait canvas width
#define RP_PH 640          // portrait canvas height
#define RP_LW 640          // landscape raster width  (scanout)
#define RP_LH 480          // landscape raster height (scanout)

#define RP_I_BLACK 0
#define RP_I_GREY1 12
#define RP_I_GREY2 13
#define RP_I_GREY3 14
#define RP_I_WHITE 14      // (3,3,3) is both greys' top level: white

// Canvas geometry (portrait pixels). Rows run across the 480-px width.
static const int RP_ROW_Y[4]   = { 24, 144, 264, 384 };   // R, G, B, W
static const int RP_ROW_H      = 100;
static const int RP_LABEL_X    = 16;
static const int RP_PATCH_X0   = 72;
static const int RP_PATCH_PITCH = 100;
static const int RP_PATCH_W    = 90;
static const int RP_GRAD_Y     = 504;
static const int RP_GRAD_H     = 120;

typedef void (*RpPutFn)(void *ctx, int lx, int ly, uint8_t index);

// A 5x7 bitmap font, copied from src/ramp_screen.h (same glyphs, same rows) so
// a photograph of this ramp is read the same way as rung 3a's. Only the glyphs
// this screen draws are here.
struct RpGlyph { char c; uint8_t row[7]; };
static const RpGlyph RP_FONT[] = {
    { 'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11} },
    { 'G', {0x0E, 0x11, 0x10, 0x13, 0x11, 0x11, 0x0E} },
    { 'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E} },
    { 'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11} },
    { '0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E} },
    { '1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E} },
    { '2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F} },
    { '3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E} },
};

// Portrait canvas coordinates -> landscape raster coordinates (90 deg CW).
static inline int rpMapX(int xp, int yp) { (void)xp; return RP_LW - 1 - yp; }
static inline int rpMapY(int xp, int yp) { (void)yp; return xp; }

static inline void rpPut(RpPutFn put, void *ctx, int xp, int yp, uint8_t idx)
{
    if ((unsigned)xp >= RP_PW || (unsigned)yp >= RP_PH)
        return;
    put(ctx, rpMapX(xp, yp), rpMapY(xp, yp), idx);
}

static inline void rpFill(RpPutFn put, void *ctx, int x, int y, int w, int h, uint8_t idx)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            rpPut(put, ctx, x + i, y + j, idx);
}

static inline void rpChar(RpPutFn put, void *ctx, char ch, int x, int y, int scale, uint8_t idx)
{
    for (unsigned k = 0; k < sizeof(RP_FONT) / sizeof(RP_FONT[0]); k++) {
        const RpGlyph &g = RP_FONT[k];
        if (g.c != ch) continue;
        for (int r = 0; r < 7; r++)
            for (int c = 0; c < 5; c++)
                if (g.row[r] & (0x10 >> c))
                    rpFill(put, ctx, x + c * scale, y + r * scale, scale, scale, idx);
        return;
    }
}

// Grey level q (0..3) -> the palette index that renders it. The band is
// dithered by the *same* light-domain quantiser the photos use, with the same
// 8x8 Bayer matrix, so the gradient is a dither diagnostic and not a mock-up.
static inline uint8_t rpGreyIndex(int q)
{
    return q == 0 ? RP_I_BLACK : q == 1 ? RP_I_GREY1 : q == 2 ? RP_I_GREY2 : RP_I_GREY3;
}

static inline void rampPortraitDraw(RpPutFn put, void *ctx)
{
    rpFill(put, ctx, 0, 0, RP_PW, RP_PH, RP_I_BLACK);

    const char labels[3] = { 'R', 'G', 'B' };
    for (int ch = 0; ch < 3; ch++) {
        const int y = RP_ROW_Y[ch];
        const int base = ch * 4;                    // 0, 4, 8 -- level 0 = black
        rpChar(put, ctx, labels[ch], RP_LABEL_X, y + 36, 4, RP_I_WHITE);
        for (int q = 0; q < 4; q++)
            rpFill(put, ctx, RP_PATCH_X0 + q * RP_PATCH_PITCH, y,
                   RP_PATCH_W, RP_ROW_H, (uint8_t)(base + q));
        // The numeric level under each patch, so the photograph is self-labelling.
        for (int q = 0; q < 4; q++)
            rpChar(put, ctx, (char)('0' + q), RP_PATCH_X0 + q * RP_PATCH_PITCH + 38,
                   y + RP_ROW_H + 4, 3, RP_I_WHITE);
    }

    // Combined grey ramp: all three channels at the same level.
    rpChar(put, ctx, 'W', RP_LABEL_X, RP_ROW_Y[3] + 36, 4, RP_I_WHITE);
    for (int q = 0; q < 4; q++) {
        rpFill(put, ctx, RP_PATCH_X0 + q * RP_PATCH_PITCH, RP_ROW_Y[3],
               RP_PATCH_W, RP_ROW_H, rpGreyIndex(q));
        rpChar(put, ctx, (char)('0' + q), RP_PATCH_X0 + q * RP_PATCH_PITCH + 38,
               RP_ROW_Y[3] + RP_ROW_H + 4, 3, RP_I_WHITE);
    }

    // Smooth gradient: dithered in light against the 4 grey levels.
    for (int x = 0; x < RP_PW; x++) {
        const int v = (x * 255) / (RP_PW - 1);
        for (int y = 0; y < RP_GRAD_H; y++) {
            const int q = ladderQuant((uint8_t)v, BAYER8[y & 7][x & 7],
                                      LADDER_MODE_PERCEPTUAL);
            rpPut(put, ctx, x, RP_GRAD_Y + y, rpGreyIndex(q));
        }
    }
}
