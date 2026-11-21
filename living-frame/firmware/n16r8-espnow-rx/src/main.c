/*
 * 0230 rung 2 -- ESP-NOW receiver on the frame board (N16R8, ESP32-S3).
 *
 * The CAM sends a fixed, deterministic test transfer of PKT_COUNT packets of
 * PKT_LEN bytes each.  This app receives them, verifies every packet's pattern
 * against what the sender must have produced from its own sequence number, and
 * hashes the reassembled stream (packets in sequence order, zero-filled gaps) so
 * the host can compare it with the CAM-side SHA-256 printed on the CAM console.
 *
 * Reporting: the FT232R console on this bench is dead (0225), so every line is
 * also written to the `report` data partition at 0x310000 and read back with
 *   esptool --port /dev/ttyUSB0 --baud 460800 read_flash 0x310000 0x10000 report.bin
 * Header: uint32 magic '0''2''3''0', uint32 length, then the text.
 *
 * No display work: this build drives no panel.
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_partition.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "mbedtls/sha256.h"

#define PKT_LEN      240
#define PKT_COUNT    32
#define TOTAL_LEN    (PKT_LEN * PKT_COUNT)
#define CHANNEL      1
#define RADIO        0   /* wifi_phy_rate_t: 0 = 1 Mbps 802.11b (same on both ends) */

static char   g_report[8000];
static size_t g_report_len;

static uint8_t  g_buf[TOTAL_LEN];
static uint8_t  g_seen[PKT_COUNT];
static uint32_t g_tfer_id;
static uint16_t g_expect_pkts;
static int      g_rx_pkts;
static int      g_rx_bytes;
static int      g_bad_pattern;
static int      g_dup;
static int      g_out_of_range;
static int      g_first_seq = -1, g_last_seq = -1;
static volatile bool g_dirty;

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

/* the sender's pattern: byte i of packet seq = (seq * 7 + i * 3 + 0x5a) & 0xff */
static inline uint8_t pattern_byte(int seq, int i)
{
    return (uint8_t)(seq * 7 + i * 3 + 0x5a);
}

static const char *reason_name(uint8_t reason)
{
    switch (reason) {
    case ESP_NOW_SEND_SUCCESS: return "SUCCESS";
    default: return "FAIL";
    }
}

/* The report partition is this build's only log (the FT232R console is dead on
 * this bench), so a failed write must not be silent: every step is checked, the
 * read-back length is verified, and the outcome is counted.  Peer review
 * (2026-11-20) flagged the unchecked writes. */
static int g_report_errors;

static void write_report(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "report");
    uint32_t header[2] = { 0x30323330u, (uint32_t)g_report_len };  /* "0230" */
    if (part == NULL) { g_report_errors++; return; }
    if (sizeof(header) + g_report_len > part->size) { g_report_errors++; return; }

    esp_err_t e;
    if ((e = esp_partition_erase_range(part, 0, part->size)) != ESP_OK) {
        g_report_errors++; fputs("REPORT: report_erase_failed\n", stdout); return;
    }
    if ((e = esp_partition_write(part, 0, header, sizeof(header))) != ESP_OK) {
        g_report_errors++; fputs("REPORT: report_header_write_failed\n", stdout); return;
    }
    if ((e = esp_partition_write(part, sizeof(header), g_report, g_report_len)) != ESP_OK) {
        g_report_errors++; fputs("REPORT: report_body_write_failed\n", stdout); return;
    }
    /* read the header AND the whole body back: the text is the authoritative log,
     * so the check has to cover the bytes the host will parse, not just the length */
    static uint8_t check[sizeof(header) + sizeof(g_report)];
    if ((e = esp_partition_read(part, 0, check, sizeof(header) + g_report_len)) != ESP_OK) {
        g_report_errors++; fputs("REPORT: report_readback_failed\n", stdout); return;
    }
    if (memcmp(check, header, sizeof(header)) != 0 ||
        memcmp(check + sizeof(header), g_report, g_report_len) != 0) {
        g_report_errors++; fputs("REPORT: report_readback_mismatch\n", stdout);
    }
}

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len != PKT_LEN) {
        g_out_of_range++;
        return;
    }
    /* packet header: u16 seq, u16 expect_count, u32 transfer_id */
    uint16_t seq = (uint16_t)(data[0] | (data[1] << 8));
    uint16_t expect = (uint16_t)(data[2] | (data[3] << 8));
    uint32_t tid = (uint32_t)data[4] | ((uint32_t)data[5] << 8) |
                   ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24);

    if (g_tfer_id == 0) g_tfer_id = tid;
    if (tid != g_tfer_id) return;
    if (expect != PKT_COUNT) return;
    g_expect_pkts = expect;

    if (seq >= PKT_COUNT) { g_out_of_range++; return; }

    g_rx_pkts++;
    g_rx_bytes += len;
    if (g_first_seq < 0) g_first_seq = seq;
    g_last_seq = seq;
    if (g_seen[seq]) g_dup++;

    int pattern_ok = 1;
    for (int i = 8; i < PKT_LEN; i++) {
        if (data[i] != pattern_byte(seq, i)) { pattern_ok = 0; break; }
    }
    if (!pattern_ok) g_bad_pattern++;

    memcpy(g_buf + seq * PKT_LEN, data, PKT_LEN);
    g_seen[seq] = 1;
    g_dirty = true;

    (void)info;
}

/* Write the report until a write completes with no new error, so the
 * report_write_errors line stored on flash is not one write stale. */
static void finalize_report(void)
{
    size_t marker = g_report_len;
    app_logf("REPORT: report_write_errors=%d\n", g_report_errors);
    for (int i = 0; i < 3; i++) {
        int before = g_report_errors;
        write_report();
        if (g_report_errors == before) return;      /* the write that just happened was clean */
        g_report_len = marker;                      /* re-label with the new count */
        app_logf("REPORT: report_write_errors=%d\n", g_report_errors);
    }
    write_report();
}

static void sha256_hex(const uint8_t *data, size_t len, char out[65])
{
    uint8_t digest[32];
    mbedtls_sha256(data, len, digest, 0);
    for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", digest[i]);
    out[64] = 0;
}

/* access point-less station: no credentials, no association, just a channel */
static void wifi_start(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* plain 802.11b/g/n -- an ESP32 peer cannot talk the S3's LR mode, and the
     * rate must match on both sides or nothing is received at all */
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
    ESP_ERROR_CHECK(esp_wifi_set_channel(CHANNEL, WIFI_SECOND_CHAN_NONE));
}

void app_main(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    app_logf("REPORT: boot=n16r8-espnow-rx task=0230\n");
    app_logf("REPORT: mac=%02x:%02x:%02x:%02x:%02x:%02x channel=%d rate=1Mbps-11b\n",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], CHANNEL);
    app_logf("REPORT: expect pkts=%d len=%d total=%d pattern='(seq*7+i*3+0x5a)&0xff'\n",
             PKT_COUNT, PKT_LEN, TOTAL_LEN);

    wifi_start();
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(recv_cb));

    char sha[65];
    size_t expected_pkts = 0;
    int64_t deadline = 0;   /* set on the first packet */
    int64_t window_start = esp_log_timestamp();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(250));
        if (g_dirty) {
            g_dirty = false;
            write_report();
        }
        /* finish 6 s after the first packet, or 90 s after boot with nothing */
        int64_t now = esp_log_timestamp();
        if (deadline == 0 && g_rx_pkts > 0) deadline = now + 6000;
        if (deadline == 0 && now - window_start > 90000) deadline = now;
        if (deadline != 0 && now > deadline) break;
    }

    for (int i = 0; i < PKT_COUNT; i++) expected_pkts += g_seen[i];
    sha256_hex(g_buf, TOTAL_LEN, sha);

    app_logf("REPORT: rx_packets=%d distinct=%u rx_bytes=%d\n",
             g_rx_pkts, (unsigned)expected_pkts, g_rx_bytes);
    app_logf("REPORT: bad_pattern=%d dup=%d out_of_range=%d\n",
             g_bad_pattern, g_dup, g_out_of_range);
    app_logf("REPORT: tfer_id=0x%08x expected_packets=%d first_seq=%d last_seq=%d\n",
             (unsigned)g_tfer_id, (int)g_expect_pkts, g_first_seq, g_last_seq);
    app_logf("REPORT: sha256_rx=%s\n", sha);
    app_logf("REPORT: result=%s\n",
             (expected_pkts == PKT_COUNT && g_bad_pattern == 0) ? "espnow_rx_complete"
                                                                : "espnow_rx_incomplete");
    finalize_report();

    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
