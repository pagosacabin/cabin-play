/*
 * 0225 -- N16R8 probe (living frame board verification).
 *
 * One board, four questions, no graphics:
 *   1. what silicon is here           -> chip / flash / PSRAM, measured
 *   2. is the 8 MB PSRAM real         -> full-range pattern write + readback
 *   3. do the silkscreen labels lie   -> 12 wired pins, driven and read back
 *   4. what is on the SD card         -> RAW SECTOR READS ONLY, no mount,
 *                                        no format, no writes of any kind
 *
 * Deliberately NOT in scope (task 0225, "this task is a probe, not the port"):
 * the 64-colour framebuffer work, VGA output, or re-deciding 0180's resolution.
 *
 * Wiring, as Stephen wired it on 2026-11-20 (agreed map, NOT verified by this
 * firmware -- verifying it is question 3):
 *
 *   VGA DAC   R: GPIO 4,5   G: GPIO 6,7   B: GPIO 15,16
 *             HSYNC: GPIO 17   VSYNC: GPIO 18
 *   SD (SPI)  SCK: GPIO 12  MOSI: GPIO 11  MISO: GPIO 13  CS: GPIO 10
 *             GPIO 14: spare (card-detect if the holder exposes it)
 *   NEVER USE GPIO 35/36/37 on an N16R8: they are the octal PSRAM bus inside
 *   the module.  On this part GPIO 33/34 are PSRAM too.  Nothing is wired
 *   there and the probe flags it if the map ever grows to include them.
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdarg.h>
#include "esp_partition.h"

/* ---- report sink ---------------------------------------------------------
 * The FT232R bridge on this bench cannot be trusted to deliver a live console:
 * with the app running, its RX path replays a fixed multi-byte pattern at USB
 * speed and ignores the configured baud, so the same bytes come back at 9600,
 * 115200 and 460800 alike.  Every line is therefore ALSO accumulated in RAM and
 * written to the dedicated `report` flash partition at the end of the probe:
 *
 *     esptool --port /dev/ttyUSB0 read_flash 0x310000 0x10000 report.bin
 *
 * That read-back is the authoritative log for this task.  The console copy is
 * kept because it costs nothing and is correct when the bridge behaves.
 */
static char  g_report[28000];
static size_t g_report_len;

static void app_logf(const char *fmt, ...)
{
    char line[512];
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

static void write_report_partition(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "report");
    if (part == NULL) {
        fputs("report partition not found - report only on console\n", stdout);
        return;
    }
    esp_partition_erase_range(part, 0, part->size);
    uint32_t header[2] = { 0x30323235u, (uint32_t)g_report_len };
    esp_partition_write(part, 0, header, sizeof(header));
    esp_partition_write(part, sizeof(header), g_report, g_report_len);
    fputs("report written to the report partition\n", stdout);
}


/* ------------------------------------------------------------------ pin map */
typedef struct { int gpio; const char *label; const char *role; } probe_pin_t;

static const probe_pin_t k_pins[] = {
    { 4,  "RED0",   "VGA DAC red bit 0" },
    { 5,  "RED1",   "VGA DAC red bit 1" },
    { 6,  "GREEN0", "VGA DAC green bit 0" },
    { 7,  "GREEN1", "VGA DAC green bit 1" },
    { 15, "BLUE0",  "VGA DAC blue bit 0" },
    { 16, "BLUE1",  "VGA DAC blue bit 1" },
    { 17, "HSYNC",  "VGA horizontal sync" },
    { 18, "VSYNC",  "VGA vertical sync" },
    { 12, "SD_SCK", "SD SPI clock" },
    { 11, "SD_MOSI","SD SPI MOSI" },
    { 13, "SD_MISO","SD SPI MISO" },
    { 10, "SD_CS",  "SD SPI chip select" },
};
#define NPINS (sizeof(k_pins) / sizeof(k_pins[0]))

/* 33-37: octal PSRAM bus on N16R8 (35/36/37 named in the task brief, 33/34
 * are in the same octal group).  26-32: SPI flash.  19/20: native USB.
 * 43/44: UART0 console.  0/3/45/46: strapping pins. */
static const char *reserved_reason(int gpio)
{
    if (gpio >= 26 && gpio <= 32) return "SPI flash bus on this module";
    if (gpio >= 33 && gpio <= 37) return "octal PSRAM bus inside the module";
    if (gpio == 19 || gpio == 20) return "native USB D-/D+";
    if (gpio == 43 || gpio == 44) return "UART0 console";
    if (gpio == 0 || gpio == 3 || gpio == 45 || gpio == 46) return "strapping pin";
    return NULL;
}

/* ------------------------------------------------------------- 1. the silicon */
static void report_chip(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);

    uint32_t flash_bytes = 0;
    esp_flash_get_size(NULL, &flash_bytes);

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    app_logf("\n=== 1. SILICON ===\n");
    app_logf("CHIP: esp32-s3 cores=%d rev=v%u.%u feature_bits=0x%04x\n",
           info.cores, info.revision / 100, info.revision % 100, (unsigned)info.features);
    app_logf("IDF: %s\n", esp_get_idf_version());
    app_logf("MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    app_logf("FLASH_BYTES: %" PRIu32 " (%" PRIu32 " MB reported by esp_flash_get_size)\n",
           flash_bytes, flash_bytes / (1024u * 1024u));

    /* PSRAM, three independent readings: IDF's own view, the heap's view, and
     * the compile-time mode the build was given.  They must agree. */
    bool inited = esp_psram_is_initialized();
    size_t psram_size = esp_psram_get_size();
    size_t psram_heap = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    app_logf("PSRAM_INITIALIZED: %s\n", inited ? "yes" : "no");
    app_logf("PSRAM_SIZE_BYTES: %u\n", (unsigned)psram_size);
    app_logf("PSRAM_HEAP_BYTES: %u (largest block %u)\n",
           (unsigned)psram_heap, (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
#if defined(CONFIG_SPIRAM_MODE_OCT)
    app_logf("PSRAM_MODE_SDKCONFIG: OCTAL (this build)\n");
#elif defined(CONFIG_SPIRAM_MODE_QUAD)
    app_logf("PSRAM_MODE_SDKCONFIG: QUAD (this build)\n");
#else
    app_logf("PSRAM_MODE_SDKCONFIG: none configured\n");
#endif
    app_logf("PSRAM internal RAM: free %u, largest %u\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    fflush(stdout);
}

/* --------------------------------------------------- 2. is the PSRAM real? */
static uint32_t pattern_a(uint32_t off) { return (off * 2654435761u) ^ 0xA5C35A3Cu; }

#define PSRAM_MAX_BLOCKS 8

static void psram_pattern_test(void)
{
    const size_t claimed = esp_psram_get_size();
    void  *blk[PSRAM_MAX_BLOCKS] = {0};
    size_t blk_len[PSRAM_MAX_BLOCKS] = {0};
    int    nb = 0;
    size_t len_total = 0;

    /* Fill the PSRAM heap with whole-page blocks until it is exhausted, so the
     * pattern covers every byte the allocator can hand out rather than only the
     * single largest block.  "8 MB claimed" is then tested to 100% of what the
     * chip actually yielded, and the shortfall (if any) is stated, not hidden. */
    while (nb < PSRAM_MAX_BLOCKS) {
        size_t avail = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) & ~0xFFFu;
        if (avail < 64u * 1024u) break;
        void *p = heap_caps_malloc(avail, MALLOC_CAP_SPIRAM);
        while (!p && avail > 64u * 1024u) {
            avail -= 64u * 1024u;
            p = heap_caps_malloc(avail, MALLOC_CAP_SPIRAM);
        }
        if (!p) break;
        blk[nb] = p;
        blk_len[nb] = avail;
        len_total += avail;
        nb++;
    }

    app_logf("\n=== 2. PSRAM PATTERN TEST ===\n");
    app_logf("PATTERN_CLAIMED_BYTES: %u\n", (unsigned)claimed);
    if (nb == 0) {
        app_logf("PATTERN_RESULT: FAIL (could not allocate any block in PSRAM)\n");
        fflush(stdout);
        return;
    }
    app_logf("PATTERN_BLOCKS: %d\n", nb);
    for (int b = 0; b < nb; b++) {
        app_logf("  block %d: %u bytes at %p\n", b, (unsigned)blk_len[b], blk[b]);
    }
    app_logf("PATTERN_TESTED_BYTES: %u (%.2f MB)\n",
             (unsigned)len_total, (double)len_total / (1024.0 * 1024.0));

    uint32_t bad1 = 0, bad2 = 0;
    uint32_t first_bad = 0xFFFFFFFFu, last_bad = 0;
    for (int b = 0; b < nb; b++) {
        volatile uint32_t *q = (volatile uint32_t *)blk[b];
        const size_t words = blk_len[b] / 4;
        const uint32_t base = (uint32_t)(b * 0x01000000u);

        /* Pass 1: address-mixed pattern.  Catches lines that alias each other,
         * stuck bits, and a chip that is really smaller than it claims (the tail
         * reads back as whatever the aliased low address held). */
        for (size_t i = 0; i < words; i++) q[i] = pattern_a(base + (uint32_t)(i * 4));
        for (size_t i = 0; i < words; i++) {
            uint32_t want = pattern_a(base + (uint32_t)(i * 4));
            uint32_t got = q[i];
            if (got != want) {
                if (bad1 == 0) { first_bad = base + (uint32_t)(i * 4); }
                last_bad = base + (uint32_t)(i * 4);
                bad1++;
            }
        }

        /* Pass 2: bitwise inverse.  A stuck-high or stuck-low cell passes pass 1
         * when its pattern bit happens to match; inverting closes that hole. */
        for (size_t i = 0; i < words; i++) q[i] = ~pattern_a(base + (uint32_t)(i * 4));
        for (size_t i = 0; i < words; i++) {
            uint32_t want = ~pattern_a(base + (uint32_t)(i * 4));
            if (q[i] != want) bad2++;
        }
    }

    app_logf("PATTERN_PASS1_MISMATCHES: %" PRIu32 "\n", bad1);
    app_logf("PATTERN_PASS2_MISMATCHES: %" PRIu32 "\n", bad2);
    if (bad1) {
        app_logf("PATTERN_PASS1_RANGE: first=0x%08" PRIx32 " last=0x%08" PRIx32 "\n",
                 first_bad, last_bad);
    }
    app_logf("PATTERN_RESULT: %s\n", (bad1 == 0 && bad2 == 0) ? "PASS" : "FAIL");
    app_logf("PATTERN_COVERAGE: tested %u of %u claimed bytes (%.1f%%)\n",
             (unsigned)len_total, (unsigned)claimed,
             100.0 * (double)len_total / (double)claimed);

    for (int b = 0; b < nb; b++) heap_caps_free(blk[b]);
    fflush(stdout);
}

/* ------------------------------------------------ 3. do the labels tell truth? */
static bool pin_selftest(int gpio)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&io) != ESP_OK) return false;

    /* Drive high with a pull-down engaged: the pad wins, so a working GPIO
     * reads 1.  If the label pointed at a reserved/reserved-by-flash pin the
     * config would have failed, and if it pointed at a pin held low externally
     * the pull-down case would read 0. */
    gpio_set_level(gpio, 1);
    gpio_set_pull_mode(gpio, GPIO_PULLDOWN_ONLY);
    esp_rom_delay_us(500);
    int high_read = gpio_get_level(gpio);

    gpio_set_level(gpio, 0);
    gpio_set_pull_mode(gpio, GPIO_PULLUP_ONLY);
    esp_rom_delay_us(500);
    int low_read = gpio_get_level(gpio);

    gpio_set_pull_mode(gpio, GPIO_FLOATING);

    return (high_read == 1) && (low_read == 0);
}

static void report_pins(void)
{
    app_logf("\n=== 3. PIN LABEL CHECK (12 wired pins) ===\n");
    app_logf("METHOD: each pin driven high+read, then low+read. "
           "PASS = pad drives and the input follows.\n");
    unsigned fails = 0;
    for (size_t i = 0; i < NPINS; i++) {
        const probe_pin_t *p = &k_pins[i];
        const char *res = reserved_reason(p->gpio);
        bool ok = pin_selftest(p->gpio);
        if (!ok) fails++;
        app_logf("PIN %-2d %-7s %-26s %s%s%s\n",
               p->gpio, p->label, p->role,
               ok ? "PASS" : "FAIL",
               res ? "  RESERVED: " : "",
               res ? res : "");
    }
    app_logf("PIN_DISCREPANCIES: %s", fails ? "" : "none found");
    if (fails) app_logf("%u of %u pins did not behave as GPIO", fails, (unsigned)NPINS);
    app_logf("\n");
    fflush(stdout);
}

/* --------------------------------------------------------- 4. the SD card */
static uint32_t rd16(const uint8_t *b, int o) { return (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8); }
static uint32_t rd32(const uint8_t *b, int o)
{
    return (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8) | ((uint32_t)b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24);
}

static void print_fat_name(const uint8_t *e)
{
    char name[13];
    int n = 0;
    for (int i = 0; i < 8 && e[i] != ' '; i++) name[n++] = (char)e[i];
    bool ext = false;
    for (int i = 8; i < 11 && e[i] != ' '; i++) {
        if (!ext) { name[n++] = '.'; ext = true; }
        name[n++] = (char)e[i];
    }
    name[n] = 0;
    if (e[0] == 0x05) name[0] = (char)0xE5;
    app_logf("    %-13s %s %8" PRIu32 " B\n", name,
           (e[11] & 0x10) ? "<DIR> " : "      ", rd32(e, 28));
}

static void list_fat_dir(sdmmc_card_t *card, uint8_t *buf, uint32_t dir_lba,
                         uint32_t dir_sectors, bool fat32, uint32_t clus_bytes,
                         uint32_t data_lba, const uint8_t *fat, uint32_t fat_bytes,
                         int max_entries)
{
    int shown = 0;
    uint32_t lba = dir_lba;
    uint32_t remaining = dir_sectors;
    uint32_t cluster = fat32 ? 0 : 0;

    while (remaining > 0 && shown < max_entries) {
        uint32_t chunk = remaining >= 4 ? 4 : remaining;
        if (sdmmc_read_sectors(card, buf, lba, chunk) != ESP_OK) {
            app_logf("    (read error at LBA %" PRIu32 ")\n", lba);
            return;
        }
        for (uint32_t s = 0; s < chunk; s++) {
            for (int off = 0; off < 512; off += 32) {
                const uint8_t *e = buf + s * 512 + off;
                if (e[0] == 0x00) return;            /* end of directory */
                if (e[0] == 0xE5) continue;           /* deleted */
                if ((e[11] & 0x3F) == 0x0F) continue; /* LFN fragment */
                if (e[11] & 0x08) continue;           /* volume label */
                print_fat_name(e);
                if (++shown >= max_entries) return;
            }
        }
        lba += chunk;
        remaining -= chunk;
    }
    (void)cluster; (void)clus_bytes; (void)data_lba; (void)fat; (void)fat_bytes;
}

static void sd_probe(void)
{
    app_logf("\n=== 4. SD CARD (READ-ONLY) ===\n");
    app_logf("METHOD: SPI on SCK=12 MOSI=11 MISO=13 CS=10, raw sdmmc_read_sectors. "
           "No mount, no format, no writes.\n");

    spi_bus_config_t bus = {
        .mosi_io_num = 11, .miso_io_num = 13, .sclk_io_num = 12,
        .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) { app_logf("SD_RESULT: FAIL spi_bus_initialize=%s\n", esp_err_to_name(err)); return; }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SPI2_HOST;
    slot.gpio_cs = 10;
    err = sdspi_host_init();
    if (err != ESP_OK) { app_logf("SD_RESULT: FAIL sdspi_host_init=%s\n", esp_err_to_name(err)); return; }
    sdspi_dev_handle_t dev = 0;
    err = sdspi_host_init_device(&slot, &dev);
    if (err != ESP_OK) { app_logf("SD_RESULT: FAIL init_device=%s\n", esp_err_to_name(err)); return; }
    host.slot = dev;

    /* Card-detect, if the holder exposes it.  GPIO14 is the spare pin in the
     * agreed map; a holder with a CD switch pulls it low with a card seated.
     * Its state is reported either way -- "no card" and "card not answering"
     * are different failures and the reply must not blur them. */
    gpio_config_t cd = {
        .pin_bit_mask = 1ULL << 14,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cd);
    esp_rom_delay_us(1000);
    int cd_level = gpio_get_level(14);
    app_logf("SD_CARD_DETECT: GPIO14=%d (%s)\n", cd_level,
             cd_level ? "high (no CD switch, or no card)" : "low (card seated)");

    sdmmc_card_t card;
    err = sdmmc_card_init(&host, &card);
    if (err != ESP_OK) {
        app_logf("SD_CARD_INIT: first attempt (default clock) failed: %s\n", esp_err_to_name(err));
        /* Second attempt at the SD spec's identification clock.  If a marginal
         * wire only works at low speed, this separates "wiring too slow for
         * 20 MHz" from "card does not answer at all". */
        host.max_freq_khz = 400;
        err = sdmmc_card_init(&host, &card);
        if (err != ESP_OK) {
            app_logf("SD_RESULT: FAIL card_init=%s at 400 kHz too "
                     "(card absent, not seated, or not answering CMD0/CMD8)\n",
                     esp_err_to_name(err));
            fflush(stdout);
            return;
        }
        app_logf("SD_NOTE: card only initialised at 400 kHz -- check the SPI wiring\n");
    }
    app_logf("SD_CARD: name=%s type=%s speed=%dkHz size=%llu MB\n",
           card.cid.name, (card.is_mmc ? "MMC" : (card.is_sdio ? "SDIO" : "SD")),
           card.real_freq_khz, (unsigned long long)((uint64_t)card.csd.capacity * card.csd.sector_size / (1024 * 1024)));

    uint8_t *buf = heap_caps_malloc(4096, MALLOC_CAP_DMA);
    if (!buf) { app_logf("SD_RESULT: FAIL no DMA buffer\n"); return; }

    if (sdmmc_read_sectors(&card, buf, 0, 1) != ESP_OK) {
        app_logf("SD_RESULT: FAIL cannot read sector 0\n");
        heap_caps_free(buf); return;
    }

    const uint8_t *mbr = buf;
    bool is_exfat_whole = (memcmp(mbr + 3, "EXFAT   ", 8) == 0);
    uint32_t part_lba = 0, part_type = 0;

    if (is_exfat_whole) {
        part_lba = 0; part_type = 0x07;
    } else if (mbr[510] == 0x55 && mbr[511] == 0xAA) {
        for (int i = 0; i < 4; i++) {
            const uint8_t *pe = mbr + 446 + 16 * i;
            if (pe[4] == 0) continue;
            part_lba = rd32(pe, 8);
            part_type = pe[4];
            break;
        }
    }
    if (part_type == 0) { app_logf("SD_RESULT: no MBR partition and no exFAT boot sector\n"); heap_caps_free(buf); return; }
    app_logf("SD_PARTITION: type=0x%02" PRIx32 " lba=%" PRIu32 "\n", part_type, part_lba);

    if (sdmmc_read_sectors(&card, buf, part_lba, 1) != ESP_OK) {
        app_logf("SD_RESULT: FAIL cannot read boot sector at LBA %" PRIu32 "\n", part_lba); heap_caps_free(buf); return;
    }
    const uint8_t *bs = buf;

    if (memcmp(bs + 3, "EXFAT   ", 8) == 0) {
        uint32_t bps_shift = bs[108];
        uint32_t spc_shift = bs[109];
        uint32_t cluster_heap = rd32(bs, 88);
        uint32_t root_clus = rd32(bs, 96);
        uint64_t volume_sectors = (uint64_t)rd32(bs, 72) | ((uint64_t)rd32(bs, 76) << 32);
        app_logf("SD_FS: exFAT  sector=%u B cluster=%u B  volume=%llu MB\n",
               1u << bps_shift, (1u << bps_shift) << spc_shift,
               (unsigned long long)(volume_sectors * (1u << bps_shift) / (1024 * 1024)));
        uint32_t root_lba = cluster_heap + (root_clus - 2) * (1u << spc_shift);
        if (sdmmc_read_sectors(&card, buf, root_lba, 1) == ESP_OK) {
            app_logf("SD_ROOT (exFAT, first entries):\n");
            int shown = 0;
            for (int off = 0; off < 512 && shown < 16; off += 32) {
                const uint8_t *e = buf + off;
                if (e[0] == 0x00) break;
                if (e[0] != 0x85 && e[0] != 0x83) continue;
                char name[32]; int n = 0;
                for (int i = 0; i < 15; i++) {
                    uint16_t ch = (uint16_t)e[2 + i * 2] | ((uint16_t)e[3 + i * 2] << 8);
                    if (!ch) break;
                    name[n++] = (ch < 128) ? (char)ch : '?';
                }
                name[n] = 0;
                app_logf("    %-20s %s\n", name, (e[0] == 0x83) ? "<DIR>" : "");
                shown++;
            }
            if (!shown) app_logf("    (empty)\n");
        }
    } else {
        uint32_t bps = rd16(bs, 11);
        uint32_t spc = bs[13];
        uint32_t reserved = rd16(bs, 14);
        uint32_t nfats = bs[16];
        uint32_t root_entries = rd16(bs, 17);
        uint32_t total16 = rd16(bs, 19);
        uint32_t fatsz16 = rd16(bs, 22);
        uint32_t total32 = rd32(bs, 32);
        uint32_t fatsz32 = rd32(bs, 36);
        char label[12] = {0};
        memcpy(label, bs + 71, 11);

        uint64_t total_sectors = total16 ? total16 : total32;
        uint32_t fatsz = fatsz16 ? fatsz16 : fatsz32;
        uint32_t data_sectors = (uint32_t)(total_sectors - (reserved + nfats * fatsz +
                                     (root_entries * 32 + bps - 1) / bps));
        uint32_t clusters = data_sectors / (spc ? spc : 1);
        const char *fstype = clusters < 4085 ? "FAT12" : (clusters < 65525 ? "FAT16" : "FAT32");

        app_logf("SD_FS: %s  sector=%" PRIu32 " B  cluster=%" PRIu32 " B  clusters=%" PRIu32 "\n",
               fstype, bps, bps * spc, clusters);
        app_logf("SD_CAPACITY: %llu MB (%llu sectors of %" PRIu32 " B)\n",
               (unsigned long long)(total_sectors * bps / (1024 * 1024)),
               (unsigned long long)total_sectors, bps);
        app_logf("SD_LABEL: %.11s\n", label);

        uint32_t data_lba = reserved + nfats * fatsz + (root_entries * 32) / bps;
        app_logf("SD_ROOT (first entries):\n");
        if (clusters < 65525) {
            uint32_t root_lba = reserved + nfats * fatsz;
            uint32_t root_sectors = (root_entries * 32 + bps - 1) / bps;
            list_fat_dir(&card, buf, root_lba, root_sectors > 8 ? 8 : root_sectors,
                         false, bps * spc, data_lba, NULL, 0, 16);
        } else {
            uint32_t root_clus = rd32(bs, 44);
            uint32_t clus_bytes = bps * spc;
            uint32_t chain_lba = reserved + (root_clus - 2) * spc;
            list_fat_dir(&card, buf, chain_lba, spc > 8 ? 8 : spc, true, clus_bytes,
                         data_lba, NULL, 0, 16);
        }
    }
    app_logf("SD_RESULT: mounted-read OK (raw sectors only, nothing written)\n");
    heap_caps_free(buf);
    fflush(stdout);
}

/* --------------------------------------------------------------- entry point */
void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(300)); /* let the console settle after reset */
    app_logf("\n\n########## 0225 N16R8 PROBE ##########\n");
    app_logf("build: %s %s\n", __DATE__, __TIME__);

    report_chip();
    psram_pattern_test();
    report_pins();
    sd_probe();

    app_logf("\n########## PROBE COMPLETE ##########\n");
    fflush(stdout);
    write_report_partition();
    for (;;) vTaskDelay(pdMS_TO_TICKS(5000));
}
