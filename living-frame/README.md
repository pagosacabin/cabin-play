# Living Frame (working title)

Old Magnavox 22MD311B/F7 22" LCD TV -> wooden-framed living photo frame.
A Christmas gift.

## How it works

- A classic ESP32 drives the TV through its VGA/PC input (bitluni ESP32Lib
  or FabGL) and shows a JPEG slideshow from a microSD card.
- A separate ESP32-CAM unit with a big arcade button: press -> 3-2-1
  countdown (buzzer/LED) -> captures a photo -> sends the JPEG over ESP-NOW
  to the display unit -> display unit saves it to SD and it joins the
  rotation. "Added!" pops up on screen.
- Why two units: two MCUs cannot safely share one SD card. The ESP-NOW
  wireless hop is the proven fix (JPEG sent in chunks).
- The TV is driven through its inputs, NOT by ripping the panel (LVDS is a
  dead end for ESP32). Final form: plastic bezel removed, panel + boards
  mounted in finished wood.

## Parts

Display unit:

- Classic ESP32 devkit (ESP32-WROOM-32 style — the proven VGA path)
- microSD card module + card
- VGA connector + resistor ladder (per bitluni ESP32Lib wiring)
- 5V supply, VGA cable

Camera unit:

- ESP32-CAM with OV2640 (usually bundled)
- Big arcade button, small buzzer or bright LED
- USB 5V supply (or LiPo for a roaming photo button)

## Build order — proof first

- Rung 0 (THE PROOF): VGA test pattern / static image from the ESP32 on the
  Magnavox. Proves the entire display path. Nothing else starts until this
  holds a stable, correct-color image.
- Rung 1: JPEG slideshow from SD on the TV.
- Rung 2: ESP32-CAM captures on button press with countdown beeps.
- Rung 3: ESP-NOW camera -> display transfer; photo joins the rotation.
- Rung 4: Wood frame — remove plastic bezel, mount panel + boards in
  finished wood.
- Rung 5: Polish — "Added!" overlay, power management, gift wrap.

## Acceptance bars

- Rung 0: recognizable test image on the TV, stable, correct colors.
- Rung 3: press button -> photo appears in the slideshow within ~30s.

## Bench inventory

Classic ESP32 (the proven VGA path) — both confirmed on hand:

- ESP32-WROOM-32 devkit module(s)
- DFRobot FireBeetle ESP32-E (DFR0654)

Two ESP32-S3-family boards (native USB-Serial/JTAG, `303a:1001`) were also
attached to the bench host on 2026-10-15. The S3 has no proven VGA path here,
so rung 0 targets the classic WROOM-32 devkit.
