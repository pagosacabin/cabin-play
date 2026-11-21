# n16r8-sdprobe — task 0229

SD-card diagnosis on the living frame's one wired board (YD-ESP32-23 / N16R8,
ESP32-S3, MAC `c4:9e:7e:08:6d:f4`) after Stephen resoldered the holder's pins
10-13 at 15:21 on 2026-11-20.

The question was Stephen's: *does the SD card work now, and report back either
way with numbers.* It does answer now. The firmware library still will not
mount it, and this probe names why.

## Wiring this probe assumes

```
SD (SPI)  SCK GPIO12   MOSI GPIO11   MISO GPIO13   CS GPIO10
          GPIO14 spare / card-detect (reads high: the holder has no CD switch)
```

## Build, flash, read the result

```bash
pio run -e n16r8-sdprobe                 # build
pio run -e n16r8-sdprobe -t upload       # /dev/ttyUSB0, the FT232R bridge
esptool --chip esp32s3 --port /dev/ttyUSB0 --baud 460800 \
        read_flash 0x310000 0x10000 report.bin
python3 -c "import struct;d=open('report.bin','rb').read();m,n=struct.unpack('<II',d[:8]);print(d[8:8+n].decode())"
```

The FT232R console delivers nothing usable (0225), so every line — including
the IDF driver's own log, routed through `esp_log_set_vprintf` — is collected in
RAM and written to the `report` partition at `0x310000`. That read-back is the
authoritative record; `logs/` holds the runs quoted in the reply.

## What the probe does

1. Line levels (MISO/CS/MOSI/SCK/GPIO14) with and without internal pull-ups.
2. Raw SPI command probe at 400 kHz with CS driven by hand: CMD0, CMD8,
   CMD55/ACMD41, CMD58, CMD13 — printed as raw bytes, not as one driver code.
   Repeated up to 5 times, attempt number recorded.
3. Read-only content inspection through CMD17/CMD9/CMD10: CSD → capacity,
   CID, MBR, boot sector, root directory, first 32 bytes of the first file.
   **Nothing is written to the card** — no mount with write access, no format.
4. `sdmmc_card_init` over SPI at 400 kHz, 100 kHz and the default 20 MHz, with
   the driver's own DEBUG log captured.
5. `sdmmc_card_init` in SDMMC 1-bit mode on the same pins (CLK 12, CMD 11,
   D0 13).

## Result of the 2026-11-20 20:09-20:15 MST run

| Item | Measured |
| --- | --- |
| Card answers | **YES** — CMD0 10/10, CMD8 R7 echoes `0x1AA`, ACMD41 READY in 12-19 ms |
| Card identity | **SDSC 1 GB** — CSD v1, capacity 1,006,632,960 B (960 MB), CID serial `0x000013a4` |
| Addressing | **byte-addressed** (OCR CCS=0), CMD17 arg = LBA x 512 |
| Filesystem | **FAT32**, 512 B sectors, 4096 B clusters, 245,020 clusters, 959 MB, label `NO NAME` |
| Partition | MBR type `0x0c` at LBA 2048 |
| Root directory | cluster 2 → LBA 3872, first sector all `0x00`: **empty** |
| Reads | work over CMD17 (read-only) — MBR and BPB parse at their exact offsets |
| `sdmmc_card_init` (SPI) | **FAIL** `ESP_ERR_NOT_SUPPORTED` at 400 kHz, 100 kHz and 20 MHz — all at `sdmmc_init_spi_crc` |
| CMD59 after the driver's CMD5 probe | **R1=0x05** (illegal command) |
| CMD59 with no CMD5 in the sequence | **R1=0x01** (accepted; bit0 is the idle bit, not an error) |
| SDMMC 1-bit | FAIL `ESP_ERR_TIMEOUT` (see caveat below) |

### The mechanism, from the driver's own log plus a controlled replay

`logs/02-driver-log-names-the-fatal-step.txt`:

```
V sdspi_transaction: r1 = 0x05 ... cmd=52, R1 response: command not supported  <- tolerated
V sdspi_transaction: r1 = 0x00                                                <- CMD0 ok
V sdspi_transaction: r1 = 0x01 hw_cmd.r[0]=0xaa0100f0                        <- CMD8, "SDHC/SDXC card"
D sdspi_host: sdspi_host_start_command: cmd=5 error=0x107                    <- CMD5: no response
V sdspi_transaction: r1 = 0x05 ... cmd=59, R1 response: command not supported <- FATAL
E sdmmc_sd: sdmmc_init_spi_crc: sdmmc_send_cmd_crc_on_off returned 0x106
```

`sdmmc_card_init` probes SDIO with **CMD5** before it enables CRC, and this card
does not tolerate CMD5. The replay in the final report isolates it:

```
with CMD52+CMD5 first: CMD52=0x05  CMD5=0xff  CMD59=0x05   <- poisoned
without CMD5 (control):                        CMD59=0x01   <- accepted
CMD52 only,   then CMD59=0x01                               <- CMD52 is harmless
CMD5 only,    then CMD59=0x05                               <- CMD5 is the poison
```

After a CMD5 the card answers `0x05` (illegal command) to everything until a
CMD0 reset. That is a card firmware quirk, not a wiring fault: the card reads
perfectly, but the driver's own SDIO probe locks it out of the step that
follows. Note that `R1=0x01` is **not** an error — bit0 means "in idle state" —
so an idle-state CMD59 answering `0x01` is an accepted command.

The driver cannot be talked out of the CMD5 probe, so the SPI init sequence has
to be bypassed for this card (this probe already reads sectors directly), or a
card that tolerates CMD5 used instead.

### First-pass flakiness worth naming

The first init pass after a reset fails at CMD8 with `R1=0x05` in 11 of 12
completed runs; the immediately following pass succeeds 12 of 12. This is the
shape of a marginal contact or a card that wants longer after power-up, and it
is exactly what rung 2 (meter: 3.3 V at the holder's VCC with the card seated,
continuity pad-to-pin, shorts between adjacent lines) is there to settle.

### SDMMC 1-bit caveat

The native-mode test ran after the card had been put into SPI mode, and a card
stays in SPI mode until its power is cycled — so that failure is **not** a
measurement of the 1-bit path. It would need a power cycle before the first
command to be meaningful.

### Verdicts the probe prints (separated on purpose)

```
SD_VERDICT_RAW:    the card ANSWERS on SPI (raw init READY)
SD_VERDICT_DRIVER: sdmmc_card_init is unavailable for this card (CMD59 rejected in
                   idle state; see the driver log above) -- an API-library limit,
                   not a wiring fault
SD_VERDICT_CARD:   identified (see CSD/CID/FAT lines above)
SD_VERDICT_READ:   read-only inspection completed through CMD17
```

A single pass/fail line would erase this task's whole finding: the card works and
the library does not.

## Peer review (Copilot, 2026-11-20)

Copilot reviewed `ea58165` and returned three findings, all fixed before the
follow-up commit:

1. **Medium — a false "card does not answer" verdict.** `raw_probe()` could reach
   READY and read the card while the final verdict still said the card did not
   answer, because the raw result was not part of `ok`. Fixed by splitting the
   verdict into RAW / DRIVER / CARD / READ lines (above).
2. **Medium — a false "inspection OK".** A failed root-directory or boot-sector
   read was logged but the caller still printed `inspection OK`. Fixed with an
   explicit `g_inspect_ok` that every read failure clears.
3. **Low — corrupted FAT listing.** The first-file read reused the directory
   buffer, so the rest of that chunk parsed as directory entries. The duplicate
   driver-path lister (`read_root()`/`inspect_card()`) was deleted entirely --
   one lister, one buffer discipline, and it is the path that actually runs.

A second pass on that commit found the fixes incomplete, and it was right:
the DRIVER verdict asserted the CMD59 mechanism even when the probe never ran
(now gated on a tri-state probe result), `g_inspect_ok` was not cleared by
root-sector read errors (now every failure path clears it), and the aliasing
fix had landed in `raw_inspect()` while `read_root()` still aliased (moot now
that `read_root()` is gone).

A third pass found two more, both fixed: `0xFF` (no response) was counted as a
CMD59 *rejection* -- it is now a distinct "no response" state, and the mechanism
is only named for a real R1 rejection; and a failed CID read or first-file read
did not clear `g_inspect_ok`, which it now does.

## What this means for the write path (task 0230)

A mount through ESP-IDF's `sdmmc`/`sdspi` driver is not available for this card
as written. The options, in order of cost:

1. a card that implements CMD59 in idle state (any ordinary card);
2. bypass `sdmmc_init_spi_crc` — drive the card directly (this probe already
   reads sectors) and put a FAT writer on top, or use the Arduino `SD` library,
   which ignores a failed CMD59;
3. keep the kit card read-only and use a different card for writes.

No write was attempted on the kit card: 0229 designated it read-only unless
Stephen names a scratch card.

## Restore

The accepted 800x600 build goes back on the board afterwards, byte-identical:

```bash
cd ../n16r8-photo
esptool --chip esp32s3 --port /dev/ttyUSB0 --baud 460800 write_flash \
  0x0    .pio/build/n16r8-photo-800x600/bootloader.bin \
  0x8000 .pio/build/n16r8-photo-800x600/partitions.bin \
  0x10000 .pio/build/n16r8-photo-800x600/firmware.bin
```

Verified from the board's own report (`logs/05-frame-board-restored-800x600.txt`):
`mode 800x600`, framebuffer `sha256 204578cc…bd306`, `MATCH` — the frozen 0228
proof reproduced, `362 vsync in 5.998 s` (one frame of measurement grain).

## Board bugs this probe had to fix (each cost a run)

- **Data arrives inside the polling transfer.** Searching for the `0xFE` token
  after the transfer and then reading the payload in a fresh one returns idle
  `0xFF` bytes — an all-`0xFF` "CSD" that computes a 2 TB card. Capture the
  payload from the same buffer.
- **`g_byte_addressed`.** An SDSC card takes a byte address in CMD17. Reading
  LBA 2048 as byte 2048 lands inside sector 4 and looks like a corrupt boot
  sector.
- **FAT32 root directory is data-area relative.** `dir_lba = data_lba + (root_clus-2)*spc`;
  omitting `data_lba` prints the FAT table as directory entries.
- **CRC7 is real once a card is told CRC_ON_OFF=1.** A placeholder `0xFF` then
  earns `R1=0x08` on every frame. `sd_crc7()` here is validated against the two
  spec constants (CMD0 → `0x95`, CMD8(0x1AA) → `0x87`).
- **The main task stack is 3.5 kB by default.** 512-byte sector buffers plus
  300-byte SPI frames overflow it, and the app dies before writing the report
  (the read-back then shows the *previous* run's report, which reads as "no
  change"). `CONFIG_ESP_MAIN_TASK_STACK_SIZE=16384`, plus a partial report
  flush after each section.

---

# 0235 — stock driver to a mount, and a first-pass counter (build-only)

This task extends the probe; it does not re-diagnose the card, and it did not
touch the hardware: the firmware was **built, not flashed** (the board and the
seated card were off-limits for this task). So no run is claimed here — the
numbers in the 0229 section above remain the last measured ones, and re-running
the probe is what produces 0235's own report through the read-back procedure at
the top of this file, unchanged.

Two additions to `src/main.c`, both in the existing report style:

1. **`=== 2A. STOCK DRIVER: sdmmc_card_init + read-only FAT mount ===`**
   `esp_vfs_fat_sdspi_mount()` at 400 kHz — the clock the raw probe proved —
   with `format_if_mount_failed = false`, a root listing (first 16 entries) and
   the first 32 bytes of the first regular file, opened `"rb"`. No write API is
   called anywhere (no `f_write`, rename, unlink or mkfs). Because 0229's CMD5
   replay (`DRIVER SEQUENCE REPLAY`: `g_seq_with`, `g_seq_without`,
   `g_seq_52only`) already runs inside the first `raw_probe()`, a failure here
   is **attributed** rather than merely reported: `SD_VERDICT_MOUNT` names the
   driver's SDIO probe only when the replay's controls actually isolate CMD5
   (0x05 after CMD5, accepted without it, CMD52 alone harmless), and says
   `NOT attributed` otherwise.
2. **`=== 2B. FIRST-PASS INIT COUNTER ===`** — one slot per boot, kept in the
   `nvs` partition (namespace `sdprobe0235`, blob `fp`, 16 slots) so the table
   survives the resets that produce it. Each slot records `FIRST=PASS/FAIL`,
   the first CMD0 and CMD8 R1 of the boot (captured on attempt 1, before any
   retry), the attempt that reached READY and its ACMD41 time. The table is
   keyed by a hash of the build stamp, so a reflash starts a new run instead of
   mixing builds, and NVS is never erased here (the 800x600 build's settings
   live in the same partition). Ten or more resets make 0229's hand-measured
   question — *does the first init pass after a reset fail?* — answerable by the
   board itself.

Both sections go out through `app_logf()` / `write_report_partition()`, and the
verdict block gains `SD_VERDICT_MOUNT` and `SD_VERDICT_FIRSTPASS`. Existing
probe logic, log formats and partition offsets are untouched: the only edited
lines in 0229's code capture return values that were previously discarded.

Build check (no upload):

```bash
cd living-frame/firmware/n16r8-sdprobe && pio run      # exit 0
```
