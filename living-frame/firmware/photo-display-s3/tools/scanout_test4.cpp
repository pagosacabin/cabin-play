// ---------------------------------------------------------------------------
//  tools/scanout_test4.cpp -- unit test for the 4bpp (rung 3b) bounce fill.
//
//  Rung 3a's test (tools/scanout_test.cpp) covers the doubling path and is left
//  untouched. This one covers src/scanout.h's scanout_index4_fill: the exact
//  function the LCD_CAM ISR calls in PHOTO_MODE_FULL16, driven the way the
//  driver drives it -- a whole 640x480 frame in 10,240-byte chunks -- and
//  compared byte-for-byte against a naive per-pixel reference.
//
//  Chunk sizes chosen deliberately:
//    10240  the device's real chunk
//    307200 one chunk for the whole frame
//    1000   odd length -> every subsequent chunk starts on an ODD pixel, i.e.
//           it exercises the scalar path and the nibble-order rule there
//     999   same, and a chunk that also splits a source byte across two calls
//
//  Negative controls, so a pass cannot be vacuous: nibbles swapped within each
//  byte, and two palette entries exchanged, must both produce differences.
//
//  Build: g++ -O2 -DPHOTO_S3_HOST -Isrc tools/scanout_test4.cpp -o /tmp/scanout_test4
// ---------------------------------------------------------------------------
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "scanout.h"

static const int FRAME = SCANOUT_FULL_PX;                 // 307,200 output pixels
static const int SRC_BYTES = SCANOUT_FULL_PX / 2;         // 153,600 4bpp bytes

static void fill_source(uint8_t *src)
{
    uint32_t s = 0x9e3779b9u;
    for (int i = 0; i < SRC_BYTES; i++) {
        s = s * 1664525u + 1013904223u;
        src[i] = (uint8_t)(s >> 23);
    }
}

static void make_palette(uint8_t *pal)
{
    for (int i = 0; i < 16; i++)
        pal[i] = (uint8_t)(((i & 3) << 0) | (((i >> 2) & 3) << 2) | (((i >> 3) & 3) << 4));
    // Make sure no two entries are equal, so an index mix-up shows up.
    for (int i = 0; i < 16; i++)
        pal[i] = (uint8_t)(pal[i] ^ (i << 1));
}

static void reference(const uint8_t *src, const uint8_t *pal, uint8_t *out)
{
    for (int px = 0; px < FRAME; px++) {
        const uint8_t b = src[px >> 1];
        const int idx = (px & 1) ? (b >> 4) : (b & 0x0F);
        out[px] = pal[idx];
    }
}

static void run_scanout(const uint8_t *src, const uint16_t *expand, uint8_t *out, int chunk)
{
    memset(out, 0xEE, FRAME);
    for (int pos = 0; pos < FRAME; pos += chunk) {
        int len = FRAME - pos;
        if (len > chunk) len = chunk;
        scanout_index4_fill(out + pos, src, expand, pos, len);
    }
}

static int diff_count(const uint8_t *a, const uint8_t *b)
{
    int n = 0;
    for (int i = 0; i < FRAME; i++)
        if (a[i] != b[i]) n++;
    return n;
}

int main(void)
{
    static uint8_t src[SRC_BYTES];
    static uint8_t ref[FRAME];
    static uint8_t got[FRAME];
    static uint8_t pal[16];
    static uint16_t expand[256];

    fill_source(src);
    make_palette(pal);
    scanout_build_expand(expand, pal);
    reference(src, pal, ref);

    int failures = 0;

    // The expansion table itself: expand[b] must be pal[b&15] in the low byte and
    // pal[b>>4] in the high byte (little-endian store order -> even pixel first).
    int bad_entries = 0;
    for (int b = 0; b < 256; b++) {
        const uint16_t e = expand[b];
        if ((uint8_t)e != pal[b & 15] || (uint8_t)(e >> 8) != pal[(b >> 4) & 15])
            bad_entries++;
    }
    printf("expand table: %d bad entries of 256  %s\n", bad_entries,
           bad_entries ? "FAIL" : "PASS");
    if (bad_entries) failures++;

    const int chunks[] = { 10240, FRAME, 1000, 999, 2 };
    for (unsigned k = 0; k < sizeof(chunks) / sizeof(chunks[0]); k++) {
        run_scanout(src, expand, got, chunks[k]);
        const int d = diff_count(ref, got);
        printf("chunk %6d B: differing bytes %d of %d  %s\n", chunks[k], d, FRAME,
               d == 0 ? "PASS" : "FAIL");
        if (d) failures++;
    }

    // Negative control 1: nibble order swapped in the reference.
    static uint8_t swapped[FRAME];
    for (int px = 0; px < FRAME; px++) {
        const uint8_t b = src[px >> 1];
        swapped[px] = pal[(px & 1) ? (b & 0x0F) : (b >> 4)];
    }
    const int ds = diff_count(ref, swapped);
    printf("negative control (nibble order swapped): differing bytes %d %s\n", ds,
           ds > 0 ? "OK (comparison is live)" : "BROKEN");
    if (ds == 0) failures++;

    // Negative control 2: two palette entries exchanged.
    uint8_t pal2[16];
    memcpy(pal2, pal, sizeof(pal2));
    const uint8_t t = pal2[0];
    pal2[0] = pal2[1];
    pal2[1] = t;
    static uint8_t ref2[FRAME];
    reference(src, pal2, ref2);
    const int dp = diff_count(ref, ref2);
    printf("negative control (palette 0<->1 swapped): differing bytes %d %s\n", dp,
           dp > 0 ? "OK" : "BROKEN");
    if (dp == 0) failures++;

    printf("%s\n", failures ? "RESULT: FAIL" : "RESULT: PASS");
    return failures ? 1 : 0;
}
