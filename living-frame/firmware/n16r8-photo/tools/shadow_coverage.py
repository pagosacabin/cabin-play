#!/usr/bin/env python3
"""0228: compare shadow coverage between two 64-colour renderings.

For a defined dark region -- the darkest quartile of source-luminance pixels in the
480x640 portrait canvas -- report, for each rendering:

  * cover%    : percentage of those pixels that come out non-black (DAC byte != 0)
  * mean DAC  : mean DAC byte over the region (0..63)
  * isolated% : of the non-black pixels, the share with at most one non-black
                8-neighbour -- the "starfield" signature Stephen photographed
  * mean NN   : mean count of non-black 8-neighbours for non-black pixels

The mask is mapped into the landscape raster with the converter's own rotation
(rx = cy, ry = (LH-1) - cx) so the same image positions are compared in both files.

Usage:  shadow_coverage.py <source.jpg> <raster_a.bin> <raster_b.bin> [labels...]
"""
import sys
import numpy as np
from PIL import Image

PW, PH = 480, 640
LW, LH = 640, 480
GAMMA = np.float32(2.2)
LUM_W = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


def cover_crop(im, w, h):
    sw, sh = im.size
    scale = max(w / sw, h / sh)
    nw, nh = max(1, int(round(sw * scale))), max(1, int(round(sh * scale)))
    im = im.resize((nw, nh), Image.LANCZOS)
    left, top = (nw - w) // 2, (nh - h) // 2
    return im.crop((left, top, left + w, top + h))


def dark_mask(source):
    im = cover_crop(Image.open(source).convert("RGB"), PW, PH)
    code = np.asarray(im, dtype=np.float32) / 255.0
    lin = code ** GAMMA
    lum = (lin * LUM_W).sum(axis=2)
    thr = np.quantile(lum, 0.25)
    return lum <= thr, lum


def to_raster(mask):
    out = np.zeros((LH, LW), dtype=bool)
    ys, xs = np.nonzero(mask)              # xs in [0,PW), ys in [0,PH)
    out[(LH - 1) - xs, ys] = True          # rx = cy, ry = (LH-1) - cx
    return out


def stats(bin_path, rmask):
    d = np.frombuffer(open(bin_path, "rb").read(), dtype=np.uint8).reshape(LH, LW)
    region = d[rmask]
    nz = region != 0
    cover = 100.0 * nz.sum() / max(1, region.size)
    mean_dac = float(region.mean())

    # 8-neighbour count over the whole raster, then read the region's values.
    d_b = (d != 0).astype(np.uint8)
    pad = np.pad(d_b, 1)
    nn = sum(pad[1 + dy:1 + dy + LH, 1 + dx:1 + dx + LW]
             for dy in (-1, 0, 1) for dx in (-1, 0, 1) if (dy, dx) != (0, 0))
    nn_region = nn[rmask][nz]
    isolated = 100.0 * float((nn_region <= 1).mean()) if nn_region.size else 0.0
    return cover, mean_dac, isolated, float(nn_region.mean()) if nn_region.size else 0.0


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    source, bins = sys.argv[1], sys.argv[2:]
    mask, lum = dark_mask(source)
    rmask = to_raster(mask)
    print(f"source        : {source}")
    print(f"dark region   : darkest quartile by linear luminance "
          f"(threshold {np.quantile(lum, 0.25):.4f}), {int(mask.sum())} of {PW * PH} canvas px")
    print()
    print(f"{'rendering':<26} {'cover%':>8} {'meanDAC':>8} {'isolated%':>10} {'meanNN':>7}")
    for b in bins:
        label = b.split("/")[-1]
        c, m, iso, nn = stats(b, rmask)
        print(f"{label:<26} {c:8.2f} {m:8.2f} {iso:10.1f} {nn:7.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
