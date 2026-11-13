#!/usr/bin/env python3
"""0180 (rung 3b) image pipeline: photo -> 640x480 portrait-native 16-colour frame.

WHAT THIS PRODUCES
  src/images16.h -- two 640x480 frames at 4 bits per pixel (153,600 B each) plus
  one 16-entry palette per photo. Two pixels share a byte; the EVEN column is the
  low nibble (that is the order src/scanout.h's 4bpp expansion assumes).

WHY A PER-IMAGE PALETTE
  The 4-bit frame can name only 16 colours, but the DAC can render 64 (4 levels
  per channel on the measured 270/470 ohm ladder). A fixed 16 of those 64 wastes
  the budget: a bird photo and a globe need different slices of the cube. So the
  16 entries are chosen per photo from the 64 the ladder can actually produce --
  never a fixed system palette.

METHOD (each step is deliberate, and measured rather than assumed)
  1. Portrait: scale to COVER 480x640 and centre-crop, so no letterboxing. The
     frame stored for scanout is 640x480 -- the portrait image rotated 90 deg
     CLOCKWISE into the landscape raster (see `rotate_into_scanout`). Nothing is
     rotated on the device: the rotation is baked in here, so the ISR stays a
     flat index->DAC-byte expansion.
  2. Linear light: sRGB bytes are gamma-decoded with the same 2.2 exponent the
     ladder model uses (src/ladder64.h). Both palette selection and dithering
     happen in linear light -- the 0177 round-2 finding was that dithering in the
     voltage (sRGB-code) domain speckles shadows.
  3. Median cut in linear light: recursively split the pixel set along its widest
     channel at that channel's weighted median until there are 16 boxes; each box
     contributes its weighted mean.
  4. Snap to the ladder: each box mean is replaced by the nearest of the 64
     ladder colours, distance weighted by luminance (0.2126/0.7152/0.0722), so a
     candidate that gets luminance right wins over one that only gets chroma
     right. Duplicates are dropped -- a photo whose 16 boxes collapse to fewer
     distinct ladder colours reports that smaller number (it is a property of the
     DAC, not a bug).
  5. Floyd-Steinberg error diffusion in linear light against that 16-colour
     palette. Error diffusion (rather than an ordered threshold) is what makes an
     arbitrary, irregularly spaced palette work: the error a pixel leaves behind
     is carried to its neighbours instead of assuming evenly spaced levels.

float32 is used throughout to match the C implementation's arithmetic.
"""
import argparse
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
DATA = os.path.join(PROJ, "data")

PW, PH = 480, 640          # portrait content
LW, LH = 640, 480          # landscape scanout raster (the rotated frame)

GAMMA = np.float32(2.2)
DAC_R_HEAVY = np.float32(270.0)
DAC_R_WEAK = np.float32(470.0)
DAC_R_TERM = np.float32(75.0)
LUM_W = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32)


def ladder_levels():
    v = [
        np.float32(0.0),
        DAC_R_TERM / (DAC_R_WEAK + DAC_R_TERM),
        DAC_R_TERM / (DAC_R_HEAVY + DAC_R_TERM),
        DAC_R_TERM / ((DAC_R_HEAVY * DAC_R_WEAK) / (DAC_R_HEAVY + DAC_R_WEAK) + DAC_R_TERM),
    ]
    vmax = v[3]
    return np.array([x / vmax for x in v], dtype=np.float32)


V = ladder_levels()                      # level -> fraction of full scale
L = np.array([0.0 if i == 0 else float(V[i]) ** float(GAMMA) for i in range(4)],
             dtype=np.float32)           # level -> linear light


def srgb_to_linear(a):
    s = a.astype(np.float32) / np.float32(255.0)
    return np.power(s, GAMMA)


def linear_to_srgb_byte(x):
    x = np.clip(x, 0.0, 1.0)
    return np.rint(np.power(x, np.float32(1.0 / float(GAMMA))) * 255.0).astype(np.uint8)


def cube64():
    """The 64 colours the ladder can produce, as (linear rgb, 6-bit code)."""
    lin = np.zeros((64, 3), dtype=np.float32)
    code = np.zeros(64, dtype=np.uint8)
    for qb in range(4):
        for qg in range(4):
            for qr in range(4):
                i = (qb << 4) | (qg << 2) | qr
                lin[i] = (L[qr], L[qg], L[qb])
                code[i] = i
    return lin, code


def rotate_into_scanout(portrait):
    """480x640 portrait -> 640x480 landscape, rotated 90 deg CLOCKWISE.

    Clockwise means the portrait's TOP edge becomes the raster's RIGHT edge, so
    the picture reads upright on a monitor pivoted 90 deg COUNTER-CLOCKWISE
    (rotate-left). rotate_into_scanout_cw/ccw are the two candidates; the marker
    test below fixes which is which by construction rather than by reasoning:
    portrait (0,0) must land at raster (LW-1, 0).
    """
    out = np.rot90(portrait, k=-1)               # clockwise
    assert out.shape[:2] == (LH, LW), out.shape
    # Independent statement of the same mapping, asserted on a marker: the
    # portrait's top-left pixel is the raster's top-right pixel.
    assert np.array_equal(out[0, LW - 1], portrait[0, 0])
    assert np.array_equal(out[LH - 1, 0], portrait[PH - 1, PW - 1])
    return out


def rotate_back_to_portrait(landscape):
    """Inverse of rotate_into_scanout, for previews as Stephen will see them."""
    out = np.rot90(landscape, k=1)
    assert out.shape[:2] == (PH, PW), out.shape
    return out


def load_portrait(path):
    im = Image.open(path).convert("RGB")
    w, h = im.size
    scale = max(PW / w, PH / h)
    im = im.resize((max(PW, int(round(w * scale))), max(PH, int(round(h * scale)))), Image.LANCZOS)
    w, h = im.size
    left = (w - PW) // 2
    top = (h - PH) // 2
    im = im.crop((left, top, left + PW, top + PH))
    assert im.size == (PW, PH), im.size
    return np.asarray(im, dtype=np.uint8)


def median_cut(samples, weights, n):
    """Linear-light median cut. samples: (N,3) float32, weights: (N,) float32."""
    boxes = [np.arange(samples.shape[0])]
    while len(boxes) < n:
        best, best_score = -1, -1.0
        for bi, idx in enumerate(boxes):
            if idx.size < 2:
                continue
            pts = samples[idx]
            rng = pts.max(axis=0) - pts.min(axis=0)          # per-channel extent
            score = float(weights[idx].sum()) * float(rng.max())
            if score > best_score:
                best_score, best = score, bi
        if best < 0:
            break                                            # nothing splittable
        idx = boxes.pop(best)
        pts = samples[idx]
        rng = pts.max(axis=0) - pts.min(axis=0)
        ch = int(np.argmax(rng))
        order = idx[np.argsort(pts[:, ch], kind="stable")]
        w_sorted = weights[order]
        cum = np.cumsum(w_sorted)
        cut = int(np.searchsorted(cum, cum[-1] * 0.5))
        cut = min(max(cut, 1), order.size - 1)
        boxes.append(order[:cut])
        boxes.append(order[cut:])
    return boxes


def refine_palette(lin_rgb, pal_codes, iters=8):
    """Constrained k-means: centroids must be ladder colours, all distinct.

    Median cut alone leaves the boxes' means where the box boundaries put them;
    re-assigning every pixel to its nearest palette entry and re-snapping the
    centroid each round is a Lloyd iteration restricted to the 64 reachable
    colours, which is what the dither will actually be asked to reproduce. On
    collision (two centroids snapping to one colour) the smaller cluster is
    moved to its nearest still-unused colour, so the 16 slots stay distinct.
    """
    cube_lin, cube_code = cube64()
    flat = lin_rgb.reshape(-1, 3)
    codes = np.array(pal_codes, dtype=np.uint8)
    for _ in range(iters):
        pal_lin = np.array([(L[c & 3], L[(c >> 2) & 3], L[(c >> 4) & 3]) for c in codes],
                           dtype=np.float32)
        d = ((flat[:, None, :] - pal_lin[None, :, :]) ** 2 * LUM_W[None, None, :]).sum(axis=2)
        assign = np.argmin(d, axis=1)
        counts = np.bincount(assign, minlength=codes.size)
        new = np.empty_like(codes)
        used = set()
        order = np.argsort(-counts, kind="stable")
        for i in order:
            m = flat[assign == i]
            target = m.mean(axis=0) if m.size else pal_lin[i]
            dc = ((cube_lin - target) ** 2 * LUM_W).sum(axis=1)
            for cand in np.argsort(dc, kind="stable"):
                c = int(cube_code[cand])
                if c not in used:
                    used.add(c)
                    new[i] = c
                    break
        if np.array_equal(np.sort(new), np.sort(codes)):
            codes = np.sort(new)
            break
        codes = np.sort(new)
    return codes


def choose_palette(lin_rgb):
    """16-entry palette (raw DAC bytes) chosen from the 64 ladder colours."""
    flat = lin_rgb.reshape(-1, 3)
    weights = np.full(flat.shape[0], 1.0, dtype=np.float32)
    boxes = median_cut(flat, weights, 16)

    means = np.array([flat[idx].mean(axis=0) for idx in boxes], dtype=np.float32)
    sizes = np.array([idx.size for idx in boxes], dtype=np.float32)
    cube_lin, cube_code = cube64()

    # Replacing each box mean by its own nearest ladder colour lets two boxes
    # collide on one colour and silently waste slots (seen: 16 boxes -> 8 cubes).
    # Because the cost decomposes as sum||p-c||^2 = sum||p-m||^2 + n*||m-c||^2 and
    # the first term is constant per box, a greedy one-to-one assignment on
    # n*||m-c||^2 is the exact optimum for picking distinct colours.
    cost = sizes[:, None] * ((cube_lin[None, :, :] - means[:, None, :]) ** 2
                             * LUM_W[None, None, :]).sum(axis=2)
    used_rows, used_cols = set(), set()
    chosen = []
    for _ in range(min(cost.shape)):
        best = None
        for r in range(cost.shape[0]):
            if r in used_rows:
                continue
            for c in range(cost.shape[1]):
                if c in used_cols:
                    continue
                if best is None or cost[r, c] < best[0]:
                    best = (cost[r, c], r, c)
        if best is None:
            break
        used_rows.add(best[1])
        used_cols.add(best[2])
        chosen.append(int(cube_code[best[2]]))
    pal_codes = sorted(set(chosen))
    return np.array(pal_codes, dtype=np.uint8), means


def nearest_index(px, pal_lin):
    d = ((pal_lin[None, :, :] - px[:, None, :]) ** 2 * LUM_W[None, None, :]).sum(axis=2)
    return np.argmin(d, axis=1)


def dither_fs(lin_rgb, pal_codes):
    """Floyd-Steinberg in linear light. Returns (H,W) uint8 palette indices."""
    pal_lin = np.array([(L[c & 3], L[(c >> 2) & 3], L[(c >> 4) & 3]) for c in pal_codes],
                       dtype=np.float32)
    h, w, _ = lin_rgb.shape
    buf = lin_rgb.astype(np.float32).copy()
    out = np.zeros((h, w), dtype=np.uint8)
    for y in range(h):
        row = buf[y]
        for x in range(w):
            t = row[x]
            d = ((pal_lin - t) ** 2 * LUM_W).sum(axis=1)
            i = int(np.argmin(d))
            out[y, x] = i                       # index into pal_codes
            e = t - pal_lin[i]
            if x + 1 < w:
                row[x + 1] += e * np.float32(7.0 / 16.0)
            if y + 1 < h:
                if x > 0:
                    buf[y + 1, x - 1] += e * np.float32(3.0 / 16.0)
                buf[y + 1, x] += e * np.float32(5.0 / 16.0)
                if x + 1 < w:
                    buf[y + 1, x + 1] += e * np.float32(1.0 / 16.0)
    return out, pal_lin


def pack4(indices):
    """Two pixels per byte, EVEN column in the low nibble (scanout.h contract)."""
    h, w = indices.shape
    assert w % 2 == 0
    hi = indices[:, 1::2].astype(np.uint8)
    lo = indices[:, 0::2].astype(np.uint8)
    return ((hi << 4) | lo).astype(np.uint8)


def render(packed, pal_codes):
    """What the DAC shows: index -> ladder level -> sRGB byte, per channel."""
    lo = packed & 0x0F
    hi = (packed >> 4) & 0x0F
    h, wb = packed.shape
    idx = np.zeros((h, wb * 2), dtype=np.uint8)
    idx[:, 0::2] = lo
    idx[:, 1::2] = hi
    codes = pal_codes[idx]
    out = np.zeros((h, wb * 2, 3), dtype=np.uint8)
    out[:, :, 0] = linear_to_srgb_byte(L[codes & 3])
    out[:, :, 1] = linear_to_srgb_byte(L[(codes >> 2) & 3])
    out[:, :, 2] = linear_to_srgb_byte(L[(codes >> 4) & 3])
    return out, idx


def write_c_array(fh, name, data, per_line=24):
    fh.write("static const uint8_t %s[%d] = {\n" % (name, data.size))
    flat = data.reshape(-1)
    for i in range(0, flat.size, per_line):
        fh.write("    " + ",".join("0x%02x" % v for v in flat[i:i + per_line]) + ",\n")
    fh.write("};\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview-dir", default=os.path.join(PROJ, "tools"))
    ap.add_argument("--no-preview", action="store_true")
    args = ap.parse_args()

    jobs = [("IMG16_LORIKEET", "parrot.jpg"), ("IMG16_BLUE_MARBLE", "photo.jpg")]
    results = {}

    print("ladder fractions (fraction of full scale) :", " ".join("%.4f" % float(x) for x in V))
    print("ladder linear light (V^2.2)               :", " ".join("%.4f" % float(x) for x in L))
    print("level sRGB codes                          :",
          " ".join(str(int(round(float(x) * 255.0))) for x in V))

    for name, fn in jobs:
        src = os.path.join(DATA, fn)
        if not os.path.exists(src):
            sys.exit("missing source image: %s" % src)
        portrait = load_portrait(src)
        lin = srgb_to_linear(portrait)
        pal_codes, box_means = choose_palette(lin)
        pal_raw = pal_codes.copy()
        pal_codes = refine_palette(lin, pal_codes)
        idx, pal_lin = dither_fs(lin, pal_codes)          # portrait coordinates
        # mean error of the *undithered* nearest-colour mapping, before and after
        def nearest_mae(codes):
            pl = np.array([(L[c & 3], L[(c >> 2) & 3], L[(c >> 4) & 3]) for c in codes],
                          dtype=np.float32)
            dd = ((lin.reshape(-1, 3)[:, None, :] - pl[None, :, :]) ** 2
                  * LUM_W[None, None, :]).sum(axis=2)
            a = np.argmin(dd, axis=1)
            return float(np.abs(lin.reshape(-1, 3) - pl[a]).mean())
        mae_before = nearest_mae(pal_raw)
        mae_after = nearest_mae(pal_codes)
        idx_land = rotate_into_scanout(idx)               # -> landscape scanout
        packed = pack4(idx_land)
        shown_land, _ = render(packed, pal_codes)         # what the raster carries
        shown = rotate_back_to_portrait(shown_land)       # what Stephen sees
        err = shown.astype(np.int16) - portrait.astype(np.int16)
        mae = float(np.abs(err).mean())
        maer = float(np.abs(err[:, :, 0]).mean())
        print("%-16s palette entries: %2d  codes: %s" % (name, pal_codes.size,
              " ".join("%02x" % c for c in pal_codes)))
        print("%-16s linear error, nearest-colour mean: %.4f (median cut) -> %.4f (refined)"
              % ("", mae_before, mae_after))
        print("%-16s dither MAE: %.2f sRGB codes (all channels), %.2f red  "
              "max |err| %d" % ("", mae, maer, int(np.abs(err).max())))
        uniq = np.unique(idx)
        print("%-16s unique indices used: %d of %d, frame bytes %d"
              % ("", uniq.size, pal_codes.size, packed.size))
        results[name] = dict(packed=packed, pal=pal_codes, portrait=portrait, shown=shown)

        if not args.no_preview:
            Image.fromarray(shown).save(
                os.path.join(args.preview_dir, "%s-portrait.png" % name.lower()), optimize=True)
            Image.fromarray(shown_land).save(
                os.path.join(args.preview_dir, "%s-scanout.png" % name.lower()), optimize=True)
            print("%-16s wrote %s-portrait.png / %s-scanout.png"
                  % ("", name.lower(), name.lower()))

    hdr = os.path.join(PROJ, "src", "images16.h")
    with open(hdr, "w") as fh:
        fh.write("// GENERATED by tools/convert_images16.py -- do not edit.\n")
        fh.write("// 640x480, 4 bits per pixel (two pixels per byte, EVEN column in the\n")
        fh.write("// low nibble), already rotated 90 deg clockwise from 480x640 portrait\n")
        fh.write("// content for the landscape scanout. Each nibble is an index into the\n")
        fh.write("// photo's own 16-entry palette (a raw (B<<4)|(G<<2)|R DAC byte), chosen\n")
        fh.write("// by median cut in linear light from the ladder's 64 colours and\n")
        fh.write("// dithered with Floyd-Steinberg in linear light.\n")
        fh.write("#pragma once\n#include <stdint.h>\n\n")
        fh.write("#define IMG16_W %d\n#define IMG16_H %d\n#define IMG16_BYTES %d\n\n"
                 % (LW, LH, LW * LH // 2))
        for name, _ in jobs:
            r = results[name]
            pal = np.zeros(16, dtype=np.uint8)
            pal[:r["pal"].size] = r["pal"]
            fh.write("// %s: %d distinct ladder colours (of 16 slots, of the 64 reachable)\n"
                     % (name, r["pal"].size))
            write_c_array(fh, name + "_PAL", pal)
            fh.write("#define %s_PAL_N %d\n" % (name, r["pal"].size))
            write_c_array(fh, name, r["packed"])
            fh.write("\n")
    print("wrote", hdr)


if __name__ == "__main__":
    main()
