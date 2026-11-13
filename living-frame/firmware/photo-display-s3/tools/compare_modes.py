#!/usr/bin/env python3
"""Numbers behind 0180's question: full-res 16 colours vs quarter-res 64 colours.

Stephen's eyes decide, but a numeric baseline keeps the comparison honest and
catches a pipeline that is quietly wrong (e.g. a rotation that does not invert).
Both modes are rebuilt from the same portrait source here:

  full16   480x640, 4 bpp, per-photo 16-colour palette from tools/convert_images16.py
           (the committed frames, read back out of src/images16.h via the module)
  doubled  480x640 seen as 320x240 doubled 2x2, 64 colours, light-domain Bayer
           dither -- rung 3a's pipeline (tools/convert_images.py) applied to the
           same portrait source instead of rung 2's landscape photo

Error is reported in sRGB codes (mean absolute, per channel and overall) after
the reconstruction Stephen would actually see: full16 at native resolution,
doubled nearest-neighbour upscaled back to 480x640.
"""
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import convert_images16 as c16                                    # noqa: E402

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


def doubled64(portrait):
    """rung 3a's 64-colour path: 320x240, light-domain Bayer, doubled back up."""
    im = Image.fromarray(portrait).resize((320, 240), Image.LANCZOS)
    a = np.asarray(im, dtype=np.uint8)
    V, L = c16.V, c16.L
    out = np.zeros((240, 320, 3), dtype=np.uint8)
    for ch in range(3):
        dx, dy = BAYER_DX[ch], BAYER_DY[ch]
        plane = a[:, :, ch].astype(np.float32) / np.float32(255.0)
        lin = np.power(plane, c16.GAMMA)
        # ladder position in light, in 1/256ths of a level step -- the same
        # arithmetic as ladder64.h's s_qtab[PERCEPTUAL] plus the Bayer offset.
        idx1 = lin <= L[1]
        idx2 = (lin > L[1]) & (lin <= L[2])
        idx3 = lin > L[2]
        pos = np.where(idx1, (lin - L[0]) / (L[1] - L[0]),
                       np.where(idx2, 1.0 + (lin - L[1]) / (L[2] - L[1]),
                                2.0 + (lin - L[2]) / (L[3] - L[2])))
        pos = np.clip(pos, 0.0, 3.0)
        thr = BAYER8[(np.arange(240)[:, None] + dy) & 7,
                     (np.arange(320)[None, :] + dx) & 7].astype(np.float32)
        q = np.floor((pos * 256.0 + thr * 4.0) / 256.0).astype(np.int32)
        q = np.clip(q, 0, 3)
        out[:, :, ch] = np.rint(V[q] * 255.0).astype(np.uint8)
    up = np.asarray(Image.fromarray(out).resize((480, 640), Image.NEAREST), dtype=np.uint8)
    return up, out


def main():
    jobs = [("lorikeet", "parrot.jpg"), ("blue-marble", "photo.jpg")]
    print("%-12s %-28s %-28s" % ("image", "full16 (480x640, 16 colours)",
                                 "doubled (320x240, 64 colours)"))
    for name, fn in jobs:
        portrait = c16.load_portrait(os.path.join(c16.DATA, fn))
        lin = c16.srgb_to_linear(portrait)
        pal_codes, _ = c16.choose_palette(lin)
        pal_codes = c16.refine_palette(lin, pal_codes)
        idx, _ = c16.dither_fs(lin, pal_codes)
        idx_land = c16.rotate_into_scanout(idx)
        packed = c16.pack4(idx_land)
        shown_land, _ = c16.render(packed, pal_codes)
        a = c16.rotate_back_to_portrait(shown_land)
        b, _ = doubled64(portrait)
        ea = np.abs(a.astype(np.int16) - portrait.astype(np.int16))
        eb = np.abs(b.astype(np.int16) - portrait.astype(np.int16))
        print("%-12s MAE %6.2f  max %3d          MAE %6.2f  max %3d"
              % (name, ea.mean(), ea.max(), eb.mean(), eb.max()))
        la = (a.astype(np.float32) @ c16.LUM_W) / 255.0
        lb = (b.astype(np.float32) @ c16.LUM_W) / 255.0
        lp = (portrait.astype(np.float32) @ c16.LUM_W) / 255.0
        print("%-12s luminance MAE %0.4f (linear)          luminance MAE %0.4f"
              % ("", np.abs(la - lp).mean(), np.abs(lb - lp).mean()))
    print("\nBoth are dithered reconstructions of the same 480x640 portrait. A lower")
    print("MAE is not the verdict -- colour count and resolution trade off, which is")
    print("exactly what Stephen is being asked to judge on the screen.")


if __name__ == "__main__":
    main()
