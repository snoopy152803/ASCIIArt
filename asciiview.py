#!/usr/bin/env python3
"""asciiview - show pictures and play videos in the terminal as coloured ASCII art.

    python asciiview.py photo.jpg
    python asciiview.py photo.jpg --mode ascii
    python asciiview.py clip.mp4
    python asciiview.py anim.gif
    python asciiview.py camera

Images, animated GIFs, videos (anything ffmpeg can decode) and webcams.
Run with --help for the full option list.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time

import numpy as np
from PIL import Image, ImageOps, ImageSequence

import asciiart
import glyphs
import ramp as ramp_mod
import term

IMAGE_EXT = {".png", ".jpg", ".jpeg", ".bmp", ".webp", ".tif", ".tiff", ".tga", ".ico", ".ppm", ".pgm"}
VIDEO_EXT = {".mp4", ".mkv", ".mov", ".avi", ".webm", ".m4v", ".wmv", ".flv", ".mpg", ".mpeg", ".ts", ".ogv", ".m2ts"}

RAMPS = {"full": asciiart.RAMP, "short": asciiart.RAMP_SHORT, "blocks": asciiart.RAMP_BLOCKS}

CSI = "\x1b["            # escape prefix for terminal queries
ESC_RE = "\\x1b"          # the same, inside a regex
FULL_BLOCK = "\u2588"


def get_ramp(name: str) -> str:
    """'measured' is built from real glyph ink coverage; the rest are fixed."""
    if name == "measured":
        return ramp_mod.build(32)
    return RAMPS[name]


# ------------------------------------------------------------------ frame -> text

def frame_to_text(arr: np.ndarray, args) -> str:
    """Render one RGB numpy frame (already at sample resolution) to an ANSI string."""
    arr = asciiart.adjust(arr, args.gamma, args.contrast, args.saturation)
    if args.levels:
        arr = asciiart.auto_levels(arr)
    if args.quantize:
        arr = asciiart.quantize(arr, args.quantize)
    kw = {}
    if args.mode == "ascii":
        kw = {"match": args.match, "invert": args.invert, "colour": args.colours != "none"}
        if args.match == "shape":
            chars, bank = glyphs.load(args.chars or glyphs.DEFAULT_CHARS)
            kw.update(chars=chars, bank=bank, edge=args.edge, whiten=args.whiten)
        else:
            kw.update(ramp=args.ramp_chars, density=args.density)
    elif args.mode == "braille":
        kw = {"invert": args.invert, "colour": args.colours != "none",
              "threshold": args.threshold}
    cells, fg, bg = asciiart.render(arr, args.mode, **kw)
    return asciiart.encode(cells, fg, bg, colours=args.colours, clear_eol=args.clear_eol)


def grid_for(img_w: int, img_h: int, args):
    """Resolve the cell grid and the pixel size to sample at."""
    cols, rows = args.cols, args.rows
    if cols is None or rows is None:
        tc, tl = term.size()
        cols = cols if cols is not None else tc
        rows = rows if rows is not None else max(1, tl - 1)
    sub = asciiart.subcells(args.mode, getattr(args, "match", "shape"))

    # Cap the grid for video. Shape-matched ascii samples 4x8 pixels per cell,
    # so a big terminal asks ffmpeg for an enormous frame: 877x329 cells means
    # scaling every frame to 3508x2632 and pushing 831 MB/s down the pipe,
    # which simply stalls. Stills are left alone - they are rendered once.
    cap = getattr(args, "max_cells", 0)
    if cap and cols * rows > cap:
        shrink = (cap / float(cols * rows)) ** 0.5
        cols = max(16, int(cols * shrink))
        rows = max(6, int(rows * shrink))

    if args.fill:
        return cols, rows, cols * sub[0], rows * sub[1]
    return asciiart.plan_geometry(img_w, img_h, cols, rows, args.mode, args.cell_aspect, sub)


def pil_to_sample(img: Image.Image, sample_w: int, sample_h: int) -> np.ndarray:
    img = img.convert("RGB").resize((max(1, sample_w), max(1, sample_h)), Image.LANCZOS)
    return np.asarray(img, dtype=np.uint8)


# ----------------------------------------------------------------------- pictures

def show_image(path: str, args) -> int:
    img = Image.open(path)
    img = ImageOps.exif_transpose(img)  # honour camera rotation
    frames = getattr(img, "n_frames", 1)

    if frames > 1 and not args.still:
        return play_gif(img, args)

    cols, rows, sw, sh = grid_for(img.width, img.height, args)
    text = frame_to_text(pil_to_sample(img, sw, sh), args)

    if args.out:
        with open(args.out, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text + "\n")
        print(f"wrote {args.out}  ({cols}x{rows} cells, mode={args.mode}, colours={args.colours})")
        return 0

    term.write(text + "\n")
    if args.info:
        print(f"{os.path.basename(path)}  {img.width}x{img.height} -> "
              f"{cols}x{rows} cells  mode={args.mode}  colours={args.colours}  "
              f"cell-aspect={args.cell_aspect:.2f}")
    return 0


def play_gif(img: Image.Image, args) -> int:
    """Loop an animated GIF/WebP using its own frame delays."""
    cols, rows, sw, sh = grid_for(img.width, img.height, args)
    frames, delays = [], []
    for fr in ImageSequence.Iterator(img):
        frames.append(frame_to_text(pil_to_sample(fr, sw, sh), args))
        delays.append(max(0.02, fr.info.get("duration", 100) / 1000.0) / args.speed)
    if not frames:
        return 1
    return _run_frames(lambda i: (frames[i % len(frames)], delays[i % len(frames)]),
                       total=None if args.loop else len(frames), args=args, rows=rows)


def _run_frames(get, total, args, rows) -> int:
    """Shared playback loop for pre-rendered frame sequences."""
    term.write(term.ALT_SCREEN_ON + term.HIDE_CURSOR + term.CLEAR)
    paused = False
    i = 0
    try:
        with term.KeyPoller() as keys:
            next_t = time.perf_counter()
            while total is None or i < total:
                text, delay = get(i)
                term.write(term.HOME + text)
                i += 1
                next_t += delay
                while True:
                    k = keys.get()
                    if k in ("q", "Q", "\x1b", "\x03"):
                        return 0
                    if k == " ":
                        paused = not paused
                        next_t = time.perf_counter()
                    if paused:
                        time.sleep(0.03)
                        next_t = time.perf_counter()
                        continue
                    wait = next_t - time.perf_counter()
                    if wait <= 0:
                        break
                    time.sleep(min(wait, 0.02))
    except KeyboardInterrupt:
        pass
    finally:
        term.write(term.SHOW_CURSOR + term.RESET + term.ALT_SCREEN_OFF)
    return 0


# ------------------------------------------------------------------------- video

def _tool(name: str) -> str | None:
    return shutil.which(name)


def probe(path: str) -> dict:
    """Width/height/fps/duration via ffprobe, with an ffmpeg-stderr fallback."""
    info = {"width": 0, "height": 0, "fps": 0.0, "duration": 0.0}
    ffprobe = _tool("ffprobe")
    if ffprobe:
        cmd = [ffprobe, "-v", "error", "-select_streams", "v:0", "-show_entries",
               "stream=width,height,avg_frame_rate:format=duration", "-of", "json", path]
        try:
            out = subprocess.run(cmd, capture_output=True, text=True, timeout=30).stdout
            data = json.loads(out or "{}")
            st = (data.get("streams") or [{}])[0]
            info["width"] = int(st.get("width") or 0)
            info["height"] = int(st.get("height") or 0)
            rate = st.get("avg_frame_rate") or "0/0"
            num, _, den = rate.partition("/")
            if den and float(den) != 0:
                info["fps"] = float(num) / float(den)
            info["duration"] = float((data.get("format") or {}).get("duration") or 0.0)
        except Exception:
            pass
    if not info["width"]:
        try:
            err = subprocess.run([_tool("ffmpeg") or "ffmpeg", "-i", path],
                                 capture_output=True, text=True, timeout=30).stderr
            m = re.search(r"(\d{2,5})x(\d{2,5})", err)
            if m:
                info["width"], info["height"] = int(m.group(1)), int(m.group(2))
            m = re.search(r"([\d.]+) fps", err)
            if m:
                info["fps"] = float(m.group(1))
        except Exception:
            pass
    return info


def play_video(source, args, is_camera=False) -> int:
    ffmpeg = _tool("ffmpeg")
    if not ffmpeg:
        print("ffmpeg not found on PATH - needed for video playback.", file=sys.stderr)
        return 2

    if is_camera:
        vw, vh, src_fps = 640, 480, 30.0
    else:
        info = probe(source)
        vw, vh = info["width"] or 640, info["height"] or 480
        src_fps = info["fps"] or 30.0

    fps = args.fps or min(src_fps, 30.0)
    cols, rows, sw, sh = grid_for(vw, vh, args)
    # rawvideo needs even dimensions for some scalers; keep them as asked but safe
    sw, sh = max(2, sw), max(2, sh)

    cmd = [ffmpeg, "-hide_banner", "-loglevel", "error"]
    if is_camera:
        cmd += ["-f", "dshow", "-i", f"video={source}"] if os.name == "nt" else ["-i", source]
    else:
        if args.start:
            cmd += ["-ss", str(args.start)]
        cmd += ["-i", source]
        if args.duration:
            cmd += ["-t", str(args.duration)]
    if is_camera:
        # The camera's native aspect isn't known without opening it, so fit the
        # picture inside the grid and letterbox the remainder rather than risk
        # stretching a 16:9 webcam into a 4:3 grid.
        scale = (f"scale={sw}:{sh}:force_original_aspect_ratio=decrease:flags=lanczos,"
                 f"pad={sw}:{sh}:(ow-iw)/2:(oh-ih)/2")
    else:
        scale = f"scale={sw}:{sh}:flags=lanczos"
    cmd += ["-vf", f"fps={fps:.4f},{scale}", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"]

    audio = None
    if args.audio and not is_camera and _tool("ffplay"):
        acmd = [_tool("ffplay"), "-nodisp", "-autoexit", "-loglevel", "quiet"]
        if args.start:
            acmd += ["-ss", str(args.start)]
        if args.duration:
            acmd += ["-t", str(args.duration)]
        acmd += ["-i", source]

    nbytes = sw * sh * 3
    term.write(term.ALT_SCREEN_ON + term.HIDE_CURSOR + term.CLEAR)
    proc = None
    frames = dropped = 0
    t0 = time.perf_counter()
    try:
        with term.KeyPoller() as keys:
            while True:  # outer loop = --loop replays
                proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                        bufsize=nbytes * 4)
                if audio is None and args.audio and not is_camera and _tool("ffplay"):
                    try:
                        audio = subprocess.Popen(acmd, stdout=subprocess.DEVNULL,
                                                 stderr=subprocess.DEVNULL)
                    except Exception:
                        audio = None
                buf = bytearray(nbytes)
                view = memoryview(buf)
                start = time.perf_counter()
                n = 0
                paused = False
                while True:
                    got = 0
                    while got < nbytes:
                        chunk = proc.stdout.readinto(view[got:])
                        if not chunk:
                            break
                        got += chunk
                    if got < nbytes:
                        break  # end of stream

                    n += 1
                    frames += 1
                    due = start + n / fps
                    behind = time.perf_counter() - due
                    # If we cannot keep up, skip rendering this frame but keep
                    # reading so audio and wall-clock stay in step.
                    if behind > 1.5 / fps and not args.no_drop:
                        dropped += 1
                        continue

                    arr = np.frombuffer(buf, dtype=np.uint8).reshape(sh, sw, 3)
                    text = frame_to_text(arr, args)
                    if args.status:
                        el = time.perf_counter() - t0
                        text += (f"\r\n\x1b[0m {args.mode} {cols}x{rows}  {fps:.0f}fps target  "
                                 f"{frames / max(el, 1e-6):5.1f}fps actual  {dropped} dropped  "
                                 f"[q]uit [space]pause\x1b[K")
                    term.write(term.HOME + text)

                    while True:
                        k = keys.get()
                        if k in ("q", "Q", "\x1b", "\x03"):
                            return 0
                        if k == " ":
                            paused = not paused
                            if paused:
                                start_pause = time.perf_counter()
                            else:
                                start += time.perf_counter() - start_pause
                        if paused:
                            time.sleep(0.03)
                            continue
                        wait = due - time.perf_counter()
                        if wait <= 0:
                            break
                        time.sleep(min(wait, 0.02))

                proc.stdout.close()
                proc.wait(timeout=5)
                if not args.loop:
                    break
                if audio is not None:
                    audio.terminate()
                    audio = None
    except KeyboardInterrupt:
        pass
    finally:
        for p in (proc, audio):
            if p is not None and p.poll() is None:
                try:
                    p.terminate()
                except Exception:
                    pass
        term.write(term.SHOW_CURSOR + term.RESET + term.ALT_SCREEN_OFF)
        el = time.perf_counter() - t0
        note = ""
        tc, tl = term.size()
        if args.max_cells and cols < tc - 2:
            note = (f"  [grid capped to keep up; raise with --max-cells "
                    f"{args.max_cells * 4} or 0 for no cap]")
        print(f"played {frames} frames in {el:.1f}s "
              f"({frames / max(el, 1e-6):.1f} fps, {dropped} dropped) at {cols}x{rows} cells{note}")
    return 0


def list_cameras() -> list[str]:
    ffmpeg = _tool("ffmpeg")
    if not ffmpeg or os.name != "nt":
        return []
    out = subprocess.run([ffmpeg, "-hide_banner", "-list_devices", "true", "-f", "dshow",
                          "-i", "dummy"], capture_output=True, text=True,
                         errors="replace").stderr
    names, in_video = [], False
    for line in out.splitlines():
        if "Alternative name" in line:
            continue
        # Older ffmpeg builds group devices under section headers; these lines
        # carry no device name themselves, so test them first.
        if "DirectShow video devices" in line:
            in_video = True
            continue
        if "DirectShow audio devices" in line:
            in_video = False
            continue
        m = re.search(r'"([^"]+)"', line)
        if not m:
            continue
        tail = line.rstrip()
        if tail.endswith("(video)"):      # ffmpeg >= 7 tags each device inline
            names.append(m.group(1))
        elif tail.endswith("(audio)"):
            continue
        elif in_video:
            names.append(m.group(1))
    return names


# --------------------------------------------------------------------------- CLI

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="asciiview",
        description="Show images and play videos in the terminal as coloured ASCII/Unicode art.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""examples:
  python asciiview.py photo.jpg                 best-quality half-block render
  python asciiview.py photo.jpg --mode ascii    classic character ramp, in colour
  python asciiview.py photo.jpg --mode ascii --colors none --out art.txt
  python asciiview.py photo.jpg --mode braille  highest detail, dithered
  python asciiview.py clip.mp4 --status         play a video with a stats line
  python asciiview.py clip.mp4 --mode quad --fps 20 --loop
  python asciiview.py anim.gif                  loop an animated GIF
  python asciiview.py camera                    live webcam (Windows/dshow)

keys during playback:  q or Esc = quit, space = pause
""")
    p.add_argument("path", nargs="?", default="", help="image / video / GIF file, or 'camera'")
    p.add_argument("--mode", choices=["ascii", "half", "quad", "braille"], default="ascii",
                   help="rendering style (default: ascii, real characters)")
    p.add_argument("--cols", type=int, help="width in character cells (default: terminal width)")
    p.add_argument("--rows", type=int, help="height in character cells (default: terminal height)")
    p.add_argument("--cell-aspect", type=float, default=None,
                   help="terminal cell height/width (default: ask the terminal, else 2.0). "
                        "Raise it if pictures come out squashed, lower if stretched")
    p.add_argument("--fill", action="store_true", help="stretch to the whole grid, ignoring aspect ratio")
    p.add_argument("--colors", "--colours", dest="colours", default="auto",
                   choices=["auto", "truecolor", "256", "16", "none"],
                   help="colour depth (default: auto-detect)")
    p.add_argument("--mono", action="store_true", help="shorthand for --colors none")
    p.add_argument("--ramp", choices=["measured"] + list(RAMPS), default="measured",
                   help="character set for --mode ascii (default: measured from your font's glyphs)")
    p.add_argument("--chars", help="custom ascii ramp, darkest first, e.g. \" .:-=+*#%%@\"")
    p.add_argument("--match", choices=["shape", "density"], default=None,
                   help="ascii: match glyph shapes to the image (default for stills), "
                        "or pick purely by brightness (default for video: 3x the cells "
                        "for the same effort)")
    p.add_argument("--whiten", type=float, default=0.0,
                   help="0..1 how far saturated colours wash out to match brightness")
    p.add_argument("--edge", type=float, default=1.0,
                   help="0..1 weight on local structure vs flat tone when shape matching")
    p.add_argument("--density", type=float, default=0.7,
                   help="--match density: share of brightness carried by glyph vs colour")
    p.add_argument("--levels", dest="levels", action="store_true", default=None,
                   help="stretch the histogram to full range (default: on for ascii/braille stills)")
    p.add_argument("--no-levels", dest="levels", action="store_false",
                   help="leave the histogram alone")
    p.add_argument("--invert", action="store_true", help="invert brightness (for light terminals)")
    p.add_argument("--threshold", type=float, help="fixed 0..1 cutoff for braille instead of dithering")
    p.add_argument("--gamma", type=float, default=1.0, help="gamma correction (>1 brightens)")
    p.add_argument("--contrast", type=float, default=1.0, help="contrast multiplier")
    p.add_argument("--saturation", type=float, default=1.0, help="colour saturation multiplier")
    p.add_argument("--quantize", type=int, default=None, metavar="BITS",
                   help="drop N low colour bits for speed (default: 0 for images, 2 for video)")
    p.add_argument("--fps", type=float, help="playback frame rate (default: source, capped at 30)")
    p.add_argument("--speed", type=float, default=1.0, help="GIF speed multiplier")
    p.add_argument("--start", type=float, help="seek to this many seconds in")
    p.add_argument("--duration", type=float, help="stop after this many seconds")
    p.add_argument("--loop", action="store_true", help="repeat video/GIF until quit")
    p.add_argument("--still", action="store_true", help="show only the first frame of an animation")
    p.add_argument("--no-audio", dest="audio", action="store_false", help="mute video playback")
    p.add_argument("--no-drop", action="store_true", help="never skip frames, even when behind")
    p.add_argument("--max-cells", type=int, default=None, metavar="N",
                   help="cap the grid when playing video (default 20000; 0 = no cap). "
                        "A huge terminal asks ffmpeg for a huge frame and stalls")
    p.add_argument("--status", action="store_true", help="show a stats line under the video")
    p.add_argument("--info", action="store_true", help="print size info after an image")
    p.add_argument("--out", help="write the rendered art to a file instead of the screen")
    p.add_argument("--list-cameras", action="store_true", help="list webcam device names and exit")
    p.add_argument("--probe", action="store_true",
                   help="report what each cell-size detection method says, and exit")
    p.add_argument("--square", action="store_true",
                   help="print test squares at several cell aspects, and exit")
    return p


def finalize(args, is_video=False):
    """Resolve the 'auto' defaults that depend on mode, output and terminal."""
    if args.mono:
        args.colours = "none"
    if args.colours == "auto":
        args.colours = "truecolor" if term.supports_truecolor() else "256"
    args.ramp_chars = args.chars if args.chars else get_ramp(args.ramp)
    # Writing to a file: no cursor tricks, and keep lines clean.
    args.clear_eol = not args.out
    if args.levels is None:
        # Helps the 1-bit-ish modes a lot, but re-levelling every frame of a
        # video makes the picture pulse, so leave moving images alone.
        args.levels = args.mode in ("ascii", "braille") and not is_video
    if args.quantize is None:
        args.quantize = 2 if is_video else 0
    if args.match is None:
        # Shape matching samples 4x8 pixels per cell and costs ~3x as much to
        # render. On a moving picture more cells beats better glyphs, so video
        # defaults to the cheap path and spends the budget on resolution.
        args.match = "density" if is_video else "shape"
    if args.max_cells is None:
        args.max_cells = 60000 if is_video else 0
    if args.cell_aspect is None:
        # Terminals with extra line spacing have cells taller than the usual
        # 2:1, which stretches the picture vertically unless we measure.
        args.cell_aspect = term.cell_aspect()
    return args


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)

    term.setup_stdout()
    term.enable_ansi()

    if args.probe:
        size = term.console_cell_size()
        aspect = f"  -> aspect {size[1] / size[0]:.3f}" if size else "  (no answer)"
        print(f"  Win32 GetCurrentConsoleFontEx : {size}{aspect}")
        queries = ((CSI + "16t", ESC_RE + r"\[6;(\d+);(\d+)t", "CSI 16t  cell px (h,w)"),
                   (CSI + "14t", ESC_RE + r"\[4;(\d+);(\d+)t", "CSI 14t  window px (h,w)"),
                   (CSI + "18t", ESC_RE + r"\[8;(\d+);(\d+)t", "CSI 18t  window cells (r,c)"))
        for req, pat, what in queries:
            m = term._query(req, pat)
            got = (int(m.group(1)), int(m.group(2))) if m else "(no answer)"
            print(f"  {what:30s}: {got}")
        print(f"  terminal grid (shutil)        : {term.size()}")
        print(f"  --> cell aspect in use        : {term.cell_aspect():.3f}")
        print()
        print("If that aspect is wrong, measure it with:  asciiview.py --square")
        return 0

    if args.square:
        width = 21
        print("Whichever block looks SQUARE gives your --cell-aspect.")
        print()
        for aspect in (1.8, 2.0, 2.2, 2.4, 2.6, 2.8):
            rows = max(1, round(width / aspect))
            print(f"  --cell-aspect {aspect}")
            for _ in range(rows):
                print("      " + FULL_BLOCK * width)
            print()
        return 0

    if args.list_cameras:
        names = list_cameras()
        print("\n".join(names) if names else "no video capture devices found")
        return 0

    is_video = (os.path.splitext(args.path)[1].lower() in VIDEO_EXT
                or args.path.lower() in ("camera", "cam", "webcam"))
    finalize(args, is_video)

    if args.path.lower() in ("camera", "cam", "webcam"):
        names = list_cameras()
        if not names:
            print("no webcam found (needs ffmpeg with dshow on Windows)", file=sys.stderr)
            return 2
        return play_video(names[0], args, is_camera=True)

    if not os.path.exists(args.path):
        print(f"no such file: {args.path}", file=sys.stderr)
        return 2

    ext = os.path.splitext(args.path)[1].lower()
    if ext in VIDEO_EXT:
        return play_video(args.path, args)
    if ext in IMAGE_EXT or ext == ".gif":
        return show_image(args.path, args)

    # Unknown extension: try it as an image, fall back to video.
    try:
        return show_image(args.path, args)
    except Exception:
        return play_video(args.path, args)


if __name__ == "__main__":
    sys.exit(main())
