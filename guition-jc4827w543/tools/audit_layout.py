#!/usr/bin/env python3
"""Audit the previewed BMS page against the numbers the lambda draws with.

    python3 tools/audit_layout.py [out.png]

Every check is derived from jc4827w543.yaml's own coordinates, and every measurement is taken
from the rendered pixels -- not from the lambda's source. Exit status is non-zero if any check
fails, so it can be wired into a build step.
"""
import sys
import pathlib
import math
import collections

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from PIL import Image  # noqa: E402
import preview as P  # noqa: E402

CHECKS = []


def check(name, ok, detail=''):
    CHECKS.append((name, bool(ok), detail))


def near(c, target, tol=24):
    return sum(abs(a - b) for a, b in zip(c, target)) <= tol


def count(img, colour, tol=24):
    return sum(1 for c in img.getdata() if near(c, colour, tol))


def bbox_of(img, box, pred):
    """Bounding box of pixels satisfying pred() inside box=(x0,y0,x1,y1)."""
    x0, y0, x1, y1 = box
    xs, ys = [], []
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            if pred(img.getpixel((x, y))):
                xs.append(x)
                ys.append(y)
    if not xs:
        return None
    return (min(xs), min(ys), max(xs), max(ys))


def main():
    out = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else
                       (pathlib.Path(__file__).resolve().parent / 'audit-page.png'))
    P.BMS['soc'] = 86.0
    bms = P.render_bms(P.BMS)
    bms.save(out)

    # ---- 1. palette: the canonical hexes are the page, and nothing else --------------
    for name, colour in (('peach #FFCC99', P.PEACH), ('orange #FF9900', P.ORANGE),
                         ('light orange #FFCC66', P.LORANGE), ('salmon #CC6666', P.SALMON),
                         ('purple #CC99CC', P.PURPLE), ('lavender #9999CC', P.LAVENDER),
                         ('periwinkle #9999FF', P.PERIWINKLE), ('pale #FFFF99', P.PALE),
                         ('white', P.WHITE)):
        n = count(bms, colour)
        check('palette %-22s present' % name, n > 50, '%d px' % n)

    # No green and no cyan anywhere: the banned families, tested by hue, not by hex.
    banned = 0
    for c in bms.getdata():
        r, g, b = c
        if g > 90 and g > r + 45 and g > b + 45:
            banned += 1          # green / chartreuse
        elif b > 90 and g > 90 and b > r + 45 and g > r + 45:
            banned += 1          # cyan / sky blue
    check('no green and no cyan pixels', banned == 0, '%d px' % banned)

    # The colours the previous revision used and this one retires. A solid fill of any of
    # them would mean a stale constant survived the rewrite. (Antialiased glyph edges are
    # blends of palette colours, so this is a nearest-colour test with a tight tolerance.)
    for name, old in (('retired #FFFFCC pale', (255, 255, 204)),
                      ('retired #2C2C50 dim navy', (44, 44, 80)),
                      ('retired #FFCCCC pale pink', (255, 204, 204))):
        n = count(bms, old, 4)
        check('no fill of the %s' % name, n == 0, '%d px' % n)

    # ---- 2. the frame elbows: continuous, no subtracted blob --------------------------
    # Holes: a black pixel whose four neighbours are all frame colour is the blob this
    # revision removes (the old build cut a 9 px black disc out of each joint).
    holes = 0
    for y in range(5, 263):
        for x in range(9, 471):
            if bms.getpixel((x, y)) == (0, 0, 0):
                n = [bms.getpixel((x + dx, y + dy)) for dx, dy in
                     ((-1, 0), (1, 0), (0, -1), (0, 1))]
                if all(near(c, P.PEACH) for c in n):
                    holes += 1
    check('no black blob inside the elbow band', holes == 0, '%d px' % holes)

    # The top-left joint: single material run per row, boundary monotone, fillet present.
    widths = []
    for y in range(31, 46):
        run = 0
        for x in range(8, 60):
            if near(bms.getpixel((x, y)), P.PEACH):
                run = x - 8 + 1
            elif run:
                break
        widths.append((y, run))
    mono = all(widths[i][1] >= widths[i + 1][1] for i in range(len(widths) - 1))
    check('joint sweep is monotone (no bite)', mono, str(widths[:4] + widths[-3:]))
    check('joint fillet is present (material below the bar)',
          widths[0][1] > 28 and widths[-1][1] == 28,
          'row31=%d row45=%d (column is 28 px incl. both edges)' % (widths[0][1], widths[-1][1]))

    # Outer corner is round, not square: the bar's top-left corner pixel is background.
    check('outer corner is round', bms.getpixel((8, 4)) == (0, 0, 0)
          and near(bms.getpixel((21, 4)), P.PEACH))

    # The right stub: same style, and it ends with a round cap at y 63.
    check('right stub present and periwinkle',
          near(bms.getpixel((460, 40)), P.PERIWINKLE), str(bms.getpixel((460, 40))))
    check('right stub ends at y=63 with a round cap',
          near(bms.getpixel((458, 50)), P.PERIWINKLE) and near(bms.getpixel((458, 63)), P.PERIWINKLE)
          and bms.getpixel((458, 67)) == (0, 0, 0))

    # ---- 3. text boxes: each string inside its own column ------------------------------
    dark = lambda c: sum(c) / 3 < 90
    bright = lambda c: min(c) > 150

    hdr = bbox_of(bms, (44, 8, 462, 26), dark)      # only the peach bar carries dark text
    check('header ink inside the bar', hdr and hdr[1] >= 8 and hdr[3] <= 26, str(hdr))
    h1 = bbox_of(bms, (44, 8, 200, 26), dark)
    h2 = bbox_of(bms, (240, 8, 330, 26), dark)
    h3 = bbox_of(bms, (350, 8, 462, 26), dark)
    check('three header fields, none overlapping',
          h1 and h2 and h3 and h1[2] < h2[0] and h2[2] < h3[0],
          'panel %s clock %s date %s' % (h1, h2, h3))

    lab = bbox_of(bms, (52, 52, 155, 68), dark)
    check('PACK SOC %% label inside its pill', lab and lab[0] >= 52 and lab[2] <= 155, str(lab))

    num = bbox_of(bms, (44, 70, 250, 160), lambda c: near(c, P.PALE))
    check('SOC number inside its block and left of the readings',
          num and num[0] >= 44 and num[2] <= 236 and num[1] >= 70 and num[3] <= 165, str(num))

    for (y, _, label), col in ((P.ROWS[0], 'PACK V'), (P.ROWS[1], 'CHARGE'), (P.ROWS[2], 'TEMP')):
        lb = bbox_of(bms, (259, y + 2, 325, y + 12), dark)   # inside the pill, clear of its antialiased edge
        vb = bbox_of(bms, (340, y - 6, 471, y + 30), lambda c: near(c, P.WHITE))
        check('%s pill label inside its pill' % col, lb and lb[0] >= 259 and lb[2] <= 325, str(lb))
        check('%s reading inside the right margin' % col, vb and vb[2] <= 471 and vb[3] < 212, str(vb))
        check('%s reading clear of the label pill' % col, vb and vb[0] > 335, str(vb))

    # ---- 4. the meter ------------------------------------------------------------------
    mx, my, mw, mh = P.METER
    inside = lambda x: mx <= x <= mx + mw - 1 and my <= y <= my + mh - 1
    check('meter track is lavender',
          near(bms.getpixel((mx + mw - 5, my + mh // 2)), P.LAVENDER))
    # 86 % -> the fill ends at mx + mh + (mw-mh)*0.86 = 44+21+349 = 414 (+-3 px for the cap)
    edge = None
    for x in range(mx, mx + mw):
        if not near(bms.getpixel((x, my + mh // 2)), P.ORANGE):
            edge = x
            break
    check('meter fill matches 86 %%', edge and abs(edge - (mx + mh + (mw - mh) * 0.86)) <= 3,
          'fill ends at x=%s, expected %.0f' % (edge, mx + mh + (mw - mh) * 0.86))
    check('meter sits above the footer', my + mh < P.FOOT_Y, '%d < %d' % (my + mh, P.FOOT_Y))

    # ---- 5. the alarm and NAN states ---------------------------------------------------
    P.BMS['soc'] = 18.0
    low = P.render_bms(P.BMS)
    check('soc 18 %% paints the fill salmon',
          near(low.getpixel((60, my + mh // 2)), P.SALMON)
          and not near(low.getpixel((60, my + mh // 2)), P.ORANGE))
    P.BMS['soc'] = 0.0
    zero = P.render_bms(P.BMS)
    check('soc 0 %% leaves the track whole (only the end knob lit)',
          near(zero.getpixel((mx + mh + 5, my + mh // 2)), P.LAVENDER))

    P.BMS['soc'] = float('nan')
    nan = P.render_bms(P.BMS)
    nan_num = bbox_of(nan, (44, 70, 250, 165), lambda c: near(c, P.PALE))
    check('NAN soc renders "--" inside the block',
          nan_num and nan_num[0] >= 44 and nan_num[2] <= 236, str(nan_num))
    check('NAN soc leaves the meter as a bare track',
          near(nan.getpixel((mx + mh + 5, my + mh // 2)), P.LAVENDER))
    P.BMS['soc'] = 86.0

    # ---- 6. canvas ---------------------------------------------------------------------
    ink = bbox_of(bms, (0, 0, 479, 271), lambda c: c != (0, 0, 0))
    check('ink inside the canvas', ink and ink[0] >= 8 and ink[1] >= 4 and ink[2] <= 471,
          str(ink))
    check('column spans the full height (one elbow, top to foot)',
          near(bms.getpixel((20, 8)), P.PEACH) and near(bms.getpixel((20, 250)), P.PEACH)
          and near(bms.getpixel((20, 260)), P.ORANGE),   # the footer's first block covers the foot
          'y8=%s y250=%s y260=%s' % (bms.getpixel((20, 8)), bms.getpixel((20, 250)),
                                     bms.getpixel((20, 260))))
    check('footer is the reference row of five blocks',
          all(near(bms.getpixel((x, 259)), c) for x, c in
              ((20, P.ORANGE), (140, P.PEACH), (230, P.SALMON), (350, P.LAVENDER), (450, P.PURPLE))))

    # ---- report ------------------------------------------------------------------------
    bad = [c for c in CHECKS if not c[1]]
    for name, ok, detail in CHECKS:
        print('%-4s %-46s %s' % ('PASS' if ok else 'FAIL', name, detail))
    print('\n%d checks, %d passed, %d failed' % (len(CHECKS), len(CHECKS) - len(bad), len(bad)))
    print('page written to %s' % out)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
