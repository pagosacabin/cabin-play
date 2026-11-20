#!/bin/bash
# Prove the flash-size claim independently of any size report: write a pattern
# ABOVE the 8 MB mark and read it back.   A chip that is really 8 MB aliases or
# refuses that region; a real 16 MB part returns the bytes.
set -euo pipefail
cd /home/astroboy/cabin-play/living-frame/firmware/n16r8-probe

python3 - <<'PY'
import hashlib
d = bytes((i * 7 + 13) & 0xFF for i in range(4096))
open('pat4k.bin', 'wb').write(d)
print('pattern sha256', hashlib.sha256(d).hexdigest())
PY

esptool --port /dev/ttyUSB0 --baud 460800 write_flash 0x00E00000 pat4k.bin 2>&1 | tail -3
esptool --port /dev/ttyUSB0 --baud 460800 read_flash 0x00E00000 0x1000 readback14m.bin 2>&1 | tail -2

python3 - <<'PY'
import hashlib
a = open('pat4k.bin', 'rb').read()
b = open('readback14m.bin', 'rb').read()
print('written  ', hashlib.sha256(a).hexdigest())
print('readback ', hashlib.sha256(b).hexdigest())
print('RESULT: MATCH at 0x00E00000 (14 MB)' if a == b else 'RESULT: MISMATCH at 0x00E00000 (14 MB)')
PY
