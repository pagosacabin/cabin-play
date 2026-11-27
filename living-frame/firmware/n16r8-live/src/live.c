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
 *      and it is published at a frame boundary (0260: main.c takes s_fb_pending
 *      in the bounce ISR at pos_px == 0 -- see publish() below)
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
#include "esp_random.h"      /* 0259: shuffle the slideshow order */
#include <sys/stat.h>
#include "prompt_strip.h"    /* 0254: pre-rendered keep/discard prompt */
#include "notice_deleted.h"  /* 0254: shown after a discard */
#include "notice_camera.h"   /* 0256: shown when a capture fails */
#include "delete_strip.h"    /* 0257: the silent delete confirm */
#include "notice_deleting.h" /* 0257: shown while a photo is removed */
#include "notice_sd.h"       /* 0277: shown when the card cannot be contacted */

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
extern uint8_t *volatile s_fb_pending;     /* 0260: frame waiting for a boundary */
extern volatile uint32_t s_scan_pos;       /* 0260: last pos_px the bounce ISR saw */
extern volatile uint32_t s_swap_applied;   /* 0260: swaps taken at a boundary */
extern volatile uint32_t s_swap_vsync;     /* 0260: vsync count of the last swap */
extern volatile int64_t  s_swap_req_us;    /* 0260: request timestamp */
extern volatile int64_t  s_swap_lat_us;    /* 0260: request -> applied, in the ISR */
extern portMUX_TYPE s_swap_mux;            /* 0260: guards the pending handoff */
extern volatile uint32_t s_vsync_count;
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
/* 0259: a contact bounce is a few ms; a deliberate press is 100 ms or more. */
#define MIN_PRESS_MS        60
/* 0277: how often to try the card again while it is missing */
#define SD_RETRY_MS      10000
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
static volatile int64_t  s_notice_until;
static int s_restore_tries;                /* 0260: bounded retry of a failed restore */
/* 0260: what the panel is ACTUALLY showing.  s_cur names a file; only this says a
 * photograph is on the glass, so a hold can never open the delete confirm for a
 * photo hidden behind a notice or a confirm whose restore failed (review, 5th pass). */
static volatile bool s_photo_on_panel;
static bool s_hold_is_photo;   /* 0254: hold the slideshow while a notice shows */
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

/* ---- 0260: frame-boundary publication -----------------------------------
 * What the code showed: main.c's bounce ISR copies from s_fb_live one 16-line
 * burst at a time (once per ~0.4 ms at 25 MHz), and this file used to publish a
 * finished frame with a bare `s_fb_live = buf;` from the loop.  That store lands
 * wherever the raster happens to be, so the lines already scanned out of the old
 * buffer stay and the rest come from the new one: one refresh is part old image,
 * part new -- the torn splash->photo handover Stephen filmed, and the same
 * artifact on every return to a photo (keep prompt, notice, delete).
 *
 * The frame is now handed over in s_fb_pending and only the ISR takes it, at
 * pos_px == 0, before the first burst of a frame is filled.  Everything below
 * publishes through this helper; after boot nothing else assigns s_fb_live.
 * ------------------------------------------------------------------------- */
#define SWAP_TIMEOUT_US   100000     /* 100 ms: no boundary this long means no scanout */
#define SWAP_FLUSH_EVERY  12         /* persist the swap log about once per 10-photo cycle */
static uint32_t s_swaps_since_flush;

/* Returns true when the frame is on the panel.  A false return means the handoff
 * was cancelled because no frame boundary came: callers that would act on what the
 * panel shows (accept a delete, keep a photo, advance the slideshow index) must not
 * proceed.  (Copilot review, third pass, finding 4.) */
static bool publish(uint8_t *buf, const char *what)
{
    uint32_t pos    = s_scan_pos;         /* last burst the ISR was handed */
    uint32_t vs_req = s_vsync_count;

    /* Persist the log BEFORE the request, so a flash write's cache stall can only
     * disturb the outgoing frame -- never the frame this swap is installing.  The
     * timeout clock starts after this, so a flush can never eat into the wait. */
    if (++s_swaps_since_flush >= SWAP_FLUSH_EVERY) {
        s_swaps_since_flush = 0;
        write_report();
    }

    int64_t t0 = esp_timer_get_time();
    s_swap_req_us = t0;
    taskENTER_CRITICAL(&s_swap_mux);
    s_fb_pending = buf;
    taskEXIT_CRITICAL(&s_swap_mux);
    while (s_fb_pending != NULL && esp_timer_get_time() - t0 < SWAP_TIMEOUT_US)
        vTaskDelay(1);
    int64_t polled = esp_timer_get_time() - t0;

    bool applied;
    taskENTER_CRITICAL(&s_swap_mux);
    applied = (s_fb_pending == NULL);          /* the ISR took it at a boundary */
    if (!applied && s_fb_pending == buf) s_fb_pending = NULL;   /* cancel: see below */
    taskEXIT_CRITICAL(&s_swap_mux);

    if (!applied) {
        /* No frame boundary for 100 ms.  Publishing directly from the task would
         * land wherever the raster is if scanout resumes -- the exact tear 0260
         * removes -- so the frame is ABANDONED: the panel keeps the frame it has,
         * the caller's buffer stays off screen (s_w is not toggled) and the next
         * swap tries again.  The pending slot is cancelled inside the same critical
         * section that the ISR takes it in, so a late boundary can never publish a
         * buffer the caller has started rewriting.  (Copilot review, findings 1.) */
        app_logf("swap       : %s NOT published -- no frame boundary in %lld us"
                 " (scanout stopped?); handoff cancelled, panel unchanged\n",
                 what, (long long)polled);
        return false;
    }
    uint32_t line = pos / OUT_W;
    app_logf("swap       : %s requested at px %u (raster at line %u/%d, vsync %u) ->"
             " applied at pos 0 of the next frame in %lld us (vsync %u), swap #%u,"
             " mixed lines 0 (a bare pointer store would have mixed %d)\n",
             what, (unsigned)pos, (unsigned)line, OUT_H, (unsigned)vs_req,
             (long long)s_swap_lat_us, (unsigned)s_swap_vsync,
             (unsigned)s_swap_applied, OUT_H - (int)line);
    s_w ^= 1;
    return true;
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

/* finding 8: only LIVE<digits>.BIN counts, and the next index is max+1.
 * 0259 (Copilot review): live_store() writes %04u, which silently becomes five
 * digits at 10000, so the scan has to accept four OR five -- otherwise the ten
 * thousandth photo is stored and then never shown. */
static bool parse_live_name(const char *nm, uint32_t *out)
{
    size_t len = strlen(nm);
    if (len != 12 && len != 13) return false;
    if (strncmp(nm, "LIVE", 4) != 0) return false;
    if (strcmp(nm + len - 4, ".BIN") != 0) return false;
    uint32_t v = 0;
    for (size_t i = 4; i < len - 4; i++) {
        char c = nm[i];
        if (c < '0' || c > '9') return false;
        v = v * 10u + (uint32_t)(c - '0');
    }
    *out = v;
    return true;
}

static void live_scan(void)
{
    DIR *d = opendir(LIVE_DIR);
    s_lib_count = 0;
    s_lib_max = 0;
    if (!d) return;
    struct dirent *e;
    uint32_t maxidx = 0, n = 0;
    while ((e = readdir(d)) != NULL) {
        uint32_t idx;
        if (!parse_live_name(e->d_name, &idx)) continue;
        /* 0259: the order holds at most 65535 entries and its counter is uint16, so
         * the highest usable index is 65534.  store, scan and order all agree on
         * that bound -- counting a 65535 index would describe 65536 files. */
        if (idx >= 65535u) continue;
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
    /* 0259 (Copilot review): the shuffle order indexes cards through a uint16, so
     * the library must stop where that can still represent every file.  Storing a
     * 65536th photo would create a file the slideshow can never show. */
    /* The order array is indexed by uint16 and counts entries as uint16, so the
     * library tops out at 65535 FILES (indices 0..65534).  Guarding on the count,
     * not the next index, keeps that exact (Copilot review, second pass). */
    if (s_lib_max >= 65535u) {
        app_logf("live       : library full at %u files, frame not stored\n",
                 (unsigned)s_lib_count);
        return false;
    }
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

/* 0260: *retry is set when the file was fine but the handoff could not be published
 * -- the ordered slideshow must then keep this photo for the next attempt instead of
 * spending it on a frame nobody saw (Copilot review, fourth pass). */
static bool live_load(uint32_t i, bool *retry)
{
    if (retry) *retry = false;
    if (!s_mounted) return false;
    char path[64];
    snprintf(path, sizeof(path), LIVE_DIR "/LIVE%04u.BIN", (unsigned)i);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t *dst = s_buf[s_w];               /* never the buffer on screen */
    size_t rd = fread(dst, 1, FB_BYTES, f);
    fclose(f);
    if (rd != FB_BYTES) return false;
    /* 0260: applied at a frame boundary; if the swap could not be published the
     * frame is not on the panel, so this is not a successful load. */
    if (publish(dst, "photo")) {
        s_photo_on_panel = true;
        return true;
    }
    if (retry) *retry = true;
    return false;
}

/* ---- 0259: shuffled slideshow order -------------------------------------- */
/* Each card file appears once per cycle and the order is reshuffled when the cycle
 * ends, so consecutive cycles differ.  Deleted files are dropped from the order in
 * place, which keeps "next photo" meaning the next one in the shuffled order. */
static uint16_t *s_order;
static uint16_t  s_order_cap, s_order_n, s_order_pos;

static bool order_build(void)
{
    DIR *d = opendir(LIVE_DIR);
    if (!d) return false;
    /* the order holds indices, so the cap has to follow the highest index, not the
     * file count -- and indices above 65535 would not fit; those are refused below */
    uint16_t cap = (uint16_t)(s_lib_max ? s_lib_max : 1);   /* s_lib_max <= 65535 by the store guard */
    if (cap < 2) cap = 2;
    if (s_order == NULL || s_order_cap < cap) {
        if (s_order) { heap_caps_free(s_order); s_order = NULL; }
        s_order = (uint16_t *)heap_caps_malloc((size_t)cap * sizeof(uint16_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_order == NULL) { closedir(d); return false; }
        s_order_cap = cap;
    }
    s_order_n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && s_order_n < s_order_cap) {
        uint32_t idx;
        if (parse_live_name(e->d_name, &idx) && idx < 65535u)
            s_order[s_order_n++] = (uint16_t)idx;   /* 65535 entries is the ceiling */
    }
    closedir(d);
    for (int i = (int)s_order_n - 1; i > 0; i--) {          /* Fisher-Yates */
        int j = (int)(esp_random() % (uint32_t)(i + 1));
        uint16_t t = s_order[i]; s_order[i] = s_order[j]; s_order[j] = t;
    }
    s_order_pos = 0;
    return s_order_n > 0;
}

static void order_drop(uint16_t idx)
{
    if (s_order == NULL) return;
    for (uint16_t i = s_order_pos; i < s_order_n; i++)
        if (s_order[i] == idx) {
            memmove(&s_order[i], &s_order[i + 1],
                    (size_t)(s_order_n - i - 1) * sizeof(uint16_t));
            s_order_n--;
            return;
        }
}

/* 0259: main.c holds the splash on the panel until a card photo is published; once
 * one is, the logo buffer can go.  The short delay lets any bounce buffer that is
 * still scanning the logo finish first. */
extern uint8_t *g_splash_buf;
static void splash_released(void)
{
    if (g_splash_buf != NULL && s_fb_live != g_splash_buf) {
        vTaskDelay(pdMS_TO_TICKS(250));
        heap_caps_free(g_splash_buf);
        g_splash_buf = NULL;
        app_logf("splash     : logo buffer released after the first photo\n");
    }
}

static bool show_next_shuffled(void)
{
    int guard = (int)s_order_n + 2;
    while (guard-- > 0) {
        if (s_order_pos >= s_order_n && !order_build()) return false;
        uint16_t idx = s_order[s_order_pos];
        bool retry = false;
        if (live_load(idx, &retry)) {
            s_order_pos++;
            s_cur = (int32_t)idx;
            app_logf("live       : show %u (shuffled %u/%u)\n", (unsigned)idx,
                     (unsigned)s_order_pos, (unsigned)s_order_n);
            splash_released();
            return true;
        }
        if (retry) {
            /* the photo is still good -- it was not spent, try it next time */
            app_logf("live       : %u not shown (handoff not published); kept for the next attempt\n",
                     (unsigned)idx);
            return false;
        }
        s_order_pos++;
        app_logf("live       : skip %u (no such file)\n", (unsigned)idx);
    }
    return false;
}

/* ---- 0258: portrait canvas ---------------------------------------------- */
/* The panel is mounted rotated, and every photograph reaches this buffer through
 * convert_photo64.py's transform: portrait canvas (480x640) -> landscape raster,
 *   bx = cy,  by = (OUT_H-1) - cx
 * Text used to be blitted as a landscape strip, so it read sideways next to the
 * pictures.  It now goes through the same mapping: assets are generated portrait
 * (upright as the viewer sees them, 480 wide) and placed with these helpers. */
#define VIEW_W  OUT_H      /* 480: what the viewer sees across  */
#define VIEW_H  OUT_W      /* 640: what the viewer sees down    */

static inline size_t view_index(int vx, int vy)
{
    return (size_t)((OUT_H - 1 - vx)) * OUT_W + vy;
}

/* copy a w x h portrait block so it lands at viewer position (vx0, vy0) */
static void blit_viewer(uint8_t *dst, int vx0, int vy0, int w, int h, const uint8_t *src)
{
    for (int y = 0; y < h; y++) {
        int vy = vy0 + y;
        if (vy < 0 || vy >= VIEW_H) continue;
        for (int x = 0; x < w; x++) {
            int vx = vx0 + x;
            if (vx < 0 || vx >= VIEW_W) continue;
            dst[view_index(vx, vy)] = src[(size_t)y * w + x];
        }
    }
}

/* filled rectangle in viewer coordinates (used for the countdown glyphs) */
static void fill_viewer_rect(uint8_t *dst, int vx, int vy, int w, int h, uint8_t val)
{
    if (vx < 0) { w += vx; vx = 0; }
    if (vy < 0) { h += vy; vy = 0; }
    if (vx + w > VIEW_W) w = VIEW_W - vx;
    if (vy + h > VIEW_H) h = VIEW_H - vy;
    if (w <= 0 || h <= 0) return;
    int bx0 = vy;                        /* column range in the raster */
    int by0 = (OUT_H - 1) - (vx + w - 1);
    for (int by = by0; by < by0 + w; by++) {
        if (by < 0 || by >= OUT_H) continue;
        memset(dst + (size_t)by * OUT_W + bx0, val, h);
    }
}

/* ---- countdown ---------------------------------------------------------- */
static const uint8_t DIG[3][5] = {
    { 0x7, 0x1, 0x3, 0x1, 0x7 },   /* 3 */
    { 0x7, 0x1, 0x7, 0x4, 0x7 },   /* 2 */
    { 0x2, 0x6, 0x2, 0x2, 0x7 },   /* 1 */
};

static void draw_countdown(int step)
{
    const int blk = 40, gw = 3 * blk, gh = 5 * blk;
    uint8_t *dst = s_buf[s_w];               /* off-screen buffer, then publish */
    memset(dst, 0x00, FB_BYTES);
    int x0 = (VIEW_W - gw) / 2, y0 = (VIEW_H - gh) / 2;
    const uint8_t *g = DIG[3 - step];
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 3; c++)
            if (g[r] & (0x4 >> c))
                fill_viewer_rect(dst, x0 + c * blk, y0 + r * blk, blk, blk, 0x3f);
    (void)publish(dst, "countdown");   /* cosmetic: a lost countdown frame is not a fault */
    s_photo_on_panel = false;
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
    s_hold_is_photo = s_photo_on_panel;
}

static bool restore_previous(const char *why)
{
    uint8_t *prev = s_buf[s_w];
    if (s_hold != NULL) memcpy(prev, s_hold, FB_BYTES);
    if (!publish(prev, "restore previous")) {
        app_logf("live       : restore NOT on the panel -- handoff not published (%s)\n", why);
        return false;
    }
    s_photo_on_panel = s_hold_is_photo;      /* the snapshot is what is back on the glass */
    app_logf("live       : previous frame restored to the panel (%s)\n", why);
    return true;
}

/* 0254: compose the confirm view.  The photograph is scaled to fit above the
 * prompt strip (533x400 in a 640x400 area, bars either side) so neither the
 * picture nor the question is cropped. */
static void build_confirm_view(uint8_t *dst, const uint8_t *photo,
                               const uint8_t *strip, int strip_h)
{
    /* 0258: laid out in portrait.  The photograph keeps its aspect -- 420x560 in
     * the 480x560 area above the strip, 30 px bars either side -- and both the
     * picture and the question go through the canvas->raster transform, so the
     * pair reads upright the way the photographs already do. */
    const int area_h = VIEW_H - strip_h;                        /* 560 */
    const int scaled_w = (int)((long)VIEW_W * area_h / VIEW_H); /* 420 */
    const int x0 = (VIEW_W - scaled_w) / 2;                     /* 30 */
    memset(dst, 0, FB_BYTES);
    for (int vy = 0; vy < area_h; vy++) {
        int srcy = vy * VIEW_H / area_h;
        for (int x = 0; x < scaled_w; x++) {
            int srcx = x * VIEW_W / scaled_w;
            (void)x0;
            dst[view_index(x0 + x, vy)] =
                photo[(size_t)(OUT_H - 1 - srcx) * OUT_W + srcy];
        }
    }
    blit_viewer(dst, 0, area_h, VIEW_W, strip_h, strip);
}

/* 0254: the hold needs to be visible.  A bar under the prompt fills as the
 * button is held, so 2 s of silence reads as a countdown rather than a freeze.
 * Drawn straight into the on-screen buffer: seven rows at the bottom, so a tear
 * can at worst make the bar look a frame stale. */
static void draw_hold_bar(uint8_t *fb, int pct)
{
    /* 0258: the bar lives in the prompt strip, so it is laid out in portrait too --
     * filling left to right as the viewer sees it, just above the bottom edge. */
    const int vy0 = VIEW_H - 12;
    const int vx0 = 20, wmax = VIEW_W - 40;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    fill_viewer_rect(fb, vx0, vy0, wmax, 6, 0x15);            /* track: level-1 grey */
    int w = wmax * pct / 100;
    if (w > 0) fill_viewer_rect(fb, vx0, vy0, w, 6, 0x1B);    /* fill: brand orange */
}

/* 0254: the frame shown after a discard.  Without it the panel republished the
 * buffer the countdown had last drawn into, so the person saw the digit "1"
 * sitting there after deleting a photo. */
static void build_notice_view(uint8_t *dst, const uint8_t *asset, int w, int h)
{
    memset(dst, 0, FB_BYTES);
    blit_viewer(dst, (VIEW_W - w) / 2, (VIEW_H - h) / 2, w, h, asset);
}

/* 0259: after a prompt has consumed presses, the button state must be settled
 * before the loop trusts it again.  What actually went wrong: ask_delete() (and
 * ask_keep()) read the button themselves while the loop is blocked inside them,
 * so the loop's press_since/press_used/last_level could still describe the press
 * that answered the prompt.  The release could then be re-read as a fresh shoot
 * press -- the double-shoot Stephen saw -- or, when the press began while the
 * loop was blocked, press_since stayed 0 and the next real press was swallowed.
 * Waiting for release and letting the contacts settle removes both. */
/* 0259: wait for the press that answered the prompt to be released, and no longer
 * than 2 s.  There is deliberately no fixed quiet delay any more: one swallowed a
 * complete press that happened to land inside it.  Contact bounce is filtered in
 * the shoot branch instead, by requiring a minimum press length. */
static void settle_button(void)
{
    int64_t t0 = esp_timer_get_time();
    while (gpio_get_level(PIN_BTN) == 0 && esp_timer_get_time() - t0 < 2000000LL)
        vTaskDelay(pdMS_TO_TICKS(10));
}

/* 0256: one place that puts a notice on the panel.  restore_after=true means the
 * notice is a transient message: when its 4 s are up the frame puts the previous
 * photograph back, so a failed capture no longer returns to the last picture
 * silently.  A discard notice stays put until the slideshow takes over. */
static void show_notice(const uint8_t *asset, int w, int h, const char *why,
                        bool restore_after, int ms)
{
    uint8_t *notice = s_buf[s_w];
    build_notice_view(notice, asset, w, h);
    if (!publish(notice, why))
        app_logf("live       : notice could not be published (%s)\n", why);
    s_notice_until = esp_timer_get_time() + (int64_t)ms * 1000;
    s_notice_restore = restore_after;
    s_restore_tries = 0;
    s_photo_on_panel = false;
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
            if (held < MIN_PRESS_MS) {                            /* 0259: bounce */
                app_logf("confirm    : ignored a %lld ms tap (bounce)\n", (long long)held);
                press_us = 0;
                last = level;
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
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
            if (held < MIN_PRESS_MS) {
                /* 0259 (Copilot review): a contact bounce must never answer the
                 * delete prompt -- deleting is the one irreversible answer here */
                app_logf("delete     : ignored a %lld ms tap (bounce)\n", (long long)held);
                press_us = 0;
                last = level;
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
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
    build_confirm_view(fb, s_hold, DELETE_STRIP, DELETE_STRIP_H);
    if (!publish(fb, "delete confirm")) {
        /* The confirm never reached the panel, so there is nothing to answer: a
         * press here must not delete a file the viewer cannot see a question about. */
        app_logf("delete     : confirm NOT published; press ignored, nothing deleted\n");
        *last_show = esp_timer_get_time();
        s_need_report = true;
        return;
    }
    s_photo_on_panel = false;                 /* a question is on the panel, not a photo */
    app_logf("delete     : hold opened the confirm for %s\n", path);

    int ans = ask_delete(fb);
    if (ans == 1) {
        show_notice(NOTICE_DELETING, NOTICE_W, NOTICE_H, "deleting", false, 2000);
        bool ok = (unlink(path) == 0);
        if (ok) { order_drop((uint16_t)s_cur); live_scan(); }
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
        show_notice(NOTICE_CAMERA, NOTICE_W, NOTICE_H, "hash mismatch", true, 4000);
        return -1;
    }
    /* 0254: ask before storing.  The verified frame stays in PSRAM (s_rx) and is
     * composed into the off-screen buffer with the prompt below it; nothing is
     * written to the card until the answer is keep. */
    uint8_t *dst = s_buf[s_w];
    build_confirm_view(dst, s_rx, PROMPT_STRIP, PROMPT_STRIP_H);
    int64_t pub_us = esp_timer_get_time();
    bool prompt_up = publish(dst, "keep prompt");   /* photo + prompt on screen */
    if (prompt_up) s_photo_on_panel = false;
    if (!prompt_up) {
        app_logf("confirm    : prompt NOT published; frame dropped, nothing stored\n");
        s_cur = -1;
        /* the panel still shows the last countdown digit -- put the snapshot back so a
         * dropped frame cannot leave "1" sitting there (Copilot review, fourth pass),
         * and queue the same bounded retry if that handoff is also missed (5th pass) */
        if (!restore_previous("keep prompt not published")) {
            s_notice_until = 0;
            s_notice_restore = true;
            s_restore_tries = 0;
        }
        s_need_report = true;
        return -1;
    }
    app_logf("timing     : shoot->display %lld ms, fs->display %lld ms\n",
             s_shoot_us ? (long long)((pub_us - s_shoot_us) / 1000) : -1LL,
             s_fs_us ? (long long)((pub_us - s_fs_us) / 1000) : -1LL);

    app_logf("confirm    : library %u frames before the answer\n", (unsigned)s_lib_count);
    bool timed_out = false;
    bool keep = ask_keep(dst, &timed_out);
    if (!keep) {
        app_logf("live       : discarded on hold, nothing written, library still %u\n",
                 (unsigned)s_lib_count);
        show_notice(NOTICE_DELETED, NOTICE_W, NOTICE_H, "discarded", false, 4000);
        s_need_report = true;
        return 0;
    }

    /* keep, or the timeout kept it: show the clean photo, then store it (0245
     * rung 1 -- publish first, the ack still means stored). */
    uint8_t *clean = s_buf[s_w];
    memcpy(clean, s_rx, FB_BYTES);
    bool shown = publish(clean, "kept photo");
    if (shown) s_photo_on_panel = true;
    app_logf("confirm    : keeping (%s), storing%s\n",
             timed_out ? "30 s timeout" : "answered",
             shown ? "" : " (WARNING: clean photo not published -- panel still shows the prompt)");

    char path[64] = "(not stored)";
    int64_t st0 = esp_timer_get_time();
    bool stored = live_store(clean, path, sizeof(path));
    int64_t store_ms = (esp_timer_get_time() - st0) / 1000;
    app_logf("live       : frame ok, sent sha %s, stored sha %s, %s %s, library %u, store %lld ms\n",
             want_hex, got_hex, stored ? "written" : "NOT written", path,
             (unsigned)s_lib_count, (long long)store_ms);

    if (!stored) {
        app_logf("live       : STORE FAILED - shown but not kept, no ack sent\n");
        /* 0257 (Copilot review finding 1): the panel is showing a photograph that
         * is not in the library.  Leaving s_cur on the previous index would let a
         * hold open the delete confirm for -- and delete -- the previous file. */
        s_cur = -1;
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

    /* 0277: without a card the frame used to sit on the Nordtronics logo forever,
     * which is indistinguishable from a hang.  Put the reason on the panel, keep
     * it there, and keep trying the card in the loop below. */
    if (!s_mounted) {
        show_notice(NOTICE_SD, NOTICE_SD_W, NOTICE_SD_H, "no SD card", false, 86400000);
        splash_released();          /* the logo must give way to the error message */
        app_logf("sd         : no card at boot -- error on panel, retrying every %d s\n",
                 SD_RETRY_MS / 1000);
    }

    /* 0259: no placeholder any more.  The splash stays on the panel until the
     * first card photo is published, so the first slide must not wait out a dwell. */
    int64_t last_show = esp_timer_get_time() - (int64_t)DWELL_MS * 1000;
    int64_t last_mount_try = esp_timer_get_time();
    uint32_t show_idx = 0;
    int last_level = 1;

    for (;;) {
        int64_t now = esp_timer_get_time();
        int level = gpio_get_level(PIN_BTN);
        static bool armed = true;
        static bool press_used = false;    /* 0257: this press became a hold */
        static int64_t press_since = 0;    /* 0257: when the current press began */
        static bool press_delete_ok = false;  /* 0257: press began on the slideshow */

        int prev = last_level;
        if (level != last_level) last_level = level;
        if (level == 1) armed = true;      /* a press needs a seen release first */
        if (level == 0 && prev == 1) {
            press_used = false;
            press_since = now;
            /* 0257 (Copilot review finding 2): a press that begins while a notice
             * is on the panel must not open the delete confirm when the notice
             * expires under the still-held button -- s_cur can be stale there. */
            /* the gesture belongs to the slideshow: no notice up AND a photograph
             * already on the panel when the press starts (Copilot finding 3 -- a
             * hold begun at startup used to cross the first slideshow load) */
            press_delete_ok = (now > s_notice_until && s_cur >= 0 && s_photo_on_panel);
        }

        /* 0257: the 2 s hold is now the silent delete gesture, so the shoot fires
         * on the RELEASE of a short press instead of 30 ms into the press -- a
         * hold that also fired the camera could never be a silent menu.  The hold
         * only counts while a photograph is on the slideshow and no notice is up. */
        if (level == 0 && !press_used && !s_shooting && press_delete_ok && s_cur >= 0 &&
            s_photo_on_panel && now > s_notice_until && press_since != 0 &&
            now - press_since > (int64_t)HOLD_DELETE_MS * 1000) {
            press_used = true;
            press_since = 0;
            delete_current_photo(&show_idx, &last_show);
            settle_button();
            last_level = gpio_get_level(PIN_BTN);
            armed = (last_level == 1);
        } else if (level == 0 && !press_used && !s_shooting && press_since != 0 &&
                   now - press_since > (int64_t)HOLD_DELETE_MS * 1000) {
            /* a hold that could not open the confirm used to leave no trace at all */
            press_used = true;
            int64_t ignored_ms = (now - press_since) / 1000;
            press_since = 0;
            app_logf("delete     : hold of %lld ms ignored (s_cur %d, delete_ok %d, notice %d, shooting %d)\n",
                     (long long)ignored_ms, (int)s_cur, (int)press_delete_ok,
                     (int)(now <= s_notice_until), (int)s_shooting);
        } else if (level == 1 && prev == 0 && armed && !press_used && !s_shooting &&
                   press_since != 0 &&
                   now - press_since >= (int64_t)MIN_PRESS_MS * 1000 &&
                   now - press_since < (int64_t)HOLD_DELETE_MS * 1000) {
            press_used = true;             /* the press is spent either way */
            int64_t held_ms = (now - press_since) / 1000;   /* read BEFORE clearing */
            press_since = 0;
            armed = false;
            s_shooting = true;
            s_notice_until = 0;            /* 0254: a press overrides the notice */
            s_notice_restore = false;      /* 0256: and cancels a pending restore */
            app_logf("live       : BUTTON pressed (%lld ms), library %u frames\n",
                     (long long)held_ms, (unsigned)s_lib_count);
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
                if (outcome >= 0) app_logf("live       : photo received and on screen\n");
                /* 0254/0259: the press that answered the prompt must not arm
                 * another shoot; settle the button before trusting it again */
                settle_button();
                last_level = gpio_get_level(PIN_BTN);
                armed = (last_level == 1);
            } else {
                app_logf("live       : TIMEOUT waiting for the frame (12 s)\n");
                show_notice(NOTICE_CAMERA, NOTICE_W, NOTICE_H, "capture timeout", true, 4000);
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
            settle_button();
            last_level = gpio_get_level(PIN_BTN);
            armed = (last_level == 1);
            if (outcome == 1) { s_cur = (int32_t)(s_lib_max ? s_lib_max - 1 : -1); show_idx = 0; last_show = esp_timer_get_time(); }
            else if (outcome == 0) { last_show = esp_timer_get_time() - (int64_t)DWELL_MS * 1000; }
        }

        if (!s_shooting && s_lib_count && esp_timer_get_time() > s_notice_until &&
            esp_timer_get_time() - last_show > (int64_t)DWELL_MS * 1000) {
            last_show = esp_timer_get_time();
            show_next_shuffled();          /* 0259: shuffled, one of each per cycle */
        }

        /* 0277: while there is no card, try again now and then.  The task only asks
         * that a later boot works, but one mount attempt every 10 s costs nothing and
         * means a re-seated card recovers without a power cycle. */
        if (!s_mounted && now - last_mount_try > (int64_t)SD_RETRY_MS * 1000) {
            last_mount_try = now;
            fold_presacrifice();
            if (card_mount()) {
                s_mounted = true;
                live_scan();
                order_build();
                s_notice_until = 0;
                last_show = esp_timer_get_time() - (int64_t)DWELL_MS * 1000;
                app_logf("sd         : card came back, library %u frames\n",
                         (unsigned)s_lib_count);
            }
        }

        /* 0256: a transient notice hands the panel back to the previous photo */
        if (s_notice_restore && !s_shooting && esp_timer_get_time() > s_notice_until) {
            s_notice_restore = false;
            /* a restore that could not be published is retried a few times, then left
             * to the next slideshow frame rather than spinning on a dead scanout */
            if (!restore_previous("notice expired") && ++s_restore_tries < 3)
                s_notice_restore = true;
        }

        if (s_need_report && !s_shooting) { s_need_report = false; write_report(); }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
