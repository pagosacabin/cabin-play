// ---------------------------------------------------------------------------
//  tools/render_check.cpp -- host-side check of the firmware's JPEG -> 64-color
//  pipeline, run on a PC instead of on the ESP32.
//
//  It compiles the SAME src/photo_render64.h the firmware uses (same decoder
//  configuration, same quantiser, same 8x8 Bayer dither, same output callback)
//  and swaps only the pixel sink: instead of vga.setRawPixel() the levels go
//  into a buffer, and the buffer is written out as a PPM scaled by the level's
//  displayed sRGB code. That isolates everything except the VGA electrical
//  path, which needs a monitor.
//
//  Build and run (from the project directory):
//      gcc -O2 -I../photo-display/lib/TJpgDec -c ../photo-display/lib/TJpgDec/tjpgd.c -o /tmp/tjpgd64.o
//      g++ -O2 -I../photo-display/lib/TJpgDec -Isrc tools/render_check.cpp /tmp/tjpgd64.o -o /tmp/render_check64
//      /tmp/render_check64 data/photo.jpg out.ppm [perceptual|legacy]
//
//  Reported:
//    * pixels drawn
//    * distinct palette entries used out of 64, and the per-channel level
//      histograms (the palette census the task asks for)
//    * fidelity in BOTH domains, because they disagree and the disagreement is
//      the point of round 2:
//        - byte domain: block-averaged RMS of the displayed sRGB code against
//          the source sRGB code. Round 1 optimised this, and it is the wrong
//          target: the eye integrates light, not codes, so a dither that
//          matches average *codes* is visibly too bright.
//        - light domain: both images gamma-decoded to luminance first, then
//          block-averaged and compared. This is what the panel shows.
//    * black-floor leak: of the pixels the source places in near-black
//      (luma < 16 of 255), the percentage the pipeline lights up above level 0.
//      That is Stephen's "speckled black background", as a number.
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

static inline double toLight(double code)
{
    return pow(code / 255.0, (double)PHOTO64_GAMMA) * 255.0;
}

// Block-average an image into a bw x bh grid of grey (luminance) means.
// `light` selects the domain: 0 = sRGB codes as they are, 1 = gamma-decoded.
static void blockLuma(const uint8_t (*img)[3], int bw, int bh, double *out, int light)
{
    const int w = H_RES / bw, h = V_RES / bh;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            double sum = 0;
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    const uint8_t *p = img[(by * h + y) * H_RES + (bx * w + x)];
                    double r = p[0], g = p[1], b = p[2];
                    if (light) { r = toLight(r); g = toLight(g); b = toLight(b); }
                    sum += 0.299 * r + 0.587 * g + 0.114 * b;
                }
            out[by * bw + bx] = sum / (w * h);
        }
    }
}

// Block-averaged RGB RMS error, in 0..255 units, over bw x bh blocks.
static void blockRMSE(int bw, int bh, double out[3], int light)
{
    const int w = H_RES / bw, h = V_RES / bh;
    double sq[3] = {0, 0, 0};
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++)
            for (int c = 0; c < 3; c++) {
                double a = 0, b = 0;
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++) {
                        const int i = (by * h + y) * H_RES + (bx * w + x);
                        a += light ? toLight(g_rgb[i][c]) : g_rgb[i][c];
                        b += light ? toLight(g_src[i][c]) : g_src[i][c];
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
    const char *modeName = argc > 3 ? argv[3] : "perceptual";
    const int   mode     = strcmp(modeName, "legacy") == 0
                           ? PHOTO64_MODE_LEGACY : PHOTO64_MODE_PERCEPTUAL;

    float L[4];
    photo64Ladder(L);
    for (int i = 0; i < 4; i++) g_levelByte[i] = (uint8_t)(L[i] * 255.0f + 0.5f);
    uint8_t disp[4], lum[4];
    photo64LevelBytes(disp);
    photo64LevelLuminance(lum);

    printf("dither mode: %s\n",
           mode == PHOTO64_MODE_LEGACY ? "legacy (voltage domain)"
                                       : "perceptual (light domain)");
    printf("dac ladder (fraction of full scale): %.4f %.4f %.4f %.4f\n",
           L[0], L[1], L[2], L[3]);
    printf("level sRGB code sent to the DAC: %d %d %d %d\n",
           disp[0], disp[1], disp[2], disp[3]);
    printf("level luminance (V^2.2) as code: %d %d %d %d\n",
           lum[0], lum[1], lum[2], lum[3]);

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
    target.cfg.mode       = mode;

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

    double e8[3], e2[3], l8[3], l2[3];
    blockRMSE(40, 30, e8, 0);       // 8x8 pixel blocks, sRGB-code domain
    blockRMSE(160, 120, e2, 0);     // 2x2 pixel blocks, sRGB-code domain
    blockRMSE(40, 30, l8, 1);       // 8x8 pixel blocks, light domain
    blockRMSE(160, 120, l2, 1);     // 2x2 pixel blocks, light domain
    printf("block RMS byte domain  8x8 : R %.2f G %.2f B %.2f\n", e8[0], e8[1], e8[2]);
    printf("block RMS byte domain  2x2 : R %.2f G %.2f B %.2f\n", e2[0], e2[1], e2[2]);
    printf("block RMS light domain 8x8 : R %.2f G %.2f B %.2f\n", l8[0], l8[1], l8[2]);
    printf("block RMS light domain 2x2 : R %.2f G %.2f B %.2f\n", l2[0], l2[1], l2[2]);

    static double la[40 * 30], lb[40 * 30];
    blockLuma(g_rgb, 40, 30, la, 1);
    blockLuma(g_src, 40, 30, lb, 1);
    const double n8 = ncc(la, lb, 40 * 30, 0);
    const double n8r = ncc(la, lb, 40 * 30, 10);
    static double la2[160 * 120], lb2[160 * 120];
    blockLuma(g_rgb, 160, 120, la2, 1);
    blockLuma(g_src, 160, 120, lb2, 1);
    const double n2 = ncc(la2, lb2, 160 * 120, 0);
    printf("luma NCC (light) 40x30: %.4f   (source rolled 10 px: %.4f)\n", n8, n8r);
    printf("luma NCC (light) 160x120: %.4f\n", n2);

    // Black-floor leak: pixels the SOURCE puts in near-black, which the
    // pipeline lights up above level 0. This is the "speckled black sky".
    int darkPix = 0, darkLit = 0;
    for (int i = 0; i < H_RES * V_RES; i++) {
        const int luma = (299 * g_src[i][0] + 587 * g_src[i][1] + 114 * g_src[i][2]) / 1000;
        if (luma < 16) {
            darkPix++;
            if (g_q[i][0] || g_q[i][1] || g_q[i][2]) darkLit++;
        }
    }
    printf("black-floor leak: %d of %d near-black source pixels lit (%.1f%%)\n",
           darkLit, darkPix, darkPix ? 100.0 * darkLit / darkPix : 0.0);

    // Palette reachability: sweep a 16x16x16 lattice of source colours through
    // the SAME quantiser at every dither phase and count the distinct outputs.
    // 64/64 means the pipeline can address the whole palette; the photo's own
    // count is a property of the picture, not a limit of the firmware.
    int reach[64]; memset(reach, 0, sizeof(reach));
    for (int r = 0; r < 256; r += 17)
        for (int g = 0; g < 256; g += 17)
            for (int b = 0; b < 256; b += 17)
                for (int y = 0; y < 8; y++)
                    for (int x = 0; x < 8; x++) {
                        const int qr = photo64Quant((uint8_t)r, BAYER8[(y + BAYER_DY[0]) & 7][(x + BAYER_DX[0]) & 7], mode);
                        const int qg = photo64Quant((uint8_t)g, BAYER8[(y + BAYER_DY[1]) & 7][(x + BAYER_DX[1]) & 7], mode);
                        const int qb = photo64Quant((uint8_t)b, BAYER8[(y + BAYER_DY[2]) & 7][(x + BAYER_DX[2]) & 7], mode);
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
