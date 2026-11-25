/*
 * 0244 -- frame side live loop: button, countdown, SHOOT trigger, ESP-NOW
 * receive, SD library write, ~10 s slideshow over that library.
 *
 * Wiring (YD-ESP32-S3 N16R8):
 *   SD   SCK 12 / MOSI 11 / MISO 13 / CS 10   (0241's pins, 400 kHz mount)
 *   VGA  data 5,4,7,6,16,15 / hsync 17 / vsync 18
 *   Button  GPIO14 to GND, pull-up, active-low (pin map's recorded spare)
 *
 * Peer-review fixes folded in (Copilot, 2026-11-23):
 *   1  chunk coverage is tracked in a bitmap and a frame is only accepted when
 *      every chunk of it arrived -- the hash alone can be satisfied by stale bytes
 *   2  store is hardened: max-index+1 naming, fflush/fclose checked, partial file
 *      unlinked, so a hole in the sequence cannot overwrite a higher-numbered file
 *   3  the receive callback only assembles bytes; SHA, SD write, display and the
 *      ESP-NOW ack all happen in this loop, never in the WiFi callback
 *   4  two buffers: drawing happens in the buffer that is NOT being scanned out,
 *      and it is published with one pointer store (main.c reads s_fb_live)
 *   6  packets are gated on the camera's MAC and carry a per-boot session id
 *   8  the library scan matches exactly LIVEdddd.BIN
 *
 * Library is separate from 0243's /sdcard/FRAMES (800x600) and from /NEWCARD.BIN.
 * Nothing here formats a card: format_if_mount_failed is false everywhere.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "prompt_strip.h"    /* 0254: pre-rendered keep/discard prompt */
#include "notice_deleted.h"  /* 0254: shown after a discard */
#include "notice_camera.h"   /* 0256: shown when a capture fails */
#include "delete_strip.h"    /* 0257: the silent delete confirm */
#include "notice_deleting.h" /* 0257: shown while a photo is removed */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_lcd_panel_ops.h"
#include "mbedtls/sha256.h"

extern uint8_t *s_fb;
extern uint8_t *volatile s_fb_live;
extern void app_logf(const char *fmt, ...);
extern void write_report(void);

#define PIN_SCK   12
#define PIN_MOSI  11
#define PIN_MISO  13
#define PIN_CS    10
#define PIN_BTN   14
#define SD_MOUNT  "/sdcard"
#define LIVE_DIR  "/sdcard/LIVE"
#define DWELL_MS  10000

/* 0254: keep-or-discard confirm.  Short press keeps, a ~2 s hold discards, and
 * 30 s of silence keeps -- the timeout must never throw a photo away. */
#define PROMPT_MS      30000
#define KEEP_HOLD_MS    2000
/* 0257: silent delete.  A 2 s hold on the slideshow opens the confirm; the hold
 * itself never deletes, and a timeout cancels. */
#define DELETE_PROMPT_MS 30000
#define HOLD_DELETE_MS    2000
#ifndef SD_MOUNT_KHZ
#define SD_MOUNT_KHZ 16000        /* settled rung: the ladder in 0245 climbed 400k -> 16 MHz
                                     with 3 cold boots at each step, zero mount regressions
                                     and hash-exact stores throughout */
#endif
#define PAYLOAD   240
#define CHANNEL   6

#ifndef OUT_W
#define OUT_W 640
#endif
#ifndef OUT_H
#define OUT_H 480
#endif
#define FB_BYTES  ((uint32_t)OUT_W * (uint32_t)OUT_H)
#define CHUNKS    ((FB_BYTES + PAYLOAD - 1) / PAYLOAD)

static uint8_t s_cam_mac[6] = { 0xF0, 0x16, 0x1D, 0xAE, 0x2B, 0x20 };
static uint8_t *s_rx;                 /* receive buffer */
static uint8_t *s_buf[2];             /* draw buffers, one published at a time */
static uint8_t *s_hold;               /* 0254: clean copy of the frame before the countdown */
static int      s_w;                  /* index of the buffer NOT on screen */
static uint8_t  s_seen[CHUNKS / 8 + 1];
static volatile uint32_t s_expect_len, s_seen_chunks;
static volatile uint16_t s_sess;
static volatile bool s_rx_active, s_pending, s_shooting, s_need_report;
static volatile int64_t s_shoot_us, s_fs_us;   /* 0245 timing instrumentation */
static volatile uint32_t s_lib_max, s_lib_count;
static volatile int64_t  s_notice_until;   /* 0254: hold the slideshow while a notice shows */
static volatile bool     s_notice_restore;  /* 0256: put the previous photo back when it expires */
static volatile int32_t  s_cur = -1;        /* 0257: index of the photograph on the panel */
static uint8_t  s_rx_hash[32];
static sdmmc_card_t *s_card;
static bool s_mounted;

static void sha_hex(const uint8_t *d, size_t n, char out[65])
{
    uint8_t h[32];
    mbedtls_sha256(d, n, h, 0);
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", h[i]);
    out[64] = 0;
}

/* ---- SD ----------------------------------------------------------------- */
static const char *g_cmd_tag = "";
static bool g_raw_tx_err;

static uint8_t sd_crc7(const uint8_t *d, int n)
{
    uint8_t crc = 0;
    for (int k = 0; k < n; k++) {
        uint8_t b = d[k];
        for (int i = 0; i < 8; i++) {
            crc = (uint8_t)(crc << 1);
            if ((b & 0x80) ^ (crc & 0x80)) crc ^= 0x09;
            b = (uint8_t)(b << 1);
        }
    }
    return (uint8_t)((crc << 1) | 1);
}

static uint8_t sd_raw_cmd(spi_device_handle_t dev, uint8_t cmd, uint32_t arg,
                          int extra, uint8_t *out, int out_max)
{
    uint8_t tx[32], rx[32];
    memset(tx, 0xFF, sizeof tx);
    memset(rx, 0x00, sizeof rx);
    g_raw_tx_err = false;
    tx[0] = 0x40 | cmd;
    tx[1] = (uint8_t)(arg >> 24);
    tx[2] = (uint8_t)(arg >> 16);
    tx[3] = (uint8_t)(arg >> 8);
    tx[4] = (uint8_t)(arg);
    tx[5] = sd_crc7(tx, 5);
    int n = 6 + 8 + extra;
    if (n > (int)sizeof(tx)) n = (int)sizeof(tx);

    gpio_set_level(PIN_CS, 0);
    spi_transaction_t t = {0};
    t.length = (size_t)n * 8;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    esp_err_t err = spi_device_transmit(dev, &t);
    gpio_set_level(PIN_CS, 1);
    uint8_t ff = 0xFF;
    spi_transaction_t tail = {0};
    tail.length = 8;
    tail.tx_buffer = &ff;
    if (spi_device_transmit(dev, &tail) != ESP_OK) g_raw_tx_err = true;
    if (err != ESP_OK) { g_raw_tx_err = true; return 0xFF; }

    int idx = -1;
    for (int i = 6; i < n; i++) if ((rx[i] & 0x80) == 0) { idx = i; break; }
    if (idx < 0) return 0xFF;
    int copied = 0;
    for (int i = idx + 1; i < n && copied < out_max && copied < extra; i++, copied++)
        out[copied] = rx[i];
    return rx[idx];
}

/* 0239 fold: 74+ clocks with CS high, one throwaway CMD0 on the raw bus at
 * 400 kHz, then release the bus so the stock driver can initialise SPI2.  This
 * card returns ESP_ERR_TIMEOUT (263) without it. */
static esp_err_t fold_presacrifice(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI, .miso_io_num = PIN_MISO, .sclk_io_num = PIN_SCK,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) { app_logf("live       : fold: bus init failed (%s)\n", esp_err_to_name(err)); return err; }
    gpio_set_direction(PIN_CS, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_CS, 1);

    spi_device_interface_config_t devcfg = {0};
    devcfg.clock_speed_hz = 400 * 1000;
    devcfg.mode = 0;
    devcfg.spics_io_num = -1;
    devcfg.queue_size = 1;
    spi_device_handle_t dev = NULL;
    err = spi_bus_add_device(SPI2_HOST, &devcfg, &dev);
    if (err != ESP_OK) { spi_bus_free(SPI2_HOST); return err; }

    uint8_t ff[10];
    memset(ff, 0xFF, sizeof ff);
    spi_transaction_t t = {0};
    t.length = sizeof(ff) * 8;
    t.tx_buffer = ff;
    err = spi_device_transmit(dev, &t);
    if (err != ESP_OK) { spi_bus_remove_device(dev); spi_bus_free(SPI2_HOST); return err; }

    uint8_t pl[8] = {0};
    uint8_t r1 = sd_raw_cmd(dev, 0, 0, sizeof pl, pl, sizeof pl);
    gpio_set_level(PIN_CS, 1);
    spi_bus_remove_device(dev);
    spi_bus_free(SPI2_HOST);
    app_logf("live       : fold CMD0 R1=0x%02x%s\n", r1, g_raw_tx_err ? " (tx error)" : "");
    return ESP_OK;
}

static bool card_mount(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI, .miso_io_num = PIN_MISO, .sclk_io_num = PIN_SCK,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 4096,
    };
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
        app_logf("live       : spi_bus_initialize failed\n");
        return false;
    }
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SPI2_HOST;
    slot.gpio_cs = PIN_CS;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = SD_MOUNT_KHZ;

    esp_vfs_fat_mount_config_t mcfg = {
        .format_if_mount_failed = false,     /* never formats */
        .max_files = 4,
        .allocation_unit_size = 0,
    };
    esp_err_t err = esp_vfs_fat_sdspi_mount(SD_MOUNT, &host, &slot, &mcfg, &s_card);
    if (err != ESP_OK) {
        app_logf("live       : SD mount failed: %s (%d)\n", esp_err_to_name(err), (int)err);
        spi_bus_free(SPI2_HOST);             /* next fold must be able to init */
        return false;
    }
    app_logf("live       : SD mounted, %.*s psn=0x%08x, %llu MB\n",
             8, s_card->cid.name, (unsigned)s_card->cid.serial,
             (unsigned long long)((uint64_t)s_card->csd.capacity * s_card->csd.sector_size
                                  / (1024 * 1024)));
    mkdir(LIVE_DIR, 0777);
    return true;
}

/* finding 8: only LIVEdddd.BIN counts, and the next index is max+1 */
static void live_scan(void)
{
    DIR *d = opendir(LIVE_DIR);
    s_lib_count = 0;
    s_lib_max = 0;
    if (!d) return;
    struct dirent *e;
    uint32_t maxidx = 0, n = 0;
    while ((e = readdir(d)) != NULL) {
        const char *nm = e->d_name;
        if (strlen(nm) != 12 || strncmp(nm, "LIVE", 4) != 0 || strcmp(nm + 8, ".BIN") != 0)
            continue;
        bool digits = true;
        for (int i = 4; i < 8; i++) if (nm[i] < '0' || nm[i] > '9') digits = false;
        if (!digits) continue;
        uint32_t idx = (uint32_t)atoi(nm + 4);
        if (idx + 1 > maxidx) maxidx = idx + 1;
        n++;
    }
    closedir(d);
    s_lib_count = n;
    s_lib_max = maxidx;      /* next file to write */
}

/* finding 2: no partial file survives, and a hole cannot overwrite a file */
static bool live_store(const uint8_t *frame, char *path_out, size_t path_len)
{
    if (!s_mounted) return false;
    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info(SD_MOUNT, &total, &freeb) == ESP_OK &&
        freeb < (uint64_t)FB_BYTES + 65536) {
        app_logf("live       : SD full (%llu B free), frame not stored\n",
                 (unsigned long long)freeb);
        return false;
    }
    snprintf(path_out, path_len, LIVE_DIR "/LIVE%04u.BIN", (unsigned)s_lib_max);
    FILE *f = fopen(path_out, "wb");
    if (!f) { app_logf("live       : cannot open %s\n", path_out); return false; }
    size_t wr = fwrite(frame, 1, FB_BYTES, f);
    bool flushed = (fflush(f) == 0);
    bool closed = (fclose(f) == 0);
    if (wr != FB_BYTES || !flushed || !closed) {
        app_logf("live       : write failed wr=%u flush=%d close=%d -> unlinking\n",
                 (unsigned)wr, (int)flushed, (int)closed);
        unlink(path_out);
        return false;
    }
    s_lib_max++;
    s_lib_count++;
    return true;
}

static bool live_load(uint32_t i)
{
    if (!s_mounted) return false;
    char path[64];
    snprintf(path, sizeof(path), LIVE_DIR "/LIVE%04u.BIN", (unsigned)i);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t *dst = s_buf[s_w];               /* never the buffer on screen */
    size_t rd = fread(dst, 1, FB_BYTES, f);
    fclose(f);
    if (rd != FB_BYTES) return false;
    s_fb_live = dst;                         /* atomic publish */
    s_w ^= 1;
    return true;
}

/* ---- countdown ---------------------------------------------------------- */
static const uint8_t DIG[3][5] = {
    { 0x7, 0x1, 0x3, 0x1, 0x7 },   /* 3 */
    { 0x7, 0x1, 0x7, 0x4, 0x7 },   /* 2 */
    { 0x2, 0x6, 0x2, 0x2, 0x7 },   /* 1 */
};

static void draw_countdown(int step)
{
    const int blk = 36, gw = 3 * blk, gh = 5 * blk;
    uint8_t *dst = s_buf[s_w];               /* off-screen buffer, then publish */
    memset(dst, 0x00, FB_BYTES);
    int x0 = (OUT_W - gw) / 2, y0 = (OUT_H - gh) / 2;
    const uint8_t *g = DIG[3 - step];
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 3; c++)
            if (g[r] & (0x4 >> c))
                for (int y = 0; y < blk; y++) {
                    uint8_t *row = dst + (size_t)(y0 + r * blk + y) * OUT_W + x0 + c * blk;
                    memset(row, 0x3f, blk);
                }
    s_fb_live = dst;
    s_w ^= 1;
}

/* ---- esp-now ------------------------------------------------------------ */
static inline void seen_set(uint32_t idx)
{
    uint32_t byte = idx >> 3, bit = idx & 7;
    if (!(s_seen[byte] & (1u << bit))) {
        s_seen[byte] |= (uint8_t)(1u << bit);
        s_seen_chunks++;
    }
}

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    /* finding 6: only the configured camera, and only a matching session */
    if (memcmp(info->src_addr, s_cam_mac, 6) != 0) return;
    if (len < 4) return;

    if (data[0] == 'F' && data[1] == 'S' && len >= 8) {
        uint32_t l = (uint32_t)data[2] | ((uint32_t)data[3] << 8) |
                     ((uint32_t)data[4] << 16) | ((uint32_t)data[5] << 24);
        uint16_t sess = (uint16_t)(data[6] | (data[7] << 8));
        if (l != FB_BYTES) { s_rx_active = false; return; }
        s_expect_len = l;
        s_sess = sess;
        memset(s_seen, 0, sizeof s_seen);
        s_seen_chunks = 0;
        s_rx_active = true;
        s_fs_us = esp_timer_get_time();         /* 0245: transfer-start timestamp */
        return;
    }
    if (data[0] == 'F' && data[1] == 'D' && s_rx_active) {
        if (len < 6) return;
        uint16_t sess = (uint16_t)(data[4] | (data[5] << 8));
        if (sess != s_sess) return;                 /* stale/foreign chunk */
        uint32_t idx = (uint32_t)data[2] | ((uint32_t)data[3] << 8);
        size_t off = (size_t)idx * PAYLOAD, n = (size_t)len - 6;
        size_t want = (idx + 1 == CHUNKS) ? (FB_BYTES - off) : PAYLOAD;
        if (idx >= CHUNKS || off + n > FB_BYTES || n != want) {
            app_logf("live       : chunk %u rejected (len %u want %u)\n",
                     (unsigned)idx, (unsigned)n, (unsigned)want);
            return;
        }
        memcpy(s_rx + off, data + 6, n);
        seen_set(idx);
        return;
    }
    if (data[0] == 'F' && data[1] == 'E' && s_rx_active && len >= 36) {
        uint16_t sess = (uint16_t)(data[2] | (data[3] << 8));
        if (sess != s_sess) { s_rx_active = false; return; }
        s_rx_active = false;
        /* finding 1: a complete frame or nothing -- the hash alone is not enough */
        if (s_seen_chunks != CHUNKS) {
            app_logf("live       : INCOMPLETE frame, %u of %u chunks, dropped\n",
                     (unsigned)s_seen_chunks, (unsigned)CHUNKS);
            return;
        }
        memcpy(s_rx_hash, data + 4, 32);
        s_pending = true;           /* finding 3: work happens in the loop */
    }
}

static bool shoot(void)
{
    uint8_t msg[2] = { 'F', 'T' };
    s_shoot_us = esp_timer_get_time();          /* 0245: SHOOT-sent timestamp */
    if (esp_now_send(s_cam_mac, msg, sizeof(msg)) != ESP_OK) return false;
    app_logf("live       : SHOOT sent to %02x:%02x:%02x:%02x:%02x:%02x\n",
             s_cam_mac[0], s_cam_mac[1], s_cam_mac[2], s_cam_mac[3], s_cam_mac[4], s_cam_mac[5]);
    return true;
}

/* 0247: a failed shot must not leave the countdown digit on the panel.  The
 * buffer that is not being scanned out still holds the frame shown before the
 * countdown, so publishing it again puts the picture back. */
/* 0254 (Copilot review finding 1): the three countdown frames overwrite BOTH draw
 * buffers, so "previous" could only ever mean a digit.  s_hold keeps a clean copy
 * of whatever the panel was showing before the countdown started, and every
 * failure path restores from that instead. */
static void snapshot_previous(void)
{
    if (s_hold != NULL && s_fb_live != NULL)
        memcpy(s_hold, (const void *)s_fb_live, FB_BYTES);
}

static void restore_previous(const char *why)
{
    uint8_t *prev = s_buf[s_w];
    if (s_hold != NULL) memcpy(prev, s_hold, FB_BYTES);
    s_fb_live = prev;
    s_w ^= 1;
    app_logf("live       : previous frame restored to the panel (%s)\n", why);
}

/* 0254: compose the confirm view.  The photograph is scaled to fit above the
 * prompt strip (533x400 in a 640x400 area, bars either side) so neither the
 * picture nor the question is cropped. */
static void build_confirm_view(uint8_t *dst, const uint8_t *photo, const uint8_t *strip)
{
    const int area_h = OUT_H - PROMPT_STRIP_H;                 /* 400 */
    const int scaled_w = (int)((long)OUT_W * area_h / OUT_H);  /* 533 */
    const int x0 = (OUT_W - scaled_w) / 2;                     /* 53 */
    memset(dst, 0, FB_BYTES);
    for (int y = 0; y < area_h; y++) {
        int sy = y * OUT_H / area_h;
        const uint8_t *src = photo + (size_t)sy * OUT_W;
        uint8_t *row = dst + (size_t)y * OUT_W + x0;
        for (int x = 0; x < scaled_w; x++) row[x] = src[x * OUT_W / scaled_w];
    }
    memcpy(dst + (size_t)area_h * OUT_W, strip,
           (size_t)PROMPT_STRIP_W * PROMPT_STRIP_H);
}

/* 0254: the hold needs to be visible.  A bar under the prompt fills as the
 * button is held, so 2 s of silence reads as a countdown rather than a freeze.
 * Drawn straight into the on-screen buffer: seven rows at the bottom, so a tear
 * can at worst make the bar look a frame stale. */
static void draw_hold_bar(uint8_t *fb, int pct)
{
    const int y0 = OUT_H - PROMPT_STRIP_H + 70;
    const int rows = 7;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int w = (OUT_W - 40) * pct / 100;
    for (int y = y0; y < y0 + rows; y++) {
        uint8_t *row = fb + (size_t)y * OUT_W + 20;
        memset(row, 0x15, OUT_W - 40);            /* track: level-1 grey */
        if (w > 0) memset(row, 0x1B, w);          /* fill: brand orange */
    }
}

/* 0254: the frame shown after a discard.  Without it the panel republished the
 * buffer the countdown had last drawn into, so the person saw the digit "1"
 * sitting there after deleting a photo. */
static void build_notice_view(uint8_t *dst, const uint8_t *asset)
{
    memset(dst, 0, FB_BYTES);
    int y0 = (OUT_H - NOTICE_H) / 2;
    if (y0 < 0) y0 = 0;
    memcpy(dst + (size_t)y0 * OUT_W, asset, (size_t)NOTICE_W * NOTICE_H);
}

/* 0256: one place that puts a notice on the panel.  restore_after=true means the
 * notice is a transient message: when its 4 s are up the frame puts the previous
 * photograph back, so a failed capture no longer returns to the last picture
 * silently.  A discard notice stays put until the slideshow takes over. */
static void show_notice(const uint8_t *asset, const char *why, bool restore_after, int ms)
{
    uint8_t *notice = s_buf[s_w];
    build_notice_view(notice, asset);
    s_fb_live = notice;
    s_w ^= 1;
    s_notice_until = esp_timer_get_time() + (int64_t)ms * 1000;
    s_notice_restore = restore_after;
    app_logf("live       : notice on panel for 4 s (%s)%s\n", why,
             restore_after ? ", previous photo returns after" : "");
}

/* 0254: wait for the answer.  Returns true to keep.  A press consumed here is
 * never seen by the shoot path -- the caller re-arms the button afterwards. */
static bool ask_keep(uint8_t *fb, bool *out_timed_out)
{
    int64_t t0 = esp_timer_get_time();
    int64_t press_us = 0;
    int last_pct = -1;
    int last = gpio_get_level(PIN_BTN);
    app_logf("confirm    : shown, waiting up to %d s (press = keep, hold %d s = discard)\n",
             PROMPT_MS / 1000, KEEP_HOLD_MS / 1000);

    /* A button still held from the shoot press is not an answer.  Waiting for the
     * release stops the shot's own press from being read as a 2 s hold and
     * discarding the photo before the person has seen the question. */
    int64_t rel_wait = esp_timer_get_time();
    while (gpio_get_level(PIN_BTN) == 0 && esp_timer_get_time() - rel_wait < 5000000LL)
        vTaskDelay(pdMS_TO_TICKS(20));
    if (gpio_get_level(PIN_BTN) == 0)
        app_logf("confirm    : button still held after 5 s, accepting an answer anyway\n");
    last = gpio_get_level(PIN_BTN);
    draw_hold_bar(fb, 0);

    while (esp_timer_get_time() - t0 < (int64_t)PROMPT_MS * 1000) {
        int level = gpio_get_level(PIN_BTN);
        int64_t now = esp_timer_get_time();
        if (level == 0 && last == 1) {                            /* press begins */
            press_us = now;
            last_pct = -1;
        }
        if (level == 0 && press_us != 0) {                        /* while held */
            int held = (int)((now - press_us) / 1000);
            int pct = held * 100 / KEEP_HOLD_MS;
            if (pct != last_pct) { draw_hold_bar(fb, pct); last_pct = pct; }
            if (held >= KEEP_HOLD_MS) {
                draw_hold_bar(fb, 100);
                app_logf("confirm    : DISCARD (held %d ms, bar full)\n", held);
                return false;
            }
        }
        if (level == 1 && last == 0 && press_us != 0) {           /* released */
            int64_t held = (now - press_us) / 1000;
            draw_hold_bar(fb, 0);
            if (held < KEEP_HOLD_MS) {
                app_logf("confirm    : KEEP (short press, %lld ms)\n", (long long)held);
                return true;
            }
            app_logf("confirm    : DISCARD (held %lld ms)\n", (long long)held);
            return false;
        }
        last = level;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (out_timed_out != NULL) *out_timed_out = true;
    app_logf("confirm    : KEEP (no answer in %d s; timeout never discards)\n",
             PROMPT_MS / 1000);
    return true;
}

/* 0257: the answer to the delete prompt.  1 = delete, 0 = cancel, -1 = timeout
 * (also a cancel).  Same shape as ask_keep: the hold that opened this prompt has
 * to be released before anything counts, a short press answers yes, a 2 s hold
 * answers no, and silence cancels. */
static int ask_delete(uint8_t *fb)
{
    int64_t t0 = esp_timer_get_time();
    int64_t press_us = 0;
    int last_pct = -1;
    app_logf("delete     : confirm shown, waiting up to %d s (press = delete, hold %d s = cancel)\n",
             DELETE_PROMPT_MS / 1000, HOLD_DELETE_MS / 1000);

    int64_t rel_wait = esp_timer_get_time();
    while (gpio_get_level(PIN_BTN) == 0 && esp_timer_get_time() - rel_wait < 5000000LL)
        vTaskDelay(pdMS_TO_TICKS(20));
    if (gpio_get_level(PIN_BTN) == 0)
        app_logf("delete     : button still held after 5 s, accepting an answer anyway\n");
    int last = gpio_get_level(PIN_BTN);
    draw_hold_bar(fb, 0);

    while (esp_timer_get_time() - t0 < (int64_t)DELETE_PROMPT_MS * 1000) {
        int level = gpio_get_level(PIN_BTN);
        int64_t now = esp_timer_get_time();
        if (level == 0 && last == 1) { press_us = now; last_pct = -1; }
        if (level == 0 && press_us != 0) {
            int held = (int)((now - press_us) / 1000);
            int pct = held * 100 / HOLD_DELETE_MS;
            if (pct != last_pct) { draw_hold_bar(fb, pct); last_pct = pct; }
            if (held >= HOLD_DELETE_MS) {
                draw_hold_bar(fb, 100);
                app_logf("delete     : CANCEL (held %d ms)\n", held);
                return 0;
            }
        }
        if (level == 1 && last == 0 && press_us != 0) {
            int64_t held = (now - press_us) / 1000;
            draw_hold_bar(fb, 0);
            if (held < HOLD_DELETE_MS) {
                app_logf("delete     : DELETE (short press, %lld ms)\n", (long long)held);
                return 1;
            }
            app_logf("delete     : CANCEL (held %lld ms)\n", (long long)held);
            return 0;
        }
        last = level;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    app_logf("delete     : CANCEL (no answer in %d s; timeout never deletes)\n",
             DELETE_PROMPT_MS / 1000);
    return -1;
}

/* 0257: the silent delete.  Nothing on screen advertises it -- the only way in is
 * a 2 s hold while the slideshow is showing a photograph.  The hold never deletes:
 * it opens ask_delete(), and only a short press there removes the file. */
static void delete_current_photo(uint32_t *show_idx, int64_t *last_show)
{
    if (s_cur < 0 || !s_mounted) return;
    char path[64];
    snprintf(path, sizeof(path), LIVE_DIR "/LIVE%04u.BIN", (unsigned)s_cur);

    snapshot_previous();                       /* the photo, for the cancel path */
    uint8_t *fb = s_buf[s_w];
    build_confirm_view(fb, s_hold, DELETE_STRIP);
    s_fb_live = fb;
    s_w ^= 1;
    app_logf("delete     : hold opened the confirm for %s\n", path);

    int ans = ask_delete(fb);
    if (ans == 1) {
        show_notice(NOTICE_DELETING, "deleting", false, 2000);
        bool ok = (unlink(path) == 0);
        if (ok) live_scan();
        app_logf("delete     : %s %s, library %u frames\n",
                 ok ? "removed" : "FAILED to remove", path, (unsigned)s_lib_count);
        s_cur = -1;
        *last_show = esp_timer_get_time() - (int64_t)DWELL_MS * 1000;  /* next photo now */
    } else {
        restore_previous(ans == 0 ? "delete cancelled" : "delete timed out");
        *last_show = esp_timer_get_time();
    }
    s_need_report = true;
}

/* runs in the loop, never in the WiFi callback */
/* returns 1 = stored, 0 = discarded by the user, -1 = not stored (rejected/failed) */
static int finish_frame(void)
{
    char want_hex[65], got_hex[65];
    uint8_t hash[32];
    mbedtls_sha256(s_rx, FB_BYTES, hash, 0);
    for (int i = 0; i < 32; i++) {
        sprintf(want_hex + i * 2, "%02x", s_rx_hash[i]);
        sprintf(got_hex + i * 2, "%02x", hash[i]);
    }
    want_hex[64] = got_hex[64] = 0;

    /* 0256 (Copilot review): a verified frame supersedes any notice still on the
     * panel.  Without this, a frame that lands inside the camera notice's 4 s
     * window gets kept and shown, and then the notice's own expiry repaints the
     * previous photo over it. */
    s_notice_until = 0;
    s_notice_restore = false;

    if (memcmp(hash, s_rx_hash, 32) != 0) {
        app_logf("live       : frame REJECTED, sent %s got %s\n", want_hex, got_hex);
        s_need_report = true;
        show_notice(NOTICE_CAMERA, "hash mismatch", true, 4000);
        return -1;
    }
    /* 0254: ask before storing.  The verified frame stays in PSRAM (s_rx) and is
     * composed into the off-screen buffer with the prompt below it; nothing is
     * written to the card until the answer is keep. */
    uint8_t *dst = s_buf[s_w];
    build_confirm_view(dst, s_rx, PROMPT_STRIP);
    int64_t pub_us = esp_timer_get_time();
    s_fb_live = dst;                         /* photo + prompt on screen */
    s_w ^= 1;
    app_logf("timing     : shoot->display %lld ms, fs->display %lld ms\n",
             s_shoot_us ? (long long)((pub_us - s_shoot_us) / 1000) : -1LL,
             s_fs_us ? (long long)((pub_us - s_fs_us) / 1000) : -1LL);

    app_logf("confirm    : library %u frames before the answer\n", (unsigned)s_lib_count);
    bool timed_out = false;
    bool keep = ask_keep(dst, &timed_out);
    if (!keep) {
        app_logf("live       : discarded on hold, nothing written, library still %u\n",
                 (unsigned)s_lib_count);
        show_notice(NOTICE_DELETED, "discarded", false, 4000);
        s_need_report = true;
        return 0;
    }

    /* keep, or the timeout kept it: show the clean photo, then store it (0245
     * rung 1 -- publish first, the ack still means stored). */
    uint8_t *clean = s_buf[s_w];
    memcpy(clean, s_rx, FB_BYTES);
    s_fb_live = clean;
    s_w ^= 1;
    app_logf("confirm    : keeping (%s), storing\n",
             timed_out ? "30 s timeout" : "answered");

    char path[64] = "(not stored)";
    int64_t st0 = esp_timer_get_time();
    bool stored = live_store(clean, path, sizeof(path));
    int64_t store_ms = (esp_timer_get_time() - st0) / 1000;
    app_logf("live       : frame ok, sent sha %s, stored sha %s, %s %s, library %u, store %lld ms\n",
             want_hex, got_hex, stored ? "written" : "NOT written", path,
             (unsigned)s_lib_count, (long long)store_ms);

    if (!stored) {
        app_logf("live       : STORE FAILED - shown but not kept, no ack sent\n");
        s_need_report = true;
        return -1;
    }

    uint8_t ack[34];
    ack[0] = 'F'; ack[1] = 'A';
    memcpy(ack + 2, hash, 32);
    esp_now_send(s_cam_mac, ack, sizeof(ack));
    s_need_report = true;
    return 1;
}

/* ---- the loop ----------------------------------------------------------- */
void live_start(esp_lcd_panel_handle_t panel)
{
    (void)panel;
    s_rx = (uint8_t *)heap_caps_malloc(FB_BYTES, MALLOC_CAP_SPIRAM);
    s_buf[0] = (uint8_t *)heap_caps_malloc(FB_BYTES, MALLOC_CAP_SPIRAM);
    s_buf[1] = (uint8_t *)heap_caps_malloc(FB_BYTES, MALLOC_CAP_SPIRAM);
    s_hold = (uint8_t *)heap_caps_malloc(FB_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_rx || !s_buf[0] || !s_buf[1] || !s_hold) {
        app_logf("live       : no PSRAM for rx/back buffers\n");
        return;
    }
    s_w = 0;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BTN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase(); err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wcfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(CHANNEL, WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, s_cam_mac, 6);
    peer.channel = CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    esp_now_add_peer(&peer);
    app_logf("live       : esp-now ch %d, peer %02x:%02x:%02x:%02x:%02x:%02x\n",
             CHANNEL, s_cam_mac[0], s_cam_mac[1], s_cam_mac[2], s_cam_mac[3],
             s_cam_mac[4], s_cam_mac[5]);

    /* first-init is intermittent (INVALID_CRC 265 cold); each attempt re-runs
     * the fold and the failure path frees SPI2 */
    for (int attempt = 1; attempt <= 6 && !s_mounted; attempt++) {
        fold_presacrifice();
        s_mounted = card_mount();
        if (!s_mounted) {
            app_logf("live       : mount attempt %d failed, retrying\n", attempt);
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
    if (s_mounted) live_scan();
    app_logf("live       : %s library holds %u frames, next index %u in %s\n",
             s_mounted ? "SD" : "no-SD", (unsigned)s_lib_count, (unsigned)s_lib_max, LIVE_DIR);
    write_report();

    int64_t last_show = esp_timer_get_time();
    uint32_t show_idx = 0;
    int last_level = 1;

    for (;;) {
        int64_t now = esp_timer_get_time();
        int level = gpio_get_level(PIN_BTN);
        static bool armed = true;
        static bool press_used = false;    /* 0257: this press became a hold */
        static int64_t press_since = 0;    /* 0257: when the current press began */

        int prev = last_level;
        if (level != last_level) last_level = level;
        if (level == 1) armed = true;      /* a press needs a seen release first */
        if (level == 0 && prev == 1) { press_used = false; press_since = now; }

        /* 0257: the 2 s hold is now the silent delete gesture, so the shoot fires
         * on the RELEASE of a short press instead of 30 ms into the press -- a
         * hold that also fired the camera could never be a silent menu.  The hold
         * only counts while a photograph is on the slideshow and no notice is up. */
        if (level == 0 && !press_used && !s_shooting && s_cur >= 0 && now > s_notice_until &&
            press_since != 0 && now - press_since > (int64_t)HOLD_DELETE_MS * 1000) {
            press_used = true;
            press_since = 0;
            delete_current_photo(&show_idx, &last_show);
            last_level = gpio_get_level(PIN_BTN);
            armed = (last_level == 1);
        } else if (level == 1 && prev == 0 && armed && !press_used && !s_shooting &&
                   now - press_since < (int64_t)HOLD_DELETE_MS * 1000) {
            press_used = true;             /* the press is spent either way */
            press_since = 0;
            armed = false;
            s_shooting = true;
            s_notice_until = 0;            /* 0254: a press overrides the notice */
            s_notice_restore = false;      /* 0256: and cancels a pending restore */
            app_logf("live       : BUTTON pressed, library %u frames\n", (unsigned)s_lib_count);
            snapshot_previous();           /* 0254: clean frame for any failure path */
            for (int step = 3; step >= 1; step--) {
                draw_countdown(step);
                app_logf("live       : countdown %d\n", step);
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            s_pending = false;
            if (!shoot()) app_logf("live       : SHOOT send failed\n");
            /* 0247: 12 s is ample -- the transfer is 1.7-2.1 s and the camera's
             * warm-up is ~1 s.  The old 30 s only made a failure slower to see. */
            int64_t deadline = esp_timer_get_time() + 12000000LL;
            while (!s_pending && esp_timer_get_time() < deadline)
                vTaskDelay(pdMS_TO_TICKS(50));
            int outcome = -1;
            if (s_pending) {
                s_pending = false;
                outcome = finish_frame();
                app_logf("live       : photo received and on screen\n");
                /* 0254: the press that answered the prompt must not arm another
                 * shoot; a held button only counts again after it is released */
                last_level = gpio_get_level(PIN_BTN);
                armed = (last_level == 1);
            } else {
                app_logf("live       : TIMEOUT waiting for the frame (12 s)\n");
                show_notice(NOTICE_CAMERA, "capture timeout", true, 4000);
            }
            s_shooting = false;
            /* 0254: a kept photo is shown next; a discard carries on cycling from
             * where the slideshow was rather than restarting with a fresh wait. */
            if (outcome == 1) {
                s_cur = (int32_t)(s_lib_max ? s_lib_max - 1 : -1);   /* 0257 */
                show_idx = 0;
                last_show = esp_timer_get_time();
            } else if (outcome == 0) {
                last_show = esp_timer_get_time() - (int64_t)DWELL_MS * 1000;
            } else {
                last_show = esp_timer_get_time();
            }
            s_need_report = true;
        }

        /* a frame that arrived outside a press still gets finished */
        if (s_pending && !s_shooting) {
            s_pending = false;
            int outcome = finish_frame();
            last_level = gpio_get_level(PIN_BTN);
            armed = (last_level == 1);
            if (outcome == 1) { s_cur = (int32_t)(s_lib_max ? s_lib_max - 1 : -1); show_idx = 0; last_show = esp_timer_get_time(); }
            else if (outcome == 0) { last_show = esp_timer_get_time() - (int64_t)DWELL_MS * 1000; }
        }

        if (!s_shooting && s_lib_count && esp_timer_get_time() > s_notice_until &&
            esp_timer_get_time() - last_show > (int64_t)DWELL_MS * 1000) {
            last_show = esp_timer_get_time();
            uint32_t tries = 0;
            /* 0247: indices run to s_lib_max and the naming scheme allows holes,
             * so bounding the search by the file count can burn whole 10 s cycles */
            while (tries++ < (s_lib_max ? s_lib_max : 1)) {
                uint32_t idx = show_idx % (s_lib_max ? s_lib_max : 1);
                if (live_load(idx)) {
                    s_cur = (int32_t)idx;              /* 0257: this one is on the panel */
                    app_logf("live       : show %u\n", (unsigned)show_idx);
                    show_idx++;
                    break;
                }
                show_idx++;
            }
        }

        /* 0256: a transient notice hands the panel back to the previous photo */
        if (s_notice_restore && !s_shooting && esp_timer_get_time() > s_notice_until) {
            s_notice_restore = false;
            restore_previous("notice expired");
        }

        if (s_need_report && !s_shooting) { s_need_report = false; write_report(); }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
