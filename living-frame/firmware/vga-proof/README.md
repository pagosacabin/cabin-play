# living-frame — rung 0: VGA proof firmware

Proof-of-display-path firmware for the living-frame project (Christmas gift):
a **classic ESP32** driving a 640×480 VGA colour-bar test pattern into the
Magnavox 22MD311B/F7 through its VGA / PC input.

Nothing else in the project starts until this rung shows a stable, correctly
coloured picture. This directory is that rung and nothing more.

```
vga-proof/
├── platformio.ini          PlatformIO project (env: esp32dev)
├── src/main.cpp            the firmware: colour bars + text label + corner marks
├── WIRING.md               THE WIRING SPEC — read this before touching a wire
├── build-esp32dev.log.txt  build log excerpt for the esp32dev target
└── lib/ESP32Lib/           vendored bitluni ESP32Lib (see its NOTICE.md)
```

## Build and flash

```bash
cd living-frame/firmware/vga-proof
pio run                       # build
pio run -t upload             # flash (auto-detects the port)
pio device monitor -b 115200  # optional; the firmware prints its pin map
```

In VS Code, install the **PlatformIO IDE** extension and use *PlatformIO:
Upload* on this folder. The first build downloads the `espressif32@6.12.0`
platform and the Xtensa toolchain once (a few hundred MB).

Verified locally on 2026-10-15:

```
PLATFORM: Espressif 32 (6.12.0) > Espressif ESP32 Dev Module
HARDWARE: ESP32 240MHz, 320KB RAM, 4MB Flash
RAM:   [=         ]   7.1% (used 23128 bytes from 327680 bytes)
Flash: [==        ]  24.0% (used 315009 bytes from 1310720 bytes)
========================= [SUCCESS] Took 6.18 seconds =========================
```

(Release `v0.4.0` of ESP32Lib is Creative Commons Attribution-ShareAlike 4.0 —
attribution and the exact list of changes to it are in
`lib/ESP32Lib/NOTICE.md`.)

## Wiring

See **[WIRING.md](WIRING.md)** — the connection table, the adapter route, the
direct-DB15 fallback, the resistor values and the troubleshooting table all live
there. In one line:

| ESP32 | 14 | 19 | 27 | 32 | 33 | GND |
|---|---|---|---|---|---|---|
| VGA | R (1) | G (2) | B (3) | HSYNC (13) | VSYNC (14) | 5–10 |

…with 270 Ω in series on R/G/B (those resistors are already on Stephen's
PCB_0024 adapter).

## Why these choices

* **Library: bitluni's ESP32Lib**, not FabGL. ESP32Lib is the library the
  classic-ESP32 VGA community has used for this exact job for years (VGA out
  over I2S); it is small, and its 3-bit driver keeps a 640×480 frame buffer
  inside the ESP32's 320 kB internal SRAM. FabGL would need more of the chip to
  do the same, and has no advantage for a static colour bar.
* **3-bit colour (8 colours).** Forced by memory: a 640×480 frame buffer is
  614 kB at 14-bit and 307 kB at 6-bit, and the classic ESP32 only has 320 kB
  in total. At 3-bit it is 154 kB and fits. It costs nothing here — the classic
  eight-bar pattern *is* eight colours.
* **Vendored library with a two-line patch.** Upstream's `I2S_ESP32.cpp` calls
  the ESP-IDF 3.x RTC clock API, which no longer exists in the ESP-IDF 4.4
  inside arduino-esp32 2.x, so upstream does not compile against a current
  PlatformIO install. The fix is the exact call already commented out one line
  above the broken one; it is applied in `lib/ESP32Lib/src/I2S/I2S_ESP32.cpp`
  and documented in `lib/ESP32Lib/NOTICE.md`.
* **Pin numbers are the library's own** classic-ESP32 set (`PinConfig VGAv01` /
  `VGABlackEdition`), repeated as named constants in `src/main.cpp` so that the
  firmware and WIRING.md cannot drift.

## Status of the proof

* Builds cleanly for the classic ESP32 target — verified, log above.
* Wiring spec — complete for the signals and the resistor values, with one
  declared gap: Stephen's `PCB_0024_VGA_ADAPTER v1` schematic was not reachable
  from here, so the adapter's **JP1 position numbers** are not in the table;
  connections are named by signal instead. WIRING.md §2 gives the two ways to
  close that gap in a minute (silkscreen names, or a continuity check against
  the DB15 pins).
* **On-screen result: not yet observed here, and it cannot be.** No VGA display
  and no classic ESP32 are attached to this host — the two Espressif boards
  plugged in are ESP32-S3 devices (`303a:1001`), which are not the VGA target.
  Stephen's photo of the Magnavox is the acceptance evidence for this rung.
