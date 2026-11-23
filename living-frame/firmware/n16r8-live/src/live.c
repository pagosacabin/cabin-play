/*
 * 0244 -- frame side live loop: button, countdown, SHOOT trigger, ESP-NOW
 * receive, SD library write, and the ~10 s slideshow over that library.
 *
 * Wiring (frame board YD-ESP32-S3 N16R8):
 *   SD   SCK 12 / MOSI 11 / MISO 13 / CS 10   (0241's proven pins, 400 kHz mount)
 *   VGA  data 5,4,7,6,16,15 / hsync 17 / vsync 18
 *   Button  GPIO14 to GND, internal pull-up, active-low   (the pin map records 14 as spare)
 *
 * Flow, and nothing more:
 *   press -> panel shows 3, 2, 1 (one per second) -> send 'F','T' to the camera
 *   -> camera captures, filters, sends 1,280 chunks -> this side verifies the
 *   SHA-256, writes the frame to /sdcard/LIVE/LIVE####.BIN, shows it, and
 *   resumes cycling the whole LIVE library at DWELL_MS.
 *
 * The library is deliberately separate from 0243's /sdcard/FRAMES (800x600) and
 * from /NEWCARD.BIN: this build is 640x480 and never touches either.
 * Nothing here formats a card: format_if_mount_failed is false throughout.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>

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
#include "mbedtls/sha256.h"
#include "esp_lcd_panel_ops.h"

extern uint8_t *s_fb;                          /* main.c: the live framebuffer */
extern void app_logf(const char *fmt, ...);
extern void write_report(void);

#define LIVE_TAG "0244"
#define PIN_SCK   12
#define PIN_MOSI  11
#define PIN_MISO  13
#define PIN_CS    10
#define PIN_BTN   14
#define SD_MOUNT  "/sdcard"
#define LIVE_DIR  "/sdcard/LIVE"
#define DWELL_MS  10000
#define PAYLOAD   240
#define CHANNEL   6

#ifndef OUT_W
#define OUT_W 640
#endif
#ifndef OUT_H
#define OUT_H 480
#endif
#define FB_BYTES ((uint32_t)OUT_W * (uint32_t)OUT_H)

static uint8_t s_cam_mac[6] = { 0xF0, 0x16, 0x1D, 0xAE, 0x2B, 0x20 };
static uint8_t *s_rx;
static volatile uint32_t s_rx_expect, s_rx_got;
static volatile bool s_rx_active, s_frame_ready, s_shooting, s_need_report;
static volatile uint32_t s_lib_count, s_shown_sha_wait;
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
/* ---- 0239 fold ----------------------------------------------------------
 * This card does not answer the stock driver cold: it needs 74+ clocks with CS
 * high and one throwaway CMD0 on the raw bus first, then the bus released so the
 * driver can initialise SPI2 itself.  Value and intent are 0239's; without it
 * esp_vfs_fat_sdspi_mount returns ESP_ERR_TIMEOUT (263) -- which is exactly what
 * the first 0244 run logged.
 */
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
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
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
    host.max_freq_khz = 400;               /* 0241/0243 proved 400 kHz on this rig */

    esp_vfs_fat_mount_config_t mcfg = {
        .format_if_mount_failed = false,   /* never formats */
        .max_files = 4,
        .allocation_unit_size = 0,
    };
    esp_err_t err = esp_vfs_fat_sdspi_mount(SD_MOUNT, &host, &slot, &mcfg, &s_card);
    if (err != ESP_OK) {
        app_logf("live       : SD mount failed: %s (%d)\n", esp_err_to_name(err), (int)err);
        spi_bus_free(SPI2_HOST);     /* release so the next attempt's fold can init */
        return false;
    }
    app_logf("live       : SD mounted, %.*s psn=0x%08x, %llu MB\n",
             8, s_card->cid.name, (unsigned)s_card->cid.serial,
             (unsigned long long)((uint64_t)s_card->csd.capacity * s_card->csd.sector_size
                                  / (1024 * 1024)));
    mkdir(LIVE_DIR, 0777);
    return true;
}

static uint32_t live_scan(void)
{
    DIR *d = opendir(LIVE_DIR);
    if (!d) return 0;
    uint32_t n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (e->d_name[0] != '.' && strstr(e->d_name, ".BIN")) n++;
    closedir(d);
    return n;
}

static bool live_store(const uint8_t *frame, char *path_out, size_t path_len)
{
    if (!s_mounted) return false;
    snprintf(path_out, path_len, LIVE_DIR "/LIVE%04u.BIN", (unsigned)s_lib_count);
    FILE *f = fopen(path_out, "wb");
    if (!f) { app_logf("live       : cannot open %s\n", path_out); return false; }
    size_t wr = fwrite(frame, 1, FB_BYTES, f);
    fclose(f);
    if (wr != FB_BYTES) {
        app_logf("live       : short write %u of %u\n", (unsigned)wr, (unsigned)FB_BYTES);
        return false;
    }
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
    size_t rd = fread(s_fb, 1, FB_BYTES, f);
    fclose(f);
    return rd == FB_BYTES;
}

/* ---- countdown ---------------------------------------------------------- */
/* 3x5 block digits, scaled -- no font project, just enough to read across a room */
static const uint8_t DIG[3][5] = {
    { 0x7, 0x1, 0x3, 0x1, 0x7 },   /* 3 */
    { 0x7, 0x1, 0x7, 0x4, 0x7 },   /* 2 */
    { 0x2, 0x6, 0x2, 0x2, 0x7 },   /* 1 */
};

static void draw_countdown(int step)   /* step 3,2,1 -> index 0,1,2 */
{
    const int blk = 36, gw = 3 * blk, gh = 5 * blk;
    memset(s_fb, 0x00, FB_BYTES);                       /* black card */
    int x0 = (OUT_W - gw) / 2, y0 = (OUT_H - gh) / 2;
    const uint8_t *g = DIG[3 - step];
    for (int r = 0; r < 5; r++)
        for (int c = 0; c < 3; c++)
            if (g[r] & (0x4 >> c))
                for (int y = 0; y < blk; y++) {
                    uint8_t *row = s_fb + (size_t)(y0 + r * blk + y) * OUT_W + x0 + c * blk;
                    memset(row, 0x3f, blk);              /* white in RGB222 */
                }
}

/* ---- esp-now ------------------------------------------------------------ */
static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    (void)info;
    if (len < 4) return;
    if (data[0] == 'F' && data[1] == 'S' && len >= 6) {
        s_rx_expect = (uint32_t)data[2] | ((uint32_t)data[3] << 8) |
                      ((uint32_t)data[4] << 16) | ((uint32_t)data[5] << 24);
        s_rx_got = 0;
        s_rx_active = (s_rx_expect == FB_BYTES);
    } else if (data[0] == 'F' && data[1] == 'D' && s_rx_active) {
        uint32_t idx = (uint32_t)data[2] | ((uint32_t)data[3] << 8);
        size_t off = (size_t)idx * PAYLOAD, n = (size_t)len - 4;
        if (off + n <= FB_BYTES) { memcpy(s_rx + off, data + 4, n); s_rx_got += n; }
    } else if (data[0] == 'F' && data[1] == 'E' && s_rx_active && len >= 34) {
        uint8_t hash[32];
        mbedtls_sha256(s_rx, FB_BYTES, hash, 0);
        s_rx_active = false;
        if (memcmp(hash, data + 2, 32) == 0) {
            char got_hex[65], want_hex[65];
            for (int i = 0; i < 32; i++) {
                sprintf(got_hex + i * 2, "%02x", hash[i]);
                sprintf(want_hex + i * 2, "%02x", data[2 + i]);
            }
            got_hex[64] = want_hex[64] = 0;
            memcpy(s_fb, s_rx, FB_BYTES);
            char path[64] = "(not stored)";
            bool stored = live_store(s_rx, path, sizeof(path));
            app_logf("live       : frame ok, sent sha %s, stored sha %s, %s %s, library %u\n",
                     want_hex, got_hex, stored ? "written" : "NOT written", path,
                     (unsigned)s_lib_count);
            s_frame_ready = true;
            s_need_report = true;   /* persisted from the loop, not the callback:
                                     * a flash write in the recv path can fault the
                                     * bounce ISR that is reading PSRAM */
            uint8_t ack[34];
            ack[0] = 'F'; ack[1] = 'A';
            memcpy(ack + 2, hash, 32);
            esp_now_send(s_cam_mac, ack, sizeof(ack));
        } else {
            app_logf("live       : frame REJECTED (sha mismatch), got %u of %u\n",
                     (unsigned)s_rx_got, (unsigned)s_rx_expect);
        }
    }
}

static bool shoot(void)
{
    uint8_t msg[2] = { 'F', 'T' };
    if (esp_now_send(s_cam_mac, msg, sizeof(msg)) != ESP_OK) return false;
    app_logf("live       : SHOOT sent to %02x:%02x:%02x:%02x:%02x:%02x\n",
             s_cam_mac[0], s_cam_mac[1], s_cam_mac[2], s_cam_mac[3], s_cam_mac[4], s_cam_mac[5]);
    return true;
}

/* ---- the loop ----------------------------------------------------------- */
void live_start(esp_lcd_panel_handle_t panel)
{
    (void)panel;
    s_rx = (uint8_t *)heap_caps_malloc(FB_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_rx) { app_logf("live       : no PSRAM for rx buffer\n"); return; }

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
    app_logf("live       : esp-now on ch %d, peer %02x:%02x:%02x:%02x:%02x:%02x\n",
             CHANNEL, s_cam_mac[0], s_cam_mac[1], s_cam_mac[2], s_cam_mac[3],
             s_cam_mac[4], s_cam_mac[5]);

    /* 0239: first-init on this rig is intermittent (11/19 cold, 14/14 with
     * retries).  Each attempt re-runs the fold and releases the bus on failure,
     * so a CRC/timeout on attempt 1 is not a dead card. */
    for (int attempt = 1; attempt <= 6 && !s_mounted; attempt++) {
        fold_presacrifice();
        s_mounted = card_mount();
        if (!s_mounted) {
            app_logf("live       : mount attempt %d failed, retrying\n", attempt);
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
    s_lib_count = s_mounted ? live_scan() : 0;
    app_logf("live       : %s library holds %u frames at %s\n",
             s_mounted ? "SD" : "no-SD", (unsigned)s_lib_count, LIVE_DIR);
    write_report();

    int64_t last_show = esp_timer_get_time();
    uint32_t show_idx = 0;
    int last_level = 1;
    int64_t level_since = esp_timer_get_time();

    for (;;) {
        int level = gpio_get_level(PIN_BTN);
        int64_t now = esp_timer_get_time();

        /* debounced falling edge: press = 1 -> 0 held for 30 ms */
        if (level != last_level) { last_level = level; level_since = now; }
        if (level == 0 && now - level_since > 30000 && !s_shooting) {
            s_shooting = true;                       /* presses during the flow are ignored */
            app_logf("live       : BUTTON pressed, library %u frames\n", (unsigned)s_lib_count);
            for (int step = 3; step >= 1; step--) {
                draw_countdown(step);
                app_logf("live       : countdown %d\n", step);
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            s_frame_ready = false;
            shoot();
            int64_t deadline = esp_timer_get_time() + 30000000LL;   /* 30 s budget */
            while (!s_frame_ready && esp_timer_get_time() < deadline)
                vTaskDelay(pdMS_TO_TICKS(100));
            app_logf("live       : %s\n", s_frame_ready ? "photo received and on screen"
                                                        : "TIMEOUT waiting for the frame");
            s_shooting = false;
            last_show = esp_timer_get_time();
            show_idx = 0;
            write_report();
        }

        /* slideshow: one library image per DWELL_MS, looping */
        if (!s_shooting && s_lib_count && esp_timer_get_time() - last_show > (int64_t)DWELL_MS * 1000) {
            last_show = esp_timer_get_time();
            if (show_idx >= s_lib_count) show_idx = 0;
            if (live_load(show_idx))
                app_logf("live       : show %u of %u\n", (unsigned)(show_idx + 1), (unsigned)s_lib_count);
            show_idx++;
        }
        if (s_need_report && !s_shooting) { s_need_report = false; write_report(); }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
