#!/usr/bin/python3
"""Flash a PlatformIO-built esp32cam image and capture the console report.

Usage: capture.py <firmware.bin> <outdir> [seconds]

Writes <outdir>/console.txt (raw), <outdir>/report.txt (REPORT: lines),
<outdir>/cam.jpg (decoded from the B64START/B64END block) and prints a summary.
"""
import base64
import hashlib
import os
import subprocess
import sys
import time

import serial

PORT = "/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0"

binfile, outdir = sys.argv[1], sys.argv[2]
seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 40.0
os.makedirs(outdir, exist_ok=True)

flash = subprocess.run(
    ["esptool", "--chip", "esp32", "--port", PORT, "--baud", "115200",
     "write_flash", "-z", "0x10000", binfile],
    capture_output=True, text=True, timeout=300)
open(os.path.join(outdir, "flash.txt"), "w").write(flash.stdout + "\n" + flash.stderr)
print("FLASH rc", flash.returncode)
print((flash.stdout + flash.stderr).strip().splitlines()[-6:])

time.sleep(1.0)
raw = bytearray()
deadline = time.time() + seconds
ser = serial.Serial()
ser.port = PORT
ser.baudrate = 115200
ser.timeout = 0.3
ser.dtr = False      # keep out of the GPIO0 strap
ser.rts = False
ser.open()
ser.reset_input_buffer()
while time.time() < deadline:
    chunk = ser.read(4096)
    if chunk:
        raw.extend(chunk)
ser.close()

text = raw.decode("utf-8", "replace")
open(os.path.join(outdir, "console.txt"), "w").write(text)
reports = [l for l in text.splitlines() if l.startswith("REPORT:")]
open(os.path.join(outdir, "report.txt"), "w").write("\n".join(reports) + "\n")
print("\n".join(reports))

# decode the base64 block
try:
    body = text.split("B64START\n", 1)[1].split("\nB64END", 1)[0]
    blob = base64.b64decode("".join(l.strip() for l in body.splitlines()))
    p = os.path.join(outdir, "cam.jpg")
    open(p, "wb").write(blob)
    print("JPEG bytes", len(blob), "sha256", hashlib.sha256(blob).hexdigest(),
          "SOI", blob[:2].hex(), "EOI", blob[-2:].hex())
except Exception as exc:  # noqa: BLE001
    print("no base64 block:", exc)
