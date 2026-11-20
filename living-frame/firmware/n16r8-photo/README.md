# n16r8-photo — task 0227

One photograph, full-screen, on the wired N16R8 board: 0226's driver (0180's
LCD_CAM path, native 640x480, 8 bpp RGB222, framebuffer in PSRAM, 16-line bounce
buffers, nothing doubled) with the test card replaced by a dithered photo that
ships inside the app image.

## Source photograph

`living-frame/firmware/photo-display-s3/data/parrot.jpg` — 640x480, the
**LORIKEET** test image (`IMG16_LORIKEET` in 0180, `IMG_LORIKEET` in 0179, and
the subject of the 0179 doubled-mode samples Stephen judged). It was the first
photograph phase in both earlier rungs; the second test image,
`data/photo.jpg` (BLUE_MARBLE), is untouched. Nothing was substituted — if 0227
meant the globe instead, it is a one-argument change:
`--source ../photo-display-s3/data/photo.jpg`.

## Conversion (`tools/convert_photo64.py`)

1. scale-to-cover 480x640 portrait, centre-crop — same rule as the 0180 pipeline,
   so the framing is comparable to what Stephen already judged
2. sRGB -> linear light (2.2), because dithering in code space speckles shadows
   (0177 round-2 finding)
3. **Floyd–Steinberg error diffusion** in linear light over the 64 RGB222 colours.
   The nearest-colour test is squared error in linear RGB weighted by the
   luminance coefficients (0.2126/0.7152/0.0722) — a heuristic that favours
   luminance accuracy, not a perceptual luminance/chroma metric (peer review was
   right to pin that wording down)
4. pack each pixel as the board's DAC byte, `(b<<4)|(g<<2)|r`, 2-bit fields in
   (MSB,LSB) order — the same packing 0226's `dac()` uses
5. rotate the portrait canvas into the landscape raster **90° counter-clockwise**,
   i.e. flipped 180° from 0226's test card, per Stephen's amendment: his monitor is
   pivoted the other way, so the photo reads upright from where he sits

The tool writes `src/photo64_data.h` (the array the firmware embeds),
`src/photo64_meta.h` (source, size, SHA-256), and two previews:
`*-raster.png` is exactly what the panel receives, `*-as-seen.png` is that
rotated back to the portrait Stephen should see.

## 0228 — gamma-space dither test (ONE variable)

Stephen's verdict on 0227 was "it's still pixelated", and his close-ups showed why:
in dark areas the colour was built from rare isolated single pixels. At 2 bits per
channel the first step above black is 1/3 of full scale, and 0227 dithered in
**linear light**, which makes those steps perceptually even coarser and starves the
shadows.

0228 changes exactly one thing: the dithering working space, linear -> **gamma
(sRGB code) space**. Same source, same cover-crop, same rotation, same orientation,
same pins, same timings, same firmware path. `tools/convert_photo64.py --space`
selects it; `--space linear` still reproduces 0227's bytes exactly
(sha256 `5ee7c64e…16cd335`), which is the check that nothing else moved.

Shadow coverage, over the darkest quartile of source-luminance pixels (76,816 of
307,200 canvas px, `tools/shadow_coverage.py`):

| Rendering | cover% | mean DAC byte | isolated dots | mean non-black neighbours |
| --- | --- | --- | --- | --- |
| 0227 linear | **1.47** | 0.06 | 98.9% | 0.16 |
| 0228 gamma | **21.36** | 1.28 | 49.8% | 1.56 |

Dark-region dot coverage rises **14.5x** (1.47% -> 21.36%) and the share of
isolated specks halves (98.9% -> 49.8%): the starfield signature the photos showed
is exactly what these numbers describe. Dithering in gamma space also reduces
pure-black pixels overall from 155,515 to 93,299.

Whether that reads as better on the panel is Stephen's call — the diagnosis is a
hypothesis until he says so.

## Result on the panel (firmware's own report, 2026-11-20)

| Item | Value |
| --- | --- |
| Source | `parrot.jpg`, 640x480 -> 480x640 portrait cover-crop |
| Colours | 64 (RGB222), Floyd–Steinberg, 47 of the 64 DAC bytes used |
| Framebuffer | **307,200 B in PSRAM** at `0x3c090998`, heap integrity ok |
| Embedded image | 307,200 B, `0x3c0381b4` in the app image |
| **SHA-256 check** | 0228 (gamma) runtime `f8dded81…97d9c` **==** tool hash → **MATCH**, unchanged after 6.0 s of streaming (0227's linear run was `5ee7c64e…16cd335`) |
| Output | 640x480 native, 1 fb pixel = 1 DAC byte = 1 output pixel |
| Timing | 25 MHz pclk, h 96+48+640+16, v 2+33+480+10 → 59.52 Hz configured |
| Measured | **357 vsync in 5.998 s → 59.52 Hz**; 10,713 fills → **30.0 per frame** |
| Rotation | 90° counter-clockwise (180° from the 0226 test card) |

What the SHA-256 comparison proves, precisely: the bytes the app holds and streams
match the converter's output, and they still match after 6 s of continuous
scanout, so nothing wrote into the framebuffer while the DMA was reading it. It
does **not** measure the electrical DAC output, and it cannot detect a fault after
the check — so "the panel is showing this photograph" rests on the hash plus the
vsync/fill counters, and on Stephen's eyes for the picture itself.

## Rebuild

```bash
python3 tools/convert_photo64.py \
    --source ../photo-display-s3/data/parrot.jpg --out src/photo64_640x480.bin
pio run -e n16r8-photo
pio run -e n16r8-photo -t upload
esptool --port /dev/ttyUSB0 --baud 460800 read_flash 0x310000 0x10000 report.bin
```

The report partition exists because the FT232R console on this bench delivers no
usable output (0225 finding); the read-back is the authoritative log, as 0225/0226
established and 0227's Proof section allows.

## Peer review (Copilot, 2026-11-20, reviewed at 39283d0)

Five findings, one Medium:

1. **Rotation / previews** (Low) — the counter-clockwise mapping is 180° opposite
   0226's clockwise card, and the previews match what is scanned out.
2. **Floyd–Steinberg + RGB222** (Low) — weights (7/16, 3/16, 5/16, 1/16) and the
   four-level candidates are correct; the distance is luminance-weighted squared
   error in linear RGB, a heuristic rather than a perceptual metric. README
   wording corrected.
3. **DAC packing** (Low) — tool and firmware agree with the agreed pin map.
4. **Runtime SHA-256** (Medium) — proves the copy matches the converter and is not
   disturbed during scanout; it does not prove electrical output. Accepted: the
   firmware now re-hashes after the 6 s window (`unchanged`), and the README says
   exactly what the hash does and does not cover.
5. **Const image + PSRAM copy** (Low) — fits the 3 MB app partition, no extra
   writable RAM.

## Files

| File | Purpose |
| --- | --- |
| `tools/convert_photo64.py` | the conversion pipeline above |
| `src/main.c` | driver + photo load + SHA-256 check + report |
| `src/photo64_data.h` | generated 307,200 B framebuffer array |
| `src/photo64_meta.h` | generated source/size/SHA-256 |
| `src/photo64_640x480-as-seen.png` | what Stephen's eye should see |
| `src/photo64_640x480-raster.png` | exactly what the panel receives |
| `report-2026-11-20.txt` | the device's own read-back report |
| `partitions.csv`, `sdkconfig.defaults` | same as 0225/0226 |
