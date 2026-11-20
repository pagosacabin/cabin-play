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
3. **Floyd–Steinberg error diffusion** in linear light over the 64 RGB222 colours,
   distance weighted by luminance (0.2126/0.7152/0.0722) so luminance errors beat
   chroma errors
4. pack each pixel as the board's DAC byte, `(b<<4)|(g<<2)|r`, 2-bit fields in
   (MSB,LSB) order — the same packing 0226's `dac()` uses
5. rotate the portrait canvas into the landscape raster **90° counter-clockwise**,
   i.e. flipped 180° from 0226's test card, per Stephen's amendment: his monitor is
   pivoted the other way, so the photo reads upright from where he sits

The tool writes `src/photo64_data.h` (the array the firmware embeds),
`src/photo64_meta.h` (source, size, SHA-256), and two previews:
`*-raster.png` is exactly what the panel receives, `*-as-seen.png` is that
rotated back to the portrait Stephen should see.

## Result on the panel (firmware's own report, 2026-11-20)

| Item | Value |
| --- | --- |
| Source | `parrot.jpg`, 640x480 -> 480x640 portrait cover-crop |
| Colours | 64 (RGB222), Floyd–Steinberg, 47 of the 64 DAC bytes used |
| Framebuffer | **307,200 B in PSRAM** at `0x3c090998`, heap integrity ok |
| Embedded image | 307,200 B, `0x3c0381b4` in the app image |
| **SHA-256 check** | runtime `5ee7c64e…16cd335` **==** tool `5ee7c64e…16cd335` → **MATCH** |
| Output | 640x480 native, 1 fb pixel = 1 DAC byte = 1 output pixel |
| Timing | 25 MHz pclk, h 96+48+640+16, v 2+33+480+10 → 59.52 Hz configured |
| Measured | **357 vsync in 5.998 s → 59.52 Hz**; 10,713 fills → **30.0 per frame** |
| Rotation | 90° counter-clockwise (180° from the 0226 test card) |

The SHA-256 comparison is the point of the check: the app hashes the framebuffer
it actually streams and compares it with the converter's own hash, so "the panel
is showing the converted photograph" is measured rather than assumed. Whether it
looks good is Stephen's call, as with 0180/0226.

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
