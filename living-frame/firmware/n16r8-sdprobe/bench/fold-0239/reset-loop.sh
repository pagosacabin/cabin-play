#!/usr/bin/env bash
# 0239: drive N host resets for one arm, reading the report partition after each
# reset. Same harness discipline as 0236/0238: esptool hard_reset, 42 s settle,
# read_flash 0x310000 0x10000, and the report is what the board wrote.
set -u
ARM=$1
N=${2:-12}
S=/home/astroboy/.hermes/profiles/cronrunner/cache/scratch/0239
OUT=$S/arm$ARM
mkdir -p "$OUT"
cd "$OUT" || exit 1
for i in $(seq 1 "$N"); do
  echo "===== ARM $ARM RESET $i $(date -Is)"
  esptool --chip esp32s3 -p /dev/ttyUSB0 -b 460800 --after hard_reset read_mac \
      > "reset-$i.esptool.log" 2>&1
  echo "  esptool rc=$? $(tail -2 "reset-$i.esptool.log" | tr '\n' ' ')"
  sleep 42
  esptool --chip esp32s3 -p /dev/ttyUSB0 -b 460800 read_flash 0x310000 0x10000 "report-$i.bin" \
      > "read-$i.log" 2>&1
  /usr/bin/python3 /home/astroboy/.hermes/profiles/cronrunner/cache/scratch/0239/extract_report.py \
      "report-$i.bin" > "report-$i.txt" 2>&1
  grep -E 'FOLD_ARM|FOLD_FIRST_INIT|FOLD_SUMMARY|SACRIFICE|FOLD_E2E_MOUNT|FOLD_SLOT' "report-$i.txt" \
      | sed 's/^/  /'
done
echo "===== LOOP DONE $(date -Is)"
