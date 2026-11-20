/*
 * 0226 -- 64-colour port at full native 640x480, living frame, N16R8.
 *
 * This is 0180's LCD_CAM RGB driver path moved to the wired N16R8 board, with
 * the framebuffer grown from 4 bpp in internal RAM to 8 bpp (64 colours) in
 * PSRAM (0225 proved 8 MB octal PSRAM, full-range pattern PASS).
 *
 * NOTHING IS DOUBLED.  One framebuffer pixel is one DAC byte is one output
 * pixel: 640x480 native, exactly as 0180 shipped.  0179's 320x240 doubled mode
 * and 0177's FabGL build are both superseded and are not referenced here.
 *
 * Wiring (agreed map, wired by Stephen 2026-11-20; 0225 verified all 12 pads
 * are live GPIOs -- it did NOT verify silkscreen position, so a swapped channel
 * would show on the test card and is to be reported, not compensated in code):
 *
 *   DAC byte bit -> pin      bit0 R_LSB/5  bit1 R_MSB/4  bit2 G_LSB/7
 *                            bit3 G_MSB/6  bit4 B_LSB/16 bit5 B_MSB/15
 *   HSYNC 17   VSYNC 18      (both active low, as 0180)
 *   pclk / DE unrouted, as 0180 -- a VGA DE-15 carries RGB + syncs only.
 *
 * Orientation: content is drawn on a 480x640 portrait canvas and rotated 90 deg
 * clockwise into the 640x480 landscape raster, the same convention as 0180, so
 * the card reads upright on a monitor pivoted counter-clockwise.
 *
 * Console: the FT232R bridge on this bench does not deliver a live console
 * (0225 finding), so the reported numbers are ALSO written to a `report` flash
 * partition and read back with esptool.  See partitions.csv.
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_ops.h"
#include "esp_timer.h"
#include "esp_partition.h"
#include "soc/soc_caps.h"

#include "font5x7.h"

#define OUT_W 640
#define OUT_H 480
#define FB_BYTES (OUT_W * OUT_H)   /* 307,200 B at 8 bpp  */

#define CANVAS_W 480               /* portrait content */
#define CANVAS_H 640

/* 640x480@60 VESA: 800 x 525 total, 25.175 MHz nominal.  25.000 MHz from the
 * 160 MHz PLL gives 59.52 Hz; 0180 measured 59.40-59.77 Hz on the Heltec. */
#define PCLK_HZ       25000000u
#define H_SYNC_PW     96
#define H_BACK_PORCH  48
#define H_FRONT_PORCH 16
#define V_SYNC_PW     2
#define V_BACK_PORCH  33
#define V_FRONT_PORCH 10

#define BB_LINES 16
#define BB_PX    (OUT_W * BB_LINES)

static uint8_t *s_fb;
static volatile uint32_t s_vsync_count;
static volatile uint32_t s_fill_count;

/* ---- report sink (same reason and shape as the 0225 probe) ---------------- */
static char  g_report[8000];
static size_t g_report_len;

static void app_logf(const char *fmt, ...)
{
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    fputs(line, stdout);
    if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;
    if (g_report_len + (size_t)n < sizeof(g_report)) {
        memcpy(g_report + g_report_len, line, (size_t)n);
        g_report_len += (size_t)n;
    }
}

static void write_report(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "report");
    if (part == NULL) return;
    esp_partition_erase_range(part, 0, part->size);
    uint32_t header[2] = { 0x30323236u, (uint32_t)g_report_len };  /* "0226" */
    esp_partition_write(part, 0, header, sizeof(header));
    esp_partition_write(part, sizeof(header), g_report, g_report_len);
}

/* ---- DAC bytes and canvas drawing ---------------------------------------- */
/* RGB222 packed the way 0180 packs it: (B<<4)|(G<<2)|R, each field (MSB,LSB). */
static inline uint8_t dac(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint8_t)(((b & 3u) << 4) | ((g & 3u) << 2) | (r & 3u));
}

/* canvas (portrait 480x640) -> raster (landscape 640x480), 90 deg clockwise */
static inline void put(int cx, int cy, uint8_t v)
{
    if ((unsigned)cx >= CANVAS_W || (unsigned)cy >= CANVAS_H) return;
    const int rx = OUT_W - 1 - cy;
    const int ry = cx;
    s_fb[ry * OUT_W + rx] = v;
}

static void fill_rect(int x, int y, int w, int h, uint8_t v)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            put(x + i, y + j, v);
}

static void draw_text(int x, int y, const char *s, int scale, uint8_t v)
{
    for (; *s; s++) {
        int gi = font_index(*s);
        if (gi >= 0) {
            for (int col = 0; col < 5; col++) {
                uint8_t bits = FONT5X7[gi][col];
                for (int row = 0; row < 7; row++) {
                    if (bits & (1u << row)) {
                        for (int sy = 0; sy < scale; sy++)
                            for (int sx = 0; sx < scale; sx++)
                                put(x + col * scale + sx, y + row * scale + sy, v);
                    }
                }
            }
        }
        x += 6 * scale;
    }
}

/* ---- test card ----------------------------------------------------------- */
static uint8_t s_level_bytes[4];   /* one channel's 0..3 ladder bytes */

static void draw_test_card(void)
{
    const uint8_t black = dac(0, 0, 0);
    const uint8_t white = dac(3, 3, 3);

    memset(s_fb, 0, FB_BYTES);

    draw_text(10, 10, "64 COLOUR VGA", 3, white);
    draw_text(10, 42, "640X480 NATIVE", 2, white);
    draw_text(10, 62, "8BPP FB IN PSRAM", 2, dac(2, 2, 2));
    draw_text(10, 82, "NO DOUBLING ANYWHERE IN THE PATH", 1, dac(2, 1, 0));

    /* Three separate channel ramps, each labelled, so a swapped channel or a
     * swapped bit inside a channel is visible immediately. */
    struct { const char *label; uint8_t r, g, b; int y; } ramps[3] = {
        { "RED RAMP",   1, 0, 0, 100 },
        { "GREEN RAMP", 0, 1, 0, 178 },
        { "BLUE RAMP",  0, 0, 1, 256 },
    };
    for (int r = 0; r < 3; r++) {
        draw_text(10, ramps[r].y, ramps[r].label, 2, white);
        for (int lvl = 0; lvl < 4; lvl++) {
            uint8_t v = dac((uint8_t)(ramps[r].r * lvl), (uint8_t)(ramps[r].g * lvl),
                            (uint8_t)(ramps[r].b * lvl));
            s_level_bytes[lvl] = v;
            fill_rect(10 + lvl * 110, ramps[r].y + 18, 104, 44, v);
        }
    }

    /* Full-range 64-colour grid: r + 4g + 16b, so a bit swap reorders it. */
    draw_text(10, 340, "64 COLOURS RGB222", 2, white);
    for (int b = 0; b < 4; b++)
        for (int g = 0; g < 4; g++)
            for (int r = 0; r < 4; r++) {
                int idx = r + 4 * g + 16 * b;
                int cx = 10 + (idx & 7) * 34;
                int cy = 362 + (idx >> 3) * 34;
                fill_rect(cx, cy, 33, 33, dac((uint8_t)r, (uint8_t)g, (uint8_t)b));
            }

    /* 1-pixel checkerboard: the pixelation test Stephen's eyes are the judge of. */
    draw_text(300, 362, "1PX", 2, white);
    draw_text(300, 382, "CHECKER", 2, white);
    for (int y = 0; y < 120; y++)
        for (int x = 0; x < 120; x++)
            put(300 + x, 400 + y, ((x + y) & 1) ? white : black);

    /* Native-resolution small text: 1 framebuffer pixel = 1 output pixel. */
    draw_text(300, 532, "NATIVE 1X TEXT", 1, white);
    draw_text(300, 542, "IF THIS IS FUZZY", 1, dac(1, 1, 0));
    draw_text(300, 552, "THE PATH IS NOT 1:1", 1, dac(1, 1, 0));
    draw_text(10, 620, "0226 N16R8", 1, dac(2, 2, 2));
}

/* ---- LCD_CAM ------------------------------------------------------------- */
static bool IRAM_ATTR bounce_fill(esp_lcd_panel_handle_t panel, void *bounce_buf,
                                  int pos_px, int len_bytes, void *user_ctx)
{
    (void)panel; (void)user_ctx;
    s_fill_count++;
    /* 8 bpp: one framebuffer byte per output pixel, straight from PSRAM.
     * No index expansion, no doubling -- that is the whole point of this rung. */
    memcpy(bounce_buf, s_fb + pos_px, (size_t)len_bytes);
    return false;
}

static bool IRAM_ATTR on_vsync(esp_lcd_panel_handle_t panel,
                               const esp_lcd_rgb_panel_event_data_t *edata, void *user_ctx)
{
    (void)panel; (void)edata; (void)user_ctx;
    s_vsync_count++;
    return false;
}

static esp_err_t panel_start(esp_lcd_panel_handle_t *out_panel)
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
            .flags = { .hsync_idle_low = 1, .vsync_idle_low = 1, .pclk_active_neg = 1 },
        },
        .data_width = 8,
        .bits_per_pixel = 8,
        .num_fbs = 0,
        .bounce_buffer_size_px = BB_PX,
        .hsync_gpio_num = 17,
        .vsync_gpio_num = 18,
        .de_gpio_num = -1,
        .pclk_gpio_num = -1,
        .disp_gpio_num = -1,
        .flags = { .no_fb = 1 },
    };
    for (int i = 0; i < SOC_LCDCAM_RGB_DATA_WIDTH; i++) cfg.data_gpio_nums[i] = -1;
    cfg.data_gpio_nums[0] = 5;   /* R LSB */
    cfg.data_gpio_nums[1] = 4;   /* R MSB */
    cfg.data_gpio_nums[2] = 7;   /* G LSB */
    cfg.data_gpio_nums[3] = 6;   /* G MSB */
    cfg.data_gpio_nums[4] = 16;  /* B LSB */
    cfg.data_gpio_nums[5] = 15;  /* B MSB  <- N16R8 map (0180 used 19/20) */

    esp_err_t err = esp_lcd_new_rgb_panel(&cfg, out_panel);
    if (err != ESP_OK) return err;
    esp_lcd_rgb_panel_event_callbacks_t cbs = {};
    cbs.on_bounce_empty = bounce_fill;
    cbs.on_vsync = on_vsync;
    err = esp_lcd_rgb_panel_register_event_callbacks(*out_panel, &cbs, NULL);
    if (err != ESP_OK) return err;
    return esp_lcd_panel_init(*out_panel);   /* starts the stream in no_fb mode */
}

void app_main(void)
{
    app_logf("\n\n########## 0226 64-COLOUR PORT (N16R8) ##########\n");
    app_logf("build: %s %s\n", __DATE__, __TIME__);

    s_fb = (uint8_t *)heap_caps_malloc(FB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_fb == NULL) {
        app_logf("FATAL: could not allocate %d B framebuffer in PSRAM\n", FB_BYTES);
        write_report();
        abort();
    }

    app_logf("resolution : %dx%d native, 1 fb pixel = 1 DAC byte = 1 output pixel\n", OUT_W, OUT_H);
    app_logf("colour     : 8 bpp = 64 colours (RGB222 through the 6-bit DAC)\n");
    app_logf("framebuffer: %p, %d B, in PSRAM (%s)\n", (void *)s_fb, FB_BYTES,
             heap_caps_check_integrity(MALLOC_CAP_SPIRAM, true) ? "integrity ok" : "INTEGRITY FAIL");
    app_logf("timing     : pclk %u Hz, h %d+%d+%d+%d, v %d+%d+%d+%d -> %.2f Hz (configured)\n",
             (unsigned)PCLK_HZ, H_SYNC_PW, H_BACK_PORCH, OUT_W, H_FRONT_PORCH,
             V_SYNC_PW, V_BACK_PORCH, OUT_H, V_FRONT_PORCH,
             (double)PCLK_HZ / ((double)(H_SYNC_PW + H_BACK_PORCH + OUT_W + H_FRONT_PORCH) *
                                (double)(V_SYNC_PW + V_BACK_PORCH + OUT_H + V_FRONT_PORCH)));
    app_logf("sync pins  : hsync GPIO17, vsync GPIO18, both active-low\n");
    app_logf("data pins  : bit0=R_LSB/5 bit1=R_MSB/4 bit2=G_LSB/7 bit3=G_MSB/6 "
             "bit4=B_LSB/16 bit5=B_MSB/15 (bits 6-7, pclk, de unrouted)\n");
    app_logf("dac bytes  : level bytes 0x%02x,0x%02x,0x%02x,0x%02x (channel at 0..3)\n",
             dac(0,0,0), dac(1,0,0), dac(2,0,0), dac(3,0,0));

    draw_test_card();
    app_logf("test card  : R/G/B ramps + 64-colour grid + 1px checker + 1x text drawn\n");

    esp_lcd_panel_handle_t panel = NULL;
    esp_err_t perr = panel_start(&panel);
    if (perr != ESP_OK) {
        app_logf("FATAL: panel start failed: %s\n", esp_err_to_name(perr));
        write_report();
        abort();
    }
    app_logf("panel      : streaming (no_fb, %d-line bounce buffers, %d B each)\n",
             BB_LINES, BB_PX);
    write_report();

    /* Measure what the driver is actually doing for 6 s, then report once. */
    uint32_t v0 = s_vsync_count, f0 = s_fill_count;
    int64_t t0 = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS(6000));
    int64_t dt = esp_timer_get_time() - t0;
    uint32_t dv = s_vsync_count - v0, df = s_fill_count - f0;
    double hz = dt > 0 ? (double)dv * 1e6 / (double)dt : 0.0;
    app_logf("measured   : %u vsync in %.3f s -> %.2f Hz refresh\n",
             (unsigned)dv, (double)dt / 1e6, hz);
    app_logf("fills      : %u bounce fills in the same window -> %.1f per frame\n",
             (unsigned)df, dv ? (double)df / (double)dv : 0.0);
    app_logf("expected   : %.1f fills/frame (%d lines / %d-line bounce buffer)\n",
             (double)OUT_H / (double)BB_LINES, OUT_H, BB_LINES);
    write_report();
    app_logf("card held  : test card stays on screen; Ctrl-C/power cycle to stop\n");

    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
