#!/usr/bin/env python3
"""Self-test: renders the sample images in every mode, checks the encoder
round-trips, checks aspect ratios survive, times playback, and writes
side-by-side PNGs into verify_out/ so the results can be eyeballed.

    python selftest.py
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
import time

import numpy as np
from PIL import Image, ImageOps

import asciiart
import asciiview
import term
import verify

HERE = os.path.dirname(os.path.abspath(__file__))
SAMPLES = os.path.join(HERE, "samples")
OUT = os.path.join(HERE, "verify_out")
MODES = ["ascii", "half", "quad", "braille"]
IMAGES = ["testcard.png", "sphere.png", "screenshot.png", "bars.png", "photo.jpg"]

passed = failed = 0


def check(name: str, ok: bool, detail: str = ""):
    global passed, failed
    if ok:
        passed += 1
        print(f"  PASS  {name}  {detail}")
    else:
        failed += 1
        print(f"  FAIL  {name}  {detail}")


def test_geometry():
    """A square image must come out square on screen, in every mode."""
    print("\ngeometry (aspect ratio preserved on a 2:1 cell grid)")
    for mode in MODES:
        for w, h in [(640, 480), (480, 640), (500, 500), (1920, 1080)]:
            cols, rows, sw, sh = asciiart.plan_geometry(w, h, 100, 60, mode, 2.0)
            shown = cols / (rows * 2.0)   # on-screen aspect in cell widths
            want = w / h
            err = abs(shown - want) / want
            check(f"{mode:8s} {w}x{h}", err < 0.06,
                  f"grid {cols}x{rows}, aspect {shown:.3f} vs {want:.3f} ({err * 100:.1f}% off)")


def test_roundtrip():
    """Every cell the encoder emits must parse back with the right colours."""
    print("\nencoder round-trip (parse the ANSI back, compare cell by cell)")
    rng = np.random.default_rng(7)
    for colours in ("truecolor", "256", "16"):
        arr = rng.integers(0, 255, (16, 20, 3), dtype=np.uint8)
        cells, fg, bg = asciiart.render_half(arr)
        text = asciiart.encode(cells, fg, bg, colours=colours)
        grid = verify.parse_ansi(text)
        ok_shape = len(grid) == cells.shape[0] and all(len(r) == cells.shape[1] for r in grid)
        mismatch = 0
        if ok_shape and colours == "truecolor":
            for y in range(cells.shape[0]):
                for x in range(cells.shape[1]):
                    ch, f, b = grid[y][x]
                    if ord(ch) != cells[y, x] or f != tuple(fg[y, x]):
                        mismatch += 1
                    elif ord(ch) != 0x2588 and b != tuple(bg[y, x]):
                        mismatch += 1
        check(f"round-trip {colours}", ok_shape and mismatch == 0,
              f"{cells.shape[1]}x{cells.shape[0]} cells, {mismatch} mismatched")

    # A flat image must collapse to almost no escape codes.
    flat = np.full((16, 20, 3), 90, np.uint8)
    cells, fg, bg = asciiart.render_half(flat)
    text = asciiart.encode(cells, fg, bg)
    check("run-length collapse", len(text) < 400, f"{len(text)} bytes for 20x8 flat cells")


def test_full_cell_coverage():
    """No cell may depend on a glyph filling its box.

    A terminal with extra line spacing draws block glyphs short of the cell,
    so anything relying on U+2588 to hide the background leaves black
    scanlines between rows. Flat cells must be spaces painted with the
    background colour instead.
    """
    print("\nfull-cell coverage (no reliance on block glyphs filling the cell)")
    rng = np.random.default_rng(3)
    for mode, maker in (("half", asciiart.render_half), ("quad", asciiart.render_quad)):
        sx, sy = asciiart.SUBCELLS[mode]
        flat = np.full((8 * sy, 10 * sx, 3), 77, np.uint8)      # uniform: the bad case
        mixed = rng.integers(0, 255, (8 * sy, 10 * sx, 3), dtype=np.uint8)
        for label, arr in (("flat", flat), ("mixed", mixed)):
            cells, fg, bg = maker(arr)
            full = int((cells == 0x2588).sum())
            check(f"{mode} {label}: no U+2588", full == 0, f"{full} full-block cells")
        # and a flat field must still come out the right colour
        cells, fg, bg = maker(flat)
        ok = bool(np.all(bg[cells == 0x20] == 77))
        check(f"{mode} flat renders as background 77", ok)

        # The real test: rasterise it on a terminal whose glyphs are drawn at
        # 75% of the cell height. A flat field must stay one solid colour.
        text = asciiart.encode(cells, fg, bg, colours="truecolor")
        px = np.asarray(verify.rasterise(verify.parse_ansi(text), cell_fill=0.75))
        px = px.reshape(-1, 3)
        black = float((px.sum(axis=1) < 60).mean() * 100)
        shades = len(np.unique(px, axis=0))
        check(f"{mode} no scanlines with short glyphs", black == 0.0 and shades == 1,
              f"{black:.1f}% black, {shades} distinct colour(s)")


def test_cell_aspect_probe():
    """The terminal-size queries must be parsed correctly and fail safe."""
    print("\ncell aspect probe (CSI 16t / 14t+18t)")
    import re

    real = term._query
    try:
        # CSI 16 t: cell is 32 px tall, 14 px wide
        term._query = lambda req, pat, timeout=0.3: re.search(pat, "\x1b[6;32;14t")
        got = term.cell_aspect()
        check("CSI 16t parsed", abs(got - 32 / 14) < 1e-6, f"{got:.3f} (expected {32 / 14:.3f})")

        # No 16t support; fall back to window pixels / window characters.
        def two_step(req, pat, timeout=0.3):
            if "16t" in req:
                return None
            reply = "\x1b[4;1200;800t" if "14t" in req else "\x1b[8;50;100t"
            return re.search(pat, reply)

        term._query = two_step
        got = term.cell_aspect()
        want = (1200 / 50) / (800 / 100)      # 24 px tall / 8 px wide = 3.0
        check("CSI 14t+18t parsed", abs(got - want) < 1e-6, f"{got:.3f} (expected {want:.3f})")

        # Nonsense replies must not be trusted.
        term._query = lambda req, pat, timeout=0.3: re.search(pat, "\x1b[6;1;99t")
        check("absurd cell size rejected", term.cell_aspect(2.0) == 2.0)
        term._query = lambda req, pat, timeout=0.3: None
        check("silent terminal falls back", term.cell_aspect(2.0) == 2.0)
    finally:
        term._query = real


def test_ramp():
    print("\nascii ramp (measured glyph coverage must increase monotonically)")
    cover = ramp_cover()
    r = asciiview.get_ramp("measured")
    vals = [cover.get(c, -1) for c in r]
    mono = all(b >= a for a, b in zip(vals, vals[1:]))
    check("measured ramp monotone", mono and len(r) >= 8, f"{len(r)} levels: {r!r}")


def ramp_cover():
    import ramp

    return ramp.measure() or {}


def tone_correlation(path: str, cols=110) -> float:
    """How well the art's on-screen brightness tracks the original.

    PSNR is the wrong yardstick for character art: a glyph is the right glyph
    even though its ink lands in different pixels than the source. What should
    hold is that each cell *reads* as bright as the original - the product of
    the chosen glyph's ink coverage and its colour's luminance.
    """
    import glyphs

    args = asciiview.build_parser().parse_args([path, "--cols", str(cols), "--rows", "1000"])
    asciiview.finalize(args)
    src = ImageOps.exif_transpose(Image.open(path)).convert("RGB")
    c, r, sw, sh = asciiview.grid_for(src.width, src.height, args)
    arr = asciiview.pil_to_sample(src, sw, sh)
    if args.levels:
        arr = asciiart.auto_levels(arr)

    chars, bank = glyphs.load()
    cells, fg, _ = asciiart.render_ascii_shape(arr, chars, bank, colour=True)
    cov = glyphs.coverage(bank)
    lookup = {ord(ch): i for i, ch in enumerate(chars)}
    per_cell = np.array([[cov[lookup[int(v)]] for v in row] for row in cells])

    shown = per_cell * asciiart.luminance(fg)
    want = asciiart.luminance(arr).reshape(r, glyphs.GH, c, glyphs.GW).mean(axis=(1, 3))
    return float(np.corrcoef(shown.ravel(), want.ravel())[0, 1])


def test_tone():
    print("\ntone fidelity of character art (displayed brightness vs source)")
    for name in ("testcard.png", "sphere.png", "screenshot.png"):
        path = os.path.join(SAMPLES, name)
        if os.path.exists(path):
            r = tone_correlation(path)
            check(f"{name} tone", r > 0.85, f"correlation {r:.3f}")


def test_images():
    print("\nimage rendering (PSNR vs source, side-by-side PNGs in verify_out/)")
    for name in IMAGES:
        path = os.path.join(SAMPLES, name)
        if not os.path.exists(path):
            continue
        for mode in MODES:
            score = verify.run(path, mode, 100, 1000, "truecolor", OUT)
            # Block modes reproduce colour directly; glyph modes trade colour
            # accuracy for shape, so they are held to a lower bar.
            floor = 18.0 if mode in ("half", "quad") else 4.0
            check(f"{name} {mode}", score > floor, f"PSNR {score:.1f} dB (floor {floor})")


def test_video():
    print("\nvideo")
    clip = os.path.join(SAMPLES, "clip.mp4")
    if not os.path.exists(clip):
        print("  SKIP  no samples/clip.mp4")
        return

    info = asciiview.probe(clip)
    check("probe", info["width"] == 640 and info["height"] == 480 and info["fps"] > 0,
          f"{info['width']}x{info['height']} @ {info['fps']:.0f}fps, {info['duration']:.1f}s")

    # Pull a real frame through the ffmpeg pipe and render it, same as playback.
    with tempfile.TemporaryDirectory() as td:
        png = os.path.join(td, "f.png")
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-ss", "2",
                        "-i", clip, "-frames:v", "1", "-y", png], check=True)
        score = verify.run(png, "half", 100, 1000, "truecolor", OUT)
        check("decoded frame renders", score > 18, f"PSNR {score:.1f} dB")

    # Timed playback, to measure the real frame rate. The art is UTF-8, so the
    # pipe must be decoded as UTF-8 rather than the console's ANSI code page.
    for mode in ("half", "braille"):
        t = time.perf_counter()
        r = subprocess.run([sys.executable, os.path.join(HERE, "asciiview.py"), clip,
                            "--cols", "100", "--rows", "30", "--mode", mode,
                            "--no-audio", "--duration", "3"],
                           capture_output=True, encoding="utf-8", errors="replace", cwd=HERE)
        el = time.perf_counter() - t
        # When stdout is a pipe the alt-screen escapes are inert, so the summary
        # can end up glued to the last frame; match it rather than split lines.
        m = re.search(r"played \d+ frames in [\d.]+s \(([\d.]+) fps, \d+ dropped\)[^\n]*",
                      r.stdout or "")
        summary = m.group(0) if m else ""
        fps = float(m.group(1)) if m else 0.0
        check(f"playback {mode}", fps > 20 and el < 8,
              summary or (r.stderr or "").strip()[:80])


def main():
    print(f"python {sys.version.split()[0]}  numpy {np.__version__}")
    test_geometry()
    test_roundtrip()
    test_full_cell_coverage()
    test_cell_aspect_probe()
    test_ramp()
    test_tone()
    test_images()
    test_video()
    print(f"\n{passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
