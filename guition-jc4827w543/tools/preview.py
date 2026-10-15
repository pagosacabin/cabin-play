#!/usr/bin/env python3
"""Offline preview of the JC4827W543 two-page layout.

Mirrors the geometry and draw order of the display lambda in jc4827w543.yaml so a layout
can be inspected in two seconds instead of a flash-and-squint loop. Numbers and pixel
positions here must be kept in step with the lambda by hand.

    python3 tools/preview.py [out.png]

Caveats (why this is a guide, not proof):
  * PIL renders the TTF with its own antialiasing; ESPHome rasterises the same TTF into a
    4bpp bitmap font. Glyph widths match closely but not pixel-for-pixel.
  * ESPHome's TextAlign::TOP_* anchors on the glyph bounding box; PIL's 't' anchors sit on
    the line box. Expect a pixel or two of vertical disagreement.
  * The panel runs `rotation: 180`, so on the glass this picture is upside down.
"""
import pathlib
import sys

from PIL import Image, ImageDraw, ImageFont

W, H = 480, 272
FONT_TTF = pathlib.Path(__file__).resolve().parents[1] / '.esphome' / 'font' / 'Roboto@400@False@v1.ttf'
BG, FG = (0, 0, 0), (255, 255, 255)

# --- values to draw (edit these to check other states) -------------------------------
BMS = dict(soc=79.0, volts=54.49, amps=29.2, temp=57.6, local=92.0, clock='09:42', date='Wed 15 Oct')
LOW = dict(local=8.0, volts=3.58)


def font(px):
    return ImageFont.truetype(str(FONT_TTF), px)


def render_bms(v):
    img = Image.new('RGB', (W, H), BG)
    d = ImageDraw.Draw(img)
    d.text((464, 2), v['clock'], font=font(48), fill=FG, anchor='rt')
    d.text((464, 56), v['date'], font=font(16), fill=FG, anchor='rt')
    d.text((16, 2), '%.0f%%' % v['soc'], font=font(48), fill=FG, anchor='lt')
    d.text((16, 56), 'PACK SOC', font=font(16), fill=FG, anchor='lt')
    d.line([(16, 78), (464, 78)], fill=FG)
    col2 = 250
    r1lab, r1val, r2lab, r2val = 112, 130, 192, 210
    d.text((16, r1lab), 'PACK', font=font(16), fill=FG, anchor='lt')
    d.text((16, r1val), '%.2f V' % v['volts'], font=font(28), fill=FG, anchor='lt')
    d.text((col2, r1lab), 'CHARGE' if v['amps'] >= 0 else 'DISCHG', font=font(16), fill=FG, anchor='lt')
    d.text((col2, r1val), '%+.1f A' % v['amps'], font=font(28), fill=FG, anchor='lt')
    d.text((16, r2lab), 'TEMP', font=font(16), fill=FG, anchor='lt')
    d.text((16, r2val), '%.1f F' % v['temp'], font=font(28), fill=FG, anchor='lt')
    d.text((col2, r2lab), 'PANEL', font=font(16), fill=FG, anchor='lt')
    d.text((col2, r2val), '%.0f %%' % v['local'], font=font(28), fill=FG, anchor='lt')
    return img


def render_status(v):
    img = Image.new('RGB', (W, H), BG)
    d = ImageDraw.Draw(img)
    d.text((240, 24), 'JC4827W543 up', font=font(24), fill=FG, anchor='mt')
    d.text((240, 84), 'BAT %3.0f%%   %.2f V' % (v['local'], v['volts']), font=font(24), fill=FG, anchor='mt')
    bx, by, bw, bh, nub, pad = 170, 150, 120, 52, 8, 4
    d.rectangle([bx, by, bx + bw, by + bh], outline=FG)
    d.rectangle([bx + bw + 1, by + bh // 3, bx + bw + 1 + nub, by + bh // 3 + bh // 3], fill=FG)
    fillw = int((bw - 2 * pad) * v['local'] / 100.0)
    if fillw > 0:
        d.rectangle([bx + pad, by + pad, bx + pad + fillw, by + bh - pad], fill=FG)
    return img


def main():
    out = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else 'preview.png')
    pages = [
        ('BMS page  (panel >= 10%)', render_bms(BMS)),
        ('status page  (panel < 10%)', render_status(LOW)),
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
