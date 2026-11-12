// ---------------------------------------------------------------------------
//  tools/render_check.cpp -- host-side check of the firmware's JPEG -> 3-bit
//  pipeline, run on a PC instead of on the ESP32.
//
//  It compiles the SAME src/photo_render.h the firmware uses (same decoder
//  configuration, same Bayer dither, same out-callback) and swaps only the
//  pixel sink: instead of vga.dotFast() the pixels go into an RGB buffer that
//  is written out as a PPM. That isolates everything except the VGA electrical
//  path, which needs a monitor and Stephen's acceptance photo.
//
//  Build and run (from the project directory):
//      gcc -O2 -Ilib/TJpgDec -c lib/TJpgDec/tjpgd.c -o /tmp/tjpgd.o
//      g++ -O2 -Ilib/TJpgDec -Isrc tools/render_check.cpp /tmp/tjpgd.o \
//          -o /tmp/render_check
//      /tmp/render_check data/photo.jpg /tmp/render.ppm
// ---------------------------------------------------------------------------
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "photo_render.h"

static const int H_RES = 640;
static const int V_RES = 480;
static uint8_t g_img[H_RES * V_RES * 3];

static void bufPutPixel(void *ctx, int x, int y, int r, int g, int b)
{
    (void)ctx;
    uint8_t *p = &g_img[(y * H_RES + x) * 3];
    p[0] = (uint8_t)r;
    p[1] = (uint8_t)g;
    p[2] = (uint8_t)b;
}

int main(int argc, char **argv)
{
    const char *jpegPath = argc > 1 ? argv[1] : "data/photo.jpg";
    const char *outPath  = argc > 2 ? argv[2] : "render.ppm";

    FILE *f = fopen(jpegPath, "rb");
    if (!f) { perror("open jpeg"); return 2; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *jpg = (uint8_t *)malloc((size_t)n);
    if (!jpg || fread(jpg, 1, (size_t)n, f) != (size_t)n) { perror("read jpeg"); return 2; }
    fclose(f);

    memset(g_img, 0, sizeof(g_img));

    static uint8_t pool[TJPGD_WORKSPACE_SIZE] __attribute__((aligned(4)));
    RenderTarget target;
    memset(&target, 0, sizeof(target));
    target.stream.p    = jpg;
    target.stream.left = (size_t)n;
    target.put         = bufPutPixel;
    target.ctx         = NULL;
    target.screenW     = H_RES;
    target.screenH     = V_RES;

    JDEC jd;
    memset(&jd, 0, sizeof(jd));

    JRESULT res = jd_prepare(&jd, photoInFunc, pool, sizeof(pool), &target);
    if (res != JDR_OK) {
        printf("jd_prepare FAILED: %s (%d)\n", jdErrorString(res), (int)res);
        return 3;
    }
    printf("jpeg: %ux%u, components=%u\n",
           (unsigned)jd.width, (unsigned)jd.height, (unsigned)jd.ncomp);

    res = jd_decomp(&jd, photoOutFunc, 0);
    if (res != JDR_OK) {
        printf("jd_decomp FAILED: %s (%d)\n", jdErrorString(res), (int)res);
        return 3;
    }
    printf("pixels drawn: %u (expected %d)\n",
           (unsigned)target.pixelsDrawn, H_RES * V_RES);

    // Palette census: with one bit per channel there can be at most 8 values.
    int used[8] = {0};
    for (int i = 0; i < H_RES * V_RES; i++) {
        const uint8_t *p = &g_img[i * 3];
        int idx = ((p[0] ? 4 : 0) | (p[1] ? 2 : 0) | (p[2] ? 1 : 0));
        used[idx]++;
    }
    printf("palette (r,g,b) pixel counts:");
    for (int i = 0; i < 8; i++)
        printf(" %d%d%d=%d", (i >> 2) & 1, (i >> 1) & 1, i & 1, used[i]);
    printf("\n");

    FILE *o = fopen(outPath, "wb");
    if (!o) { perror("open out"); return 2; }
    fprintf(o, "P6\n%d %d\n255\n", H_RES, V_RES);
    fwrite(g_img, 1, sizeof(g_img), o);
    fclose(o);
    printf("wrote %s\n", outPath);
    return 0;
}
