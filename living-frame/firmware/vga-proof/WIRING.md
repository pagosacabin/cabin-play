# Wiring — living-frame rung 0 VGA proof

Everything the ESP32 needs to drive are **five signal pins and a ground**. The
resistor DAC is *not* something you have to build: it lives on the adapter.

## 1. The connection table

| ESP32-WROOM-32 pin | Signal | Adapter header label | VGA DB15 pin (direct route) | Series resistor |
|---|---|---|---|---|
| **GPIO 14** | RED   | `R`            | 1  | 270 Ω (on the adapter) |
| **GPIO 19** | GREEN | `G`            | 2  | 270 Ω (on the adapter) |
| **GPIO 27** | BLUE  | `B`            | 3  | 270 Ω (on the adapter) |
| **GPIO 32** | HSYNC | `HS` / `HSYNC` | 13 | none — direct |
| **GPIO 33** | VSYNC | `VS` / `VSYNC` | 14 | none — direct |
| **GND**     | GND   | `G` / `GND`    | 5, 6, 7, 8, 10 | none |

Those five GPIO numbers are the library's own standard classic-ESP32 set
(ESP32Lib `PinConfig` `VGAv01` / `VGABlackEdition`, whose 3-bit view is
`r4/g4/b3` + `hSync/vSync` = 14/19/27/32/33). The firmware repeats them as named
constants at the top of `src/main.cpp`, so the drawing and this table cannot
drift apart. **Do not renumber them without editing `src/main.cpp`.**

## 2. Route A — Stephen's PCB_0024_VGA_ADAPTER (preferred)

The adapter already carries the 270 Ω series resistors on R/G/B and takes
HSYNC/VSYNC straight through to its DB15. So on this route there is no ladder to
build: the ESP32 sits on the adapter's header and only the five signal pins plus
ground have to line up, per the table above.

The adapter pin the ESP32 is plugged into is the one the firmware is written
for; the values are the ones in the table.

> **DECLARED GAP — the JP1 pin-by-pin mapping was not available to me, and the
> task said to say so rather than guess.**
> `PCB_0024_VGA_ADAPTER v1` could not be found in any place this run can read:
> not on `nordtronics` `main` or any of its branches, not in `cabin-play`, not
> anywhere on this host, not in `pagosacabin/ESP32_projects`, and GitHub code
> search for `PCB_0024` / `JP1` / `DB15` returns only this task file itself. So
> the table identifies each connection by the adapter header's **signal label**
> (`R` / `G` / `B` / `HS` / `VS` / `GND`), not by a JP1 position number.
>
> Two ways to close the gap in a minute, both without the schematic:
> * If the header is silkscreened with signal names, the table is already
>   unambiguous — match by name.
> * If it is silkscreened with numbers, the adapter is passive, so each input
>   pin maps 1:1 to its DB15 pin: put a multimeter in continuity mode between a
>   JP1 pin and the corresponding DB15 pin (1 = R, 2 = G, 3 = B, 13 = HSYNC,
>   14 = VSYNC, 5–10 = GND) and label it. Ten seconds per pin.

### If the adapter's R/G/B are two-rung ladders (the "270/470" note)

A 2-bit-per-colour DAC has two input pins per channel (usually 270 Ω for the
high bit and 470 Ω for the low bit, or vice versa). This firmware drives **one
output pin per colour**, so:

* connect it to the rung that gives full scale, and
* leave the other rung unconnected (it contributes nothing).

Which rung is which is arithmetic, not guesswork. The TV terminates each colour
line with 75 Ω, so the output voltage is `3.3 V × 75 / (R + 75)`:

| Rung resistor | Into the TV's 75 Ω | Result |
|---|---|---|
| 270 Ω | `3.3 × 75/345` = **0.72 V** | full-scale colour (VGA wants 0.7 V) |
| 470 Ω | `3.3 × 75/545` = **0.45 V** | ~60 % colour — visibly dim |

So: **connect the ESP32 to the 270 Ω rung.** If R, G and B all come out dim but
the picture is otherwise correct and stable, you are on the 470 Ω rung — that no
longer falsifies the display path, but move the wires to the 270 Ω rung for a
correct-level picture.

## 3. Route B — straight to a DB15 (fallback, if the adapter mapping stays unclear)

Any male DB15 breakout (screw-terminal ones cost a few dollars). Wire per the
table above with **270 Ω in series with GPIO 14, 19 and 27**, syncs and ground
direct. That is the standard passive VGA hack; nothing else is needed.

Notes that matter on this route:

* The ESP32's 3.3 V CMOS syncs are accepted by essentially every TV/monitor,
  including this Magnavox. If a particular TV refuses to lock, a 68–100 Ω series
  resistor on each sync line is the usual cure — don't reach for a level shifter.
* Do **not** feed the ESP32 from the VGA connector's pin 9 (+5 V). Power the
  devkit from its own USB cable. Grounds are commoned through the VGA cable.

## 4. Why the picture is 3-bit colour (8 colours), not more

The task asks for 640 × 480. On a classic ESP32 the frame buffer has to live in
the internal 320 kB SRAM:

| Colour depth | Frame buffer at 640×480 | Fits? |
|---|---|---|
| 14-bit (R5G5B4) | 614 kB | no |
| 6-bit (R2G2B2) | 307 kB | no |
| **3-bit (R1G1B1)** | **154 kB** (2 pixels/byte, unpacked by the I2S interrupt) | **yes** |

So 640×480 is only reachable in 3-bit mode. That is not a handicap for this
proof: the classic eight-bar colour-bar pattern is *exactly* eight colours —
white, yellow, cyan, green, magenta, red, blue, black — so every bar in the
picture is at full scale and nothing is being approximated.

If you later want more colours, the trade is resolution: 320×200 fits 6-bit
(64 colours) comfortably. Change `MODE640x480` in `src/main.cpp` and set
`H_RES`/`V_RES` to match the new mode, and the bar layout follows.

## 5. Power-up sequence that avoids false alarms

1. Wire everything with the ESP32 **unpowered**.
2. Connect the VGA cable to the Magnavox and select the **PC / VGA** input.
3. Set the TV's picture size to **"Just scan" / "1:1" / "Screen fit"**, not
   "16:9" — otherwise the TV crops the frame and the four corner alignment
   marks in the pattern get cut off (a cropped mark means "the TV is scaling",
   not "the firmware is wrong").
4. Plug the ESP32's USB cable into the laptop, flash, then watch.

## 6. Flash instructions

```bash
cd living-frame/firmware/vga-proof
pio run                      # build only
pio run -t upload            # build + flash, auto-detects the port
pio run -t upload --upload-port /dev/ttyUSB0   # if there are several ports
pio device monitor -b 115200 # optional: the firmware prints its pin map
```

In VS Code: install the **PlatformIO IDE** extension, *File → Open Folder* on
`living-frame/firmware/vga-proof`, then use **PlatformIO: Upload** (the → arrow
in the status bar). The first build downloads the `espressif32` platform and the
Xtensa toolchain (a few hundred MB, once).

The port must be a **USB-serial (CP2102/CH340)** port on a classic ESP32 devkit
(`/dev/ttyUSB0` on Linux, `COM5`-ish on Windows). A board that enumerates as a
native USB-JTAG device (`303a:1001`) on `/dev/ttyACM0` is an ESP32-S3/C3 and is
not the board this firmware targets.

Expected serial output at 115200 baud:

```
living-frame rung 0 -- VGA proof
pins: R=14 G=19 B=27 HSYNC=32 VSYNC=33
frame drawn, free heap: <~100 kB> bytes
```

## 7. What a correct picture looks like

* Eight equal vertical bars across the top three quarters, left to right:
  white, yellow, cyan, green, magenta, red, blue, black.
* Below them, a dark grey band carrying two lines of white text:
  `NORDTRONICS  LIVING-FRAME` and `VGA PROOF  -  640x480  -  ESP32`.
* Four small white squares, one pair at the very top corners and one pair on the
  bottom edge of the bars.
* Completely static — nothing moves, flickers or tears.

## 8. Troubleshooting

| Symptom | First thing to check |
|---|---|
| "No signal" on the TV | TV input is PC/VGA; ESP32 GND really connected; sync pins not swapped |
| Correct bars but one colour missing/dim | that colour's wire, or you are on the 470 Ω ladder rung |
| Colours wrong (e.g. yellow where cyan belongs) | R/G/B wires crossed — swap them, don't edit the firmware |
| Rolling, tearing, or the picture drifts | a sync wire, or a missing ground |
| Picture present but cropped | TV picture-size set to "Just scan"/1:1 |
| Half the picture black / torn | marginal timing at 640×480 on this TV — try another of the library's modes (`MODE640x400`, `MODE800x600`) with `H_RES`/`V_RES` updated |
| Nothing at all, and the serial log stops before "frame drawn" | `VGA init failed` — the pins are correct, so it is a power/flash problem |
