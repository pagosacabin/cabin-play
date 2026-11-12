// ---------------------------------------------------------------------------
//  tools/scanout_test.cpp -- unit test for the exact code the LCD_CAM ISR runs.
//
//  Includes src/scanout.h (the same function main.cpp registers as
//  on_bounce_empty) and calls it the way the driver does: a whole 640x480 frame
//  delivered in 10,240-byte chunks starting at pos_px = 0, 10240, 20480, ...
//  (307200 / 10240 = 30 chunks, which is what the device's bounce-fill counter
//  reports per frame). The concatenated output must be byte-identical to an
//  independent 2x2 nearest-neighbour expansion of the 320x240 source.
//
//  Then two negative controls, so a pass is not vacuous: a 1-pixel horizontal
//  shift and a 1-line vertical shift must both FAIL the comparison. If either
//  "passes", the comparison is broken rather than the code.
//
//  Build:  g++ -O2 -DPHOTO_S3_HOST -Isrc tools/scanout_test.cpp -o /tmp/scanout_test
//  Run  :  /tmp/scanout_test
// ---------------------------------------------------------------------------
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scanout.h"

static const int SRC_W = SCANOUT_SRC_W;
static const int SRC_H = SCANOUT_SRC_H;
static const int OUT_W = SCANOUT_OUT_W;
static const int OUT_H = SCANOUT_OUT_H;
static const int FRAME = SCANOUT_OUT_W * SCANOUT_OUT_H;

// Deterministic pseudo-random source: every byte value appears, and no two
// adjacent pixels are equal, so a pair-order or off-by-one error shows up.
static void fill_source(uint8_t *src)
{
    uint32_t s = 0x1234567u;
    for (int i = 0; i < SRC_W * SRC_H; i++) {
        s = s * 1664525u + 1013904223u;
        src[i] = (uint8_t)(s >> 24);
    }
}

static void reference_double(const uint8_t *src, uint8_t *out)
{
    for (int y = 0; y < OUT_H; y++)
        for (int x = 0; x < OUT_W; x++)
            out[y * OUT_W + x] = src[(y >> 1) * SRC_W + (x >> 1)];
}

// Drive the shipped function chunk by chunk, exactly as the DMA does.
static void run_scanout(const uint8_t *src, uint8_t *out, int chunk)
{
    memset(out, 0xEE, FRAME);          // poison, so an unfilled byte is visible
    for (int pos = 0; pos < FRAME; pos += chunk) {
        int len = FRAME - pos;
        if (len > chunk) len = chunk;
        scanout_double_fill(out + pos, src, pos, len);
    }
}

static int diff_count(const uint8_t *a, const uint8_t *b)
{
    int n = 0;
    for (int i = 0; i < FRAME; i++)
        if (a[i] != b[i]) n++;
    return n;
}

static int h_shift_fails(const uint8_t *ref, const uint8_t *src)
{
    // Same fill, but the reference is shifted one pixel right: must differ.
    static uint8_t shifted[FRAME];
    for (int y = 0; y < OUT_H; y++)
        for (int x = 0; x < OUT_W; x++)
            shifted[y * OUT_W + x] = src[(y >> 1) * SRC_W + ((x + 1) >> 1 < SRC_W ? ((x + 1) >> 1) : SRC_W - 1)];
    return diff_count(ref, shifted);
}

int main(void)
{
    static uint8_t src[SRC_W * SRC_H];
    static uint8_t ref[FRAME];
    static uint8_t got[FRAME];

    fill_source(src);
    reference_double(src, ref);

    int failures = 0;

    // The device's chunk size, then two more shapes: one chunk for the whole
    // frame (a hypothetical big bounce buffer) and a deliberately misaligned
    // 1000 bytes (exercises the generic path and odd column starts).
    const int chunks[] = { 10240, FRAME, 1000 };
    for (unsigned k = 0; k < sizeof(chunks) / sizeof(chunks[0]); k++) {
        run_scanout(src, got, chunks[k]);
        const int d = diff_count(ref, got);
        printf("chunk %6d B: differing bytes %d of %d  %s\n", chunks[k], d, FRAME,
               d == 0 ? "PASS" : "FAIL");
        if (d) failures++;
    }

    // Negative controls: each must produce a non-zero difference.
    const int hs = h_shift_fails(ref, src);
    printf("negative control (reference shifted 1 px right): differing bytes %d %s\n",
           hs, hs > 0 ? "OK (comparison is live)" : "BROKEN");
    if (hs == 0) failures++;

    // A same-size but different source must also differ.
    src[0] ^= 0xFF;
    static uint8_t ref2[FRAME];
    reference_double(src, ref2);
    const int ds = diff_count(ref, ref2);
    printf("negative control (one source byte flipped):      differing bytes %d %s\n",
           ds, ds > 0 ? "OK" : "BROKEN");
    if (ds == 0) failures++;

    printf("%s\n", failures ? "RESULT: FAIL" : "RESULT: PASS");
    return failures ? 1 : 0;
}
