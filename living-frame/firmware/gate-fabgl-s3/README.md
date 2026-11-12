# Gate: can FabGL drive 640×480 / 64 colours on the ESP32-S3? — **NO**

This directory is the step-1 gate for mailbox task **0178** (living frame rung 3).
It contains no application firmware. It answers one question, with the library
source, the compiler, and the board itself:

> Does pinned FabGL 1.0.9 support, on ESP32-S3: (a) the 64-colour bitmapped VGA
> controller, (b) custom pins via the documented overload, (c) a frame buffer in
> PSRAM at 640×480 — and does a minimal S3 VGA build compile for the Heltec target?

**Verdict: the gate fails on (a) and (c) outright, and (b) is moot because the
library does not compile for the target at all.** No firmware was written and the
alternative was not built (per the task's "stop" clause).

A second, independent blocker was found on the bench: **the Heltec board in hand
has no PSRAM** (0 bytes, verified in two PSRAM line modes), so criterion (c) is
physically unattainable on this part regardless of library choice.

---

## 1. Custom pins — present, documented (the one criterion that passes)

`src/dispdrivers/vgacontroller.h` (FabGL 1.0.9) documents the 8-GPIO overload:

```
* This example initializes VGA Controller with 64 colors (8 GPIOs used) and 640x350 resolution:
*     VGAController.begin(GPIO_NUM_22, GPIO_NUM_21, GPIO_NUM_19, GPIO_NUM_18,
*                         GPIO_NUM_5, GPIO_NUM_4, GPIO_NUM_23, GPIO_NUM_15);
```

`nativePixelFormat()` for `fabgl::VGAController` is `SBGR2222` — 6 bits of colour,
i.e. the 2-bits-per-channel resistor ladder on Stephen's breakout board. The
class is also documented as "64 colors". So the pin map and the colour depth are
a supported *configuration* on classic ESP32.

## 2. 64-colour VGA controller on ESP32-S3 — **not supported**

Two independent proofs.

**(a) It does not compile.** `logs/build-1-no-shim.log`:

```
src/fabutils.h:45:10: fatal error: soc/frc_timer_reg.h: No such file or directory
```

`soc/frc_timer_reg.h` is the ESP32 (classic) FRC timer register map; the S3
replaced the FRC timer with the LAC timer, so the header does not exist for
`esp32s3`. The include is unconditional in `fabutils.h` (line 45), i.e. the
library's *core utility header* has no S3 path. The library contains **zero**
target guards: `grep -rn "ESP32S3\|CONFIG_IDF_TARGET_ESP32S3\|ARDUINO_ARCH_ESP32S3"`
over `src/` returns 0 hits, and no `CONFIG_IDF_TARGET` at all.

**(b) Papering over that header does not get close.** `platformio-shim.ini`
puts a stub `soc/frc_timer_reg.h` on the include path (diagnostic only, values
never executed). `logs/build-2-with-shim.log`: build still fails with 17 errors,
now from the PS/2 driver and the ROM DMA descriptor:

```
ps2controller.cpp:1154: error: 'SENS_SAR_START_FORCE_REG' was not declared in this scope
ps2controller.cpp:1175: error: 'ETS_RTC_CORE_INTR_SOURCE' was not declared in this scope
ps2controller.cpp:1350: error: 'RTC_CNTL_SAR_INT_ST' was not declared in this scope
esp32/rom/lldesc.h:67: error: redefinition of 'struct lldesc_s'
```

**(c) The scanout mechanism itself does not exist on the S3.** FabGL generates
VGA by putting **I2S in LCD/parallel mode** — `src/devdrivers/swgenerator.cpp`
(lines 120-141, called from `VGABaseController::startGPIOStream()`):

```c
periph_module_enable(PERIPH_I2S1_MODULE);
...
// LCD mode
I2S1.conf2.val            = 0;
I2S1.conf2.lcd_en         = 1;
I2S1.conf2.lcd_tx_wrx2_en = 1;
```

The S3's `soc/i2s_struct.h` has **no `conf2` register at all**
(`grep -n "conf2\|lcd" .../esp32s3/.../soc/i2s_struct.h` → 0 hits). On the S3 the
parallel/RGB video peripheral is `LCD_CAM` (`soc/lcd_cam_struct.h` exists), a
different register block entirely. So this is not a header fix — the pixel path
itself has to be rewritten against LCD_CAM.

Upstream agrees: `fdivitto/FabGL` issue **#330** ("ESP32-S3 support -
soc/frc_timer_reg.h missing?") and discussion **#312** ("SUPPORT FOR ESP32S3
BOARDS?") both die at the same header, with no S3 port in the project.

## 3. Frame buffer in PSRAM at 640×480 — **not a FabGL capability**

- The VGA viewport is allocated with `MALLOC_CAP_DMA`:
  `vgacontroller.cpp:96-99` → `VGABaseController::allocateViewPort(MALLOC_CAP_DMA, m_viewPortWidth)`
  → `vgabasecontroller.cpp:448` `heap_caps_malloc(linesCount * rowlen, allocCaps)`.
  `MALLOC_CAP_DMA` is internal RAM on IDF 4.4; PSRAM is not returned by it.
- The library has **no PSRAM path at all**: `grep -rn "SPIRAM\|ps_malloc\|MALLOC_CAP_SPIRAM" src/`
  → 0 hits. (The only "PSRAM" string in the tree is a classic-ESP32 cache
  workaround macro in `fabutils.h:104`.)

So even a hypothetical S3 port of FabGL would keep the frame buffer in internal
RAM, not PSRAM.

## 4. Bench: the board in hand has **no PSRAM** (independent of FabGL)

Serial devices on the machine, `bench/identity-and-ports.log`
(esptool `chip_id`, read-only):

| Port | Chip | MAC | Note |
|---|---|---|---|
| `/dev/ttyUSB1` | **ESP32-S3 (QFN56) rev v0.2, 8 MB embedded flash (GD)** | `9c:13:9e:a1:37:40` | the Heltec (target) |
| `/dev/ttyUSB0` | ESP32-D0WD-V3 rev v3.1 | `84:1f:e8:30:a8:50` | a classic ESP32, **not** the 0177 board (`4c:c3:82:da:d5:68`) — not touched |
| `/dev/ttyACM0` | ESP32-C3 (QFN32) rev v0.4, 4 MB | `9c:cc:01:d0:64:6c` | a C3, not the target |

The S3 is on a CP2102 UART bridge (`/dev/ttyUSB1`), **not** the native USB port —
note that the Espressif USB-JTAG device that is plugged in is the C3.

Bench probe (`bench/probe-psram.cpp`, flashed to `/dev/ttyUSB1`; **this probe
overwrote whatever firmware the board previously held, declared in the reply**):

```
E (90) psram: PSRAM ID read error: 0x00ffffff, PSRAM chip not found or not supported, or wrong PSRAM line mode
chip model      : ESP32-S3
flash size      : 8388608 bytes
psram size      : 0 bytes
heap total      : 399188 bytes
heap free       : 373460 bytes
largest DMA blk : 335860 bytes
```

Rebuilt for octal PSRAM (`qio_opi`) as well, in case the part were an R8 variant —
`bench/console-boot-psram-octal.txt`:

```
E (72) opi psram: PSRAM ID read error: 0x00000000, PSRAM chip not found ...
E (72) spiram: SPI RAM enabled but initialization failed. Bailing out.
psram size      : 0 bytes
```

`ESP32-S3` with 8 MB *embedded* flash is the **ESP32-S3FN8** ordering code — flash,
no stacked PSRAM. The task's premise ("ESP32-S3R2 — 512 KB SRAM + 2 MB PSRAM") does
not match the part on the bench.

**Arithmetic that follows:** a 640×480 frame buffer at 1 byte/pixel is
**307,200 B** against a largest internal DMA block of **335,860 B** and a total
heap of **399,188 B** — 91 % of the only block that could hold it, leaving ~28 KB
for TJpgDec, the FreeRTOS stacks and everything else. Not viable. The 2-bit
ladder gives 64 colours ⇒ 1 byte/pixel; there is nowhere to put it on this board.

---

## Alternatives (evidence attached, **nothing built** — decision owed)

1. **LCD_CAM scanout + a PSRAM-equipped S3.** The S3's RGB panel peripheral is
   exactly the right engine, and there is a working precedent for this exact
   shape: PicoMite's `drivers/vga_lcdcam_s3/vga_lcdcam_s3.{c,h}` — "Generates a
   640x480@60 VGA signal out of the ESP32-S3 LCD_CAM (RGB panel) ... The
   peripheral owns a single PSRAM frame buffer of HRes*VRes bytes (one RGB332
   byte per pixel), then feeds LCD DMA through small internal-RAM bounce
   buffers." Its pin struct takes `data_gpio[8]` individually and allows `-1`
   for unused lines, so a **6-bit** ladder maps straight onto it. Two other
   Arduino-level references: `bitluni/ESP32-S3-VGA` (its own README warns
   "Higher resolution lose sync on PSRAM" and "Only compatible with 8MB psram
   version ESP32 S3 for high resolutions") and `Cvarc-Xtal/esp32s3-vga`
   (640×480 only, PSRAM double buffers, needs arduino-esp32 ≥ 3.1.0-RC1 — the
   official PlatformIO `espressif32` platform here pins 2.0.17, so that one
   would need the pioarduino platform).
   Cost: a new driver (~300-500 lines), no FabGL API (Canvas/fonts/VGAController
   all gone), plus **a board that actually has PSRAM** — a purchase, or a
   different part from the drawer. The 0177 render pipeline survives: the
   palette/dither/JPEG glue already lives behind a `put()` callback in
   `photo_render64.h`, so only the scanout end changes.
2. **Same board, 640×480 at 16 colours.** 640×480×4 bpp = 153,600 B, which does
   fit (largest DMA block 335,860 B). Same LCD_CAM driver work as (1), no
   purchase, but 16 colours instead of 64. Resolution — the thing Stephen
   objected to — is fixed.
3. **Stay on FabGL, classic ESP32, 640×480 at 16 colours.** No new driver, proven
   library and the existing wiring; `VGA4Controller` is the 4 bpp paletted
   controller. Rung 1 already ran 640×480 on that chip (3-bit), so the memory
   arithmetic is known-good. 16 colours, not 64.
4. **Port FabGL to the S3.** Not recommended: FRC timer, PS/2 sensor registers
   and the I2S LCD mode are all ESP32-only; upstream has no S3 port and this is
   fork-scale work, not a pin bump.

## Files

| Path | What it is |
|---|---|
| `platformio.ini`, `src/main.cpp` | minimal `fabgl::VGAController` bring-up on the agreed pin map (`logs/build-1-no-shim.log`) |
| `platformio-shim.ini`, `shim/soc/frc_timer_reg.h` | second probe: the first missing header stubbed to show how deep the port would go (`logs/build-2-with-shim.log`) |
| `bench/probe-psram.cpp`, `bench/probe-platformio.ini` | the PSRAM/heap probe flashed to `/dev/ttyUSB1` |
| `bench/capture_boot.py` | the serial capture (CP2102 DTR/RTS ladder, boot log preserved) |
| `bench/identity-and-ports.log` | esptool `chip_id` for all three ports + `lsusb` + by-id |
| `bench/console-boot-psram.txt` | quad-PSRAM build, full boot log |
| `bench/console-boot-psram-octal.txt` | octal-PSRAM build, full boot log |
| `bench/console-probe-no-psram-init.txt` | first probe (no PSRAM init attempted) |
