# n16r8-live -- 0244 frame side

YD-ESP32-S3 N16R8 living frame: VGA DAC output, SD library, ESP-NOW link, and
the physical button that starts the photo flow.

    build / flash:  pio run -e n16r8-photo -t upload     (640x480, 16-line bounce)

## Flow

    press (GPIO14 -> GND, pull-up, 30 ms debounce)
      -> panel shows 3, 2, 1 (one second each, block digits)
      -> 'F','T' SHOOT to the camera
      -> 1,280 ESP-NOW chunks arrive (~1.6 s while associated; ~6.5 s unassociated)
      -> SHA-256 checked against the sender's; on a match the frame is written
         to /sdcard/LIVE/LIVE####.BIN (307,200 B) and shown
      -> cycling resumes over the whole LIVE library at 10 s per image

Presses during a countdown or a transfer are ignored.  The library is separate
from 0243's `/sdcard/FRAMES` (800x600) and from `/NEWCARD.BIN`; nothing here
formats a card -- `format_if_mount_failed` is false everywhere.

## SD bring-up (this is the part that bites)

The card in this rig does not answer the stock driver cold.  Two things are
required, both learned the hard way:

1. **The 0239 fold**: 74+ clocks with CS high, one throwaway CMD0 on the raw bus
   at 400 kHz, then release the bus so `esp_vfs_fat_sdspi_mount` can initialise
   SPI2 itself.  Without it the mount returns `ESP_ERR_TIMEOUT (263)`.
2. **Retries**: first-init is intermittent (`ESP_ERR_INVALID_CRC (265)` is normal
   on attempt 1).  Up to six attempts, each re-running the fold; the failure path
   must `spi_bus_free(SPI2_HOST)` or the next fold dies with
   `ESP_ERR_INVALID_STATE`.

## Files

- `src/main.c` -- VGA driver, panel start, report partition, fallback photograph
- `src/live.c` -- SD library, button, countdown, SHOOT, ESP-NOW receive, slideshow

The console on this bench is unusable (the FT232R returns a fixed byte cycle),
so every claim the app makes is written to the `report` flash partition and read
back with `esptool read_flash 0x310000 0x10000 report.bin`.
