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
