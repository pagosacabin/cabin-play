/*
 * 0244 -- camera side.  Freenove ESP32-Wrover CAM (ESP32-D0WD-V3, GC0308).
 *
 * The frame board owns the button, the countdown and the photo library.  This
 * board is a camera: it waits for a SHOOT trigger, takes one settled frame,
 * runs the colour chain, and sends the packed frame back.  It keeps NO library
 * -- no PSRAM ring retention, no camera-side SD writes.
 *
 * Trigger (frame -> camera):   'F','T'
 * Frame   (camera -> frame):   'F','S' len32 / 'F','D' idx16 + <=240 B / 'F','E' sha256
 * Ack     (frame -> camera):   'F','A' sha256      (logged only)
 *
 * Colour chain (unchanged from the build proven this morning):
 *   grey-world white balance at WB_STRENGTH, smoothed across frames
 *   256-entry gamma LUT  (level v emits (v/3)^2.2 of full scale)
 *   4x4 ordered Bayer dither on the level fraction
 *
 * Warp-up: the GC0308's first frames after an idle spell are badly exposed, so
 * WARMUP_FRAMES are taken and discarded before the frame that is sent.
 *
 * Sensor pin map is CAMERA_MODEL_WROVER_KIT from the kit's camera_pins.h.
 * Nothing here needs editing to change pins: board_config.h is the reference.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_camera.h>
#include <esp_heap_caps.h>
#include "secrets.h"
#include "mbedtls/sha256.h"

/* frame board's own WiFi station MAC, from the 0225 probe report */
static uint8_t PEER[6] = { 0xC4, 0x9E, 0x7E, 0x08, 0x6D, 0xF4 };
static const uint8_t CHANNEL = 6;

#define FB_W 640
#define FB_H 480
#define FB_BYTES (FB_W * FB_H)
#define PAYLOAD 240
#define HDR 4
#define WARMUP_FRAMES 3

#ifndef WB_STRENGTH
#define WB_STRENGTH 0.5f
#endif

typedef struct { uint8_t lo, hi, thr; } qstep_t;
static qstep_t s_q[3][256];
static uint8_t s_bayer[4][4];
static float   s_gain[3] = { 1.0f, 1.0f, 1.0f };

static uint8_t *s_frame;
static uint16_t s_sess;          /* per-boot session id (0244 review finding 6) */
static volatile bool s_shoot;
static volatile uint32_t s_shots;
static volatile bool s_cb_done, s_last_ok;
static uint32_t s_ok, s_fail;
static volatile uint32_t s_cb_count;   /* callbacks seen, for windowed sending */

#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  21
#define SIOD_GPIO_NUM  26
#define SIOC_GPIO_NUM  27
#define Y9_GPIO_NUM    35
#define Y8_GPIO_NUM    34
#define Y7_GPIO_NUM    39
#define Y6_GPIO_NUM    36
#define Y5_GPIO_NUM    19
#define Y4_GPIO_NUM    18
#define Y3_GPIO_NUM    5
#define Y2_GPIO_NUM    4
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM  23
#define PCLK_GPIO_NUM  22

static void on_sent(const uint8_t *mac, esp_now_send_status_t st)
{
    (void)mac;
    s_last_ok = (st == ESP_NOW_SEND_SUCCESS);
    if (s_last_ok) s_ok++; else s_fail++;
    s_cb_done = true;
    s_cb_count++;
}

static void on_recv(const uint8_t *mac, const uint8_t *data, int len)
{
    (void)mac;
    if (len >= 2 && data[0] == 'F' && data[1] == 'T') {
        s_shoot = true;                      /* SHOOT trigger from the frame */
        return;
    }
    if (len >= 34 && data[0] == 'F' && data[1] == 'A') {
        char hex[65];
        for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", data[2 + i]);
        hex[64] = 0;
        Serial.printf("peer ack: sha %s\n", hex);
    }
}

/* 0247: a chunk that comes back ESP_NOW_SEND_FAIL is retried in place, up to
 * three attempts.  The frame-level SHA still decides whether the picture is
 * accepted; this just stops a single lost chunk from costing a whole frame. */
static bool send_pkt(const uint8_t *buf, size_t len)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        s_cb_done = false;
        s_last_ok = false;
        if (esp_now_send(PEER, buf, len) != ESP_OK) { s_fail++; continue; }
        uint32_t t0 = millis();
        while (!s_cb_done && millis() - t0 < 40) delay(1);
        if (s_cb_done && s_last_ok) return true;
    }
    return false;
}

static void build_quant_table(void)
{
    const float L[4] = { 0.0f, 0.0891f, 0.4097f, 1.0f };   /* (v/3)^2.2 */
    for (int c = 0; c < 256; c++) {
        float T = powf(c / 255.0f, 2.2f);
        int v = 0;
        while (v < 3 && T > L[v + 1]) v++;
        float f = (v == 3) ? 0.0f : (T - L[v]) / (L[v + 1] - L[v]);
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        uint8_t thr = (uint8_t)(f * 255.0f);
        uint8_t hi = (uint8_t)(v == 3 ? 3 : v + 1);
        for (int ch = 0; ch < 3; ch++) s_q[ch][c] = (qstep_t){ (uint8_t)v, hi, thr };
    }
    static const uint8_t B[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 },
                                     { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            s_bayer[y][x] = (uint8_t)((B[y][x] + 0.5f) * 255.0f / 16.0f);
}

/* 0247: gains are computed fresh for every shot.  The old 60/40 smoothing against
 * the previous frame was a streaming leftover -- one press per minute means it
 * smoothed across scenes, not within one.  And a black frame divides by zero:
 * ma/mr is inf/NaN, the 0.5..2.0 clamps are false for NaN, so the filter used to
 * carry NaN until reboot.  Bail to identity on any near-zero channel. */
static void wb_update(const uint8_t *cam)
{
    if (WB_STRENGTH <= 0.0f) return;
    uint32_t sr = 0, sg = 0, sb = 0, n = 0;
    for (size_t px = 0; px < (size_t)FB_W * FB_H; px += 16) {
        size_t i = px * 2;
        uint16_t p = (uint16_t)(((uint16_t)cam[i] << 8) | cam[i + 1]);
        sr += (p >> 11) & 0x1F;
        sg += (p >> 5) & 0x3F;
        sb += p & 0x1F;
        n++;
    }
    float mr = sr / (float)n, mg = sg / (float)n, mb = sb / (float)n;
    if (mr < 1.0f || mg < 1.0f || mb < 1.0f) {       /* black or near-black frame */
        s_gain[0] = s_gain[1] = s_gain[2] = 1.0f;
        return;
    }
    float ma = (mr + mg + mb) / 3.0f;
    float raw[3] = { ma / mr, ma / mg, ma / mb };
    for (int c = 0; c < 3; c++) {
        float g = 1.0f + WB_STRENGTH * (raw[c] - 1.0f);
        if (!(g > 0.5f)) g = 0.5f;                   /* NaN-safe: NaN -> 0.5 */
        if (g > 2.0f) g = 2.0f;
        s_gain[c] = g;                               /* no carry-over between shots */
    }
}

static inline uint8_t wb_code(int ch, uint8_t code)
{
    float v = code * s_gain[ch];
    if (v < 0.0f) v = 0.0f;
    if (v > 255.0f) v = 255.0f;
    return (uint8_t)(v + 0.5f);
}

static void build_raster(const uint8_t *cam, uint8_t *out)
{
    for (int ry = 0; ry < FB_H; ry++) {
        for (int rx = 0; rx < FB_W; rx++) {
            size_t i = ((size_t)ry * FB_W + rx) * 2;   /* RGB565 is big-endian */
            uint16_t p = (uint16_t)(((uint16_t)cam[i] << 8) | cam[i + 1]);
            uint8_t r = wb_code(0, (uint8_t)((((p >> 11) & 0x1F) * 255) / 31));
            uint8_t g = wb_code(1, (uint8_t)((((p >> 5) & 0x3F) * 255) / 63));
            uint8_t b = wb_code(2, (uint8_t)(((p & 0x1F) * 255) / 31));
            uint8_t t = s_bayer[ry & 3][rx & 3];
            qstep_t qr = s_q[0][r], qg = s_q[1][g], qb = s_q[2][b];
            uint8_t lr = (t < qr.thr) ? qr.hi : qr.lo;
            uint8_t lg = (t < qg.thr) ? qg.hi : qg.lo;
            uint8_t lb = (t < qb.thr) ? qb.hi : qb.lo;
            out[ry * FB_W + rx] = (uint8_t)((lb << 4) | (lg << 2) | lr);
        }
    }
}

static void sha_hex(const uint8_t *d, size_t n, char out[65])
{
    uint8_t h[32];
    mbedtls_sha256(d, n, h, 0);
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", h[i]);
    out[64] = 0;
}

static bool camera_start(void)
{
    camera_config_t c = {};
    c.ledc_channel = LEDC_CHANNEL_0; c.ledc_timer = LEDC_TIMER_0;
    c.pin_d0 = Y2_GPIO_NUM; c.pin_d1 = Y3_GPIO_NUM; c.pin_d2 = Y4_GPIO_NUM;
    c.pin_d3 = Y5_GPIO_NUM; c.pin_d4 = Y6_GPIO_NUM; c.pin_d5 = Y7_GPIO_NUM;
    c.pin_d6 = Y8_GPIO_NUM; c.pin_d7 = Y9_GPIO_NUM;
    c.pin_xclk = XCLK_GPIO_NUM; c.pin_pclk = PCLK_GPIO_NUM;
    c.pin_vsync = VSYNC_GPIO_NUM; c.pin_href = HREF_GPIO_NUM;
    c.pin_sccb_sda = SIOD_GPIO_NUM; c.pin_sccb_scl = SIOC_GPIO_NUM;
    c.pin_pwdn = PWDN_GPIO_NUM; c.pin_reset = RESET_GPIO_NUM;
    c.xclk_freq_hz = 10000000;
    c.pixel_format = PIXFORMAT_RGB565;      /* GC0308 has no JPEG engine */
    c.frame_size   = FRAMESIZE_VGA;
    c.fb_count     = 1;
    c.grab_mode    = CAMERA_GRAB_LATEST;
    c.fb_location  = CAMERA_FB_IN_PSRAM;
    if (esp_camera_init(&c) != ESP_OK) return false;
    sensor_t *s = esp_camera_sensor_get();
    s->set_brightness(s, 1);
    s->set_saturation(s, -1);
    return true;
}

static void send_frame(void)
{
    char hex[65];
    sha_hex(s_frame, FB_BYTES, hex);

    uint8_t pkt[HDR + 2 + PAYLOAD];
    pkt[0] = 'F'; pkt[1] = 'S';
    pkt[2] = (uint8_t)(FB_BYTES & 0xFF);
    pkt[3] = (uint8_t)((FB_BYTES >> 8) & 0xFF);
    pkt[4] = (uint8_t)((FB_BYTES >> 16) & 0xFF);
    pkt[5] = (uint8_t)((FB_BYTES >> 24) & 0xFF);
    pkt[6] = (uint8_t)(s_sess & 0xFF);
    pkt[7] = (uint8_t)(s_sess >> 8);
    send_pkt(pkt, 8);

    /* 0245 rung 3, attempt 1: an 8-deep window of in-flight chunks measured
     * 9.6 s with 435-1696 failures per frame -- the radio drops under that
     * pressure and the retries make it slower, not faster.  Reverted to
     * stop-and-wait, which is the shape that holds fail 0.  The PHY rate change
     * below is measured separately. */
    uint32_t t0 = millis(), sent = 0;
    for (size_t off = 0, idx = 0; off < FB_BYTES; off += PAYLOAD, idx++, sent++) {
        pkt[0] = 'F'; pkt[1] = 'D';
        pkt[2] = (uint8_t)(idx & 0xFF); pkt[3] = (uint8_t)((idx >> 8) & 0xFF);
        pkt[4] = (uint8_t)(s_sess & 0xFF);          /* session id on every chunk */
        pkt[5] = (uint8_t)(s_sess >> 8);
        size_t n = (FB_BYTES - off) < PAYLOAD ? (FB_BYTES - off) : PAYLOAD;
        memcpy(pkt + HDR + 2, s_frame + off, n);
        send_pkt(pkt, HDR + 2 + n);
        if ((idx & 15) == 0) delay(1);
    }
    uint8_t end[36];
    end[0] = 'F'; end[1] = 'E';
    end[2] = (uint8_t)(s_sess & 0xFF);
    end[3] = (uint8_t)(s_sess >> 8);
    for (int i = 0; i < 32; i++) {
        char b[3] = { hex[i * 2], hex[i * 2 + 1], 0 };
        end[4 + i] = (uint8_t)strtol(b, NULL, 16);
    }
    send_pkt(end, sizeof(end));

    Serial.printf("sent    : %u pkts, %u ms, ok %u fail %u, sha %s\n",
                  (unsigned)sent, (unsigned)(millis() - t0),
                  (unsigned)s_ok, (unsigned)s_fail, hex);
}

static void take_shot(const char *why)
{
    /* discard warm-up frames: the GC0308's first frames after idle are dark */
    for (int i = 0; i < WARMUP_FRAMES; i++) {
        camera_fb_t *w = esp_camera_fb_get();
        if (w) esp_camera_fb_return(w);
        delay(40);
    }
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { Serial.println("capture : failed"); return; }
    wb_update(fb->buf);
    build_raster(fb->buf, s_frame);
    esp_camera_fb_return(fb);
    s_shots++;
    s_sess++;                   /* 0247: one session id per frame, not per boot */
    Serial.printf("shoot   : %s, shot %u, warmup %d, wb r %.3f g %.3f b %.3f (strength %.2f)\n",
                  why, (unsigned)s_shots, WARMUP_FRAMES,
                  (double)s_gain[0], (double)s_gain[1], (double)s_gain[2], (double)WB_STRENGTH);
    send_frame();
}

void setup(void)
{
    Serial.begin(115200);
    delay(200);
    Serial.println("\n=== Freenove CAM (0244): waits for SHOOT, sends one frame ===");
    s_frame = (uint8_t *)heap_caps_malloc(FB_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_frame) { Serial.println("no PSRAM for frame"); return; }
    s_sess = (uint16_t)(esp_random() & 0xFFFF);
    build_quant_table();
    if (!camera_start()) { Serial.println("camera  : init failed"); return; }
    Serial.printf("camera  : pid 0x%02x, VGA RGB565, no library on this side\n",
                  esp_camera_sensor_get()->id.PID);

    /* 0247: the association stays, and that is a measured decision, not inertia.
     * Running the STA unassociated on the same channel was tried with power save
     * off and TX power forced to 19.5 dBm: fail 0, but 6544-6959 ms per frame
     * against 1692-2097 ms while associated.  The AP's beacons and the driver's
     * associated rate table are worth more than the credentials cost. */
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; i++) delay(500);
    Serial.printf("wifi    : %s rssi %d ch %d\n", WiFi.localIP().toString().c_str(),
                  WiFi.RSSI(), WiFi.channel());
    esp_wifi_set_channel(CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (esp_now_init() != ESP_OK) { Serial.println("esp_now_init failed"); return; }
    esp_now_register_send_cb(on_sent);
    esp_now_register_recv_cb(on_recv);
    /* PHY rate: 24 Mbps on the STA interface if the stack exposes it */
    esp_err_t rerr = esp_wifi_config_espnow_rate(WIFI_IF_STA, WIFI_PHY_RATE_24M);
    Serial.printf("rate    : espnow 24M -> %s\n", esp_err_to_name(rerr));
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, PEER, 6);
    peer.channel = CHANNEL;
    if (esp_now_add_peer(&peer) != ESP_OK) { Serial.println("add_peer failed"); return; }
    Serial.printf("link    : peer %02x:%02x:%02x:%02x:%02x:%02x ch %d, waiting for 'FT'\n",
                  PEER[0], PEER[1], PEER[2], PEER[3], PEER[4], PEER[5], CHANNEL);
}

void loop(void)
{
    if (Serial.available()) {                 /* bench hook: 'c' shoots without the frame */
        int ch = Serial.read();
        if (ch == 'c' || ch == 'C') { Serial.println("serial  : shoot"); take_shot("serial"); }
    }
    if (s_shoot) {
        s_shoot = false;
        take_shot("trigger");
    }
    delay(5);
}
