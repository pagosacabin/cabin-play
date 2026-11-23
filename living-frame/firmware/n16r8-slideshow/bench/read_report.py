#!/usr/bin/env python3
"""Read the `report` partition and print it.  0243.

    ./read_report.py report.bin      # parse a file already on disk
    ./read_report.py                 # esptool read_flash first (resets the board!)

The header is two little-endian u32: task magic and payload length.  Magic
0x30323237 = "0227" (the accepted photo build), 0x30323433 = "0243".
"""
import struct, subprocess, sys

PORT = "/dev/ttyUSB0"
ESPTOOL = "/home/astroboy/.platformio/packages/tool-esptoolpy/esptool.py"
MAGIC = {0x30323237: "0227", 0x30323238: "0228", 0x30323433: "0243"}


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
    tag = MAGIC.get(magic)
    if tag is None or not (0 < n < 60000):
        print("no 0243/0227 report magic (0x%08x, len %d)" % (magic, n))
        return 1
    print("report from task %s (%d B):" % (tag, n))
    print(d[8:8 + n].decode("utf-8", "replace"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
