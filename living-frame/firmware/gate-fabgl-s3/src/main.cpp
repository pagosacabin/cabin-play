// 0178 step-1 gate: minimal FabGL 1.0.9 64-colour VGA bring-up on ESP32-S3.
// Nothing is driven here beyond the VGA signal generation itself; the point is
// whether the library builds and initialises for this target at all.
#include <Arduino.h>
#include <fabgl.h>

fabgl::VGAController VGA;

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("chip=%s cores=%d psram=%u\n", ESP.getChipModel(), ESP.getChipCores(),
                (unsigned)ESP.getPsramSize());
  VGA.begin(GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_7,
            GPIO_NUM_19, GPIO_NUM_20, GPIO_NUM_38, GPIO_NUM_39);
  VGA.setResolution(VGA_640x480_60Hz);
  Serial.printf("free heap after begin: %u\n", (unsigned)ESP.getFreeHeap());
}

void loop() { delay(1000); }
