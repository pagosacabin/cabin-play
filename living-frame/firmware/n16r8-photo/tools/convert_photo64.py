#!/usr/bin/env python3
"""0227: source photograph -> 64-colour (RGB222) dithered 640x480 scanout raster.

Pipeline (same shape as the 0180 converter, minus palette selection because this
build has all 64 ladder colours to spend):

  1. portrait 480x640, scale-to-cover + centre-crop, so no letterboxing;
  2. sRGB -> linear light (2.2), because dithering in code space speckles shadows
     (the 0177 round-2 finding);
  3. Floyd-Steinberg error diffusion over the 64 RGB222 colours, either in linear
     light (0227) or in gamma/sRGB code space (0228) -- `--space`.  Linear light
     makes shadow steps perceptually even coarser than they are, which starved the
     shadows of dots on the panel; gamma space distributes dots the way the eye
     sees brightness.  Nothing else changes between the two runs.
     Distance weighted by luminance so luminance errors win over chroma errors;
  4. pack each pixel as the DAC byte the board expects: (b<<4)|(g<<2)|r with the
     2-bit fields in (MSB,LSB) order, exactly as 0226's dac() packs them;
  5. rotate the portrait canvas into the landscape raster 90 deg COUNTER-
     clockwise.  0226 used 90 deg clockwise; Stephen's monitor is pivoted the
     other way, so this build is flipped 180 deg relative to that test card
     (task 0227 amendment, 2026-11-20).  Nothing is rotated on the device.

Outputs (next to --out):
  *.bin   307,200 B raw framebuffer, ready to embed
  *-raster.png    exactly what the panel receives (640x480, as scanned out)
  *-as-seen.png   the raster rotated back, i.e. what Stephen's eye should see
"""
import argparse
import hashlib
import os
import sys

import numpy as np
from PIL import Image

PW, PH = 480, 640          # portrait canvas
LW, LH = 640, 480          # landscape raster
GAMMA = np.float32(2.2)
LUM_W = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)

# The 6-bit DAC's reachable levels per channel: two pins through a 270/470/75
# ohm ladder, i.e. 0, 1/3, 2/3, 1 of full scale (0180's ladder64.h model).
LEVELS = np.array([0.0, 1.0 / 3.0, 2.0 / 3.0, 1.0], dtype=np.float32)


def cover_crop(im, w, h):
    """Scale to cover w x h, then centre-crop.  Same rule as the 0180 pipeline."""
    sw, sh = im.size
    scale = max(w / sw, h / sh)
    nw, nh = max(1, int(round(sw * scale))), max(1, int(round(sh * scale)))
    im = im.resize((nw, nh), Image.LANCZOS)
    left = (nw - w) // 2
    top = (nh - h) // 2
    return im.crop((left, top, left + w, top + h))


def to_linear(rgb_u8):
    return (rgb_u8.astype(np.float32) / 255.0) ** GAMMA


def dither_fs(lin, space="gamma"):
    """Floyd-Steinberg over the 64 RGB222 colours; returns (h,w) index arrays.

    space="linear":  diffuse error in linear light      (0227's rendering)
    space="gamma":   diffuse error in sRGB code space   (0228's test)

    The quantiser always lands on the same four DAC levels per channel; only the
    space the error is measured and carried in changes.
    """
    if space == "gamma":
        # DAC level v of full scale emits ~v^gamma; invert that to compare codes.
        work = np.clip(lin, 0.0, 1.0) ** np.float32(1.0 / GAMMA)
        cand_levels = LEVELS.astype(np.float32) ** np.float32(1.0 / GAMMA)
    else:
        work = lin.copy()
        cand_levels = LEVELS
    h, w, _ = work.shape
    out = np.zeros((h, w, 3), dtype=np.uint8)      # level indices 0..3
    # pre-compute the 64 candidate colours in whichever space is being dithered
    cand = np.stack(np.meshgrid(cand_levels, cand_levels, cand_levels, indexing="ij"),
                    axis=-1).reshape(-1, 3).astype(np.float32)
    for y in range(h):
        for x in range(w):
            px = np.clip(work[y, x], 0.0, 1.0)
            d = cand - px
            dist = (d * d) * LUM_W
            k = int(np.argmin(dist.sum(axis=1)))
            chosen = cand[k]
            out[y, x] = ((k // 16) % 4, (k // 4) % 4, k % 4)   # r,g,b index order below
            err = px - chosen
            if x + 1 < w:
                work[y, x + 1] += err * np.float32(7 / 16)
            if y + 1 < h:
                if x > 0:
                    work[y + 1, x - 1] += err * np.float32(3 / 16)
                work[y + 1, x] += err * np.float32(5 / 16)
                if x + 1 < w:
                    work[y + 1, x + 1] += err * np.float32(1 / 16)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source", default="parrot.jpg")
    ap.add_argument("--out", default="photo64_640x480.bin")
    ap.add_argument("--space", choices=("linear", "gamma"), default="gamma",
                    help="dithering working space: gamma (0228, default) or linear (0227)")
    args = ap.parse_args()

    im = Image.open(args.source).convert("RGB")
    src_size = im.size
    im = cover_crop(im, PW, PH)
    lin = to_linear(np.asarray(im))

    idx = dither_fs(lin, args.space)   # (640,480,3) level indices in r,g,b order
    # cand was built as meshgrid(idx i=r, j=g, k=b) so index k = r*16 + g*4 + b;
    # unpack to r,g,b explicitly to avoid guessing:
    r = idx[:, :, 0]; g = idx[:, :, 1]; b = idx[:, :, 2]
    dac = ((b.astype(np.uint8) & 3) << 4) | ((g.astype(np.uint8) & 3) << 2) | (r.astype(np.uint8) & 3)

    # portrait canvas -> landscape raster, 90 deg COUNTER-clockwise:
    #   rx = cy,  ry = (LH-1) - cx
    raster = np.zeros((LH, LW), dtype=np.uint8)
    for cy in range(PH):
        row = dac[cy]                       # 480 canvas pixels
        rx = np.full(PW, cy, dtype=int)
        ry = (LH - 1) - np.arange(PW)
        raster[ry, rx] = row

    data = raster.tobytes()
    open(args.out, "wb").write(data)
    sha = hashlib.sha256(data).hexdigest()

    # previews
    rgb = np.zeros((LH, LW, 3), dtype=np.uint8)
    rgb[:, :, 0] = (raster & 3) * 85
    rgb[:, :, 1] = ((raster >> 2) & 3) * 85
    rgb[:, :, 2] = ((raster >> 4) & 3) * 85
    tag = "" if args.space == "linear" else "-gamma"
    raster_png = args.out.replace(".bin", f"{tag}-raster.png")
    seen_png = args.out.replace(".bin", f"{tag}-as-seen.png")
    Image.fromarray(rgb, "RGB").save(raster_png)
    Image.fromarray(np.rot90(rgb, k=-1), "RGB").save(seen_png)

    # A tiny header so the firmware's runtime checksum has something to compare
    # against that is not hand-typed (drift between tool output and firmware is
    # exactly the class of error this task is exposed to).
    # The data itself is emitted as a C array: PlatformIO's IDF builder mishandles
    # embedded-binary paths here (it generates the .S with a doubled build path and
    # the link fails with missing symbols), and a header has no such moving parts.
    datah = os.path.join(os.path.dirname(args.out), "photo64_data.h")
    with open(datah, "w") as f:
        f.write("/* generated by tools/convert_photo64.py -- do not edit */\n")
        f.write("#pragma once\n#include <stdint.h>\n")
        f.write(f"static const uint8_t PHOTO64_IMAGE[{len(data)}] = {{\n")
        for i in range(0, len(data), 24):
            f.write("    " + ",".join(f"0x{b:02x}" for b in data[i:i + 24]) + ",\n")
        f.write("};\n")

    meta = os.path.join(os.path.dirname(args.out), "photo64_meta.h")
    with open(meta, "w") as f:
        f.write("/* generated by tools/convert_photo64.py -- do not edit */\n")
        f.write(f'#define PHOTO64_SOURCE "{os.path.basename(args.source)}"\n')
        f.write(f'#define PHOTO64_SPACE "{args.space}"\n')
        f.write(f'#define PHOTO64_SRC_SIZE "{src_size[0]}x{src_size[1]}"\n')
        f.write(f'#define PHOTO64_BYTES {len(data)}\n')
        f.write(f'#define PHOTO64_SHA256 "{sha}"\n')
        f.write('#define PHOTO64_ROTATION "90deg counter-clockwise (180deg from the 0226 test card)"\n')

    hist = np.bincount(raster.ravel(), minlength=64)
    print(f"source      : {args.source} {src_size[0]}x{src_size[1]} -> cover-crop {PW}x{PH} portrait")
    print(f"colours     : 64 (RGB222), Floyd-Steinberg in {args.space} space")
    print(f"output      : {args.out}, {len(data)} B, sha256 {sha}")
    print(f"distinct    : {int((hist > 0).sum())} of 64 DAC bytes used; "
          f"most common 0x{int(np.argmax(hist)):02x} ({int(hist.max())} px)")
    print(f"raster      : portrait rotated 90 deg COUNTER-clockwise (180 deg from 0226's card)")
    print(f"previews    : {raster_png} , {seen_png}")


if __name__ == "__main__":
    sys.exit(main())
