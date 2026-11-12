// ---------------------------------------------------------------------------
//  photo_render.h -- JPEG -> 3-bit VGA pixel stream, shared by the firmware and
//  by the host-side render check in tools/.
//
//  This file contains no Arduino/ESP32 dependencies on purpose: the same
//  source is compiled into the firmware (src/main.cpp) and into
//  tools/render_check.cpp on the host, so the decode path, the palette
//  decision and the dither are the *same code* in both, not a re-implementation
//  that could drift.
//
//  The caller supplies the pixel sink: put(ctx, x, y, r, g, b) where r/g/b are
//  already quantised to 0 or 255 (one bit per channel -- the hardware limit of
//  the 3-bit VGA path).
// ---------------------------------------------------------------------------
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <tjpgd.h>

// 4x4 ordered (Bayer) dither matrix, thresholds spread over 0..255. Each
// channel is compared against the threshold for its pixel position and becomes
// one bit. Plain rounding to nearest throws away all gradient structure at 1
// bit per channel; the dither converts it into a pattern the eye integrates.
static const uint8_t BAYER4[4][4] = {
    {  0, 128,  32, 160},
    {192,  64, 224,  96},
    { 48, 176,  16, 144},
    {240, 112, 208,  80}
};

// TJpgDec input callback: stream from a read-only memory buffer (in the
// firmware, that buffer is the JPEG embedded in flash).
typedef struct {
    const uint8_t *p;
    size_t         left;
} MemStream;

typedef void (*PutPixelFn)(void *ctx, int x, int y, int r, int g, int b);

// One device block, because TJpgDec gives both callbacks the SAME
// jd->device pointer: the stream to read from and the sink to write to must
// travel together. (Splitting them into two structs is the bug that makes
// every pixel silently vanish -- jd_prepare() sets jd->device to whatever it
// was handed, so the output callback would cast the stream to a render target.)
typedef struct {
    MemStream  stream;
    PutPixelFn put;
    void      *ctx;
    int        screenW;
    int        screenH;
    uint32_t   pixelsDrawn;
} RenderTarget;

static inline size_t photoInFunc(JDEC *jd, uint8_t *buff, size_t nbyte)
{
    MemStream *s = &((RenderTarget *)jd->device)->stream;
    if (nbyte > s->left) nbyte = s->left;
    if (buff) memcpy(buff, s->p, nbyte);
    s->p    += nbyte;
    s->left -= nbyte;
    return nbyte;
}

// TJpgDec output callback: one MCU block of RGB888 pixels -> dithered pixels.
static inline int photoOutFunc(JDEC *jd, void *bitmap, JRECT *rect)
{
    RenderTarget  *t   = (RenderTarget *)jd->device;
    const uint8_t *src = (const uint8_t *)bitmap;
    const int w = rect->right - rect->left + 1;
    const int h = rect->bottom - rect->top + 1;

    for (int j = 0; j < h; j++) {
        const int y = rect->top + j;
        for (int i = 0; i < w; i++) {
            const int x = rect->left + i;
            const uint8_t r = *src++;
            const uint8_t g = *src++;
            const uint8_t b = *src++;
            if (x >= t->screenW || y >= t->screenH) continue;
            const uint8_t th = BAYER4[y & 3][x & 3];
            t->put(t->ctx, x, y,
                   r > th ? 255 : 0,
                   g > th ? 255 : 0,
                   b > th ? 255 : 0);
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
