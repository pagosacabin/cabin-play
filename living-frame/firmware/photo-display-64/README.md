# living-frame — rung 2: 64-color photo display (FabGL)

Rung 2 of the living-frame project (Christmas gift): full-screen **320×240 in
64 colours — 2 bits per channel** — on a VGA monitor, driven by **FabGL 1.0.9**
on a classic ESP32-WROOM-32 devkit.

Rung 0 proved the display path with colour bars. Rung 1 put one JPEG on screen
in 8 colours and read as a *dot puzzle*. Rung 2 multiplied the colour depth by
8 using the 2-bit-per-channel DAC the existing VGA breakout already carries. No
SD card, no WiFi, no slideshow — those are later rungs.

**Round 2 (2026-11-12, after the first acceptance photo came back "same as
before, still a dot puzzle").** Three changes, all firmware — nothing rewired,
same pin map, same 320×240@60 mode:

1. a **diagnostic ramp screen** at boot, so a photograph can prove whether each
   channel's two bits arrive in the right order and at the right weight;
2. a **second photo with saturated colour** (a rainbow lorikeet) through the
   identical pipeline, so the palette is judged on a subject that uses it;
3. a **gamma-correct quantiser**, which removes the "speckled black" the round-1
   pipeline produced (numbers below).

```
photo-display-64/
├── platformio.ini               PlatformIO project (env: esp32dev, fabgl@1.0.9 pinned)
├── src/main.cpp                 VGA init, screen selection, decode + hold
├── src/photo_render64.h         TJpgDec glue + ladder-aware quantiser (both domains)
├── src/ramp_screen.h            diagnostic ramp layout, shared with the host preview
├── data/photo.jpg               rung 1's exact Blue Marble file (byte-identical), embedded
├── data/parrot.jpg              rainbow lorikeet (CC BY 4.0), 640×480, embedded
├── data/PHOTO-SOURCE.md         provenance and licence of both photos
├── tools/render_check.cpp       host-side render check (same code path, PC pixel sink)
├── tools/render-check.png       Blue Marble, gamma-correct pipeline
├── tools/render-check-lorikeet.png        lorikeet, gamma-correct pipeline
├── tools/render-check-legacy.png          Blue Marble, round-1 pipeline (A/B)
├── tools/render-check-lorikeet-legacy.png lorikeet, round-1 pipeline (A/B)
├── tools/ramp_preview.cpp       host render of the diagnostic ramp (same layout code)
├── tools/ramp-preview.png       what the ramp screen asks the DAC for
├── build-esp32dev.log.txt       build + flash log for the esp32dev target
└── .gitignore                   (.ppm intermediates are generated, not committed)
```

## Screens

Selected over the serial port at 115200 (CP2102 on the devkit); boot order
needs no terminal.

| Key | Screen |
|---|---|
| `1` | diagnostic ramp — per-channel 4 steps, grey ramp, smooth gradient |
| `2` | **default** lorikeet, gamma-correct pipeline |
| `3` | Blue Marble, gamma-correct pipeline |
| `4` | lorikeet, round-1 (voltage-domain) pipeline — A/B |
| `5` | Blue Marble, round-1 pipeline — A/B |

Boot: ramp held **30 s** (photograph it), then the lorikeet. Power-cycle to see
the ramp again; `q` cuts a wait short.

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

## The diagnostic ramp

Four patches per channel, left to right, in the order **0, LSB only, MSB only,
both**, then the same four levels on all three channels at once (W row), then a
dithered black→white gradient. Everything else is black, patches are 60 px wide
with 4 px gutters, and each row is labelled R/G/B/W at the left.

What the steps should measure, from the ladder (`tools/ramp_preview` prints it):

| level | bits | sRGB code sent to the DAC | luminance that code shows |
|---|---|---|---|
| 0 | 00 | 0 | 0 % |
| 1 | 01 (LSB only) | 115 | 18 % |
| 2 | 10 (MSB only) | 182 | 48 % |
| 3 | 11 (both) | 255 | 100 % |

A photograph with the steps **out of order** on one row but correct on the
others is a bit-order fault on that channel; steps in order but one row much
brighter or dimmer than its siblings is a wiring-weight (resistor) fault; steps
in order and evenly progressing on all three rows plus a grey row that looks
neutral is a correct DAC path, and any remaining complaint is the panel's
resolution/contrast rather than the digital side.

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

The trade against rung 1 is deliberate and visible: half the linear resolution
in exchange for eight times the colour depth.

## How 64 colours are actually used

The DAC is **not** a uniform 2-bit DAC. Each channel is a two-path resistor
ladder (270 Ω heavy bit, 470 Ω weak bit) into the monitor's 75 Ω termination,
so the four codes produce approximately **0.00, 0.452, 0.714, 1.000** of full
scale rather than the even quarters 0.00/0.33/0.67/1.00. The quantiser derives
those fractions from the resistor values at startup (`photo64Ladder()`).

That ladder is also why the shadows were the worst part of the picture: the
first step is 0.452 of full scale, i.e. sRGB code 115, which a panel driven
analogue (luminance ≈ voltage^2.2) shows as **18 % of full light** — a mid-grey
dot. Any shadow detail has to be drawn as a sprinkle of mid-grey dots on black,
which is exactly what "speckled black background" describes.

### Two quantiser modes

| mode | what it dithers to | use |
|---|---|---|
| `PHOTO64_MODE_LEGACY` | correct average **voltage** (round 1) | A/B only (keys 4, 5) |
| `PHOTO64_MODE_PERCEPTUAL` | correct average **light** (default) | screens 2, 3 |

The perceptual mode gamma-decodes the source byte first and then picks the
dither so the average *luminance* matches, using `PHOTO64_GAMMA = 2.2` as the
display model. The model is a standard assumption, not a measurement of the
213T panel, and it is one constant so it can be changed or reverted.

Dithering is 8×8 ordered (Bayer), applied per channel with a different matrix
offset per channel. Sharing one threshold across R, G and B correlates them —
all three step up or down together, the frame collapses onto the grey diagonal
and uses only 22 of the 64 entries. Independent offsets avoid that.

## Verification

`tools/render_check.cpp` compiles the **same** `src/photo_render64.h` the
firmware uses and swaps only the pixel sink, so it exercises the real decode,
quantise and dither code. It measures fidelity in both domains, because they
disagree and the disagreement is the point: *byte-domain* RMS is what round 1
optimised (average sRGB code), *light-domain* RMS is what the eye integrates.

| photo / mode | palette | light RMS 8×8 (R/G/B) | byte RMS 8×8 | near-black pixels lit |
|---|---|---|---|---|
| Blue Marble, legacy | 24/64 | 6.05 / 5.87 / 5.02 | 2.63 / 2.57 / 2.46 | 532 of 17,291 (3.1 %) |
| Blue Marble, perceptual | 24/64 | **2.36 / 2.46 / 2.75** | 14.67 / 13.53 / 9.96 | **1 of 17,291 (0.0 %)** |
| lorikeet, legacy | 40/64 | 6.87 / 6.96 / 6.51 | 2.31 / 2.47 / 2.43 | 2,240 of 14,277 (15.7 %) |
| lorikeet, perceptual | **45/64** | **1.67 / 1.53 / 1.29** | 17.98 / 18.50 / 17.73 | **30 of 14,277 (0.2 %)** |

Read the two RMS columns together: the perceptual dither is ~2.5–4× more
accurate in light and much *worse* in raw code averages, because matching
average codes is precisely what made round 1 look too bright. The black-floor
row is the direct measure of the speckle complaint.

Build and run:

```bash
gcc -O2 -I../photo-display/lib/TJpgDec -c ../photo-display/lib/TJpgDec/tjpgd.c -o /tmp/tjpgd64.o
g++ -O2 -I../photo-display/lib/TJpgDec -Isrc tools/render_check.cpp /tmp/tjpgd64.o -o /tmp/render_check64
/tmp/render_check64 data/photo.jpg tools/render-check.ppm perceptual
/tmp/render_check64 data/photo.jpg tools/render-check-legacy.ppm legacy
g++ -O2 -I../photo-display/lib/TJpgDec -Isrc tools/ramp_preview.cpp -o /tmp/ramp_preview -lm
/tmp/ramp_preview tools/ramp-preview.ppm
magick tools/render-check.ppm -strip tools/render-check.png
```

This is a render check, not an observation of the VGA output: no VGA display
or capture device is attached to the build host, so the screen itself is only
observable through Stephen's photograph — which is what the ramp and the photo
are for.

## Shared source with rung 1

`data/photo.jpg` is a byte-identical copy of rung 1's file
(sha256 `dc6de88234c4900dbbc1df2dac679f10afdcbecd01668218157d6df70192a464`),
so the two rungs are directly comparable on screen. It is copied rather than
referenced through `../photo-display/data/` because PlatformIO derives the
embedded symbol name from the path inside the project. The JPEG decoder itself
is reused in place from `../photo-display/lib/TJpgDec` via `lib_extra_dirs`, so
both rungs build identical decoder source.
