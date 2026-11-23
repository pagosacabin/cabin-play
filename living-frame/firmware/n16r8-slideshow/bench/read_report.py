#!/usr/bin/env python3
"""Read the `report` partition and print it.  0243.

    ./read_report.py report.bin      # parse a file already on disk
    ./read_report.py                 # esptool read_flash first (resets the board!)

Since 0243 the partition is a sequence of 4 KiB records -- `magic u32, len u32,
text` -- one record per persist, because the app erases the partition once at
boot (panel stopped) and then only ever writes a single erased sector while the
display streams.  Older single-header reports (0227: magic + length at offset 0)
are still parsed.

Magic 0x30323433 = "0243"; 0x30323237/0x30323238 = the accepted 0227/0228 build.
"""
import struct
import subprocess
import sys

PORT = "/dev/ttyUSB0"
ESPTOOL = "/home/astroboy/.platformio/packages/tool-esptoolpy/esptool.py"
SECTOR = 4096
SECTORS = 16
MAGIC = {0x30323433: "0243", 0x30323237: "0227", 0x30323238: "0228"}


def read_flash(path):
    subprocess.run([sys.executable, ESPTOOL, "--chip", "esp32s3", "--port", PORT,
                    "--baud", "460800", "read_flash", "0x310000", "0x10000", path],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "report.bin"
    if len(sys.argv) < 2:
        read_flash(path)
    d = open(path, "rb").read()

    magic, n = struct.unpack("<II", d[:8])
    if magic != 0x30323433 and magic in MAGIC and 0 < n < SECTOR - 8:
        print("report from task %s (%d B):" % (MAGIC[magic], n))
        print(d[8:8 + n].decode("utf-8", "replace"))
        return 0

    records = []
    for i in range(SECTORS):
        off = i * SECTOR
        if off + 8 > len(d):
            break
        m, ln = struct.unpack("<II", d[off:off + 8])
        if m != 0x30323433 or not (0 < ln <= SECTOR - 8) or off + 8 + ln > len(d):
            break
        records.append(d[off + 8:off + 8 + ln].decode("utf-8", "replace"))

    if not records:
        print("no 0243/0227 report magic (0x%08x, len %d)" % (magic, n))
        return 1
    text = "".join(records)
    print("report from task 0243 (%d record(s), %d B):" % (len(records), len(text)))
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
