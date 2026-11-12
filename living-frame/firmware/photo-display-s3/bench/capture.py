#!/usr/bin/env python3
"""Capture the photo-display-s3 console from the Heltec over its CP2102.

Port: /dev/ttyUSB1 by default -- the ESP32-S3 (MAC 9c:13:9e:a1:37:40). The other
two ports on the bench are NOT this board: /dev/ttyACM0 is an ESP32-C3 and
/dev/ttyUSB0 a classic ESP32-D0WD (see gate-fabgl-s3/bench/identity-and-ports.log).

The CP2102 reset ladder is used deliberately: this firmware's whole proof is in
its boot banner and its phase-cycle log, so the run starts from a known reset.
(That is the classic-ESP32 ladder; it boots app0 and does not enter download
mode, unlike the S3's native USB-Serial/JTAG.)
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB1"
dur = float(sys.argv[2]) if len(sys.argv) > 2 else 85.0

s = serial.Serial(port, 115200, timeout=0.2)
s.reset_input_buffer()
s.dtr = False
s.rts = True
time.sleep(0.2)
s.rts = False

buf = bytearray()
t0 = time.time()
while time.time() - t0 < dur:
    chunk = s.read(4096)
    if chunk:
        buf += chunk
s.close()
sys.stdout.write(buf.decode("utf-8", "replace"))
