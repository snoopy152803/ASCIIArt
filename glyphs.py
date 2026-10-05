"""Build a bank of glyph bitmaps so cells can be matched by shape, not just brightness.

Picking a character purely by average brightness throws away the one thing
characters are good at: they have shapes. A diagonal edge should become '/',
a vertical one '|', a horizontal one '_' or '-'. Doing that just means
rendering every candidate glyph to a small bitmap once, then choosing the
glyph whose bitmap is closest to the cell's pixels.

Each glyph is rasterised in a real monospace font at a 1:2 cell, then box-
downsampled to GW x GH coverage values in 0..1.

Run this file directly to inspect the bank.
"""

from __future__ import annotations

import os

import numpy as np

import ramp

# Sub-samples per cell. 4x8 in a cell that is twice as tall as wide means each
# sample is square, and 32 numbers is plenty to tell glyph shapes apart.
GW, GH = 4, 8

CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "glyphbank.npz")

# Default candidate set: printable ASCII, minus the ones that are nearly
# invisible duplicates of others at this size or that read as noise.
DEFAULT_CHARS = "".join(chr(c) for c in range(32, 127) if chr(c) not in "`")


def render_bank(chars=DEFAULT_CHARS, font_path=None, px=48):
    """-> (chars, bank array (N, GH, GW) float32 in 0..1)."""
    from PIL import Image, ImageDraw, ImageFont

    font_path = font_path or ramp._find_font()
    if not font_path:
        return "", np.zeros((0, GH, GW), np.float32)

    font = ImageFont.truetype(font_path, px)
    cw = max(1, round(font.getlength("M")))
    ch = cw * 2                      # terminal cells are about twice as tall as wide
    ascent, descent = font.getmetrics()
    y0 = (ch - (ascent + descent)) // 2   # sit the baseline where a terminal puts it

    out = []
    for c in chars:
        img = Image.new("L", (cw, ch), 0)
        ImageDraw.Draw(img).text((0, y0), c, font=font, fill=255)
        small = img.resize((GW, GH), Image.BOX)   # box filter == mean coverage
        out.append(np.asarray(small, dtype=np.float32) / 255.0)
    return chars, np.stack(out)


def load(chars=DEFAULT_CHARS, use_cache=True):
    """Cached (chars, bank) pair."""
    if use_cache and os.path.exists(CACHE):
        try:
            data = np.load(CACHE, allow_pickle=False)
            if str(data["chars"]) == chars and data["bank"].shape[1:] == (GH, GW):
                return chars, data["bank"].astype(np.float32)
        except Exception:
            pass
    chars, bank = render_bank(chars)
    try:
        np.savez_compressed(CACHE, chars=np.array(chars), bank=bank)
    except Exception:
        pass
    return chars, bank


def coverage(bank: np.ndarray) -> np.ndarray:
    """Mean ink coverage of each glyph, 0..1."""
    return bank.reshape(len(bank), -1).mean(axis=1)


if __name__ == "__main__":
    chars, bank = load(use_cache=False)
    cov = coverage(bank)
    print(f"font: {ramp._find_font()}")
    print(f"{len(chars)} glyphs, bank {bank.shape}, coverage {cov.min():.3f}..{cov.max():.3f}")
    order = np.argsort(cov)
    print("sparsest:", "".join(chars[i] for i in order[:12]))
    print("densest: ", "".join(chars[i] for i in order[-12:]))
    # Show a couple of glyphs as coverage maps.
    for want in "/|_@":
        i = chars.index(want)
        print(f"\n{want!r}  coverage {cov[i]:.3f}")
        for row in bank[i]:
            print("   " + "".join(" .:-=+*#@"[min(8, int(v * 9))] for v in row))
