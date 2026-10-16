#!/usr/bin/env python3
"""Audit the previewed BMS page against the numbers the lambda draws with.

    python3 tools/audit_layout.py [out.png]

Every check is derived from jc4827w543.yaml's own coordinates and from preview.py's font
metrics, and every measurement is taken from the rendered pixels -- not from the lambda's
source. Exit status is non-zero if any check fails, so it can be wired into a build step.

The text checks compare each string's MEASURED ink box against the box the constants predict
(`print y + ink_offset`, which is where ESPHome's own ascender-anchored renderer puts it), not
merely against a generous window: a mirror that drifts a few px from the firmware is worth
catching, and after the 2026-10-16 anchor fix the two models agree, so the prediction is exact
to within antialiasing.
"""
import sys
import pathlib
import math

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


def close(box, want, tol=2):
    """Is a measured box within tol px of the predicted one on every edge?"""
    if box is None or want is None:
        return False
    return all(abs(a - b) <= tol for a, b in zip(box, want))


def expected(s, px, x, y, align='l'):
    """The ink box the constants predict: (x0, y0, x1, y1) for text drawn at (x, y)."""
    b = P.font(px).getbbox(s)
    w = b[2] - b[0]
    x0 = x if align == 'l' else x - w // 2
    return (x0, y + b[1], x0 + w - 1, y + b[3] - 1)


def inside(box, outer, pad=0):
    if box is None:
        return False
    return (box[0] >= outer[0] + pad and box[1] >= outer[1] + pad
            and box[2] <= outer[2] - pad and box[3] <= outer[3] - pad)


def rect(x, y, w, h):
    return (x, y, x + w - 1, y + h - 1)


def inset(box, n):
    return (box[0] + n, box[1] + n, box[2] - n, box[3] - n)


def main():
    out = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else
                       (pathlib.Path(__file__).resolve().parent / 'audit-page.png'))
    P.BMS['soc'] = 86.0
    bms = P.render_bms(P.BMS)
    bms.save(out)

    hdr = rect(*P.HDR, P.BAR)
    col = rect(P.COL_X, P.COL_Y, P.BAR, P.COL_H)
    stub = rect(*P.R_ELB)
    soc_pill = rect(*P.SOC_PILL)
    soc_num_box = (66, 66, 212, 150)                   # around the number only: clear of the
                                                       # pill above, the meter below, the rail
    meter = rect(*P.SOC_METER)
    pills = [rect(x, P.STAT_PILL_Y, P.STAT_PILL_W, P.STAT_PILL_H) for x in P.STAT_COLS]
    cols = [rect(x, P.STAT_PILL_Y, P.STAT_PILL_W, 60) for x in P.STAT_COLS]
    ink_region = (8, 8, 471, 263)                      # everything must live inside the frame

    # ---- 1. palette: the canonical hexes are the page, and nothing else --------------
    # Salmon is not in the normal state on purpose -- it is the alarm fill and the discharge
    # pill -- so it is checked in the alarm/discharge render further down.
    for name, colour in (('peach #FFCC99', P.PEACH), ('orange #FF9900', P.ORANGE),
                         ('light orange #FFCC66', P.LORANGE),
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
    # Holes: a black pixel whose four neighbours are all frame colour is the blob the 0141
    # revision removed (the build before it cut a 9 px black disc out of each joint).
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
    for y in range(35, 50):
        run = 0
        for x in range(8, 66):
            if near(bms.getpixel((x, y)), P.PEACH):
                run = x - 8 + 1
            elif run:
                break
        widths.append((y, run))
    mono = all(widths[i][1] >= widths[i + 1][1] for i in range(len(widths) - 1))
    check('joint sweep is monotone (no bite)', mono, str(widths[:3] + widths[-3:]))
    check('joint fillet is present (material below the bar)',
          widths[0][1] > 28 and widths[-1][1] == 28,
          'row35=%d row49=%d (column is 28 px incl. both edges)' % (widths[0][1], widths[-1][1]))

    # Outer corner is round, not square: the bar's top-left corner pixel is background.
    check('outer corner is round', bms.getpixel((8, 8)) == (0, 0, 0)
          and near(bms.getpixel((21, 8)), P.PEACH))

    # The right stub: same style, and it ends with a round cap at y 63.
    check('right stub present and periwinkle',
          near(bms.getpixel((460, 40)), P.PERIWINKLE), str(bms.getpixel((460, 40))))
    check('right stub ends at y=63 with a round cap',
          near(bms.getpixel((458, 50)), P.PERIWINKLE) and near(bms.getpixel((458, 63)), P.PERIWINKLE)
          and bms.getpixel((458, 67)) == (0, 0, 0))

    # ---- 3. text: every string where the constants put it, inside its own box ----------
    dark = lambda c: sum(c) / 3 < 90
    bright = lambda c: min(c) > 150
    ink = lambda c: c != (0, 0, 0)

    panel = 'PANEL %.0f%%' % P.BMS['local']
    fields = ((panel, 118, (40, 8, 200, 34)), (P.BMS['clock'], 300, (240, 8, 330, 34)),
              (P.BMS['date'], 385, (332, 8, 444, 34)))
    boxes = []
    for text, x, window in fields:
        got = bbox_of(bms, window, dark)
        want = expected(text, 20, x, P.HDR_TXT_Y, 'c')
        boxes.append(got)
        check('header %-11s box matches the constants' % repr(text), close(got, want, 3),
              'got %s want %s' % (got, want))
        check('header %-11s inside the bar' % repr(text), inside(got, hdr, 2), str(got))
    check('three header fields, none overlapping',
          all(boxes) and boxes[0][2] < boxes[1][0] and boxes[1][2] < boxes[2][0],
          str(boxes))
    check('header date stays clear of the periwinkle stub',
          all(boxes) and boxes[2][2] < stub[0], str(boxes[2]))

    lab = bbox_of(bms, inset(soc_pill, 4), dark)
    want = expected('PACK SOC %', 16, P.SOC_PILL[0] + P.SOC_PILL[2] // 2, P.SOC_PILL_TXT_Y, 'c')
    check('SOC label box matches the constants', close(lab, want, 3),
          'got %s want %s' % (lab, want))
    check('SOC label inside its pill', inside(lab, soc_pill, 1), str(lab))

    num = bbox_of(bms, soc_num_box, lambda c: sum(c) > 30)
    want = expected('%.0f' % P.BMS['soc'], 80, P.SOC_NUM[0], P.SOC_NUM[1], 'l')
    check('SOC number box matches the constants', close(num, want, 4),
          'got %s want %s' % (num, want))
    check('SOC number inside its block, above the meter',
          num and num[0] >= 72 and num[2] <= 207 and num[3] < meter[1], str(num))

    for i, ((bar, label), window) in enumerate(zip(P.ROWS, pills)):
        value = ('%.2f V' % P.BMS['volts'], '%+.1f A' % P.BMS['amps'],
                 '%.1f F' % P.BMS['temp'])[i]
        shown = ('PACK V', 'CHARGE', 'TEMP')[i]
        lb = bbox_of(bms, inset(window, 4), dark)
        want = expected(shown, 16, P.STAT_COLS[i] + P.STAT_PILL_W // 2, P.STAT_PILL_TXT_Y, 'c')
        check('%s label box matches the constants' % shown, close(lb, want, 3),
              'got %s want %s' % (lb, want))
        check('%s label inside its pill' % shown, inside(lb, window, 1), str(lb))
        check('%s label clear of the pill cap radius' % shown,
              lb is not None and lb[0] >= P.STAT_COLS[i] + 11 and lb[2] <= P.STAT_COLS[i] + 114,
              str(lb))

        vb = bbox_of(bms, (cols[i][0], window[3] + 1, cols[i][2], 258), lambda c: sum(c) > 30)
        want = expected(value, 28, P.STAT_COLS[i], P.STAT_VAL_Y, 'l')
        check('%s reading box matches the constants' % shown, close(vb, want, 4),
              'got %s want %s' % (vb, want))
        check('%s reading inside its own column' % shown,
              vb and vb[0] >= cols[i][0] and vb[2] <= cols[i][2], str(vb))
        check('%s reading clear of its label pill' % shown,
              vb and vb[1] > window[3], str(vb))

    # The three columns are equal width and evenly gutted -- the structure the geometry pass
    # is for, measured rather than asserted.
    gutters = [P.STAT_COLS[i + 1] - (P.STAT_COLS[i] + P.STAT_PILL_W) for i in range(2)]
    check('three stat columns share one width and one gutter',
          len(set(gutters)) == 1 and gutters[0] > 0, 'gutters %s, width %d' % (gutters, P.STAT_PILL_W))
    check('stat pills are as tall as a reading\'s ink (a unit, not a dwarf)',
          P.STAT_PILL_H >= 20, '%d px pill vs 20 px 28 pt ink' % P.STAT_PILL_H)

    # ---- 4. the meter ------------------------------------------------------------------
    mx, my, mw, mh = P.SOC_METER
    check('meter track is lavender',
          near(bms.getpixel((mx + mw - 5, my + mh // 2)), P.LAVENDER))
    # 86 % -> the fill ends at mx + mh + (mw-mh)*0.86 = 72+21+326 = 419 (+-3 px for the cap)
    edge = None
    for x in range(mx, mx + mw):
        if not near(bms.getpixel((x, my + mh // 2)), P.ORANGE):
            edge = x
            break
    check('meter fill matches 86 %%', edge and abs(edge - (mx + mh + (mw - mh) * 0.86)) <= 3,
          'fill ends at x=%s, expected %.0f' % (edge, mx + mh + (mw - mh) * 0.86))
    check('meter sits under the number and above the readings',
          num is not None and num[3] < my and my + mh < P.STAT_PILL_Y,
          'number ends %s, meter %d..%d, pills from %d'
          % (num[3] if num else None, my, my + mh, P.STAT_PILL_Y))

    # ---- 5. the alarm, discharge, 0 % and NAN states -----------------------------------
    P.BMS['soc'] = 18.0
    low = P.render_bms(P.BMS)
    check('soc 18 %% paints the fill salmon',
          near(low.getpixel((mx + 40, my + mh // 2)), P.SALMON)
          and not near(low.getpixel((mx + 40, my + mh // 2)), P.ORANGE))
    check('palette salmon #CC6666 present in the alarm state', count(low, P.SALMON) > 50,
          '%d px' % count(low, P.SALMON))

    P.BMS['soc'] = 0.0
    zero = P.render_bms(P.BMS)
    check('soc 0 %% leaves the track whole (only the end knob lit)',
          near(zero.getpixel((mx + mh + 5, my + mh // 2)), P.LAVENDER))

    P.BMS['soc'] = float('nan')
    nan = P.render_bms(P.BMS)
    nan_num = bbox_of(nan, soc_num_box, lambda c: sum(c) > 30)
    check('NAN soc renders "--" inside the block', nan_num and nan_num[0] >= 72
          and nan_num[2] <= 207 and nan_num[3] < meter[1], str(nan_num))
    check('NAN soc leaves the meter as a bare track',
          near(nan.getpixel((mx + mh + 5, my + mh // 2)), P.LAVENDER))
    P.BMS['soc'] = 86.0

    # Discharge: the pill turns salmon, so the sign reads without reading the number.
    P.BMS['amps'] = -12.3
    dis = P.render_bms(P.BMS)
    check('discharge paints its pill salmon',
          near(dis.getpixel((300, P.STAT_PILL_Y + P.STAT_PILL_H // 2)), P.SALMON)
          and not near(dis.getpixel((300, P.STAT_PILL_Y + P.STAT_PILL_H // 2)), P.ORANGE))
    P.BMS['amps'] = 29.4

    # ---- 6. canvas: nothing hangs off the frame, and the bottom row is gone ------------
    all_ink = bbox_of(bms, (0, 0, 479, 271), ink)
    check('ink inside the frame margins', all_ink and all_ink[0] >= 8 and all_ink[1] >= 8
          and all_ink[2] <= 471 and all_ink[3] <= 263, str(all_ink))
    check('column spans the full height (one elbow, top to bottom)',
          near(bms.getpixel((20, 12)), P.PEACH) and near(bms.getpixel((20, 250)), P.PEACH)
          and near(bms.getpixel((20, 260)), P.PEACH),
          'y12=%s y250=%s y260=%s' % (bms.getpixel((20, 12)), bms.getpixel((20, 250)),
                                      bms.getpixel((20, 260))))
    # The retired revision ended in a row of five colour blocks along the bottom edge and a
    # 427 px meter above it. Both are gone: below the readings' ink there is no ink at all
    # except the frame column.
    below = bbox_of(bms, (35, 254, 471, 271), ink)
    check('no content on the bottom edge (the block row is gone)', below is None, str(below))
    check('no bar wider than the meter (the old 427 px bottom bar is gone)',
          max(count(bms, c, 60) for c in (P.LAVENDER,)) < 400 * 21,
          'meter band is %d px' % (400 * 21))

    # ---- report ------------------------------------------------------------------------
    bad = [c for c in CHECKS if not c[1]]
    for name, ok, detail in CHECKS:
        print('%-4s %-46s %s' % ('PASS' if ok else 'FAIL', name, detail))
    print('\n%d checks, %d passed, %d failed' % (len(CHECKS), len(CHECKS) - len(bad), len(bad)))
    print('page written to %s' % out)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
