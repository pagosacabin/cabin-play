#!/usr/bin/env python3
"""Capture the full boot log + probe output from the Heltec (CP2102, /dev/ttyUSB1).

Classic CP2102 ladder (DTR deasserted, RTS pulse) then read WITHOUT flushing the
buffer, so the second-stage bootloader's PSRAM complaint is not discarded.
"""
import sys, time, serial

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB1"
dur = float(sys.argv[2]) if len(sys.argv) > 2 else 6.0

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
