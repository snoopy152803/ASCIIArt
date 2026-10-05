#!/usr/bin/env python3
"""Regenerate the sample images and clips. Needs ffmpeg on PATH for the video.

    python samples/make_samples.py
"""

from __future__ import annotations

import os
import random
import subprocess

import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))


def out(name):
    return os.path.join(HERE, name)


def testcard(w=640, h=480):
    """Flat shapes and a linear gradient: checks tone ramp and edges."""
    img = Image.new("RGB", (w, h), (0, 0, 0))
    d = ImageDraw.Draw(img)
    for x in range(w):
        v = int(255 * x / (w - 1))
        d.rectangle([x, 0, x, h // 3], fill=(v, v, v))
    d.ellipse([40, h // 3 + 20, 220, h // 3 + 200], fill=(255, 255, 255))
    d.polygon([(260, h // 3 + 200), (360, h // 3 + 20), (460, h // 3 + 200)], fill=(200, 200, 200))
    for i in range(0, 8, 2):
        x0 = 480 + i * 18
        d.rectangle([x0, h // 3 + 20, x0 + 9, h // 3 + 200], fill=(255, 255, 255))
    try:
        f = ImageFont.truetype(r"C:\Windows\Fonts\arialbd.ttf", 90)
    except Exception:
        f = ImageFont.load_default()
    d.text((40, h - 130), "ASCII", font=f, fill=(255, 255, 255))
    img.save(out("testcard.png"))


def sphere(w=640, h=480):
    """A lit sphere over a checkered floor.

    Smooth shading plus hard edges plus perspective texture: the standard way
    to judge whether tonal art holds up, and much closer to a real photograph
    than flat shapes are.
    """
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    u = (x - w / 2) / (h / 2)
    v = (y - h / 2) / (h / 2)

    img = np.zeros((h, w, 3), np.float32)

    # sky: vertical gradient
    img[..., 0] = 0.10 + 0.25 * (1 - y / h)
    img[..., 1] = 0.14 + 0.34 * (1 - y / h)
    img[..., 2] = 0.22 + 0.52 * (1 - y / h)

    # floor: checkerboard in perspective, below the horizon
    horizon = 0.12
    below = v > horizon
    depth = np.where(below, 1.0 / np.maximum(v - horizon, 1e-3), 0.0)
    cu = np.floor(u * depth * 0.7)
    cv = np.floor(depth * 0.7)
    check = ((cu + cv) % 2 == 0)
    fade = np.clip(1.0 - depth / 60.0, 0.15, 1.0)
    floor = np.where(check, 0.78, 0.26) * fade
    for c, tint in enumerate((1.0, 0.95, 0.85)):
        img[..., c] = np.where(below, floor * tint, img[..., c])

    # sphere, lit from the upper left with a specular highlight
    cx, cy, r = -0.15, -0.05, 0.52
    dx, dy = u - cx, v - cy
    d2 = dx * dx + dy * dy
    on = d2 < r * r
    nz = np.sqrt(np.maximum(r * r - d2, 0.0)) / r
    nx, ny = dx / r, dy / r
    lx, ly, lz = -0.5, -0.65, 0.57
    diff = np.clip(nx * lx + ny * ly + nz * lz, 0.0, 1.0)
    spec = np.power(diff, 48.0)
    body = 0.08 + 0.92 * diff
    for c, tint in enumerate((1.0, 0.45, 0.35)):
        img[..., c] = np.where(on, np.clip(body * tint + spec, 0, 1), img[..., c])

    Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8)).save(out("sphere.png"))




def _sky(img, w, h, top, bottom, upto=None):
    """Smooth vertical gradient, dithered per pixel.

    Filling one flat rectangle per row quantises the ramp to whole 8-bit
    steps, which puts a real edge on every scanline. That is invisible in the
    tonal modes but line-art mode finds every one of them and the picture
    fills with dashes. Dithering has to be per pixel to help: a single offset
    per row just replaces a monotonic step with a random one.
    """
    upto = h if upto is None else upto
    t = (np.arange(upto, dtype=np.float32) / max(1, upto))[:, None, None]
    a = np.array(top, dtype=np.float32)[None, None, :]
    b = np.array(bottom, dtype=np.float32)[None, None, :]
    ramp = a + (b - a) * t                                   # (upto, 1, 3)
    ramp = np.repeat(ramp, w, axis=1)
    noise = np.random.default_rng(7).random((upto, w, 1)).astype(np.float32) - 0.5
    px = np.clip(ramp + noise, 0, 255).astype(np.uint8)
    img.paste(Image.fromarray(px), (0, 0))


def bench(w=720, h=540):
    """A park bench under a tree, late afternoon. Strong silhouette."""
    img = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(img)
    _sky(img, w, h, (250, 206, 140), (252, 243, 214), int(h * 0.72))
    d.rectangle([0, int(h * 0.72), w, h], fill=(96, 122, 66))          # grass
    d.ellipse([-80, int(h * 0.68), w + 80, h + 120], fill=(78, 104, 54))

    sx, sy = int(w * 0.76), int(h * 0.20)                              # low sun
    for r in range(130, 0, -6):
        v = 1 - r / 130.0
        d.ellipse([sx - r, sy - r, sx + r, sy + r],
                  fill=(255, int(226 + 20 * v), int(150 + 80 * v)))

    tx = int(w * 0.17)                                                 # tree
    d.polygon([(tx - 16, int(h * 0.74)), (tx - 9, int(h * 0.20)),
               (tx + 9, int(h * 0.20)), (tx + 16, int(h * 0.74))], fill=(58, 44, 34))
    for cx, cy, r in ((tx - 10, int(h * 0.17), 86), (tx + 56, int(h * 0.22), 64),
                      (tx + 10, int(h * 0.09), 70), (tx - 62, int(h * 0.24), 58)):
        d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(46, 78, 44))

    bx, by, bw = int(w * 0.40), int(h * 0.60), int(w * 0.40)           # bench
    seat, back, legs = (116, 74, 48), (132, 86, 56), (54, 46, 42)
    for i in range(3):                                                 # backrest
        y0 = by - 76 + i * 20
        d.rectangle([bx, y0, bx + bw, y0 + 12], fill=back)
    d.rectangle([bx, by, bx + bw, by + 14], fill=seat)                 # seat
    d.rectangle([bx + 6, by + 10, bx + bw - 6, by + 20], fill=(92, 60, 40))
    for lx in (bx + 10, bx + bw - 22):                                 # legs + arms
        d.rectangle([lx, by + 14, lx + 12, by + 86], fill=legs)
        d.rectangle([lx, by - 80, lx + 10, by + 4], fill=legs)
    d.ellipse([bx - 30, by + 78, bx + bw + 30, by + 100], fill=(70, 94, 52))
    img.save(out("bench.png"))


def mountains(w=760, h=500):
    """Lake, peaks, reflection: a wide, smooth tonal range."""
    img = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(img)
    horizon = int(h * 0.60)
    _sky(img, w, h, (58, 96, 158), (206, 196, 176), horizon)
    d.ellipse([int(w * 0.70), int(h * 0.12), int(w * 0.70) + 74, int(h * 0.12) + 74],
              fill=(255, 250, 232))

    ranges = [((40, 58, 86), 0.30, 7), ((64, 84, 112), 0.42, 5), ((92, 110, 132), 0.52, 4)]
    import random
    rng = random.Random(11)
    for colour, base, peaks in ranges:
        pts = [(0, horizon)]
        for i in range(peaks + 1):
            x = w * i / peaks
            y = h * (base + rng.uniform(-0.07, 0.03))
            pts.append((x, y))
        pts.append((w, horizon))
        d.polygon(pts, fill=colour)
        snow = [(p[0], p[1]) for p in pts[1:-1]]
        for px, py in snow:
            if py < h * 0.38:
                d.polygon([(px - 26, py + 34), (px, py), (px + 26, py + 34)],
                          fill=(236, 240, 246))

    d.rectangle([0, horizon, w, h], fill=(46, 72, 104))                # lake
    mirror = img.crop((0, int(h * 0.26), w, horizon)).transpose(Image.FLIP_TOP_BOTTOM)
    mirror = mirror.point(lambda v: int(v * 0.55 + 30))
    img.paste(mirror, (0, horizon))
    dd = ImageDraw.Draw(img)
    for y in range(horizon, h, 5):                                     # ripples
        dd.rectangle([0, y, w, y + 1], fill=(52, 80, 112))
    img.save(out("mountains.png"))


def city(w=760, h=520):
    """Skyline at dusk with lit windows - lots of fine high-contrast detail."""
    import random
    rng = random.Random(5)
    img = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(img)
    ground = int(h * 0.86)
    _sky(img, w, h, (26, 30, 62), (236, 142, 92), ground)
    d.ellipse([int(w * 0.14), int(h * 0.60), int(w * 0.14) + 96, int(h * 0.60) + 96],
              fill=(255, 206, 140))

    x = -20
    while x < w + 20:
        bw = rng.randint(42, 92)
        bh = rng.randint(int(h * 0.20), int(h * 0.62))
        top = ground - bh
        shade = rng.randint(22, 46)
        d.rectangle([x, top, x + bw, ground], fill=(shade, shade + 4, shade + 14))
        for wy in range(top + 10, ground - 10, 18):                    # windows
            for wx in range(x + 8, x + bw - 10, 14):
                if rng.random() < 0.42:
                    lit = (255, 214, 130) if rng.random() < 0.75 else (150, 196, 235)
                    d.rectangle([wx, wy, wx + 7, wy + 10], fill=lit)
        x += bw + rng.randint(4, 16)
    d.rectangle([0, ground, w, h], fill=(16, 18, 28))
    img.save(out("city.png"))


def chess(w=640, h=640):
    """A chessboard with turned pieces, viewed flat on."""
    img = Image.new("RGB", (w, h), (28, 28, 32))
    d = ImageDraw.Draw(img)
    m = int(w * 0.05)
    sq = (w - 2 * m) // 8
    light, dark = (226, 214, 188), (92, 74, 58)
    for r in range(8):
        for c in range(8):
            d.rectangle([m + c * sq, m + r * sq, m + (c + 1) * sq, m + (r + 1) * sq],
                        fill=light if (r + c) % 2 == 0 else dark)

    def piece(col, row, height, white):
        cx = m + col * sq + sq // 2
        base = m + row * sq + int(sq * 0.80)
        body = (246, 243, 236) if white else (34, 32, 34)
        edge = (170, 165, 155) if white else (72, 70, 72)
        d.ellipse([cx - sq * 0.30, base - sq * 0.10, cx + sq * 0.30, base + sq * 0.10],
                  fill=edge)
        top = base - sq * height
        d.polygon([(cx - sq * 0.22, base), (cx - sq * 0.12, top + sq * 0.12),
                   (cx + sq * 0.12, top + sq * 0.12), (cx + sq * 0.22, base)], fill=body)
        d.ellipse([cx - sq * 0.17, top, cx + sq * 0.17, top + sq * 0.30], fill=body)
        d.ellipse([cx - sq * 0.10, top + sq * 0.04, cx - sq * 0.02, top + sq * 0.14],
                  fill=edge)

    for c in range(8):
        piece(c, 6, 0.45, True)
        piece(c, 1, 0.45, False)
    for c, hgt in ((0, 0.55), (7, 0.55), (1, 0.6), (6, 0.6), (2, 0.65), (5, 0.65),
                   (3, 0.85), (4, 0.95)):
        piece(c, 7, hgt, True)
        piece(c, 0, hgt, False)
    img.save(out("chess.png"))



def ff(args, name):
    try:
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", *args, "-y", out(name)],
                       check=True)
    except Exception as e:
        print(f"  skipped {name}: {e}")


def main():
    testcard()
    sphere()
    bench()
    mountains()
    city()
    chess()
    ff(["-f", "lavfi", "-i", "smptebars=size=640x480", "-frames:v", "1"], "bars.png")
    ff(["-f", "lavfi", "-i", "testsrc2=size=640x480:rate=30:duration=6",
        "-f", "lavfi", "-i", "sine=frequency=440:duration=6",
        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest"], "clip.mp4")
    ff(["-f", "lavfi", "-i", "testsrc2=size=320x240:rate=12:duration=2"], "anim.gif")
    for f in sorted(os.listdir(HERE)):
        if not f.endswith(".py"):
            print(f"  {f}")


if __name__ == "__main__":
    main()
