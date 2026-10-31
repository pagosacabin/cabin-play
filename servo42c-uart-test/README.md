# servo42c-uart-test — MKS SERVO42C over UART from an ESP32

Bench self-test for a **Makerbase MKS SERVO42C** closed-loop stepper driven over
its 4-pin UART header from a classic **ESP32 devkit** (`esp32dev`), instead of
step/dir pulses — so the driver's own encoder feedback is actually used.

On boot the sketch runs one 8-step self-test, prints one `PASS`/`FAIL` line per
step, and ends with `SELF-TEST: PASS|FAIL (n of 8 steps failed)`. Verified on
hardware: 8/8 PASS, +90.13° / +90.10° measured for a commanded +90° move.

`PROTOCOL.md` is the cited protocol reference (every frame format claim traced to
a Makerbase source). `src/main.cpp` is the sketch; `platformio.ini` builds it.

## Wiring

Direct ESP32 UART2 ↔ driver UART header, **no** MKS APT V1.0 adapter in the path.

| Driver UART header (JST-SH 1.0 mm) | ESP32 devkit | Notes |
|---|---|---|
| `TX` (driver → host) | **GPIO16** — UART2 RX | crossed, not straight-through |
| `RX` (host → driver) | **GPIO17** — UART2 TX | |
| `G` | **GND** | common ground is mandatory — no common ground, no reply |
| `3V3` | leave open | the MKS manual lists it as floating; do not back-feed |

| Driver screw terminal | Supply |
|---|---|
| `V+` / `Gnd` | **12–24 V DC** for the motor stage (separate from the ESP32's 5 V USB) |

UART logic is 3.3 V TTL. Nothing 5 V may touch TX/RX. The ESP32's USB port is
only for flashing and for the 115200 console.

## Driver OLED settings — check all four before the first run

| Menu item | Value | Why |
|---|---|---|
| `CtrMode` | **`CR_UART`** | default is `CR_vFOC` (step/dir); with it, every `0xFD` move frame is rejected |
| `UartBaud` | **9600** | the setting this driver answers at. `38400` (the value quoted in `PROTOCOL.md` §1 as the factory index default) returned **zero bytes** here |
| `UartAddr` | **`e0`** | matches `ADDR` in the sketch (`0xE0`) |
| `MStep` | **16** | must equal `STEP_SUBDIVISION` in `src/main.cpp` — the protocol has no "read MStep" command, so the sketch cannot discover it. MStep 16 → 3200 pulses/rev → 90° = 800 pulses |
| `Dir` | either (`CW` here) | direction is handled in the command's bit 7 (`DIR_FORWARD_BIT`); the drive's `Dir` does **not** invert the reported encoder angle |

If your unit really is on 38400, change `DRIVER_BAUD` in `src/main.cpp` to match —
nothing else depends on the baud.

## Build, flash, run

```bash
cd servo42c-uart-test
pio run                      # compile for esp32dev
pio run -t upload            # flash over USB
pio device monitor -b 115200 # watch the self-test
```

The test is one-shot: it runs on every boot/reset, then idles. Press the ESP32's
`EN` button (or `pio run -t upload` again) to re-run.

## What the 8 steps do

| # | Step | PASS means |
|---|---|---|
| 1 | framing / protocol probe (`0x36`, checksummed V1.1 vs plain V1.0) | the driver answered — link, baud, address and mode are right |
| 2 | read shaft angle (`0x36`) | angle feedback available |
| 3 | read following error (`0x39`) | closed-loop error readable |
| 4 | enable motor (`0xF3 01`) + confirm (`0x3A`) | motor enabled and reported enabled |
| 5 | command +90° (`0xFD`, 800 pulses, gear 16) | driver acknowledged the move frame |
| 6 | measured move is +90° ± 5° (**sign** graded) | the shaft actually moved the commanded amount |
| 7 | command the reverse 800 pulses | return move acknowledged |
| 8 | shaft back on the start angle ± 5° | no net drift |

After step 8 the sketch runs an ungraded **INFO** probe (4 × 800 pulses = one
nominal revolution, then undone) that prints the driver's own pulse counter
(`0x33`) and encoder (`0x30`) either side of every move. Use it to check the
pulse unit: at `MStep 16`, 800 pulses must be 90° and `0x33` must track ±800.
Note that a *single* 3200-pulse command does not complete on this driver — the
driver acknowledges it and executes only part of it — which is why the probe is
four 800-pulse moves.

Every frame is echoed on the console as `TX:` / `RX:` hex, so a failure is
diagnosable from the log alone.

## If step 1 fails (no reply)

Check, in this order: TX/RX crossed as in the table above · common GND · the
four OLED settings · driver powered at 12–24 V (the UART does not answer on
logic power alone if the motor stage is unpowered).

| Symptom | Cause |
|---|---|
| zero bytes on every read | wiring / `UartBaud` / `UartAddr` / `CtrMode` |
| step 1 passes, `0xFD` rejected (`result 00`) | `CtrMode` is not `CR_UART` |
| move accepted but distance wrong | `STEP_SUBDIVISION` ≠ the `MStep` shown on the OLED |
| reads occasionally fail | this link loses replies; the sketch already retries each read 3× |
