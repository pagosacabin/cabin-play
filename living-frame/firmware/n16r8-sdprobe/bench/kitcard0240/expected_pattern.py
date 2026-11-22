#!/usr/bin/env python3
"""Task 0240: the expected SHA-256 of the firmware's test pattern.

The kit build writes byte i = ((i*31) + ((i>>9)*7) + ((i>>17)*131)) ^ 0xA5,
all in uint32 arithmetic (see kit_pattern() in src/main.c).  Recomputing it
here is the independent half of the check: the board's own mbedtls hash must
equal this value for a write/read-back pass to mean anything.

    python3 bench/kitcard0240/expected_pattern.py [bytes]

Defaults to 128 KiB, the size the firmware writes (KIT_FILE_BYTES).
"""

import hashlib
import sys

MASK = 0xFFFFFFFF


def pattern(nbytes: int) -> bytes:
    out = bytearray(nbytes)
    for i in range(nbytes):
        v = ((i * 31) + ((i >> 9) * 7) + ((i >> 17) * 131)) & MASK
        out[i] = (v ^ 0xA5) & 0xFF
    return bytes(out)


def main() -> int:
    nbytes = int(sys.argv[1]) if len(sys.argv) > 1 else 128 * 1024
    data = pattern(nbytes)
    print(f"bytes:  {nbytes}")
    print(f"sha256: {hashlib.sha256(data).hexdigest()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
