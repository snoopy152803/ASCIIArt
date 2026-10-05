#!/usr/bin/env python3
"""Generate asciiapp.ico.

The large sizes are a real render: a letter 'A' put through this project's own
shape-matching renderer, so the icon is literally made of characters. The
small sizes drop to a plain bold glyph, because at 16 px a grid of characters
is indistinguishable from noise.

    python make_icon.py
"""

from __future__ import annotations

import os

import numpy as np
from PIL import Image, ImageDraw, ImageFont

import asciiart
import glyphs

HERE = os.path.dirname(os.path.abspath(__file__))
BG = (14, 16, 22)
INK = (126, 214, 255)
WARM = (255, 196, 92)


def _font(size, bold=True):
    for name in (("arialbd.ttf", "seguisb.ttf") if bold else ("arial.ttf",)):
        p = os.path.join(r"C:\Windows\Fonts", name)
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, size)
            except Exception:
                pass
    return ImageFont.load_default()


def source_art(px=512):
    """A big 'A' with a tonal sweep behind it - something with real shading
    for the renderer to chew on, so the icon shows actual character texture."""
    # Black ground so everything outside the letter renders as spaces and the
    # tile stays clean; the letter itself carries a gradient so the renderer
    # has real shading to put characters into.
    img = Image.new("RGB", (px, px), (0, 0, 0))
    letter = Image.new("RGB", (px, px), (0, 0, 0))
    ld = ImageDraw.Draw(letter)
    for y in range(px):
        t = y / px
        ld.rectangle([0, y, px, y],
                     fill=(int(255 - 60 * t), int(250 - 40 * t), int(235 - 10 * t)))
    mask = Image.new("L", (px, px), 0)
    md = ImageDraw.Draw(mask)
    f = _font(int(px * 0.92))
    box = md.textbbox((0, 0), "A", font=f)
    md.text(((px - (box[2] - box[0])) / 2 - box[0],
             (px - (box[3] - box[1])) / 2 - box[1]), "A", font=f, fill=255)
    img.paste(letter, (0, 0), mask)
    return img


def render_ascii_tile(size, cols):
    """Run the project's renderer and draw the resulting glyphs into a tile."""
    chars, bank = glyphs.load()
    rows = max(1, cols // 2)
    arr = np.asarray(source_art().resize((cols * glyphs.GW, rows * glyphs.GH),
                                         Image.LANCZOS), dtype=np.uint8)
    arr = asciiart.auto_levels(arr)
    cells, fg, _ = asciiart.render_ascii_shape(arr, chars, bank, colour=True)

    cw, chh = size / cols, size / rows
    tile = Image.new("RGB", (size, size), BG)
    d = ImageDraw.Draw(tile)
    f = _font(max(6, int(chh * 0.95)), bold=False)
    for y in range(rows):
        for x in range(cols):
            c = chr(int(cells[y, x]))
            if c == " ":
                continue
            r, g, b = (int(v) for v in fg[y, x])
            # tint towards the icon's palette so it reads as one object
            r = min(255, (r + INK[0] * 2) // 2)
            g = min(255, (g + INK[1] * 2) // 2)
            b = min(255, (b + INK[2] * 2) // 2)
            d.text((x * cw, y * chh - chh * 0.1), c, font=f, fill=(r, g, b))
    return tile


def simple_tile(size):
    """A plain bold 'A' for the sizes where characters cannot be resolved."""
    tile = Image.new("RGB", (size, size), BG)
    d = ImageDraw.Draw(tile)
    f = _font(int(size * 0.8))
    box = d.textbbox((0, 0), "A", font=f)
    d.text(((size - (box[2] - box[0])) / 2 - box[0],
            (size - (box[3] - box[1])) / 2 - box[1]), "A", font=f, fill=INK)
    # a couple of character-art specks, so it still hints at the idea
    d.text((size * 0.06, size * 0.60), "#", font=_font(int(size * 0.3)), fill=WARM)
    return tile


def rounded(img, radius_frac=0.18):
    """Round the corners and return RGBA."""
    size = img.size[0]
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, size - 1, size - 1],
                                           radius=int(size * radius_frac), fill=255)
    out = img.convert("RGBA")
    out.putalpha(mask)
    return out


def main():
    layers = []
    for size in (256, 128, 64, 48):
        cols = max(7, size // 12)
        layers.append(rounded(render_ascii_tile(size, cols), 0.16))
    for size in (32, 24, 16):
        layers.append(rounded(simple_tile(size), 0.14))

    out = os.path.join(HERE, "asciiapp.ico")
    layers[0].save(out, format="ICO",
                   sizes=[(im.size[0], im.size[1]) for im in layers],
                   append_images=layers[1:])
    print(f"wrote {out}  ({', '.join(str(i.size[0]) for i in layers)} px)")
    layers[0].resize((256, 256), Image.LANCZOS).save(os.path.join(HERE, "verify_out",
                                                                  "icon_preview.png"))


if __name__ == "__main__":
    main()
