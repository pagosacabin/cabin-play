#!/usr/bin/env python3
"""0250 rung 1 -- generate the 12 DAC calibration test frames.

One solid frame per (channel, code): for each of R, G, B and each 2-bit code
0..3, a frame with that code on that channel and 0 on the other two.  Twelve
files, 480,000 B each, in the frame board's accepted packed format:

    1 byte per pixel = 1 DAC byte = (B<<4) | (G<<2) | R,  800 x 600

A solid frame is uniform, so the file bytes are exactly that one DAC byte
repeated -- the display path in n16r8-slideshow streams the file straight into
the framebuffer, so what the DAC emits is literally the file content.  That is
the property the measurement needs: no camera, no white balance, no dither.

Displayed by n16r8-slideshow, which enumerates /sdcard/FRAMES sorted by name and
shows each for ~10 s.  Names sort R0..R3, G0..G3, B0..B3.

Usage:
    python3 make_dac_frames.py --out bench/dac-cal/test-frames   # 12 .BIN + SHA256SUMS
"""

import argparse
import hashlib
import os
import sys

W, H = 800, 600
FRAME_BYTES = W * H          # 480,000
CHANNELS = ("R", "G", "B")   # dac_byte() owns the bit layout


def dac_byte(r: int, g: int, b: int) -> int:
    for name, v in (("r", r), ("g", g), ("b", b)):
        if not 0 <= v <= 3:
            raise ValueError(f"{name} code {v} out of range 0..3")
    return (b << 4) | (g << 2) | r


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, help="directory to write the frames into")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    manifest = []
    for ch_name in CHANNELS:
        for code in range(4):
            codes = {"R": 0, "G": 0, "B": 0}
            codes[ch_name] = code
            value = dac_byte(codes["R"], codes["G"], codes["B"])
            data = bytes([value]) * FRAME_BYTES
            assert len(data) == FRAME_BYTES
            fn = f"DAC-{ch_name}{code}.BIN"
            path = os.path.join(args.out, fn)
            with open(path, "wb") as f:
                f.write(data)
            digest = hashlib.sha256(data).hexdigest()
            manifest.append((digest, fn))
            print(f"{fn}: byte 0x{value:02x} x {FRAME_BYTES}  sha256 {digest}")

    # Distinctness: the three code-0 frames are identical black by construction
    # (0x00 on every channel), so 12 files carry 10 distinct DAC bytes.  That is
    # a feature, not a collision: the three zero frames must read the same ~0 V
    # and are the meter's own zero check.
    distinct = {dac_byte(c if n == "R" else 0, c if n == "G" else 0, c if n == "B" else 0)
                for n in CHANNELS for c in range(4)}
    assert len(distinct) == 10, f"expected 10 distinct DAC bytes, got {sorted(distinct)}"
    zero = {m[0] for m in manifest if m[1].endswith("0.BIN")}
    assert len(manifest) == 12 and len(zero) == 1, "the three code-0 frames must be identical"
    with open(os.path.join(args.out, "SHA256SUMS"), "w") as f:
        for digest, fn in manifest:
            f.write(f"{digest}  {fn}\n")
    print(f"\n{len(manifest)} frames, {len({m[1] for m in manifest})} names, "
          f"{FRAME_BYTES} B each, manifest written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
