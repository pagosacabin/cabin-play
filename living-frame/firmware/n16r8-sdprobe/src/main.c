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
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "mbedtls/sha256.h"
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

/* 0238 -- framing of the first transaction after reset.
 *
 * 0236 killed the timing hypothesis (0-250 ms changed nothing, 65 boots) and
 * exposed the shape of the fault instead: on attempt 1 CMD0 answers with a
 * 0xF8 lead byte in front of a plausible R1 (ff ff f8 47) and CMD8's R7 echo
 * arrives one byte late (ff 00 00 00 01 aa).  The first frame is *shifted*, not
 * slow, so this task tests two framing variants between the 74+ dummy clocks
 * and the first real CMD0:
 *
 *   0 = control           -- unmodified 0236 arrangement (default)
 *   1 = sacrificial frame -- one throwaway CMD0 after the dummy clocks, its
 *                            response logged and discarded; the real attempt
 *                            then runs exactly as the control does
 *   2 = clean CS re-assert-- CS driven high, held >= 1 ms, then the real CMD0
 *                            is issued in the following CS-low period
 *
 * Build-time only, and the framing variants add nothing else: clock stays
 * 400 kHz, the delay stays at SD_FIRSTPASS_DELAY_MS, pull-ups unchanged.
 *   PLATFORMIO_BUILD_FLAGS="-DSD_FRAMING_VARIANT=1" pio run -e n16r8-sdprobe
 */
#ifndef SD_FRAMING_VARIANT
#define SD_FRAMING_VARIANT 0
#endif
#if SD_FRAMING_VARIANT < 0 || SD_FRAMING_VARIANT > 2
#error "SD_FRAMING_VARIANT out of range (0=control, 1=sacrificial frame, 2=CS re-assert)"
#endif

/* 0239 -- does a raw pre-sacrifice make the STOCK driver's first init clean?
 *
 * 0238 proved one throwaway CMD0 after the dummy clocks makes the RAW probe's
 * first init pass reliable (13/13 vs 0/13 control).  This task asks whether the
 * same sacrifice carries over to the stock ESP-IDF driver: per reset, ONE
 * sdmmc_card_init() call is the measurement, and the arm decides whether a raw
 * pre-sacrifice (74+ dummy clocks with CS high, one throwaway CMD0, response
 * logged and discarded, CS high) precedes it.
 *
 *   0 = control   -- one sdmmc_card_init, nothing before it
 *   1 = sacrifice -- raw pre-sacrifice, then the identical single init call
 *
 * Defining SD_FOLD_ARM selects the compact 0239 flow in app_main(); leaving it
 * undefined leaves the 0229/0234/0236/0238 probe byte-for-byte in place.  One
 * variable only: 400 kHz, SD_FIRSTPASS_DELAY_MS stays 0, pull-ups unchanged,
 * card read-only.
 *   PLATFORMIO_BUILD_FLAGS="-DSD_FOLD_ARM=1" pio run -e n16r8-sdprobe
 */
#ifdef SD_FOLD_ARM
#if SD_FOLD_ARM < 0 || SD_FOLD_ARM > 1
#error "SD_FOLD_ARM out of range (0=control, 1=pre-sacrifice)"
#endif
#define SD_FOLD_TASK 1
#else
#define SD_FOLD_TASK 0
#endif

/* 0240 -- the kit card's write path: identity gate, driver init behind the
 * 0239 pre-sacrifice, format, write, read back, remount, read back again.
 *
 * This arm REQUIRES the 0239 block (it reuses fold_presacrifice() and
 * fold_one_init()), so it is built as `-DSD_FOLD_ARM=1 -DSD_KIT_TASK=1`.
 * DESTRUCTIVE to the card that is in the holder -- the identity gate below is
 * what keeps it from touching the 512 GB Recalbox card: nothing is written
 * unless the seated card reads back as the ~1 GB SDSC kit card.
 *   PLATFORMIO_BUILD_FLAGS="-DSD_FOLD_ARM=1 -DSD_KIT_TASK=1" pio run -e n16r8-sdprobe
 */
#ifdef SD_KIT_TASK
#if !SD_FOLD_TASK
#error "SD_KIT_TASK needs SD_FOLD_ARM=1 (it reuses the 0239 pre-sacrifice)"
#endif
#define SD_KIT_FLOW 1
#else
#define SD_KIT_FLOW 0
#endif

/* 0241 -- the NEW card's write path.  Same chain as 0240 (identity read ->
 * 0239 pre-sacrifice -> one stock-driver init -> explicit format -> 128 KiB
 * pattern -> read back -> unmount/remount -> read back again), with one
 * difference: the gate AUTHORIZES any seated card except the two identities
 * this bench must never touch.  The 1 GB kit card is refused because 0240
 * already measured it, and the 512 GB SanDisk SR512 (Recalbox) is refused
 * absolutely -- it is never scratch, under any reading of any task.
 *
 * Built as `-DSD_FOLD_ARM=1 -DSD_NEWCARD_TASK=1`.  The destructive path is
 * still gated: DESTRUCTIVE only to a card that passes the authorization test.
 *   PLATFORMIO_BUILD_FLAGS="-DSD_FOLD_ARM=1 -DSD_NEWCARD_TASK=1" pio run -e n16r8-sdprobe
 */
#ifdef SD_NEWCARD_TASK
#if !SD_FOLD_TASK
#error "SD_NEWCARD_TASK needs SD_FOLD_ARM=1 (it reuses the 0239 pre-sacrifice)"
#endif
/* SD_KIT_FLOW is already defined (0) by the SD_KIT_TASK block above, so the
 * earlier `#ifndef` guard silently left the write flow compiled OUT and the
 * build ran the 0239 fold app instead (peer review, 2026-11-22, BLOCKER). */
#undef  SD_KIT_FLOW
#define SD_KIT_FLOW 1
#define SD_NEWCARD_POLICY 1
#define TASK_NO "0241"
#else
#define SD_NEWCARD_POLICY 0
#define TASK_NO "0240"
#endif

/* The write flow exists only when one of the two tasks selected it. */
#if SD_KIT_FLOW && !defined(SD_KIT_TASK) && !defined(SD_NEWCARD_TASK)
#error "SD_KIT_FLOW is set but no task selected it"
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

#if SD_FOLD_TASK
/* 0239: how many CMD0 frames the stock driver itself issued in the current
 * init call, counted from its own verbose log (one per line, keyed on the
 * `CMD0,` field sdspi_host prints).  The driver sends CMD0 twice by design in
 * SPI mode (sdmmc_send_cmd_go_idle_state), so a count of 2 means its internal
 * retry ran -- the first frame was absorbed, not used. */
static uint16_t g_fold_cmd0_frames;
#endif

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
    /* 0238: own namespace.  0234/0236's "sdprobe0234" namespace was observed to
     * hand its table to a new build whose stamp differed (first 0238 boot
     * reported FIRSTPASS_BOOT: 16 with 15 rows carrying the control's
     * signatures), which would misreport a variant's first-pass rate.  A
     * separate namespace makes each 0238 run start from an empty table without
     * touching the accepted 800x600 build's settings in NVS. */
    err = nvs_open("sdprobe0238", NVS_READWRITE, &g_nvs);
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
    app_logf("FIRSTPASS_FRAMING: variant=%d (%s)\n", SD_FRAMING_VARIANT,
             SD_FRAMING_VARIANT == 0 ? "control"
               : SD_FRAMING_VARIANT == 1 ? "sacrificial frame" : "CS re-assert");
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
        bool to_report = true;
#if SD_NEWCARD_POLICY
        /* 0241: the report partition is 64 KiB and the driver emits hundreds of
         * DEBUG/VERBOSE lines per mount at 400 kHz, which filled the 60 kB
         * buffer before the format even started -- every later line (the format
         * result, all three read-back hashes and the verdict) was silently
         * dropped.  Keep only INFO and above in the report; the D/V lines still
         * go to the UART.  esp_log's default format puts the level letter and a
         * space first. */
        if ((line[0] == 'D' || line[0] == 'V') && line[1] == ' ') to_report = false;
#endif
        if (to_report && g_report_len + len < sizeof(g_report)) {
            memcpy(g_report + g_report_len, line, len);
            g_report_len += len;
        }
        fputs(line, stdout);
#if SD_FOLD_TASK
        /* 0239: count the driver's own CMD0 frames from its log. */
        for (const char *p = line; (p = strstr(p, "CMD0,")) != NULL; p += 5) {
            if (g_fold_cmd0_frames != 0xFFFF) g_fold_cmd0_frames++;
        }
#endif
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

/* 0238: log-line tag for the sacrificial frame (empty for every real command). */
static const char *g_cmd_tag = "";

/* 0239: sd_raw_cmd returns 0xFF for BOTH "the card drove nothing" and "the SPI
 * transfer itself failed", and those are different findings for a sacrifice
 * that must actually reach the bus.  This flag separates them; the base probe
 * never reads it, so its behaviour is unchanged. */
static bool g_raw_tx_err;

static uint8_t sd_raw_cmd(spi_device_handle_t dev, uint8_t cmd, uint32_t arg,
                          uint8_t crc, int extra, uint8_t *out, int out_max)
{
    /* 0238: the sacrificial frame goes through this same path so its timing is
     * the control's; g_cmd_tag only makes its log lines distinguishable. */
    uint8_t tx[32], rx[32];
    memset(tx, 0xFF, sizeof tx);
    memset(rx, 0x00, sizeof rx);
    g_raw_tx_err = false;

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
        /* 0239: the frame is only "sent and cleaned up" if this succeeds too. */
        if (spi_device_transmit(dev, &tail) != ESP_OK) g_raw_tx_err = true;
    }

    if (err != ESP_OK) {
        app_logf("    %sCMD%-2u: spi transfer failed (%s)\n", g_cmd_tag, cmd, esp_err_to_name(err));
        g_raw_tx_err = true;
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
        app_logf("    %sCMD%-2u arg=0x%08" PRIx32 ": NO RESPONSE (raw %s)\n", g_cmd_tag, cmd, arg, hex);
        return 0xFF;
    }

    int copied = 0;
    for (int i = idx + 1; i < n && copied < out_max && copied < extra; i++, copied++) {
        out[copied] = rx[i];
    }
    app_logf("    %sCMD%-2u arg=0x%08" PRIx32 ": R1=0x%02x payload[%d]=%s\n",
             g_cmd_tag, cmd, arg, rx[idx], copied, hex);
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

#if SD_FRAMING_VARIANT == 1
        /* 0238 rung 1 -- sacrificial frame.  One throwaway CMD0 whose response
         * is logged and discarded; the real measurement below then runs exactly
         * as the control does, one transaction later. */
        if (capture_first && attempt == 1) {
            app_logf("  SACRIFICIAL FRAME (0238 rung 1)\n");
            g_cmd_tag = "SACRIFICE ";
            uint8_t sp[8] = {0};
            (void)sd_raw_cmd(dev, 0, 0x00000000, 0x95, 0, sp, sizeof sp);
            g_cmd_tag = "";
        }
#elif SD_FRAMING_VARIANT == 2
        /* 0238 rung 2 -- clean CS re-assert: CS high, held >= 1 ms, then the
         * real CMD0 is issued in the CS-low period that follows. */
        if (capture_first && attempt == 1) {
            gpio_set_level(PIN_CS, 1);
            esp_rom_delay_us(2000);
            app_logf("  CS RE-ASSERT (0238 rung 2): CS high %d us, then CMD0\n", 2000);
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

/* ======================================================= 0239 fold (task) ==
 * Compact flow, selected by -DSD_FOLD_ARM: per boot, ONE sdmmc_card_init()
 * call is the measurement.  The sacrifice arm runs the raw pre-sacrifice on the
 * same pins and the same SPI2 peripheral first, then releases the bus so the
 * stock driver initialises it itself -- that is the closest arrangement to what
 * real firmware would do, and it keeps the driver unmodified.
 *
 * Cross-reset table: own NVS namespace `sdprobe0239`, keyed by a hash of the
 * build stamp (the 0238 lesson -- a shared namespace hands a new build the old
 * build's rows).  The accepted 800x600 build's own NVS settings are untouched.
 */
#if SD_FOLD_TASK

typedef struct __attribute__((packed)) {
    uint8_t  magic;        /* 0xA6: a slot that was actually written */
    uint8_t  arm;          /* 0 = control, 1 = pre-sacrifice */
    uint8_t  init_ok;      /* the ONE sdmmc_card_init call returned ESP_OK */
    uint8_t  sac_r1;       /* sacrifice CMD0 response (0xFF = not run) */
    uint16_t init_err;     /* esp_err_t of that call, low 16 bits */
    uint16_t cmd0_frames;  /* CMD0 frames the driver issued (2 = its retry ran) */
} fold_slot_t;

#define FOLD_SLOTS 32
static fold_slot_t   g_fold[FOLD_SLOTS];
static int           g_fold_n;
static bool          g_fold_nvs_ok;
static nvs_handle_t  g_fold_nvs;
static uint32_t      g_fold_build_id;

static void fold_load(void)
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK && err != ESP_ERR_NVS_NO_FREE_PAGES) {
        app_logf("FOLD: nvs_flash_init failed (%s) - table will not survive resets\n",
                 esp_err_to_name(err));
        return;
    }
    err = nvs_open("sdprobe0239", NVS_READWRITE, &g_fold_nvs);
    if (err != ESP_OK) {
        app_logf("FOLD: nvs_open failed (%s) - table will not survive resets\n",
                 esp_err_to_name(err));
        return;
    }
    g_fold_nvs_ok = true;

    /* The table key carries the arm as well as the build stamp: two builds of
     * this file can share a __TIME__ second, and a shared table would let one
     * arm's rows into the other's summary (peer review, 0239). */
    char stamp[64];
    snprintf(stamp, sizeof stamp, "%s %s arm=%d", __DATE__, __TIME__, SD_FOLD_ARM);
    uint32_t bid = 2166136261u;
    for (const char *c = stamp; *c; c++) { bid ^= (uint8_t)*c; bid *= 16777619u; }
    g_fold_build_id = bid;

    uint32_t stored = 0;
    bool same = (nvs_get_u32(g_fold_nvs, "bid", &stored) == ESP_OK && stored == bid);
    size_t len = sizeof(g_fold);
    err = nvs_get_blob(g_fold_nvs, "fold", g_fold, &len);
    if (err == ESP_OK && same) {
        g_fold_n = (int)(len / sizeof(fold_slot_t));
        if (g_fold_n > FOLD_SLOTS) g_fold_n = FOLD_SLOTS;
        app_logf("FOLD: resuming this build's table (%d slot(s))\n", g_fold_n);
    } else {
        g_fold_n = 0;
        nvs_set_u32(g_fold_nvs, "bid", bid);
        nvs_commit(g_fold_nvs);
        app_logf("FOLD: new build stamp, table starts empty\n");
    }
}

static void fold_append(uint8_t arm, uint8_t init_ok, uint8_t sac_r1,
                        esp_err_t init_err, uint16_t cmd0_frames)
{
    fold_slot_t s = { 0xA6, arm, init_ok, sac_r1, (uint16_t)((uint32_t)init_err & 0xFFFFu),
                      cmd0_frames };
    if (g_fold_n >= FOLD_SLOTS) {
        memmove(&g_fold[0], &g_fold[1], sizeof(fold_slot_t) * (FOLD_SLOTS - 1));
        g_fold_n = FOLD_SLOTS - 1;
    }
    g_fold[g_fold_n++] = s;
    if (g_fold_nvs_ok) {
        esp_err_t err = nvs_set_blob(g_fold_nvs, "fold", g_fold,
                                     sizeof(fold_slot_t) * (size_t)g_fold_n);
        if (err == ESP_OK) err = nvs_commit(g_fold_nvs);
        if (err != ESP_OK) app_logf("FOLD: persisting slot failed (%s)\n", esp_err_to_name(err));
    }
}

static void fold_report(void)
{
    int ok = 0, n = 0, invalid = 0;
    for (int i = 0; i < g_fold_n; i++) {
        const fold_slot_t *s = &g_fold[i];
        if (s->magic != 0xA6) continue;
        bool valid = (s->arm == (uint8_t)SD_FOLD_ARM);
        if (valid) {
            n++;
            if (s->init_ok) ok++;
        } else {
            invalid++;      /* 0xFE = the pre-sacrifice itself did not run */
        }
        app_logf("FOLD_SLOT %2d: arm=%d%s first_init=%s err=%u(0x%04x) sac_r1=0x%02x "
                 "cmd0_frames=%u\n",
                 i + 1, s->arm, valid ? "" : " INVALID", s->init_ok ? "OK" : "FAIL",
                 (unsigned)s->init_err, (unsigned)s->init_err, s->sac_r1,
                 (unsigned)s->cmd0_frames);
    }
    app_logf("FOLD_SLOTS: %d valid for arm %d, %d invalid, build_id=%08x\n",
             n, SD_FOLD_ARM, invalid, g_fold_build_id);
    app_logf("FOLD_SUMMARY: arm=%d first_init_ok=%d/%d\n", SD_FOLD_ARM, ok, n);
}

/* The raw pre-sacrifice: 74+ dummy clocks with CS high, one throwaway CMD0
 * (GO_IDLE_STATE, CRC7 0x95), response logged and discarded, CS high again.
 * Then the bus is released so the stock driver initialises SPI2 itself. */
static esp_err_t fold_presacrifice(uint8_t *r1_out)
{
    *r1_out = 0xFF;
    app_logf("\n--- 0239 PRE-SACRIFICE (raw bus, before the driver is initialised) ---\n");

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
        app_logf("  SACRIFICE: spi_bus_initialize failed (%s) -- boot NOT counted\n",
                 esp_err_to_name(err));
        return err;
    }

    gpio_set_direction(PIN_CS, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_CS, 1);

    spi_device_interface_config_t devcfg = {0};
    devcfg.clock_speed_hz = 400 * 1000;
    devcfg.mode = 0;
    devcfg.spics_io_num = -1;          /* CS driven by hand */
    devcfg.queue_size = 1;
    spi_device_handle_t dev = NULL;
    err = spi_bus_add_device(SPI2_HOST, &devcfg, &dev);
    if (err != ESP_OK) {
        app_logf("  SACRIFICE: add_device failed (%s) -- boot NOT counted\n",
                 esp_err_to_name(err));
        spi_bus_free(SPI2_HOST);
        return err;
    }

    /* 74+ clocks with CS high before the first command (SD spec).  The transfer
     * result is checked: a failed clock burst means the sacrifice did not run. */
    {
        uint8_t ff[10];
        memset(ff, 0xFF, sizeof ff);
        gpio_set_level(PIN_CS, 1);
        spi_transaction_t t = {0};
        t.length = sizeof(ff) * 8;
        t.tx_buffer = ff;
        err = spi_device_transmit(dev, &t);
    }
    if (err != ESP_OK) {
        app_logf("  SACRIFICE: dummy-clock transfer failed (%s) -- boot NOT counted\n",
                 esp_err_to_name(err));
        spi_bus_remove_device(dev);
        spi_bus_free(SPI2_HOST);
        return err;
    }

    uint8_t pl[8] = {0};
    g_cmd_tag = "SACRIFICE ";
    uint8_t r1 = sd_raw_cmd(dev, 0, 0x00000000, 0x95, 0, pl, sizeof pl);
    g_cmd_tag = "";
    if (g_raw_tx_err) {
        /* 0xFF from a failed transfer is not a sacrifice: the frame never went
         * out, so the driver's init below cannot be attributed to arm 1. */
        app_logf("  SACRIFICE: CMD0 transfer failed -- boot NOT counted\n");
        gpio_set_level(PIN_CS, 1);
        spi_bus_remove_device(dev);
        spi_bus_free(SPI2_HOST);
        return ESP_FAIL;
    }
    gpio_set_level(PIN_CS, 1);

    spi_bus_remove_device(dev);
    spi_bus_free(SPI2_HOST);
    app_logf("  SACRIFICE: R1=0x%02x (discarded); device removed, SPI2 freed before "
             "the driver initialises it\n", r1);
    *r1_out = r1;
    return ESP_OK;   /* 0xFF here means "no response", not "the sacrifice did not run" */
}

/* THE measurement: exactly one sdmmc_card_init() at the given clock.  No retry
 * is wrapped around it here -- whatever the driver does internally is the
 * driver's own business and is visible in its log (g_fold_cmd0_frames). */
static esp_err_t fold_one_init(uint32_t khz, bool *init_called)
{
    *init_called = false;
    app_logf("\n--- 0239 ONE sdmmc_card_init, stock driver, %" PRIu32 " kHz ---\n", khz);

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SPI2_HOST;
    slot.gpio_cs = PIN_CS;
    err = sdspi_host_init();
    if (err != ESP_OK) {
        app_logf("  FAIL sdspi_host_init=%s\n", esp_err_to_name(err));
        spi_bus_free(SPI2_HOST);
        return err;
    }
    sdspi_dev_handle_t dev = 0;
    err = sdspi_host_init_device(&slot, &dev);
    if (err != ESP_OK) {
        app_logf("  FAIL sdspi_host_init_device=%s\n", esp_err_to_name(err));
        sdspi_host_deinit();
        spi_bus_free(SPI2_HOST);
        return err;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = dev;
    host.max_freq_khz = khz;

    sdmmc_card_t *card = heap_caps_calloc(1, sizeof(sdmmc_card_t),
                                          MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!card) {
        sdspi_host_remove_device(dev);
        sdspi_host_deinit();
        spi_bus_free(SPI2_HOST);
        return ESP_ERR_NO_MEM;
    }

    *init_called = true;                     /* from here the result IS the call's */
    err = sdmmc_card_init(&host, card);      /* the single measured call */
    if (err == ESP_OK) {
        app_logf("  FOLD_FIRST_INIT_RAW: OK name=%.*s speed=%" PRIu32 " kHz size=%llu MB\n",
                 8, card->cid.name, card->real_freq_khz,
                 (unsigned long long)((uint64_t)card->csd.capacity * card->csd.sector_size
                                      / (1024 * 1024)));
    } else {
        app_logf("  FOLD_FIRST_INIT_RAW: FAIL %s (%d)\n", esp_err_to_name(err), (int)err);
    }

    heap_caps_free(card);
    sdspi_host_remove_device(dev);
    sdspi_host_deinit();
    spi_bus_free(SPI2_HOST);
    return err;
}

static void fold_driver_log_levels(void)
{
    esp_log_level_set("sdmmc_cmd", ESP_LOG_DEBUG);
    esp_log_level_set("sdmmc_sd", ESP_LOG_DEBUG);
    esp_log_level_set("sdmmc_common", ESP_LOG_DEBUG);
    esp_log_level_set("sdmmc_init", ESP_LOG_DEBUG);
    esp_log_level_set("sdmmc_io", ESP_LOG_DEBUG);
    esp_log_level_set("sdspi_host", ESP_LOG_VERBOSE);       /* CMDn, arg, flags */
    esp_log_level_set("sdspi_transaction", ESP_LOG_VERBOSE); /* r1 per command */
}

/* ===================== 0240: the kit card's write path ===================== */
#if SD_KIT_FLOW

#define KIT_PATH           "/sdcard/KITCARD.BIN"
#if SD_NEWCARD_POLICY
/* 0241: a distinct filename so nobody confuses the two cards' test files. */
#undef  KIT_PATH
#define KIT_PATH           "/sdcard/NEWCARD.BIN"
#define KIT_NEVER_SCRATCH_BYTES (400ull * 1024ull * 1024ull * 1024ull)
/* Host-computed value from bench/kitcard0240/expected_pattern.py; the task
 * states the same constant. */
#define KIT_EXPECT_SHA "9fdfcb84758db72e1c98ada8d10a47a7cb9945ebf3afd47d0a5fbe2914071dbd"
#endif
#define KIT_FILE_BYTES     (128u * 1024u)     /* 32 chunks of 4 KiB, > the 64 KiB floor */
#define KIT_CHUNK          4096u
#define KIT_CAP_MIN_BYTES  (900ull * 1024ull * 1024ull)
#define KIT_CAP_MAX_BYTES  (1200ull * 1024ull * 1024ull)
/* The kit card's own CID serial, as recorded by 0229 from its raw CID
 * (05 00 0c 61 73 64 66 67 22 00 00 13 a4 01 9a 00 -> PSN 0x000013a4).  This is
 * the identity half of the gate: capacity alone would admit any 1 GB SDSC card. */
#define KIT_CARD_PSN       0x000013a4u
#if SD_NEWCARD_POLICY
/* The Recalbox card's serial, from 0234's recorded raw CID (03 53 44 53 52 35
 * 31 32 86 e4 84 5e 78 01 89 a5 -> PSN 0xe4845e78).  Excluded by identity so
 * the never-scratch guarantee does not rest on its CSD alone. */
#define RECALBOX_CARD_PSN  0xe4845e78u
#endif

/* The driver's verbose log is what names an init failure, but it would also
 * log every block of a 128 KiB write and overflow the 60 kB report before the
 * result lines are appended.  Quiet it after the mount steps. */
static void kit_quiet_driver(void)
{
    esp_log_level_set("sdspi_host", ESP_LOG_WARN);
    esp_log_level_set("sdspi_transaction", ESP_LOG_WARN);
    esp_log_level_set("sdmmc_cmd", ESP_LOG_WARN);
    esp_log_level_set("sdmmc_sd", ESP_LOG_WARN);
    esp_log_level_set("sdmmc_common", ESP_LOG_WARN);
    esp_log_level_set("sdmmc_init", ESP_LOG_WARN);
    esp_log_level_set("sdmmc_io", ESP_LOG_WARN);
}

#if SD_NEWCARD_POLICY
/* ---- 0241 stage timing + heartbeat ---------------------------------------
 * f_mkfs on a 30 GB card writes thousands of FAT sectors in one blocking call
 * and the report buffer only reaches flash at a flush, so a stalled format and
 * a slow one look identical from the host.  Record the elapsed time of every
 * stage, and flush a heartbeat line from a second task while the format runs,
 * so a later report read-back says which of the two it was. */
static volatile const char *g_nc_stage = "boot";
static volatile uint32_t    g_nc_stage_ms;
static volatile bool        g_nc_hb_stop;

static uint32_t nc_up_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void nc_set_stage(const char *s)
{
    g_nc_stage = s;
    g_nc_stage_ms = nc_up_ms();
}

static void nc_heartbeat_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20000));
        if (g_nc_hb_stop) break;
        app_logf("HEARTBEAT: up=%" PRIu32 "s stage=%s elapsed=%" PRIu32 "s\n",
                 nc_up_ms() / 1000, (const char *)g_nc_stage,
                 (nc_up_ms() - (uint32_t)g_nc_stage_ms) / 1000);
        write_report_partition();
    }
    vTaskDelete(NULL);
}
#endif

/* Deterministic payload: byte i = ((i*31) + ((i>>9)*7) + ((i>>17)*131)) ^ 0xA5,
 * all in uint32 arithmetic.  The host tool recomputes the same bytes, so the
 * board's hash can be checked against an independent implementation. */
static void kit_pattern(uint8_t *b, size_t n, uint32_t off)
{
    for (size_t i = 0; i < n; i++) {
        uint32_t x = off + (uint32_t)i;
        b[i] = (uint8_t)(((x * 31u) + ((x >> 9) * 7u) + ((x >> 17) * 131u)) ^ 0xA5u);
    }
}

/* Step 1 -- the hard gate.  Read CID and CSD over the raw path (read-only, no
 * format, no write) and refuse unless the seated card is the ~1 GB kit SDSC
 * card.  Returns true only when every gate test passes. */
#if SD_NEWCARD_POLICY
/* CRC16-CCITT (poly 0x1021, init 0), the checksum the card appends to an SPI
 * data block.  Used to confirm the register bytes above were received intact. */
static uint16_t newcard_crc16(const uint8_t *d, int n)
{
    uint16_t crc = 0;
    for (int i = 0; i < n; i++) {
        crc ^= (uint16_t)((uint16_t)d[i] << 8);
        for (int k = 0; k < 8; k++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* 16-byte register + its 2-byte SPI CRC16 (18 bytes). */
static int raw_read_reg18(spi_device_handle_t dev, uint8_t cmd, uint8_t *out18)
{
    return raw_read_data(dev, cmd, 0, out18, 18);
}

/* Read one register three times and require: every read identical, the
 * register's own CRC7 valid, and -- when the card supplies one (non-0x0000) --
 * the data-block CRC16 matching.  Returns true only then. */
static bool newcard_read_reg(spi_device_handle_t dev, uint8_t cmd, const char *tag,
                             uint8_t *out16, bool *crc7_ok, bool *crc16_ok)
{
    uint8_t r[3][18];
    memset(r, 0, sizeof r);
    for (int k = 0; k < 3; k++) {
        if (raw_read_reg18(dev, cmd, r[k]) != 0) {
            app_logf("  %s_IDENTITY: read %d/3 FAILED -- identity not verified\n",
                     tag, k + 1);
            return false;
        }
    }

    bool same = memcmp(r[0], r[1], 18) == 0 && memcmp(r[0], r[2], 18) == 0;
    uint8_t calc7 = sd_crc7(r[0], 15);
    *crc7_ok = (calc7 == r[0][15]);
    uint16_t calc16 = newcard_crc16(r[0], 16);
    uint16_t got16 = (uint16_t)(((uint16_t)r[0][16] << 8) | r[0][17]);
    /* "Absent" is a property of the BYTES THE CARD SENT, never of the computed
     * value: an all-zero pair must not be matched by a coincidentally-zero
     * calculation (peer review, 2026-11-22). */
    bool crc16_present = !(r[0][16] == 0x00 && r[0][17] == 0x00);
    *crc16_ok = (calc16 == got16);

    app_logf("  %s_IDENTITY: 3x_identical=%d crc7 stored=0x%02x calc=0x%02x ok=%d | "
             "crc16 stored=0x%04x calc=0x%04x ok=%d present=%d\n",
             tag, (int)same, r[0][15], calc7, (int)*crc7_ok, got16, calc16,
             (int)*crc16_ok, (int)crc16_present);

    /* Fail closed, unconditionally: identical reads are necessary but not
     * sufficient, CRC7 is ALWAYS required, and a CRC16 the card supplied must
     * match.  (The counterfeit kit card returns 0x00 in the CRC7 byte, so it
     * can never reach an authorized state -- which is the intended outcome.) */
    if (!same) return false;
    if (!*crc7_ok) return false;
    if (crc16_present && !*crc16_ok) {
        app_logf("  %s_IDENTITY: CRC16 present and MISMATCHED -- refusing to "
                 "trust this register\n", tag);
        return false;
    }
    memcpy(out16, r[0], 16);
    return true;
}
#endif

static bool kit_raw_identity(void)
{
    app_logf("\n=== " TASK_NO " STEP 1: SEATED-CARD IDENTITY (raw SPI, read-only) ===\n");

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
        app_logf("KIT_GATE: SPI bus init failed (%s)\n", esp_err_to_name(err));
        return false;
    }

    gpio_set_direction(PIN_CS, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_CS, 1);
    gpio_set_pull_mode(PIN_MISO, GPIO_FLOATING);
    gpio_set_pull_mode(PIN_CS, GPIO_FLOATING);
    gpio_set_pull_mode(PIN_MOSI, GPIO_FLOATING);

    spi_device_interface_config_t devcfg = {0};
    devcfg.clock_speed_hz = 400 * 1000;
    devcfg.mode = 0;
    devcfg.spics_io_num = -1;
    devcfg.queue_size = 1;
    spi_device_handle_t dev = NULL;
    err = spi_bus_add_device(SPI2_HOST, &devcfg, &dev);
    if (err != ESP_OK) {
        app_logf("KIT_GATE: add_device failed (%s)\n", esp_err_to_name(err));
        spi_bus_free(SPI2_HOST);
        return false;
    }

    uint8_t pl[8] = {0};
    bool ready = false;
    for (int attempt = 1; attempt <= 5 && !ready; attempt++) {
        uint8_t ff[10];
        memset(ff, 0xFF, sizeof ff);
        gpio_set_level(PIN_CS, 1);
        spi_transaction_t t = {0};
        t.length = sizeof(ff) * 8;
        t.tx_buffer = ff;
        spi_device_transmit(dev, &t);

        uint8_t r0 = sd_raw_cmd(dev, 0, 0x00000000, 0x95, 0, pl, sizeof pl);
        vTaskDelay(pdMS_TO_TICKS(5));
        uint8_t r8 = sd_raw_cmd(dev, 8, 0x000001AA, 0x87, 4, pl, sizeof pl);
        bool v2 = (r8 == 0x01 && pl[2] == 0x01 && pl[3] == 0xAA);
        app_logf("  IDENTITY_INIT attempt %d: CMD0=0x%02x CMD8=0x%02x (%s)\n",
                 attempt, r0, r8, v2 ? "R7 echoes 0x1AA" : "no valid R7 echo");
        if (!v2) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }

        sd_raw_cmd(dev, 55, 0, 0x65, 0, pl, sizeof pl);
        uint8_t a41 = sd_raw_cmd(dev, 41, 0x40000000u, 0x77, 0, pl, sizeof pl);
        uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
        while (a41 == 0x01) {
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
            if (now - t0 > 1000) break;
            vTaskDelay(pdMS_TO_TICKS(20));
            sd_raw_cmd(dev, 55, 0, 0x65, 0, pl, sizeof pl);
            a41 = sd_raw_cmd(dev, 41, 0x40000000u, 0x77, 0, pl, sizeof pl);
        }
        app_logf("  IDENTITY_INIT: ACMD41 R1=0x%02x after %u ms\n", a41,
                 (unsigned)((uint32_t)(esp_timer_get_time() / 1000) - t0));
        if (a41 == 0x00) { ready = true; break; }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (!ready) {
        app_logf("KIT_GATE: STOP -- the card did not reach READY over the raw path. "
                 "Nothing was written.\n");
        spi_bus_remove_device(dev);
        spi_bus_free(SPI2_HOST);
        return false;
    }

    uint8_t ocr[8] = {0};
    sd_raw_cmd(dev, 58, 0, 0xFF, 4, ocr, sizeof ocr);
    bool ccs = ((ocr[0] >> 6) & 1) != 0;
    g_byte_addressed = !ccs;
    app_logf("  OCR: %02x %02x %02x %02x  OCR_CCS=%d  ADDRESSING=%s\n",
             ocr[0], ocr[1], ocr[2], ocr[3], (int)ccs,
             ccs ? "BLOCK (SDHC/SDXC)" : "BYTE (SDSC)");

    uint8_t cid[16] = {0};
    bool have_cid = (raw_read_reg(dev, 10, cid) == 0);
    if (have_cid) {
        bool blank = true;
        for (int i = 0; i < 16; i++) if (cid[i] != 0xFF) blank = false;
        have_cid = !blank;
    }
    uint8_t csd[16] = {0};
    bool have_csd = (raw_read_reg(dev, 9, csd) == 0);
    if (have_csd) {
        bool blank = true;
        for (int i = 0; i < 16; i++) if (csd[i] != 0xFF) blank = false;
        have_csd = !blank;
    }

#if SD_NEWCARD_POLICY
    /* The authorization below decides whether an ERASE may happen, so the
     * identity it reads must be VERIFIED, not merely non-blank and not merely
     * repeated (peer review, 2026-11-22: three identical reads can repeat a
     * persistent corruption).  Each register is read three times, 18 bytes at a
     * time -- the 16-byte register plus the 2-byte SPI data-block CRC16 -- and
     * must come back byte-identical every time with its own CRC7 valid; a
     * register whose CRC16 is present (non-0x0000) must match too. */
    uint8_t cid18[18] = {0}, csd18[18] = {0};
    bool cid_crc7 = false, cid_crc16 = false, csd_crc7 = false, csd_crc16 = false;
    bool cid_verified = newcard_read_reg(dev, 10, "CID", cid18, &cid_crc7, &cid_crc16);
    bool csd_verified = newcard_read_reg(dev, 9, "CSD", csd18, &csd_crc7, &csd_crc16);
    uint8_t ocr_b[8] = {0};
    bool ocr_x2 = (sd_raw_cmd(dev, 58, 0, 0xFF, 4, ocr_b, sizeof ocr_b) != 0xFF) &&
                  memcmp(ocr, ocr_b, 4) == 0;
    app_logf("  IDENTITY_VERIFY: cid_verified=%d csd_verified=%d ocr_2x_identical=%d\n",
             (int)cid_verified, (int)csd_verified, (int)ocr_x2);
    if (cid_verified) memcpy(cid, cid18, sizeof cid);
    if (csd_verified) memcpy(csd, csd18, sizeof csd);
    /* Only verified register data is authorization data. */
    have_cid = cid_verified;
    have_csd = csd_verified;
#endif

    spi_bus_remove_device(dev);
    spi_bus_free(SPI2_HOST);

    uint32_t psn = 0;
    if (have_cid) {
        /* CID layout (zero-based): MID[0] OID[1..2] PNM[3..7] PRV[8]
         * PSN[9..12] MDT[13..14] CRC[15].  Checked against 0229's recorded raw
         * CID for this card, 05 00 0c 61 73 64 66 67 22 00 00 13 a4 01 9a 00:
         * cid[9..12] = 00 00 13 a4 = 0x000013a4, the serial 0229 logged.  (An
         * interim 10..13 parse in the first fix attempt was off by one, caught
         * in the follow-up review before the bench run.) */
        char name[6] = {0};
        for (int i = 0; i < 5; i++) name[i] = (char)cid[3 + i];
        psn = ((uint32_t)cid[9]  << 24) | ((uint32_t)cid[10] << 16) |
              ((uint32_t)cid[11] << 8)  | (uint32_t)cid[12];
        app_logf("  CID: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                 cid[0], cid[1], cid[2], cid[3], cid[4], cid[5], cid[6], cid[7],
                 cid[8], cid[9], cid[10], cid[11], cid[12], cid[13], cid[14], cid[15]);
        app_logf("  CID_MID: 0x%02x  CID_NAME: %.5s  CID_PSN: 0x%08" PRIx32 "\n",
                 cid[0], name, psn);
    } else {
        app_logf("  CID: read FAILED\n");
    }

    uint64_t bytes = 0;
    unsigned structure = 0;
    if (have_csd) {
        structure = (unsigned)(csd[0] >> 6);
        bytes = csd_capacity_bytes(csd);
        app_logf("  CSD: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                 csd[0], csd[1], csd[2], csd[3], csd[4], csd[5], csd[6], csd[7],
                 csd[8], csd[9], csd[10], csd[11], csd[12], csd[13], csd[14], csd[15]);
        app_logf("  CSD_STRUCTURE: v%u  CAPACITY: %llu MB (%llu bytes)\n",
                 structure == 0 ? 1u : 2u,
                 (unsigned long long)(bytes / (1024ull * 1024ull)),
                 (unsigned long long)bytes);
    } else {
        app_logf("  CSD: read FAILED\n");
    }

    /* The gate is IDENTITY as well as geometry (peer review, 2026-11-22: a
     * capacity band alone would admit any 1 GB SDSC card and format it).
     * Pass requires: CID read, CID serial == the kit card's recorded serial,
     * CSD v1 (SDSC), byte addressing, and capacity in the 1 GB class.  The 512 GB
     * Recalbox card fails all four; an empty holder fails the READY test above. */
#if SD_NEWCARD_POLICY
    /* 0241 policy: AUTHORIZE the seated card, except the two it must never
     * touch.  Refused, no exceptions:
     *   - the 1 GB kit card (serial 0x000013a4) -- 0240 already measured it;
     *   - anything at or above 400 GiB -- the never-scratch class that the
     *     512 GB SanDisk SR512 (Recalbox) falls in, refused with margin rather
     *     than by a single capacity equality that a relabelled card could miss.
     * Anything else with a readable CID is the card Stephen seated for 0241. */
    bool is_kit        = have_cid && psn == KIT_CARD_PSN;
    /* 0234 recorded the Recalbox card's own CID:
     *   03 53 44 53 52 35 31 32 86 e4 84 5e 78 01 89 a5
     *   MID 0x03 (SanDisk), OID "SD", PNM "SR512", PSN 0xe4845e78
     * Capacity is 511,868,665,856 B = 476.7 GiB.  It is excluded by IDENTITY
     * as well as by size, so a misread C_SIZE cannot let it through: this is
     * the check that does not depend on the CSD at all. */
    bool is_recalbox_name = have_cid && cid[0] == 0x03 && memcmp(cid + 3, "SR512", 5) == 0;
    bool is_recalbox_psn  = have_cid && psn == RECALBOX_CARD_PSN;
    bool never_scratch    = have_csd && bytes >= KIT_NEVER_SCRATCH_BYTES;
    /* structure 0 = CSD v1 (SDSC), 1 = CSD v2 (SDHC/SDXC); 2 and 3 are
     * reserved and are refused rather than treated as v2 (peer review). */
    bool struct_v1  = have_csd && structure == 0;
    bool struct_v2  = have_csd && structure == 1;
    bool class_ok   = (struct_v1 || struct_v2) && ocr_x2 && (ccs == struct_v2);
    bool size_plaus = struct_v1 ? (bytes <= (4ull << 30))
                                : (struct_v2 && bytes >= (2ull << 30) &&
                                   bytes <= (2ull << 40));
    bool verified   = cid_verified && csd_verified && ocr_x2;
    bool gate       = verified && class_ok && size_plaus && !is_kit &&
                      !is_recalbox_name && !is_recalbox_psn && !never_scratch;

    app_logf("NEWCARD_GATE: %s\n", gate
             ? "AUTHORIZED -- not the kit card and not the 512 GB Recalbox card"
             : "STOP -- nothing will be formatted or written");
    app_logf("NEWCARD_GATE_TESTS: identity_verified=%d ocr_csd_class_consistent=%d "
             "size_plausible=%d is_kit_0x%08" PRIx32 "=%d is_recalbox_SR512=%d "
             "is_recalbox_psn_0x%08" PRIx32 "=%d capacity=%llu MB "
             "never_scratch_ge_%lluGiB=%d csd_structure=%u\n",
             (int)verified, (int)class_ok, (int)size_plaus,
             (uint32_t)KIT_CARD_PSN, (int)is_kit, (int)is_recalbox_name,
             RECALBOX_CARD_PSN, (int)is_recalbox_psn,
             (unsigned long long)(bytes / (1024ull * 1024ull)),
             (unsigned long long)(KIT_NEVER_SCRATCH_BYTES / (1ull << 30)),
             (int)never_scratch, structure);
    if (is_kit) {
        app_logf("NEWCARD_GATE: the seated card is the 1 GB KIT card -- 0240 "
                 "already measured it; no format, no write.\n");
    }
    if (is_recalbox_name || is_recalbox_psn || never_scratch) {
        app_logf("NEWCARD_GATE: the seated card is the NEVER-SCRATCH card (the "
                 "512 GB SanDisk SR512 / Recalbox, 476.7 GiB); no format, no "
                 "write, refused absolutely.\n");
    }
    if (!verified) {
        app_logf("NEWCARD_GATE: card identity could not be verified (three "
                 "identical reads of CID and CSD with a valid CRC7 and no CRC16 "
                 "mismatch, plus two identical OCR reads, are required); failing "
                 "closed with no format, no write.\n");
    }
    return gate;
#else
    bool cid_ok    = have_cid && psn == KIT_CARD_PSN;
    bool is_sdsc   = have_csd && structure == 0;
    bool byte_addr = !ccs;
    bool cap_ok    = have_csd && bytes >= KIT_CAP_MIN_BYTES && bytes <= KIT_CAP_MAX_BYTES;
    bool gate      = cid_ok && is_sdsc && byte_addr && cap_ok;

    app_logf("KIT_GATE: %s (cid_serial_matches_0x%08" PRIx32 "=%d sdsc=%d "
             "byte_addressed=%d capacity_in_900MB..1200MB=%d)\n",
             gate ? "PASS -- seated card is the kit card (identity + geometry)"
                  : "FAIL -- STOP, nothing will be formatted or written",
             (uint32_t)KIT_CARD_PSN, (int)cid_ok, (int)is_sdsc, (int)byte_addr,
             (int)cap_ok);
    return gate;
#endif
}

/* Mount the card through the stock driver + FatFS.  Returns a live mount on
 * success; on failure the bus is released so the caller can retry. */
static bool kit_mount(const char *tag, bool format_if_failed, sdmmc_card_t **out)
{
    app_logf("\n--- KIT MOUNT [%s] ---\n", tag);

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
        app_logf("  MOUNT_FAIL spi_bus_initialize=%s\n", esp_err_to_name(err));
        return false;
    }

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SPI2_HOST;
    slot.gpio_cs = PIN_CS;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = 400;

    esp_vfs_fat_mount_config_t mcfg = {
        .format_if_mount_failed = format_if_failed,
        .max_files = 4,
#if SD_NEWCARD_POLICY
        /* 0241: f_mkfs clears the ENTIRE FAT area, so the FAT size sets the
         * format's cost.  At 4 KiB clusters a 32 GB card needs ~62 MB of FAT
         * (~122k sectors); at the 64 KiB maximum (512 B x 128 sectors, the
         * IDF ceiling) it needs ~3.8 MB (~7.6k sectors).  Measured on this
         * bench at 400 kHz: 0.41 s/sector with the driver's VERBOSE log on,
         * so this is the difference between ~14 h and ~10 min.  Cluster size
         * is the only knob touched -- clock, CS handling, pull-ups and the
         * zero delay are unchanged. */
        .allocation_unit_size = 64 * 1024,
#else
        .allocation_unit_size = 0,
#endif
    };
    sdmmc_card_t *card = NULL;
    err = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mcfg, &card);
    if (err != ESP_OK) {
        app_logf("  MOUNT_FAIL[%s]: %s (%d)\n", tag, esp_err_to_name(err), (int)err);
        spi_bus_free(SPI2_HOST);
        return false;
    }

    app_logf("  MOUNT_OK[%s]: card=%s speed=%" PRIu32 " kHz size=%llu MB "
             "format_if_mount_failed=%d\n",
             tag, card->cid.name, card->real_freq_khz,
             (unsigned long long)((uint64_t)card->csd.capacity * card->csd.sector_size
                                  / (1024 * 1024)),
             (int)format_if_failed);
    *out = card;
    return true;
}

/* Returns the unmount result: the cross-remount half of the proof only means
 * something if the volume really was unmounted (peer review, 2026-11-22). */
static esp_err_t kit_unmount(sdmmc_card_t *card)
{
    esp_err_t err = esp_vfs_fat_sdcard_unmount("/sdcard", card);
    app_logf("  UNMOUNT: %s\n", err == ESP_OK ? "ok" : esp_err_to_name(err));
    spi_bus_free(SPI2_HOST);
    return err;
}

static void kit_sha_hex(const uint8_t *d, char *out /* 65 */)
{
    for (int i = 0; i < 32; i++) snprintf(out + i * 2, 3, "%02x", d[i]);
    out[64] = 0;
}

/* One pass over the file: write it (mode "wb") or read it (mode "rb"),
 * hashing what crosses the API.  Returns 0 on success, -1 on the first error. */
#if SD_NEWCARD_POLICY
/* Byte-exact read-back accounting, filled in by kit_pass on read passes. */
static uint32_t s_cmp_ok, s_cmp_bad, s_cmp_first;

/* ---------------------------------------------------------------------------
 * 0241: self-contained SHA-256.
 *
 * mbedtls' digest came back with its first 8 bytes zeroed whenever it was
 * computed in this function's card-I/O path -- reproducibly, on all three
 * passes, with the remaining 24 bytes exactly right, and with both in-firmware
 * known-answer tests (3-byte and 128 KiB chunked) passing in the same boot.
 * Since the digest is the deliverable's evidence, it is computed here instead,
 * and checked against the same two known answers at boot before it is trusted.
 * ------------------------------------------------------------------------- */
typedef struct {
    uint32_t h[8];
    uint64_t total;
    uint8_t  blk[64];
    size_t   fill;
} sw_sha256_t;

static const uint32_t S256_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static uint32_t s256_rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void s256_block(sw_sha256_t *s, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = s256_rotr(w[i - 15], 7) ^ s256_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = s256_rotr(w[i - 2], 17) ^ s256_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
    uint32_t e = s->h[4], f = s->h[5], g = s->h[6], hh = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = s256_rotr(e, 6) ^ s256_rotr(e, 11) ^ s256_rotr(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = hh + S1 + ch + S256_K[i] + w[i];
        uint32_t S0 = s256_rotr(a, 2) ^ s256_rotr(a, 13) ^ s256_rotr(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += hh;
}

static void s256_init(sw_sha256_t *s)
{
    s->h[0] = 0x6a09e667; s->h[1] = 0xbb67ae85; s->h[2] = 0x3c6ef372; s->h[3] = 0xa54ff53a;
    s->h[4] = 0x510e527f; s->h[5] = 0x9b05688c; s->h[6] = 0x1f83d9ab; s->h[7] = 0x5be0cd19;
    s->total = 0; s->fill = 0;
}

static void s256_update(sw_sha256_t *s, const uint8_t *p, size_t n)
{
    s->total += n;
    while (n--) {
        s->blk[s->fill++] = *p++;
        if (s->fill == 64) { s256_block(s, s->blk); s->fill = 0; }
    }
}

static void s256_final(sw_sha256_t *s, uint8_t out[32])
{
    uint64_t bits = s->total * 8ull;
    uint8_t pad = 0x80;
    s256_update(s, &pad, 1);
    uint8_t z = 0;
    while (s->fill != 56) s256_update(s, &z, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    s256_update(s, len, 8);
    for (int i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(s->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(s->h[i]);
    }
}
#endif
static int kit_pass(const char *tag, bool write, uint8_t *buf, char *hex_out)
{
#if SD_NEWCARD_POLICY
    if (!write) { s_cmp_ok = 0; s_cmp_bad = 0; s_cmp_first = 0xFFFFFFFFu; }
#endif
    FILE *f = fopen(KIT_PATH, write ? "wb" : "rb");
    if (!f) {
        app_logf("  %s_OPEN: FAILED (errno %d)\n", tag, errno);
        return -1;
    }

#if SD_NEWCARD_POLICY
    sw_sha256_t ctx;
    s256_init(&ctx);
#else
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
#endif

    uint32_t done = 0;
    int rc = 0;
    while (done < KIT_FILE_BYTES) {
        uint32_t want = KIT_FILE_BYTES - done;
        if (want > KIT_CHUNK) want = KIT_CHUNK;
        size_t got;
        if (write) {
            kit_pattern(buf, want, done);
            got = fwrite(buf, 1, want, f);
        } else {
            got = fread(buf, 1, want, f);
        }
        if (got != want) {
            app_logf("  %s_IO: %s at offset %" PRIu32 " (%u of %u bytes, errno %d)\n",
                     tag, write ? "short write" : "short read", done,
                     (unsigned)got, (unsigned)want, errno);
            rc = -1;
            break;
        }
#if SD_NEWCARD_POLICY
        /* Independent of SHA-256 (whose digest came back damaged in the first
         * runs): a byte-exact comparison of the read-back against the same
         * deterministic pattern the write pass generated. */
        if (!write && rc == 0) {
            static uint8_t cmp[KIT_CHUNK];
            kit_pattern(cmp, want, done);
            for (uint32_t k = 0; k < want; k++) {
                if (buf[k] != cmp[k]) {
                    if (s_cmp_bad == 0) s_cmp_first = done + k;
                    s_cmp_bad++;
                } else {
                    s_cmp_ok++;
                }
            }
        }
#endif
#if SD_NEWCARD_POLICY
        s256_update(&ctx, buf, want);
#else
        mbedtls_sha256_update(&ctx, buf, want);
#endif
        done += want;
    }

    if (write && rc == 0) {
        if (fflush(f) != 0) { app_logf("  %s_FLUSH: FAILED (errno %d)\n", tag, errno); rc = -1; }
        if (fsync(fileno(f)) != 0) { app_logf("  %s_FSYNC: FAILED (errno %d)\n", tag, errno); rc = -1; }
    }
    if (fclose(f) != 0) { app_logf("  %s_CLOSE: FAILED (errno %d)\n", tag, errno); rc = -1; }

    uint8_t digest[32];
#if SD_NEWCARD_POLICY
    s256_final(&ctx, digest);
#else
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);
#endif
    kit_sha_hex(digest, hex_out);
#if SD_NEWCARD_POLICY
    /* The first full run printed 16 leading zeros in all three hashes while the
     * trailing 48 characters matched the host value exactly, so the report must
     * carry the digest as words as well as as a string: that distinguishes a
     * compute fault from a formatting one without another guess. */
    {
        uint32_t w[8];
        memcpy(w, digest, sizeof w);
        app_logf("  %s_DIGEST_WORDS: %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " "
                 "%08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 " %08" PRIx32 "\n",
                 tag, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
        app_logf("  %s_VS_EXPECTED: %s\n", tag,
                 strcmp(hex_out, KIT_EXPECT_SHA) == 0 ? "MATCH" : "MISMATCH");
        if (!write) {
            app_logf("  %s_BYTES: %" PRIu32 " of %u matched, %" PRIu32 " mismatched, "
                     "first mismatch offset %d (-1 = none)\n", tag, s_cmp_ok,
                     (unsigned)KIT_FILE_BYTES, s_cmp_bad,
                     s_cmp_bad ? (int)s_cmp_first : -1);
        }
    }
#endif

    struct stat st;
    if (stat(KIT_PATH, &st) == 0) {
        app_logf("  %s_RESULT: %s %" PRIu32 " bytes, file size %ld, sha256 %s\n",
                 tag, rc == 0 ? "ok" : "FAILED", done, (long)st.st_size, hex_out);
        if (st.st_size != (long)KIT_FILE_BYTES) {
            app_logf("  %s_SIZE: FAILED (expected %u)\n", tag, (unsigned)KIT_FILE_BYTES);
            rc = -1;
        }
    } else {
        app_logf("  %s_RESULT: %s %" PRIu32 " bytes, stat failed (errno %d), sha256 %s\n",
                 tag, rc == 0 ? "ok" : "FAILED", done, errno, hex_out);
        rc = -1;
    }
    return rc;
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(300));

    esp_log_set_vprintf(report_vprintf);
    fold_driver_log_levels();

    app_logf("\n\n########## " TASK_NO ": CARD FORMAT / WRITE / READ BACK ##########\n");
    app_logf("build: %s %s\n", __DATE__, __TIME__);
    app_logf("KIT_FILE: %s, %u bytes, chunk %u bytes, clock 400 kHz\n",
             KIT_PATH, (unsigned)KIT_FILE_BYTES, (unsigned)KIT_CHUNK);
#if SD_NEWCARD_POLICY
    app_logf("NEWCARD_POLICY: format authorized for any seated card with a readable "
             "CID except the 1 GB kit card (serial 0x%08" PRIx32 ") and any card "
             ">= %llu GiB (the 512 GB Recalbox card is never scratch)\n",
             (uint32_t)KIT_CARD_PSN,
             (unsigned long long)(KIT_NEVER_SCRATCH_BYTES / (1024ull * 1024ull * 1024ull)));
#endif

    report_identity();
#if SD_NEWCARD_POLICY
    /* Known-answer self-test of the hash path BEFORE any card work.  The first
     * run rendered the first 8 digest bytes as zeros with the remaining 24
     * bytes exactly right, so the report must show whether the digest or its
     * hex formatting is at fault, in the same build that quotes the hashes. */
    {
        static const char kexp[] =
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
        uint8_t kd[32];
        mbedtls_sha256_context k;
        mbedtls_sha256_init(&k);
        mbedtls_sha256_starts(&k, 0);
        mbedtls_sha256_update(&k, (const uint8_t *)"abc", 3);
        mbedtls_sha256_finish(&k, kd);
        mbedtls_sha256_free(&k);
        char khex[65];
        kit_sha_hex(kd, khex);
        app_logf("SHA_KAT: sha256(\"abc\") = %s\nSHA_KAT_EXPECT:              %s\n"
                 "SHA_KAT: %s\n", khex, kexp,
                 strcmp(khex, kexp) == 0 ? "MATCH -- hash path is sound"
                                         : "MISMATCH -- hash path is NOT sound");

        /* Second KAT: the exact 128 KiB / 4 KiB-chunk path kit_pass uses, so a
         * length- or buffer-dependent defect cannot hide behind the short one. */
        uint8_t *kb = heap_caps_malloc(KIT_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!kb) kb = heap_caps_malloc(KIT_CHUNK, MALLOC_CAP_8BIT);
        if (kb) {
            mbedtls_sha256_context k2;
            mbedtls_sha256_init(&k2);
            mbedtls_sha256_starts(&k2, 0);
            for (uint32_t off = 0; off < KIT_FILE_BYTES; off += KIT_CHUNK) {
                uint32_t want = KIT_FILE_BYTES - off;
                if (want > KIT_CHUNK) want = KIT_CHUNK;
                kit_pattern(kb, want, off);
                mbedtls_sha256_update(&k2, kb, want);
            }
            uint8_t d2[32];
            mbedtls_sha256_finish(&k2, d2);
            mbedtls_sha256_free(&k2);
            char h2[65];
            kit_sha_hex(d2, h2);
            app_logf("SHA_KAT2: pattern(128 KiB, 4 KiB chunks) = %s\n"
                     "SHA_KAT2_EXPECT:                            "
                     "9fdfcb84758db72e1c98ada8d10a47a7cb9945ebf3afd47d0a5fbe2914071dbd\n"
                     "SHA_KAT2: %s\n", h2,
                     strcmp(h2, "9fdfcb84758db72e1c98ada8d10a47a7cb9945ebf3afd47d0a5fbe2914071dbd") == 0
                         ? "MATCH -- chunked path is sound" : "MISMATCH");
            heap_caps_free(kb);
        } else {
            app_logf("SHA_KAT2: skipped (no buffer)\n");
        }

        /* The software SHA-256 that the file passes actually use: same two
         * known answers, so it is verified in this boot before it is trusted. */
        {
            sw_sha256_t s1;
            uint8_t d3[32];
            char h3[65];
            s256_init(&s1);
            s256_update(&s1, (const uint8_t *)"abc", 3);
            s256_final(&s1, d3);
            kit_sha_hex(d3, h3);
            app_logf("SW_SHA_KAT:  sha256(\"abc\") = %s -> %s\n", h3,
                     strcmp(h3, kexp) == 0 ? "MATCH" : "MISMATCH");

            sw_sha256_t s2;
            uint8_t *kb2 = heap_caps_malloc(KIT_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (kb2) {
                s256_init(&s2);
                for (uint32_t off = 0; off < KIT_FILE_BYTES; off += KIT_CHUNK) {
                    uint32_t want = KIT_FILE_BYTES - off;
                    if (want > KIT_CHUNK) want = KIT_CHUNK;
                    kit_pattern(kb2, want, off);
                    s256_update(&s2, kb2, want);
                }
                uint8_t d4[32];
                char h4[65];
                s256_final(&s2, d4);
                kit_sha_hex(d4, h4);
                app_logf("SW_SHA_KAT2: pattern(128 KiB) = %s -> %s\n", h4,
                         strcmp(h4, KIT_EXPECT_SHA) == 0 ? "MATCH" : "MISMATCH");
                heap_caps_free(kb2);
            }
        }
    }
#endif
    write_report_partition();

    vTaskDelay(pdMS_TO_TICKS(250));

    /* ---- step 1: the identity gate.  Nothing is written unless it passes. */
    bool gate = kit_raw_identity();
    write_report_partition();
    if (!gate) {
        app_logf("\nKIT_STOP: identity gate failed -- no format, no write. "
#if SD_NEWCARD_POLICY
                 "This task authorizes a card that is neither the 1 GB kit card nor "
                 "the 512 GB Recalbox card; the seated card is one of those (or could "
                 "not be identified).\n");
#else
                 "Wrong card or empty holder; this task authorizes the kit card only.\n");
#endif
        write_report_partition();
        for (;;) vTaskDelay(pdMS_TO_TICKS(5000));
    }

    /* ---- step 2: the 0239 pre-sacrifice, then ONE stock-driver init. */
    uint8_t sac_r1 = 0xFF;
    esp_err_t serr = fold_presacrifice(&sac_r1);
    app_logf("KIT_PRESACRIFICE: %s r1=0x%02x\n",
             serr == ESP_OK ? "ran" : "FAILED", sac_r1);

    bool init_called = false;
    esp_err_t ierr = fold_one_init(400, &init_called);
    app_logf("KIT_DRIVER_INIT: called=%d result=%s (%d)\n",
             (int)init_called, ierr == ESP_OK ? "OK" : esp_err_to_name(ierr), (int)ierr);
    write_report_partition();

    /* ---- step 3: mount -- one attempt, one retry, then a flag-carrying mount
     * only to obtain a card handle.  The erase itself is ALWAYS the explicit
     * esp_vfs_fat_sdcard_format() below: format_if_mount_failed formats only
     * when that mount attempt's own plain mount failed first, so it cannot be
     * reported as the format that happened (peer review, 2026-11-22). */
    sdmmc_card_t *card = NULL;
    bool mounted = kit_mount("attempt 1, format_if_mount_failed=false", false, &card);
    bool format_flag_needed = false;
    if (!mounted) {
        mounted = kit_mount("attempt 2 (retry), format_if_mount_failed=false", false, &card);
    }
    if (!mounted) {
        mounted = kit_mount("attempt 3, format_if_mount_failed=true", true, &card);
        format_flag_needed = mounted;
    }
    write_report_partition();

    if (!mounted) {
        app_logf("\nKIT_STOP: the stock driver could not mount this card at 400 kHz "
                 "(see the driver's own log lines above). No format, no write.\n");
        write_report_partition();
        for (;;) vTaskDelay(pdMS_TO_TICKS(5000));
    }

#if SD_NEWCARD_POLICY
    /* Quiet the per-transaction log BEFORE the format: at 115200 baud the
     * driver's VERBOSE lines cost ~0.41 s per sector and f_mkfs writes
     * thousands of them.  The stage results below are appended to the report
     * either way. */
    kit_quiet_driver();

    /* Raw sector throughput on THIS card, THIS boot.  The format's cost is
     * thousands of such sectors, so this is the measurement that says whether
     * the format can finish in one session at 400 kHz.  LBA 0x2000 lies inside
     * the volume that is about to be reformatted, so the write is not a loss. */
    {
        uint8_t *sb = heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (sb) {
            uint32_t ok = 0, t0 = nc_up_ms();
            for (int i = 0; i < 8; i++) {
                memset(sb, (int)(0x5a ^ i), 4096);
                if (sdmmc_write_sectors(card, sb, 0x2000 + (size_t)i * 8, 8) == ESP_OK) ok++;
            }
            uint32_t t1 = nc_up_ms();
            uint32_t rok = 0, t2 = nc_up_ms();
            for (int i = 0; i < 2; i++) {
                if (sdmmc_read_sectors(card, sb, 0x2000 + (size_t)i * 8, 8) == ESP_OK) rok++;
            }
            uint32_t t3 = nc_up_ms();
            app_logf("KIT_RAW_RATE: write %u/8 x 4096 B in %" PRIu32 " ms = %" PRIu32 " B/s; "
                     "read %u/2 x 4096 B in %" PRIu32 " ms = %" PRIu32 " B/s\n",
                     (unsigned)ok, t1 - t0,
                     (t1 > t0) ? (ok * 4096u * 1000u / (t1 - t0)) : 0u,
                     (unsigned)rok, t3 - t2,
                     (t3 > t2) ? (rok * 4096u * 1000u / (t3 - t2)) : 0u);
            heap_caps_free(sb);
        } else {
            app_logf("KIT_RAW_RATE: no 4 KiB internal buffer available\n");
        }
        write_report_partition();
    }

    app_logf("KIT_FORMAT_START: f_mkfs clearing the FAT area (allocation unit 64 KiB); "
             "this is the long step on this bench\n");
    write_report_partition();
    nc_set_stage("FORMAT");
    xTaskCreate(nc_heartbeat_task, "nc_hb", 3072, NULL, 3, NULL);
    uint32_t fmt_t0 = nc_up_ms();
#endif
    esp_err_t ferr = esp_vfs_fat_sdcard_format("/sdcard", card);
#if SD_NEWCARD_POLICY
    uint32_t fmt_ms = nc_up_ms() - fmt_t0;
    g_nc_hb_stop = true;
    nc_set_stage("FORMAT_DONE");
    app_logf("KIT_FORMAT_ELAPSED: %" PRIu32 " ms\n", fmt_ms);
#endif
    app_logf("KIT_FORMAT_METHOD: esp_vfs_fat_sdcard_format (FatFS f_mkfs, FAT32) -> %s "
             "(mount handle obtained with format_if_mount_failed=%d)\n",
             ferr == ESP_OK ? "OK" : esp_err_to_name(ferr), (int)format_flag_needed);
    if (ferr != ESP_OK) {
        app_logf("\nKIT_STOP: format failed; no write attempted.\n");
        write_report_partition();
        for (;;) vTaskDelay(pdMS_TO_TICKS(5000));
    }

    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info("/sdcard", &total, &freeb) == ESP_OK) {
        app_logf("KIT_VOLUME: total=%llu B free=%llu B\n",
                 (unsigned long long)total, (unsigned long long)freeb);
    } else {
        app_logf("KIT_VOLUME: esp_vfs_fat_info failed\n");
    }
    write_report_partition();

    /* ---- step 4: write, read back in place, unmount, remount, read again. */
    kit_quiet_driver();
    uint8_t *buf = heap_caps_malloc(KIT_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#if SD_NEWCARD_POLICY
    /* 0241: prefer INTERNAL RAM for the hash buffer.  With a PSRAM buffer the
     * SHA-256 digest came back with its first 8 bytes zeroed on all three
     * passes (digest words 0x00000000 0x00000000 a8ad981c ...), while the same
     * chunked hash over a PSRAM buffer computed before any SPI DMA ran was
     * correct -- i.e. the digest, not the data, was damaged.  4 KiB of internal
     * RAM removes the PSRAM/DMA interaction from the measurement. */
    if (buf) { heap_caps_free(buf); buf = NULL; }
    buf = heap_caps_malloc(KIT_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
    if (!buf) buf = heap_caps_malloc(KIT_CHUNK, MALLOC_CAP_8BIT);
    if (!buf) {
        app_logf("KIT_STOP: no %u-byte buffer available\n", (unsigned)KIT_CHUNK);
        write_report_partition();
        for (;;) vTaskDelay(pdMS_TO_TICKS(5000));
    }

    char sha_w[65] = {0}, sha_r[65] = {0}, sha_m[65] = {0};
    nc_set_stage("WRITE");
    uint32_t pass_t0 = nc_up_ms();
    int wrc = kit_pass("KIT_WRITE", true, buf, sha_w);
    app_logf("KIT_WRITE_ELAPSED: %" PRIu32 " ms\n", nc_up_ms() - pass_t0);
    write_report_partition();

    nc_set_stage("READBACK");
    pass_t0 = nc_up_ms();
    int rrc = kit_pass("KIT_READBACK", false, buf, sha_r);
    app_logf("KIT_READBACK_ELAPSED: %" PRIu32 " ms\n", nc_up_ms() - pass_t0);
    write_report_partition();

    esp_err_t urc = kit_unmount(card);
    vTaskDelay(pdMS_TO_TICKS(500));

    sdmmc_card_t *card2 = NULL;
    bool remounted = false;
    int mrc = -1;
    if (urc != ESP_OK) {
        /* Without a successful unmount the "survived a remount" claim would be
         * vacuous: the volume may still have been mounted throughout. */
        app_logf("  KIT_REMOUNT_READ: skipped -- the unmount failed (%s), so a remount "
                 "would not prove anything\n", esp_err_to_name(urc));
    } else {
        remounted = kit_mount("remount after unmount, format_if_mount_failed=false",
                              false, &card2);
        if (remounted) {
            mrc = kit_pass("KIT_REMOUNT_READ", false, buf, sha_m);
            esp_err_t urc2 = kit_unmount(card2);
            if (urc2 != ESP_OK) {
                app_logf("  KIT_REMOUNT_READ: second unmount failed (%s)\n",
                         esp_err_to_name(urc2));
            }
        } else {
            app_logf("  KIT_REMOUNT_READ: mount failed -- cross-remount read not measured\n");
        }
    }
    heap_caps_free(buf);
    write_report_partition();

    app_logf("\n=== " TASK_NO " RESULT ===\n");
    app_logf("KIT_HASH_WRITTEN:   %s\n", sha_w);
    app_logf("KIT_HASH_READBACK:  %s\n", sha_r);
    app_logf("KIT_HASH_REMOUNT:   %s\n", sha_m);
    app_logf("KIT_CHAIN: write=%s readback=%s unmount=%s remount_read=%s\n",
             wrc == 0 ? "ok" : "FAIL", rrc == 0 ? "ok" : "FAIL",
             urc == ESP_OK ? "ok" : "FAIL", mrc == 0 ? "ok" : "FAIL");
    bool all = (wrc == 0 && rrc == 0 && mrc == 0 && urc == ESP_OK &&
                strcmp(sha_w, sha_r) == 0 && strcmp(sha_w, sha_m) == 0);
    app_logf("KIT_VERDICT: %s\n",
             all ? "PASS -- format/mount/write/read-back complete, all three hashes match"
                 : "FAIL -- see the stage lines above");
    app_logf("\n########## " TASK_NO " COMPLETE ##########\n");
    write_report_partition();

    for (;;) vTaskDelay(pdMS_TO_TICKS(5000));
}

#else   /* the 0239 fold app_main, unchanged */
void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(300));

    esp_log_set_vprintf(report_vprintf);
    fold_driver_log_levels();

    app_logf("\n\n########## 0239 FOLD: pre-sacrifice before the stock driver init ##########\n");
    app_logf("build: %s %s\n", __DATE__, __TIME__);
    app_logf("FOLD_ARM: %d (%s), ONE sdmmc_card_init call per boot\n", SD_FOLD_ARM,
             SD_FOLD_ARM == 1 ? "pre-sacrifice then init" : "control, init only");
    app_logf("FOLD_CLOCK: 400 kHz; SD_FIRSTPASS_DELAY_MS=%d (unused in this flow)\n",
             SD_FIRSTPASS_DELAY_MS);

    fold_load();
    report_identity();
    write_report_partition();

    /* Same power-up settle the base probe uses before its first command. */
    vTaskDelay(pdMS_TO_TICKS(250));

    uint8_t sac_r1 = 0xFF;
    bool sac_ran = true;
    if (SD_FOLD_ARM == 1) {
        esp_err_t serr = fold_presacrifice(&sac_r1);
        if (serr != ESP_OK) {
            sac_ran = false;    /* the procedure did not complete: not a trial */
            app_logf("  FOLD_SACRIFICE_FAILED: %s -- this boot is recorded INVALID "
                     "and excluded from the arm's tally\n", esp_err_to_name(serr));
        }
    }

    g_fold_cmd0_frames = 0;
    bool init_called = false;
    esp_err_t err = fold_one_init(400, &init_called);
    if (!init_called) {
        /* The driver was never reached (bus/host setup or allocation failed).
         * That is not a measurement of sdmmc_card_init and is not a trial. */
        sac_ran = false;
        app_logf("  FOLD_INIT_NOT_CALLED: %s -- boot recorded INVALID\n",
                 esp_err_to_name(err));
    }

    app_logf("\nFOLD_FIRST_INIT: arm=%d%s ok=%s err=%s (%d) driver_cmd0_frames=%u\n",
             SD_FOLD_ARM, sac_ran ? "" : " (INVALID boot)",
             err == ESP_OK ? "OK" : "FAIL", esp_err_to_name(err), (int)err,
             (unsigned)g_fold_cmd0_frames);
    fold_append(sac_ran ? (uint8_t)SD_FOLD_ARM : 0xFE, err == ESP_OK ? 1 : 0, sac_r1, err,
                g_fold_cmd0_frames);
    write_report_partition();   /* the measurement is durable before the mount */

    if (SD_FOLD_ARM == 1 && sac_ran) {
        /* End-to-end, read-only, AFTER the first-call result is recorded and
         * flushed: format_if_mount_failed = false, listing only.  Running it on
         * every sacrifice boot makes the last boot of the arm the task's
         * "final sacrifice-arm boot" without a second firmware. */
        bool mounted = try_spi_mount("fold e2e read-only", 400);
        app_logf("FOLD_E2E_MOUNT: %s\n",
                 mounted ? "mounted and listed the root directory" : "mount FAILED");
    }

    fold_report();
    app_logf("\n########## FOLD COMPLETE ##########\n");
    fflush(stdout);
    write_report_partition();

    for (;;) vTaskDelay(pdMS_TO_TICKS(5000));
}

#endif  /* SD_KIT_FLOW */

#else   /* the 0229/0234/0236/0238 probe, unchanged */

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
    app_logf("FIRSTPASS_FRAMING: variant=%d (0238; %s)\n", SD_FRAMING_VARIANT,
             SD_FRAMING_VARIANT == 0 ? "control"
               : SD_FRAMING_VARIANT == 1 ? "sacrificial frame before the first real attempt"
                                         : "CS high >= 1 ms re-assert before the first CMD0");

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

#endif  /* SD_FOLD_TASK */
