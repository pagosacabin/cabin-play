# n16r8-sdprobe — tasks 0229 and 0234

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

# 0234 — retest with Stephen's new card (2026-11-21)

Stephen seated a different card on the morning of 2026-11-21 (~07:17 MST) and
said it is "a 512 MB card, one he hadn't used for a bit". The probe was extended
(not rewritten) and re-flashed; the card was treated as **read-only** — Stephen
did not designate it scratch, so step 5's write test was skipped.

## Extensions added for this task

- **First-pass table, persisted across resets** (`nvs` partition, key
  namespace `sdprobe0234`, blob `fp`): each boot appends one slot — the first
  CMD0/CMD8 responses *before any retry*, the attempt that reached READY, and
  the ACMD41 time. The table is keyed by a hash of the build stamp, so a
  reflash starts a new run without erasing NVS (the accepted 800x600 build's
  own settings live there). `bench/reset-loop.sh` drives 12 resets and reads
  the report after each.
- **Stock driver to a mounted volume**: `esp_vfs_fat_sdspi_mount` at 400 kHz
  (then the default clock if that fails) with `format_if_mount_failed = false`,
  a root listing, and the first 32 bytes of the first regular file, opened
  `"rb"`. No write API is called anywhere.
- **CID decoded at both byte alignments** (the SPI read may carry one extra
  leading byte) plus **capacity cross-checks**: a read past 512 MB and a read
  at the CSD-declared last block.
- **Every MBR entry walked**, with its own boot sector's label and root
  cluster, and the root sector re-read five times.

## Bug found and fixed in the 0229 probe (it changes one 0229 conclusion)

`raw_inspect()` computed the FAT32 root LBA from **partition-relative** offsets
and never added the partition's base LBA, so on a card whose partition starts at
2048 the root was read 2048 sectors too low (12320 instead of 14368) and came
back as zeros. 0229 reported that as "root directory ... first sector all
`0x00`: empty". With the fix the same reads list the directory. **The old
card's "root empty" line is therefore not evidence that the card was blank** —
it was a probe bug. This card's root is populated and reads correctly, and the
raw path now agrees with the driver path (23 entries, and `START4.ELF` starts
with the ELF magic).

## Result of the 2026-11-21 07:32–07:52 MST run

| Item | Measured |
| --- | --- |
| Card answers | **YES** — CMD0 10/10 `R1=0x01`, CMD8 R7 echoes `0x1AA`, ACMD41 READY in 38–59 ms |
| Card identity | `MID=0x03` (SanDisk), `OID="SD"`, `PNM="SR512"`, `PSN=e4845e78`, PRV 8.6, manufacture date **2024/9**, CSD v2 |
| Capacity (CSD v2) | **511,868,665,856 B = 488 GiB ≈ 512 GB** — not the 512 MB Stephen expected |
| Addressing | **block** (CCS=1, SDHC/SDXC class) |
| Volume | MBR 3 partitions: `0x0c` FAT32 `RECALBOX` at LBA 2048 (6,301,696 s), `0x07` at 6,305,792 (991,338,497 s), `0x83` at 997,646,336 (2,097,152 s) — a Raspberry-Pi Recalbox image written for this card's size |
| FAT32 | 512 B sectors, 4096 B clusters, 3.0 GiB volume, label `RECALBOX` |
| Root directory | **23 entries** (CRT, START4.ELF, FIXUP4.DAT, config.txt, overlays, …) |
| First file read | `START4.ELF`, 2,258,496 B, first bytes `7f 45 4c 46` (ELF) |
| Read stability | root sector identical on 6 consecutive reads; a 1,000,000 LBA read and the CSD last LBA both succeed |
| **CMD59 in idle state** | **R1=0x01 — ACCEPTED**, also after the driver's CMD52+CMD5 probe (CMD5 itself answers 0x05 = no response) |
| `sdmmc_card_init` (SPI) | **OK at 400 kHz, 100 kHz and 20 MHz** |
| **`esp_vfs_fat_sdspi_mount`** | **OK at 400 kHz** — the stock ESP-IDF path works on this card |
| First-pass behaviour | **14/14 boots failed the FIRST raw init pass** (CMD8 `R1=0x00`, first CMD0 garbage `0x3f`/`0x47`), READY on attempt 2 every time |

### What followed the card and what stayed with the board

- **Followed the OLD card**: the CMD59-rejected-in-idle-state quirk and the
  resulting `sdmmc_card_init` failure at `sdmmc_init_spi_crc`. This card
  accepts CMD59 (idle bit only) even after CMD5, so the stock driver both
  initialises and mounts it.
- **Stayed with the board**: the first-pass flakiness. 0229 saw it as "CMD8
  `R1=0x05` in 11 of 12 runs"; on this card it is 14/14 (and 13/13 in the intermediate
  pre-review build) with a different signature (`R1=0x00` and a garbage first CMD0). Same *shape* — the first SPI
  exchange after a reset is not understood and the second is clean — so it is
  the board's power-up/contact behaviour, not either card's.

### Working configuration for task 0230's write path

SPI mode, CS GPIO10, 400 kHz through `esp_vfs_fat_sdspi_mount` (20 MHz
`sdmmc_card_init` also verified). The write test itself was not run: the card is
read-only for this task. The card is a 512 GB card carrying a Recalbox image,
so any write-path work needs Stephen to name this card scratch first.

## Restore

The 800x600 build went back on and is byte-identical to the accepted build:
read-back of `0x10000..0xB5000` = `a2829d610cd2be6fa7455b18cc3e1b378248864f975b370e2eb949e862cdf0fc`,
the same hash as `.pio/build/n16r8-photo-800x600/firmware.bin` and as the
pre-flash read-back. The board's own report (`logs/07-...`) shows
`mode 800x600`, framebuffer `sha256 204578cc…`, `MATCH`, `362 vsync in 5.998 s`.

## 0236 — power-up delay before the first CMD0 (one variable)

0234 left the fault precisely measured: after every reset the *first* SPI
exchange with the card is garbage (CMD0 answers `0x3f`/`0x47`; CMD8 answers
`R1=0x00` with the R7 echo shifted one byte late) and the *second* attempt is
clean, 27 of 27 resets on two cards and two builds. Stephen's hypothesis: the
card needs more settling time after power-up.

The probe now takes a build-time settle delay, `SD_FIRSTPASS_DELAY_MS`,
inserted between the standard 74+ dummy clocks (CS high) and the first CMD0 of
the **first** raw-init attempt, and nowhere else — attempts 2+ and the second
probe call are byte-for-byte 0234's code path:

```bash
PLATFORMIO_BUILD_FLAGS="-DSD_FIRSTPASS_DELAY_MS=50" pio run -e n16r8-sdprobe
PLATFORMIO_BUILD_FLAGS="-DSD_FIRSTPASS_DELAY_MS=50" pio run -e n16r8-sdprobe -t upload
```

`0` compiles the delay out entirely (the 0234 control); a value outside 0..2000
is a compile error, which also proves the `-D` reached the file. The value is
printed in the report header and above the first-pass table, so every committed
log names its own delay. (The two added `FIRSTPASS_DELAY_MS:` lines are the only
report difference at delay 0 — peer review's one NIT.)

### Measured: 12 resets per value, five values, 65 boots, 0 first-pass successes

The probe's own report partition is the record; `logs/09`–`13` are the full
read-backs (13 slots each: the flash boot plus the 12 driven resets).

| delay (ms) | first_pass_ok / boots | CMD0 first response | CMD8 R1 (first attempt) |
| ---: | :---: | :--- | :--- |
| 0 (control) | **0 / 13** | `0x3f` x6, `0x47` x7 | `0x00` x13 |
| 10 | **0 / 13** | `0x3f` x10, `0x47` x3 | `0x00` x13 |
| 50 | **0 / 13** | `0x3f` x10, `0x47` x3 | `0x00` x13 |
| 100 | **0 / 13** | `0x3f` x11, `0x47` x2 | `0x00` x13 |
| 250 | **0 / 13** | `0x3f` x11, `0x47` x2 | `0x00` x13 |

**Verdict: no delay up to 250 ms changes the first-pass rate — the first init
pass failed on all 65 boots.** Note that these delays are *on top of* the 250 ms
post-boot settle already in `app_main` (`FIRSTPASS_DELAY_MS=250` therefore puts
~500 ms between reset release and the first CMD0), so the tested window runs to
about half a second of settling and the fault is still there.

The junk pattern does not change with delay either: attempt 1 still returns
`ff ff f8 47` for CMD0 (a leading `0xf8` before a real-looking R1) and
`ff 00 00 00 01 aa` for CMD8 — the R7 echo is present but read one byte late,
i.e. the first exchange after a reset is **misaligned**, not slow. Attempt 2's
clean `ff 01 ff ff…` / `ff 01 00 00 01 aa` shows the corrected alignment.
Settling time is therefore not the mechanism; the CS hypothesis (the other
candidate the task names) is a separate task and was not touched here.

The `FIRSTPASS_VERDICT:` line in the report is 0234's fixed wording ("does not
follow the old card") and is unrelated to the delay; read `FIRSTPASS_SUMMARY`.
Card seated throughout: SanDisk `SR512`, CSD capacity 511,868,665,856 B (512 GB),
label `RECALBOX` — the 0234 card. Read-only: raw init and reads only, no mount
with write, no format.

### Board state found before the run, and the restore

The frame board was **not** on the accepted build when this task started: the
app region matched
`/home/astroboy/wt/0235-goal-trial/…/n16r8-photo-800x600/firmware.bin` (built
2026-11-21 10:07) and its report partition held
`FATAL: embedded image is 307200 B, framebuffer needs 480000 B` — an 800x600 env
carrying the 640x480 raster, so it aborted at boot. Restored at the end from the
committed source (`tools/convert_photo64.py --source data/hummingbird.jpg
--space gamma --mode 800x600`, then `env:n16r8-photo-800x600`), with the
generated headers reverted afterwards so the committed tree is unchanged:

- board report: `mode 800x600`, `embedded 480000 B`, framebuffer
  `sha256 204578cc…` == tool `204578cc…`, `MATCH`,
  `362 vsync in 5.998 s -> 60.35 Hz`, `14473 fills -> 40.0 per frame`
- app region `0x10000..0xC5000` read back = `df402554…b7211`, byte-identical to
  the `firmware.bin` that was flashed, and `esptool verify_flash` reports
  `digest matched`
- the flashed image's own banner reads `build Nov 20 2026 17:23:09`: the object
  cache for that exact build (the accepted 0228 800x600 build, rebuilt during
  0230) was reused because the regenerated header is byte-identical to the one
  it was built from, so no translation unit recompiled. The raster sha256 chain
  and the read-back are what prove which image is on the board.

Peer review: Copilot (Herdr pane t17) on `21ceff2`, one pass, findings: 1 NIT
(the two added report lines at delay 0), **Verdict: SHIP**. No GitHub PR exists
in this repo for that branch; the review record is this section.

## 0238 — framing of the first transaction: a sacrificial frame cures it

0236 killed the timing hypothesis (0–250 ms of settle after the 74+ dummy clocks,
0/13 first passes at every value, 65 boots) and exposed the shape of the fault
instead: on attempt 1 CMD0 answers with a `0xF8` lead byte in front of a plausible
R1 (`ff ff f8 47`) or three bytes late (`ff ff ff c1/c2 3f`), and CMD8's R7 echo
arrives one byte late (`ff 00 00 00 01 aa`). The frame is **shifted**, not slow.

`SD_FRAMING_VARIANT` (build-time, `PLATFORMIO_BUILD_FLAGS`) selects the variant;
nothing else moves — clock 400 kHz, `SD_FIRSTPASS_DELAY_MS` stays 0, pull-ups as
in 0236, and no card write of any kind (raw init + reads only; the probe's FAT
mount is the base probe's and is unchanged).

| Variant | `-DSD_FRAMING_VARIANT=` | First pass | READY |
| --- | --- | --- | --- |
| Control (0236 arrangement) | 0 | **0/13** | 12/13 (one boot never reached READY) |
| Rung 1 — one sacrificial CMD0 after the dummy clocks | 1 | **13/13** | 13/13, every one on attempt 1 |
| Rung 2 — CS high ≥1 ms, then CMD0 | 2 | not run | rung 1 cured the first pass |

**Verdict: a single sacrificial CMD0 after the dummy clocks makes the first init
pass reliable (13/13 vs 0/13 control); the CS re-assert rung was not needed.**

The pairing is the evidence. In every rung-1 boot the throwaway frame comes back
with the control's broken shape and the frame immediately after it is clean:

```
SACRIFICE CMD0  arg=0x00000000: R1=0x3f payload[0]=ff ff ff c1 3f ff ff ff
CMD0        arg=0x00000000: R1=0x01 payload[0]=ff 01 ff ff ff ff ff ff
CMD8        arg=0x000001aa: R1=0x01 payload[4]=ff 01 00 00 01 aa ff ff ff ff ff ff
```

Control, same positions, every reset (13 boots, two runs of 13 and 14):

```
CMD0  arg=0x00000000: R1=0x3f payload[0]=ff ff ff c1 3f ff ff ff      (or 0x47 / ff ff f8 47)
CMD8  arg=0x000001aa: R1=0x00 payload[4]=ff 00 00 00 01 aa ff ff ff ff ff ff
```

Per-reset read-backs: `logs/14-0238-control-13resets.txt`,
`logs/15-0238-sacrificial-frame-13resets.txt`; the persisted tables:
`logs/16-…-table.txt`, `logs/17-…-table.txt`; harness logs `logs/18`, `logs/19`.
Each entry is one `esptool --after hard_reset` on the FT232R port followed by a
42 s settle and a `read_flash 0x310000 0x10000` of the report partition.

### Two tooling facts this run had to correct

1. **The 0234/0236 NVS namespace handed its table to a new build.** The first
   0238 boot reported `FIRSTPASS_BOOT: 16` with 15 rows carrying the *control's*
   signatures, under a build stamp that did not match — which would report a
   variant's rate as its predecessor's. 0238 opens its own namespace,
   `sdprobe0238`, so every run starts from an empty table; the accepted 800x600
   build's settings in NVS are untouched. Both arms were re-run after the change
   and the numbers above are from those clean runs.
2. **After the first flash of the rung-1 image the board rebooted itself 14
   times in ~45 s**, each boot appending a failing first pass (`0x3f`/`0x47`),
   before settling into the 13 clean reset boots measured above. It is not a host
   reset burst: one `esptool` connection per call, no retries in the logs. A
   re-flash of the *identical* image afterwards produced exactly one boot, clean.
   Mechanism undetermined (the sacrifice's benefit is absent on those back-to-back
   boots); recorded rather than explained — 14 failing boots out of 27 recorded
   for this build, which is why the verdict above is scoped to host-driven resets.

### Board state found and restored

Board arrived on the accepted 800x600 build: report partition held
`mode 800x600`, framebuffer `sha256 204578cc…`, `MATCH`, `362 vsync in 5.998 s`
(`logs/20-0238-board-pre-state.txt`). Restored to the same build afterwards —
`mode 800x600`, `sha256 204578cc… == tool`, `MATCH`, `362 vsync in 5.998 s`,
`14473 fills -> 40.0 per frame`, `Hash of data verified.` on the write
(`logs/21-0238-board-restored-800x600.txt`). No card write of any kind: raw init
and CMD17/CMD9/CMD10 reads only.

Peer review: Copilot (Herdr pane t17) on the probe diff before the bench run —
one pass, no findings, **Verdict: SHIP**.


## 0239 — does a pre-sacrifice make the stock driver's first init clean?

0238 cured the **raw** probe's first init pass with one throwaway CMD0 after the
dummy clocks (13/13 vs 0/13). This task asks whether the same sacrifice carries
over to the **stock ESP-IDF driver**: per boot, exactly one
`sdmmc_card_init()` call is the measurement, and the arm decides whether a raw
pre-sacrifice precedes it (`SD_FOLD_ARM`, build-time; leaving it undefined
leaves the 0229/0234/0236/0238 probe byte-for-byte in place).

```bash
PLATFORMIO_BUILD_FLAGS="-DSD_FOLD_ARM=0" pio run -e n16r8-sdprobe -t upload   # control
PLATFORMIO_BUILD_FLAGS="-DSD_FOLD_ARM=1" pio run -e n16r8-sdprobe -t upload   # sacrifice
```

One variable: 400 kHz, `SD_FIRSTPASS_DELAY_MS` stays 0, pull-ups unchanged, and
the card is strictly read-only (raw init + reads; the sacrifice arm's only mount
is `format_if_mount_failed = false`, listing only).

### Measured: 12 host resets per arm, one init call per boot

The tally is the board's own persisted table (NVS namespace `sdprobe0239`,
keyed by a hash of the build stamp **and the arm**), read back at the end of
each arm — `logs/24`, `logs/25`; per-boot reports `logs/22`, `logs/23`.

| Arm | `-DSD_FOLD_ARM=` | first-call `sdmmc_card_init` OK / boots | failure shape |
| --- | --- | :---: | --- |
| Control | 0 | **11 / 19** (58 %) | 8 × `ESP_ERR_INVALID_CRC` (265) at `sdmmc_io_reset`; the driver never reached CMD0 |
| Pre-sacrifice | 1 | **14 / 14** (100 %) | none |

**Verdict: the pre-sacrifice does carry over — it makes the stock driver's first
init reliable on this board (14/14 vs 11/19), because the bad first frame lands
on the sacrificial CMD0 instead of on the driver's own first command.**

The sacrifice's own response is the evidence that it caught the fault every
time: `sac_r1 = 0x3f` ×9, `0x47` ×4, `0x7f` ×1 (14/14 boots) — the same
misaligned-junk family 0236/0238 measured — and it was discarded, after which
the driver's call was clean.

### Where the first call actually fails — one command earlier than expected

0234 said the driver "retries internally, so its success hides the first-pass
fault". On this card the failing command is **not** CMD0: the driver's very first
transaction is the SDIO reset (`CMD52`, inside `sdmmc_io_reset`, which
`sdmmc_card_init` runs before `sdmmc_send_cmd_go_idle_state`), and that is what
comes back as `r1=0x3f` and aborts the call with `ESP_ERR_INVALID_CRC`:

```
V sdspi_host: sdspi_host_start_command: slot=1, CMD52, arg=0x80000c08 flags=0x80 ...
V sdspi_transaction: r1 = 0x3f hw_cmd.r[0]=0xffffffff
I sdspi_transaction: cmd=52, R1 response: command CRC error
E sdmmc_io: sdmmc_io_reset: unexpected return: 0x109
```

`driver_cmd0_frames` (counted from the driver's own log) is **2 on every boot
that got past CMD52** and **0 on every boot that failed**: the driver's
mandatory double CMD0 in SPI mode always ran when it was reached, so its
internal retry is not what this fault defeats — the first transaction of the
call is. In 0234 the stock `esp_vfs_fat_sdspi_mount` succeeded because the probe
had already pushed hundreds of transactions through the bus in that same boot
before the driver's init; the driver on a freshly reset bus fails ~2 boots in 5.

### End-to-end on the final sacrifice boot

Every sacrifice boot runs the mount after the first-call result is written to
the report partition, so the arm's last boot is the end-to-end proof:
`esp_vfs_fat_sdspi_mount` OK at 400 kHz, `ROOT_ENTRIES: 23` — the RECALBOX
listing 0234 recorded (`logs/23`, `FOLD_E2E_MOUNT`).

### Board state found and restored

Arrived on the accepted 800x600 build (`logs/28`: `mode 800x600`, framebuffer
`sha256 204578cc…`, `MATCH`, `362 vsync in 5.998 s`). Restored to the same build
afterwards — `esptool verify-flash` → `Verification successful (digest matched)`,
and the board's own report after the restore is **byte-identical** to the
pre-state read-back (`logs/29`, diffed against `logs/28`).

### Peer review (Copilot, Herdr pane t17) — four passes, each finding fixed

1. **MAJOR** — a sacrifice whose bus setup failed still counted as a trial. Fixed:
   `fold_presacrifice()` returns `esp_err_t`, and any failed step (bus init,
   device add, dummy-clock burst, CMD0 transfer, trailing 8-clock cleanup) marks
   the boot `arm=0xFE` (INVALID), excluded from the tally and from the mount.
2. **NIT** — the table key was the build stamp alone, so two arm builds in the
   same second could share a table. Fixed: the key hashes the stamp **and** the arm.
3. **NIT** — `fold_one_init()` could fail before calling `sdmmc_card_init` and the
   error was recorded as the call's result. Fixed with `bool *init_called`; an
   uncalled init records INVALID.
4. **MAJOR** — `sd_raw_cmd()` ignored the trailing 8-clock transfer's result, so a
   sacrifice whose frame cleanup failed could still count. Fixed: that failure
   sets `g_raw_tx_err` too (the base probe never reads the flag).

Final verdict: **SHIP**. No GitHub PR in this repo; this section is the record.

---

# 0240 — kit card: format, mount, write, read back

Stephen designated **the 1 GB kit SDSC card** as scratch (the card 0229 identified:
CSD v1, 960 MB, CID `05 00 0c 61 73 64 66 67 22 00 00 13 a4 01 9a 00`, serial
`0x000013a4`) and approved erasing it, so 0230's rung 3 got its write-path test.

The kit build (`SD_FOLD_ARM=1 SD_KIT_TASK=1`) adds a destructive arm to this probe:

1. **identity gate** — raw CID/CSD read, refuses anything that is not this card
   (CID serial `0x000013a4` **and** SDSC/byte-addressed/900-1200 MB). An empty holder,
   the 512 GB Recalbox card, or any other ~1 GB card stops the run before a write;
2. the 0239 pre-sacrifice, then **one** `sdmmc_card_init` (the measurement);
3. mount (plain, one retry, then a flag-carrying mount only to obtain a card handle),
   then an **explicit** `esp_vfs_fat_sdcard_format()` — the erase is never inferred from a
   successful mount;
4. a 128 KiB deterministic pattern written, read back in place, then read again after
   unmount → remount; board-side mbedtls SHA-256 on all three passes, and
   `bench/kitcard0240/expected_pattern.py` recomputes the same bytes on the host.

## Result of the 2026-11-22 06:56-06:59 MST run — stopped at driver init

| Item | Measured |
| --- | --- |
| Seated card | **the kit card** — gate PASS, CID `05 00 0c 61 73 64 66 67 22 00 00 13 a4 01 9a 00`, serial `0x000013a4`, CSD v1, 960 MB, `OCR 80 ff 80 00`, CCS=0 |
| Pre-sacrifice | ran, R1=`0x01` (idle), discarded |
| `sdmmc_card_init` (400 kHz) | **FAIL `ESP_ERR_NOT_SUPPORTED` (262)** at `sdmmc_init_spi_crc` |
| Failure shape | CMD52 R1=`0x05` (tolerated) → CMD0 → CMD8 R7 `0xaa0100f0` → **CMD5 error `0x107`** ("not IO card") → **CMD59 R1=`0x05` illegal** → `sdmmc_init_spi_crc returned 0x106` |
| Mount attempts 1, 2 and 3 | all FAIL `ESP_ERR_NOT_SUPPORTED` (262), same stage |
| Format / write | **not attempted** — the firmware stops at the mount failure by design |
| Card content | untouched: no format call ran, no write path was reached |

So the answer to 0239's open question is **no**: the pre-sacrifice does not rescue this card.
0239's fold was measured on the 512 GB SR512 card (14/14 first-init OK, mount OK); the same
firmware on the kit card fails at the same `CMD59`-after-`CMD5` step 0229 named. The fault
follows the card, not the fold, the wiring or the clock. Per this task's constraints there is
no in-task workaround ladder: the two real options are a card that tolerates CMD5, or a raw
sector writer that bypasses `sdmmc_init_spi_crc` (a different task).

Logs: `logs/31-0240-board-pre-state.txt` (pre-flash), `logs/32-0240-kitcard-gate-and-init.txt`
(the full report read back from `0x310000`), `logs/33-0240-board-restored-800x600.txt`.
The board was restored to the accepted 800x600 build and verified from its own report
(framebuffer `sha256 204578cc…bd306`, MATCH).

Peer review (Copilot pane t17, 2026-11-22): three passes. Findings — (1) BLOCKER the gate
admitted any ~1 GB SDSC card, fixed by requiring the CID serial; (2) MEDIUM
`format_if_mount_failed` cannot be reported as the format, fixed by always formatting
explicitly; (3) MEDIUM the unmount result was ignored, fixed by requiring it for a PASS.
Final verdict **SHIP** at `f2fcbe9`.
