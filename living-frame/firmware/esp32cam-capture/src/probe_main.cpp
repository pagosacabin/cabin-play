// 0230 rung 1 — board/storage probe for the kit ESP32-CAM.
//
// Runs when the camera module does not answer: it records the board identity
// (MAC, flash, PSRAM) and exercises the CAM's own microSD slot independently of
// the camera, so "no camera" and "no card" are separate, measured facts.
//
// SD_MMC is wired per the AI-Thinker map: CLK 14, CMD 15, D0 2 (1-bit mode).

#include <Arduino.h>
#include <SD_MMC.h>
#include <esp_mac.h>
#include <esp_system.h>

#define SD_CLK 14
#define SD_CMD 15
#define SD_D0 2

static void dump_dir(const char *path, int depth) {
  File dir = SD_MMC.open(path);
  if (!dir || !dir.isDirectory()) {
    Serial.printf("REPORT: dir_open_failed=%s\n", path);
    return;
  }
  for (File f = dir.openNextFile(); f && depth < 2; f = dir.openNextFile()) {
    Serial.printf("REPORT: sd_entry %s%s size=%u\n", path, f.name(),
                  (unsigned)f.size());
    f.close();
  }
  dir.close();
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("REPORT: boot=esp32cam-probe task=0230");

  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  Serial.printf("REPORT: mac=%02x:%02x:%02x:%02x:%02x:%02x chip=%s rev=%d\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], ESP.getChipModel(),
                ESP.getChipRevision());
  Serial.printf("REPORT: flash_size=%u psram_size=%u heap=%u\n",
                (unsigned)ESP.getFlashChipSize(), (unsigned)ESP.getPsramSize(),
                (unsigned)ESP.getFreeHeap());

  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  bool ok = SD_MMC.begin("/sdcard", true);  // 1-bit
  uint8_t type = SD_MMC.cardType();
  Serial.printf("REPORT: sd_begin=%d card_type=%u (0=NONE,1=MMC,2=SDSC,3=SDHC)\n",
                ok, type);
  if (ok && type != CARD_NONE) {
    Serial.printf("REPORT: sd_size_mb=%llu\n",
                  SD_MMC.cardSize() / (1024ULL * 1024ULL));
    dump_dir("/", 0);
    uint64_t used = SD_MMC.usedBytes(), total = SD_MMC.totalBytes();
    Serial.printf("REPORT: sd_used=%llu sd_total=%llu\n", used, total);
    // Read-only here: this probe must not write to a card whose identity the
    // 0229 diagnosis still owes (Stephen's scratch card call).
    Serial.println("REPORT: sd_write=skipped_by_design_read_only_probe");
  } else {
    Serial.println("REPORT: sd_mounted=0 no_card_or_mount_failure");
  }

  Serial.println("REPORT: result=probe_done");
}

void loop() { delay(1000); }
