#!/usr/bin/env bash
# 0234: drive 12 board resets, reading the report each time so the first-pass
# table's growth is visible. Resets are esptool's hard_reset (the FT232R bridge
# resets the board reliably; 0229 used the same path).
set -u
S=/home/astroboy/.hermes/profiles/cronrunner/cache/scratch/0234
cd "$S"
for i in $(seq 1 12); do
  echo "===== RESET $i $(date -Is)"
  esptool --chip esp32s3 -p /dev/ttyUSB0 -b 460800 --after hard_reset read_mac > "reset-$i.esptool.log" 2>&1
  tail -2 "reset-$i.esptool.log"
  sleep 42
  esptool --chip esp32s3 -p /dev/ttyUSB0 -b 460800 read_flash 0x310000 0x10000 "report-reset-$i.bin" > "read-$i.log" 2>&1
  /usr/bin/python3 - "report-reset-$i.bin" <<'PY'
import struct, sys
d=open(sys.argv[1],'rb').read()
m,n=struct.unpack('<II',d[:8])
t=d[8:8+n].decode(errors='replace')
for l in t.splitlines():
    if l.startswith(('FIRSTPASS_SLOTS','FIRSTPASS_SUMMARY','RAW_INIT:','PROBE COMPLETE')):
        print('  ',l)
PY
done
echo "===== LOOP DONE $(date -Is)"
