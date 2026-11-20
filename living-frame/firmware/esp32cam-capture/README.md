# ESP32-CAM bring-up (task 0230)

Kit ESP32-CAM (AI-Thinker form factor, OV2640) → ESP-NOW → living-frame board's
SD card. This directory is the camera end of that pipeline.

Three build environments, in bring-up order:

| env | what it does |
|---|---|
| `esp32cam-probe` | board identity (MAC, flash, PSRAM) + read-only SD-slot probe |
| `esp32cam-diag` | SCCB bus scan at XCLK off / 20 MHz / 10 MHz, PWDN both states, alt pin pair, OV2640 PID read |
| `esp32cam` | the capture build: OV2640 → JPEG in PSRAM → SHA-256 → SD write/read-back → base64 dump on the console |

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
