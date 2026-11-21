# ESP32-CAM bring-up (task 0230)

Kit ESP32-CAM (AI-Thinker form factor, OV2640) → ESP-NOW → living-frame board's
SD card. This directory is the camera end of that pipeline.

Three build environments, in bring-up order:

| env | what it does |
|---|---|
| `esp32cam-probe` | board identity (MAC, flash, PSRAM) + read-only SD-slot probe |
| `esp32cam-diag` | SCCB bus scan at XCLK off / 20 MHz / 10 MHz, PWDN both states, alt pin pair, OV2640 PID read |
| `esp32cam` | the capture build: OV2640 → JPEG in PSRAM → SHA-256 → SD write/read-back → base64 dump on the console |
| `esp32cam-espnow-tx` | rung 2: sends the deterministic 32 × 240 B test transfer to the frame board (no camera, no SD) |

```
pio run -e esp32cam-probe -t upload
/usr/bin/python3 bench/capture.py .pio/build/<env>/firmware.bin <outdir> <seconds>
```

`bench/capture.py` flashes with esptool, then reads the CH340 console with
pyserial (`dtr=False`, `rts=False` so the GPIO0 strap is untouched), keeps every
`REPORT:` line, and decodes the `B64START … B64END` block into `cam.jpg` so the
host holds a byte-exact copy of the capture even with no card seated.

## Board as found (2026-11-20)

| fact | value | how |
|---|---|---|
| chip | ESP32-D0WD-V3 rev 3 | esptool + on-target `esp_read_mac` |
| MAC | `f0:16:1d:ae:2b:20` | esptool `read_mac` |
| flash | 4,194,304 B | `ESP.getFlashChipSize()` |
| PSRAM | 4,192,123 B | `ESP.getPsramSize()` |
| uplink | CH340 on `/dev/serial/.../usb-1a86_USB_Serial-if00-port0` | enumerated 14:54 MST |

Flash works over that uplink (`Hash of data verified.`), and the console is
alive end-to-end (every REPORT line below came back over it). **The live console
fault recorded for the bench FTDI bridge (0225) does not apply to this CH340
path** — that is the frame board's adapter, not the CAM's.

## What does not work yet, and the evidence

`esp_camera_init()` returns **261 (`ESP_ERR_NOT_FOUND`)** — the sensor probe gets
no ACK on SCCB. The diagnostic build rules out everything except the module
itself:

```
REPORT: scan=pwdn_high_xclk_off  sda=26 scl=27 idle_sda=1 idle_scl=1 -> 0 devices
REPORT: scan=pwdn_low_xclk_off   sda=26 scl=27 idle_sda=1 idle_scl=1 -> 0 devices
REPORT: scan=pwdn_low_xclk_20mhz sda=26 scl=27 idle_sda=1 idle_scl=1 -> 0 devices
REPORT: ov2640_pid_20mhz=0x00 (expect 0x26)
REPORT: scan=pwdn_low_xclk_10mhz sda=26 scl=27 idle_sda=1 idle_scl=1 -> 0 devices
REPORT: scan=alt_pins_21_22_xclk_off                            -> 0 devices
```

Reading of that: the SCCB lines idle HIGH under the ESP32's internal pull-ups and
**never see a device at any address**, with XCLK stopped or driven at two
frequencies and with PWDN both high and low. A seated OV2640 ACKs at 0x30
(7-bit) as soon as XCLK runs. Zero devices on both candidate pin pairs is not a
pin-map error — it is an absent or unpowered module (ribbon not seated, module
not fitted, or a dead module). That is a hands-at-the-bench check, not a firmware
fix.

The CAM's own microSD slot is also empty:

```
REPORT: sd_begin=0 card_type=0 (0=NONE,1=MMC,2=SDSC,3=SDHC)
REPORT: sd_mounted=0 no_card_or_mount_failure
```

The probe is read-only by design: the card identity/scratch-card decision belongs
to 0229 and nothing here writes to a card.

## Retest after the 2026-11-20 17:00 MST reseat — rung 1 still fails

Stephen unplugged the board, unplugged the camera module, replugged it and
replugged the board, **all to the same slots**, then authorised a re-run. Both
bring-up builds were flashed again and captured; the result is unchanged:

| check | before reseat (16:15 MST) | after reseat (17:16 MST) |
|---|---|---|
| `i2c_found_count` at 26/27, PWDN high + XCLK off | 0 | 0 |
| … PWDN low, XCLK off | 0 | 0 |
| … PWDN low, XCLK 20 MHz | 0 | 0 |
| … PWDN low, XCLK 10 MHz | 0 | 0 |
| alternate pair 21/22 | 0 | 0 |
| `ov2640_pid_20mhz` | `0x00` (expect `0x26`) | `0x00` |
| SCCB lines idle | SDA 1 / SCL 1 | SDA 1 / SCL 1 |
| CAM microSD slot | `card_type=0` (NONE) | `card_type=0` (NONE) |

Logs: `bench/run-2026-11-20-reseat/diag/` and `.../probe/`. The board identity is
identical too (`ESP32-D0WD-V3 rev3`, MAC `f0:16:1d:ae:2b:20`, 4 MB flash, 4 MB
PSRAM), and the boot was a clean `POWERON_RESET` — the two console captures
contain no brownout line and no `ESP-ROM` banner, so neither capture reset the
board.

Reading of the unchanged result: a module connection that was broken and remade
with **zero** change in the bus state is not a seating fault any more. With the
lines still idling high under the ESP32's internal pull-ups and no device at any
address on two candidate pin pairs, the remaining candidates are (a) the module
itself (or its ribbon) is dead, (b) the module is not receiving power — the
AI-Thinker feeds the camera from the board's 3.3 V regulator, so a supply fed in
at 3.3 V instead of 5 V would run the ESP32 while starving the camera, and
(c) the FFC contacts are facing the wrong way in the connector. All three are
hands-and-multimeter checks; nothing in firmware can distinguish them.

The CAM slot still holds **no card** (`sd_begin=0 card_type=0`), as the task's
17:02 update asked to be reported.

## Rung 2 — the link works (2026-11-20 17:20 MST)

The camera is not needed for the radio link, and Stephen granted the rung-2
window, so the link was proven this run with `esp32cam-espnow-tx`:

```
REPORT: sha256_tx=74bb840392a757f743926df1e363626405bb5416482b85e30e817766afaa2f28
REPORT: pass=1 send_ok=32 send_fail=0
REPORT: pass=2 send_ok=32 send_fail=0
REPORT: result=espnow_tx_sent
```

The frame board reassembled the same 7,680 bytes with the same hash — full result
in `../n16r8-espnow-rx/README.md`. Logs:
`bench/run-2026-11-20-reseat/espnow-tx/`.

## Next actions at the bench (in order)

1. Re-seat the OV2640 ribbon in the CAM's FFC connector (contacts toward the
   board, latch closed) and re-run `esp32cam-diag`. Expect `i2c_found=0x30` and
   `ov2640_pid_20mhz=0x26`.
2. Seat a card in the CAM's slot and re-run `esp32cam-probe`; expect
   `card_type=2|3` and the `sd_entry` listing.
3. Then `esp32cam` produces the capture, the SD read-back hash and the base64
   copy in one boot.

## Bring-up numbers the later rungs depend on

- `FRAMESIZE_VGA` (640×480), `jpeg_quality = 12` → expected JPEG tens of kB;
  ESP-NOW's 250-byte payload means ~200-400 chunks per photo.
- The camera-side SHA-256 printed by `esp32cam` is the number every later rung
  must reproduce (frame-board reassembly, SD file at rest).

See `PROTOCOL.md` for the chunked-transfer design that rungs 2 and 3 implement.
