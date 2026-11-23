#!/usr/bin/env bash
# 0243 stage: put the ten rasters in the `frames` flash partition, then let the
# stage build copy them onto the card in the holder and verify each one.
#
#   1. write frames-blob.bin (built by tools/build_frames.py) at 0x400000
#   2. flash env:n16r8-slideshow-stage and let it run to completion
#   3. read the report back
set -eu
cd "$(dirname "$0")/.."
ESPTOOL=~/.platformio/packages/tool-esptoolpy/esptool.py
pio run -e n16r8-slideshow-stage
python3 "$ESPTOOL" --chip esp32s3 --port /dev/ttyUSB0 --baud 460800 \
    write_flash --flash_size 16MB 0x400000 bench/frames-blob.bin
pio run -e n16r8-slideshow-stage -t upload
echo "stage build running; wait ~4 min, then: bench/read_report.py bench/stage-report.bin"
