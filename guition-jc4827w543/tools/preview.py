#!/usr/bin/env python3
"""Offline preview of the JC4827W543 bench-panel layout (LCARS-style BMS page).

Mirrors the geometry, colours and draw order of the display lambda in jc4827w543.yaml.
Design here first, then port: this renders in ~1 s and can be looked at, where the panel
is a 10 s flash per iteration and needs eyes in the room.

    python3 tools/preview.py [out.png]

It reports the SOC number's width against the left block it has to fit in, and each reading's
width against its own column, because those are the places this layout can silently overflow.

Caveats (why this is a guide, not proof):
  * PIL renders the TTF with its own antialiasing; ESPHome rasterises the same TTF and blends
    glyph edges against the colour passed as `background`. Glyph widths match closely but not
    pixel-for-pixel.
  * Text is drawn with PIL's 'a' vertical anchor (the ASCENDER line), which is where ESPHome
    puts it: `glyph_to_glyphinfo()` sets each glyph's `offset_y = ascender - bitmap_top`, and
    the renderer draws from `y_start + offset_y`, so TextAlign::TOP_* anchors the ascender
    line, not the ink. PIL's 't' anchor (used here until 2026-10-16) anchors the ink box
    instead, which floated every string 3-17 px above where the panel draws it — 17 px for
    the 80 pt SOC number. 'la' is the faithful anchor; the ink offsets below are the real
    ones, so the same y goes in the lambda and in this file.
  * It draws the lambda's logical canvas -- the physical mounting is set separately by
    `rotation: 180`, which the preview does not model, and the panel's own colour polarity
    is a device setting (`invert_colors`), not something this file can check.
  * The elbow sweep is drawn here exactly as the lambda draws it: a chain of discs along an
    arc. That chain is what makes the joint continuous, so this file is the only place the
    shape can be checked without a panel.

Layout (2026-10-16 geometry pass, nordtronics task 0144):

    y  8..34   the elbow: peach header bar (x 8..471) + periwinkle stub (x 445..471)
    y 42..64   SOC label pill, x 72..183
    y 76..135  the 80 pt SOC number's ink, x 72..207
    y 152..172 the SOC meter, full content width x 72..471
    y 196..218 three stat label pills, x 72..197 / 208..333 / 344..469
    y 226..245 each stat's reading, in that stat's own column

One content left margin (x = 72) throughout, which is clear of the elbow's own footprint
(the swept joint stays left of x = 41 below the header bar). The decorative block row that
used to run along the bottom edge is gone and the meter is the only bar on the page, sitting
directly under the number it belongs to.
"""
import math
import pathlib
import sys

from PIL import Image, ImageDraw, ImageFont

W, H = 480, 272
FONT_TTF = pathlib.Path(__file__).resolve().parents[1] / '.esphome' / 'font' / 'Roboto@400@False@v1.ttf'
BG = (0, 0, 0)

# --- canonical TNG palette (the task's hex list, verbatim) -----------------------------
PEACH = (255, 204, 153)         # #FFCC99  large elbow frame pieces
ORANGE = (255, 153, 0)          # #FF9900  primary frame, main pills, key labels
LORANGE = (255, 204, 102)       # #FFCC66  secondary pills
SALMON = (204, 102, 102)        # #CC6666  accents, warnings
PURPLE = (204, 153, 204)        # #CC99CC  secondary sections
LAVENDER = (153, 153, 204)      # #9999CC  secondary bars
PERIWINKLE = (153, 153, 255)    # #9999FF  secondary bars
PALE = (255, 255, 153)          # #FFFF99  highlights only (the SOC number)
WHITE = (255, 255, 255)         # readings on black
INK = (0, 0, 0)                 # text sitting on a lit bar

# --- geometry (must match the lambda) --------------------------------------------------
BAR = 27                              # bar thickness: odd, so the caps are one 13 px disc
COL_X, COL_Y, COL_H = 8, 8, 256       # left column, y 8..263
HDR = (8, 8, 464)                     # header bar, x 8..471
SWEEP_C, SWEEP_R, SWEEP_DISC = (48, 48), 26, 13    # the joint: arc centre, arc radius, disc
R_ELB = (445, 8, 27, 56)              # right periwinkle stub, x 445..471, y 8..63

CX = 72                               # the single content left margin
HDR_TXT_Y = 9                         # 20 pt header labels (ink 5 px lower)
HDR_FIELDS = ((118, 'panel'), (300, 'clock'), (385, 'date'))
SOC_PILL = (72, 42, 112, 23)          # orange, label "PACK SOC %"
SOC_PILL_TXT_Y = 44
SOC_NUM = (72, 59)                    # 80 pt, TOP_LEFT (ink 17 px lower)
SOC_METER = (72, 152, 400, 21)        # lavender track, the one bar on the page
STAT_PILL_Y, STAT_PILL_W, STAT_PILL_H = 196, 126, 23
STAT_PILL_TXT_Y = 198
STAT_COLS = (72, 208, 344)            # x 72..197 / 208..333 / 344..469
STAT_VAL_Y = 220                      # 28 pt readings (ink 6 px lower), one under each pill
ROWS = ((LORANGE, 'PACK V'), (ORANGE, 'CHARGE'), (PURPLE, 'TEMP'))

# --- values to draw (edit these to check other states) -------------------------------
BMS = dict(soc=86.0, volts=54.54, amps=29.4, temp=58.5, local=91.0, clock='11:19',
           date='Wed 15 Oct', charging=False)
LOW = dict(local=8.0, volts=3.58)


def font(px):
    return ImageFont.truetype(str(FONT_TTF), px)


def ink_offset(s, px):
    """How far the string's topmost ink sits below the ascender line (ESPHome's offset_y)."""
    b = font(px).getbbox(s)
    return b[1]


def text_w(s, px):
    b = font(px).getbbox(s)
    return b[2] - b[0]


def pill(d, x, y, w, h, c):
    """LCARS pill: body plus a disc capping each end.

    h must be odd. ESPHome's filled_circle covers 2r+1 px, so h = 2r+1 makes each cap box
    exactly the end h x h cells of the pill; an even height overhangs by a pixel at each end.
    """
    r = (h - 1) // 2
    d.rectangle([x + r, y, x + w - 1 - r, y + h - 1], fill=c)
    d.ellipse([x, y, x + 2 * r, y + 2 * r], fill=c)
    d.ellipse([x + w - 1 - 2 * r, y, x + w - 1, y + 2 * r], fill=c)


def vbar(d, x, y, w, h, c):
    """The same shape stood on end. The cap radius comes from the width, not the height."""
    r = (w - 1) // 2
    d.rectangle([x, y + r, x + w - 1, y + h - 1 - r], fill=c)
    d.ellipse([x, y, x + 2 * r, y + 2 * r], fill=c)
    d.ellipse([x, y + h - 1 - 2 * r, x + 2 * r, y + h - 1], fill=c)


def sweep(d, cx, cy, r_mid, r_disc, a0, a1, c, step=5):
    """The elbow joint: a chain of discs along the arc, as the lambda draws it.

    This is what makes the joint continuous -- an outer round corner with a CONCAVE inner
    corner. Subtracting a black disc at the corner instead (what this page used to do) eats
    both bars and leaves a blob.
    """
    for a in range(a0, a1 + 1, step):
        ra = math.radians(a)
        x, y = cx + r_mid * math.cos(ra), cy + r_mid * math.sin(ra)
        d.ellipse([x - r_disc, y - r_disc, x + r_disc, y + r_disc], fill=c)


def render_bms(v):
    img = Image.new('RGB', (W, H), BG)
    d = ImageDraw.Draw(img)

    # --- the frame: one asymmetric elbow, joint swept ---------------------------------
    vbar(d, COL_X, COL_Y, BAR, COL_H, PEACH)
    pill(d, *HDR, BAR, PEACH)
    sweep(d, *SWEEP_C, SWEEP_R, SWEEP_DISC, 180, 270, PEACH)
    vbar(d, R_ELB[0], R_ELB[1], R_ELB[2], R_ELB[3], PERIWINKLE)

    # header text, black on the peach frame
    panel = 'PANEL CHG' if v['charging'] else 'PANEL %.0f%%' % v['local']
    for x, field, text in ((HDR_FIELDS[0][0], 'panel', panel),
                           (HDR_FIELDS[1][0], 'clock', v['clock']),
                           (HDR_FIELDS[2][0], 'date', v['date'])):
        d.text((x, HDR_TXT_Y), text, font=font(20), fill=INK, anchor='ma')

    # --- state of charge: label pill, the page's biggest number, then its own meter ----
    pill(d, *SOC_PILL, ORANGE)
    d.text((SOC_PILL[0] + SOC_PILL[2] // 2, SOC_PILL_TXT_Y), 'PACK SOC %', font=font(16),
           fill=INK, anchor='ma')
    isnan = v['soc'] != v['soc']
    d.text(SOC_NUM, '--' if isnan else '%.0f' % v['soc'], font=font(80), fill=PALE, anchor='la')

    mx, my, mw, mh = SOC_METER
    pill(d, mx, my, mw, mh, LAVENDER)
    frac = 0.0 if isnan else max(0.0, min(100.0, v['soc'])) / 100.0
    if frac > 0.0:
        fill = mh + int((mw - mh) * frac)
        pill(d, mx, my, fill, mh, SALMON if v['soc'] < 25 else ORANGE)

    # --- the three readings: one coherent column each, pill above reading --------------
    amps = v['amps']
    values = ('%.2f V' % v['volts'], '%+.1f A' % amps, '%.1f F' % v['temp'])
    labels = ('PACK V', 'CHARGE' if amps >= 0 else 'DISCHG', 'TEMP')
    for x, (bar, _), label, value in zip(STAT_COLS, ROWS, labels, values):
        if label == 'DISCHG':
            bar = SALMON
        pill(d, x, STAT_PILL_Y, STAT_PILL_W, STAT_PILL_H, bar)
        d.text((x + STAT_PILL_W // 2, STAT_PILL_TXT_Y), label, font=font(16), fill=INK,
               anchor='ma')
        d.text((x, STAT_VAL_Y), value, font=font(28), fill=WHITE, anchor='la')

    # --- width report ------------------------------------------------------------------
    # The number's real overflow bound is the content right edge at x = 471; the stat
    # columns start at x = 208 lower down the page, so there is no column to collide with.
    for probe, val in (('9', 9.0), ('86', 86.0), ('100', 100.0)):
        print('  soc %-4s -> %3d px wide, room to x=471: %+d px'
              % (probe, text_w('%.0f' % val, 80), 471 - (SOC_NUM[0] + text_w('%.0f' % val, 80))))
    for x, label, value in zip(STAT_COLS, labels, values):
        print('  %-7s %-8s -> %3d px wide from x=%d, room to x=%d: %+d px'
              % (label, value, text_w(value, 28), x, x + STAT_PILL_W - 1,
                 (x + STAT_PILL_W) - (x + text_w(value, 28))))
    for x, label in zip(STAT_COLS, labels):
        print('  %-7s label  -> %3d px wide in a %d px pill' % (label, text_w(label, 16),
                                                                STAT_PILL_W))
    return img


def render_status(v):
    """The panel's original screen — unchanged, and what the <10% fallback shows."""
    img = Image.new('RGB', (W, H), BG)
    d = ImageDraw.Draw(img)
    d.text((240, 24), 'JC4827W543 up', font=font(24), fill=WHITE, anchor='ma')
    d.text((240, 84), 'BAT %3.0f%%   %.2f V' % (v['local'], v['volts']), font=font(24), fill=WHITE, anchor='ma')
    bx, by, bw, bh, nub, pad = 170, 150, 120, 52, 8, 4
    d.rectangle([bx, by, bx + bw, by + bh], outline=WHITE)
    d.rectangle([bx + bw + 1, by + bh // 3, bx + bw + 1 + nub, by + bh // 3 + bh // 3], fill=WHITE)
    fillw = int((bw - 2 * pad) * v['local'] / 100.0)
    if fillw > 0:
        d.rectangle([bx + pad, by + pad, bx + pad + fillw, by + bh - pad], fill=WHITE)
    return img


def main():
    out = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else 'preview.png')
    pages = [
        ('BMS page  (panel >= 10%) — LCARS', render_bms(BMS)),
        ('status page  (panel < 10%) — unchanged fallback', render_status(LOW)),
    ]
    head = 22
    sheet = Image.new('RGB', (W, (H + head) * len(pages)), (24, 24, 24))
    d = ImageDraw.Draw(sheet)
    for i, (label, img) in enumerate(pages):
        y = i * (H + head)
        d.text((6, y + 5), label, font=font(14), fill=(180, 180, 180), anchor='lt')
        sheet.paste(img, (0, y + head))
        d.rectangle([0, y + head, W - 1, y + head + H - 1], outline=(70, 70, 70))
    sheet.save(out)
    print(f'wrote {out} ({sheet.width}x{sheet.height})')


if __name__ == '__main__':
    main()
