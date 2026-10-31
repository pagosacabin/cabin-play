# MKS SERVO42C UART protocol — as implemented by `firmware/servo42c-uart-test`

Scope: the Makerbase **MKS SERVO42C** closed-loop stepper driver, UART header
(JST-SH 1.0 mm 4-pin: `3V3 / G / TX / RX`), driven directly from an ESP32 UART
with the **MKS APT V1.0 adapter out of the serial path**.

Every protocol claim below is taken from a Makerbase source and cited in the
table. Nothing here was inferred from a third-party driver.

---

## 1. Link settings

| Parameter | Value | Source |
|---|---|---|
| Baud rate | **38400** 8N1 (menu item `UartBaud`, default index `04`) | https://github.com/makerbase-mks/MKS-SERVO42C/wiki/Serial-communication-format |
| Slave address | **0xE0** (menu item `UartAddr`, default `00`→`e0`) | https://github.com/makerbase-mks/MKS-SERVO42C/wiki/Serial-communication-format |
| Byte order | multi-byte fields are big-endian ("high 8 bits first") | https://github.com/makerbase-mks/MKS-SERVO42C/wiki/Serial-communication-format |
| Work mode | must be `CtrMode = CR_UART` (UART control); default is `CR_vFOC` (step/dir) | https://vallder.com/wp-content/uploads/2024/06/MKS-SERVO42C-User-Manual-V1.1.2.pdf |

## 2. Frame format

```
request : <addr> <func> [data…] <tCHK>
reply   : <addr> [data…] <rCHK>          result byte 0x01 = ok, 0x00 = failed
```

`tCHK` / `rCHK` is **the low byte of the additive sum of every preceding byte**,
verified against the manufacturer's own worked examples:

| Frame | Sum | Check byte | Manufacturer example |
|---|---|---|---|
| `e0 36` | 0x116 | `16` | `e0 36 16` |
| `e0 f3 01` | 0x1D4 | `d4` | `e0 f3 01 d4` |
| `e0 f7` | 0x1D7 | `d7` | `e0 f7 d7` |
| `e0 f6 10` | 0x1E6 | `e6` | `e0 f6 10 e6` |
| `e0 fd 01 00 00 0c 80` | 0x26A | `6a` | `e0 fd 01 00 00 0c 80 6a` |
| reply `e0 00 00 40 00` | 0x120 | `20` | `e0 00 00 40 00 20` (90°) |
| reply `e0 01` | 0xE1 | `e1` | `e0 01 e1` |

Source for the rule and for all examples above:
https://github.com/makerbase-mks/MKS-SERVO42C/wiki/Serial-communication-description
(`# Verification`, and the per-command examples).

### Two incompatible firmware generations

| | firmware **V1.0** | firmware **V1.1.x** |
|---|---|---|
| Check byte | **absent** | present (sum & 0xFF) |
| `0x36` reply | `e0 00 00 00 00` (5 B) | `e0 00 00 40 00 20` (6 B) |
| `0x33` reply | int16 | int32 |
| `0xFD` pulse field | **int16** (`e0 fd SS HH LL`) | **int32** (`e0 fd SS HH HH LL LL`) |
| Source | https://github.com/makerbase-mks/MKS-SERVO42C/wiki/Serial-communication-description-V1.0 | https://github.com/makerbase-mks/MKS-SERVO42C/wiki/Serial-communication-description |

The sketch therefore **probes both framings at boot** (a checksummed `0x36`
produces a 6-byte valid reply; otherwise a plain `0x36` producing 5 bytes is
accepted) and builds `0xFD` with the matching pulse-field width. Detect it and
print which generation answered — do not assume.

## 3. Command table (firmware V1.1; V1.0 is the same minus the check byte)

| Operation | Frame | Reply / meaning | Source |
|---|---|---|---|
| Read encoder value | `e0 30 tCHK` | `u16`, 0…0xFFFF (motor must be calibrated) | https://github.com/makerbase-mks/MKS-SERVO42C/wiki/Serial-communication-description |
| Read received pulses | `e0 33 tCHK` | `i32` pulse count (int16 on V1.0) | https://github.com/makerbase-mks/MKS-SERVO42C/wiki/Serial-communication-description |
| **Read shaft angle** | `e0 36 tCHK` | `i32`, one rev = **0…65535**, e.g. `40 00` = 90° | https://github.com/makerbase-mks/MKS-SERVO42C/wiki/Serial-communication-description |
| **Read following error** | `e0 39 tCHK` | `i16` = (target − actual), 65536 units per rev, `00 B7` = 1° | https://github.com/makerbase-mks/MKS-SERVO42C/wiki/Serial-communication-description |
| Read En pin status | `e0 3a tCHK` | `01` enabled, `02` disabled, `00` error | https://github.com/makerbase-mks/MKS-SERVO42C/wiki/Serial-communication-description |
| Read shaft status | `e0 3e tCHK` | `01` blocked, `02` unblocked, `00` error | https://github.com/makerbase-mks/MKS-SERVO42C/wiki/Serial-communication-description |
| **Motor enable / disable** | `e0 f3 xx tCHK`, `xx` `01`=enable `00`=disable | `01` ok | https://github.com/makerbase-mks/MKS-SERVO42C/wiki/Serial-communication-description |
| Constant-speed run | `e0 f6 xx tCHK`, bit7=dir, low7=speed gear | `01` ok | https://github.com/makerbase-mks/MKS-SERVO42C/wiki/Serial-communication-description |
| Stop | `e0 f7 tCHK` | `01` ok | https://github.com/makerbase-mks/MKS-SERVO42C/wiki/Serial-communication-description |
| **Relative move (N pulses)** | `e0 fd <dir|speed> <i32 pulses> tCHK` | `01` ok — moves N pulses relative to the **current** position | https://github.com/makerbase-mks/MKS-SERVO42C/wiki/Serial-communication-description |
| Set subdivision (`MStep`) | `e0 84 xx tCHK`, 1…256 | `01` ok | https://github.com/makerbase-mks/MKS-SERVO42C/wiki/Serial-communication-description |
| Set baud rate | `e0 8a xx tCHK`, `04`=38400 | `01` ok | https://github.com/makerbase-mks/MKS-SERVO42C/wiki/Serial-communication-description |
| Zero-mode: set reference zero | `e0 91 00 tCHK` | `01` ok (requires `0_Mode`, cmd `0x90`, set first) | https://github.com/makerbase-mks/MKS-SERVO42C/wiki/Serial-communication-description |
| Zero-mode: go home | `e0 94 00 tCHK` | `01` ok | https://github.com/makerbase-mks/MKS-SERVO42C/wiki/Serial-communication-description |
| Save / clear power-on run | `e0 ff c8 TCHK` / `e0 ff ca` | `01` ok (driver disables itself after save) | https://github.com/makerbase-mks/MKS-SERVO42C/wiki/Serial-communication-description |
| Restore defaults | `e0 3f tCHK` | `01` ok (serial settings then need re-set) | https://github.com/makerbase-mks/MKS-SERVO42C/wiki/Serial-communication-description |

No command carries a version/identity string: `0x3F` is *restore defaults*, not a
firmware query, so this sketch reports the framing generation as its identity
result instead of inventing a version query.

Speed gear → rpm (1.8° motor, `Mstep` from the OLED):

```
Vrpm = speed × 30000 / (Mstep × 200)      // e.g. gear 16, Mstep 16 → 150 rpm
```

Source: https://github.com/makerbase-mks/MKS-SERVO42C/wiki/Serial-communication-description

## 4. What the requested operations map to — and what does not exist

The task asked for "absolute move, relative move, set-zero". Firmware V1.1
implements **only the relative form**, and it has **no "set current position as
zero" command**:

| Requested | Reality on the SERVO42C | Implemented here |
|---|---|---|
| relative move | `0xFD` = N pulses from the current position, direction bit + speed gear | yes — +90° = `MStep × 200 / 4` pulses (16 → 800 = `00 00 03 20`) |
| absolute move | **not present.** Position is tracked in pulses relative to wherever the counter stands; the only absolute notion is the *zeroMode* reference point used by the homing routine | no — the sketch emulates "return to zero" as a reverse relative move of the same pulse count and verifies the shaft angle lands back on the start value |
| set zero | `0x91` stores the zeroMode reference point for the homing routine (`0x94`), i.e. an absolute datum for *homing*, not a re-zero of the live position counter | no — writing `0x91` would overwrite the driver's stored datum, which this read/write-light bench test must not do |

Consequence for the test: step 5/7 command ±800 pulses and steps 6/8 judge the
**measured shaft angle** (0x36), not the driver's pulse counter. That is the only
honest absolute check available on this firmware.

## 5. Where sources disagree (encountered while researching)

1. **The V1.0 wiki page contradicts itself.** Its `0x36` entry claims the reply
   ends in `tCHK`, but every V1.0 example is a 5-byte reply with no check byte
   (`e0 00 00 00 00`) and the V1.0 format page has no checksum rule at all. The
   examples and the format page are consistent; the one-line description is a
   copy/paste leftover from V1.1. Handled by probing, not by trusting the prose.
   (https://github.com/makerbase-mks/MKS-SERVO42C/wiki/Serial-communication-description-V1.0)
2. **Most community "SERVO42C" drivers on the web implement a different
   protocol family** — the ZDT/Emm42 style `FA <addr> <func> … <crc>` /
   `FB …` frames with a fixed `0x6B` checksum byte. Those belong to the
   **SERVO42D/57D** (and ZDT X42S) line, not the 42C: the SERVO42D manual shows
   `send "FA 01 30 2B"` for its optional MODBUS-RTU mode
   (https://darxton.ru/files/_misc/ed4/ed4621f7-1463-11f0-aa2b-000c29f5eb60.pdf,
   §7.3). Copying those constants to a 42C yields silence. The `0xE0`-addressed,
   sum-checksummed frames documented by Makerbase are the correct ones.
3. **A `0x6B` byte appears in both worlds by coincidence**: it is the fixed ZDT
   checksum *and* the valid sum-check for `e0 84 07` (set 7 microsteps). A
   driver that "works with 0x6B appended" is not evidence of the ZDT protocol.
4. **`MStep` is write-only.** There is no read command for the subdivision, yet
   the pulse count for a given angle depends on it. The sketch takes it as a
   compile-time constant (`STEP_SUBDIVISION`) and prints an explicit corrective
   hint when the measured move disagrees with the commanded one, instead of
   silently writing the driver's settings.
5. **The `0xFD` direction bit is not the drive's `Dir` setting.** Measured on this
   unit (2026-10-31): with the OLED `Dir` on **CW**, `0xFD` with bit7=0 still moves
   the shaft in the **negative** `0x36` direction, and setting `Dir` does not change
   the reported sign — the driver remaps motor phases and encoder together, so the
   encoder sign is invariant. Bit7=1 is "forward = increasing angle" here. Do not
   infer the command direction from the `Dir` menu item.
6. **A long `0xFD` move can stop short while acknowledging the frame.** One
   3200-pulse command (nominal 1 rev at MStep 16) returned result `01` but the
   `0x33` counter advanced only **2224**, and the shaft moved exactly 2224 x 20.48
   counts — repeatably, in both directions. 800-pulse moves execute exactly. So
   `0x33` around every move is the check that separates "short move" from "wrong
   scale": a short move and a 4x scale error look identical in `0x36` alone (this is
   what made 0162's -25 deg look like MStep 64 / a 16384-count scale).
7. **`0x30` did not answer as documented.** The reply captured was
   `e0 FF FF FF FF FE DD B7`, which only validates as addr + **6** payload bytes
   (sum `B7`), not the documented `u16` (addr + 2 + chk). The sketch does not use
   `0x30` as evidence; `0x33` and `0x36` carry the proof. Unexplained.
