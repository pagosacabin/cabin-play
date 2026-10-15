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
"""
import math
import pathlib
import sys

from PIL import Image, ImageDraw, ImageFont

W, H = 480, 272
FONT_TTF = pathlib.Path(__file__).resolve().parents[1] / '.esphome' / 'font' / 'Roboto@400@False@v1.ttf'
BG = (0, 0, 0)

# --- LCARS palette (TNG console colours) ----------------------------------------------
ORANGE = (255, 156, 0)
GOLD = (255, 204, 102)
LAVENDER = (204, 153, 204)
BLUE = (153, 153, 204)
SALMON = (204, 102, 102)
TAN = (204, 153, 102)
GREEN = (153, 204, 153)
DIM = (44, 44, 80)          # unlit arc segments
INK = (0, 0, 0)             # text sitting on a lit pill
WHITE = (255, 255, 255)

# --- dial geometry (must match the lambda) -------------------------------------------
CX, CY = 240, 122
R_MID, DISC_R = 60, 6                 # ring is 2*DISC_R thick (r 54..66), centred on R_MID
SWEEP_START, SWEEP_DEG = 120, 300     # 0 deg = 3 o'clock, positive = clockwise on screen
SEG_DEG = 3                           # disc spacing along the arc

# --- values to draw (edit these to check other states) -------------------------------
BMS = dict(soc=86.0, volts=54.54, amps=29.4, temp=58.5, local=91.0, clock='11:19', date='Wed 15 Oct')
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


def dial(d, soc):
    """A chain of overlapping discs along the arc, as the lambda draws it: discs cannot seam
    (the quad version left 1 px gaps along shared edges) and their round ends come free."""
    lit = SALMON if soc < 25 else GOLD
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

    # --- top band: panel cell + clock ---
    # 20 pt, not 24: at arm's length the header is a status strip, not a headline.
    pill(d, 8, 4, 168, 27, ORANGE)
    d.text((8 + 84, 7), 'PANEL %.0f%%' % v['local'], font=font(20), fill=INK, anchor='mt')
    pill(d, 304, 4, 168, 27, BLUE)
    d.text((304 + 84, 7), v['clock'], font=font(20), fill=INK, anchor='mt')
    # LCARS frames join their blocks rather than floating them: a bar across the gap
    # between the two pills, and a rule closing the header off from the dial.
    d.rectangle([184, 15, 296, 20], fill=TAN)
    d.rectangle([8, 38, 472, 40], fill=TAN)

    # --- dial, with its label and the date bookending it ---
    # The number is drawn WITHOUT the % sign and with nothing else inside the ring: at
    # 48 pt a 3-digit reading plus "%" is wider than the ring's inner diameter, and a label
    # under the number crosses the ring's lower arms. The unit rides on the left label
    # instead, and the two dead bands beside the dial carry the label and the date.
    dial(d, v['soc'])
    num = '%.0f' % v['soc']
    d.text((CX, CY - 24), num, font=font(48), fill=WHITE, anchor='mt')
    d.text((16, CY), 'PACK SOC %', font=font(16), fill=GOLD, anchor='lm')
    d.text((464, CY), v['date'], font=font(16), fill=LAVENDER, anchor='rm')
    for probe in ('9', '86', '100'):
        print('  soc %-4s -> %3d px wide, clearance to the inner ring: %+d px'
              % (probe, text_w(probe, 48), (R_MID - DISC_R) - text_w(probe, 48) / 2))

    # --- three data pills under the dial, columns flush with the header and footer ---
    cols = [(8, 150, BLUE, 'PACK V', '%.2f V' % v['volts']),
            (164, 151, GOLD, 'CHARGE' if v['amps'] >= 0 else 'DISCHG', '%+.1f A' % v['amps']),
            (321, 151, LAVENDER, 'TEMP', '%.1f F' % v['temp'])]
    for x, w, colour, label, value in cols:
        pill(d, x, 192, w, 21, colour)
        d.text((x + w // 2, 195), label, font=font(16), fill=INK, anchor='mt')
        d.text((x + w // 2, 218), value, font=font(28), fill=WHITE, anchor='mt')

    # --- LCARS footer: a row of coloured blocks ---
    bx = 8
    for colour, bw in ((ORANGE, 120), (GOLD, 80), (SALMON, 60), (BLUE, 100), (LAVENDER, 80)):
        d.rectangle([bx, 256, bx + bw, 261], fill=colour)
        bx += bw + 6
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
