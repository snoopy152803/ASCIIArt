"""Terminal setup helpers: VT escape support, UTF-8 output, size, key polling."""

from __future__ import annotations

import os
import shutil
import sys
import time

CSI = "\x1b["
HIDE_CURSOR = CSI + "?25l"
SHOW_CURSOR = CSI + "?25h"
HOME = CSI + "H"
RESET = CSI + "0m"
CLEAR = CSI + "2J" + CSI + "3J" + HOME
ALT_SCREEN_ON = CSI + "?1049h"
ALT_SCREEN_OFF = CSI + "?1049l"


def enable_ansi() -> bool:
    """Turn on virtual-terminal processing so escapes work in conhost too."""
    if os.name != "nt":
        return True
    try:
        import ctypes

        k32 = ctypes.windll.kernel32
        ok = False
        for handle_id in (-11, -12):  # stdout, stderr
            h = k32.GetStdHandle(handle_id)
            mode = ctypes.c_uint32()
            if k32.GetConsoleMode(h, ctypes.byref(mode)):
                # 0x0004 = ENABLE_VIRTUAL_TERMINAL_PROCESSING
                if k32.SetConsoleMode(h, mode.value | 0x0004):
                    ok = True
        return ok
    except Exception:
        return False


def setup_stdout() -> None:
    """Make stdout UTF-8 and big-buffered; block glyphs need both."""
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace", newline="")
    except Exception:
        pass
    if os.name == "nt":
        try:
            import ctypes

            ctypes.windll.kernel32.SetConsoleOutputCP(65001)
        except Exception:
            pass


def size(default=(100, 30)) -> tuple[int, int]:
    """(columns, lines) of the terminal, falling back to a sane default."""
    try:
        c, l = shutil.get_terminal_size(default)
    except Exception:
        c, l = default
    return max(10, c), max(5, l)


def _query(request: str, pattern: str, timeout=0.3):
    """Send a terminal report request and parse its reply, or give up quietly.

    Terminals that don't implement the request simply say nothing, so this
    always has to be able to time out rather than block.
    """
    import re

    if not (sys.stdout.isatty() and sys.stdin.isatty()):
        return None

    rx = re.compile(pattern)
    buf = ""
    deadline = time.monotonic() + timeout

    if os.name == "nt":
        try:
            import msvcrt
        except Exception:
            return None
        sys.stdout.write(request)
        sys.stdout.flush()
        while time.monotonic() < deadline:
            if msvcrt.kbhit():
                ch = msvcrt.getwch()
                if ch in ("\x00", "\xe0"):      # function key prefix, discard pair
                    msvcrt.getwch()
                    continue
                buf += ch
                m = rx.search(buf)
                if m:
                    return m
                if len(buf) > 64:
                    return None
            else:
                time.sleep(0.004)
        return None

    try:
        import select
        import termios
        import tty
    except Exception:
        return None
    fd = sys.stdin.fileno()
    saved = termios.tcgetattr(fd)
    try:
        tty.setcbreak(fd)
        sys.stdout.write(request)
        sys.stdout.flush()
        while time.monotonic() < deadline:
            if select.select([sys.stdin], [], [], max(0.0, deadline - time.monotonic()))[0]:
                buf += sys.stdin.read(1)
                m = rx.search(buf)
                if m:
                    return m
                if len(buf) > 64:
                    return None
            else:
                break
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, saved)
    return None


def _plausible(ratio: float) -> bool:
    """A character cell is never squarer than 1:1 nor taller than about 1:4.

    Anything outside that means the reply was not the one we think it was, so
    the default is safer than a number that would wreck the geometry.
    """
    return 1.0 <= ratio <= 4.0


def console_cell_size():
    """(width, height) of a console cell in pixels, via the Win32 console API.

    GetCurrentConsoleFontEx reports the real cell size and is answered by both
    conhost and Windows Terminal, which makes it far more dependable here than
    the CSI escape queries - plenty of terminals simply ignore those, and a
    query that goes unanswered leaves us guessing 2.0.
    """
    if os.name != "nt":
        return None
    try:
        import ctypes
        from ctypes import wintypes

        class COORD(ctypes.Structure):
            _fields_ = [("X", ctypes.c_short), ("Y", ctypes.c_short)]

        class CONSOLE_FONT_INFOEX(ctypes.Structure):
            _fields_ = [("cbSize", wintypes.ULONG),
                        ("nFont", wintypes.DWORD),
                        ("dwFontSize", COORD),
                        ("FontFamily", wintypes.UINT),
                        ("FontWeight", wintypes.UINT),
                        ("FaceName", ctypes.c_wchar * 32)]

        k32 = ctypes.windll.kernel32
        handle = k32.GetStdHandle(-11)                 # STD_OUTPUT_HANDLE
        info = CONSOLE_FONT_INFOEX()
        info.cbSize = ctypes.sizeof(CONSOLE_FONT_INFOEX)
        if not k32.GetCurrentConsoleFontEx(handle, False, ctypes.byref(info)):
            return None
        w, h = int(info.dwFontSize.X), int(info.dwFontSize.Y)
        return (w, h) if w > 0 and h > 0 else None
    except Exception:
        return None


def cell_aspect(default=2.0):
    """Measured height/width of one character cell.

    Assuming the usual 2.0 goes wrong on terminals with extra line spacing:
    the picture comes out stretched vertically, because the grid is sized for
    cells that are shorter than the real ones. Ask the terminal instead.

    Tries CSI 16 t (cell size in pixels), then CSI 14 t with CSI 18 t (window
    pixels and window characters), then falls back to `default`.
    """
    size = console_cell_size()
    if size and _plausible(size[1] / size[0]):
        return size[1] / size[0]

    m = _query("\x1b[16t", r"\x1b\[6;(\d+);(\d+)t")
    if m:
        h, w = int(m.group(1)), int(m.group(2))
        if w > 0 and _plausible(h / w):
            return h / w

    m = _query("\x1b[14t", r"\x1b\[4;(\d+);(\d+)t")
    if not m:
        return default
    px_h, px_w = int(m.group(1)), int(m.group(2))
    m = _query("\x1b[18t", r"\x1b\[8;(\d+);(\d+)t")
    if not m:
        return default
    ch_rows, ch_cols = int(m.group(1)), int(m.group(2))
    if min(px_h, px_w, ch_rows, ch_cols) <= 0:
        return default
    ratio = (px_h / ch_rows) / (px_w / ch_cols)
    return ratio if _plausible(ratio) else default


def supports_truecolor() -> bool:
    ct = os.environ.get("COLORTERM", "").lower()
    if "truecolor" in ct or "24bit" in ct:
        return True
    if os.environ.get("WT_SESSION") or os.environ.get("TERM_PROGRAM"):
        return True
    return os.name == "nt"  # Windows Terminal / modern conhost both do 24-bit


def write(s: str) -> None:
    sys.stdout.write(s)
    sys.stdout.flush()


class KeyPoller:
    """Non-blocking single-key reads, used for pause/quit during playback."""

    def __init__(self):
        self._msvcrt = None
        self._posix = None
        if os.name == "nt":
            try:
                import msvcrt

                self._msvcrt = msvcrt
            except Exception:
                pass
        elif sys.stdin.isatty():
            try:
                import termios
                import tty

                self._posix = (termios, tty, sys.stdin.fileno())
            except Exception:
                pass

    def __enter__(self):
        if self._posix:
            termios, tty, fd = self._posix
            self._saved = termios.tcgetattr(fd)
            tty.setcbreak(fd)
        return self

    def __exit__(self, *exc):
        if self._posix and getattr(self, "_saved", None):
            termios, _tty, fd = self._posix
            termios.tcsetattr(fd, termios.TCSADRAIN, self._saved)
        return False

    def get(self) -> str | None:
        if self._msvcrt is not None:
            if self._msvcrt.kbhit():
                ch = self._msvcrt.getwch()
                if ch in ("\x00", "\xe0"):  # function/arrow prefix
                    self._msvcrt.getwch()
                    return None
                return ch
            return None
        if self._posix:
            import select

            if select.select([sys.stdin], [], [], 0)[0]:
                return sys.stdin.read(1)
        return None
