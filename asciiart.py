"""Core image -> ANSI terminal art renderer.

Four rendering modes, all geometry-correct (a circle comes out round):

  half    one upper-half-block per cell, fg = top pixel, bg = bottom pixel.
          2 true-colour pixels per cell, square pixels. Best all-round fidelity.
  quad    2x2 sub-pixels per cell using the Unicode quadrant blocks, with the
          four pixels split into two colour groups. Sharpest edges.
  ascii   classic character-density ramp, optionally with colour per character.
  braille 2x4 dots per cell (U+2800 block) with ordered dithering. Highest
          spatial resolution, one colour per cell.

Everything is vectorised with numpy so it is fast enough for video.
"""

from __future__ import annotations

import numpy as np

# Characters ordered from least to most "ink". Classic 70-step ramp.
RAMP = " .'`^\",:;Il!i><~+_-?][}{1)(|\\/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$"
RAMP_SHORT = " .:-=+*#%@"
RAMP_BLOCKS = " ░▒▓█"

# Quadrant blocks indexed by bitmask: 1=top-left 2=top-right 4=bottom-left 8=bottom-right
QUAD_CHARS = " ▘▝▀▖▌▞▛▗▚▐▜▄▙▟█"

# image pixels sampled per terminal cell, (x, y)
SUBCELLS = {"ascii": (1, 1), "half": (1, 2), "quad": (2, 2), "braille": (2, 4)}


def subcells(mode: str, match: str = "shape") -> tuple[int, int]:
    """Pixels sampled per cell. Shape-matched ASCII needs a whole sub-grid."""
    if mode == "ascii" and match == "shape":
        import glyphs

        return glyphs.GW, glyphs.GH
    return SUBCELLS[mode]

# 4x4 Bayer matrix for ordered dithering (stable frame-to-frame, unlike error diffusion)
_BAYER4 = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]], dtype=np.float32)
_BAYER4 = (_BAYER4 + 0.5) / 16.0


_LUMA = np.array([0.2126, 0.7152, 0.0722], dtype=np.float32) / 255.0


def luminance(rgb: np.ndarray) -> np.ndarray:
    """Rec.709 luma of a (..., 3) uint8 array, as float32 in 0..1.

    Done as a matrix product so BLAS handles the per-channel conversion in one
    pass; the obvious three-term form costs about twice as much on video-sized
    frames, and this runs on every frame of every mode.
    """
    flat = np.ascontiguousarray(rgb).reshape(-1, 3)
    return (flat.astype(np.float32) @ _LUMA).reshape(rgb.shape[:-1])


def plan_geometry(img_w, img_h, max_cols, max_rows, mode="half", cell_aspect=2.0, sub=None):
    """Pick a cell grid that preserves the image's aspect ratio.

    A terminal cell is `cell_aspect` times taller than it is wide, so the grid
    must satisfy  cols / (rows * cell_aspect) == img_w / img_h.
    Returns (cols, rows, sample_w, sample_h).
    """
    max_cols = max(1, int(max_cols))
    max_rows = max(1, int(max_rows))
    cols = max_cols
    rows = max(1, round(cols * img_h / (img_w * cell_aspect)))
    if rows > max_rows:
        rows = max_rows
        cols = max(1, min(max_cols, round(rows * cell_aspect * img_w / img_h)))
    sx, sy = sub if sub else SUBCELLS[mode]
    return cols, rows, cols * sx, rows * sy


def adjust(rgb: np.ndarray, gamma=1.0, contrast=1.0, saturation=1.0) -> np.ndarray:
    """Optional tone/colour tweaks. Returns uint8."""
    if gamma == 1.0 and contrast == 1.0 and saturation == 1.0:
        return rgb
    f = rgb.astype(np.float32) / 255.0
    if gamma != 1.0:
        f = np.power(np.clip(f, 0.0, 1.0), 1.0 / gamma)
    if contrast != 1.0:
        f = (f - 0.5) * contrast + 0.5
    if saturation != 1.0:
        grey = (0.2126 * f[..., 0] + 0.7152 * f[..., 1] + 0.0722 * f[..., 2])[..., None]
        f = grey + (f - grey) * saturation
    return np.clip(f * 255.0, 0, 255).astype(np.uint8)


def colorize(rgb: np.ndarray, target: np.ndarray, whiten: float = 0.0) -> np.ndarray:
    """Recolour so each cell's luminance equals `target` (0..1), keeping hue.

    Scaling a peak-normalised colour by a brightness factor does NOT give that
    brightness: a saturated hue carries little luminance to begin with (pure
    blue is 0.07), so a blue sky comes out almost black. Here the hue is first
    taken to full value, then scaled to hit the target exactly - and when the
    target is brighter than the hue can reach at full saturation, it blends
    towards white, which is what a bright blue actually looks like.
    """
    f = rgb.astype(np.float32)
    peak = f.max(axis=-1, keepdims=True)
    hue = f / np.maximum(peak, 1e-3)                  # full-value colour, hue kept
    lh = luminance(np.clip(hue * 255.0, 0, 255))[..., None]
    t = np.clip(target, 0.0, 1.0)[..., None]

    darker = hue * (t / np.maximum(lh, 1e-3))         # room to spare: just scale down
    k = np.clip((t - lh) / np.maximum(1.0 - lh, 1e-3), 0.0, 1.0)
    # Glyphs cover at most ~35% of a cell, so hitting the target exactly would
    # push nearly everything to white and throw the colour away. `whiten` caps
    # how far saturated hues are allowed to wash out: 0 keeps colour fully and
    # accepts a dimmer picture, 1 matches brightness and goes monochrome.
    k = np.minimum(k, whiten)
    lighter = hue + (1.0 - hue) * k                   # out of room: desaturate to white
    out = np.where(t <= lh, darker, lighter)
    return np.clip(out * 255.0, 0, 255).astype(np.uint8)


def _brighten(rgb: np.ndarray, amount: float) -> np.ndarray:
    """Push colours towards full value, keeping hue.

    In coloured-ASCII mode the glyph already encodes brightness, so a dark
    pixel would otherwise be darkened twice (sparse glyph *and* dark colour).
    """
    if amount <= 0:
        return rgb
    f = rgb.astype(np.float32)
    peak = f.max(axis=-1, keepdims=True)
    scale = np.where(peak > 1.0, 255.0 / np.maximum(peak, 1.0), 1.0)
    scale = 1.0 + (scale - 1.0) * amount
    return np.clip(f * scale, 0, 255).astype(np.uint8)


# --------------------------------------------------------------------------- modes
# Each renderer takes an RGB array already sampled at the mode's resolution and
# returns (codepoints (rows, cols) int32, fg (rows, cols, 3) uint8, bg or None).


def render_half(arr: np.ndarray):
    top, bot = arr[0::2], arr[1::2]
    rows = min(top.shape[0], bot.shape[0])
    top, bot = top[:rows], bot[:rows]
    cells = np.full(top.shape[:2], 0x2580, dtype=np.int32)  # UPPER HALF BLOCK
    # Where both halves match, draw a space in the background colour rather
    # than a full block in the foreground. Both look identical when the font's
    # block glyph fills its cell exactly - but a terminal with extra line
    # spacing draws the block short, letting the untouched background show
    # through as black scanlines. A space is always painted edge to edge.
    flat = np.all(top == bot, axis=-1)
    cells[flat] = 0x20
    return cells, top.copy(), bot.copy()


def render_quad(arr: np.ndarray):
    h, w = arr.shape[0] // 2 * 2, arr.shape[1] // 2 * 2
    a = arr[:h, :w]
    rows, cols = h // 2, w // 2
    # (rows, cols, 4, 3) with sub-pixel order TL, TR, BL, BR
    cell = a.reshape(rows, 2, cols, 2, 3).transpose(0, 2, 1, 3, 4).reshape(rows, cols, 4, 3)
    lum = luminance(cell)
    # Split the four pixels into a bright group (foreground) and a dark one.
    mask = lum >= lum.mean(axis=2, keepdims=True)
    bits = np.array([1, 2, 4, 8], dtype=np.int32)
    idx = (mask.astype(np.int32) * bits).sum(axis=2)

    f = cell.astype(np.float32)
    m = mask[..., None].astype(np.float32)
    n_fg = m.sum(axis=2)
    n_bg = 4.0 - n_fg
    fg = (f * m).sum(axis=2) / np.maximum(n_fg, 1.0)
    bg = (f * (1.0 - m)).sum(axis=2) / np.maximum(n_bg, 1.0)
    empty = (n_bg[..., 0] == 0)
    bg[empty] = fg[empty]  # solid cell: background never shows

    cells = np.array([ord(c) for c in QUAD_CHARS], dtype=np.int32)[idx]
    fg, bg = fg.astype(np.uint8), bg.astype(np.uint8)
    # Same reasoning as render_half: a solid cell becomes a background-coloured
    # space, so it cannot leave gaps where the glyph falls short of the cell.
    solid = (idx == 15)
    cells[solid] = 0x20
    bg[solid] = fg[solid]
    return cells, fg, bg


def auto_levels(rgb: np.ndarray, low_pct=1.0, high_pct=99.0) -> np.ndarray:
    """Stretch the luminance histogram to the full range, preserving hue.

    Photographs rarely span 0..1, so without this the glyph ramp only ever uses
    its middle section and the art comes out flat.
    """
    lum = luminance(rgb)
    lo, hi = np.percentile(lum, [low_pct, high_pct])
    if hi - lo < 1e-3:
        return rgb
    scale = np.clip((lum - lo) / (hi - lo), 0.0, 1.0) / np.maximum(lum, 1e-3)
    return np.clip(rgb.astype(np.float32) * scale[..., None], 0, 255).astype(np.uint8)


def render_ascii(arr: np.ndarray, ramp=RAMP, invert=False, colour=True, density=0.7):
    """Character-density art.

    Perceived brightness of a cell is roughly (glyph ink coverage) x (colour
    brightness), so the source luminance L is split between the two:
    coverage ~ L**density and colour brightness ~ L**(1-density). That keeps
    the total right instead of darkening twice, which is what makes a sparse
    glyph in a dark colour vanish.
    """
    lum = np.clip(luminance(arr), 0.0, 1.0)
    if invert:
        lum = 1.0 - lum

    table = np.array([ord(c) for c in ramp], dtype=np.int32)
    coverage = np.power(lum, density)
    idx = np.clip((coverage * len(ramp)).astype(np.int32), 0, len(ramp) - 1)
    cells = table[idx]

    if colour:
        fg = colorize(arr, np.power(lum, 1.0 - density))  # brightness colour must carry
    else:
        fg = np.full(arr.shape, 255, np.uint8)
    return cells, fg, None


_BANK_TERMS: dict[int, tuple] = {}


def _bank_terms(bank: np.ndarray):
    """Per-glyph constants for shape matching, computed once per bank."""
    key = id(bank)
    hit = _BANK_TERMS.get(key)
    if hit is not None:
        return hit

    flat = bank.reshape(len(bank), -1).astype(np.float32)
    n = flat.shape[1]
    gm = flat.mean(axis=1)
    cmax = float(gm.max())
    gn2 = np.maximum((flat * flat).sum(axis=1) - n * gm * gm, 0.0)
    gnorm = (1.0 / (np.sqrt(gn2) + 1e-6)).astype(np.float32)   # 1 / ||g - mean||
    gmat = np.ascontiguousarray((gnorm[:, None] * flat).T)     # (n, glyphs), for one sgemm
    out = (gm, cmax, n, (gm, gmat, (n * gm * gnorm).astype(np.float32),
                         (gm * gm / (cmax * cmax)).astype(np.float32)))
    _BANK_TERMS.clear()          # one bank is in play at a time
    _BANK_TERMS[key] = out
    return out


def render_ascii_shape(arr: np.ndarray, chars: str, bank: np.ndarray,
                       invert=False, colour=True, edge=1.0, whiten=0.0):
    """Character art that matches glyph *shape* to the image, not just brightness.

    Each cell is a GH x GW patch of luminance. Every candidate glyph is a
    bitmap of the same size, so picking the glyph with the smallest squared
    difference gives '/' on a diagonal edge, '|' on a vertical one and '@' on
    a bright flat patch - all from one comparison, with no edge detection.

    The image is scaled into the bank's coverage range first, so mid-grey is
    compared against mid-density glyphs rather than against solid ink.
    """
    import glyphs

    gw, gh = glyphs.GW, glyphs.GH
    rows, cols = arr.shape[0] // gh, arr.shape[1] // gw
    a = arr[:rows * gh, :cols * gw]

    lum = np.clip(luminance(a), 0.0, 1.0)
    if invert:
        lum = 1.0 - lum
    blocks = lum.reshape(rows, gh, cols, gw).transpose(0, 2, 1, 3).reshape(rows * cols, gh * gw)

    cover, cmax, n, gterms = _bank_terms(bank)
    gm, gmat, gmn, gm2c = gterms
    scaled = (blocks * cmax).astype(np.float32)   # into "mean coverage" units

    # Tone and structure are scored separately rather than as one squared
    # difference. Raw SSD punishes a glyph for concentrating its ink, so a flat
    # mid-grey patch scores worse against '|' than against ' ' and the midtones
    # collapse to blank. Tone compares average coverage; structure compares the
    # patterns only after removing both means:
    #
    #   score = (bm - gm)^2 / cmax^2  +  w * strength * (1 - corr)
    #
    # Expanded, every term that depends only on the cell is the same for all
    # glyphs and cannot change which one wins, so bm^2, w*strength and the
    # 1/||b|| inside corr all fold into per-cell scalars and drop out. What is
    # left is one matmul plus three rank-1 updates.
    bm = scaled.mean(axis=1)
    bn2 = np.maximum((scaled * scaled).sum(axis=1) - n * bm * bm, 0.0)

    # A patch with no internal contrast has no structure to match, so let tone
    # decide there instead of chasing noise.
    strength = np.clip(np.sqrt(bn2 / n) / (0.12 * cmax), 0.0, 1.0)
    alpha = (0.5 * edge) * strength / (np.sqrt(bn2) + 1e-6)
    beta = (2.0 / (cmax * cmax)) * bm

    scores = scaled @ gmat
    scores *= -alpha[:, None]
    scores += gm2c[None, :]
    scores -= beta[:, None] * gm[None, :]
    scores += (alpha * bm)[:, None] * gmn[None, :]
    idx = scores.argmin(axis=1)

    cells = np.array([ord(c) for c in chars], dtype=np.int32)[idx].reshape(rows, cols)

    if colour:
        # The glyph now fixes how much ink the cell has, so the colour carries
        # whatever brightness is left over: coverage x colour = cell luminance.
        # Averaging each cell's pixels via a BOX resize: Pillow does it in C in
        # about a third of the time numpy takes to reduce the strided view.
        from PIL import Image

        mean_rgb = np.asarray(Image.fromarray(a).resize((cols, rows), Image.BOX),
                              dtype=np.uint8).astype(np.float32)
        want = blocks.mean(axis=1).reshape(rows, cols)
        # coverage x colour luminance = cell luminance, so solve for the colour.
        fg = colorize(mean_rgb, want / np.maximum(cover[idx].reshape(rows, cols), 0.02), whiten)
    else:
        fg = np.full(cells.shape + (3,), 255, np.uint8)
    return cells, fg, None


def render_braille(arr: np.ndarray, invert=False, colour=True, threshold=None):
    h, w = arr.shape[0] // 4 * 4, arr.shape[1] // 2 * 2
    a = arr[:h, :w]
    rows, cols = h // 4, w // 2
    lum = luminance(a)
    if invert:
        lum = 1.0 - lum
    if threshold is None:
        # Ordered dithering: compare against a tiled Bayer matrix instead of a
        # flat cutoff, which recovers shading detail from 1-bit dots.
        tile = np.tile(_BAYER4, (h // 4 + 1, w // 4 + 1))[:h, :w]
        on = lum > tile
    else:
        on = lum > threshold

    # (rows, cols, 2 x-dots, 4 y-dots)
    dots = on.reshape(rows, 4, cols, 2).transpose(0, 2, 3, 1)
    bits = np.array([[0x01, 0x02, 0x04, 0x40], [0x08, 0x10, 0x20, 0x80]], dtype=np.int32)
    cells = 0x2800 + (dots.astype(np.int32) * bits).sum(axis=(2, 3))

    if colour:
        px = a.reshape(rows, 4, cols, 2, 3).transpose(0, 2, 3, 1, 4).reshape(rows, cols, 8, 3)
        m = dots.reshape(rows, cols, 8, 1).astype(np.float32)
        n = m.sum(axis=2)
        lit = (px.astype(np.float32) * m).sum(axis=2) / np.maximum(n, 1.0)
        allm = px.astype(np.float32).mean(axis=2)
        fg = np.where(n > 0, lit, allm)
        fg = _brighten(fg.astype(np.uint8), 0.6)
    else:
        fg = np.full(cells.shape + (3,), 255, np.uint8)
    return cells, fg, None


def render(arr: np.ndarray, mode="half", **kw):
    if mode == "half":
        return render_half(arr)
    if mode == "quad":
        return render_quad(arr)
    if mode == "ascii":
        if kw.pop("match", "shape") == "shape":
            kw.pop("ramp", None)
            kw.pop("density", None)
            return render_ascii_shape(arr, **kw)
        kw.pop("chars", None)
        kw.pop("bank", None)
        kw.pop("edge", None)
        return render_ascii(arr, **kw)
    if mode == "braille":
        return render_braille(arr, **kw)
    raise ValueError(f"unknown mode: {mode}")


# ----------------------------------------------------------------------- encoding

def rgb_to_256(rgb: np.ndarray) -> np.ndarray:
    """Nearest xterm-256 index for a (..., 3) uint8 array."""
    f = rgb.astype(np.int32)
    # 6x6x6 colour cube; levels are 0, 95, 135, 175, 215, 255
    levels = np.array([0, 95, 135, 175, 215, 255], dtype=np.int32)
    ci = np.abs(f[..., None, :] - levels[:, None]).argmin(axis=-2)
    cube_rgb = levels[ci]
    cube_idx = 16 + 36 * ci[..., 0] + 6 * ci[..., 1] + ci[..., 2]
    cube_err = ((cube_rgb - f) ** 2).sum(axis=-1)
    # 24-step grey ramp at 8 + 10*n
    grey = f.mean(axis=-1)
    gi = np.clip(np.rint((grey - 8.0) / 10.0), 0, 23).astype(np.int32)
    grey_val = 8 + 10 * gi
    grey_err = ((grey_val[..., None] - f) ** 2).sum(axis=-1)
    return np.where(grey_err < cube_err, 232 + gi, cube_idx)


def quantize(rgb: np.ndarray, bits: int) -> np.ndarray:
    """Drop `bits` low bits per channel.

    Visually almost free, but it makes neighbouring cells share colours, which
    both lengthens the encoder's colour runs and raises the SGR cache hit rate.
    Worth 2 bits or so when playing video.
    """
    if bits <= 0:
        return rgb
    m = (0xFF << bits) & 0xFF
    half = (1 << bits) >> 1
    return (rgb & m) | half if bits < 8 else rgb


def _color_keys(rgb: np.ndarray, colours: str) -> np.ndarray:
    """Map colours to compact int keys (one per cell)."""
    if colours == "truecolor":
        f = rgb.astype(np.int32)
        return (f[..., 0] << 16) | (f[..., 1] << 8) | f[..., 2]
    if colours == "256":
        return rgb_to_256(rgb)
    if colours == "16":
        f = rgb.astype(np.float32)
        bright = (f.max(axis=-1) > 160).astype(np.int32)
        bits = (f > 96).astype(np.int32)
        # low 3 bits are r,g,b flags, which is exactly the ANSI colour order
        return bits[..., 0] | (bits[..., 1] << 1) | (bits[..., 2] << 2) | (bright << 3)
    raise ValueError(colours)


# key -> escape string, kept between frames: consecutive video frames reuse
# most of their colours, so this turns into a near-pure lookup after a second.
_SGR_CACHE: dict[tuple[str, int], dict[int, str]] = {}
_SGR_CACHE_MAX = 200_000


def _sgr_cache(colours: str, base: int) -> dict[int, str]:
    cache = _SGR_CACHE.setdefault((colours, base), {})
    if len(cache) > _SGR_CACHE_MAX:
        cache.clear()
    return cache


def _make_sgr(key: int, base: int, colours: str) -> str:
    if colours == "truecolor":
        return "\x1b[%d;2;%d;%d;%dm" % (base, (key >> 16) & 255, (key >> 8) & 255, key & 255)
    if colours == "256":
        return "\x1b[%d;5;%dm" % (base, key)
    return "\x1b[%dm" % (base + (60 if key & 8 else 0) + (key & 7))


def encode(cells, fg, bg=None, colours="truecolor", clear_eol=True, row_sep="\n"):
    """Turn a rendered grid into an ANSI string.

    Colour escapes are only emitted when the colour actually changes, which
    typically cuts the byte count several-fold on real pictures.
    """
    rows, cols = cells.shape
    ch = cells.tolist()
    if colours == "none":
        return row_sep.join("".join(map(chr, ch[y])) for y in range(rows))

    fk = _color_keys(fg, colours).tolist()
    fcache = _sgr_cache(colours, 38)
    if bg is not None:
        bk = _color_keys(bg, colours).tolist()
        bcache = _sgr_cache(colours, 48)
    else:
        bk = bcache = None

    eol = "\x1b[0m" + ("\x1b[K" if clear_eol else "")
    out = []
    for y in range(rows):
        cr, fr = ch[y], fk[y]
        br = bk[y] if bk is not None else None
        pf = pb = None
        row = []
        push = row.append
        for x in range(cols):
            c = cr[x]
            if br is not None:
                b = br[x]
                if b != pb:
                    s = bcache.get(b)
                    if s is None:
                        s = bcache[b] = _make_sgr(b, 48, colours)
                    push(s)
                    pb = b
            if c != 0x20:  # a space has no foreground
                f = fr[x]
                if f != pf:
                    s = fcache.get(f)
                    if s is None:
                        s = fcache[f] = _make_sgr(f, 38, colours)
                    push(s)
                    pf = f
            push(chr(c))
        out.append("".join(row))
    return (eol + row_sep).join(out) + eol
