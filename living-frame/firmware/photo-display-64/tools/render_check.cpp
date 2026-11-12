// ---------------------------------------------------------------------------
//  tools/render_check.cpp -- host-side check of the firmware's JPEG -> 64-color
//  pipeline, run on a PC instead of on the ESP32.
//
//  It compiles the SAME src/photo_render64.h the firmware uses (same decoder
//  configuration, same ladder-aware quantiser, same 8x8 Bayer dither, same
//  output callback) and swaps only the pixel sink: instead of
//  vga.setRawPixel() the levels go into a buffer, and the buffer is written
//  out as a PPM scaled by the DAC's real level voltages. That isolates
//  everything except the VGA electrical path, which needs a monitor.
//
//  Build and run (from the project directory):
//      gcc -O2 -I../photo-display/lib/TJpgDec -c ../photo-display/lib/TJpgDec/tjpgd.c -o /tmp/tjpgd64.o
//      g++ -O2 -I../photo-display/lib/TJpgDec -Isrc tools/render_check.cpp /tmp/tjpgd64.o -o /tmp/render_check64
//      /tmp/render_check64 data/photo.jpg tools/render-check.ppm
//
//  Reported:
//    * pixels drawn
//    * distinct palette entries used out of 64, and the per-channel level
//      histograms (the palette census the task asks for)
//    * fidelity, measured the only way it means anything for dithered output:
//      block-averaged RMS error against the source (per-pixel error of a
//      dithered image is ~half a quantisation step by construction) and
//      luminance NCC at two block sizes, plus a rolled-source control so the
//      NCC number is shown to be measuring structure rather than agreeing with
//      itself.
// ---------------------------------------------------------------------------
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "photo_render64.h"

static const int H_RES = 320;
static const int V_RES = 240;

static uint8_t  g_levelByte[4];
static uint8_t  g_q  [H_RES * V_RES][3];   // quantised level per channel
static uint8_t  g_rgb[H_RES * V_RES][3];   // displayed byte per channel
static uint8_t  g_src[H_RES * V_RES][3];   // source byte per channel
static uint32_t g_palette[64];

static void bufPutPixel64(void *ctx, int x, int y,
                          int qr, int qg, int qb, const uint8_t *src)
{
    (void)ctx;
    const int i = y * H_RES + x;
    g_q[i][0] = (uint8_t)qr; g_q[i][1] = (uint8_t)qg; g_q[i][2] = (uint8_t)qb;
    g_rgb[i][0] = g_levelByte[qr];
    g_rgb[i][1] = g_levelByte[qg];
    g_rgb[i][2] = g_levelByte[qb];
    g_src[i][0] = src[0]; g_src[i][1] = src[1]; g_src[i][2] = src[2];
    g_palette[(qb << 4) | (qg << 2) | qr]++;
}

// Block-average both images into a bw x bh grid of grey (luminance) means.
static void blockLuma(const uint8_t (*img)[3], int bw, int bh, double *out)
{
    const int w = H_RES / bw, h = V_RES / bh;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            double sum = 0;
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    const uint8_t *p = img[(by * h + y) * H_RES + (bx * w + x)];
                    sum += 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
                }
            out[by * bw + bx] = sum / (w * h);
        }
    }
}

// Block-averaged RGB RMS error, in 0..255 units, over bw x bh blocks.
static void blockRMSE(int bw, int bh, double out[3])
{
    const int w = H_RES / bw, h = V_RES / bh;
    double sq[3] = {0, 0, 0};
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++)
            for (int c = 0; c < 3; c++) {
                double a = 0, b = 0;
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++) {
                        a += g_rgb[(by * h + y) * H_RES + (bx * w + x)][c];
                        b += g_src[(by * h + y) * H_RES + (bx * w + x)][c];
                    }
                const double d = (a - b) / (w * h);
                sq[c] += d * d;
            }
    for (int c = 0; c < 3; c++) out[c] = sqrt(sq[c] / (bw * bh));
}

// Normalised cross-correlation of two equally sized luma planes, optionally
// with the second rolled by `roll` pixels horizontally.
static double ncc(const double *a, const double *b, int n, int roll)
{
    double ma = 0, mb = 0;
    for (int i = 0; i < n; i++) { ma += a[i]; mb += b[i]; }
    ma /= n; mb /= n;
    double num = 0, da = 0, db = 0;
    for (int i = 0; i < n; i++) {
        const int j = (i + roll) % n;
        const double x = a[i] - ma, y = b[j] - mb;
        num += x * y; da += x * x; db += y * y;
    }
    return num / sqrt(da * db);
}

int main(int argc, char **argv)
{
    const char *jpegPath = argc > 1 ? argv[1] : "data/photo.jpg";
    const char *outPath  = argc > 2 ? argv[2] : "render-check.ppm";

    float L[4];
    photo64Ladder(L);
    for (int i = 0; i < 4; i++) g_levelByte[i] = (uint8_t)(L[i] * 255.0f + 0.5f);

    printf("dac ladder (fraction of full scale): %.4f %.4f %.4f %.4f\n",
           L[0], L[1], L[2], L[3]);
    printf("level bytes: %d %d %d %d\n",
           g_levelByte[0], g_levelByte[1], g_levelByte[2], g_levelByte[3]);

    FILE *f = fopen(jpegPath, "rb");
    if (!f) { perror("open jpeg"); return 2; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *jpg = (uint8_t *)malloc((size_t)n);
    if (!jpg || fread(jpg, 1, (size_t)n, f) != (size_t)n) { perror("read jpeg"); return 2; }
    fclose(f);

    photo64Init();

    static uint8_t pool[TJPGD_WORKSPACE_SIZE] __attribute__((aligned(4)));
    RenderTarget64 target;
    memset(&target, 0, sizeof(target));
    target.stream.p       = jpg;
    target.stream.left    = (size_t)n;
    target.put            = bufPutPixel64;
    target.ctx            = NULL;
    target.cfg.screenW    = H_RES;
    target.cfg.screenH    = V_RES;
    target.cfg.scaleShift = 1;

    JDEC jd;
    memset(&jd, 0, sizeof(jd));

    JRESULT res = jd_prepare(&jd, photo64InFunc, pool, sizeof(pool), &target);
    if (res != JDR_OK) {
        printf("jd_prepare FAILED: %s (%d)\n", jdErrorString(res), (int)res);
        return 3;
    }
    printf("jpeg: %ux%u, components=%u -> %dx%d, 1/2 descale\n",
           (unsigned)jd.width, (unsigned)jd.height, (unsigned)jd.ncomp, H_RES, V_RES);

    res = jd_decomp(&jd, photo64OutFunc, target.cfg.scaleShift);
    if (res != JDR_OK) {
        printf("jd_decomp FAILED: %s (%d)\n", jdErrorString(res), (int)res);
        return 3;
    }
    printf("pixels drawn: %u (expected %d)\n",
           (unsigned)target.pixelsDrawn, H_RES * V_RES);

    int used = 0;
    uint32_t minUse = 0xFFFFFFFFu, maxUse = 0;
    int hist[3][4]; memset(hist, 0, sizeof(hist));
    for (int i = 0; i < 64; i++) {
        if (g_palette[i]) used++;
        if (g_palette[i] && g_palette[i] < minUse) minUse = g_palette[i];
        if (g_palette[i] > maxUse) maxUse = g_palette[i];
    }
    for (int i = 0; i < H_RES * V_RES; i++)
        for (int c = 0; c < 3; c++) hist[c][g_q[i][c]]++;

    printf("distinct colors in frame: %d of 64 (counts %u..%u)\n",
           used, (unsigned)minUse, (unsigned)maxUse);
    printf("level histogram R: %d %d %d %d\n", hist[0][0], hist[0][1], hist[0][2], hist[0][3]);
    printf("level histogram G: %d %d %d %d\n", hist[1][0], hist[1][1], hist[1][2], hist[1][3]);
    printf("level histogram B: %d %d %d %d\n", hist[2][0], hist[2][1], hist[2][2], hist[2][3]);

    double e8[3], e2[3];
    blockRMSE(40, 30, e8);          // 8x8 pixel blocks
    blockRMSE(160, 120, e2);        // 2x2 pixel blocks
    printf("block RMS error 8x8 blocks  (0..255): R %.2f G %.2f B %.2f\n", e8[0], e8[1], e8[2]);
    printf("block RMS error 2x2 blocks  (0..255): R %.2f G %.2f B %.2f\n", e2[0], e2[1], e2[2]);

    static double la[40 * 30], lb[40 * 30];
    blockLuma(g_rgb, 40, 30, la);
    blockLuma(g_src, 40, 30, lb);
    const double n8 = ncc(la, lb, 40 * 30, 0);
    const double n8r = ncc(la, lb, 40 * 30, 10);
    static double la2[160 * 120], lb2[160 * 120];
    blockLuma(g_rgb, 160, 120, la2);
    blockLuma(g_src, 160, 120, lb2);
    const double n2 = ncc(la2, lb2, 160 * 120, 0);
    printf("luma NCC 40x30: %.4f   (source rolled 10 px: %.4f)\n", n8, n8r);
    printf("luma NCC 160x120: %.4f\n", n2);

    // Palette reachability: the photo is a near-neutral subject (space ->
    // ocean -> cloud) and cannot exercise all 64 entries by itself. Sweep a
    // 16x16x16 lattice of source colours through the SAME quantiser at every
    // dither phase and count the distinct outputs. 64/64 here means the
    // pipeline can address the whole palette; the photo's own count is a
    // property of the picture, not a limit of the firmware.
    int reach[64]; memset(reach, 0, sizeof(reach));
    for (int r = 0; r < 256; r += 17)
        for (int g = 0; g < 256; g += 17)
            for (int b = 0; b < 256; b += 17)
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++) {
                        const int qr = photo64Quant((uint8_t)r, BAYER8[(y + BAYER_DY[0]) & 7][(x + BAYER_DX[0]) & 7]);
                        const int qg = photo64Quant((uint8_t)g, BAYER8[(y + BAYER_DY[1]) & 7][(x + BAYER_DX[1]) & 7]);
                        const int qb = photo64Quant((uint8_t)b, BAYER8[(y + BAYER_DY[2]) & 7][(x + BAYER_DX[2]) & 7]);
                        reach[(qb << 4) | (qg << 2) | qr] = 1;
                    }
    int reachable = 0;
    for (int i = 0; i < 64; i++) reachable += reach[i];
    printf("palette reachability (synthetic lattice): %d of 64\n", reachable);

    FILE *o = fopen(outPath, "wb");
    if (!o) { perror("open out"); return 2; }
    fprintf(o, "P6\n%d %d\n255\n", H_RES, V_RES);
    fwrite(g_rgb, 1, sizeof(g_rgb), o);
    fclose(o);
    printf("wrote %s\n", outPath);
    return 0;
}
