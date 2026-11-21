/*
 * 0230 rung 2 -- ESP-NOW transmitter on the ESP32-CAM (AI-Thinker, ESP32).
 *
 * Sends a deterministic test transfer to the frame board (MAC in FRAME_MAC):
 * PKT_COUNT packets of PKT_LEN bytes.  Packet layout (matches
 * living-frame/firmware/n16r8-espnow-rx/src/main.c):
 *   u16 seq | u16 expect_count | u32 transfer_id | payload
 *   payload byte at offset i (8..239) = (seq*7 + i*3 + 0x5a) & 0xff
 * The whole concatenation's SHA-256 is printed here, so the frame board's
 * reassembled hash can be compared against the sender's own value.
 *
 * No camera is involved: this rung proves the radio link, not the capture.
 * The console is the CH340 uplink at 115200.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include "mbedtls/sha256.h"

#define PKT_LEN    240
#define PKT_COUNT  32
#define TOTAL_LEN  (PKT_LEN * PKT_COUNT)
#define CHANNEL    1

static const uint8_t FRAME_MAC[6] = { 0xc4, 0x9e, 0x7e, 0x08, 0x6d, 0xf4 };
static const uint32_t TFR_ID = 0x0230BEEFu;

static uint8_t  g_buf[TOTAL_LEN];
static volatile int g_sent_ok;
static volatile int g_sent_fail;
/* Per-sequence ACK bookkeeping: the status callback does not say which packet
 * it belongs to, so sends are issued one at a time and the sequence in flight
 * is recorded here.  Peer review (2026-11-20) was right that counting ACKs
 * alone can hide a repeated sequence or a missing one. */
static volatile int      g_inflight_seq = -1;
static volatile uint8_t  g_ack_ok[PKT_COUNT];
static volatile uint8_t  g_ack_fail[PKT_COUNT];

static inline uint8_t pattern_byte(int seq, int i)
{
    return (uint8_t)(seq * 7 + i * 3 + 0x5a);
}

static void sha256_hex(const uint8_t *data, size_t len, char out[65])
{
    uint8_t digest[32];
    mbedtls_sha256(data, len, digest, 0);
    for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", digest[i]);
    out[64] = 0;
}

static void on_sent(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    int seq = g_inflight_seq;
    if (status == ESP_NOW_SEND_SUCCESS) {
        g_sent_ok++;
        if (seq >= 0 && seq < PKT_COUNT) g_ack_ok[seq] = 1;
    } else {
        g_sent_fail++;
        if (seq >= 0 && seq < PKT_COUNT) g_ack_fail[seq] = 1;
    }
}

void setup()
{
    Serial.begin(115200);
    delay(300);

    uint8_t mac[6];
    WiFi.macAddress(mac);
    Serial.printf("REPORT: boot=esp32cam-espnow-tx task=0230\n");
    Serial.printf("REPORT: mac=%02x:%02x:%02x:%02x:%02x:%02x channel=%d peer=%02x:%02x:%02x:%02x:%02x:%02x\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], CHANNEL,
                  FRAME_MAC[0], FRAME_MAC[1], FRAME_MAC[2],
                  FRAME_MAC[3], FRAME_MAC[4], FRAME_MAC[5]);

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    esp_err_t ch = esp_wifi_set_channel(CHANNEL, WIFI_SECOND_CHAN_NONE);
    Serial.printf("REPORT: set_channel=%d\n", (int)ch);

    if (esp_now_init() != ESP_OK) {
        Serial.printf("REPORT: result=espnow_init_failed\n");
        return;
    }
    esp_now_register_send_cb(on_sent);

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, FRAME_MAC, 6);
    peer.channel = CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    esp_err_t perr = esp_now_add_peer(&peer);
    Serial.printf("REPORT: add_peer=%d (0=OK)\n", (int)perr);
    if (perr != ESP_OK) {
        Serial.printf("REPORT: result=espnow_peer_failed\n");
        return;
    }

    /* build the whole transfer once, hash it, then send packet by packet */
    for (int seq = 0; seq < PKT_COUNT; seq++) {
        uint8_t *p = g_buf + seq * PKT_LEN;
        memset(p, 0, PKT_LEN);
        p[0] = seq & 0xff; p[1] = (seq >> 8) & 0xff;
        p[2] = PKT_COUNT & 0xff; p[3] = (PKT_COUNT >> 8) & 0xff;
        p[4] = TFR_ID & 0xff; p[5] = (TFR_ID >> 8) & 0xff;
        p[6] = (TFR_ID >> 16) & 0xff; p[7] = (TFR_ID >> 24) & 0xff;
        for (int i = 8; i < PKT_LEN; i++) p[i] = pattern_byte(seq, i);
    }
    char sha[65];
    sha256_hex(g_buf, TOTAL_LEN, sha);

    Serial.printf("REPORT: transfer_id=0x%08x pkts=%d len=%d total=%d\n",
                  (unsigned)TFR_ID, PKT_COUNT, PKT_LEN, TOTAL_LEN);
    Serial.printf("REPORT: sha256_tx=%s\n", sha);
    delay(1500);   /* let the receiver come up / host start reading */

    for (int attempt = 0; attempt < 2; attempt++) {
        int ok0 = g_sent_ok, fail0 = g_sent_fail;
        for (int seq = 0; seq < PKT_COUNT; seq++) {
            g_inflight_seq = seq;
            esp_err_t e = esp_now_send(FRAME_MAC, g_buf + seq * PKT_LEN, PKT_LEN);
            if (e != ESP_OK) { g_sent_fail++; g_ack_fail[seq] = 1; }
            delay(30);
        }
        g_inflight_seq = -1;
        delay(300);
        Serial.printf("REPORT: pass=%d send_ok=%d send_fail=%d\n",
                      attempt + 1, g_sent_ok - ok0, g_sent_fail - fail0);
    }

    int seq_acked = 0, seq_missing = 0;
    for (int seq = 0; seq < PKT_COUNT; seq++) {
        if (g_ack_ok[seq]) seq_acked++;
        else { seq_missing++; if (seq_missing <= 8) Serial.printf("REPORT: missing_seq=%d\n", seq); }
    }
    Serial.printf("REPORT: send_ok_total=%d send_fail_total=%d\n", g_sent_ok, g_sent_fail);
    Serial.printf("REPORT: seq_acked=%d seq_missing=%d of %d\n", seq_acked, seq_missing, PKT_COUNT);
    /* success now means every sequence number was individually ACKed */
    Serial.printf("REPORT: result=%s\n",
                  (seq_missing == 0) ? "espnow_tx_sent" : "espnow_tx_incomplete");
    Serial.printf("REPORT: done\n");
}

void loop()
{
    /* one-shot test build: everything was reported in setup() */
    delay(5000);
}
