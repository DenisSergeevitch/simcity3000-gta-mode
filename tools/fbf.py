#!/usr/bin/env python3
"""Text in SimCity 3000's own bitmap fonts (Res/Text/<language>/*.FBF), rendered to transparent PNGs.

FBF layout (reverse-engineered 2026-10-04):
  u32 1, u32 atlas width (640), u32 atlas height, u32 1
  atlas: width*height bytes, one palette index per pixel. 1 = full ink ... 0x10 = no ink, 0x11 = outside a glyph cell
  palette: 256 x u16 RGB565 (the game pre-colours its fonts for the blue UI panels; we only use the ink level)
  glyphs: 256 x (i32 x0, y0, x1, y1, advance), indexed by the Windows-1252 code of the character
Fonts: MAIN9/MAIN10 (bold pixel UI font), SYSTEM9, TITLE9/TITLE12 (rounded title font, antialiased),
SERIF17 (newspaper serif, antialiased).

  fbf.py out.png "TEXT" --font TITLE12 --scale 6 --fill ffffff --outline 000000 --shadow 000000
"""
import argparse, os, struct
from PIL import Image

# The game's Apps folder: SC3U_APPS, else the author's layout (this repo next to the Wine prefix)
APPS = os.environ.get('SC3U_APPS') or os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                    '../../../prefix/drive_c/Games/SimCity 3000 Unlimited/Apps')
FONT_DIR = os.path.join(APPS, 'Res', 'Text', os.environ.get('SC3U_LANG', 'ENGLISH'))


class Font:
    def __init__(self, name):
        d = open(os.path.join(FONT_DIR, name + '.FBF'), 'rb').read()
        _, self.w, self.h, _ = struct.unpack_from('<4I', d, 0)
        self.atlas = d[16:16 + self.w * self.h]
        base = 16 + self.w * self.h + 512
        self.glyphs = [struct.unpack_from('<5i', d, base + 20 * k) for k in range(256)]
        self.line = max(g[3] - g[1] for g in self.glyphs)

    def ink(self, x, y):
        v = self.atlas[y * self.w + x]
        return 0 if v >= 0x10 else (0x10 - v) * 255 // 15

    def mask(self, text, spacing=0):
        """Alpha mask (L image) of one line of text, 1:1."""
        codes = text.encode('cp1252', errors='replace')
        width = sum(self.glyphs[c][4] + spacing for c in codes) - spacing + 2
        m = Image.new('L', (max(width, 1), self.line), 0)
        px = m.load(); pen = 0
        for c in codes:
            x0, y0, x1, y1, adv = self.glyphs[c]
            for yy in range(y0, y1):
                for xx in range(x0, x1):
                    a = self.ink(xx, yy)
                    if a and 0 <= pen + xx - x0 < m.width: px[pen + xx - x0, yy - y0] = max(px[pen + xx - x0, yy - y0], a)
            pen += adv + spacing
        bbox = m.getbbox()   # keep the full advance width: spaces matter when coloured segments are joined
        return m.crop((0, 0, max(pen - spacing, bbox[2] if bbox else 0, 1), m.height))


def hexrgb(s):
    s = s.lstrip('#'); return tuple(int(s[i:i + 2], 16) for i in (0, 2, 4))


def render(text, font='TITLE12', scale=6, fill='ffffff', outline=None, shadow=None, spacing=0, pad=None, smooth=False):
    """Coloured text: the game's glyphs scaled by an integer factor (nearest neighbour, so the game's pixels stay
    visible), with an optional 1-pixel (font pixels) outline and a drop shadow."""
    f = Font(font) if isinstance(font, str) else font
    m = f.mask(text, spacing)
    o = 1 if outline else 0; s = 1 if shadow else 0
    pad = pad if pad is not None else o + s
    W, H = m.width + 2 * pad, m.height + 2 * pad
    out = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    def layer(rgb, dx, dy, src):
        img = Image.new('RGBA', src.size, rgb + (255,)); img.putalpha(src)
        out.alpha_composite(img, (pad + dx, pad + dy))
    if outline:
        thick = m.point(lambda a: 255 if a > 40 else 0)
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                if dx or dy: layer(hexrgb(outline), dx, dy, thick)
    if shadow:
        thick = m.point(lambda a: 255 if a > 40 else 0)
        for dx, dy in ((1, 1), (2, 2)) if outline else ((1, 1),):
            layer(hexrgb(shadow), dx, dy, thick)
        # redraw the outline on top of the shadow so the shadow sits behind it
        if outline:
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    if dx or dy: layer(hexrgb(outline), dx, dy, thick)
    layer(hexrgb(fill), 0, 0, m)
    return out.resize((W * scale, H * scale), Image.LANCZOS if smooth else Image.NEAREST)


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('out'); ap.add_argument('text')
    ap.add_argument('--font', default='TITLE12'); ap.add_argument('--scale', type=int, default=6)
    ap.add_argument('--fill', default='ffffff'); ap.add_argument('--outline'); ap.add_argument('--shadow')
    ap.add_argument('--spacing', type=int, default=0)
    a = ap.parse_args()
    render(a.text, a.font, a.scale, a.fill, a.outline, a.shadow, a.spacing).save(a.out)
