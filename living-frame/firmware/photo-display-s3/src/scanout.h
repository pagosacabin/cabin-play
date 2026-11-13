// ---------------------------------------------------------------------------
//  scanout.h -- the pixel-doubling bounce-buffer fill, shared by the firmware
//  (src/main.cpp, called from the LCD_CAM DMA ISR) and by the host unit test
//  (tools/scanout_test.cpp).
//
//  Why it is a separate header: this routine is the one piece of the driver
//  that can be wrong in a way nothing else catches. A mistake here (an odd
//  column, an off-by-one line, the pair order in the 32-bit store) produces a
//  picture that is shifted, torn or colour-swapped, and neither the build nor
//  the RAM/heap figures nor the VSYNC counters would show it. Extracting it
//  lets the host test drive the exact same code over a whole frame and compare
//  its output byte-for-byte with an independent 2x2 nearest-neighbour
//  expansion.
//
//  Contract, as the esp_lcd RGB driver defines it: fill `len_bytes` bytes at
//  `out`, for the frame pixel position `pos_px` (pixels already sent this
//  frame, counting the 640-wide *output* raster). Chunks are always h_res
//  aligned in practice (the bounce buffer is a multiple of 640 px), but the
//  generic path handles any start position.
// ---------------------------------------------------------------------------
#pragma once

#include <stdint.h>

#ifdef PHOTO_S3_HOST
#define SCANOUT_FN static inline
#else
#include "esp_attr.h"
#define SCANOUT_FN static inline IRAM_ATTR
#endif

#define SCANOUT_SRC_W 320
#define SCANOUT_SRC_H 240
#define SCANOUT_OUT_W 640
#define SCANOUT_OUT_H 480

// One source pixel -> a 2x2 block of output pixels: x doubled by the byte
// expansion, y doubled by reading source line (line >> 1).
SCANOUT_FN void scanout_double_fill(uint8_t *out, const uint8_t *src,
                                    int pos_px, int len_bytes)
{
    int left = len_bytes;
    int px = pos_px;

    while (left > 0) {
        const int line = px / SCANOUT_OUT_W;          // destination scan line
        const int col  = px - line * SCANOUT_OUT_W;   // destination column
        const int sy   = line >> 1;                   // vertical doubling
        const uint8_t *row = src + (size_t)sy * SCANOUT_SRC_W;
        int n = SCANOUT_OUT_W - col;
        if (n > left) n = left;

        int i = 0;
        if ((col & 1) == 0) {
            // Two source pixels -> four output bytes, one aligned 32-bit store.
            for (; i + 4 <= n; i += 4) {
                const uint8_t a = row[(col + i) >> 1];
                const uint8_t b = row[(col + i + 2) >> 1];
                // Each source byte becomes two adjacent output bytes:
                //   w = [a a b b]  (little-endian store order).
                // NB: the multiplier is 0x0101, not 0x00010001 -- the latter
                // duplicates into the *upper half* and leaves every second
                // output byte zero (caught by tools/scanout_test.cpp).
                uint32_t w = (uint32_t)a * 0x0101u;
                w |= ((uint32_t)b * 0x0101u) << 16;
                *(uint32_t *)(out + i) = w;
            }
        }
        for (; i < n; i++)
            out[i] = row[(col + i) >> 1];

        out  += n;
        px   += n;
        left -= n;
    }
}


// ---------------------------------------------------------------------------
//  Rung 3b (task 0180): full-resolution 4-bit-per-pixel scanout.
//
//  The 640x480 frame is 153,600 B at 4 bpp, so it fits internal RAM with no
//  doubling: every output pixel is a palette index and the raster is sent at
//  its native size. Two pixels share a byte, EVEN column in the low nibble.
//
//  The ISR cannot look a palette entry up per pixel without costing ~8 loads
//  per 4 pixels, so the palette is folded into a 256-entry table at init:
//    expand[b] = pal[b & 15] | (pal[(b >> 4) & 15] << 8)
//  i.e. one 16-bit load replaces two palette loads, and two source bytes become
//  one aligned 32-bit store of four output pixels -- the same shape as the
//  doubling path above. The table is rebuilt only at a phase switch, into a
//  different table slot, so the ISR never sees a half-written entry.
// ---------------------------------------------------------------------------
#define SCANOUT_FULL_W 640
#define SCANOUT_FULL_H 480
#define SCANOUT_FULL_PX (SCANOUT_FULL_W * SCANOUT_FULL_H)

// One entry of the index->DAC-byte expansion table.
static inline uint16_t scanout_expand_entry(const uint8_t *pal, int b)
{
    return (uint16_t)((uint16_t)pal[b & 15] | ((uint16_t)pal[(b >> 4) & 15] << 8));
}

static inline void scanout_build_expand(uint16_t *tab, const uint8_t *pal)
{
    for (int b = 0; b < 256; b++)
        tab[b] = scanout_expand_entry(pal, b);
}

// Fill `len_bytes` output pixels starting at output pixel `pos_px`, reading the
// 4bpp source at half that position. Odd starting positions are handled by the
// scalar path (the real DMA always starts on a 640-px line boundary).
SCANOUT_FN void scanout_index4_fill(uint8_t *out, const uint8_t *src,
                                    const uint16_t *expand, int pos_px, int len_bytes)
{
    if ((pos_px & 1) == 0) {
        int i = 0;
        for (; i + 4 <= len_bytes; i += 4) {
            const uint8_t *s = src + ((pos_px + i) >> 1);
            const uint32_t w = (uint32_t)expand[s[0]] | ((uint32_t)expand[s[1]] << 16);
            *(uint32_t *)(out + i) = w;
        }
        // Tail: one pixel at a time, and the nibble rule still applies (a chunk
        // length need not be even -- an odd tail puts the last pixel on the high
        // nibble; taking the low byte there silently duplicates its neighbour).
        for (; i < len_bytes; i++) {
            const int px = pos_px + i;
            const uint16_t e = expand[src[px >> 1]];
            out[i] = (uint8_t)((px & 1) ? (e >> 8) : e);
        }
        return;
    }
    for (int i = 0; i < len_bytes; i++) {
        const int px = pos_px + i;
        const uint16_t e = expand[src[px >> 1]];
        out[i] = (uint8_t)((px & 1) ? (e >> 8) : e);
    }
}
