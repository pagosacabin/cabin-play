# n16r8-vga64 — task 0226

The 0180 ship build's driver path, moved to the wired N16R8 board, with the
framebuffer grown from 4 bpp in internal RAM to **8 bpp (64 colours) in PSRAM**
at the same full native 640x480.

**Nothing is doubled.** One framebuffer pixel is one DAC byte is one output
pixel — 640x480 native, no pixel doubling, no 320x240 upscale, no line skipping.
0179's doubled mode and 0177's FabGL build are superseded and are not referenced
in this port.

## What it draws

A diagnostic test card, held on screen indefinitely (Stephen judges the panel
with his own eyes, exactly as with 0180):

- separate **R, G and B ramps**, each 4 steps and each labelled by channel, so a
  swapped channel or a swapped bit inside a channel is visible immediately
- a **full 64-colour grid** (`r + 4g + 16b`, RGB222), so a bit-order swap
  reorders it visibly
- a **1-pixel checkerboard** — the pixelation test
- **native-resolution small text** (1 fb pixel = 1 output pixel)

Content is drawn on a 480x640 portrait canvas and rotated 90° clockwise into the
640x480 landscape raster, the same convention as 0180 (monitor pivoted
counter-clockwise).

## Wiring (agreed map, wired by Stephen 2026-11-20)

| DAC byte bit | Pin | Signal |
| --- | --- | --- |
| bit0 | GPIO 5 | R LSB |
| bit1 | GPIO 4 | R MSB |
| bit2 | GPIO 7 | G LSB |
| bit3 | GPIO 6 | G MSB |
| bit4 | GPIO 16 | B LSB |
| bit5 | GPIO 15 | B MSB |

HSYNC GPIO 17, VSYNC GPIO 18, both active-low. pclk/DE unrouted, as in 0180 — a
VGA DE-15 carries RGB + syncs only. Bits 6–7 unused.

0225 proved these 12 pads are live GPIOs but did **not** prove the silkscreen
positions. If the card shows a wrong colour channel or a swapped bit pair, that
is a wiring/label question to report and to fix at the bench — not to compensate
in code (task constraint).

## Reported numbers (firmware's own output, 2026-11-20)

| Item | Value |
| --- | --- |
| Output | **640x480 native**, 1 fb pixel = 1 DAC byte = 1 output pixel |
| Colour | **8 bpp = 64 colours**, RGB222 through the 6-bit DAC |
| Framebuffer | **307,200 B in PSRAM**, pointer `0x3c030998`, `heap_caps_check_integrity` ok |
| Pixel clock | 25,000,000 Hz (PLL160M), h 96+48+640+16, v 2+33+480+10 → **59.52 Hz configured** |
| Measured refresh | **357 vsync in 5.999 s → 59.51 Hz** |
| Bounce fills | 10,714 in the same window → **30.0 per frame**, matching 480 lines / 16-line buffer |
| Sync | hsync GPIO17, vsync GPIO18, active-low |

### What the card should look like, in numbers

The card asserts nothing by itself — Stephen judges it by eye — but these are the
DAC bytes behind each element, so a swap can be stated as a comparison rather
than a memory of the screen (the firmware logs the same lines):

| Element | Expected DAC bytes |
| --- | --- |
| Red ramp, steps 0-3 | `0x00 0x01 0x02 0x03` |
| Green ramp, steps 0-3 | `0x00 0x04 0x08 0x0c` |
| Blue ramp, steps 0-3 | `0x00 0x10 0x20 0x30` |
| 64-colour grid cell (r,g,b) | `(b<<4) | (g<<2) | r` at index `r + 4g + 16b` |
| 1-pixel checker | alternation `0x00` / `0x3f`, 120x120 canvas pixels |

A channel swap shows as the wrong bar lighting up; an MSB/LSB swap inside a
channel shows as non-monotonic steps (e.g. `0x00 0x02 0x01 0x03` on red).

The measured figures come from the driver's own vsync and bounce-empty callbacks,
so they are independent of what the configuration says: the LCD_CAM really is
scanning 30 bounce buffers per frame at 59.5 Hz, with the framebuffer being read
out of PSRAM in the fill ISR.

## Build, flash, read the numbers

```bash
pio run -e n16r8-vga64
pio run -e n16r8-vga64 -t upload          # /dev/ttyUSB0
esptool --port /dev/ttyUSB0 --baud 460800 read_flash 0x310000 0x10000 report.bin
python3 -c "import struct;d=open('report.bin','rb').read();m,n=struct.unpack('<II',d[:8]);print(d[8:8+n].decode())"
```

The flash read-back exists because the FT232R bridge on this bench delivers no
usable console (0225 finding): with the app running, `read()` returns a fixed
`00 80 78` cycle at every baud while esptool still talks to the chip in both
directions. The report partition at `0x310000` is the authoritative log.

## Peer review (Copilot, 2026-11-20, reviewed at f7600a8)

Five findings, none blocking:

1. **1:1 raster path** (Low) — no doubling, upscaling or line-skip anywhere:
   `bounce_fill()` is a plain `memcpy` from the 307,200 B framebuffer.
2. **DAC packing / GPIO order** (Low) — matches the agreed map exactly.
3. **PSRAM read inside the bounce ISR** (Medium) — plausible but not formally
   proven safe: it relies on the data cache being enabled and `memcpy` being
   ISR-safe for cached PSRAM. Accepted and now stated in the firmware output and
   here; the failure symptom would be a bounce underrun or horizontal
   corruption, not a wrong size. The measured 30.0 fills/frame at 59.5 Hz is the
   evidence it is currently holding.
4. **Swap detection** (Medium) — the card catches direct pin swaps and gross
   misrouting, but it is a visual judgement, not a machine assertion. Accepted:
   the expected DAC bytes are now logged (table above) and the reply says plainly
   that the panel is Stephen's to judge.
5. **Canvas→raster mapping** (Low) — correct 90° clockwise.

## Files

| File | Purpose |
| --- | --- |
| `src/main.c` | LCD_CAM RGB driver, 8bpp PSRAM framebuffer, test card, report |
| `src/font5x7.h` | 5x7 column font for the card's labels |
| `partitions.csv` | adds the `report` partition at `0x310000` |
| `sdkconfig.defaults` | 16 MB flash, octal PSRAM 80 MHz, UART0 console, 240 MHz |
| `report-2026-11-20.txt` | read-back report for this run |
