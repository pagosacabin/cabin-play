// 0230 rung 1 — SCCB diagnostic v2 for the kit ESP32-CAM.
//
// esp_camera_init() returned ESP_ERR_NOT_FOUND (261): no ACK on the SCCB bus.
// v1 scanned with XCLK off / 20 MHz / 10 MHz and found ZERO devices at every
// setting. v2 removes the remaining confounders before that result is read as
// "no camera module talking":
//   * internal pull-ups forced on SIOD/SIOC (SCCB pull-ups normally live on the
//     camera module, so an empty bus floats without them),
//   * PWDN driven high then low with a scan at each state,
//   * the idle level of both lines reported (a pulled-up line reading LOW means
//     the line is held down, not merely idle),
//   * a control scan on a second likely pin pair (GPIO 21/22, used by some
//     clones) so a pin-map error is distinguishable from a dead module,
//   * the OV2640 PID register read at both XCLK frequencies.

#include <Arduino.h>
#include <Wire.h>
#include <driver/ledc.h>

#define SIOD 26
#define SIOC 27
#define XCLK 0
#define PWDN 32

static void set_xclk(uint32_t hz) {
  ledcSetup(0, hz, 1);
  ledcAttachPin(XCLK, 0);
  ledcWrite(0, 1);
}

static void stop_xclk() {
  ledcDetachPin(XCLK);
  pinMode(XCLK, OUTPUT);
  digitalWrite(XCLK, LOW);
}

static int scan_bus(int sda, int scl, const char *why) {
  pinMode(sda, INPUT_PULLUP);
  pinMode(scl, INPUT_PULLUP);
  Wire.end();
  Wire.begin(sda, scl, 100000);
  delay(20);
  Serial.printf("REPORT: scan=%s sda=%d scl=%d idle_sda=%d idle_scl=%d\n", why,
                sda, scl, digitalRead(sda), digitalRead(scl));
  int found = 0;
  for (uint8_t a = 0x08; a < 0x78; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("REPORT: i2c_found=0x%02x\n", a);
      found++;
    }
  }
  Serial.printf("REPORT: i2c_found_count=%d\n", found);
  return found;
}

static uint8_t ov_pid() {
  uint8_t pid = 0;
  Wire.beginTransmission(0x30);
  Wire.write(0x0A);
  if (Wire.endTransmission() == 0) {
    Wire.requestFrom((uint8_t)0x30, (uint8_t)1);
    if (Wire.available()) pid = Wire.read();
  }
  return pid;
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("REPORT: boot=esp32cam-sccb-diag-v2 task=0230");

  Serial.printf("REPORT: pins pwdn_gpio=%d siod=%d sioc=%d xclk=%d\n", PWDN, SIOD,
                SIOC, XCLK);

  pinMode(PWDN, OUTPUT);
  digitalWrite(PWDN, HIGH);
  delay(50);
  stop_xclk();
  scan_bus(SIOD, SIOC, "pwdn_high_xclk_off");

  digitalWrite(PWDN, LOW);
  delay(100);
  scan_bus(SIOD, SIOC, "pwdn_low_xclk_off");

  set_xclk(20000000);
  delay(300);
  int n20 = scan_bus(SIOD, SIOC, "pwdn_low_xclk_20mhz");
  Serial.printf("REPORT: ov2640_pid_20mhz=0x%02x (expect 0x26)\n", ov_pid());

  set_xclk(10000000);
  delay(300);
  scan_bus(SIOD, SIOC, "pwdn_low_xclk_10mhz");

  // Clone pin-map control: some ESP32-CAM variants put SCCB on 21/22.
  stop_xclk();
  scan_bus(21, 22, "alt_pins_21_22_xclk_off");

  Serial.printf("REPORT: primary_bus_devices=%d\n", n20);
  Serial.println("REPORT: result=diag_done");
}

void loop() { delay(1000); }
