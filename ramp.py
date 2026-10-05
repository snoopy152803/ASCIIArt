"""Build a character ramp by measuring how much ink each glyph actually puts down.

The famous " .:-=+*#%@"-style ramps are eyeballed, not measured, so their steps
are uneven and clump at the dark end. Rendering each candidate glyph in a real
monospace font and measuring its coverage gives a ramp whose steps are evenly
spaced in brightness, which is what makes photographic ASCII art read correctly.

Run this file directly to print the measured ramp for the local fonts.
"""

from __future__ import annotations

import json
import os

CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ramp_cache.json")

# Measured with Consolas; used when no usable font is available at runtime.
FALLBACK = " .'\",:;!|/rvcsnuxkahbdpqOCUZ0XQ#NW@M"

FONT_CANDIDATES = [
    (r"C:\Windows\Fonts", ["consola.ttf", "cour.ttf", "lucon.ttf"]),
    (os.path.expandvars(r"%LOCALAPPDATA%\Microsoft\WindowsApps"), ["CascadiaMono.ttf"]),
    ("/usr/share/fonts/truetype/dejavu", ["DejaVuSansMono.ttf"]),
    ("/Library/Fonts", ["Menlo.ttc", "Courier New.ttf"]),
]

# Glyphs that are visually noisy or ambiguous in art, even if their coverage fits.
_SKIP = set("`~^&$%{}[]()<>?_-+=*\\tfjlI1")


def _find_font():
    for base, names in FONT_CANDIDATES:
        for name in names:
            path = os.path.join(base, name)
            if os.path.exists(path):
                return path
    return None


def measure(font_path=None, size=48) -> dict[str, float]:
    """Ink coverage (0..1) of each printable ASCII glyph."""
    from PIL import Image, ImageDraw, ImageFont

    font_path = font_path or _find_font()
    if not font_path:
        return {}
    font = ImageFont.truetype(font_path, size)
    # A monospace cell: measure the advance of a reference glyph.
    box = font.getbbox("M")
    cw = int(font.getlength("M")) or box[2]
    ch = int(size * 1.3)

    cover = {}
    for code in range(32, 127):
        c = chr(code)
        img = Image.new("L", (cw, ch), 0)
        ImageDraw.Draw(img).text((0, 0), c, font=font, fill=255)
        cover[c] = sum(img.getdata()) / (255.0 * cw * ch)
    return cover


def build(levels=32, font_path=None, use_cache=True) -> str:
    """A ramp of `levels` characters whose coverage rises in even steps."""
    if use_cache and os.path.exists(CACHE):
        try:
            with open(CACHE, encoding="utf-8") as fh:
                data = json.load(fh)
            if data.get("levels") == levels:
                return data["ramp"]
        except Exception:
            pass

    cover = measure(font_path)
    if not cover:
        return FALLBACK

    usable = {c: v for c, v in cover.items() if c == " " or c not in _SKIP}
    lo = min(usable.values())
    hi = max(usable.values())
    if hi <= lo:
        return FALLBACK

    # Walk evenly spaced coverage targets, taking the closest glyph to each.
    # Picking without exclusion keeps the ramp monotone in coverage, which
    # matters far more than hitting exactly `levels` distinct characters;
    # repeats just mean two targets share a best glyph, so they collapse.
    items = sorted(usable.items(), key=lambda kv: kv[1])
    out = []
    for i in range(levels):
        target = lo + (hi - lo) * i / (levels - 1)
        best = min(items, key=lambda kv: abs(kv[1] - target))[0]
        if not out or out[-1] != best:
            out.append(best)
    ramp = "".join(out)

    try:
        with open(CACHE, "w", encoding="utf-8") as fh:
            json.dump({"levels": levels, "ramp": ramp, "font": font_path or _find_font()}, fh)
    except Exception:
        pass
    return ramp


if __name__ == "__main__":
    font = _find_font()
    print(f"font: {font}")
    cover = measure(font)
    if cover:
        for c, v in sorted(cover.items(), key=lambda kv: kv[1]):
            print(f"  {v:6.4f}  {c!r}")
    for n in (10, 16, 32, 48):
        print(f"{n:3d} levels: {build(n, use_cache=False)!r}")
