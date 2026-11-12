#!/usr/bin/env python3
"""0179 image pipeline: JPEG -> 320x240 one-byte-per-pixel frames for the S3.

WHY PRE-CONVERT (rather than reuse rung 2's on-device TJpgDec path): the S3 rung
has no PSRAM and the scanout ISR must read its source out of internal RAM. An
on-device decoder would need TJpgDec's work pool plus a stream buffer on top of
the 76,800 B frame, and it buys nothing this rung needs -- the images are fixed
and known at build time. So the pixels are quantised here, embedded as byte
arrays in flash, and memcpy'd into the internal-RAM frame at the phase switch.
The cost is ~150 KB of flash; the benefit is that no decoder runs at all.

The quantiser is a line-by-line port of photo-display-s3/src/ladder64.h (itself
extracted verbatim from photo-display-64/src/photo_render64.h):

  * ladder levels from the measured resistors 270/470 ohm into 75 ohm
    -> 0 / 0.452 / 0.714 / 1.000 of full scale (not 0/85/170/255);
  * dither in LIGHT: the sRGB byte is gamma-decoded (2.2) before the level
    position is computed, and an 8x8 Bayer threshold is added in level-space --
    the 0177 round-2 finding that voltage-domain dither speckles shadows;
  * each channel is sampled at a different Bayer phase so R/G/B decorrelate.

float32 is used throughout to match the C implementation's arithmetic exactly
(the C code uses float, and the >>8 floor makes a 1-LSB difference visible).
"""
import json
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
DATA = os.path.join(PROJ, "data")
SRC = os.path.join(PROJ, "..", "photo-display-64", "data")

W, H = 320, 240

DAC_R_HEAVY = np.float32(270.0)
DAC_R_WEAK = np.float32(470.0)
DAC_R_TERM = np.float32(75.0)
GAMMA = np.float32(2.2)

BAYER_DX = (0, 3, 6)
BAYER_DY = (0, 5, 2)

BAYER8 = np.array([
    [0, 32,  8, 40,  2, 34, 10, 42],
    [48, 16, 56, 24, 50, 18, 58, 26],
    [12, 44,  4, 36, 14, 46,  6, 38],
    [60, 28, 52, 20, 62, 30, 54, 22],
    [3, 35, 11, 43,  1, 33,  9, 41],
    [51, 19, 59, 27, 49, 17, 57, 25],
    [15, 47,  7, 39, 13, 45,  5, 37],
    [63, 31, 55, 23, 61, 29, 53, 21],
], dtype=np.uint8)


def ladder_levels():
    v = [
        np.float32(0.0),
        DAC_R_TERM / (DAC_R_WEAK + DAC_R_TERM),
        DAC_R_TERM / (DAC_R_HEAVY + DAC_R_TERM),
        DAC_R_TERM / ((DAC_R_HEAVY * DAC_R_WEAK) / (DAC_R_HEAVY + DAC_R_WEAK) + DAC_R_TERM),
    ]
    vmax = v[3]
    return [x / vmax for x in v]


def ladder_pos(t, lev):
    if t <= lev[1]:
        j = (t - lev[0]) / (lev[1] - lev[0])
    elif t <= lev[2]:
        j = np.float32(1.0) + (t - lev[1]) / (lev[2] - lev[1])
    else:
        j = np.float32(2.0) + (t - lev[2]) / (lev[3] - lev[2])
    return min(max(j, np.float32(0.0)), np.float32(3.0))


def build_table():
    """[256] uint16: level position in 1/256ths, light-domain (PERCEPTUAL)."""
    V = ladder_levels()
    L = [np.float32(0.0)] + [np.float32(np.power(float(x), float(GAMMA))) for x in V[1:]]
    tab = np.zeros(256, dtype=np.uint16)
    for i in range(256):
        s = np.float32(i) / np.float32(255.0)
        slin = np.float32(0.0) if i == 0 else np.float32(np.power(float(s), float(GAMMA)))
        tab[i] = np.uint16(int(ladder_pos(slin, L) * np.float32(256.0) + np.float32(0.5)))
    return tab, V, L


def quantise_channel(plane, tab, dx, dy):
    h, w = plane.shape
    out = np.zeros((h, w), dtype=np.uint8)
    for y in range(h):
        for x in range(w):
            thr = int(BAYER8[(y + dy) & 7][(x + dx) & 7])
            q = (int(tab[plane[y, x]]) + thr * 4) >> 8
            out[y, x] = 0 if q < 0 else 3 if q > 3 else q
    return out


def convert(path):
    im = Image.open(path).convert("RGB")
    if im.size != (W * 2, H * 2):
        im = im.resize((W * 2, H * 2), Image.LANCZOS)
    # 2x2 box average == TJpgDec's 1/2 descale in the RGB domain, so the source
    # pixels match what rung 2 decoded on-device.
    im = im.resize((W, H), Image.BOX)
    a = np.asarray(im, dtype=np.uint8)
    tab, V, L = build_table()
    qr = quantise_channel(a[:, :, 0], tab, BAYER_DX[0], BAYER_DY[0])
    qg = quantise_channel(a[:, :, 1], tab, BAYER_DX[1], BAYER_DY[1])
    qb = quantise_channel(a[:, :, 2], tab, BAYER_DX[2], BAYER_DY[2])
    packed = ((qb << 4) | (qg << 2) | qr).astype(np.uint8)
    return packed, V, L


def write_c_array(fh, name, data):
    fh.write("static const uint8_t %s[%d] = {\n" % (name, data.size))
    flat = data.reshape(-1)
    for i in range(0, flat.size, 24):
        fh.write("    " + ",".join("0x%02x" % v for v in flat[i:i + 24]) + ",\n")
    fh.write("};\n")


def census(packed):
    vals, counts = np.unique(packed, return_counts=True)
    return {int(v): int(c) for v, c in zip(vals, counts)}


def main():
    os.makedirs(DATA, exist_ok=True)
    jobs = [("IMG_LORIKEET", "parrot.jpg"), ("IMG_BLUE_MARBLE", "photo.jpg")]
    results = {}
    for name, fn in jobs:
        src = os.path.join(SRC, fn)
        if not os.path.exists(src):
            sys.exit("missing source image: %s" % src)
        packed, V, L = convert(src)
        results[name] = packed

    hdr = os.path.join(PROJ, "src", "images.h")
    with open(hdr, "w") as fh:
        fh.write("// GENERATED by tools/convert_images.py -- do not edit.\n")
        fh.write("// %dx%d, one byte per pixel, layout (B<<4)|(G<<2)|R matching the\n" % (W, H))
        fh.write("// ladder/pin map in src/main.cpp. Quantised to the measured ladder\n")
        fh.write("// (0/0.452/0.714/1.000) with light-domain 8x8 Bayer dither.\n")
        fh.write("#pragma once\n#include <stdint.h>\n\n")
        fh.write("#define IMG_W %d\n#define IMG_H %d\n#define IMG_BYTES %d\n\n" % (W, H, W * H))
        for name, _ in jobs:
            write_c_array(fh, name, results[name])
            fh.write("\n")
    print("wrote", hdr)

    tab, V, L = build_table()
    print("ladder fractions :", ["%.4f" % float(x) for x in V])
    print("ladder luminance :", ["%.4f" % float(x) for x in L])
    print("level sRGB codes :", [int(round(float(x) * 255.0)) for x in V])
    for name, _ in jobs:
        p = results[name]
        c = census(p)
        print("%-15s unique palette entries used: %d of 64, bytes=%d"
              % (name, len(c), p.size))


if __name__ == "__main__":
    main()
