# n16r8-slideshow — SD-card slideshow on the living frame (0243)

Ten photographs live on the frame board's SD card; the firmware cycles them on
screen, one every ~10 s, looping back to the first.  It is the accepted 0228
800x600 gamma photo build with the proven SD stack bolted on, plus a fallback:
no card, no mount, or no loadable frame and the screen shows the embedded
hummingbird exactly as it does today, so it never goes blank for a storage
reason.

Board: YD-ESP32-23 / N16R8, MAC `c4:9e:7e:08:6d:f4`.
Card: the 32 GB SanDisk 0241 formatted (CID PSN `0xc6bd0f92`), FAT32, with
`/NEWCARD.BIN` on it — that file is 0241's evidence and is never touched.

## One project, two apps

| env | what it does |
|---|---|
| `n16r8-slideshow-stage` | copies the ten rasters from the `frames` flash partition to `/sdcard/FRAMES/FRAME01.BIN … FRAME10.BIN`, then reads every file back from the card and compares SHA-256 with the flash copy.  Halts when done (it does not display). |
| `n16r8-slideshow` | the slideshow.  Mounts the card (0239 fold), enumerates `/sdcard/FRAMES` sorted by name, streams each raster into the display framebuffer, advances every 10 s, loops. |

`-DAPP_STAGE=1` selects the first; the source is one file on purpose, so the SD
init sequence, the identity gate and the frame loader are the same code in both.

## How the frames get onto the card

The card is in the board's holder, so the frames are staged through the board:

```bash
cd living-frame/firmware/n16r8-slideshow
/usr/bin/python3 tools/fetch_frames.py --out frames-src     # 10 Commons photos
/usr/bin/python3 tools/build_frames.py  --src frames-src --out frames
cp frames/frames-blob.bin bench/
bench/stage.sh            # esptool writes the blob at 0x400000, flashes the
                          # stage build, waits for it to finish
bench/read_report.py      # the stage build's log, from the report partition
```

`frames-blob.bin` is 4,800,000 B — ten 480,000 B rasters, play order = name order
— and is written to the 5 MiB `frames` data partition at `0x400000`
(`partitions.csv`).  Nothing about it is specific to the stage build; the
slideshow reads the card, not the partition.

## Display and SD parameters (unchanged from the builds that proved them)

* **Display** — 0228's 800x600 mode: `pclk 40 MHz`, `h 128+88+800+40`,
  `v 4+23+600+1` → 60.3 Hz, 8 bpp RGB222 (`(b<<4)|(g<<2)|r` per DAC byte),
  `no_fb` with 15-line bounce buffers, framebuffer in PSRAM.  Data pins
  `bit0..5 = 5,4,7,6,16,15`, hsync 17, vsync 18, both active-low.
* **SD** — SPI2, SCK 12 / MOSI 11 / MISO 13 / CS 10, mode 0, **400 kHz**, with
  0239's pre-sacrifice (74+ clocks with CS high, one throwaway CMD0, bus
  released) before the single stock-driver init.  Clock, CS handling, pull-ups
  and the zero first-pass delay are exactly the configuration 0241 proved; at
  400 kHz a 480,000 B read costs ~10 s, which is why the next frame is prefetched
  into a second PSRAM framebuffer while the current one is on screen.

## Files

```
src/main.c                 both apps, one source
src/photo64_data.h         embedded fallback raster (480,000 B, 800x600 gamma)
src/photo64_meta.h         its mode / space / sha256, written by the converter
data/hummingbird.jpg       the accepted build's photograph, the fallback source
tools/convert_photo64.py   the accepted converter, unchanged
tools/fetch_frames.py      the ten Commons sources, by exact title
tools/build_frames.py      convert + sha256 + concatenate into frames-blob.bin
bench/stage.sh             blob -> flash, flash the stage build
bench/show.sh              flash the slideshow
bench/read_report.py       read/parse the report partition
logs/                      the bench runs quoted in the 0243 reply
```

## Traps this project already paid for

* **Passing an uppercase `.BIN` to `convert_photo64.py` fails after the raster is
  written.**  The preview paths are derived with `str.replace(".bin", …)`, which
  does not match `.BIN`, so PIL is asked to save a PNG to a `.bin` name and
  raises — with the 480,000 B raster already on disk and correct.
  `build_frames.py` converts to lowercase and renames afterwards.
* **The partition table has to fit the *configured* flash size before the
  partition tool will run.**  A 9 MB table was rejected with "does not fit in
  configured flash size 8MB" until `board_upload.flash_size = 16MB` was set next
  to `board_build.flash_size`.
* **A report write during scanout is not free.**  A flash erase (64 KiB, tens
  of ms) disables the cache the bounce-buffer ISR reads PSRAM through and the
  RGB FIFO underflows.  The partition is therefore erased **once at boot, before
  the panel starts**, and every later persist is a single already-erased 4 KiB
  sector write (`magic, len, text`) — a few ms, inside the bounce budget.  The
  reader walks the sectors in order; a later boot appends after the previous
  run's records instead of erasing them, which is why one dump shows the stage
  run and the slideshow run one after the other.
* **The SHA-256 is computed in software** (carried over from 0241, whose mbedtls
  digest came back with its first 8 bytes zeroed inside the card-I/O path).  Its
  known-answer test caught this project's own arithmetic: a 480,000-byte
  zero-fill hashes to 117 whole 4 KiB blocks plus a remainder, and the first KAT
  silently hashed only 479,232 B.  The KAT now uses 1 MiB of zeros.
* **The card gate wants more than a serial number.**  The stage build refuses
  to write unless the seated card's CID serial, CID name (`SD32G`) **and**
  capacity (28-34 GB) all match 0241's SanDisk, and it refuses the 1 GB kit card
  and the 512 GB Recalbox SR512 explicitly.  A relabelled card that duplicated
  all three would still pass: the write is limited to `/sdcard/FRAMES/`, and
  nothing here formats or repartitions.
* **The FT232R console on this bench delivers nothing** (0225).  Every claim in
  the reply comes from the `report` partition, read back with esptool — which
  also resets the board, so a report is only read when the run it describes is
  finished.
