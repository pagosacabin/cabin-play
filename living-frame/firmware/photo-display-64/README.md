# living-frame — rung 2: 64-color photo display (FabGL)

Rung 2 of the living-frame project (Christmas gift): the **same Blue Marble
photo as rung 1**, full-screen at **320×240 in 64 colours — 2 bits per
channel** — on a VGA monitor, driven by **FabGL 1.0.9** on a classic
ESP32-WROOM-32 devkit.

Rung 0 proved the display path with colour bars. Rung 1 put one JPEG on screen
in 8 colours and read as a *dot puzzle*. This rung keeps the picture and
multiplies the colour depth by 8, using the 2-bit-per-channel DAC that the
existing VGA breakout module already carries. No SD card, no WiFi, no
slideshow — those are later rungs.

```
photo-display-64/
├── platformio.ini           PlatformIO project (env: esp32dev, fabgl@1.0.9 pinned)
├── src/main.cpp             the firmware: VGA init + decode + hold the frame
├── src/photo_render64.h     TJpgDec glue + ladder-aware 4-level quantiser + 8x8 Bayer
├── data/photo.jpg           rung 1's exact source photo (byte-identical), embedded
├── data/PHOTO-SOURCE.md     provenance and licence of that photo
├── tools/render_check.cpp   host-side render check (same code path, PC pixel sink)
├── tools/render-check.png   that check's output — what the frame buffer holds
│                            (render-check.ppm is the same image, raw; generated,
│                             git-ignored)
├── build-esp32dev.log.txt   build + flash log for the esp32dev target
└── .gitignore
```

## Agreed pin map (FIXED — Stephen and Juno, 2026-11-12; Stephen has wired to this)

| Signal | GPIO | Board landing |
|---|---|---|
| Red MSB (heavy) | 22 | R, ~270 Ω path → VGA pin 1 |
| Red LSB (weak) | 21 | R, ~470 Ω path → VGA pin 1 |
| Green MSB (heavy) | 19 | G, ~270 Ω path → VGA pin 2 |
| Green LSB (weak) | 18 | G, ~470 Ω path → VGA pin 2 |
| Blue MSB (heavy) | 27 | B, ~270 Ω path → VGA pin 3 |
| Blue LSB (weak) | 4 | B, ~470 Ω path → VGA pin 3 |
| HSync | 23 | H, 68 Ω path → VGA pin 13 |
| VSync | 33 | V, 68 Ω path → VGA pin 14 |

This is FabGL's 64-color default map with the two strapping pins moved: Blue
MSB 5 → 27 and VSync 15 → 33. All other signals sit at FabGL defaults.

FabGL's `VGAController::begin()` signature is
`begin(red1, red0, green1, green0, blue1, blue0, hsync, vsync)` in which `1`
is the heavy (MSB) bit, so the call is
`begin(GPIO_NUM_22, GPIO_NUM_21, GPIO_NUM_19, GPIO_NUM_18, GPIO_NUM_27, GPIO_NUM_4, GPIO_NUM_23, GPIO_NUM_33)`.
There is no `VGA64Controller` class in FabGL 1.0.9 — `VGAController` *is* the
64-colour bitmapped controller; the 8-argument overload of its `begin()` is the
documented way to supply custom pins (see `vgabasecontroller.h:166` and the
class docs in `vgacontroller.h`). The agreed map is therefore a supported
configuration of the pinned library, not a source patch.

## Build and flash

```bash
cd living-frame/firmware/photo-display-64
pio run                  # build
pio run -t upload        # flash (or -t upload --upload-port /dev/ttyUSB1)
pio device monitor       # 115200
```

## Why 320×240

One byte per pixel at 64 colours, so the frame buffer is 76,800 bytes of
DMA-capable internal RAM. 640×480 would need 307,200 bytes — more than the
classic ESP32 has — and 400×300 would need 120,000 and leave the decoder very
little room. 320×240 is a standard 4:3 mode on this controller
(`QVGA_320x240_60Hz`), and the 640×480 source divided by two is exactly
320×240, so TJpgDec's 1/2 descale fills the screen with the whole frame: no
crop, no letterbox, no rescale artefacts.

The trade against rung 1 is deliberate and visible: half the linear
resolution in exchange for eight times the colour depth. Rung 1's complaint was
banding ("dot puzzle"), which comes from colour depth, not from pixel count.

## How 64 colours are actually used

The DAC is **not** a uniform 2-bit DAC. Each channel is a two-path resistor
ladder (270 Ω heavy bit, 470 Ω weak bit) into the monitor's 75 Ω termination,
so the four codes produce approximately **0.00, 0.452, 0.714, 1.000** of full
scale rather than the even quarters 0.00/0.33/0.67/1.00. The quantiser derives
those fractions from the resistor values at startup (`photo64Ladder()`) and
dithers between the *real* levels, so the average output voltage tracks the
source pixel; an even-quarter quantiser would render the shadows too bright.

Dithering is 8×8 ordered (Bayer), applied per channel with a different matrix
offset per channel. Sharing one threshold across R, G and B correlates them —
all three step up or down together, the frame collapses onto the grey diagonal
and uses only 22 of the 64 entries on this photo. Independent offsets give 24
on this photo and keep the dither from pulling hue towards grey.

The photo is a near-neutral subject (space → ocean → cloud), so it cannot
exercise all 64 entries by itself: plain nearest-level quantisation of the
source reaches only **17** of 64, with dither **24**. The pipeline can address
all 64 — the host check proves that with a synthetic colour lattice — so the
24 is a property of the picture, not a limit of the firmware.

## Verification

`tools/render_check.cpp` compiles the **same** `src/photo_render64.h` the
firmware uses and swaps only the pixel sink, so it exercises the real decode,
quantise and dither code. It reports the palette census and, because per-pixel
error is meaningless for dithered output, the fidelity of *block averages*:

```
pixels drawn: 76800 (expected 76800)
distinct colors in frame: 24 of 64 (counts 1..19478)
block RMS error 8x8 blocks  (0..255): R 2.63 G 2.57 B 2.46
block RMS error 2x2 blocks  (0..255): R 11.77 G 10.56 B 11.17
luma NCC 40x30: 0.9997   (source rolled 10 px: -0.0245)
luma NCC 160x120: 0.9957
palette reachability (synthetic lattice): 64 of 64
```

Build and run:

```bash
gcc -O2 -I../photo-display/lib/TJpgDec -c ../photo-display/lib/TJpgDec/tjpgd.c -o /tmp/tjpgd64.o
g++ -O2 -I../photo-display/lib/TJpgDec -Isrc tools/render_check.cpp /tmp/tjpgd64.o -o /tmp/render_check64
/tmp/render_check64 data/photo.jpg tools/render-check.ppm
magick tools/render-check.ppm -strip tools/render-check.png
```

This is a render check, not an observation of the VGA output: no VGA display
or capture device is attached to the build host.

## Shared source with rung 1

`data/photo.jpg` is a byte-identical copy of rung 1's file
(sha256 `dc6de88234c4900dbbc1df2dac679f10afdcbecd01668218157d6df70192a464`),
so the two rungs are directly comparable on screen. It is copied rather than
referenced through `../photo-display/data/` because PlatformIO derives the
embedded symbol name from the path inside the project. The JPEG decoder itself
is reused in place from `../photo-display/lib/TJpgDec` via `lib_extra_dirs`, so
both rungs build identical decoder source.
