#!/usr/bin/env python3
"""Verify the renderer by turning its ANSI output back into a picture.

Parses the escape sequences, draws each cell exactly as a terminal would
(block glyphs drawn geometrically, ASCII drawn with a real monospace font),
and reports PSNR against the source image. Writes a side-by-side PNG so the
result can be eyeballed.

    python verify.py photo.jpg --mode half --cols 100
    python verify.py photo.jpg --all
"""

from __future__ import annotations

import argparse
import glob
import math
import os
import re
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageOps

import asciiart
import asciiview

CELL_W, CELL_H = 8, 16
SGR = re.compile(r"\x1b\[([0-9;]*)m")
# cursor/erase sequences, which draw nothing and must not count as cells
CSI_OTHER = re.compile(r"\x1b\[[0-9;?]*[A-LN-Za-ln-z]")
CUBE = [0, 95, 135, 175, 215, 255]
ANSI16 = [(0, 0, 0), (170, 0, 0), (0, 170, 0), (170, 85, 0), (0, 0, 170), (170, 0, 170),
          (0, 170, 170), (170, 170, 170), (85, 85, 85), (255, 85, 85), (85, 255, 85),
          (255, 255, 85), (85, 85, 255), (255, 85, 255), (85, 255, 255), (255, 255, 255)]

QUAD_BITS = {ord(c): i for i, c in enumerate(asciiart.QUAD_CHARS)}


def _xterm256(n: int) -> tuple[int, int, int]:
    if n < 16:
        return ANSI16[n]
    if n < 232:
        n -= 16
        return CUBE[n // 36], CUBE[(n // 6) % 6], CUBE[n % 6]
    v = 8 + 10 * (n - 232)
    return v, v, v


def parse_ansi(text: str):
    """-> (chars list-of-lists, fg array, bg array). Mirrors a terminal's state machine."""
    rows = []
    default_fg, default_bg = (255, 255, 255), (0, 0, 0)
    for line in text.split("\n"):
        line = CSI_OTHER.sub("", line)
        fg, bg = default_fg, default_bg
        cells = []
        pos = 0
        for m in SGR.finditer(line):
            for ch in line[pos:m.start()]:
                if ch != "\x1b":
                    cells.append((ch, fg, bg))
            pos = m.end()
            params = [int(x) if x else 0 for x in m.group(1).split(";")] or [0]
            i = 0
            while i < len(params):
                p = params[i]
                if p == 0:
                    fg, bg = default_fg, default_bg
                elif p in (38, 48) and i + 1 < len(params):
                    kind = params[i + 1]
                    if kind == 2 and i + 4 < len(params):
                        col = (params[i + 2], params[i + 3], params[i + 4])
                        i += 4
                    elif kind == 5 and i + 2 < len(params):
                        col = _xterm256(params[i + 2])
                        i += 2
                    else:
                        i += 1
                        col = None
                    if col is not None:
                        if p == 38:
                            fg = col
                        else:
                            bg = col
                elif 30 <= p <= 37:
                    fg = ANSI16[p - 30]
                elif 90 <= p <= 97:
                    fg = ANSI16[p - 90 + 8]
                elif 40 <= p <= 47:
                    bg = ANSI16[p - 40]
                elif 100 <= p <= 107:
                    bg = ANSI16[p - 100 + 8]
                i += 1
        for ch in line[pos:]:
            if ch != "\x1b":
                cells.append((ch, fg, bg))
        if cells:
            rows.append(cells)
    return rows


def _font(size=14):
    for name in ("consola.ttf", "CascadiaMono.ttf", "DejaVuSansMono.ttf", "cour.ttf"):
        for base in (r"C:\Windows\Fonts", "/usr/share/fonts/truetype/dejavu", "/Library/Fonts"):
            path = os.path.join(base, name)
            if os.path.exists(path):
                try:
                    return ImageFont.truetype(path, size)
                except Exception:
                    pass
    return ImageFont.load_default()


def rasterise(rows, cell_fill=1.0) -> Image.Image:
    """Draw the parsed grid the way a terminal would.

    A terminal always paints the background across the whole cell, then draws
    the glyph on top. `cell_fill` below 1.0 simulates a terminal with extra
    line spacing, where block glyphs are drawn shorter than the cell - which
    is how relying on U+2588 to cover a cell produces black scanlines.
    """
    cols = max(len(r) for r in rows)
    img = Image.new("RGB", (cols * CELL_W, len(rows) * CELL_H), (0, 0, 0))
    d = ImageDraw.Draw(img)
    font = _font(CELL_H - 2)
    gh = max(1, int(round(CELL_H * cell_fill)))     # height the glyph may use
    hw, hh = CELL_W // 2, gh // 2

    for y, row in enumerate(rows):
        for x, (ch, fg, bg) in enumerate(row):
            x0, y0 = x * CELL_W, y * CELL_H
            # background covers the full cell, always
            d.rectangle([x0, y0, x0 + CELL_W - 1, y0 + CELL_H - 1], fill=bg)
            cp = ord(ch)
            if cp == 0x20:
                continue
            if cp == 0x2588:  # full block
                d.rectangle([x0, y0, x0 + CELL_W - 1, y0 + gh - 1], fill=fg)
            elif cp in QUAD_BITS:  # includes the half blocks
                bits = QUAD_BITS[cp]
                quads = [(0, 0), (hw, 0), (0, hh), (hw, hh)]
                for i, (dx, dy) in enumerate(quads):
                    if bits & (1 << i):
                        d.rectangle([x0 + dx, y0 + dy, x0 + dx + hw - 1, y0 + dy + hh - 1], fill=fg)
            elif cp == 0x2584:  # lower half block
                d.rectangle([x0, y0 + hh, x0 + CELL_W - 1, y0 + gh - 1], fill=fg)
            elif cp == 0x258C:
                d.rectangle([x0, y0, x0 + hw - 1, y0 + gh - 1], fill=fg)
            elif cp == 0x2590:
                d.rectangle([x0 + hw, y0, x0 + CELL_W - 1, y0 + gh - 1], fill=fg)
            elif 0x2800 <= cp <= 0x28FF:  # braille: 2x4 dots
                bits = cp - 0x2800
                order = [(0, 0, 0x01), (0, 1, 0x02), (0, 2, 0x04), (0, 3, 0x40),
                         (1, 0, 0x08), (1, 1, 0x10), (1, 2, 0x20), (1, 3, 0x80)]
                dw, dh = CELL_W / 2, gh / 4
                for dx, dy, bit in order:
                    if bits & bit:
                        cx, cy = x0 + (dx + 0.5) * dw, y0 + (dy + 0.5) * dh
                        r = min(dw, dh) * 0.45
                        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=fg)
            else:
                d.text((x0, y0 - 1), ch, font=font, fill=fg)
    return img


def psnr(a: np.ndarray, b: np.ndarray) -> float:
    mse = float(np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2))
    return math.inf if mse == 0 else 10 * math.log10(255.0 ** 2 / mse)


def run(path: str, mode: str, cols: int, rows_max: int, colours: str, outdir: str):
    args = asciiview.build_parser().parse_args([path, "--mode", mode, "--cols", str(cols),
                                                "--rows", str(rows_max), "--colors", colours])
    asciiview.finalize(args)

    src = ImageOps.exif_transpose(Image.open(path)).convert("RGB")
    c, r, sw, sh = asciiview.grid_for(src.width, src.height, args)
    text = asciiview.frame_to_text(asciiview.pil_to_sample(src, sw, sh), args)

    grid = parse_ansi(text)
    assert len(grid) == r, f"row count: parsed {len(grid)}, expected {r}"
    for i, row in enumerate(grid):
        assert len(row) == c, f"row {i} width: parsed {len(row)}, expected {c}"

    shot = rasterise(grid)
    ref = src.resize(shot.size, Image.LANCZOS)
    score = psnr(np.asarray(shot), np.asarray(ref))

    os.makedirs(outdir, exist_ok=True)
    stem = f"{os.path.splitext(os.path.basename(path))[0]}_{mode}_{c}x{r}"
    side = Image.new("RGB", (shot.width * 2 + 12, shot.height), (24, 24, 24))
    side.paste(ref, (0, 0))
    side.paste(shot, (shot.width + 12, 0))
    out = os.path.join(outdir, stem + ".png")
    side.save(out)

    bytes_per_cell = len(text.encode("utf-8")) / max(1, c * r)
    print(f"{mode:8s} {c:4d}x{r:<4d} cells  PSNR {score:5.2f} dB  "
          f"{bytes_per_cell:4.1f} B/cell  -> {os.path.relpath(out)}")
    return score


def main(argv=None):
    p = argparse.ArgumentParser(description="Round-trip check the ASCII renderer.")
    p.add_argument("path")
    p.add_argument("--mode", default="half")
    p.add_argument("--cols", type=int, default=100)
    p.add_argument("--rows", type=int, default=1000)
    p.add_argument("--colors", dest="colours", default="truecolor")
    p.add_argument("--all", action="store_true", help="check every mode")
    p.add_argument("--outdir", default="verify_out")
    a = p.parse_args(argv)

    modes = ["half", "quad", "ascii", "braille"] if a.all else [a.mode]
    for m in modes:
        run(a.path, m, a.cols, a.rows, a.colours, a.outdir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
