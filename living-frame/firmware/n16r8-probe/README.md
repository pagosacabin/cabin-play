# n16r8-probe — task 0225

Probe firmware for the ONE ESP32-S3 board Stephen wired on 2026-11-20 (the
living frame's next rung). It answers four questions and nothing else:

1. **What silicon is here** — chip, revision, flash size, PSRAM size.
2. **Is the claimed 8 MB of PSRAM real** — pattern written and read back across
   the whole of it, twice (pattern and its inverse).
3. **Do the silkscreen labels tell the truth** — each of the 12 wired GPIOs is
   driven high and low and read back.
4. **What is on the inserted SD card** — raw sector reads only. No mount, no
   format, no writes. The card's contents are Stephen's.

It deliberately does **not** start the 64-colour port: 0225 is the gate for that
rung, not the rung itself. Nothing here drives the display.

## Wiring this probe assumes (agreed map, as wired on 2026-11-20)

VGA DAC: Red GPIO 4,5 · Green GPIO 6,7 · Blue GPIO 15,16 · HSYNC 17 · VSYNC 18
SD (SPI): SCK 12 · MOSI 11 · MISO 13 · CS 10 · GPIO 14 spare (card-detect)
**Never** GPIO 35/36/37 on an N16R8: octal PSRAM bus. On this part 33/34 are in
the same group, and 26–32 are the flash bus. The probe flags any of those
appearing in the map.

## Build, flash, read the result

```bash
pio run -e n16r8                 # build
pio run -e n16r8 -t erase        # only when the partition table changed
pio run -e n16r8 -t upload       # /dev/ttyUSB0, the FT232R bridge
esptool --port /dev/ttyUSB0 --baud 460800 read_flash 0x310000 0x10000 report.bin
python3 -c "import struct;d=open('report.bin','rb').read();m,n=struct.unpack('<II',d[:8]);print(d[8:8+n].decode())"
```

### Why the report is read out of flash, not off the console

The FT232R bridge on this bench does not deliver its console. With the app
running, `read()` returns a fixed three-byte cycle (`00 80 78`) at USB bulk
speed — the same bytes at 9600, 115200 and 460800 baud — while `esptool` still
talks to the chip normally in both directions. So the probe **also** appends
every log line to RAM and writes it to the `report` partition
(`partitions.csv`, data subtype `0x40`, offset `0x310000`), and the read-back
above is the authoritative log. Fixing the bridge's RX path is a bench task,
not a firmware one.

## Result of the 2026-11-20 run (MAC c4:9e:7e:08:6d:f4)

| Item | Measured |
| --- | --- |
| Chip | ESP32-S3, 2 cores, rev v0.2, IDF 5.5.0 |
| Flash | **16 MB** — JEDEC `68 4018`, `esp_flash_get_physical_size()` = 16,777,216, and a 4 KB pattern written at `0x00E00000` (14 MB) read back identical (`flash-above-8mb.log.txt`) |
| Flash, build view | `esp_flash_get_size()` reports **8 MB** — a build/runtime size, not the chip; the probe now prints both and flags the disagreement |
| PSRAM | **8 MB, octal**, initialised; `esp_psram_get_size()` = 8,388,608 |
| PSRAM pattern | **PASS** — 8,384,512 of 8,388,608 bytes (100.0%) in 2 blocks, 0 mismatches in both passes |
| Pins | **12 of 12** labelled pads drive both levels and are unobstructed. Scope: this does **not** prove the silkscreen label maps to the header position it names |
| SD | **FAIL** — `sdmmc_card_init` = `ESP_ERR_NOT_SUPPORTED` at default clock and at 400 kHz; GPIO14 card-detect reads high |

The flash discrepancy is left in on purpose: `esp_flash_get_size()` says 8 MB
while the chip's JEDEC ID and a real write/read above the 8 MB boundary both say
16 MB. Partition layout for the 64-colour rung should be sized from the measured
16 MB, and the app-side 8 MB reading wants a follow-up (build-time flash size vs
bootloader detection), not a silent "16 MB" in the notes.

The SD card is the open item: the card does not answer CMD0/CMD8 on the agreed
SPI pins at either clock, and the holder exposes no card-detect switch (GPIO14
high). That reads as a card that is not seated, a holder wired to different pins,
or a card that needs 3.3 V/SPI mode set on the holder — Stephen's bench call, and
nothing was written to the card.

### Peer review (Copilot, 2026-11-20)

Copilot reviewed `0169a50` file by file and returned four findings, all Medium
except the SD path:

1. **PSRAM test** — strong functional evidence (aliasing and undersized parts
   fail it) but not silicon identity: `esp_psram_get_size()` and the heap are
   software reports. A die-ID/IDD measurement would be stronger. Accepted; the
   claim in the reply is "8 MB addressed and bit-clean", not "JEDEC-verified".
2. **Pin self-test** — catches a dead or reserved pin, not a swapped label: a
   working GPIO with the wrong silkscreen still passes. Accepted and fixed in
   the output: `PIN_CHECK_SCOPE` now says so explicitly, and the reply no longer
   claims the labels were verified.
3. **SD read-only** — proven: only `sdmmc_card_init` + `sdmmc_read_sectors`, no
   mount, no format, no writes.
4. **8 MB vs 16 MB flash** — build/runtime metadata, not a smaller die. Accepted
   and fixed: the probe now also calls `esp_flash_get_physical_size()` and the
   two are printed side by side with a `FLASH_DISCREPANCY` line.

## Files

| File | Purpose |
| --- | --- |
| `src/main.c` | the probe: silicon, PSRAM, pins, SD |
| `partitions.csv` | adds the `report` partition at `0x310000` |
| `sdkconfig.defaults` | 16 MB flash, **octal** PSRAM, UART0 console |
| `capture.py` | console reader that survives tty re-enumeration (kept for when the bridge is fixed) |
| `flash-above-8mb.sh` | the independent flash-size proof |
| `report-2026-11-20.txt` | the read-back report for this run |
| `flash-above-8mb.log.txt` | output of the flash-size proof |
