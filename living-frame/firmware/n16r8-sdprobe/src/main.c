/*
 * 0229 -- SD card diagnosis on the living frame board (N16R8, ESP32-S3,
 * MAC c4:9e:7e:08:6d:f4).
 *
 * Follows 0225's finding: sdmmc_card_init over SPI returned
 * ESP_ERR_NOT_SUPPORTED at both the default clock and 400 kHz, and a raw SPI
 * probe saw the card answering nothing (identical responses across modes and
 * speeds, MISO held high by a pull-up).  Stephen resoldered the holder's pins
 * 10-13 at 15:21 on 2026-11-20 and this probe re-runs the test.
 *
 * The task's ladder, as far as firmware can walk it:
 *
 *   rung 1  card init at 400 kHz and at the default clock, with the kit card
 *           seated (read-only: no mount with write, no format, no writes)
 *   rung 3  pull-ups -- force internal pull-ups on MISO/CS and retry.  This is
 *           the software half of rung 3 only; an external 10k is Stephen's hands
 *   rung 4  SDMMC 1-bit on the same pins (CLK 12, CMD 11, D0 13) as a fork:
 *           answers in 1-bit but not SPI -> SPI path/wiring, not the card
 *   and     CMD-level identification (CMD0/CMD8/ACMD41/CMD58) so a refusal is
 *           reported as raw bytes rather than one driver error code
 *
 * rung 2 (VDD at the holder, continuity, shorts) needs a meter and is not
 * something firmware can substitute for.
 *
 * Wiring (agreed map, wired by Stephen; the silkscreen positions were proven in
 * 0225 only as "these pads are working GPIOs"):
 *
 *   SD (SPI)  SCK: GPIO 12  MOSI: GPIO 11  MISO: GPIO 13  CS: GPIO 10
 *             GPIO 14: spare / card-detect.  NEVER GPIO 26-37 on an N16R8.
 *
 * The FT232R console is dead (0225), so the log also goes to the `report`
 * partition at 0x310000 and that read-back is the authoritative record:
 *
 *   esptool --chip esp32s3 --port /dev/ttyUSB0 --baud 460800 \
 *           read_flash 0x310000 0x10000 report.bin
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <inttypes.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "dirent.h"
#include "sys/stat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 0236 -- power-up settle delay before the FIRST init attempt only.
 *
 * 0234 measured the fault precisely: after every reset the first SPI exchange
 * with the card returns junk (CMD0 answers 0x3f/0x47 instead of R1=0x01; CMD8
 * answers R1=0x00 and its R7 does not echo 0x1AA) and the SECOND attempt is
 * clean -- 27 of 27 resets, two cards, two builds.  The delay below is inserted
 * between the standard 74+ dummy clocks (CS high) and the first CMD0 of the
 * first attempt, and nowhere else: attempts 2+ run exactly as in 0234 so the
 * cross-reset comparison stays honest.
 *
 * Set at build time, no source edit per value:
 *   PLATFORMIO_BUILD_FLAGS="-DSD_FIRSTPASS_DELAY_MS=50" pio run -e n16r8-sdprobe
 * 0 is the 0234 control: the #if below compiles the delay out entirely, so the
 * control build is behaviourally identical to the base revision.
 */
#ifndef SD_FIRSTPASS_DELAY_MS
#define SD_FIRSTPASS_DELAY_MS 0
#endif
/* Range guard: a value the power-up test never intends is a build error rather
 * than a silently-passed flag (and it proves the -D reached this file). */
#if SD_FIRSTPASS_DELAY_MS < 0 || SD_FIRSTPASS_DELAY_MS > 2000
#error "SD_FIRSTPASS_DELAY_MS out of range (0..2000 ms)"
#endif

#define PIN_SCK   12
#define PIN_MOSI  11
#define PIN_MISO  13
#define PIN_CS    10
#define PIN_CD    14

/* forward declarations: the raw probe runs before these are defined */
static void raw_inspect(spi_device_handle_t dev);
static uint32_t rd16(const uint8_t *b, int o);
static uint32_t rd32(const uint8_t *b, int o);
static void print_fat_name(const uint8_t *e);
static bool g_byte_addressed = true;
static bool g_raw_ready;            /* raw SPI init reached READY */
static int  g_raw_attempt;
static bool g_inspect_ok;           /* no read error during inspection */
static bool g_have_card_id;         /* CSD decoded */
static uint8_t g_csd[16];           /* CSD as read (CID is read into the same buffer) */
/* CMD59 in idle state: 0 accepted, 1 rejected with a real R1, 2 unknown (no
 * response / transfer failure -- which must NOT be reported as a rejection). */
static int  g_cmd59_idle = -1;      /* -1 not probed */
static uint8_t g_seq_with = 0xFF;   /* CMD59 after CMD52+CMD5, like the driver */
static uint8_t g_seq_without = 0xFF;/* CMD59 without CMD5 (control) */
static uint8_t g_seq_52only = 0xFF; /* CMD59 after CMD52 alone (control) */
static bool g_seq_ran;

/* First-pass evidence for 0234's cross-reset table: the very first responses
 * of this boot, captured before any retry — the same quantity 0229 measured at
 * 11 failures in 12 runs on the old card. */
static uint8_t  g_fp_first_r1   = 0xFF;
static uint8_t  g_fp_cmd0_first = 0xFF;
static uint16_t g_fp_acmd41_ms;

/* ---- report sink (same discipline as 0225) ------------------------------ */
static char  g_report[60000];
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

/* ---- first-pass counter, persisted across board resets (task 0234) -------
 * 0229 measured "first init pass after a reset fails 11 of 12" by hand, from
 * 12 hand-driven runs.  The question this task asks is whether that flakiness
 * followed the card, so it has to be measured on this card by the board
 * itself, over at least 10 resets.  Each boot appends one slot to NVS (the
 * `nvs` partition, untouched by the report read-back) so the table survives
 * the reset that produced it, and prints the whole table into the report.
 */
typedef struct __attribute__((packed)) {
    uint8_t  magic;        /* 0xA5: a slot that was actually written */
    uint8_t  ready;        /* 1 = raw SPI init reached READY this boot */
    uint8_t  first_r1;     /* CMD8 R1 on attempt 1 (0xFF = no response) */
    uint8_t  ok_attempt;   /* attempt that reached READY, 0 = none */
    uint16_t acmd41_ms;    /* time to READY on the winning attempt */
    uint16_t cmd0_first;   /* CMD0 R1 on the very first try this boot */
} fp_slot_t;

#define FP_SLOTS 24
static fp_slot_t g_slots[FP_SLOTS];
static int       g_slot_n;
static bool      g_fp_nvs_ok;
static uint32_t  g_build_id;
static nvs_handle_t g_nvs;

static void fp_load(void)
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK && err != ESP_ERR_NVS_NO_FREE_PAGES) {
        app_logf("FIRSTPASS: nvs_flash_init failed (%s) - counter will not survive resets\n",
                 esp_err_to_name(err));
        return;
    }
    err = nvs_open("sdprobe0234", NVS_READWRITE, &g_nvs);
    if (err != ESP_OK) {
        app_logf("FIRSTPASS: nvs_open failed (%s) - counter will not survive resets\n",
                 esp_err_to_name(err));
        return;
    }
    g_fp_nvs_ok = true;

    /* A table belongs to the build that produced it: a reflash starts a new
     * run, so the slots are keyed by a hash of the build stamp.  NVS itself is
     * NOT erased (the accepted 800x600 build's own settings live there). */
    char stamp[64];
    snprintf(stamp, sizeof stamp, "%s %s", __DATE__, __TIME__);
    uint32_t bid = 2166136261u;
    for (const char *c = stamp; *c; c++) { bid ^= (uint8_t)*c; bid *= 16777619u; }
    g_build_id = bid;
    uint32_t stored = 0;
    bool same = (nvs_get_u32(g_nvs, "bid", &stored) == ESP_OK && stored == bid);

    size_t len = sizeof(g_slots);
    err = nvs_get_blob(g_nvs, "fp", g_slots, &len);
    if (err == ESP_OK && same) {
        g_slot_n = (int)(len / sizeof(fp_slot_t));
        if (g_slot_n > FP_SLOTS) g_slot_n = FP_SLOTS;
    } else {
        g_slot_n = 0;
        nvs_set_u32(g_nvs, "bid", bid);
        nvs_commit(g_nvs);
        app_logf("FIRSTPASS: new build stamp, table starts empty\n");
    }
}

static void fp_append(uint8_t ready, uint8_t first_r1, uint8_t ok_attempt,
                      uint16_t acmd41_ms, uint16_t cmd0_first)
{
    fp_slot_t s = { 0xA5, ready, first_r1, ok_attempt, acmd41_ms, cmd0_first };
    if (g_slot_n >= FP_SLOTS) {
        /* keep the newest FP_SLOTS runs: drop the oldest */
        memmove(&g_slots[0], &g_slots[1], sizeof(fp_slot_t) * (FP_SLOTS - 1));
        g_slot_n = FP_SLOTS - 1;
    }
    g_slots[g_slot_n++] = s;
    if (g_fp_nvs_ok) {
        esp_err_t err = nvs_set_blob(g_nvs, "fp", g_slots, sizeof(fp_slot_t) * (size_t)g_slot_n);
        if (err == ESP_OK) err = nvs_commit(g_nvs);
        if (err != ESP_OK) app_logf("FIRSTPASS: persisting slot failed (%s)\n", esp_err_to_name(err));
    }
}

static void fp_report(void)
{
    int pass_first = 0, ready_n = 0;
    app_logf("\n=== 4. FIRST-PASS TABLE (persisted across resets, NVS) ===\n");
    app_logf("FIRSTPASS_DELAY_MS: %d\n", SD_FIRSTPASS_DELAY_MS);
    app_logf("FIRSTPASS_SLOTS: %d (of %d kept), build_id=%08x\n", g_slot_n, FP_SLOTS, g_build_id);
    for (int i = 0; i < g_slot_n; i++) {
        const fp_slot_t *s = &g_slots[i];
        app_logf("  boot %2d: CMD0_first=0x%02x CMD8_r1=0x%02x RAW=%s ok_attempt=%u acmd41=%u ms\n",
                 i + 1, s->cmd0_first, s->first_r1, s->ready ? "READY" : "FAILED",
                 s->ok_attempt, s->acmd41_ms);
        if (s->ready) ready_n++;
        if (s->ready && s->ok_attempt == 1) pass_first++;
    }
    app_logf("FIRSTPASS_SUMMARY: raw_init_ready=%d/%d first_pass_ok=%d/%d\n",
             ready_n, g_slot_n, pass_first, g_slot_n);
    if (g_slot_n > 0) {
        app_logf("FIRSTPASS_VERDICT: %s\n",
                 pass_first == g_slot_n
                   ? "every boot's FIRST init pass succeeded (no reset-dependent flakiness)"
                   : "the first init pass after a reset failed on this card too -- the "
                     "flakiness does NOT follow the old card");
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
    uint32_t header[2] = { 0x30323239u, (uint32_t)g_report_len };
    esp_partition_write(part, 0, header, sizeof(header));
    esp_partition_write(part, sizeof(header), g_report, g_report_len);
    fputs("report written to the report partition\n", stdout);
}

/* ---- capture IDF's own log lines into the report -------------------------
 * The driver is the thing being diagnosed: its step-by-step log names the
 * failure.  Routing esp_log through app_logf puts it in the flash report
 * instead of on a console nobody can read.
 */
static int report_vprintf(const char *fmt, va_list ap)
{
    char line[256];
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    if (n > 0) {
        size_t len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;
        if (g_report_len + len < sizeof(g_report)) {
            memcpy(g_report + g_report_len, line, len);
            g_report_len += len;
        }
        fputs(line, stdout);
    }
    return n;
}

/* ---------------------------------------------------------------- identity */
static void report_identity(void)
{
    esp_chip_info_t info;
    esp_chip_info(&info);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    app_logf("\n=== 1. IDENTITY ===\n");
    app_logf("CHIP: esp32-s3 cores=%d rev=v%u.%u\n",
             info.cores, info.revision / 100, info.revision % 100);
    app_logf("IDF: %s\n", esp_get_idf_version());
    app_logf("MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    app_logf("SD_PINS: SCK=%d MOSI=%d MISO=%d CS=%d CARD_DETECT=%d\n",
             PIN_SCK, PIN_MOSI, PIN_MISO, PIN_CS, PIN_CD);
}

/* --------------------------------------------------------- GPIO line levels */
static void report_line_levels(const char *tag, bool pullups)
{
    /* Card-detect first: GPIO14 is the spare pin in the agreed map. */
    gpio_config_t cd = {
        .pin_bit_mask = 1ULL << PIN_CD,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cd);

    gpio_config_t lows = {
        .pin_bit_mask = (1ULL << PIN_MISO) | (1ULL << PIN_CS) | (1ULL << PIN_MOSI) | (1ULL << PIN_SCK),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = pullups ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&lows);
    esp_rom_delay_us(2000);

    app_logf("LINES[%s] pullups=%s: MISO=%d CS=%d MOSI=%d SCK=%d CD14=%d\n", tag,
             pullups ? "internal-on" : "off",
             gpio_get_level(PIN_MISO), gpio_get_level(PIN_CS),
             gpio_get_level(PIN_MOSI), gpio_get_level(PIN_SCK),
             gpio_get_level(PIN_CD));
}

/* ------------------------------------------------- raw SPI command-level probe
 * Sends one SD command in SPI mode with CS driven by hand, and returns the R1
 * byte (0xFF when the card drove nothing).  Raw bytes are logged either way:
 * "no response" and "wrong response" are different findings.
 */
/* CRC7 over the 5 command bytes, then the stop bit -- validated against the two
 * spec constants: CMD0 -> 0x95, CMD8(0x1AA) -> 0x87.  Needed because a card
 * told CRC_ON_OFF=1 rejects every frame whose CRC is a placeholder 0xFF
 * (observed: R1=0x08), and because the driver's failure is exactly this. */
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
                          uint8_t crc, int extra, uint8_t *out, int out_max)
{
    uint8_t tx[32], rx[32];
    memset(tx, 0xFF, sizeof tx);
    memset(rx, 0x00, sizeof rx);

    tx[0] = 0x40 | cmd;
    tx[1] = (uint8_t)(arg >> 24);
    tx[2] = (uint8_t)(arg >> 16);
    tx[3] = (uint8_t)(arg >> 8);
    tx[4] = (uint8_t)(arg);
    tx[5] = sd_crc7(tx, 5);            /* real CRC7; the crc argument is legacy */
    (void)crc;

    int n = 6 + 8 + extra;                 /* command + Ncr window + payload */
    if (n > (int)sizeof(tx)) n = (int)sizeof(tx);

    gpio_set_level(PIN_CS, 0);
    spi_transaction_t t = {0};
    t.length = (size_t)n * 8;
    t.tx_buffer = tx;
    t.rx_buffer = rx;
    esp_err_t err = spi_device_transmit(dev, &t);
    gpio_set_level(PIN_CS, 1);

    /* Eight clocks with CS high complete the card's shift-out; without them
     * the next command can start inside the previous response (observed as a
     * spurious 0xF0 leading byte and as intermittent "illegal command"). */
    {
        uint8_t ff = 0xFF;
        spi_transaction_t tail = {0};
        tail.length = 8;
        tail.tx_buffer = &ff;
        spi_device_transmit(dev, &tail);
    }

    if (err != ESP_OK) {
        app_logf("    CMD%-2u: spi transfer failed (%s)\n", cmd, esp_err_to_name(err));
        return 0xFF;
    }

    int idx = -1;
    for (int i = 6; i < n; i++) {
        if ((rx[i] & 0x80) == 0) { idx = i; break; }   /* R1 has bit7 clear */
    }

    char hex[3 * 20 + 1];
    int  hn = 0;
    for (int i = 6; i < n && hn < (int)sizeof(hex) - 4; i++) {
        hn += snprintf(hex + hn, sizeof(hex) - hn, "%02x ", rx[i]);
    }
    hex[hn] = 0;

    if (idx < 0) {
        app_logf("    CMD%-2u arg=0x%08" PRIx32 ": NO RESPONSE (raw %s)\n", cmd, arg, hex);
        return 0xFF;
    }

    int copied = 0;
    for (int i = idx + 1; i < n && copied < out_max && copied < extra; i++, copied++) {
        out[copied] = rx[i];
    }
    app_logf("    CMD%-2u arg=0x%08" PRIx32 ": R1=0x%02x payload[%d]=%s\n",
             cmd, arg, rx[idx], copied, hex);
    return rx[idx];
}

static void raw_probe(const char *tag, bool pullups, bool inspect)
{
    app_logf("\n--- RAW SPI PROBE [%s], 400 kHz, manual CS, internal pull-ups %s ---\n",
             tag, pullups ? "ON" : "OFF (external only)");

    report_line_levels(tag, pullups);

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        app_logf("    SPI bus init failed: %s\n", esp_err_to_name(err));
        return;
    }

    gpio_set_direction(PIN_CS, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_CS, 1);
    if (pullups) {
        gpio_set_pull_mode(PIN_MISO, GPIO_PULLUP_ONLY);
        gpio_set_pull_mode(PIN_CS, GPIO_PULLUP_ONLY);
        gpio_set_pull_mode(PIN_MOSI, GPIO_PULLUP_ONLY);
    } else {
        gpio_set_pull_mode(PIN_MISO, GPIO_FLOATING);
        gpio_set_pull_mode(PIN_CS, GPIO_FLOATING);
        gpio_set_pull_mode(PIN_MOSI, GPIO_FLOATING);
    }

    spi_device_interface_config_t devcfg = {0};
    devcfg.clock_speed_hz = 400 * 1000;
    devcfg.mode = 0;
    devcfg.spics_io_num = -1;          /* CS driven by hand */
    devcfg.queue_size = 1;
    spi_device_handle_t dev = NULL;
    err = spi_bus_add_device(SPI2_HOST, &devcfg, &dev);
    if (err != ESP_OK) {
        app_logf("    add_device failed: %s\n", esp_err_to_name(err));
        spi_bus_free(SPI2_HOST);
        return;
    }

    /* 74+ clocks with CS high before the first command (SD spec). */
    {
        uint8_t ff[10];
        memset(ff, 0xFF, sizeof ff);
        gpio_set_level(PIN_CS, 1);
        spi_transaction_t t = {0};
        t.length = sizeof(ff) * 8;
        t.tx_buffer = ff;
        spi_device_transmit(dev, &t);
    }

    uint8_t pl[8] = {0};
    bool ready = false;
    bool v2 = false;
    int  ok_attempt = 0;

    /* First-pass evidence for the cross-reset table (0234): the very first
     * CMD0 and CMD8 responses of this boot, before any retry. */
    bool capture_first = (strcmp(tag, "no-internal-pullup") == 0);

    /* The first init exchange after a reset sometimes comes back R1=0x05
     * (illegal command) and succeeds on the next pass, so the sequence is
     * repeated and the attempt number recorded.  Counting it is the honest
     * report: one silent retry is not the same finding as a clean first try. */
    for (int attempt = 1; attempt <= 5 && !ready; attempt++) {
        app_logf("  ATTEMPT %d/5\n", attempt);

        /* 74+ clocks with CS high before every attempt (SD spec). */
        {
            uint8_t ff[10];
            memset(ff, 0xFF, sizeof ff);
            gpio_set_level(PIN_CS, 1);
            spi_transaction_t t = {0};
            t.length = sizeof(ff) * 8;
            t.tx_buffer = ff;
            spi_device_transmit(dev, &t);
        }

#if SD_FIRSTPASS_DELAY_MS > 0
        /* 0236: settle time after the clocks and before the very first CMD0 of
         * this boot.  First attempt only -- later attempts keep 0234's timing. */
        if (capture_first && attempt == 1) {
            vTaskDelay(pdMS_TO_TICKS(SD_FIRSTPASS_DELAY_MS));
        }
#endif

        uint8_t r0 = sd_raw_cmd(dev, 0, 0x00000000, 0x95, 0, pl, sizeof pl);   /* GO_IDLE */
        vTaskDelay(pdMS_TO_TICKS(5));
        uint8_t r8 = sd_raw_cmd(dev, 8, 0x000001AA, 0x87, 4, pl, sizeof pl);   /* IF_COND */
        if (capture_first && attempt == 1) {
            g_fp_first_r1 = r8;
            g_fp_cmd0_first = r0;
        }
        v2 = (r8 == 0x01 && pl[2] == 0x01 && pl[3] == 0xAA);
        app_logf("    CMD8 verdict: %s (R1=0x%02x)\n",
                 v2 ? "card is v2.00+ (R7 echoes 0x1AA)" : "no valid R7 echo", r8);

        if (v2) {
            sd_raw_cmd(dev, 55, 0, 0x65, 0, pl, sizeof pl);
            uint8_t a41 = sd_raw_cmd(dev, 41, 0x40000000u, 0x77, 0, pl, sizeof pl);
            app_logf("    ACMD41 initial: R1=0x%02x\n", a41);
            uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
            while (a41 == 0x01) {
                uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
                if (now - t0 > 1000) break;
                vTaskDelay(pdMS_TO_TICKS(20));
                sd_raw_cmd(dev, 55, 0, 0x65, 0, pl, sizeof pl);
                a41 = sd_raw_cmd(dev, 41, 0x40000000u, 0x77, 0, pl, sizeof pl);
            }
            uint32_t ms41 = (uint32_t)(esp_timer_get_time() / 1000) - t0;
            app_logf("    ACMD41 final: R1=0x%02x after %" PRIu32 " ms (%s)\n", a41,
                     ms41,
                     a41 == 0x00 ? "READY" : "not ready");
            if (capture_first && (ok_attempt == 0 || attempt == ok_attempt)) {
                g_fp_acmd41_ms = (uint16_t)(ms41 > 65535 ? 65535 : ms41);
            }
            if (a41 == 0x00) {
                ready = true;
                ok_attempt = attempt;
                sd_raw_cmd(dev, 58, 0, 0xFF, 4, pl, sizeof pl);   /* OCR */
                app_logf("    OCR: %02x %02x %02x %02x (CCS/voltage)\n", pl[0], pl[1], pl[2], pl[3]);
                g_byte_addressed = (((pl[0] >> 6) & 1) == 0);
                app_logf("    ADDRESSING: %s (CCS=%d)\n",
                         g_byte_addressed ? "BYTE (SDSC: CMD17 arg = LBA x 512)"
                                          : "BLOCK (SDHC/SDXC: CMD17 arg = LBA)",
                         (pl[0] >> 6) & 1);
                sd_raw_cmd(dev, 13, 0, 0xFF, 2, pl, sizeof pl);   /* SEND_STATUS */
            }
        }
        if (!ready) vTaskDelay(pdMS_TO_TICKS(100));
    }

    app_logf("  RAW_INIT: %s on attempt %d of 5\n",
             ready ? "READY" : "FAILED", ready ? ok_attempt : 5);
    if (ready) { g_raw_ready = true; g_raw_attempt = ok_attempt; }
    write_report_partition();

    if (ready && inspect) {
        /* Contents first, while CRC checking is still off. */
        raw_inspect(dev);
        write_report_partition();

        /* The driver reaches CMD59 after an SDIO probe (CMD52 + CMD5).  Replay
         * that order and then the same commands without CMD5: if CMD59 answers
         * fine in the control and illegally in the replay, the SDIO probe is
         * what breaks it, and the workaround is to skip that step. */
        app_logf("\n  DRIVER SEQUENCE REPLAY:\n");
        sd_raw_cmd(dev, 0, 0, 0x95, 0, pl, sizeof pl);
        sd_raw_cmd(dev, 8, 0x000001AA, 0x87, 4, pl, sizeof pl);
        uint8_t r52 = sd_raw_cmd(dev, 52, 0, 0xFF, 0, pl, sizeof pl);
        uint8_t r5  = sd_raw_cmd(dev, 5, 0, 0xFF, 0, pl, sizeof pl);
        g_seq_with = sd_raw_cmd(dev, 59, 0x00000001, 0x00, 0, pl, sizeof pl);
        app_logf("    with CMD52+CMD5 first: CMD52=0x%02x CMD5=0x%02x CMD59=0x%02x\n",
                 r52, r5, g_seq_with);
        sd_raw_cmd(dev, 0, 0, 0x95, 0, pl, sizeof pl);
        sd_raw_cmd(dev, 8, 0x000001AA, 0x87, 4, pl, sizeof pl);
        g_seq_without = sd_raw_cmd(dev, 59, 0x00000001, 0x00, 0, pl, sizeof pl);
        app_logf("    without CMD5 (control): CMD59=0x%02x\n", g_seq_without);

        /* Which of the two poisons it?  CMD52 alone, then CMD5 alone. */
        sd_raw_cmd(dev, 0, 0, 0x95, 0, pl, sizeof pl);
        sd_raw_cmd(dev, 8, 0x000001AA, 0x87, 4, pl, sizeof pl);
        sd_raw_cmd(dev, 52, 0, 0xFF, 0, pl, sizeof pl);
        g_seq_52only = sd_raw_cmd(dev, 59, 0x00000001, 0x00, 0, pl, sizeof pl);
        app_logf("    CMD52 only,   then CMD59=0x%02x\n", g_seq_52only);
        sd_raw_cmd(dev, 0, 0, 0x95, 0, pl, sizeof pl);
        sd_raw_cmd(dev, 8, 0x000001AA, 0x87, 4, pl, sizeof pl);
        sd_raw_cmd(dev, 5, 0, 0xFF, 0, pl, sizeof pl);
        app_logf("    CMD5 only,    then CMD59=0x%02x\n",
                 sd_raw_cmd(dev, 59, 0x00000001, 0x00, 0, pl, sizeof pl));
        g_seq_ran = true;
        /* Leave CRC checking off and the card idle for anything that follows. */
        sd_raw_cmd(dev, 0, 0, 0x95, 0, pl, sizeof pl);
        sd_raw_cmd(dev, 59, 0x00000000, 0x00, 0, pl, sizeof pl);

        /* Then reproduce the driver's fatal step exactly: CMD59 with CRC on,
         * issued in the IDLE state (before ACMD41), which is where
         * sdmmc_init_spi_crc sends it. */
        uint8_t ridle = sd_raw_cmd(dev, 0, 0, 0x95, 0, pl, sizeof pl);
        uint8_t r59 = sd_raw_cmd(dev, 59, 0x00000001, 0xFF, 0, pl, sizeof pl);
        /* R1 bit0 means "in idle state", not "error": 0x01 from an idle card is an
         * ACCEPTED command.  Only 0x05 (illegal command) or another error bit is a
         * rejection, and 0xFF means the card drove nothing at all. */
        if (r59 == 0xFF)            g_cmd59_idle = 2;
        else if (r59 == 0x05)       g_cmd59_idle = 1;
        else if ((r59 & 0x7E) != 0) g_cmd59_idle = 3;   /* 0x7E: every error bit except the idle bit */
        else                        g_cmd59_idle = 0;
        app_logf("    CMD59 probe in IDLE state (after CMD0 R1=0x%02x): R1=0x%02x -> %s\n",
                 ridle, r59,
                 g_cmd59_idle == 0 ? "ACCEPTED (idle bit only)" :
                 g_cmd59_idle == 1 ? "illegal command (0x05)" :
                 g_cmd59_idle == 2 ? "no response" : "other error bit set");
    }

    if (ready) {
        int ok_count = 0;
        for (int i = 0; i < 10; i++) {
            uint8_t r = sd_raw_cmd(dev, 0, 0, 0x95, 0, pl, sizeof pl);
            if (r == 0x01) ok_count++;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        app_logf("    CMD0 repeatability: %d/10 answered R1=0x01 (idle)\n", ok_count);
    }

    spi_bus_remove_device(dev);
    spi_bus_free(SPI2_HOST);
}

/* --------------------------------------------- raw block read (read-only)
 * CMD17, no mount, no writes.  Needed because this card rejects CMD59
 * (CRC_ON_OFF) -- see the driver log -- so sdmmc_card_init refuses it even
 * though every command the card needs for reading answers correctly.
 */
static void cs_high_clock(spi_device_handle_t dev)
{
    uint8_t ff = 0xFF;
    gpio_set_level(PIN_CS, 1);
    spi_transaction_t t = {0};
    t.length = 8;
    t.tx_buffer = &ff;
    spi_device_transmit(dev, &t);
}

static uint8_t g_tx[1200];
static uint8_t g_rx[1200];

/* g_byte_addressed (declared above): SDSC cards take a BYTE address in CMD17,
 * SDHC/SDXC a block (LBA) address.  This card answers CCS=0. */

/* Reads nbytes of card data after the command, handling the fact that the data
 * token and part of the payload arrive inside the same SPI transfer: whatever
 * was already clocked in is taken from the poll buffer, and only the remainder
 * is clocked after it.  (Reading the payload in a fresh transfer returned idle
 * 0xFF bytes -- a silent all-0xFF "read" that looks like a card full of 0xFF.)
 */
static int raw_read_data(spi_device_handle_t dev, uint8_t cmd, uint32_t arg,
                         uint8_t *out, int nbytes)
{
    gpio_set_level(PIN_CS, 0);

    memset(g_tx, 0xFF, 40);
    memset(g_rx, 0x00, 40);
    g_tx[0] = 0x40 | cmd;
    g_tx[1] = (uint8_t)(arg >> 24);
    g_tx[2] = (uint8_t)(arg >> 16);
    g_tx[3] = (uint8_t)(arg >> 8);
    g_tx[4] = (uint8_t)(arg);
    g_tx[5] = sd_crc7(g_tx, 5);
    spi_transaction_t t = {0};
    t.length = 6 * 8;
    t.tx_buffer = g_tx;
    t.rx_buffer = g_rx;
    spi_device_transmit(dev, &t);

    const int POLL = 1100;
    uint8_t last_r1 = 0xFF;
    int copied = 0;

    for (int attempt = 0; attempt < 8 && copied < nbytes; attempt++) {
        memset(g_tx, 0xFF, POLL);
        memset(g_rx, 0x00, POLL);
        spi_transaction_t p = {0};
        p.length = (size_t)POLL * 8;
        p.tx_buffer = g_tx;
        p.rx_buffer = g_rx;
        spi_device_transmit(dev, &p);

        int start = 0;
        for (int i = 0; i < POLL; i++) {           /* R1: first byte, bit7 clear */
            if ((g_rx[i] & 0x80) == 0) { start = i; last_r1 = g_rx[i]; break; }
        }
        int tok = -1;
        for (int i = start + 1; i < POLL; i++) {
            if (g_rx[i] == 0xFE) { tok = i; break; }
            if (g_rx[i] != 0xFF && (g_rx[i] & 0xF0) == 0x00) {
                app_logf("    CMD%u arg=0x%" PRIx32 ": data error token 0x%02x\n",
                         cmd, arg, g_rx[i]);
                gpio_set_level(PIN_CS, 1);
                return -1;
            }
        }
        if (tok < 0) continue;

        int avail = POLL - (tok + 1);
        int want = nbytes - copied;
        int take = avail < want ? avail : want;
        memcpy(out + copied, g_rx + tok + 1, (size_t)take);
        copied += take;

        if (copied < nbytes) {
            int rest = nbytes - copied;
            int total = rest + 2;                  /* data remainder + CRC16 */
            memset(g_tx, 0xFF, total < 1200 ? total : 1200);
            memset(g_rx, 0x00, total < 1200 ? total : 1200);
            spi_transaction_t d = {0};
            d.length = (size_t)total * 8;
            d.tx_buffer = g_tx;
            d.rx_buffer = g_rx;
            spi_device_transmit(dev, &d);
            memcpy(out + copied, g_rx, (size_t)rest);
            copied += rest;
        }
        break;
    }

    gpio_set_level(PIN_CS, 1);
    cs_high_clock(dev);

    if (copied < nbytes) {
        app_logf("    CMD%u arg=0x%" PRIx32 ": no data token (last R1 0x%02x, %d/%d bytes)\n",
                 cmd, arg, last_r1, copied, nbytes);
        return -1;
    }
    return 0;
}

static int raw_read_block(spi_device_handle_t dev, uint32_t lba, uint8_t *out)
{
    uint32_t arg = g_byte_addressed ? (lba * 512u) : lba;
    return raw_read_data(dev, 17, arg, out, 512);      /* CMD17 READ_SINGLE_BLOCK */
}

/* CMD9/CMD10 arrive as a 16-byte data block in SPI mode. */
static int raw_read_reg(spi_device_handle_t dev, uint8_t cmd, uint8_t *out16)
{
    return raw_read_data(dev, cmd, 0, out16, 16);
}

static uint64_t csd_capacity_bytes(const uint8_t *csd)
{
    uint8_t structure = (uint8_t)(csd[0] >> 6);        /* CSD_STRUCTURE */
    if (structure == 0) {
        /* CSD v1.0 (SDSC): capacity = (C_SIZE+1) * 2^(C_SIZE_MULT+2) * 2^READ_BL_LEN */
        uint32_t read_bl_len = csd[5] & 0x0F;
        uint32_t c_size = (((uint32_t)(csd[6] & 0x03)) << 10) |
                          (((uint32_t)csd[7]) << 2) |
                          ((csd[8] & 0xC0) >> 6);
        uint32_t c_size_mult = (((uint32_t)(csd[9] & 0x03)) << 1) | ((csd[10] & 0x80) >> 7);
        uint64_t block_len = 1ull << read_bl_len;
        uint64_t mult = 1ull << (c_size_mult + 2);
        return ((uint64_t)c_size + 1) * mult * block_len;
    }
    /* CSD v2.0 (SDHC/SDXC): C_SIZE in 512 KB units */
    uint32_t c_size = ((uint32_t)(csd[7] & 0x3F) << 16) | ((uint32_t)csd[8] << 8) | csd[9];
    return ((uint64_t)c_size + 1) * 512ull * 1024ull;
}

/* ------------------------------------------------------ FAT read-only parse */
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
    app_logf("    %-13s %s %8" PRIu32 " B  start_clus=%" PRIu32 "\n", name,
             (e[11] & 0x10) ? "<DIR> " : "      ", rd32(e, 28),
             rd16(e, 26) | ((uint32_t)rd16(e, 20) << 16));
}

/* ---------------------------------------------- read-only inspection (raw)
 * Same questions as inspect_card(), but through CMD17, because this card
 * refuses CMD59 and so cannot go through sdmmc_card_init.  Nothing is written.
 */
static void raw_inspect(spi_device_handle_t dev)
{
    uint8_t blk[512];
    uint8_t reg[16];

    app_logf("\n=== 2. CARD CONTENTS (RAW, READ-ONLY) ===\n");

    g_inspect_ok = true;
    if (raw_read_reg(dev, 9, reg) == 0) {
        bool blank = true;
        for (int i = 0; i < 16; i++) if (reg[i] != 0xFF) blank = false;
        if (blank) {
            app_logf("  CSD: read returned 16 x 0xFF -- treated as a failed read, not as a card\n");
        } else {
        uint64_t bytes = csd_capacity_bytes(reg);
        memcpy(g_csd, reg, sizeof g_csd);
        app_logf("  CSD_STRUCTURE: v%u\n", (reg[0] >> 6) == 0 ? 1u : 2u);
        app_logf("  CSD: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                 reg[0], reg[1], reg[2], reg[3], reg[4], reg[5], reg[6], reg[7],
                 reg[8], reg[9], reg[10], reg[11], reg[12], reg[13], reg[14], reg[15]);
        app_logf("  SD_CAPACITY_FROM_CSD: %llu MB (%llu bytes)\n",
                 (unsigned long long)(bytes / (1024ull * 1024ull)), (unsigned long long)bytes);
        g_have_card_id = true;
        }
    } else {
        g_inspect_ok = false;
    }
    if (raw_read_reg(dev, 10, reg) == 0) {
        bool blank = true;
        for (int i = 0; i < 16; i++) if (reg[i] != 0xFF) blank = false;
        if (blank) {
            app_logf("  CID: read returned 16 x 0xFF -- failed read\n");
        } else {
        /* CID in SPI: 16 bytes.  The SPI read may or may not carry one extra
         * leading byte, so the fields are decoded at both alignments and the
         * one whose PNM is printable ASCII is named -- 0229 decoded a name from
         * bytes 4..8, which is the offset-1 alignment. */
        app_logf("  CID: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                 reg[0], reg[1], reg[2], reg[3], reg[4], reg[5], reg[6], reg[7],
                 reg[8], reg[9], reg[10], reg[11], reg[12], reg[13], reg[14], reg[15]);
        for (int sh = 0; sh <= 1; sh++) {
            char pnm[6] = {0};
            bool printable = true;
            for (int i = 0; i < 5; i++) {
                pnm[i] = (char)reg[3 + sh + i];
                if (pnm[i] < 0x20 || pnm[i] > 0x7E) printable = false;
            }
            app_logf("  SD_CID[align %d]: MID=0x%02x OID=%c%c PNM=%s PRV=%u.%u "
                     "PSN=%02x%02x%02x%02x MDT=%u/%u%s\n",
                     sh, reg[0 + sh], reg[1 + sh], reg[2 + sh], printable ? pnm : "?????",
                     reg[8 + sh] >> 4, reg[8 + sh] & 0x0F,
                     reg[9 + sh], reg[10 + sh], reg[11 + sh], reg[12 + sh],
                     2000 + (((reg[13 + sh] & 0x0F) << 4) | (reg[14 + sh] >> 4)),
                     reg[14 + sh] & 0x0F,
                     printable ? "  <- printable PNM at this alignment" : "");
        }
        {
            /* Pick the alignment by testing all five PNM bytes, not one: a
             * single byte is usually printable at either offset. */
            int sh = -1;
            for (int cand = 0; cand <= 1 && sh < 0; cand++) {
                bool ok = true;
                for (int i = 0; i < 5; i++) {
                    uint8_t c = reg[3 + cand + i];
                    if (c < 0x20 || c > 0x7E) ok = false;
                }
                if (ok) sh = cand;
            }
            if (sh < 0) sh = 0;
            char pnm[6] = {0};
            memcpy(pnm, reg + 3 + sh, 5);
            app_logf("  SD_CID_NAME: %.5s  serial=%02x%02x%02x%02x (PSN, CID bytes %d..%d)\n",
                     pnm, reg[9 + sh], reg[10 + sh], reg[11 + sh], reg[12 + sh],
                     9 + sh, 12 + sh);
        }
        }
    } else {
        g_inspect_ok = false;   /* a failed CID read is a failed inspection */
    }

    write_report_partition();
    app_logf("  STEP: reading sector 0\n");
    write_report_partition();
    if (raw_read_block(dev, 0, blk) != 0) {
        app_logf("  SD_RESULT: FAIL cannot read sector 0\n");
        g_inspect_ok = false;
        write_report_partition();
        return;
    }
    app_logf("  STEP: sector 0 read OK (%02x %02x %02x %02x)\n",
             blk[0], blk[1], blk[2], blk[3]);
    write_report_partition();

    bool is_exfat_whole = (memcmp(blk + 3, "EXFAT   ", 8) == 0);
    uint32_t part_lba = 0, part_type = 0;
    if (is_exfat_whole) {
        part_lba = 0; part_type = 0x07;
    } else if (blk[510] == 0x55 && blk[511] == 0xAA) {
        for (int i = 0; i < 4; i++) {
            const uint8_t *pe = blk + 446 + 16 * i;
            if (pe[4] == 0) continue;
            part_lba = rd32(pe, 8);
            part_type = pe[4];
            break;
        }
    }
    if (part_type == 0) {
        app_logf("  SECTOR0: no MBR partition table and no exFAT boot sector "
                 "(first bytes %02x %02x %02x %02x)\n", blk[0], blk[1], blk[2], blk[3]);
        return;
    }
    app_logf("  SD_PARTITION: type=0x%02" PRIx32 " lba=%" PRIu32 "\n", part_type, part_lba);

    /* Every MBR entry, with its own boot sector's label and root cluster: this
     * card's raw root read returned zeros while FATFS mounted a populated
     * volume, so the walk has to prove which volume is which. */
    {
        uint8_t mbr[512], bsx[512];
        if (raw_read_block(dev, 0, mbr) == 0) {
            for (int i = 0; i < 4; i++) {
                const uint8_t *pe = mbr + 446 + 16 * i;
                if (pe[4] == 0) continue;
                uint32_t lba = rd32(pe, 8);
                uint32_t nsec = rd32(pe, 12);
                char lab[12] = {0};
                uint32_t rclus = 0, fatsz_x = 0, resv_x = 0, nfats_x = 0;
                bool bs_ok = (raw_read_block(dev, lba, bsx) == 0);
                if (bs_ok) {
                    memcpy(lab, bsx + 71, 11);
                    resv_x = rd16(bsx, 14);
                    nfats_x = bsx[16];
                    fatsz_x = rd16(bsx, 22) ? rd16(bsx, 22) : rd32(bsx, 36);
                    rclus = rd32(bsx, 44);
                }
                app_logf("  MBR_ENTRY%d: type=0x%02x lba=%" PRIu32 " sectors=%" PRIu32
                         " boot=%s label='%.11s' root_clus=%" PRIu32 "\n",
                         i, pe[4], lba, nsec, bs_ok ? "OK" : "READ FAILED", lab, rclus);
                if (bs_ok) {
                    /* the entry's own start LBA is part of the address (the
                     * same omission that was just fixed in the FAT32 walk) */
                    uint32_t dlba = lba + resv_x + nfats_x * fatsz_x +
                                    (rclus > 1 ? (rclus - 2) * bsx[13] : 0);
                    uint8_t rb[512];
                    int rrc = raw_read_block(dev, dlba, rb);
                    app_logf("    -> data_lba=%" PRIu32 " first bytes %s: %02x %02x %02x %02x\n",
                             dlba, rrc == 0 ? "OK" : "READ FAILED",
                             rrc == 0 ? rb[0] : 0, rrc == 0 ? rb[1] : 0,
                             rrc == 0 ? rb[2] : 0, rrc == 0 ? rb[3] : 0);
                }
            }
        }
    }

    if (raw_read_block(dev, part_lba, blk) != 0) {
        app_logf("  SD_RESULT: FAIL cannot read boot sector at LBA %" PRIu32 "\n", part_lba);
        g_inspect_ok = false;
        return;
    }
    const uint8_t *bs = blk;

    if (memcmp(bs + 3, "EXFAT   ", 8) == 0) {
        uint32_t bps_shift = bs[108], spc_shift = bs[109];
        uint32_t cluster_heap = rd32(bs, 88), root_clus = rd32(bs, 96);
        uint64_t volume_sectors = (uint64_t)rd32(bs, 72) | ((uint64_t)rd32(bs, 76) << 32);
        app_logf("  SD_FS: exFAT sector=%u B cluster=%u B volume=%llu MB\n",
                 1u << bps_shift, (1u << bps_shift) << spc_shift,
                 (unsigned long long)(volume_sectors * (1u << bps_shift) / (1024 * 1024)));
        uint32_t root_lba = cluster_heap + (root_clus - 2) * (1u << spc_shift);
        if (raw_read_block(dev, root_lba, blk) == 0) {
            app_logf("  SD_ROOT (exFAT):\n");
            int shown = 0;
            for (int off = 0; off < 512 && shown < 16; off += 32) {
                const uint8_t *e = blk + off;
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
        } else {
            g_inspect_ok = false;
        }
        app_logf("  SD_RESULT: %s (CMD17 only, nothing written)\n",
                 g_inspect_ok ? "read-only inspection OK" : "inspection INCOMPLETE - a read failed");
        return;
    }

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
    if (total_sectors == 0 || total_sectors > 0x4000000ull || fatsz == 0 || bps == 0) {
        app_logf("  SD_RESULT: boot sector is not usable (sectors=%llu fatsz=%" PRIu32
                 " bps=%" PRIu32 ") -- aborting before any directory walk\n",
                 (unsigned long long)total_sectors, fatsz, bps);
        return;
    }
    uint32_t data_sectors = (uint32_t)(total_sectors - (reserved + nfats * fatsz +
                                 (root_entries * 32 + bps - 1) / bps));
    uint32_t clusters = data_sectors / (spc ? spc : 1);
    const char *fstype = clusters < 4085 ? "FAT12" : (clusters < 65525 ? "FAT16" : "FAT32");

    app_logf("  SD_FS: %s sector=%" PRIu32 " B cluster=%" PRIu32 " B clusters=%" PRIu32 "\n",
             fstype, bps, bps * spc, clusters);
    app_logf("  SD_CAPACITY_BS: %llu MB (%llu sectors of %" PRIu32 " B)\n",
             (unsigned long long)(total_sectors * bps / (1024 * 1024)),
             (unsigned long long)total_sectors, bps);
    app_logf("  SD_LABEL: %.11s\n", label);

    /* Capacity cross-check (0234), read-only: if the CSD claims a capacity far
     * larger than the volume, the two must agree with each other or one of
     * them is fiction -- a classic counterfeit-card signature.  Read the last
     * block the CSD declares and a block past a 512 MB card. */
    uint64_t csd_bytes = g_have_card_id ? csd_capacity_bytes(g_csd) : 0;
    if (!g_have_card_id || csd_bytes < 1024ull) {
        /* A non-0xFF CSD is not necessarily a sane one: a CSD_STRUCTURE this
         * decoder does not know can compute a capacity under one block, and
         * `capacity/512 - 1` then underflows to UINT32_MAX.  Skip instead. */
        app_logf("  CAPACITY_PROBE: skipped -- %s\n",
                 g_have_card_id ? "CSD capacity is implausibly small"
                                : "no valid CSD was decoded");
    } else {
        uint32_t last_lba = (uint32_t)(csd_bytes / 512ull) - 1u;
        uint8_t tb[512];
        int rc1 = raw_read_block(dev, 1000000u, tb);        /* past 512 MB */
        app_logf("  CAPACITY_PROBE: LBA 1000000 (past 512 MB) -> %s%s\n",
                 rc1 == 0 ? "read OK, first bytes" : "read FAILED",
                 rc1 == 0 ? ":" : "");
        if (rc1 == 0) {
            app_logf("    [%02x %02x %02x %02x %02x %02x %02x %02x]\n",
                     tb[0], tb[1], tb[2], tb[3], tb[4], tb[5], tb[6], tb[7]);
        }
        int rc2 = raw_read_block(dev, last_lba, tb);        /* CSD's last block */
        app_logf("  CAPACITY_PROBE: CSD last LBA %" PRIu32 " (%llu MB) -> %s\n",
                 last_lba, (unsigned long long)(csd_bytes / (1024 * 1024)),
                 rc2 == 0 ? "read OK" : "read FAILED");
        if (rc2 == 0) {
            app_logf("    [%02x %02x %02x %02x %02x %02x %02x %02x]\n",
                     tb[0], tb[1], tb[2], tb[3], tb[4], tb[5], tb[6], tb[7]);
        }
    }

    /* 0234 fix: every offset below is PARTITION-relative, so the volume's base
     * LBA has to be added back before it is used as an absolute sector number.
     * 0229 omitted part_lba: on a card whose partition starts at 2048 the FAT32
     * root was read 2048 sectors too low (12320 instead of 14368) and returned
     * zeros -- which the read-back reported as "root empty". */
    uint32_t data_lba = part_lba + reserved + nfats * fatsz + (root_entries * 32) / bps;
    app_logf("  SD_BPB: reserved=%" PRIu32 " nfats=%" PRIu32 " fatsz=%" PRIu32
             " root_entries=%" PRIu32 " total=%llu\n",
             reserved, nfats, fatsz, root_entries, (unsigned long long)total_sectors);
    app_logf("  SD_ROOT (first entries):\n");

    int shown = 0;
    bool first_file_saved = false;
    uint32_t dir_lba = (clusters < 65525) ? (part_lba + reserved + nfats * fatsz) : 0;
    if (clusters >= 65525) {
        /* FAT32: the root directory is a cluster chain in the DATA area, so its
         * first sector is data_lba + (root_clus-2)*spc -- not reserved-area
         * relative.  Omitting data_lba reads the FAT table and prints it as
         * garbage directory entries. */
        uint32_t root_clus = rd32(bs, 44);
        dir_lba = data_lba + (root_clus - 2) * spc;
        app_logf("  SD_ROOT_CLUSTER: %" PRIu32 " -> dir_lba %" PRIu32
                 " (part_lba %" PRIu32 ", data_lba %" PRIu32 ", spc %" PRIu32 ")\n",
                 root_clus, dir_lba, part_lba, data_lba, spc);
    }
    uint32_t dir_sectors = (clusters < 65525)
                         ? ((root_entries * 32 + bps - 1) / bps)
                         : spc;

    for (uint32_t s = 0; s < dir_sectors && shown < 16; s++) {
        if (raw_read_block(dev, dir_lba + s, blk) != 0) {
            app_logf("    (read error at dir sector %" PRIu32 ")\n", s);
            g_inspect_ok = false;
            break;
        }
        if (s == 0) {
            app_logf("    ROOT_RAW[%02x %02x %02x %02x %02x %02x %02x %02x | %02x %02x %02x %02x %02x %02x %02x %02x]\n",
                     blk[0], blk[1], blk[2], blk[3], blk[4], blk[5], blk[6], blk[7],
                     blk[8], blk[9], blk[10], blk[11], blk[12], blk[13], blk[14], blk[15]);
            /* Read stability at this LBA (0234): the mount path listed a
             * populated root while this raw read returned zeros on the first
             * run, so the same LBA is re-read five times and compared. */
            uint8_t blk2[512];
            for (int k = 0; k < 5; k++) {
                if (raw_read_block(dev, dir_lba, blk2) != 0) {
                    app_logf("    ROOT_REREAD %d: read failed\n", k);
                    g_inspect_ok = false;
                    continue;
                }
                if (memcmp(blk, blk2, 512) != 0) {
                    app_logf("    ROOT_REREAD %d: DIFFERS (first %02x %02x %02x %02x)\n", k,
                             blk2[0], blk2[1], blk2[2], blk2[3]);
                } else {
                    app_logf("    ROOT_REREAD %d: identical to the first read\n", k);
                }
            }
        }
        bool stop = false;
        for (int off = 0; off < 512 && shown < 16; off += 32) {
            const uint8_t *e = blk + off;
            if (e[0] == 0x00) { stop = true; break; }
            if (e[0] == 0xE5) continue;
            if ((e[11] & 0x3F) == 0x0F) continue;
            if (e[11] & 0x08) continue;
            print_fat_name(e);
            shown++;
            if (!first_file_saved && !(e[11] & 0x10)) {
                uint32_t clus = rd16(e, 26) | ((uint32_t)rd16(e, 20) << 16);
                uint32_t size = rd32(e, 28);
                uint32_t file_lba = data_lba + (clus - 2) * spc;
                /* Its own buffer: reading the file into the directory buffer
                 * would make the rest of this chunk parse as directory
                 * entries (a silent source of bogus listings). */
                static uint8_t fb[512];
                if (size == 0) {
                    app_logf("    FIRST FILE is zero length (%" PRIu32 " B) -- nothing to read\n", size);
                    first_file_saved = true;
                    continue;
                }
                if (clus >= 2 && raw_read_block(dev, file_lba, fb) == 0) {
                    char head[33]; int k = 0;
                    for (int i = 0; i < 32 && i < (int)size; i++) {
                        uint8_t c = fb[i];
                        head[k++] = (c >= 32 && c < 127) ? (char)c : '.';
                    }
                    head[k] = 0;
                    app_logf("    FIRST FILE HEAD (%" PRIu32 " B): %s\n",
                             size > 32 ? (uint32_t)32 : size, head);
                    app_logf("    FIRST FILE HEX: ");
                    for (int i = 0; i < 16 && i < (int)size; i++) app_logf("%02x ", fb[i]);
                    app_logf("\n");
                } else {
                    g_inspect_ok = false;   /* the file it points at is unreadable */
                }
                first_file_saved = true;
            }
        }
        if (stop) break;
    }
    if (!shown) app_logf("    (empty)\n");
    app_logf("  SD_RESULT: %s (CMD17 only, nothing written)\n",
             g_inspect_ok ? "read-only inspection OK" : "inspection INCOMPLETE - a read failed");
}

/* ------------------------------------------------------------ rung 1 + 3 */
static bool try_spi(const char *tag, uint32_t khz)
{
    app_logf("\n--- SPI sdmmc_card_init [%s] at %" PRIu32 " kHz ---\n", tag, khz);

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) { app_logf("  FAIL spi_bus_initialize=%s\n", esp_err_to_name(err)); return false; }

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SPI2_HOST;
    slot.gpio_cs = PIN_CS;
    err = sdspi_host_init();
    if (err != ESP_OK) {
        app_logf("  FAIL sdspi_host_init=%s\n", esp_err_to_name(err));
        spi_bus_free(SPI2_HOST);
        return false;
    }
    sdspi_dev_handle_t dev = 0;
    err = sdspi_host_init_device(&slot, &dev);
    if (err != ESP_OK) {
        app_logf("  FAIL init_device=%s\n", esp_err_to_name(err));
        sdspi_host_deinit();
        spi_bus_free(SPI2_HOST);
        return false;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = dev;
    host.max_freq_khz = khz;

    sdmmc_card_t *card = heap_caps_calloc(1, sizeof(sdmmc_card_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!card) { app_logf("  FAIL no card struct memory\n"); goto done; }

    err = sdmmc_card_init(&host, card);
    if (err != ESP_OK) {
        app_logf("  SD_CARD_INIT[%s %" PRIu32 " kHz]: FAIL %s (%d)\n",
                 tag, khz, esp_err_to_name(err), (int)err);
        heap_caps_free(card);
        goto done;
    }
    app_logf("  SD_CARD_INIT[%s %" PRIu32 " kHz]: OK\n", tag, khz);
    app_logf("  SD_CARD: name=%.*s type=%s speed=%" PRIu32 " kHz size=%llu MB\n",
             8, card->cid.name, (card->is_mmc ? "MMC" : (card->is_sdio ? "SDIO" : "SD")),
             card->real_freq_khz,
             (unsigned long long)((uint64_t)card->csd.capacity * card->csd.sector_size / (1024 * 1024)));
    /* Content inspection lives in raw_inspect() only: one lister, one buffer
     * discipline, and it is the path that actually runs on this card. */
    heap_caps_free(card);
    sdspi_host_remove_device(dev);
    sdspi_host_deinit();
    spi_bus_free(SPI2_HOST);
    return true;

done:
    sdspi_host_remove_device(dev);
    sdspi_host_deinit();
    spi_bus_free(SPI2_HOST);
    return false;
}

/* ---------------------------------------------------- stock driver + mount
 * 0234 step 4: the question 0229 left open is whether THIS card tolerates the
 * driver's CMD5 probe, so the stock path is walked all the way to a mounted
 * volume, not just sdmmc_card_init.  The card is READ-ONLY this run (Stephen
 * has not designated it scratch): mount with format_if_mount_failed=false and
 * only ever read -- opendir/readdir and f_open(..., "rb").
 */
static bool try_spi_mount(const char *tag, uint32_t khz)
{
    app_logf("\n--- STOCK DRIVER + MOUNT [%s] at %" PRIu32 " kHz ---\n", tag, khz);

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) { app_logf("  MOUNT_FAIL spi_bus_initialize=%s\n", esp_err_to_name(err)); return false; }

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SPI2_HOST;
    slot.gpio_cs = PIN_CS;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = khz;

    esp_vfs_fat_mount_config_t mcfg = {
        .format_if_mount_failed = false,   /* READ-ONLY run: never format */
        .max_files = 4,
        .allocation_unit_size = 0,
    };
    sdmmc_card_t *card = NULL;
    err = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mcfg, &card);
    if (err != ESP_OK) {
        app_logf("  MOUNT[%s %" PRIu32 " kHz]: FAIL %s (%d)\n", tag, khz,
                 esp_err_to_name(err), (int)err);
        spi_bus_free(SPI2_HOST);
        return false;
    }

    app_logf("  MOUNT[%s %" PRIu32 " kHz]: OK card=%s speed=%" PRIu32 " kHz size=%llu MB\n",
             tag, khz, card->cid.name, card->real_freq_khz,
             (unsigned long long)((uint64_t)card->csd.capacity * card->csd.sector_size / (1024 * 1024)));

    int entries = 0;
    DIR *d = opendir("/sdcard");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            app_logf("  ROOT_ENTRY: %s\n", de->d_name);
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            entries++;
        }
        closedir(d);
        app_logf("  ROOT_ENTRIES: %d\n", entries);
    } else {
        app_logf("  ROOT_ENTRIES: opendir failed (errno %d)\n", errno);
    }

    /* First bytes of the first file present, read-only. */
    if (entries > 0) {
        char path[300] = {0};
        d = opendir("/sdcard");
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
                char cand[280];
                snprintf(cand, sizeof cand, "/sdcard/%s", de->d_name);
                struct stat st;
                if (stat(cand, &st) != 0 || !S_ISREG(st.st_mode)) continue;   /* skip dirs */
                snprintf(path, sizeof path, "%s", cand);
                app_logf("  FIRST_FILE_PICK: %s (%ld bytes)\n", path, (long)st.st_size);
                break;
            }
            closedir(d);
        }
        if (path[0]) {
            FILE *f = fopen(path, "rb");     /* read-only */
            if (f) {
                uint8_t head[32];
                size_t got = fread(head, 1, sizeof head, f);
                app_logf("  FIRST_FILE: %s first %u byte(s):", path, (unsigned)got);
                for (size_t i = 0; i < got; i++) app_logf(" %02x", head[i]);
                app_logf("\n");
                fclose(f);
            } else {
                app_logf("  FIRST_FILE: %s open for read failed (errno %d)\n", path, errno);
            }
        }
    }

    esp_vfs_fat_sdcard_unmount("/sdcard", card);
    spi_bus_free(SPI2_HOST);
    return true;
}

/* --------------------------------------------------------------- rung 4 */
static bool try_sdmmc_1bit(uint32_t khz)
{
    app_logf("\n--- SDMMC 1-bit [same pins] at %" PRIu32 " kHz ---\n", khz);
    app_logf("  CLK=%d CMD=%d D0=%d (CS %d unused in SD mode; card-side DAT3 pull-up assumed)\n",
             PIN_SCK, PIN_MOSI, PIN_MISO, PIN_CS);

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;
    host.max_freq_khz = khz;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = PIN_SCK;
    slot.cmd = PIN_MOSI;
    slot.d0  = PIN_MISO;
    slot.width = 1;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = sdmmc_host_init();
    if (err != ESP_OK) {
        app_logf("  FAIL sdmmc_host_init=%s\n", esp_err_to_name(err));
        return false;
    }
    err = sdmmc_host_init_slot(host.slot, &slot);
    if (err != ESP_OK) {
        app_logf("  FAIL sdmmc_host_init_slot=%s\n", esp_err_to_name(err));
        return false;
    }

    sdmmc_card_t *card = heap_caps_calloc(1, sizeof(sdmmc_card_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!card) { app_logf("  FAIL no memory\n"); sdmmc_host_deinit(); return false; }

    err = sdmmc_card_init(&host, card);
    if (err != ESP_OK) {
        app_logf("  SDMMC_CARD_INIT[1-bit %" PRIu32 " kHz]: FAIL %s (%d)\n",
                 khz, esp_err_to_name(err), (int)err);
        heap_caps_free(card);
        sdmmc_host_deinit();
        return false;
    }
    app_logf("  SDMMC_CARD_INIT[1-bit %" PRIu32 " kHz]: OK\n", khz);
    app_logf("  SD_CARD: name=%.*s size=%llu MB\n", 8, card->cid.name,
             (unsigned long long)((uint64_t)card->csd.capacity * card->csd.sector_size / (1024 * 1024)));
    heap_caps_free(card);
    sdmmc_host_deinit();
    return true;
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(300));

    /* Driver logs go into the flash report: the sdmmc step names are the
     * diagnosis, and the FT232R console cannot deliver them. */
    esp_log_set_vprintf(report_vprintf);
    esp_log_level_set("sdmmc_cmd", ESP_LOG_DEBUG);
    esp_log_level_set("sdmmc_sd", ESP_LOG_DEBUG);
    esp_log_level_set("sdmmc_common", ESP_LOG_DEBUG);
    esp_log_level_set("sdmmc_init", ESP_LOG_DEBUG);
    esp_log_level_set("sdmmc_io", ESP_LOG_DEBUG);
    esp_log_level_set("sdspi_host", ESP_LOG_DEBUG);
    esp_log_level_set("sdspi_transaction", ESP_LOG_VERBOSE);

    app_logf("\n\n########## 0234 SD RETEST (0229 probe extended) ##########\n");
    app_logf("build: %s %s\n", __DATE__, __TIME__);
    app_logf("FIRSTPASS_DELAY_MS: %d (0236; applies to attempt 1 only)\n", SD_FIRSTPASS_DELAY_MS);

    /* Cross-reset first-pass table (0234 step 3). */
    fp_load();
    app_logf("FIRSTPASS_BOOT: %d (slot index for this boot)\n", g_slot_n + 1);

    report_identity();
    write_report_partition();   /* partial flush: a later fault still leaves this */

    /* SD spec: the card needs its own power-up time before the first command. */
    vTaskDelay(pdMS_TO_TICKS(250));

    /* Line levels before anything drives them: is MISO being held, and by
     * what?  A high reading with no pull-ups means an external pull-up or a
     * card/driver holding the line; a floating read means an open line. */
    report_line_levels("idle", false);
    report_line_levels("idle", true);

    raw_probe("no-internal-pullup", false, true);
    write_report_partition();

    /* Record this boot's first-pass evidence before anything that can hang or
     * fault: the slot has to survive the reset that produced it. */
    fp_append((uint8_t)(g_raw_ready ? 1 : 0), g_fp_first_r1,
              (uint8_t)g_raw_attempt, g_fp_acmd41_ms, g_fp_cmd0_first);
    write_report_partition();
    raw_probe("internal-pullup", true, false);
    write_report_partition();

    /* 0234 step 4: the stock driver all the way to a mounted volume. */
    bool mounted = false;
    mounted |= try_spi_mount("mount 400kHz", 400);
    write_report_partition();
    if (!mounted) {
        mounted |= try_spi_mount("mount default", 20000);
        write_report_partition();
    }
    app_logf("MOUNT_VERDICT: %s\n",
             mounted ? "the stock driver MOUNTS this card (read-only listing above)"
                     : "the stock driver does not mount this card -- see the MOUNT lines");

    bool ok = false;
    ok |= try_spi("rung1 400kHz", 400);
    ok |= try_spi("rung1 100kHz", 100);
    ok |= try_spi("rung1 default", 20000);
    if (!ok) {
        ok |= try_sdmmc_1bit(400);
        ok |= try_sdmmc_1bit(20000);
    }

    /* Verdicts are separated on purpose: a card that answers at command level but
     * cannot go through sdmmc_card_init is exactly this task's finding, and a
     * single FAIL line would erase it. */
    fp_report();

    app_logf("\n=== 3. VERDICTS ===\n");
    app_logf("SD_VERDICT_RAW: %s\n", g_raw_ready
             ? "the card ANSWERS on SPI (raw init READY)"
             : "the card did not answer the raw SPI probe");
    if (g_raw_ready && g_raw_attempt > 1) {
        app_logf("SD_NOTE_RAW: init needed %d attempts after reset (first pass fails at CMD8 "
                 "with R1=0x05) -- a marginal contact or a slow power-up, to be settled by "
                 "rung 2's meter readings\n", g_raw_attempt);
    }
    if (ok) {
        app_logf("SD_VERDICT_DRIVER: sdmmc_card_init succeeded\n");
    } else if (g_seq_ran && g_seq_with == 0x05
               && (g_seq_without == 0x00 || g_seq_without == 0x01)      /* control ACCEPTED */
               && (g_seq_52only  == 0x00 || g_seq_52only  == 0x01)) {   /* CMD52 control ACCEPTED */
        app_logf("SD_VERDICT_DRIVER: sdmmc_card_init fails at sdmmc_init_spi_crc because the "
                 "driver issues an SDIO probe (CMD5) before it: after CMD5 this card answers "
                 "0x05 (illegal command) to CMD59, and to the same CMD59 with no CMD5 in the "
                 "sequence it answers 0x%02x. Controls in the replay isolate CMD5 -- CMD52 "
                 "alone leaves CMD59 working. A card firmware quirk, not a wiring fault; the "
                 "driver cannot be talked out of the CMD5 probe, so the SPI init sequence has "
                 "to be bypassed for this card\n", g_seq_without);
    } else if (g_seq_ran && g_seq_with == 0x05 && g_seq_without != 0x05) {
        app_logf("SD_VERDICT_DRIVER: sdmmc_card_init fails at sdmmc_init_spi_crc; a pre-CMD59 "
                 "probe breaks this card's command handling, but the controls do not isolate "
                 "which one (CMD52 alone -> 0x%02x, no probe -> 0x%02x). Mechanism undetermined\n",
                 g_seq_52only, g_seq_without);
    } else if (g_cmd59_idle == 1) {
        app_logf("SD_VERDICT_DRIVER: sdmmc_card_init fails at sdmmc_init_spi_crc: CMD59 "
                 "returns 0x05 (illegal command) inside the driver's own sequence, while a "
                 "hand-driven CMD59 with the same argument answers R1=0x01 on this card -- "
                 "sequence/timing dependent, mechanism not isolated at this level\n");
    } else {
        app_logf("SD_VERDICT_DRIVER: sdmmc_card_init failed for a reason this probe did not "
                 "isolate (CMD59 probe: %s) -- see the driver log above\n",
                 g_cmd59_idle == 0 ? "accepted (R1 with only the idle bit set)" :
                 g_cmd59_idle == 1 ? "illegal command (0x05)" :
                 g_cmd59_idle == 2 ? "no response" :
                 g_cmd59_idle == 3 ? "other error bit" : "not reached");
    }
    if (g_have_card_id) {
        app_logf("SD_VERDICT_CARD: identified (see CSD/CID/FAT lines above)\n");
    }
    app_logf("SD_VERDICT_READ: %s\n", g_inspect_ok
             ? "read-only inspection completed through CMD17"
             : "read-only inspection did not complete - a read failed");
    if (!g_raw_ready) {
        app_logf("NEXT: rung 2 (meter) - 3.3 V at the holder's VCC with the card seated and "
                 "init attempted; continuity from each holder pad to its ESP32 pin; shorts "
                 "between adjacent lines.  Firmware cannot measure that.\n");
    } else if (!ok) {
        app_logf("NEXT: the write path needs either the driver's CMD59 step to be worked around "
                 "(bypass sdmmc_init_spi_crc) or the sequence dependence isolated; rung 2's "
                 "meter readings are still owed.\n");
    }

    app_logf("\n########## PROBE COMPLETE ##########\n");
    fflush(stdout);
    write_report_partition();

    for (;;) vTaskDelay(pdMS_TO_TICKS(5000));
}
