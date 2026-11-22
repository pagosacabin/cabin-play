#!/usr/bin/env python3
"""Print the text of a 0239 report-partition read-back (magic + length header)."""
import struct, sys
d = open(sys.argv[1], 'rb').read()
magic, n = struct.unpack('<II', d[:8])
if n > len(d) - 8:
    n = len(d) - 8
print(d[8:8+n].decode(errors='replace'))
print(f"\n[report magic=0x{magic:08x} len={n}]")
