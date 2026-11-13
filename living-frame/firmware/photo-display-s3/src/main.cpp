// ---------------------------------------------------------------------------
//  photo-display-s3 -- the living frame's VGA driver on the Heltec WiFi LoRa 32
//  V3 (ESP32-S3FN8, 8 MB flash, no PSRAM, MAC 9c:13:9e:a1:37:40 on /dev/ttyUSB1).
//
//  TWO MODES, selected at build time (platformio.ini envs):
//
//    PHOTO_MODE_FULL16    (task 0180, rung 3b -- the default)
//        a TRUE 640x480 frame buffer at 4 bits per pixel = 153,600 B in internal
//        RAM, each nibble an index into the current image's own 16-entry
//        palette. No doubling: one index -> one DAC byte, expanded in the DMA
//        bounce-buffer ISR through a 256-entry table. Content is 480x640
//        portrait, rotated 90 deg clockwise on the host into the landscape
//        raster, so the frame is portrait-native on a pivoted monitor.
//
//    PHOTO_MODE_DOUBLED64 (task 0179, rung 3a -- preserved, still selectable)
//        a 320x240 one-byte-per-pixel frame (76,800 B) pixel-doubled 2x2 inside
//        the same ISR, 64 colours. The original proof; unchanged in behaviour.
//
//  WHY NO PSRAM IS THE WHOLE STORY.  640x480 at one byte per pixel is 307,200 B
//  and does not fit internal SRAM beside the driver's bounce buffers; 640x480 at
//  four bits per pixel is 153,600 B and does. That is why this rung trades
//  colours (64 -> 16) for resolution, and it is the question 0180 asks Stephen
//  to answer with his eyes.
//
//  PIN MAP -> DATA BYTE.  The agreed map is R MSB 4, R LSB 5, G MSB 6, G LSB 7,
//  B MSB 19, B LSB 20, HSYNC 38, VSYNC 39. A DAC byte is packed (B<<4)|(G<<2)|R
//  with each 2-bit field (MSB,LSB), i.e. bit0 = R LSB/GPIO5 ... bit5 = B
//  MSB/GPIO19, so a palette entry is a DAC byte that reaches the ladder with no
//  translation. Bits 6-7 and pclk/de are unrouted: a VGA DE-15 carries R/G/B +
//  HSYNC + VSYNC only, and the monitor rebuilds pixel timing from HSYNC.
// ---------------------------------------------------------------------------
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_ops.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "soc/soc_caps.h"

#include "ladder64.h"
#include "scanout.h"

#define PHOTO_MODE_FULL16    1
#define PHOTO_MODE_DOUBLED64 2
#ifndef PHOTO_MODE
#define PHOTO_MODE PHOTO_MODE_FULL16
#endif

#if PHOTO_MODE == PHOTO_MODE_FULL16
#include "images16.h"
#include "ramp_portrait.h"
#include "ramp_palette.h"
#define SRC_W IMG16_W              // 640
#define SRC_H IMG16_H              // 480
#define FB_BYTES IMG16_BYTES       // 153,600 (4 bpp)
#else
#include "images.h"
#include "ramp_screen.h"
#define SRC_W 320
#define SRC_H 240
#define FB_BYTES (320 * 240)
#endif

#define OUT_W 640
#define OUT_H 480

// 640x480@60 VESA: 800 x 525 total, 25.175 MHz nominal.  25.000 MHz from the
// 160 MHz PLL (divide by 6.4) gives 59.52 Hz.  Rung 3a measured 59.40-59.77 Hz.
#define PCLK_HZ       25000000u
#define H_SYNC_PW     96
#define H_BACK_PORCH  48
#define H_FRONT_PORCH 16
#define V_SYNC_PW     2
#define V_BACK_PORCH  33
#define V_FRONT_PORCH 10

// Bounce buffer: 16 output lines = 10,240 B, and 307,200 / 10,240 = 30 fills per
// frame -- the figure the console counters are checked against.
#define BB_LINES 16
#define BB_PX    (OUT_W * BB_LINES)

// The ISR's geometry constants must be the same ones main.cpp prints and
// allocates with; a mismatch would be a silent picture defect.
static_assert(OUT_W == SCANOUT_OUT_W && OUT_H == SCANOUT_OUT_H,
              "output raster disagrees with scanout.h");
#if PHOTO_MODE == PHOTO_MODE_FULL16
static_assert(SRC_W == SCANOUT_FULL_W && SRC_H == SCANOUT_FULL_H,
              "4bpp frame size disagrees with scanout.h");
#else
static_assert(SRC_W == SCANOUT_SRC_W && SRC_H == SCANOUT_SRC_H,
              "frame buffer size disagrees with scanout.h");
#endif

// --- content cycle ----------------------------------------------------------
#define RAMP_MS    30000
#define IMAGE_MS   20000

// --- scanout accounting -----------------------------------------------------
// Not decoration: these are the only host-visible evidence that the LCD_CAM is
// really scanning. A starving DMA or a desynced restart shows up as a wrong
// fills/frame ratio before it shows on a screen as tearing.
static volatile uint32_t s_vsync_count;
static volatile uint32_t s_bb_fill_count;

#if PHOTO_MODE == PHOTO_MODE_FULL16
// Index -> DAC byte expansion tables, one per palette, built at init into three
// separate slots so a phase switch flips a pointer instead of rewriting the
// table the ISR is reading mid-frame.
#define PAL_RAMP   0
#define PAL_LORI   1
#define PAL_MARBLE 2
#define PAL_SLOTS  3

static uint16_t s_expand[PAL_SLOTS][256];
static uint16_t *volatile s_expand_active;
static uint8_t *s_fb;                  // 640x480 @ 4 bpp, 153,600 B internal RAM

// The ramp's own palette lives in ramp_palette.h because tools/render16.cpp
// needs the same 16 entries to draw the preview.
#else
static uint8_t *s_src;                 // 320x240 byte-per-pixel, 76,800 B
#endif

// ---------------------------------------------------------------------------
//  The bounce-buffer fill. ISR context: IRAM_ATTR, no flash reads, no logging.
// ---------------------------------------------------------------------------
static bool IRAM_ATTR bounce_fill(esp_lcd_panel_handle_t panel, void *bounce_buf,
                                  int pos_px, int len_bytes, void *user_ctx)
{
    (void)panel; (void)user_ctx;
    s_bb_fill_count++;
    // Both fills live in src/scanout.h so tools/scanout_test.cpp can drive the
    // exact code the DMA runs over a whole frame on the host.
#if PHOTO_MODE == PHOTO_MODE_FULL16
    scanout_index4_fill((uint8_t *)bounce_buf, s_fb, s_expand_active, pos_px, len_bytes);
#else
    scanout_double_fill((uint8_t *)bounce_buf, s_src, pos_px, len_bytes);
#endif
    return false;
}

static bool IRAM_ATTR on_vsync(esp_lcd_panel_handle_t panel,
                               const esp_lcd_rgb_panel_event_data_t *edata, void *user_ctx)
{
    (void)panel; (void)edata; (void)user_ctx;
    s_vsync_count++;
    return false;
}

#if PHOTO_MODE == PHOTO_MODE_FULL16
// Canvas coordinate sink for the portrait ramp: raster = 90 deg CW of canvas.
static void fb_put_index(void *ctx, int lx, int ly, uint8_t idx)
{
    (void)ctx;
    if ((unsigned)lx >= SRC_W || (unsigned)ly >= SRC_H)
        return;
    const int byte = ly * (SRC_W / 2) + (lx >> 1);
    const uint8_t v = s_fb[byte];
    s_fb[byte] = (lx & 1) ? (uint8_t)((v & 0x0F) | (uint8_t)(idx << 4))
                          : (uint8_t)((v & 0xF0) | (idx & 0x0F));
}

// Install a 4bpp image, already packed and rotated by the host converter.
static void load_image16(const uint8_t *img, int slot)
{
    s_expand_active = s_expand[slot];
    memcpy(s_fb, img, FB_BYTES);
}

static void draw_ramp(void)
{
    s_expand_active = s_expand[PAL_RAMP];
    rampPortraitDraw(fb_put_index, NULL);
}
#else
static void put_src(void *ctx, int x, int y, uint8_t raw)
{
    (void)ctx;
    if ((unsigned)x < SRC_W && (unsigned)y < SRC_H)
        s_src[y * SRC_W + x] = raw;
}

static void load_image(const uint8_t *img) { memcpy(s_src, img, FB_BYTES); }
#endif

static const char *phase_name(int p)
{
    return p == 0 ? "ramp" : p == 1 ? "lorikeet" : "blue-marble";
}

static void print_levels(void)
{
    uint8_t code[4], lum[4];
    float V[4];
    ladderLevels(V);
    ladderLevelBytes(code);
    ladderLevelLuminance(lum);
    printf("ladder: level -> frac of full scale -> sRGB code -> luminance code\n");
    for (int i = 0; i < 4; i++)
        printf("  %d (%s%s) %.3f  %3u  %3u\n", i,
               i & 1 ? "MSB" : ".", i & 2 ? "+LSB" : "", V[i], code[i], lum[i]);
}

#if PHOTO_MODE == PHOTO_MODE_FULL16
static void print_palette(const char *label, const uint8_t *pal, int n)
{
    printf("%-14s %2d entries:", label, n);
    for (int i = 0; i < 16; i++) {
        if (i < n)
            printf(" %02x", pal[i]);
        else
            printf(" --");
    }
    printf("\n");
}
#endif

static void panel_start(esp_lcd_panel_handle_t *out_panel)
{
    esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_PLL160M,
        .timings = {
            .pclk_hz = PCLK_HZ,
            .h_res = OUT_W,
            .v_res = OUT_H,
            .hsync_pulse_width = H_SYNC_PW,
            .hsync_back_porch = H_BACK_PORCH,
            .hsync_front_porch = H_FRONT_PORCH,
            .vsync_pulse_width = V_SYNC_PW,
            .vsync_back_porch = V_BACK_PORCH,
            .vsync_front_porch = V_FRONT_PORCH,
            .flags = {
                .hsync_idle_low = 1,
                .vsync_idle_low = 1,
                .pclk_active_neg = 1,
            },
        },
        .data_width = 8,
        .bits_per_pixel = 8,
        .num_fbs = 0,
        .bounce_buffer_size_px = BB_PX,
        .hsync_gpio_num = 38,
        .vsync_gpio_num = 39,
        .de_gpio_num = -1,
        .pclk_gpio_num = -1,
        .disp_gpio_num = -1,
        .flags = { .no_fb = 1 },
    };
    for (int i = 0; i < SOC_LCDCAM_RGB_DATA_WIDTH; i++)
        cfg.data_gpio_nums[i] = -1;
    cfg.data_gpio_nums[0] = 5;   // R LSB
    cfg.data_gpio_nums[1] = 4;   // R MSB
    cfg.data_gpio_nums[2] = 7;   // G LSB
    cfg.data_gpio_nums[3] = 6;   // G MSB
    cfg.data_gpio_nums[4] = 20;  // B LSB
    cfg.data_gpio_nums[5] = 19;  // B MSB

    esp_err_t err = esp_lcd_new_rgb_panel(&cfg, out_panel);
    if (err != ESP_OK) {
        printf("FATAL: esp_lcd_new_rgb_panel: %s\n", esp_err_to_name(err));
        abort();
    }
    esp_lcd_rgb_panel_event_callbacks_t cbs = {};
    cbs.on_bounce_empty = bounce_fill;
    cbs.on_vsync = on_vsync;
    err = esp_lcd_rgb_panel_register_event_callbacks(*out_panel, &cbs, NULL);
    if (err != ESP_OK) {
        printf("FATAL: register_event_callbacks: %s\n", esp_err_to_name(err));
        abort();
    }
    err = esp_lcd_panel_init(*out_panel);      // starts the RGB stream in no_fb mode
    if (err != ESP_OK) {
        printf("FATAL: esp_lcd_panel_init: %s\n", esp_err_to_name(err));
        abort();
    }
}

extern "C" void app_main(void)
{
    ladderInit();

#if PHOTO_MODE == PHOTO_MODE_FULL16
    printf("\n=== photo-display-s3 (0180 rung 3b, full-res 16-colour, PORTRAIT) ===\n");
    printf("mode        : 640x480@~60 Hz VGA (LCD_CAM RGB), TRUE 640x480 frame at "
           "4 bpp = %d B, 16 colours per photo\n", FB_BYTES);
    printf("content     : 480x640 portrait, rotated 90 deg clockwise into the "
           "landscape raster (monitor pivoted counter-clockwise)\n");
#else
    printf("\n=== photo-display-s3 (0179 rung 3a, doubled 64-colour) ===\n");
    printf("mode        : 640x480@~60 Hz VGA (LCD_CAM RGB), %dx%d frame buffer, "
           "pixel-doubled 2x2\n", SRC_W, SRC_H);
#endif
    printf("target      : ESP32-S3, SOC_LCDCAM_RGB_LCD_SUPPORTED=%d\n",
           (int)SOC_LCDCAM_RGB_LCD_SUPPORTED);
    printf("timing      : pclk %u Hz, h %d+%d+%d+%d, v %d+%d+%d+%d -> %.2f Hz\n",
           (unsigned)PCLK_HZ, H_SYNC_PW, H_BACK_PORCH, OUT_W, H_FRONT_PORCH,
           V_SYNC_PW, V_BACK_PORCH, OUT_H, V_FRONT_PORCH,
           (double)PCLK_HZ / ((double)(H_SYNC_PW + H_BACK_PORCH + OUT_W + H_FRONT_PORCH) *
                              (V_SYNC_PW + V_BACK_PORCH + OUT_H + V_FRONT_PORCH)));
    printf("sync        : hsync GPIO38, vsync GPIO39, both active-low (VGA negative polarity)\n");
    printf("data        : 8-bit bus, 6 lines used: "
           "bit0=R_LSB/5 bit1=R_MSB/4 bit2=G_LSB/7 bit3=G_MSB/6 bit4=B_LSB/20 bit5=B_MSB/19\n");
    printf("unrouted    : pclk, de (VGA has no clock/DE wire), data bits 6-7\n");
    print_levels();

#if PHOTO_MODE == PHOTO_MODE_FULL16
    s_fb = (uint8_t *)heap_caps_malloc(FB_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_fb) {
        printf("FATAL: could not allocate %d B 4bpp frame buffer\n", FB_BYTES);
        abort();
    }
    memset(s_fb, 0, FB_BYTES);
    printf("frame buffer: %p, %d B (4 bpp, internal RAM) = %dx%d pixels, 16 colours\n",
           s_fb, FB_BYTES, SRC_W, SRC_H);
    printf("palettes    : one 16-entry set per phase, expanded to a 256-entry "
           "index->DAC-byte table in RAM (3 x %d B)\n", (int)sizeof(s_expand[0]));

    scanout_build_expand(s_expand[PAL_RAMP],   RAMP_PAL);
    scanout_build_expand(s_expand[PAL_LORI],   IMG16_LORIKEET_PAL);
    scanout_build_expand(s_expand[PAL_MARBLE], IMG16_BLUE_MARBLE_PAL);
    s_expand_active = s_expand[PAL_RAMP];
    print_palette("ramp palette", RAMP_PAL, 16);
    print_palette("lorikeet", IMG16_LORIKEET_PAL, IMG16_LORIKEET_PAL_N);
    print_palette("blue-marble", IMG16_BLUE_MARBLE_PAL, IMG16_BLUE_MARBLE_PAL_N);
    printf("palette sizes: ramp 16, lorikeet %d, blue-marble %d distinct ladder colours "
           "(of the 64 reachable)\n", IMG16_LORIKEET_PAL_N, IMG16_BLUE_MARBLE_PAL_N);
#else
    s_src = (uint8_t *)heap_caps_malloc(FB_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_src) {
        printf("FATAL: could not allocate %d B frame buffer\n", FB_BYTES);
        abort();
    }
    memset(s_src, 0, FB_BYTES);
    printf("frame buffer: %p, %d B (internal RAM), padded to %d B as sent (%dx%d)\n",
           s_src, FB_BYTES, OUT_W * OUT_H, OUT_W, OUT_H);
#endif

    esp_lcd_panel_handle_t panel = NULL;
    panel_start(&panel);
    printf("panel      : streaming started (no_fb, %d-px bounce buffers x2 = %d B each)\n",
           BB_PX, BB_PX);
    printf("heap       : free after init = %u B internal (largest block %u B)\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    printf("cycle      : ramp %d s, lorikeet %d s, blue-marble %d s, repeat\n",
           RAMP_MS / 1000, IMAGE_MS / 1000, IMAGE_MS / 1000);

    const uint32_t plan[3] = { RAMP_MS, IMAGE_MS, IMAGE_MS };
    uint32_t phase = 0;
    int64_t phase_start = esp_timer_get_time();
    printf("[%8.3f s] phase 0: %s\n", 0.0, phase_name(0));
#if PHOTO_MODE == PHOTO_MODE_FULL16
    draw_ramp();
#else
    rampDraw(put_src, NULL, SRC_W, SRC_H);
#endif

    uint32_t last_vsync = 0;
    uint32_t last_fill = 0;
    int64_t last_report = esp_timer_get_time();

    while (1) {
        int64_t now = esp_timer_get_time();
        if (now - last_report >= 5000000) {
            const uint32_t v = s_vsync_count, f = s_bb_fill_count;
            printf("[%8.3f s] scanout: %u vsync (+%.2f/s), %u bounce fills "
                   "(+%.1f/frame)\n", now / 1e6, (unsigned)v,
                   (double)(v - last_vsync) * 1e6 / (double)(now - last_report),
                   (unsigned)f, v == last_vsync ? 0.0
                                                : (double)(f - last_fill) / (double)(v - last_vsync));
            last_vsync = v;
            last_fill = f;
            last_report = now;
        }
        if (now - phase_start >= (int64_t)plan[phase] * 1000) {
            phase = (phase + 1) % 3;
            phase_start = now;
            printf("[%8.3f s] phase %u: %s\n", now / 1e6, (unsigned)phase, phase_name(phase));
#if PHOTO_MODE == PHOTO_MODE_FULL16
            if (phase == 0)
                draw_ramp();
            else if (phase == 1)
                load_image16(IMG16_LORIKEET, PAL_LORI);
            else
                load_image16(IMG16_BLUE_MARBLE, PAL_MARBLE);
#else
            if (phase == 0)
                rampDraw(put_src, NULL, SRC_W, SRC_H);
            else if (phase == 1)
                load_image(IMG_LORIKEET);
            else
                load_image(IMG_BLUE_MARBLE);
#endif
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
