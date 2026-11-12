// ---------------------------------------------------------------------------
//  tools/host_render.c -- host-side render of what the S3 scanout will show.
//
//  Compiles the SAME headers the firmware compiles (src/ramp_screen.h via
//  src/ladder64.h, and src/images.h) and reproduces the firmware's doubling
//  (each 320x240 pixel -> 2x2 screen pixels) with plain nearest-neighbour, so:
//
//    ramp-native.ppm    the 320x240 ramp buffer, byte-for-byte comparable with
//                       photo-display-64/tools/ramp-preview.ppm -- this is the
//                       proof that the copied ladder/ramp headers behave
//                       identically to rung 2's originals.
//    ramp-doubled.ppm   the same buffer at 640x480, i.e. the frame the monitor
//                       should show.
//    <image>-doubled.ppm  the two photos, likewise.
//
//  What this cannot prove: that the LCD_CAM signal is correct, or that the
//  monitor locks. Those are the bench checks (console capture + Stephen's
//  photographs). This is a check of the pixel content only.
// ---------------------------------------------------------------------------
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ramp_screen.h"
#include "images.h"

#define SRC_W 320
#define SRC_H 240
#define OUT_W 640
#define OUT_H 480

static uint8_t g_src[SRC_W * SRC_H];
static uint8_t g_code[4];

static void bufPut(void *ctx, int x, int y, uint8_t raw)
{
    (void)ctx;
    if ((unsigned)x < SRC_W && (unsigned)y < SRC_H)
        g_src[y * SRC_W + x] = raw;
}

static int writePpm(const char *path, int doubled)
{
    FILE *o = fopen(path, "wb");
    if (!o) { perror(path); return 2; }
    const int W = doubled ? OUT_W : SRC_W;
    const int H = doubled ? OUT_H : SRC_H;
    fprintf(o, "P6\n%d %d\n255\n", W, H);
    uint8_t *line = (uint8_t *)malloc((size_t)W * 3);
    for (int y = 0; y < H; y++) {
        const uint8_t *row = g_src + (doubled ? (y >> 1) : y) * SRC_W;
        for (int x = 0; x < W; x++) {
            const uint8_t raw = row[doubled ? (x >> 1) : x];
            line[x * 3 + 0] = g_code[raw & 3];
            line[x * 3 + 1] = g_code[(raw >> 2) & 3];
            line[x * 3 + 2] = g_code[(raw >> 4) & 3];
        }
        fwrite(line, 1, (size_t)W * 3, o);
    }
    free(line);
    fclose(o);
    printf("wrote %s (%dx%d)\n", path, W, H);
    return 0;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "tools";
    char path[512];

    ladderInit();
    ladderLevelBytes(g_code);
    printf("level sRGB codes: %d %d %d %d\n",
           g_code[0], g_code[1], g_code[2], g_code[3]);

    // --- ramp -------------------------------------------------------------
    memset(g_src, 0, sizeof(g_src));
    rampDraw(bufPut, NULL, SRC_W, SRC_H);
    snprintf(path, sizeof(path), "%s/ramp-native.ppm", dir);
    if (writePpm(path, 0)) return 2;
    snprintf(path, sizeof(path), "%s/ramp-doubled.ppm", dir);
    if (writePpm(path, 1)) return 2;

    printf("ramp patch centres (raw byte per channel row, left->right 0/LSB/MSB/both):\n");
    const char *rows = "RGBW";
    for (int r = 0; r < 4; r++) {
        const int y = (r < 3 ? RAMP_ROWS[r] : RAMP_GREY_Y) + RAMP_H / 2;
        printf("  row %c:", rows[r]);
        for (int q = 0; q < 4; q++) {
            const int x = RAMP_X0 + q * RAMP_PITCH + RAMP_PATCH_W / 2;
            printf(" %02X", g_src[y * SRC_W + x]);
        }
        printf("\n");
    }

    // --- images -----------------------------------------------------------
    struct { const char *name; const uint8_t *data; } imgs[] = {
        { "img_lorikeet",    IMG_LORIKEET    },
        { "img_blue_marble", IMG_BLUE_MARBLE },
    };
    for (unsigned i = 0; i < sizeof(imgs) / sizeof(imgs[0]); i++) {
        memcpy(g_src, imgs[i].data, sizeof(g_src));
        snprintf(path, sizeof(path), "%s/%s-doubled.ppm", dir, imgs[i].name);
        if (writePpm(path, 1)) return 2;
    }
    return 0;
}
