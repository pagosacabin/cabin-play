// 0230 rung 1 — ESP32-CAM (AI-Thinker, OV2640) capture + SD sink.
//
// What this proves, in order, with REPORT: lines on the console:
//   1. the OV2640 delivers a frame through the DVP bus (sensor PID read back),
//   2. that frame is a real JPEG (SOI FFD8 ... EOI FFD9) of a stated size,
//   3. a SHA-256 of the camera-side bytes, which is the number every later rung
//      (ESP-NOW chunked transfer, frame-board SD file) must reproduce,
//   4. the same bytes written to and read back from the CAM's own microSD slot
//      (1-bit SD_MMC on CLK 14 / CMD 15 / D0 2) with a matching hash,
//   5. and a base64 dump of the file so the laptop can hold a byte-exact copy
//      even when no card is seated.
//
// No radio is initialised: this rung is camera + storage only.

#include <Arduino.h>
#include <SD_MMC.h>
#include <esp_camera.h>
#include <mbedtls/sha256.h>

// ---- AI-Thinker ESP32-CAM pin map (OV2640 on DVP) ----
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

// ---- CAM microSD slot (1-bit SD_MMC) ----
#define SD_CLK 14
#define SD_CMD 15
#define SD_D0 2

static const char *kPath = "/sdcard/cam0230.jpg";

static void hex(const uint8_t *d, size_t n, char *out) {
  static const char *h = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[2 * i] = h[d[i] >> 4];
    out[2 * i + 1] = h[d[i] & 0x0f];
  }
  out[2 * n] = 0;
}

static void sha256_hex(const uint8_t *data, size_t len, char *out) {
  uint8_t digest[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts_ret(&ctx, 0);
  mbedtls_sha256_update_ret(&ctx, data, len);
  mbedtls_sha256_finish_ret(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  hex(digest, sizeof(digest), out);
}

static void dump_base64(const uint8_t *data, size_t len) {
  static const char *b64 =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  Serial.printf("REPORT: b64_len=%u\n", (unsigned)len);
  Serial.println("B64START");
  char line[81];
  size_t i = 0, o = 0;
  while (i + 2 < len) {
    uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
    line[o++] = b64[(v >> 18) & 0x3f];
    line[o++] = b64[(v >> 12) & 0x3f];
    line[o++] = b64[(v >> 6) & 0x3f];
    line[o++] = b64[v & 0x3f];
    i += 3;
    if (o >= 76) {
      line[o] = 0;
      Serial.println(line);
      o = 0;
    }
  }
  if (i < len) {
    uint32_t v = data[i] << 16;
    if (i + 1 < len) v |= data[i + 1] << 8;
    line[o++] = b64[(v >> 18) & 0x3f];
    line[o++] = b64[(v >> 12) & 0x3f];
    line[o++] = (i + 1 < len) ? b64[(v >> 6) & 0x3f] : '=';
    line[o++] = '=';
  }
  if (o) {
    line[o] = 0;
    Serial.println(line);
  }
  Serial.println("B64END");
}

static bool mount_sd() {
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  if (!SD_MMC.begin("/sdcard", true)) return false;  // 1-bit mode
  uint8_t t = SD_MMC.cardType();
  if (t == CARD_NONE) return false;
  Serial.printf("REPORT: sd_mounted=1 card_type=%u card_size_mb=%llu\n", t,
                SD_MMC.cardSize() / (1024ULL * 1024ULL));
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("REPORT: boot=esp32cam-capture task=0230 rung=1");

  camera_config_t cfg = {};
  cfg.ledc_channel = LEDC_CHANNEL_0;
  cfg.ledc_timer = LEDC_TIMER_0;
  cfg.pin_d0 = Y2_GPIO_NUM;
  cfg.pin_d1 = Y3_GPIO_NUM;
  cfg.pin_d2 = Y4_GPIO_NUM;
  cfg.pin_d3 = Y5_GPIO_NUM;
  cfg.pin_d4 = Y6_GPIO_NUM;
  cfg.pin_d5 = Y7_GPIO_NUM;
  cfg.pin_d6 = Y8_GPIO_NUM;
  cfg.pin_d7 = Y9_GPIO_NUM;
  cfg.pin_xclk = XCLK_GPIO_NUM;
  cfg.pin_pclk = PCLK_GPIO_NUM;
  cfg.pin_vsync = VSYNC_GPIO_NUM;
  cfg.pin_href = HREF_GPIO_NUM;
  cfg.pin_sccb_sda = SIOD_GPIO_NUM;
  cfg.pin_sccb_scl = SIOC_GPIO_NUM;
  cfg.pin_pwdn = PWDN_GPIO_NUM;
  cfg.pin_reset = RESET_GPIO_NUM;
  cfg.xclk_freq_hz = 20000000;
  cfg.pixel_format = PIXFORMAT_JPEG;
  cfg.frame_size = FRAMESIZE_VGA;
  cfg.jpeg_quality = 12;
  cfg.fb_count = 1;
  cfg.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  esp_err_t err = esp_camera_init(&cfg);
  Serial.printf("REPORT: camera_init=%d (%s)\n", (int)err,
                err == ESP_OK ? "ESP_OK" : esp_err_to_name(err));
  if (err != ESP_OK) {
    Serial.println("REPORT: result=camera_init_failed");
    return;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    Serial.printf(
        "REPORT: sensor_pid=0x%04x ver=0x%02x midh=0x%02x midl=0x%02x "
        "framesize=%d quality=%d\n",
        s->id.PID, s->id.VER, s->id.MIDH, s->id.MIDL, s->status.framesize,
        s->status.quality);
  }

  bool sd_ok = mount_sd();
  if (!sd_ok) Serial.println("REPORT: sd_mounted=0");

  // ---- capture ----
  Serial.println("REPORT: capturing");
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("REPORT: result=capture_failed");
    return;
  }
  char cam_hash[65];
  sha256_hex(fb->buf, fb->len, cam_hash);
  bool soi = fb->len > 4 && fb->buf[0] == 0xFF && fb->buf[1] == 0xD8;
  bool eoi = fb->len > 4 && fb->buf[fb->len - 2] == 0xFF &&
             fb->buf[fb->len - 1] == 0xD9;
  Serial.printf(
      "REPORT: capture w=%u h=%u len=%u fmt=%u soi=%d eoi=%d sha256=%s\n",
      (unsigned)fb->width, (unsigned)fb->height, (unsigned)fb->len,
      (unsigned)fb->format, soi, eoi, cam_hash);

  // ---- SD write + read-back ----
  if (sd_ok) {
    File f = SD_MMC.open(kPath, FILE_WRITE);
    if (!f) {
      Serial.println("REPORT: sd_write=open_failed");
    } else {
      size_t w = f.write(fb->buf, fb->len);
      f.close();
      Serial.printf("REPORT: sd_write bytes=%u ok=%d\n", (unsigned)w,
                    w == fb->len);
      if (w == fb->len) {
        File r = SD_MMC.open(kPath, FILE_READ);
        if (!r) {
          Serial.println("REPORT: sd_read=open_failed");
        } else {
          size_t n = r.size();
          uint8_t *tmp = (uint8_t *)malloc(n);
          size_t got = tmp ? r.read(tmp, n) : 0;
          r.close();
          char sd_hash[65] = {0};
          if (tmp && got == n) {
            sha256_hex(tmp, n, sd_hash);
            bool ssoi = n > 4 && tmp[0] == 0xFF && tmp[1] == 0xD8;
            bool seoi = n > 4 && tmp[n - 2] == 0xFF && tmp[n - 1] == 0xD9;
            Serial.printf(
                "REPORT: sd_readback len=%u sha256=%s match=%d soi=%d eoi=%d\n",
                (unsigned)n, sd_hash,
                strcmp(sd_hash, cam_hash) == 0 ? 1 : 0, ssoi, seoi);
          } else {
            Serial.printf("REPORT: sd_readback len=%u got=%u read_failed\n",
                          (unsigned)n, (unsigned)got);
          }
          free(tmp);
        }
      }
    }
  }

  dump_base64(fb->buf, fb->len);
  esp_camera_fb_return(fb);
  Serial.println("REPORT: result=done");
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last > 10000) {
    last = millis();
    Serial.printf("REPORT: alive uptime=%lu\n", (unsigned long)(millis() / 1000));
  }
  delay(100);
}
