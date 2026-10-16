#!/usr/bin/env python3
"""Offline preview of the JC4827W543 bench-panel layout (LCARS-style BMS page).

Mirrors the geometry, colours and draw order of the display lambda in jc4827w543.yaml.
Design here first, then port: this renders in ~1 s and can be looked at, where the panel
is a 10 s flash per iteration and needs eyes in the room.

    python3 tools/preview.py [out.png]

It reports the SOC readout's half-width against the dial's inner radius, because that is
the one place this layout can silently collide with the ring.

Caveats (why this is a guide, not proof):
  * PIL renders the TTF with its own antialiasing; ESPHome rasterises the same TTF into a
    4bpp bitmap font and blends glyph edges against the colour passed as `background`.
    Glyph widths match closely but not pixel-for-pixel.
  * ESPHome anchors TextAlign::TOP_* on the glyph bounding box; PIL's 't' anchors sit on
    the line box. Expect a pixel or two of vertical disagreement.
  * It draws the lambda's logical canvas — the physical mounting is set separately by
    `rotation: 180`, which the preview does not model.
  * The elbows are drawn here exactly as the lambda draws them: two bars plus a black
    disc subtracted from the inner corner. That subtraction is a fill-order trick, so
    this file is the only place the shape can be checked without a panel.
"""
import math
import pathlib
import sys

from PIL import Image, ImageDraw, ImageFont

W, H = 480, 272
FONT_TTF = pathlib.Path(__file__).resolve().parents[1] / '.esphome' / 'font' / 'Roboto@400@False@v1.ttf'
BG = (0, 0, 0)

# --- LCARS palette (the hex list from the 0140 restyle, verbatim) ----------------------
ORANGE = (255, 153, 0)          # #FF9900
GOLD = (255, 204, 153)          # #FFCC99
SALMON = (204, 102, 102)        # #CC6666
PURPLE = (204, 153, 204)        # #CC99CC
PERIWINKLE = (153, 153, 255)    # #9999FF
PALE = (255, 255, 204)          # #FFFFCC
DIM = (44, 44, 80)              # unlit arc segments (functional, not in the hex list)
INK = (0, 0, 0)                 # text sitting on a lit bar
WHITE = (255, 255, 255)         # status page only

# --- dial geometry (must match the lambda) -------------------------------------------
CX, CY = 258, 118
R_MID, DISC_R = 60, 6                 # ring is 2*DISC_R thick (r 54..66), centred on R_MID
SWEEP_START, SWEEP_DEG = 120, 300     # 0 deg = 3 o'clock, positive = clockwise on screen
SEG_DEG = 3                           # disc spacing along the arc

# --- frame geometry (must match the lambda) ------------------------------------------
BAR = 27                              # column / header bar thickness: odd, so the caps
                                      # are the same 13 px disc and the corner is round
COL_X = 8
COL_Y, COL_H = 4, 260                 # left column, y 4..263
R_ELB_X, R_ELB_W, R_ELB_H = 445, 27, 60
HDR1 = (8, 4, 202)                    # x, y, w — panel segment
HDR2 = (240, 4, 232)                  # x, y, w — clock segment
JOIN = (206, 11, 38, 11)              # the bar linking them
FIL_R = 9                             # concave corner radius
DIV = (44, 35, 393, 7)
FOOT_Y, FOOT_H = 255, 9

# --- values to draw (edit these to check other states) -------------------------------
BMS = dict(soc=86.0, volts=54.54, amps=29.4, temp=58.5, local=91.0, clock='11:19', date='Wed 15 Oct',
           charging=False)   # set True to preview the header's CHG state (on the charger)
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


def fillet(d, cx, cy, r=9):
    """The concave inner corner: a black disc subtracted after the bars are drawn."""
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=BG)


def dial(d, soc):
    """A chain of overlapping discs along the arc, as the lambda draws it: discs cannot seam
    (the quad version left 1 px gaps along shared edges) and their round ends come free."""
    lit = SALMON if soc < 25 else ORANGE
    frac = max(0.0, min(100.0, soc)) / 100.0
    for a in range(SWEEP_START, SWEEP_START + SWEEP_DEG + 1, SEG_DEG):
        f = (a - SWEEP_START) / SWEEP_DEG
        c = lit if f <= frac else DIM
        ra = math.radians(a)
        x, y = CX + R_MID * math.cos(ra), CY + R_MID * math.sin(ra)
        d.ellipse([x - DISC_R, y - DISC_R, x + DISC_R, y + DISC_R], fill=c)


def render_bms(v):
    img = Image.new('RGB', (W, H), BG)
    d = ImageDraw.Draw(img)

    # --- the frame: two elbows, both built as bar + bar - black disc ----------------
    # Left column and header share a corner disc, so their union is one L. The clock
    # segment gets the mirrored treatment at its right end, and the column turns into the
    # footer bar at the bottom.
    vbar(d, COL_X, COL_Y, BAR, COL_H, ORANGE)
    vbar(d, R_ELB_X, 4, R_ELB_W, R_ELB_H, PERIWINKLE)
    pill(d, *HDR1, BAR, ORANGE)
    pill(d, *HDR2, BAR, PERIWINKLE)
    pill(d, *JOIN, GOLD)
    fillet(d, 34, 30)
    fillet(d, R_ELB_X, 30)
    d.text((8 + 101, 7), 'PANEL CHG' if v['charging'] else 'PANEL %.0f%%' % v['local'],
           font=font(20), fill=INK, anchor='mt')
    d.text((240 + 116, 7), v['clock'], font=font(20), fill=INK, anchor='mt')
    pill(d, *DIV, GOLD)

    # --- dial, with its label and the date bookending it ---
    # The number is drawn WITHOUT the % sign and with nothing else inside the ring: at
    # 48 pt a 3-digit reading plus "%" is wider than the ring's inner diameter, and a label
    # under the number crosses the ring's lower arms. The unit rides on the left label
    # instead, and the two dead bands beside the dial carry the label and the date.
    dial(d, v['soc'])
    num = '%.0f' % v['soc']
    d.text((CX, CY - 24), num, font=font(48), fill=PALE, anchor='mt')
    d.text((44, CY), 'PACK SOC %', font=font(16), fill=GOLD, anchor='lm')
    d.text((464, CY), v['date'], font=font(16), fill=PURPLE, anchor='rm')
    for probe in ('9', '86', '100'):
        print('  soc %-4s -> %3d px wide, clearance to the inner ring: %+d px'
              % (probe, text_w(probe, 48), (R_MID - DISC_R) - text_w(probe, 48) / 2))

    # --- three data pills under the dial, columns set inside the frame (44 / 471) ---
    cols = [(44, 139, PERIWINKLE, 'PACK V', '%.2f V' % v['volts']),
            (189, 139, ORANGE, 'CHARGE' if v['amps'] >= 0 else 'DISCHG', '%+.1f A' % v['amps']),
            (334, 138, PURPLE, 'TEMP', '%.1f F' % v['temp'])]
    for x, w, colour, label, value in cols:
        pill(d, x, 192, w, 21, colour)
        d.text((x + w // 2, 195), label, font=font(16), fill=INK, anchor='mt')
        d.text((x + w // 2, 218), value, font=font(28), fill=PALE, anchor='mt')

    # --- LCARS footer: the column turns into it, so the two are one elbow ---
    bx = 8
    for colour, bw in ((ORANGE, 110), (GOLD, 80), (SALMON, 60), (PERIWINKLE, 110), (PURPLE, 80)):
        d.rectangle([bx, FOOT_Y, bx + bw - 1, FOOT_Y + FOOT_H - 1], fill=colour)
        bx += bw + 6
    fillet(d, 34, FOOT_Y)
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
