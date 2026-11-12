// ---------------------------------------------------------------------------
//  tools/ramp_preview.cpp -- host render of the diagnostic ramp screen.
//
//  Compiles the SAME src/ramp_screen.h the firmware uses (same layout, same
//  font, same quantiser and dither for the gradient band) and swaps only the
//  pixel sink: instead of FabGL's setRawPixel the raw pixel byte is expanded
//  through the ladder's level codes into an RGB triple and written as a PPM.
//
//  Build and run (from the project directory):
//      g++ -O2 -Isrc tools/ramp_preview.cpp -o /tmp/ramp_preview -lm
//      /tmp/ramp_preview tools/ramp-preview.ppm
//
//  What this proves and what it does not: it shows exactly which raw level each
//  patch asks the controller for, and that the layout puts the four steps in
//  the order 0 / LSB / MSB / both on each channel. It cannot show what the
//  monitor does with those bits -- that is the photograph the ramp exists for.
// ---------------------------------------------------------------------------
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "ramp_screen.h"

static const int H_RES = 320;
static const int V_RES = 240;

static uint8_t g_raw[V_RES * H_RES];
static uint8_t g_code[4];

static void bufPut(void *ctx, int x, int y, uint8_t raw)
{
    (void)ctx;
    if (x < 0 || y < 0 || x >= H_RES || y >= V_RES) return;
    g_raw[y * H_RES + x] = raw;
}

int main(int argc, char **argv)
{
    const char *outPath = argc > 1 ? argv[1] : "tools/ramp-preview.ppm";

    photo64Init();
    photo64LevelBytes(g_code);

    float V[4];
    photo64Ladder(V);
    printf("ramp preview %dx%d\n", H_RES, V_RES);
    printf("level -> raw bits -> sRGB code sent to the DAC / luminance code\n");
    for (int q = 0; q < 4; q++) {
        const char *bits = q == 0 ? "00" : q == 1 ? "01 (LSB only)"
                          : q == 2 ? "10 (MSB only)" : "11 (both)";
        printf("  level %d  bits %-12s code %3d  luminance %3d  (%.3f of full scale)\n",
               q, bits, g_code[q],
               (int)((q == 0 ? 0.0 : pow(V[q], PHOTO64_GAMMA)) * 255.0 + 0.5), V[q]);
    }
    printf("row order left->right: 0, LSB only, MSB only, both\n");

    memset(g_raw, 0, sizeof(g_raw));
    rampDraw(bufPut, NULL, H_RES, V_RES);

    // Sample the centre of each patch so the log states what was drawn rather
    // than what the code intended.
    const char *rows = "RGBW";
    for (int r = 0; r < 4; r++) {
        const int y = (r < 3 ? RAMP_ROWS[r] : RAMP_GREY_Y) + RAMP_H / 2;
        printf("row %c:", rows[r]);
        for (int q = 0; q < 4; q++) {
            const int x = RAMP_X0 + q * RAMP_PITCH + RAMP_PATCH_W / 2;
            printf(" %02X", g_raw[y * H_RES + x]);
        }
        printf("\n");
    }

    FILE *o = fopen(outPath, "wb");
    if (!o) { perror("open out"); return 2; }
    fprintf(o, "P6\n%d %d\n255\n", H_RES, V_RES);
    for (int i = 0; i < H_RES * V_RES; i++) {
        const uint8_t raw = g_raw[i];
        const uint8_t px[3] = { g_code[raw & 3], g_code[(raw >> 2) & 3], g_code[(raw >> 4) & 3] };
        fwrite(px, 1, 3, o);
    }
    fclose(o);
    printf("wrote %s\n", outPath);
    return 0;
}
