// ---------------------------------------------------------------------------
//  photo_render64.h -- JPEG -> 64-color VGA pixel stream (2 bits per channel),
//  shared by the firmware (src/main.cpp) and by the host-side check in tools/.
//
//  This file has no Arduino/ESP32 dependencies on purpose: the same source is
//  compiled into the firmware and into tools/render_check.cpp on the host, so
//  the decode path, the level decision and the dither are the same *code* in
//  both rather than two implementations that could drift.
//
//  WHAT "64 COLORS" MEANS HERE. Two bits per channel is four levels per
//  channel, 4^3 = 64 combinations, held in one byte as bit pairs
//  (R1R0 G1G0 B1B0 -- R1/G1/B1 are the high, "heavy" bits). The levels are not
//  evenly spaced, because the DAC is a plain resistor ladder, not a
//  thermometer DAC: each channel has a 270 ohm path (heavy bit) and a 470 ohm
//  path (weak bit) in parallel into the monitor's 75 ohm termination. The
//  fractions of full scale it produces are approx 0.00, 0.45, 0.71, 1.00
//  rather than 0.00, 0.33, 0.67, 1.00.
//
//  TWO QUANTISER MODES (round 2, 2026-11-12). Round 1 dithered in the VOLTAGE
//  domain: a pixel wanted "s of full scale" and the dither mixed levels whose
//  *voltages* average to s. A display is not linear in voltage -- an LCD or CRT
//  fed VGA maps voltage to luminance as approx V^gamma -- so dithering to the
//  average *voltage* displays the average *light* of s^gamma, i.e. everything
//  comes out too bright, and shadows are the worst case: the ladder's first
//  step is 0.452 of full scale, which is sRGB 115 once the panel's gamma is
//  applied, so a dark pixel was rendered as a spray of mid-grey dots. That is
//  the "speckled black background" Stephen saw.
//
//    PHOTO64_MODE_LEGACY      -- round 1's behaviour, voltage-domain dither.
//                                Kept so the same build can A/B the change.
//    PHOTO64_MODE_PERCEPTUAL  -- light-domain dither: the source byte is
//                                gamma-decoded to luminance first, and the
//                                dither mixes levels so the average *luminance*
//                                matches. Same 4 levels, same hardware; shadow
//                                dots become ~4x sparser and midtones stop
//                                reading bright.
//
//  GAMMA 2.2 is the display model, not a measurement of Stephen's panel (the
//  213T is an LCD driven by an analogue VGA input; sRGB-equivalent 2.2 is the
//  standard assumption). It is one constant here, so the model is stated and
//  reversible rather than buried.
//
//  Dither: 8x8 ordered (Bayer) in whichever domain the mode selects. Four
//  levels per channel with no dither bands a photo into 64 flat patches; with
//  dither the 76,800 pixels carry gradients the eye integrates. The census
//  printed on the device (and by the host check) is the honest count of the 64
//  palette entries the frame actually uses.
// ---------------------------------------------------------------------------
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <tjpgd.h>

// --- the hardware ladder ----------------------------------------------------
// Stephen's bench measurements (2026-11-12): 270 ohm = heavy bit, 470 ohm =
// weak bit, per channel, both paths joining at the VGA pin; 75 ohm is the
// monitor's termination. Kept as resistors rather than magic voltages so the
// derivation is visible and checkable.
#define DAC_R_HEAVY 270.0f
#define DAC_R_WEAK  470.0f
#define DAC_R_TERM   75.0f

// Display model: luminance = voltage^PHOTO64_GAMMA. Standard sRGB-ish 2.2.
#define PHOTO64_GAMMA 2.2f

// Quantiser modes.
#define PHOTO64_MODE_LEGACY     0   // round 1: voltage-domain dither
#define PHOTO64_MODE_PERCEPTUAL 1   // round 2: light-domain (gamma) dither

// Where each channel samples the matrix. Giving all three channels the SAME
// threshold correlates them: in a smooth region all three step up or down
// together, so the frame collapses onto the grey diagonal and uses only a
// fraction of the 64 entries. Offsetting each channel's sample point
// decorrelates them, which both widens the palette the frame actually uses and
// stops the dither from pulling hue towards grey.
static const uint8_t BAYER_DX[3] = {0, 3, 6};
static const uint8_t BAYER_DY[3] = {0, 5, 2};

// 8x8 ordered dither, 64 distinct thresholds 0..63 in Bayer order.
static const uint8_t BAYER8[8][8] = {
    { 0, 32,  8, 40,  2, 34, 10, 42},
    {48, 16, 56, 24, 50, 18, 58, 26},
    {12, 44,  4, 36, 14, 46,  6, 38},
    {60, 28, 52, 20, 62, 30, 54, 22},
    { 3, 35, 11, 43,  1, 33,  9, 41},
    {51, 19, 59, 27, 49, 17, 57, 25},
    {15, 47,  7, 39, 13, 45,  5, 37},
    {63, 31, 55, 23, 61, 29, 53, 21}
};

// Everything the decode needs to know about the display.
typedef struct {
    int screenW;
    int screenH;
    int scaleShift;     // TJpgDec descale: 0 = 1/1, 1 = 1/2, 2 = 1/4, 3 = 1/8
    int mode;           // PHOTO64_MODE_LEGACY | PHOTO64_MODE_PERCEPTUAL
} Photo64Config;

// Pixel sink. q* are 0..3 (the level each channel's two bits select) and
// src points at the three source bytes that produced them, so a caller can
// measure quantisation error without decoding twice.
typedef void (*PutPixel64Fn)(void *ctx, int x, int y,
                             int qr, int qg, int qb, const uint8_t *src);

typedef struct {
    const uint8_t *p;
    size_t         left;
} MemStream64;

// One device block: TJpgDec hands BOTH callbacks the same jd->device pointer,
// so the stream and the sink must travel together (the split-struct version
// silently drops every pixel while jd_decomp still returns JDR_OK).
typedef struct {
    MemStream64    stream;
    PutPixel64Fn   put;
    void          *ctx;
    Photo64Config  cfg;
    uint32_t       pixelsDrawn;
} RenderTarget64;

// Fraction of full scale each level produces, [0]=000, [1]=weak bit only,
// [2]=heavy bit only, [3]=both. Full scale is the both-bits-on voltage.
static inline void photo64Ladder(float out[4])
{
    const float v[4] = {
        0.0f,
        DAC_R_TERM / (DAC_R_WEAK  + DAC_R_TERM),                                         // weak only
        DAC_R_TERM / (DAC_R_HEAVY + DAC_R_TERM),                                         // heavy only
        DAC_R_TERM / ((DAC_R_HEAVY * DAC_R_WEAK) / (DAC_R_HEAVY + DAC_R_WEAK) + DAC_R_TERM) // both
    };
    const float vmax = v[3];
    for (int i = 0; i < 4; i++) out[i] = v[i] / vmax;
}

// Position of target value t within the four levels, in 1/256ths of a level
// step (0 .. 3*256). Shared by both domains; only `lev` changes.
static inline float photo64Pos(float t, const float lev[4])
{
    float j;
    if (t <= lev[1])      j = (t - lev[0]) / (lev[1] - lev[0]);
    else if (t <= lev[2]) j = 1.0f + (t - lev[1]) / (lev[2] - lev[1]);
    else                  j = 2.0f + (t - lev[2]) / (lev[3] - lev[2]);
    if (j < 0.0f) j = 0.0f;
    if (j > 3.0f) j = 3.0f;
    return j;
}

// Tables: [mode][source byte] -> level position in 1/256ths (0 .. 3*256).
static uint16_t s_qtab[2][256];

static inline void photo64Init(void)
{
    float V[4], L[4];
    photo64Ladder(V);
    for (int i = 0; i < 4; i++) L[i] = (i == 0) ? 0.0f : powf(V[i], PHOTO64_GAMMA);

    for (int i = 0; i < 256; i++) {
        const float s = i / 255.0f;                       // wanted fraction (sRGB code)
        const float sLin = (i == 0) ? 0.0f : powf(s, PHOTO64_GAMMA);  // wanted luminance
        s_qtab[PHOTO64_MODE_LEGACY][i]     = (uint16_t)(photo64Pos(s,    V) * 256.0f + 0.5f);
        s_qtab[PHOTO64_MODE_PERCEPTUAL][i] = (uint16_t)(photo64Pos(sLin, L) * 256.0f + 0.5f);
    }
}

// Quantise one channel: continuous position + ordered threshold, then floor.
// Threshold spans 0..252 in 1/256ths, i.e. effectively uniform over [0,1).
static inline int photo64Quant(uint8_t src, uint8_t bayer, int mode)
{
    const uint16_t *tab = s_qtab[mode == PHOTO64_MODE_PERCEPTUAL
                                 ? PHOTO64_MODE_PERCEPTUAL : PHOTO64_MODE_LEGACY];
    int q = (tab[src] + (int)bayer * 4) >> 8;
    if (q > 3) q = 3;
    if (q < 0) q = 0;
    return q;
}

// sRGB code each level displays (0, 115, 182, 255 for this ladder) and the
// luminance fraction behind it (0, 0.18, 0.48, 1.00). The first step of this
// ladder is therefore *mid-grey*, not a dark grey -- the fact that explains
// "speckled black": any shadow detail has to be drawn as an average of black
// and mid-grey dots. Quoted by the ramp screen and the serial log so the claim
// is checkable rather than asserted.
static inline void photo64LevelBytes(uint8_t out[4])
{
    float V[4];
    photo64Ladder(V);
    for (int i = 0; i < 4; i++) out[i] = (uint8_t)(V[i] * 255.0f + 0.5f);
}

static inline void photo64LevelLuminance(uint8_t out[4])
{
    float V[4];
    photo64Ladder(V);
    for (int i = 0; i < 4; i++)
        out[i] = (uint8_t)((i == 0 ? 0.0f : powf(V[i], PHOTO64_GAMMA)) * 255.0f + 0.5f);
}

static inline size_t photo64InFunc(JDEC *jd, uint8_t *buff, size_t nbyte)
{
    MemStream64 *s = &((RenderTarget64 *)jd->device)->stream;
    if (nbyte > s->left) nbyte = s->left;
    if (buff) memcpy(buff, s->p, nbyte);
    s->p    += nbyte;
    s->left -= nbyte;
    return nbyte;
}

// TJpgDec output callback: one MCU block of RGB888 -> 2-bit-per-channel pixels.
static inline int photo64OutFunc(JDEC *jd, void *bitmap, JRECT *rect)
{
    RenderTarget64 *t   = (RenderTarget64 *)jd->device;
    const uint8_t  *src = (const uint8_t *)bitmap;
    const int w = rect->right  - rect->left + 1;
    const int h = rect->bottom - rect->top  + 1;
    const int mode = t->cfg.mode;

    for (int j = 0; j < h; j++) {
        const int y = rect->top + j;
        for (int i = 0; i < w; i++) {
            const int x = rect->left + i;
            const uint8_t *p = src;          // r,g,b triple, advanced below
            const uint8_t r = p[0], g = p[1], b = p[2];
            src += 3;
            if (x >= t->cfg.screenW || y >= t->cfg.screenH) continue;
            const uint8_t thr = BAYER8[(y + BAYER_DY[0]) & 7][(x + BAYER_DX[0]) & 7];
            const uint8_t thg = BAYER8[(y + BAYER_DY[1]) & 7][(x + BAYER_DX[1]) & 7];
            const uint8_t thb = BAYER8[(y + BAYER_DY[2]) & 7][(x + BAYER_DX[2]) & 7];
            t->put(t->ctx, x, y,
                   photo64Quant(r, thr, mode),
                   photo64Quant(g, thg, mode),
                   photo64Quant(b, thb, mode),
                   p);
        }
    }
    t->pixelsDrawn += (uint32_t)w * (uint32_t)h;
    return 1;   // continue decoding
}

static inline const char *jdErrorString(JRESULT r)
{
    switch (r) {
        case JDR_OK:   return "OK";
        case JDR_INTR: return "interrupted by output function";
        case JDR_INP:  return "device error or wrong termination of input stream";
        case JDR_MEM1: return "insufficient memory pool for the image";
        case JDR_MEM2: return "insufficient stream input buffer";
        case JDR_PAR:  return "parameter error";
        case JDR_FMT1: return "data format error (may be broken data)";
        case JDR_FMT2: return "right format but not supported";
        case JDR_FMT3: return "not supported JPEG standard";
        default:       return "unknown";
    }
}
