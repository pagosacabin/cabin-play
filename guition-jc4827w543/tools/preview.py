#!/usr/bin/env python3
"""Offline preview of the JC4827W543 bench-panel layout (LCARS-style BMS page).

Mirrors the geometry, colours and draw order of the display lambda in jc4827w543.yaml.
Design here first, then port: this renders in ~1 s and can be looked at, where the panel
is a 10 s flash per iteration and needs eyes in the room.

    python3 tools/preview.py [out.png]

It reports the SOC number's width against the left block it has to fit in, and each reading's
width against the right margin, because those are the two places this layout can silently
overflow its column.

Caveats (why this is a guide, not proof):
  * PIL renders the TTF with its own antialiasing; ESPHome rasterises the same TTF into a
    4bpp bitmap font and blends glyph edges against the colour passed as `background`.
    Glyph widths match closely but not pixel-for-pixel.
  * ESPHome anchors TextAlign::TOP_* on the glyph bounding box; PIL's 't' anchors sit on
    the line box. Expect a pixel or two of vertical disagreement.
  * It draws the lambda's logical canvas — the physical mounting is set separately by
    `rotation: 180`, which the preview does not model, and the panel's own colour polarity
    is a device setting (`invert_colors`), not something this file can check.
  * The elbow sweep is drawn here exactly as the lambda draws it: a chain of discs along an
    arc. That chain is what makes the joint continuous, so this file is the only place the
    shape can be checked without a panel.
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
COL_X, COL_Y, COL_H = 8, 4, 260       # left column, y 4..263
HDR = (8, 4, 464)                     # header bar, x 8..471
SWEEP_C, SWEEP_R, SWEEP_DISC = (48, 44), 26, 13    # the joint: arc centre, arc radius, disc
R_ELB = (445, 4, 27, 60)              # right periwinkle stub, x 445..471, y 4..63
RULE = (44, 36, 393, 7)               # purple rule under the header
SOC_PILL = (44, 52, 120, 17)
SOC_NUM = (44, 76)                    # 80 pt, TOP_LEFT
METER = (44, 212, 427, 21)            # lavendar track, orange fill drawn over it
ROWS = ((52, LORANGE, 'PACK V'), (96, ORANGE, 'CHARGE'), (140, PURPLE, 'TEMP'))
FOOT_Y, FOOT_H = 255, 9

# --- values to draw (edit these to check other states) -------------------------------
BMS = dict(soc=86.0, volts=54.54, amps=29.4, temp=58.5, local=91.0, clock='11:19',
           date='Wed 15 Oct', charging=False)
LOW = dict(local=8.0, volts=3.58)


def font(px):
    return ImageFont.truetype(str(FONT_TTF), px)


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
    d.text((107, 6), 'PANEL CHG' if v['charging'] else 'PANEL %.0f%%' % v['local'],
           font=font(20), fill=INK, anchor='mt')
    d.text((285, 6), v['clock'], font=font(20), fill=INK, anchor='mt')
    d.text((410, 6), v['date'], font=font(20), fill=INK, anchor='mt')

    pill(d, *RULE, PURPLE)

    # --- state of charge: label pill, the page's biggest number, two-tone bar ----------
    pill(d, *SOC_PILL, ORANGE)
    d.text((104, 55), 'PACK SOC %', font=font(16), fill=INK, anchor='mt')
    isnan = v['soc'] != v['soc']
    d.text(SOC_NUM, '--' if isnan else '%.0f' % v['soc'], font=font(80), fill=PALE, anchor='lt')

    mx, my, mw, mh = METER
    pill(d, mx, my, mw, mh, LAVENDER)
    frac = 0.0 if isnan else max(0.0, min(100.0, v['soc'])) / 100.0
    if frac > 0.0:
        fill = mh + int((mw - mh) * frac)
        pill(d, mx, my, fill, mh, SALMON if v['soc'] < 25 else ORANGE)

    # --- the three readings: label pill, value white on black --------------------------
    for y, colour, label in ROWS:
        pill(d, 250, y, 85, 15, colour)
        d.text((292, y + 2), label, font=font(16), fill=INK, anchor='mt')
    for (y, _, _), value in zip(ROWS, ('%.2f V' % v['volts'], '%+.1f A' % v['amps'],
                                       '%.1f F' % v['temp'])):
        d.text((342, y - 4), value, font=font(28), fill=WHITE, anchor='lt')

    # --- LCARS footer: the reference's row of colour blocks ----------------------------
    bx = 8
    for colour, bw in ((ORANGE, 110), (PEACH, 80), (SALMON, 60), (LAVENDER, 110), (PURPLE, 80)):
        d.rectangle([bx, FOOT_Y, bx + bw - 1, FOOT_Y + FOOT_H - 1], fill=colour)
        bx += bw + 6

    # --- column overflow report --------------------------------------------------------
    for probe, val in (('9', 9.0), ('86', 86.0), ('100', 100.0)):
        print('  soc %-4s -> %3d px wide, room to x=236: %+d px'
              % (probe, text_w('%.0f' % val, 80), 236 - (SOC_NUM[0] + text_w('%.0f' % val, 80))))
    for (y, _, label), value in zip(ROWS, ('%.2f V' % v['volts'], '%+.1f A' % v['amps'],
                                           '%.1f F' % v['temp'])):
        print('  %-7s %-8s -> %3d px wide from x=342, room: %+d px'
              % (label, value, text_w(value, 28), 471 - (342 + text_w(value, 28))))
    return img


def render_status(v):
    """The panel's original screen — unchanged, and what the <10% fallback shows."""
    img = Image.new('RGB', (W, H), BG)
    d = ImageDraw.Draw(img)
    d.text((240, 24), 'JC4827W543 up', font=font(24), fill=WHITE, anchor='mt')
    d.text((240, 84), 'BAT %3.0f%%   %.2f V' % (v['local'], v['volts']), font=font(24), fill=WHITE, anchor='mt')
    bx, by, bw, bh, nub, pad = 170, 150, 120, 52, 8, 4
    d.rectangle([bx, by, bx + bw, by + bh], outline=WHITE)
    d.rectangle([bx + bw + 1, by + bh // 3, bx + bw + 1 + nub, by + bh // 3 + bh // 3], fill=WHITE)
    fillw = int((bw - 2 * pad) * v['local'] / 100.0)
    if fillw > 0:
        d.rectangle([bx + pad, by + pad, bx + pad + fillw, by + bh - pad], fill=WHITE)
    return img


GRID = (84, 84, 120)                    # scope rings / inactive track (not in the TNG list)
TRACK = dict(presence=True, dist=1.4, speed=0.04, energy=99.0, sweep_deg=35)
TRACK_CLEAR = dict(presence=False, dist=0.0, speed=0.0, energy=0.0, sweep_deg=200)
SCOPE = (118, 152, 92)                  # centre x, centre y, radius


def render_tracker(v):
    """The proximity page: three-ring scope, one arm, the blip where the arm is.

    Mirrors the tracker branch of the lambda. The blip is drawn at the arm's current angle --
    a single-range mmWave sensor reports no bearing, so there is no honest place to park it.
    """
    img = Image.new('RGB', (W, H), BG)
    d = ImageDraw.Draw(img)
    contact = v['presence']

    d.rectangle([0, 0, W - 1, 31], fill=ORANGE)
    d.text((10, 6), 'PROXIMITY SCAN', font=font(20), fill=INK, anchor='lt')
    flag = SALMON if contact else LAVENDER
    d.rectangle([368, 5, 471, 26], fill=flag)
    d.text((420, 8), 'CONTACT' if contact else 'CLEAR', font=font(16), fill=INK, anchor='mt')

    cx, cy, R = SCOPE
    d.ellipse([cx - R, cy - R, cx + R, cy + R], outline=PEACH)
    for rr in (R * 2 // 3, R // 3):
        d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], outline=GRID)
    for x0, y0, x1, y1 in ((cx - R, cy, cx - R + 7, cy), (cx + R, cy, cx + R - 7, cy),
                           (cx, cy - R, cx, cy - R + 7), (cx, cy + R, cx, cy + R - 7)):
        d.line([x0, y0, x1, y1], fill=PEACH)

    arm = math.radians(v['sweep_deg'])
    for t in range(4, 0, -1):
        a = arm - t * 0.16
        d.line([cx, cy, cx + R * math.cos(a), cy + R * math.sin(a)],
               fill=GRID if t >= 3 else LAVENDER)
    d.line([cx, cy, cx + R * math.cos(arm), cy + R * math.sin(arm)], fill=PALE)
    d.ellipse([cx - 3, cy - 3, cx + 3, cy + 3], fill=PALE)

    if contact:
        m = min(max(v['dist'], 0.0), 6.0)          # scope scaled to 6 m
        br = 14 + int((R - 18) * m / 6.0)
        for t in range(3, 0, -1):
            a = arm - t * 0.16
            x, y = cx + br * math.cos(a), cy + br * math.sin(a)
            rr = 5 - t
            d.ellipse([x - rr, y - rr, x + rr, y + rr], fill=SALMON)
        x, y = cx + br * math.cos(arm), cy + br * math.sin(arm)
        d.ellipse([x - 4, y - 4, x + 4, y + 4], fill=PALE)
        d.ellipse([x - 9, y - 9, x + 9, y + 9], outline=LORANGE)

    pill(d, 232, 40, 92, 15, LORANGE)
    d.text((278, 41), 'RANGE', font=font(16), fill=INK, anchor='mt')
    d.text((420, 58), '--' if v['dist'] is None else '%.1f' % v['dist'],
           font=font(80), fill=PALE, anchor='rt')
    d.text((428, 122), 'm', font=font(24), fill=WHITE, anchor='lt')

    pill(d, 232, 158, 92, 15, LORANGE)
    d.text((278, 159), 'SPEED', font=font(16), fill=INK, anchor='mt')
    d.text((330, 152), '%+.2f m/s' % v['speed'], font=font(28), fill=WHITE, anchor='lt')

    pill(d, 232, 196, 92, 15, SALMON)
    d.text((278, 197), 'ENERGY', font=font(16), fill=INK, anchor='mt')
    d.rectangle([330, 198, 330 + 117, 198 + 11], fill=GRID)
    e = min(max(v['energy'], 0.0), 100.0)
    if e > 0:
        d.rectangle([330, 198, 330 + int(117 * e / 100.0), 198 + 11], fill=PALE)

    d.text((232, 226), 'TARGET TRACKED' if contact else 'SCANNING  //  NO CONTACT',
           font=font(16), fill=SALMON if contact else LAVENDER, anchor='lt')

    tx = 8
    for w, c in zip((110, 80, 60, 110, 80), (ORANGE, PEACH, SALMON, LAVENDER, LORANGE)):
        d.rectangle([tx, 255, tx + w - 1, 263], fill=c)
        tx += w + 6
    return img


def main():
    out = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else 'preview.png')
    pages = [
        ('BMS page  (panel >= 10%) — LCARS', render_bms(BMS)),
        ('status page  (panel < 10%) — unchanged fallback', render_status(LOW)),
    ]
    # The tracker rows are opt-in: preview-bms-lcars.png is the audit's reference sheet
    # (tools/audit_layout.py measures the console page's text boxes in it), and adding rows to
    # that artifact would move every box the audit checks.
    if '--with-tracker' in sys.argv:
        pages += [
            ('tracker page  (C4002 contact) — proximity scope', render_tracker(TRACK)),
            ('tracker page  (no contact) — still on its dwell', render_tracker(TRACK_CLEAR)),
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
