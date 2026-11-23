#!/usr/bin/env bash
# 0243 slideshow: flash env:n16r8-slideshow and leave it running.
set -eu
cd "$(dirname "$0")/.."
pio run -e n16r8-slideshow -t upload
echo "slideshow running; one full loop + wrap takes ~2 min; then: bench/read_report.py bench/show-report.bin"
