# living-frame — rung 1: photo display firmware

Rung 1 of the living-frame project (Christmas gift): a **classic ESP32** showing
**one real photographic image**, full-screen at 640×480, on a VGA monitor.

Rung 0 (`../vga-proof`) proved the display path with colour bars. This rung
replaces the bars with an actual JPEG, decoded on the chip and dithered down to
the 3-bit palette the hardware can carry. No SD card, no WiFi, no slideshow —
those are later rungs.

```
photo-display/
├── platformio.ini           PlatformIO project (env: esp32dev)
├── src/main.cpp             the firmware: VGA init + decode + hold the frame
├── src/photo_render.h       TJpgDec glue + Bayer dither (shared with the host check)
├── data/photo.jpg           the source photo, embedded in flash (108 557 bytes)
├── data/PHOTO-SOURCE.md     where the photo came from, its licence, exact transform
├── lib/TJpgDec/             vendored TJpgDec R0.03 (see its NOTICE.md)
├── tools/render_check.cpp   host-side render check (same code path, PC pixel sink)
├── tools/render-check.png   output of that check — what the frame buffer holds
├── build-esp32dev.log.txt   build + flash log excerpt for the esp32dev target
└── .gitignore
```

## Build and flash

```bash
cd living-frame/firmware/photo-display
pio run                 # build
pio run -t upload       # flash (auto-detects the port)
```

The photo is compiled in (`board_build.embed_files = data/photo.jpg`), so a clean
clone reproduces the exact build with no extra download and no filesystem image.

## How it works

1. `data/photo.jpg` is linked into the image as a raw binary. Its size and
   address are exposed by the linker as `_binary_data_photo_jpg_start/_end`.
2. `jd_prepare()` parses the JPEG headers out of that buffer (TJpgDec, streaming,
   `JD_FASTDECODE 1`, 3.5 kB workspace in internal RAM).
3. `jd_decomp()` decodes MCU block by MCU block. TJpgDec hands each block to
   `photoOutFunc`, which converts every RGB888 pixel to the eight-colour palette
   with a 4×4 ordered (Bayer) dither and pushes it into the VGA frame buffer
   with `vga.dotFast()`.
4. `loop()` does nothing. The I2S interrupt scans the frame buffer out forever, so
   the picture is stable by construction: nothing is redrawn, so nothing can roll
   or flicker.

**No full-image buffer is ever allocated.** A 640×480 RGB buffer would be 900 kB
and the chip has 320 kB; the decoder streams 16×16 blocks and the only image
buffer in the system is the 154 kB 3-bit frame buffer that rung 0 already proved.

**Why the image looks dithered.** One bit per colour channel is the hardware
limit of this VGA path (see `../vga-proof/README.md` for the arithmetic: 640×480
at 6-bit is 307 kB, more than the whole SRAM). The dither is what turns 8 flat
colours into something that reads as a photograph; it is expected, not a defect.

## Verified

Build, on the laptop (`build-esp32dev.log.txt`):

```
RAM:   [=         ]   8.1% (used 26608 bytes from 327680 bytes)
Flash: [===       ]  29.7% (used 388753 bytes from 1310720 bytes)
========================= [SUCCESS] Took 1.18 seconds ========================
```

(the first full build for this env took 4.91 s; the committed log is an
incremental no-op rebuild of the identical binary — 389 120 bytes,
sha256 `1e95aa87…`)

Flashing to the ESP32-WROOM-32 devkit (`4c:c3:82:da:d5:68`), which was still
carrying rung 0:

```
Wrote 389120 bytes (267019 compressed) at 0x00010000 in 6.3 seconds
Hash of data verified.
```

Runtime console from that board after the flash:

```
living-frame rung 1 -- photo display
pins: R=14 G=19 B=27 HSYNC=32 VSYNC=33
I2S TX
photo.jpg embedded: 108557 bytes
jpeg: 640x480, 3 component(s) -> 640x480 screen
frame drawn in 1611 ms (307200 pixels), free heap: 100348 bytes
holding image
```

So on hardware: the embedded JPEG is complete, the decoder parses it as 640×480
YUV with 3 components, and all 307 200 pixels reached the frame buffer in 1.6 s
with 100 kB of heap still free.

`tools/render_check.cpp` compiles `src/photo_render.h` **unchanged** on a PC and
writes the resulting frame to a PPM. That isolates everything except the VGA
electrical path: decoder configuration, the dither, the palette. Its output is
`tools/render-check.png`. Checks against `data/photo.jpg`:

| Check | Result |
|---|---|
| pixels drawn | 307 200 / 307 200 |
| colours present | exactly 8 (the R1G1B1 palette) |
| block-averaged render vs source, RMSE | 0.0319 (3.2 %) |
| luminance NCC at 40×30 and 160×120 | 0.9992 |
| same NCC with the source rolled 10 px | −0.025 (the measure discriminates) |

On-screen appearance is the last unproven step and needs a monitor: **Stephen's
acceptance photo closes the rung**. Nothing here can photograph its own VGA
output.

## Reproducibility caveat

A fresh `git clone` of `main` at the commit that added this project was checked
out, and `pio run -e esp32dev` there ends `[SUCCESS] Took 4.87 seconds` — the
project is self-contained (the vendored TJpgDec and the `lib_extra_dirs` reference
into `vga-proof` both resolve from the clone).

The resulting `.bin` is **not byte-identical** to the one flashed from the
working tree: 389 168 vs 389 120 bytes, with the differences in the image header
and in string tables. That is expected — the ESP-IDF app descriptor embeds a
build timestamp and the build path, and the checkout path here is longer, which
accounts for the 48-byte size difference. So the `sha256` quoted above identifies
**the image that was flashed onto the board**, not a universal hash of the source.

## Wiring

Unchanged from rung 0 — see [`../vga-proof/WIRING.md`](../vga-proof/WIRING.md)
for the connection table, the adapter route, the direct-DB15 fallback and the
resistor values. In one line:

| ESP32 | 14 | 19 | 27 | 32 | 33 | GND |
|---|---|---|---|---|---|---|
| VGA | R (1) | G (2) | B (3) | HSYNC (13) | VSYNC (14) | 5–10 |

## Credits

* VGA: **bitluni's ESP32Lib**, vendored at `../vga-proof/lib/ESP32Lib`
  (CC BY-SA 4.0, with a two-line fix for arduino-esp32 2.x — see its NOTICE.md).
  Reused in place via `lib_extra_dirs`, not copied, so the repo holds one copy.
* JPEG: **TJpgDec R0.03** by ChaN, vendored at `lib/TJpgDec` — see its NOTICE.md
  for provenance and the single configuration change (`JD_FORMAT 0`).
* Photo: NASA / Apollo 17 crew, *The Blue Marble* (AS17-148-22727), public
  domain — see `data/PHOTO-SOURCE.md`.
