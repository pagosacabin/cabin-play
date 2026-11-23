# freenove-cam -- 0244 camera side

Freenove ESP32-Wrover CAM (ESP32-D0WD-V3, GC0308 sensor, CH340 on `/dev/ttyUSB1`).

The frame board owns the button, the countdown and the photo library.  This board
is only a camera: it waits for a SHOOT trigger over ESP-NOW, takes one settled
frame, runs the colour chain and sends the packed frame back.  It keeps no
library -- no PSRAM retention, no camera-side SD writes.

    build / flash:  pio run -e freenove-cam -t upload
    secrets:        copy src/secrets.h.example to src/secrets.h (gitignored)

Colour chain, all on this board because the frame board receives finished pixels:

1. grey-world white balance at `WB_STRENGTH` (0.5), smoothed across frames
2. 256-entry gamma LUT -- DAC level v emits `(v/3)^2.2` of full scale, so each
   8-bit code maps to the two levels that bracket it plus the fraction needed
3. 4x4 ordered Bayer dither on that fraction

Protocol on the link:

| direction | packet | meaning |
|---|---|---|
| frame -> cam | `'F','T'` | SHOOT |
| cam -> frame | `'F','S'` len32 | start of frame |
| cam -> frame | `'F','D'` idx16 + <=240 B | data chunk (1,280 per frame) |
| cam -> frame | `'F','E'` sha256 | end, hash of the whole 307,200 B |
| frame -> cam | `'F','A'` sha256 | receiver's own hash (logged) |

Sensor notes: the GC0308 has **no JPEG engine** (`PIXFORMAT_JPEG` fails with
`0x106`); capture is RGB565 and the buffer is **big-endian per pixel** -- read it
as bytes (`(b[0]<<8)|b[1]`), never as a native `uint16_t`, or the picture comes
out as structured rainbow noise.  `WARMUP_FRAMES` (3) are discarded because the
first frames after an idle spell are badly exposed.

Bench hook: send `c` on the serial port to shoot without the frame board.
