#!/usr/bin/env python3
"""0243: convert the ten downloaded photographs into scanout rasters.

Each frame goes through the accepted build's converter with the accepted
build's settings -- `--mode 800x600 --space gamma` -- so a frame is byte-identical
in format to the photograph the 0228 build displays today (the embedded fallback
raster this project compiles is produced by the same command from
data/hummingbird.jpg and hashes to 204578cc...bd306, the accepted build's hash).

Outputs, all in --out:
  FRAME01.BIN .. FRAME10.BIN   480,000 B raw framebuffers, play order = name order
  frames.sha256                the ten digests, in play order
  frames-blob.bin              the ten files concatenated, ready for
                               `esptool write_flash 0x400000 frames-blob.bin`
                               (this is what the stage build copies to the card)

The converter also wants to write PNG previews named after `--out`; it derives
them by replacing ".bin", so the files are written as FRAME01.bin and renamed to
FRAME01.BIN afterwards -- passing an uppercase name makes the preview step fail
after the raster is already written.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CONV = os.path.join(HERE, "convert_photo64.py")
FRAMES = 10


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default="frames-src")
    ap.add_argument("--out", default="frames")
    ap.add_argument("--mode", default="800x600")
    ap.add_argument("--space", default="gamma")
    ap.add_argument("--python", default=sys.executable)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    lines = []
    for i in range(1, FRAMES + 1):
        src = os.path.join(args.src, "src%02d.jpg" % i)
        if not os.path.exists(src):
            print("missing source", src)
            return 2
        tmp = os.path.join(args.out, "FRAME%02d.bin" % i)
        subprocess.run([args.python, CONV, "--source", src, "--out", tmp,
                        "--mode", args.mode, "--space", args.space], check=True,
                       stdout=subprocess.DEVNULL)
        final = os.path.join(args.out, "FRAME%02d.BIN" % i)
        os.replace(tmp, final)
        size = os.path.getsize(final)
        if size != 480000:
            print("BAD SIZE", final, size)
            return 3
        digest = sha256(final)
        lines.append("%s  %s" % (digest, os.path.basename(final)))
        print("FRAME%02d %7d B  %s" % (i, size, digest[:16]))

    with open(os.path.join(args.out, "frames.sha256"), "w") as f:
        f.write("\n".join(lines) + "\n")

    blob = os.path.join(args.out, "frames-blob.bin")
    with open(blob, "wb") as out:
        for i in range(1, FRAMES + 1):
            with open(os.path.join(args.out, "FRAME%02d.BIN" % i), "rb") as f:
                shutil.copyfileobj(f, out)
    print("blob %s %d B sha256 %s" % (blob, os.path.getsize(blob), sha256(blob)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
