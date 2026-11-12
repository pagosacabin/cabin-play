// 0178 bench identification probe (NOT the deliverable, no VGA code).
// Reports the chip and — the fact the task's S3R2 premise depends on — PSRAM size.
#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("=== 0178 bench identification probe ===");
  Serial.printf("chip model      : %s\n", ESP.getChipModel());
  Serial.printf("chip revision   : %d\n", ESP.getChipRevision());
  Serial.printf("cores           : %d\n", ESP.getChipCores());
  Serial.printf("cpu freq MHz    : %u\n", (unsigned)getCpuFrequencyMhz());
  Serial.printf("flash size      : %u bytes\n", (unsigned)ESP.getFlashChipSize());
  Serial.printf("psram size      : %u bytes\n", (unsigned)ESP.getPsramSize());
  Serial.printf("psram free      : %u bytes\n", (unsigned)ESP.getFreePsram());
  Serial.printf("heap total      : %u bytes\n", (unsigned)ESP.getHeapSize());
  Serial.printf("heap free       : %u bytes\n", (unsigned)ESP.getFreeHeap());
  Serial.printf("largest DMA blk : %u bytes\n",
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
  Serial.println("=== end probe ===");
  Serial.flush();
}

void loop() { delay(2000); }
