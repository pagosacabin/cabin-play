#!/usr/bin/env python3
"""Capture the 0225 probe's console. Survives the FTDI DTR/RTS reset: the board
is re-opened in a loop so a torn-down tty does not end the capture."""
import sys, time
import serial

PORT = "/dev/ttyUSB0"
BAUD = int(sys.argv[2]) if len(sys.argv) > 2 else 115200
SECS = int(sys.argv[1]) if len(sys.argv) > 1 else 45
OUT = "/home/astroboy/cabin-play/living-frame/firmware/n16r8-probe/serial-probe.log"

buf = bytearray()
t0 = time.time()
ser = None
try:
    ser = serial.Serial(PORT, BAUD, timeout=0.3)
    ser.rts = True
    time.sleep(0.25)
    ser.rts = False
except Exception as e:
    print(f"open failed: {e}", file=sys.stderr)

while time.time() - t0 < SECS:
    try:
        if ser is None or not ser.is_open:
            ser = serial.Serial(PORT, BAUD, timeout=0.3)
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
    except Exception:
        try:
            ser.close()
        except Exception:
            pass
        ser = None
        time.sleep(0.3)

try:
    if ser:
        ser.close()
except Exception:
    pass

text = buf.decode("utf-8", "replace")
open(OUT, "w").write(text)
print(text)
print(f"\n[captured {len(buf)} bytes in {SECS}s -> {OUT}]", file=sys.stderr)
