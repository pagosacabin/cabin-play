#!/usr/bin/env python3
"""Read the live frame's `report` partition and print it (0260).

    ./read_report.py report.bin      # parse a file already on disk
    ./read_report.py                 # esptool read_flash first (resets the board!)

The frame's console is on an FT232R that returns a fixed byte cycle, so this
partition IS the log.  Since 0260 it is a sequence of 4 KiB records -- `magic u32,
len u32, text` -- because the writer erases the partition once at boot and only
ever appends a single erased sector afterwards (a 64 KiB erase stalls the cache
the bounce ISR reads PSRAM through).  0243's format is accepted too, and the older
single-header 0227/0228 reports are still parsed.

Port: the FRAME is /dev/ttyUSB1 (FT232R).  /dev/ttyUSB0 is the camera's CH340.
"""
import struct
import subprocess
import sys

PORT = "/dev/ttyUSB1"
ESPTOOL = "/home/astroboy/.platformio/packages/tool-esptoolpy/esptool.py"
OFFSET = 0x310000
SECTOR = 4096
SECTORS = 16
REC_MAGIC = 0x30323630           # "0260"
REC_MAGICS = {0x30323630: "0260", 0x30323433: "0243"}
OLD_MAGIC = {0x30323237: "0227", 0x30323238: "0228"}


def read_flash(path):
    subprocess.run([sys.executable, ESPTOOL, "--chip", "esp32s3", "--port", PORT,
                    "--baud", "460800", "read_flash", hex(OFFSET), hex(SECTOR * SECTORS),
                    path],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "report.bin"
    if len(sys.argv) < 2:
        read_flash(path)
    d = open(path, "rb").read()

    magic, n = struct.unpack("<II", d[:8])
    if magic in OLD_MAGIC and 0 < n < SECTOR - 8:
        print("report from task %s (%d B):" % (OLD_MAGIC[magic], n))
        print(d[8:8 + n].decode("utf-8", "replace"))
        return 0

    records, task = [], None
    for i in range(SECTORS):
        off = i * SECTOR
        if off + 8 > len(d):
            break
        m, ln = struct.unpack("<II", d[off:off + 8])
        if m not in REC_MAGICS or not (0 < ln <= SECTOR - 8) or off + 8 + ln > len(d):
            break
        task = REC_MAGICS[m]
        records.append(d[off + 8:off + 8 + ln].decode("utf-8", "replace"))

    if not records:
        print("no 0260/0243/0227 report magic (0x%08x, len %d)" % (magic, n))
        return 1
    text = "".join(records)
    print("report from task %s (%d record(s), %d B):" % (task, len(records), len(text)))
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
