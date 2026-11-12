// ---------------------------------------------------------------------------
//  ladder64.h -- the 2-bit-per-channel resistor-ladder model, extracted
//  VERBATIM (no value changed) from photo-display-64/src/photo_render64.h,
//  with only the TJpgDec dependency removed (this rung pre-converts images on
//  the host, so nothing here decodes JPEG on the device).
//
//  Provenance and the proof that the copy is faithful: tools/ramp_check.c in
//  this project re-renders the ramp with THIS header and its output is
//  byte-compared against photo-display-64/tools/ramp-preview.ppm, which was
//  produced by the original header. See README.md "Faithfulness of the copied
//  quantiser".
//
//  What it models, unchanged from rung 2:
//    * 270 ohm = heavy bit, 470 ohm = weak bit per channel into the monitor's
//      75 ohm termination -> levels 0.000 / 0.452 / 0.714 / 1.000 of full scale
//      (NOT 0/85/170/255 -- the ladder is not evenly spaced).
//    * Dither in LIGHT, not voltage: the wanted sRGB code is gamma-decoded and
//      the ordered dither mixes levels so the average LUMINANCE matches (the
//      0177 round-2 finding: voltage-domain dither made shadows speckle).
//    * 8x8 Bayer, each channel sampled at a different phase so the three
//      channels decorrelate and the frame uses the palette instead of the
//      grey diagonal.
// ---------------------------------------------------------------------------
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>

// --- the hardware ladder ----------------------------------------------------
#define DAC_R_HEAVY 270.0f
#define DAC_R_WEAK  470.0f
#define DAC_R_TERM   75.0f

// Display model: luminance = voltage^LADDER_GAMMA. Standard sRGB-ish 2.2.
#define LADDER_GAMMA 2.2f

#define LADDER_MODE_LEGACY     0   // round 1: voltage-domain dither
#define LADDER_MODE_PERCEPTUAL 1   // round 2: light-domain (gamma) dither

static const uint8_t BAYER_DX[3] = {0, 3, 6};
static const uint8_t BAYER_DY[3] = {0, 5, 2};

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

// --- compatibility names ---------------------------------------------------
// src/ramp_screen.h is a verbatim copy of rung 2's file, so it still spells the
// quantiser `photo64Quant` / `PHOTO64_MODE_PERCEPTUAL`. Rather than edit the
// copied drawing code, the old names are aliased here: the ramp header stays a
// byte-for-byte copy with only its include line changed, which is what makes
// the ramp-preview comparison in tools/host_render.c a real equality test.
#define PHOTO64_GAMMA           LADDER_GAMMA
#define PHOTO64_MODE_LEGACY     LADDER_MODE_LEGACY
#define PHOTO64_MODE_PERCEPTUAL LADDER_MODE_PERCEPTUAL
#define photo64Init             ladderInit
#define photo64Quant            ladderQuant
#define photo64Ladder           ladderLevels
#define photo64LevelBytes       ladderLevelBytes
#define photo64LevelLuminance   ladderLevelLuminance

// Fraction of full scale each level produces, [0]=000, [1]=weak bit only,
// [2]=heavy bit only, [3]=both.
static inline void ladderLevels(float out[4])
{
    const float v[4] = {
        0.0f,
        DAC_R_TERM / (DAC_R_WEAK  + DAC_R_TERM),
        DAC_R_TERM / (DAC_R_HEAVY + DAC_R_TERM),
        DAC_R_TERM / ((DAC_R_HEAVY * DAC_R_WEAK) / (DAC_R_HEAVY + DAC_R_WEAK) + DAC_R_TERM)
    };
    const float vmax = v[3];
    for (int i = 0; i < 4; i++) out[i] = v[i] / vmax;
}

// Position of target value t within the four levels, in 1/256ths of a level
// step (0 .. 3*256).
static inline float ladderPos(float t, const float lev[4])
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
// s_qtab is used ONLY at init (host preview + device boot); the bounce-buffer
// ISR never touches it, so nothing here needs to be in IRAM.
static uint16_t s_qtab[2][256];

static inline void ladderInit(void)
{
    float V[4], L[4];
    ladderLevels(V);
    for (int i = 0; i < 4; i++) L[i] = (i == 0) ? 0.0f : powf(V[i], LADDER_GAMMA);

    for (int i = 0; i < 256; i++) {
        const float s = i / 255.0f;
        const float sLin = (i == 0) ? 0.0f : powf(s, LADDER_GAMMA);
        s_qtab[LADDER_MODE_LEGACY][i]     = (uint16_t)(ladderPos(s,    V) * 256.0f + 0.5f);
        s_qtab[LADDER_MODE_PERCEPTUAL][i] = (uint16_t)(ladderPos(sLin, L) * 256.0f + 0.5f);
    }
}

static inline int ladderQuant(uint8_t src, uint8_t bayer, int mode)
{
    const uint16_t *tab = s_qtab[mode == LADDER_MODE_PERCEPTUAL
                                 ? LADDER_MODE_PERCEPTUAL : LADDER_MODE_LEGACY];
    int q = (tab[src] + (int)bayer * 4) >> 8;
    if (q > 3) q = 3;
    if (q < 0) q = 0;
    return q;
}

// sRGB code each level displays (0, 115, 182, 255 for this ladder).
static inline void ladderLevelBytes(uint8_t out[4])
{
    float V[4];
    ladderLevels(V);
    for (int i = 0; i < 4; i++) out[i] = (uint8_t)(V[i] * 255.0f + 0.5f);
}

static inline void ladderLevelLuminance(uint8_t out[4])
{
    float V[4];
    ladderLevels(V);
    for (int i = 0; i < 4; i++)
        out[i] = (uint8_t)((i == 0 ? 0.0f : powf(V[i], LADDER_GAMMA)) * 255.0f + 0.5f);
}
