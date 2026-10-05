/* asciiapp - ASCII art viewer / player as a native Windows application.
 *
 *   gcc -O2 -mwindows -o asciiapp.exe asciiapp.c -lgdi32 -lcomdlg32
 *
 * Why this exists rather than just the terminal version:
 *
 *  - The cell shape is *chosen*, not guessed. We create the font with an
 *    explicit cell width and height, so a circle is round by construction.
 *    In a terminal the cell aspect has to be detected, and plenty of
 *    terminals refuse to report it.
 *  - Zoom goes well past any terminal's smallest font. Cells run from 4 px
 *    to 48 px tall here, so you can keep zooming out long after a terminal
 *    has run out of font sizes.
 *  - No escape-sequence bandwidth limit, so video is smoother.
 *
 * Media is decoded by ffmpeg (must be on PATH), read as raw RGB over a pipe.
 *
 * Keys:  + -  zoom (or wheel)        M  mode: ascii / braille / blocks
 *        F    fullscreen             L  auto-levels on/off
 *        Space pause                 ,  .  seek -/+ 5s
 *        Ctrl+O or O  open file      S  save a .bmp
 *        H or ?  key list            Q/Esc  quit
 *        G    guess-the-picture      left/right  prev/next picture
 *        C    mono        I  invert  [ ]  midtone brightness
 */

#include <windows.h>
#include <commdlg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ config */

#define GW 4                 /* sub-samples per cell, across */
#define GH 8                 /* sub-samples per cell, down   */
#define NSUB (GW * GH)
#define FIRST_CH 32
#define LAST_CH 126
#define NGLYPH (LAST_CH - FIRST_CH + 1)

#define CELL_MIN 2        /* below ~4 this is really a pixel renderer */
#define CELL_MAX 600      /* big enough for a handful of giant letters */
#define SHAPE_MIN_CELL 10    /* below this, glyph shape is invisible: use tone */
#define DECODE_MAX 1280      /* cap the decode resolution; we rescale ourselves */

#define MODE_ASCII 0
#define MODE_LINES 1
#define MODE_BRAILLE 2
#define MODE_BLOCK 3
#define MODE_COUNT 4

/* Braille: 2x4 dots per cell, so 256 patterns. The terminal viewer's `half`
   and `quad` modes are not offered here - those exist only to work around a
   terminal cell having one foreground and one background colour, a limit
   this app does not have. `block` is the equivalent and is strictly better. */
#define BRW 2
#define BRH 4
#define NBRAILLE 256

static const wchar_t *FONT_NAME = L"Consolas";

#define LIN_STEPS 4096
static float g_to_lin[256];
static unsigned char g_to_srgb[LIN_STEPS + 1];

#define DENSITY 0.75f

/* The densest ASCII glyph inks only about a third of its cell, so on a black
   background the brightest thing this medium can emit is ~0.35 of full light
   (sRGB 160). Mapping the source's light straight onto that ceiling is
   faithful but leaves the picture looking a stop and a half under-exposed,
   because most of a normal image then sits far down the curve. A display
   gamma below 1 lifts the midtones into the range that is actually
   available, which is the same thing you would do grading a photo for a
   low-contrast medium. 1.0 is the physically linear mapping. */
#define OUT_GAMMA_DEFAULT 0.62f

/* Named so there is one place to change them, and so a default can never go
   missing from the initialiser without the compiler noticing the symbol. */
#define AV_OFFSET_DEFAULT 0.50     /* seconds ffplay lags the picture by */
#define LINE_THRESH_DEFAULT 0.08f  /* edge energy a cell needs before it inks */

static float g_cover_of[256];     /* sRGB tone -> wanted ink coverage */
static float g_rel_of[256];       /* sRGB tone -> relative linear light */

/* ------------------------------------------------------------------- state */

typedef struct {
    /* glyph bank, for matching: coverage of each glyph on a GW x GH grid */
    float bank[NGLYPH][NSUB];
    float gm[NGLYPH];        /* mean coverage                  */
    float gnorm[NGLYPH];     /* 1 / ||g - mean||               */
    float gmn[NGLYPH];       /* NSUB * mean * gnorm            */
    float gm2c[NGLYPH];      /* mean^2 / cmax^2                */
    float cmax;              /* densest glyph's mean coverage  */
    float ramp_step;         /* one resolvable step of the tone ramp */
    int ramp[256];           /* tone -> glyph index, for the fast path */

    /* glyph atlas, for drawing: coverage bytes at the current cell size */
    unsigned char *atlas;    /* NGLYPH * cw * ch */
    int atlas_cw, atlas_ch;

    /* braille atlas, drawn geometrically (Consolas has no braille glyphs) */
    unsigned char *br_atlas; /* NBRAILLE * cw * ch */
    float br_cover[NBRAILLE];
    float br_cmax;
    int br_cw, br_ch;

    /* back buffer */
    HBITMAP dib;
    unsigned int *pixels;    /* 0x00RRGGBB */
    int dib_w, dib_h;

    /* source frame from ffmpeg */
    unsigned char *frame;    /* RGB24, dec_w x dec_h */
    int dec_w, dec_h, frame_w, frame_h;
    int have_frame;

    /* resampling scratch */
    float *lum;              /* (cols*GW) x (rows*GH) luminance 0..1 */
    float *lumb;             /* the same, blurred, for edge detection */
    unsigned char *cellrgb;  /* cols x rows x 3, mean colour per cell */
    int scratch_cells, scratch_sub;

    /* media */
    HANDLE pipe, proc, audio_proc;
    int audio_on, has_audio, have_ffplay;
    wchar_t path[MAX_PATH];
    int is_video, playing, fps_num;
    double duration, position, play_origin;

    int cell_h, mode, levels, show_help;
    int game, game_cols, game_reveal;   /* 'guess the picture' */
    int mono, invert;
    int nthreads;
    int slideshow;
    double av_offset;                   /* audio lead, seconds */
    float line_thresh;                  /* line art: ink this much edge */
    double seek_to;                     /* pending debounced seek */
    int seek_pending;
    wchar_t probed[MAX_PATH];           /* ffprobe cache key */
    int pr_w, pr_h, pr_audio;
    double pr_fps, pr_dur;
    int bar_y0, bar_y1;                 /* video scrub bar, for hit testing */
    LARGE_INTEGER qpf, play_t0;         /* wall clock for video pacing */
    long long frames_read, frames_dropped;
    float whiten, out_gamma;
    int cols, rows;
    HWND hwnd;
    wchar_t status[256];
} App;

static App g;

/* ------------------------------------------------------------- small utils */

static int imax(int a, int b) { return a > b ? a : b; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* Tell Windows we handle display scaling ourselves.
 *
 * Without this, on a display set above 100% scaling Windows hands the app a
 * virtual, smaller client area and bitmap-stretches whatever it draws. Three
 * things follow: the art is blurry, there are fewer real pixels to put cells
 * in (so the picture resolves less than a DPI-aware terminal on the same
 * screen), and the window's true edges sit where the stretched image is not,
 * so the mouse misses the close button at the very corner. */
static void enable_dpi_awareness(void)
{
    typedef BOOL(WINAPI * fn_ctx)(HANDLE);
    typedef HRESULT(WINAPI * fn_awareness)(int);
    typedef BOOL(WINAPI * fn_old)(void);

    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        fn_ctx f = (fn_ctx)(void *)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        /* -4 is DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 (Win10 1703+) */
        if (f && f((HANDLE)-4)) return;
    }
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        fn_awareness f = (fn_awareness)(void *)GetProcAddress(shcore, "SetProcessDpiAwareness");
        if (f && f(2) == S_OK) return;              /* per-monitor aware */
    }
    if (u32) {
        fn_old f = (fn_old)(void *)GetProcAddress(u32, "SetProcessDPIAware");
        if (f) f();                                 /* system aware, Vista+ */
    }
}

/* Zoom in constant *ratio* steps. A fixed step in pixels feels fine around
   8 px and is unusable at 300: the cell count is window/cell, so the effect
   of +2 px shrinks as the cell grows, and halving the grid went from 2 key
   presses to 150. A ratio keeps every press worth the same. */
static void zoom(int dir)
{
    int before = g.cell_h;
    int next = dir > 0 ? (int)(before * 1.25f + 0.5f) : (int)(before / 1.25f + 0.5f);
    if (next == before) next = before + dir;      /* always move at the small end */
    g.cell_h = clampi(next, CELL_MIN, CELL_MAX);
}

static void die(const wchar_t *msg)
{
    MessageBoxW(NULL, msg, L"asciiapp", MB_ICONERROR | MB_OK);
    ExitProcess(1);
}

static void auto_levels_frame(void);
static int pump_video(void);
static int open_media(const wchar_t *path, double start_at);
static void adopt_child(HANDLE proc);

/* Run a function over a range of rows, split across cores. Declared early so
   the resample can use it; defined down with the renderer. */
typedef void (*BandFn)(void *ctx, int y0, int y1);
static void parallel_rows(BandFn fn, void *ctx, int y0, int y1);
/* A 5-tap binomial blur of the luminance, run before the gradient in line
   art mode.
   Differentiating raw pixels turns every bit of sensor noise - and every
   one-level step in a banded gradient - into an "edge", which buries the
   real contours under a field of dashes. Smoothing first is the standard
   first step of edge detection for exactly this reason. */
typedef struct { int w, h; } BlurCtx;

static void blur_band(void *vctx, int y0, int y1)
{
    const BlurCtx *c = (const BlurCtx *)vctx;
    int w = c->w, h = c->h;
    for (int y = y0; y < y1; y++) {
        for (int x = 0; x < w; x++) {
            int xm2 = x > 1 ? x - 2 : 0, xm1 = x > 0 ? x - 1 : 0;
            int xp1 = x + 1 < w ? x + 1 : w - 1, xp2 = x + 2 < w ? x + 2 : w - 1;
            int ym2 = y > 1 ? y - 2 : 0, ym1 = y > 0 ? y - 1 : 0;
            int yp1 = y + 1 < h ? y + 1 : h - 1, yp2 = y + 2 < h ? y + 2 : h - 1;
            const float *L = g.lum;
#define LX(a, b) L[(size_t)(b) * w + (a)]
            float hsum = LX(xm2, y) + 4 * LX(xm1, y) + 6 * LX(x, y)
                       + 4 * LX(xp1, y) + LX(xp2, y);
            float vsum = LX(x, ym2) + 4 * LX(x, ym1) + 6 * LX(x, y)
                       + 4 * LX(x, yp1) + LX(x, yp2);
#undef LX
            g.lumb[(size_t)y * w + x] = (hsum + vsum) / 32.0f;
        }
    }
}

static void clear_band(void *ctx, int y0, int y1);

/* ------------------------------------------------------------ ffmpeg pipes */

/* Start a child process with its stdout on a pipe and no console window.
   _popen would pop up a console in a -mwindows build, so do it by hand. */
static int spawn(const wchar_t *cmdline, HANDLE *out_read, HANDLE *out_proc,
                 DWORD pipe_bytes)
{
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    /* The buffer wants to hold a good many frames. With only one frame's
       worth, ffmpeg blocks after every frame and the reader is gated by its
       decode speed - which makes skipping a frame cost as much as drawing
       one, and defeats the whole point of dropping frames to keep time. */
    if (pipe_bytes < (1u << 20)) pipe_bytes = 1u << 20;
    if (!CreatePipe(&rd, &wr, &sa, pipe_bytes)) return 0;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = NULL;

    wchar_t *mutable_cmd = _wcsdup(cmdline);
    BOOL ok = CreateProcessW(NULL, mutable_cmd, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    free(mutable_cmd);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); return 0; }
    CloseHandle(pi.hThread);
    adopt_child(pi.hProcess);
    *out_read = rd;
    *out_proc = pi.hProcess;
    return 1;
}

static int read_exact(HANDLE h, unsigned char *buf, size_t want)
{
    size_t got = 0;
    while (got < want) {
        DWORD n = 0;
        if (!ReadFile(h, buf + got, (DWORD)(want - got), &n, NULL) || n == 0) return 0;
        got += n;
    }
    return 1;
}

/* Sound is a second ffplay process rather than anything decoded in here.
 *
 * ffplay has no way to be paused or seeked from outside, so pause and seek
 * stop it and start a fresh one at the new offset. Both it and the video
 * ffmpeg run off the wall clock (-re on the video side), so they stay in
 * step without a shared timeline. */
/* Put every child in a job that dies with this process.
 *
 * ffplay outlives us otherwise: closing the video pipe is enough to stop
 * ffmpeg, but nothing links ffplay's lifetime to ours, so a crash or a
 * force-kill left sound playing with no window to stop it. */
static HANDLE g_job;

static void init_child_job(void)
{
    g_job = CreateJobObjectW(NULL, NULL);
    if (!g_job) return;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
    ZeroMemory(&li, sizeof li);
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(g_job, JobObjectExtendedLimitInformation,
                                 &li, sizeof li)) {
        CloseHandle(g_job);
        g_job = NULL;
    }
}

static void adopt_child(HANDLE proc)
{
    if (g_job && proc) AssignProcessToJobObject(g_job, proc);
}

static void stop_audio(void)
{
    if (g.audio_proc) {
        TerminateProcess(g.audio_proc, 0);
        CloseHandle(g.audio_proc);
        g.audio_proc = NULL;
    }
}

static void start_audio(double at)
{
    stop_audio();
    if (!g.audio_on || !g.has_audio || !g.is_video || !g.path[0]) return;

    /* ffplay starts a beat after the video no matter how closely the two
       are launched, so skip it forward by a fixed lead. Tunable, because the
       right value depends on the machine's audio latency. */
    double a = at + g.av_offset;
    if (a < 0) a = 0;
    if (g.duration > 0 && a >= g.duration) return;

    wchar_t cmd[MAX_PATH + 256], seek[64] = L"";
    if (a > 0.05) _snwprintf(seek, 63, L"-ss %.3f ", a);
    _snwprintf(cmd, MAX_PATH + 200,
               L"ffplay -nodisp -autoexit -loglevel quiet %s-i \"%s\"", seek, g.path);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    wchar_t *mutable_cmd = _wcsdup(cmd);
    if (CreateProcessW(NULL, mutable_cmd, NULL, NULL, FALSE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        g.audio_proc = pi.hProcess;
        adopt_child(g.audio_proc);
    }
    free(mutable_cmd);
}

static void close_media(void)
{
    stop_audio();
    if (g.pipe) { CloseHandle(g.pipe); g.pipe = NULL; }
    if (g.proc) {
        TerminateProcess(g.proc, 0);
        CloseHandle(g.proc);
        g.proc = NULL;
    }
}

/* Ask ffprobe for the stream's size, rate and duration. */
static void probe_media(const wchar_t *path, int *w, int *h, double *fps, double *dur)
{
    *w = 0; *h = 0; *fps = 0; *dur = 0;

    wchar_t cmd[MAX_PATH + 256];
    _snwprintf(cmd, 1024,
               L"ffprobe -v error -select_streams v:0 -show_entries "
               L"stream=width,height,avg_frame_rate:format=duration "
               L"-of default=nw=1 \"%s\"", path);

    HANDLE rd, proc;
    if (!spawn(cmd, &rd, &proc, 1 << 16)) return;

    char buf[4096];
    DWORD total = 0, n = 0;
    while (total < sizeof buf - 1 &&
           ReadFile(rd, buf + total, (DWORD)(sizeof buf - 1 - total), &n, NULL) && n)
        total += n;
    buf[total] = 0;
    CloseHandle(rd);
    WaitForSingleObject(proc, 3000);
    CloseHandle(proc);

    char *p;
    if ((p = strstr(buf, "width="))) *w = atoi(p + 6);
    if ((p = strstr(buf, "height="))) *h = atoi(p + 7);
    if ((p = strstr(buf, "avg_frame_rate="))) {
        int a = 0, b = 0;
        if (sscanf(p + 15, "%d/%d", &a, &b) == 2 && b) *fps = (double)a / b;
    }
    if ((p = strstr(buf, "duration="))) *dur = atof(p + 9);
}

/* Is there an audio stream at all? Without this a silent clip would get a
   pointless ffplay spawned at it on every seek, and the title bar could not
   tell "muted" apart from "nothing to play". */
static int probe_has_audio(const wchar_t *path)
{
    wchar_t cmd[MAX_PATH + 256];
    _snwprintf(cmd, MAX_PATH + 200,
               L"ffprobe -v error -select_streams a:0 -show_entries stream=codec_type "
               L"-of default=nw=1 \"%s\"", path);
    HANDLE rd, proc;
    if (!spawn(cmd, &rd, &proc, 1 << 16)) return 0;

    char buf[512];
    DWORD total = 0, n = 0;
    while (total < sizeof buf - 1 &&
           ReadFile(rd, buf + total, (DWORD)(sizeof buf - 1 - total), &n, NULL) && n)
        total += n;
    buf[total] = 0;
    CloseHandle(rd);
    WaitForSingleObject(proc, 3000);
    CloseHandle(proc);
    return strstr(buf, "audio") != NULL;
}

static int looks_like_video(const wchar_t *path)
{
    static const wchar_t *ext[] = { L".mp4", L".mkv", L".mov", L".avi", L".webm",
                                    L".m4v", L".wmv", L".flv", L".mpg", L".mpeg",
                                    L".ts", L".gif", L".ogv", NULL };
    const wchar_t *dot = wcsrchr(path, L'.');
    if (!dot) return 0;
    for (int i = 0; ext[i]; i++)
        if (!_wcsicmp(dot, ext[i])) return 1;
    return 0;
}

/* Open a file and start decoding into g.frame at g.dec_w x g.dec_h. */
static int open_media_ex(const wchar_t *path, double start_at, int wait_first)
{
    close_media();
    g.have_frame = 0;

    /* ffprobe costs a process spawn and can block for a while. Seeking
       re-opens the same file over and over, so remember the answer - this is
       most of why ',' and '.' felt unresponsive. */
    int sw = 0, sh = 0;
    double fps = 0, dur = 0;
    if (g.probed[0] && _wcsicmp(g.probed, path) == 0) {
        sw = g.pr_w; sh = g.pr_h; fps = g.pr_fps; dur = g.pr_dur;
        g.has_audio = g.pr_audio;
    } else {
        probe_media(path, &sw, &sh, &fps, &dur);
        g.has_audio = looks_like_video(path) && g.have_ffplay && probe_has_audio(path);
        wcsncpy(g.probed, path, MAX_PATH - 1);
        g.probed[MAX_PATH - 1] = 0;
        g.pr_w = sw; g.pr_h = sh; g.pr_fps = fps; g.pr_dur = dur;
        g.pr_audio = g.has_audio;
    }
    if (sw <= 0 || sh <= 0) { sw = 640; sh = 480; }
    if (fps <= 0 || fps > 240) fps = 25;

    /* Decode no larger than DECODE_MAX; we resample to the grid ourselves,
       so there is no need to restart ffmpeg when the window changes. */
    double shrink = 1.0;
    if (sw > DECODE_MAX || sh > DECODE_MAX)
        shrink = (double)DECODE_MAX / (sw > sh ? sw : sh);
    int dw = ((int)(sw * shrink)) & ~1;
    int dh = ((int)(sh * shrink)) & ~1;
    if (dw < 2) dw = 2;
    if (dh < 2) dh = 2;

    g.is_video = looks_like_video(path);
    g.dec_w = dw;
    g.dec_h = dh;
    g.duration = dur;
    g.position = start_at;
    g.play_origin = start_at;
    g.fps_num = (int)(fps + 0.5);

    /* Keep the existing buffer when the size is unchanged: a seek can then
       carry on showing the current frame instead of a flash of garbage. */
    if (!g.frame || dw != g.frame_w || dh != g.frame_h) {
        free(g.frame);
        g.frame = (unsigned char *)malloc((size_t)dw * dh * 3);
        if (!g.frame) return 0;
        memset(g.frame, 0, (size_t)dw * dh * 3);
        g.frame_w = dw;
        g.frame_h = dh;
        wait_first = 1;                 /* nothing to show yet, so we must */
    }

    wchar_t cmd[MAX_PATH + 512];
    if (g.is_video) {
        /* The seek has to be built separately: a %.3f left in the format
           string still prints "0.000" when there is no -ss in front of it,
           and ffmpeg then reads that as an output filename. */
        wchar_t seek[64] = L"";
        if (start_at > 0) _snwprintf(seek, 63, L"-ss %.3f ", start_at);
        /* -re makes ffmpeg emit at the clip's own frame rate, so the pipe
           paces playback and we can simply drop whatever we cannot draw. */
        _snwprintf(cmd, 1200,
                   L"ffmpeg -hide_banner -loglevel error -re %s-i \"%s\" "
                   L"-vf scale=%d:%d:flags=bilinear -f rawvideo -pix_fmt rgb24 -",
                   seek, path, dw, dh);
    } else {
        _snwprintf(cmd, 1200,
                   L"ffmpeg -hide_banner -loglevel error -i \"%s\" -frames:v 1 "
                   L"-vf scale=%d:%d:flags=lanczos -f rawvideo -pix_fmt rgb24 -",
                   path, dw, dh);
    }
    DWORD want = (DWORD)((size_t)dw * dh * 3 * 24);
    if (want > (48u << 20)) want = 48u << 20;
    if (!spawn(cmd, &g.pipe, &g.proc, want)) return 0;

    /* Sound starts here, not after the first frame: reading that frame
       blocks while ffmpeg (already running under -re) gets going, so
       starting ffplay afterwards would put the audio a beat behind. Doing
       both now lets their startup costs overlap. start_audio reads g.path,
       so set it first - and put it back if the decode turns out to fail. */
    wchar_t previous[MAX_PATH];
    wcscpy(previous, g.path);
    wcsncpy(g.path, path, MAX_PATH - 1);
    g.path[MAX_PATH - 1] = 0;
    if (g.is_video) start_audio(start_at);

    if (wait_first) {
        if (!read_exact(g.pipe, g.frame, (size_t)dw * dh * 3)) {
            close_media();
            wcscpy(g.path, previous);
            return 0;
        }
        g.have_frame = 1;
    }
    if (!g.is_video && g.levels) auto_levels_frame();

    if (g.is_video) {
        g.playing = 1;
        QueryPerformanceFrequency(&g.qpf);
        QueryPerformanceCounter(&g.play_t0);
        g.frames_read = 0;
        if (g.hwnd) SetTimer(g.hwnd, 1, (UINT)(500.0 / fps), NULL);
    } else {
        if (g.hwnd) KillTimer(g.hwnd, 1);
        g.playing = 0;
        close_media();          /* single frame: nothing more to read */
    }
    return 1;
}

/* -------------------------------------------------------------- glyph bank */

/* Rasterise one character into an 8-bit coverage map of size cw x ch. */
static void raster_glyph(HDC memdc, HBITMAP bmp, unsigned int *bits,
                         int cw, int ch, wchar_t c, unsigned char *out)
{
    RECT r = { 0, 0, cw, ch };
    FillRect(memdc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SetTextColor(memdc, RGB(255, 255, 255));
    SetBkMode(memdc, TRANSPARENT);
    TextOutW(memdc, 0, 0, &c, 1);
    GdiFlush();
    for (int i = 0; i < cw * ch; i++)
        out[i] = (unsigned char)(bits[i] & 0xff);     /* white on black */
    (void)bmp;
}

/* Build the matching bank once, at a generous size, then box-filter to GWxGH. */
/* Tone -> relative linear light, and -> the coverage that should carry it.
   Separate from build_bank so the gamma can be changed without re-rastering
   every glyph. */
static void build_tone_tables(void)
{
    for (int t = 0; t < 256; t++) {
        g_rel_of[t] = powf(g_to_lin[t], g.out_gamma);
        g_cover_of[t] = g.cmax * powf(g_rel_of[t], DENSITY);
    }
}

static void build_bank(void)
{
    const int cw = 16, ch = 32;
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = cw;
    bi.bmiHeader.biHeight = -ch;           /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC dc = CreateCompatibleDC(NULL);
    unsigned int *bits = NULL;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    HGDIOBJ oldbmp = SelectObject(dc, bmp);

    HFONT font = CreateFontW(-ch * 7 / 8, cw, 0, 0, FW_NORMAL, 0, 0, 0,
                             ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, FONT_NAME);
    HGDIOBJ oldfont = SelectObject(dc, font);

    unsigned char *cell = (unsigned char *)malloc(cw * ch);
    float cmax = 0;
    for (int gi = 0; gi < NGLYPH; gi++) {
        raster_glyph(dc, bmp, bits, cw, ch, (wchar_t)(FIRST_CH + gi), cell);
        /* box-downsample cw x ch -> GW x GH */
        for (int y = 0; y < GH; y++) {
            for (int x = 0; x < GW; x++) {
                int x0 = x * cw / GW, x1 = (x + 1) * cw / GW;
                int y0 = y * ch / GH, y1 = (y + 1) * ch / GH;
                int sum = 0, n = 0;
                for (int yy = y0; yy < y1; yy++)
                    for (int xx = x0; xx < x1; xx++) { sum += cell[yy * cw + xx]; n++; }
                g.bank[gi][y * GW + x] = n ? (float)sum / (255.0f * n) : 0.0f;
            }
        }
        float mean = 0;
        for (int i = 0; i < NSUB; i++) mean += g.bank[gi][i];
        mean /= NSUB;
        g.gm[gi] = mean;
        if (mean > cmax) cmax = mean;
    }
    g.cmax = cmax > 0 ? cmax : 1.0f;

    for (int gi = 0; gi < NGLYPH; gi++) {
        float ss = 0;
        for (int i = 0; i < NSUB; i++) ss += g.bank[gi][i] * g.bank[gi][i];
        float var = ss - NSUB * g.gm[gi] * g.gm[gi];
        if (var < 0) var = 0;
        g.gnorm[gi] = 1.0f / (sqrtf(var) + 1e-6f);
        g.gmn[gi] = NSUB * g.gm[gi] * g.gnorm[gi];
        g.gm2c[gi] = g.gm[gi] * g.gm[gi] / (g.cmax * g.cmax);
    }

    /* Tone ramp for the fast path: for each brightness, the glyph whose mean
       coverage is closest. Same idea as the measured ramp in ramp.py. */
    for (int t = 0; t < 256; t++) {
        float want = (t / 255.0f) * g.cmax;
        int best = 0;
        float bd = 1e9f;
        for (int gi = 0; gi < NGLYPH; gi++) {
            float d = fabsf(g.gm[gi] - want);
            if (d < bd) { bd = d; best = gi; }
        }
        g.ramp[t] = best;
    }

    /* Glyph coverages cluster, so the ramp resolves far fewer distinct levels
       than there are glyphs - which shows up as contour banding on smooth
       gradients. Count what it really resolves, so the dither can be scaled
       to exactly one step. */
    int distinct = 1;
    for (int t = 1; t < 256; t++)
        if (g.ramp[t] != g.ramp[t - 1]) distinct++;
    g.ramp_step = 1.0f / (float)(distinct > 1 ? distinct : 1);

    build_tone_tables();

    free(cell);
    SelectObject(dc, oldfont);
    DeleteObject(font);
    SelectObject(dc, oldbmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

/* Rasterise every glyph at the current cell size, for drawing.
 *
 * The glyphs are drawn supersampled and then box-filtered down. Asking GDI
 * for a 3 px font and hoping for antialiasing does not work - it hands back
 * hard on/off pixels, which at small cell sizes turns the picture into harsh
 * stripes and throws away every grey level the art depends on. Rendering big
 * and averaging gives exact coverage at any size, and it is what lets a 2x4
 * cell still carry a usable tone.
 */
static void build_atlas(int cw, int ch)
{
    if (g.atlas && g.atlas_cw == cw && g.atlas_ch == ch) return;
    free(g.atlas);
    g.atlas = (unsigned char *)malloc((size_t)NGLYPH * cw * ch);
    g.atlas_cw = cw;
    g.atlas_ch = ch;

    /* Supersample enough that the glyph is drawn at a size GDI renders well. */
    int ss = 1;
    while (ch * ss < 32 && ss < 8) ss++;
    if ((long)cw * ss * ch * ss > 4000000L) ss = 1;   /* huge cells: no need */
    int sw = cw * ss, sh = ch * ss;

    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = sw;
    bi.bmiHeader.biHeight = -sh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC dc = CreateCompatibleDC(NULL);
    unsigned int *bits = NULL;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    HGDIOBJ oldbmp = SelectObject(dc, bmp);

    /* Giving CreateFontW both a width and a height is what pins the cell
       shape exactly, which is the whole reason a circle stays round here. */
    HFONT font = CreateFontW(-sh * 7 / 8, sw, 0, 0, FW_NORMAL, 0, 0, 0,
                             ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, FONT_NAME);
    HGDIOBJ oldfont = SelectObject(dc, font);

    unsigned char *big = (unsigned char *)malloc((size_t)sw * sh);
    for (int gi = 0; gi < NGLYPH; gi++) {
        raster_glyph(dc, bmp, bits, sw, sh, (wchar_t)(FIRST_CH + gi), big);
        unsigned char *out = g.atlas + (size_t)gi * cw * ch;
        if (ss == 1) {
            memcpy(out, big, (size_t)cw * ch);
            continue;
        }
        for (int y = 0; y < ch; y++) {
            for (int x = 0; x < cw; x++) {
                unsigned int sum = 0;
                for (int yy = 0; yy < ss; yy++) {
                    const unsigned char *r = big + (size_t)(y * ss + yy) * sw + x * ss;
                    for (int xx = 0; xx < ss; xx++) sum += r[xx];
                }
                out[y * cw + x] = (unsigned char)(sum / (ss * ss));
            }
        }
    }
    free(big);

    SelectObject(dc, oldfont);
    DeleteObject(font);
    SelectObject(dc, oldbmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

/* Braille dots, drawn rather than rendered from a font: Consolas has no
   braille glyphs, and drawing them means they stay crisp at any cell size.
   Supersampled 4x for smooth edges, same as the text atlas. */
static void build_braille_atlas(int cw, int ch)
{
    if (g.br_atlas && g.br_cw == cw && g.br_ch == ch) return;
    free(g.br_atlas);
    g.br_atlas = (unsigned char *)malloc((size_t)NBRAILLE * cw * ch);
    g.br_cw = cw;
    g.br_ch = ch;

    const int ss = 4;
    float dw = (float)cw / BRW, dh = (float)ch / BRH;
    float r = 0.42f * (dw < dh ? dw : dh);
    float r2 = r * r;

    g.br_cmax = 0;
    for (int pat = 0; pat < NBRAILLE; pat++) {
        unsigned char *out = g.br_atlas + (size_t)pat * cw * ch;
        long total = 0;
        for (int y = 0; y < ch; y++) {
            for (int x = 0; x < cw; x++) {
                int hits = 0;
                for (int sy = 0; sy < ss; sy++) {
                    float py = y + (sy + 0.5f) / ss;
                    for (int sx = 0; sx < ss; sx++) {
                        float px = x + (sx + 0.5f) / ss;
                        for (int d = 0; d < 8; d++) {
                            if (!(pat & (1 << d))) continue;
                            float cx = (d / BRH + 0.5f) * dw;    /* dot column */
                            float cy = (d % BRH + 0.5f) * dh;    /* dot row    */
                            float ex = px - cx, ey = py - cy;
                            if (ex * ex + ey * ey <= r2) { hits++; break; }
                        }
                    }
                }
                unsigned char v = (unsigned char)(255 * hits / (ss * ss));
                out[y * cw + x] = v;
                total += v;
            }
        }
        g.br_cover[pat] = (float)total / (255.0f * cw * ch);
        if (g.br_cover[pat] > g.br_cmax) g.br_cmax = g.br_cover[pat];
    }
    if (g.br_cmax <= 0) g.br_cmax = 1.0f;
}

/* ---------------------------------------------------------------- resample */

/* Area-average the decoded frame down to dw x dh, writing luminance and,
   optionally, the mean colour of each cell. */
typedef struct { int sub_w, sub_h, cols, rows; } ResampleCtx;

static void resample_lum_band(void *vctx, int yy0, int yy1)
{
    const ResampleCtx *c = (const ResampleCtx *)vctx;
    int sub_w = c->sub_w, sub_h = c->sub_h;
    const unsigned char *src = g.frame;
    int sw = g.dec_w, sh = g.dec_h;

    for (int y = yy0; y < yy1; y++) {
        int y0 = y * sh / sub_h, y1 = (y + 1) * sh / sub_h;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < sub_w; x++) {
            int x0 = x * sw / sub_w, x1 = (x + 1) * sw / sub_w;
            if (x1 <= x0) x1 = x0 + 1;
            unsigned int r = 0, gg = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; yy++) {
                const unsigned char *row = src + (size_t)yy * sw * 3;
                for (int xx = x0; xx < x1; xx++) {
                    r += row[xx * 3]; gg += row[xx * 3 + 1]; b += row[xx * 3 + 2];
                    n++;
                }
            }
            float inv = 1.0f / (n ? n : 1);
            float L = (0.2126f * r + 0.7152f * gg + 0.0722f * b) * inv / 255.0f;
            g.lum[(size_t)y * sub_w + x] = g.invert ? 1.0f - L : L;
        }
    }
}

/* mean colour per cell, from the same decoded frame */
static void resample_rgb_band(void *vctx, int yy0, int yy1)
{
    const ResampleCtx *c = (const ResampleCtx *)vctx;
    int cols = c->cols, rows = c->rows;
    const unsigned char *src = g.frame;
    int sw = g.dec_w, sh = g.dec_h;

    for (int y = yy0; y < yy1; y++) {
        int y0 = y * sh / rows, y1 = (y + 1) * sh / rows;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < cols; x++) {
            int x0 = x * sw / cols, x1 = (x + 1) * sw / cols;
            if (x1 <= x0) x1 = x0 + 1;
            unsigned int r = 0, gg = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; yy++) {
                const unsigned char *row = src + (size_t)yy * sw * 3;
                for (int xx = x0; xx < x1; xx++) {
                    r += row[xx * 3]; gg += row[xx * 3 + 1]; b += row[xx * 3 + 2];
                    n++;
                }
            }
            unsigned char *o = g.cellrgb + ((size_t)y * cols + x) * 3;
            unsigned char R = (unsigned char)(r / n);
            unsigned char G = (unsigned char)(gg / n);
            unsigned char B = (unsigned char)(b / n);
            if (g.mono) {
                /* The cell's own grey, not white. In ascii and braille the
                   glyph carries the brightness so either would do, but block
                   mode paints the cell with this colour directly - white
                   there turned the whole picture into a blank sheet. */
                unsigned char L = (unsigned char)((54u * R + 183u * G + 19u * B) >> 8);
                R = G = B = L;
            }
            if (g.invert) { R = 255 - R; G = 255 - G; B = 255 - B; }
            o[0] = R; o[1] = G; o[2] = B;
        }
    }
}

static void resample(int sub_w, int sub_h, int cols, int rows)
{
    ResampleCtx c = { sub_w, sub_h, cols, rows };
    parallel_rows(resample_lum_band, &c, 0, sub_h);
    parallel_rows(resample_rgb_band, &c, 0, rows);
}

/* Stretch the decoded frame's luminance to the full range, keeping hue.

   Photographs and renders rarely span 0..1, and without this the glyph set
   only ever uses its middle, which flattens the result. Applied to stills
   only: re-levelling every frame of a video makes the picture pulse. */
static void auto_levels_frame(void)
{
    size_t n = (size_t)g.dec_w * g.dec_h;
    unsigned int hist[256];
    memset(hist, 0, sizeof hist);
    for (size_t i = 0; i < n; i++) {
        const unsigned char *p = g.frame + i * 3;
        hist[(54u * p[0] + 183u * p[1] + 19u * p[2]) >> 8]++;
    }
    size_t lowcut = n / 100, highcut = n - n / 100, run = 0;
    int lo = 0, hi = 255;
    for (int v = 0; v < 256; v++) { run += hist[v]; if (run >= lowcut) { lo = v; break; } }
    run = 0;
    for (int v = 0; v < 256; v++) { run += hist[v]; if (run >= highcut) { hi = v; break; } }
    if (hi - lo < 8) return;

    float gain[256];
    for (int v = 0; v < 256; v++) {
        float stretched = (v - lo) * 255.0f / (hi - lo);
        stretched = clampf(stretched, 0.0f, 255.0f);
        gain[v] = v > 0 ? stretched / v : 0.0f;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char *p = g.frame + i * 3;
        int l = (int)((54u * p[0] + 183u * p[1] + 19u * p[2]) >> 8);
        float s = gain[l];
        for (int c = 0; c < 3; c++)
            p[c] = (unsigned char)clampf(p[c] * s, 0.0f, 255.0f);
    }
}

/* ------------------------------------------------------------------ colour */

/* Pick a colour of the given hue whose luminance is `target` (0..1).
   Scaling a peak-normalised colour by a brightness factor does not give that
   brightness - a saturated hue carries little luminance to start with - so
   solve for it, washing towards white only when the hue cannot get there. */
static void colorize(const unsigned char *rgb, float target, float whiten,
                     unsigned char *out)
{
    float r = rgb[0], gg = rgb[1], b = rgb[2];
    float peak = r > gg ? (r > b ? r : b) : (gg > b ? gg : b);
    if (peak < 1e-3f) { out[0] = out[1] = out[2] = 0; return; }
    float hr = r / peak, hg = gg / peak, hb = b / peak;
    float lh = 0.2126f * hr + 0.7152f * hg + 0.0722f * hb;
    float t = clampf(target, 0.0f, 1.0f);

    float orr, org, orb;
    if (t <= lh) {
        float s = t / (lh > 1e-3f ? lh : 1e-3f);
        orr = hr * s; org = hg * s; orb = hb * s;
    } else {
        float k = (t - lh) / (1.0f - lh > 1e-3f ? 1.0f - lh : 1e-3f);
        if (k > whiten) k = whiten;
        orr = hr + (1 - hr) * k;
        org = hg + (1 - hg) * k;
        orb = hb + (1 - hb) * k;
    }
    out[0] = (unsigned char)clampf(orr * 255.0f, 0, 255);
    out[1] = (unsigned char)clampf(org * 255.0f, 0, 255);
    out[2] = (unsigned char)clampf(orb * 255.0f, 0, 255);
}

/* ------------------------------------------------------------------ render */

/* sRGB <-> linear light, for compositing glyphs correctly.

   Blending a glyph over its background with the encoded byte values is the
   usual shortcut and it is wrong: a glyph whose ink covers 35% of the cell
   should emit 35% of the light, but mixing code values gives 35% of the code
   value, which is only about a tenth of the light. Over a whole picture made
   of thin strokes that is the difference between a readable image and a murky
   one. Terminals gamma-correct their glyph blending, which is exactly why the
   same art looks brighter in a terminal than it did here. */

/* How much of the brightness the glyph carries, the rest going to the colour.
 *
 * The densest glyph only inks about a third of its cell, so coverage alone
 * cannot reproduce a bright image - and if coverage is made to carry all of
 * it, every colour is driven to white and the picture goes monochrome. The
 * split is: coverage ~ rel^DENSITY and colour ~ rel^(1-DENSITY), whose
 * product is rel, so the brightness still comes out right while the colour
 * keeps enough value to show its hue. */

static void build_gamma_tables(void)
{
    for (int i = 0; i < 256; i++) {
        double c = i / 255.0;
        g_to_lin[i] = (float)(c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4));
    }
    for (int i = 0; i <= LIN_STEPS; i++) {
        double l = (double)i / LIN_STEPS;
        double c = l <= 0.0031308 ? l * 12.92 : 1.055 * pow(l, 1.0 / 2.4) - 0.055;
        g_to_srgb[i] = (unsigned char)(c * 255.0 + 0.5);
    }
}

static inline unsigned char lin_to_srgb(float l)
{
    int i = (int)(l * LIN_STEPS + 0.5f);
    return g_to_srgb[i < 0 ? 0 : (i > LIN_STEPS ? LIN_STEPS : i)];
}

static void blit_glyph(const unsigned char *atlas, int gi, int px, int py,
                       int cw, int ch, unsigned int fg, int stride)
{
    const unsigned char *cov = atlas + (size_t)gi * cw * ch;
    float fr = g_to_lin[(fg >> 16) & 0xff];
    float fgc = g_to_lin[(fg >> 8) & 0xff];
    float fb = g_to_lin[fg & 0xff];
    for (int y = 0; y < ch; y++) {
        int dy = py + y;
        if (dy < 0 || dy >= g.dib_h) continue;
        unsigned int *dst = g.pixels + (size_t)dy * stride + px;
        const unsigned char *crow = cov + (size_t)y * cw;
        for (int x = 0; x < cw; x++) {
            int dx = px + x;
            if (dx < 0 || dx >= g.dib_w) continue;
            unsigned int a = crow[x];
            if (!a) continue;
            if (a == 255) { dst[x] = fg; continue; }
            float t = a * (1.0f / 255.0f), u = 1.0f - t;
            unsigned int old = dst[x];
            unsigned char nr = lin_to_srgb(fr * t + g_to_lin[(old >> 16) & 0xff] * u);
            unsigned char ng = lin_to_srgb(fgc * t + g_to_lin[(old >> 8) & 0xff] * u);
            unsigned char nb = lin_to_srgb(fb * t + g_to_lin[old & 0xff] * u);
            dst[x] = ((unsigned int)nr << 16) | ((unsigned int)ng << 8) | nb;
        }
    }
}

/* A slim progress bar along the bottom while a video plays. Clicking it
   seeks, which is the one thing a keyboard-only player kept being clumsy
   for. */
static void draw_scrub_bar(void)
{
    g.bar_y0 = g.bar_y1 = -1;
    if (!g.is_video || g.duration <= 0 || !g.pixels) return;

    int h = 6, pad = 10;
    int y0 = g.dib_h - h - pad, y1 = y0 + h;
    if (y0 < 0) return;
    g.bar_y0 = y0 - 6;                       /* a forgiving click target */
    g.bar_y1 = y1 + 6;

    double frac = g.position / g.duration;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    int split = pad + (int)((g.dib_w - 2 * pad) * frac);

    for (int y = y0; y < y1; y++) {
        unsigned int *row = g.pixels + (size_t)y * g.dib_w;
        for (int x = pad; x < g.dib_w - pad; x++)
            row[x] = (x < split) ? 0x00e8e8f0 : 0x00404048;
    }
}

/* One horizontal band of cells. Bands never share output pixels, so they can
   run on different threads with no locking; the glyph atlas, the luminance
   buffer and the per-cell colours are all read-only here. */
typedef struct {
    int y0, y1;
    int cols, rows, cw, ch, ox, oy, stride, sub_w, sub_h, shape;
} RowJob;

static void render_rows(const RowJob *j)
{
    int cols = j->cols, cw = j->cw, ch = j->ch;
    int ox = j->ox, oy = j->oy, stride = j->stride, sub_w = j->sub_w;
    int shape = j->shape;
    (void)cols;

    if (g.mode == MODE_BLOCK) {
        /* Solid cells: one colour per cell, no glyph involved. */
        for (int y = j->y0; y < j->y1; y++) {
            for (int x = 0; x < cols; x++) {
                const unsigned char *c = g.cellrgb + ((size_t)y * cols + x) * 3;
                unsigned int col = ((unsigned int)c[0] << 16) |
                                   ((unsigned int)c[1] << 8) | c[2];
                for (int yy = 0; yy < ch; yy++) {
                    int dy = oy + y * ch + yy;
                    if (dy < 0 || dy >= g.dib_h) continue;
                    unsigned int *dst = g.pixels + (size_t)dy * stride + ox + x * cw;
                    for (int xx = 0; xx < cw; xx++) dst[xx] = col;
                }
            }
        }
        return;
    }

    if (g.mode == MODE_LINES) {
        /* Draw only edges, with the character that matches the edge's
           direction. No model is needed to find that direction: an edge runs
           perpendicular to the image gradient, so a Sobel operator plus
           atan2 gives it exactly. Each cell accumulates gradient magnitude
           into four orientation bins and takes the strongest. */
        static const int BIN_CH[4] = { L'-', L'\\', L'|', L'/' };
        for (int y = j->y0; y < j->y1; y++) {
            for (int x = 0; x < cols; x++) {
                float bins[4] = { 0, 0, 0, 0 };
                float total = 0, wy = 0, wsum = 0;

                for (int sy = 0; sy < GH; sy++) {
                    for (int sx = 0; sx < GW; sx++) {
                        int px = x * GW + sx, py = y * GH + sy;
                        int xm = px > 0 ? px - 1 : px;
                        int xp = px + 1 < sub_w ? px + 1 : px;
                        int ym = py > 0 ? py - 1 : py;
                        int yp = py + 1 < j->sub_h ? py + 1 : py;
                        const float *L = g.lumb;
#define LUM(a, b) L[(size_t)(b) * sub_w + (a)]
                        float gx = (LUM(xp, ym) + 2 * LUM(xp, py) + LUM(xp, yp))
                                 - (LUM(xm, ym) + 2 * LUM(xm, py) + LUM(xm, yp));
                        float gy = (LUM(xm, yp) + 2 * LUM(px, yp) + LUM(xp, yp))
                                 - (LUM(xm, ym) + 2 * LUM(px, ym) + LUM(xp, ym));
#undef LUM
                        float m = sqrtf(gx * gx + gy * gy);
                        if (m < 0.02f) continue;
                        /* edge direction is the gradient turned 90 degrees */
                        float ang = atan2f(gx, -gy);
                        if (ang < 0) ang += 3.14159265f;
                        int b = (int)((ang + 0.39269908f) / 0.78539816f) & 3;
                        bins[b] += m;
                        total += m;
                        wy += m * (sy + 0.5f);
                        wsum += m;
                    }
                }

                /* `<=` not `<`: with a threshold of zero a cell holding no
                   edge at all would otherwise pass and ink a glyph. */
                if (total <= 0.0f || total / NSUB < g.line_thresh) continue;

                int best = 0;
                for (int b = 1; b < 4; b++) if (bins[b] > bins[best]) best = b;
                int cp = BIN_CH[best];
                if (best == 0) {
                    /* Horizontal strokes sit high, middle or low in the cell;
                       the reference art uses all three. */
                    float f = wsum > 0 ? wy / wsum / GH : 0.5f;
                    cp = f < 0.34f ? L'\'' : (f > 0.66f ? L'_' : L'-');
                }
                int gi = cp - FIRST_CH;
                if (gi < 0 || gi >= NGLYPH) continue;

                unsigned char col[3];
                colorize(g.cellrgb + ((size_t)y * cols + x) * 3, 0.95f, g.whiten, col);
                unsigned int fg = ((unsigned int)col[0] << 16) |
                                  ((unsigned int)col[1] << 8) | col[2];
                blit_glyph(g.atlas, gi, ox + x * cw, oy + y * ch, cw, ch, fg, stride);
            }
        }
        return;
    }

    if (g.mode == MODE_BRAILLE) {
        /* Ordered dithering over the 2x4 dots: a dot is on or off, so the
           only way to render a shade is to vary how many are lit. */
        static const float BAYER8[8] = { -0.5f, 0.0f, -0.25f, 0.25f,
                                         -0.375f, 0.125f, -0.125f, 0.375f };
        for (int y = j->y0; y < j->y1; y++) {
            for (int x = 0; x < cols; x++) {
                int pat = 0;
                float sum = 0;
                for (int dx = 0; dx < BRW; dx++) {
                    for (int dy = 0; dy < BRH; dy++) {
                        float v = g.lum[(size_t)(y * BRH + dy) * sub_w + x * BRW + dx];
                        sum += v;
                        /* Threshold must sweep the whole 0..1 range, or the
                           shades outside it clip to all-off / all-on. */
                        float rel = g_rel_of[clampi((int)(v * 255.0f), 0, 255)];
                        if (rel > BAYER8[(dy * BRW + dx) & 7] + 0.5f)
                            pat |= 1 << (dx * BRH + dy);
                    }
                }
                float tone = sum / (BRW * BRH);
                float cover = g.br_cover[pat];
                if (cover < 0.004f) continue;            /* nothing lit */
                unsigned char col[3];
                float rel = g_rel_of[clampi((int)(tone * 255.0f), 0, 255)];
                float want_lin = rel * g.br_cmax / cover;
                float want = lin_to_srgb(want_lin) * (1.0f / 255.0f);
                colorize(g.cellrgb + ((size_t)y * cols + x) * 3, want, g.whiten, col);
                unsigned int fg = ((unsigned int)col[0] << 16) |
                                  ((unsigned int)col[1] << 8) | col[2];
                blit_glyph(g.br_atlas, pat, ox + x * cw, oy + y * ch, cw, ch, fg, stride);
            }
        }
        return;
    }

    for (int y = j->y0; y < j->y1; y++) {
        for (int x = 0; x < cols; x++) {
            int gi;
            float tone;

            if (shape) {
                /* Gather this cell's GW x GH patch. */
                float blk[NSUB];
                float bm = 0;
                for (int sy = 0; sy < GH; sy++) {
                    const float *row = g.lum + (size_t)(y * GH + sy) * sub_w + x * GW;
                    for (int sx = 0; sx < GW; sx++) {
                        /* into coverage units, via linear light */
                        float v = g_cover_of[clampi((int)(row[sx] * 255.0f), 0, 255)];
                        blk[sy * GW + sx] = v;
                        bm += v;
                    }
                }
                bm /= NSUB;
                float ss = 0;
                for (int i = 0; i < NSUB; i++) ss += blk[i] * blk[i];
                float bn2 = ss - NSUB * bm * bm;
                if (bn2 < 0) bn2 = 0;

                /* Tone and structure scored separately: plain squared error
                   punishes a glyph for concentrating its ink, which empties
                   out the midtones. Terms constant across glyphs are dropped
                   because they cannot change the winner. */
                float strength = clampf(sqrtf(bn2 / NSUB) / (0.12f * g.cmax), 0.0f, 1.0f);
                float alpha = 0.5f * strength / (sqrtf(bn2) + 1e-6f);
                float beta = 2.0f * bm / (g.cmax * g.cmax);

                int best = 0;
                float bestv = 1e30f;
                for (int k = 0; k < NGLYPH; k++) {
                    const float *gb = g.bank[k];
                    float dot = 0;
                    for (int i = 0; i < NSUB; i++) dot += blk[i] * gb[i];
                    float s = g.gm2c[k] - beta * g.gm[k]
                              - alpha * (dot * g.gnorm[k] - bm * g.gmn[k]);
                    if (s < bestv) { bestv = s; best = k; }
                }
                gi = best;
                tone = bm / g.cmax;
            } else {
                /* Cells too small for shape to read: straight tone ramp, with
                   an ordered dither of exactly one ramp step. Without it the
                   coarse set of coverages shows as contour rings on smooth
                   shading; with it the eye integrates neighbouring cells and
                   the gradient comes back. */
                static const float BAYER[16] = {
                    -0.5f,   0.0f,  -0.375f,  0.125f,
                     0.25f, -0.25f,  0.375f, -0.125f,
                    -0.3125f, 0.1875f, -0.4375f, 0.0625f,
                     0.4375f, -0.0625f, 0.3125f, -0.1875f };
                tone = g.lum[(size_t)y * sub_w + x];
                float want = g_cover_of[clampi((int)(tone * 255.0f), 0, 255)] / g.cmax;
                float d = want + BAYER[(y & 3) * 4 + (x & 3)] * g.ramp_step;
                gi = g.ramp[clampi((int)(clampf(d, 0.0f, 1.0f) * 255.0f), 0, 255)];
            }

            /* The glyph emits `cover` of the colour's light, so the colour
               carries the rest. Solved in linear light - the space the glyph
               is actually composited in - so that coverage x colour comes out
               equal to the source cell's light, scaled by the medium's
               ceiling. */
            unsigned char col[3];
            float cover = g.gm[gi];
            float rel = g_rel_of[clampi((int)(tone * 255.0f), 0, 255)];
            float want_lin = rel * g.cmax / (cover > 0.004f ? cover : 0.004f);
            float want = lin_to_srgb(want_lin) * (1.0f / 255.0f);
            colorize(g.cellrgb + ((size_t)y * cols + x) * 3, want, g.whiten, col);
            unsigned int fg = ((unsigned int)col[0] << 16) |
                              ((unsigned int)col[1] << 8) | col[2];
            blit_glyph(g.atlas, gi, ox + x * cw, oy + y * ch, cw, ch, fg, stride);
        }
    }
}

/* Used for the resample and the clear as well as the cells: once the cell
   loop was threaded, those became the next bottleneck. */
typedef struct { BandFn fn; void *ctx; int y0, y1; } Band;

static DWORD WINAPI band_thread(LPVOID p)
{
    Band *b = (Band *)p;
    b->fn(b->ctx, b->y0, b->y1);
    return 0;
}

static void parallel_rows(BandFn fn, void *ctx, int y0, int y1)
{
    int n = y1 - y0;
    if (n <= 0) return;
    int nthreads = g.nthreads;
    if (nthreads > n) nthreads = n;
    if (nthreads < 1) nthreads = 1;
    if (nthreads == 1 || n < 8) { fn(ctx, y0, y1); return; }

    Band bands[32];
    HANDLE th[32];
    int made = 0;
    for (int i = 0; i < nthreads; i++) {
        bands[i].fn = fn;
        bands[i].ctx = ctx;
        bands[i].y0 = y0 + n * i / nthreads;
        bands[i].y1 = y0 + n * (i + 1) / nthreads;
    }
    for (int i = 1; i < nthreads; i++) {
        th[made] = CreateThread(NULL, 0, band_thread, &bands[i], 0, NULL);
        if (th[made]) made++;
        else fn(ctx, bands[i].y0, bands[i].y1);
    }
    fn(ctx, bands[0].y0, bands[0].y1);
    if (made) {
        WaitForMultipleObjects(made, th, TRUE, INFINITE);
        for (int i = 0; i < made; i++) CloseHandle(th[i]);
    }
}

static void clear_band(void *ctx, int y0, int y1)
{
    (void)ctx;
    for (int y = y0; y < y1; y++) {
        unsigned int *row = g.pixels + (size_t)y * g.dib_w;
        for (int x = 0; x < g.dib_w; x++) row[x] = 0x00101010;
    }
}

/* Spread the bands over the machine's cores. At a big window with shape
   matching this is the difference between 8 fps and something usable - the
   per-cell glyph search is most of the frame time and is entirely
   independent from cell to cell. */
static void render_band(void *ctx, int y0, int y1)
{
    RowJob j = *(const RowJob *)ctx;
    j.y0 = y0;
    j.y1 = y1;
    render_rows(&j);
}

static void render_rows_parallel(RowJob base)
{
    parallel_rows(render_band, &base, base.y0, base.y1);
}

static void render(void)
{
    if (!g.have_frame || !g.pixels) return;

    int ch, cw;
    if (g.game) {
        /* Game mode counts columns instead of pixels, so "one letter" is a
           reachable state no matter how big the window is. */
        cw = imax(1, g.dib_w / imax(1, g.game_cols));
        ch = cw * 2;
        if (ch > g.dib_h) { ch = g.dib_h; cw = imax(1, ch / 2); }
    } else {
        ch = g.cell_h;
        cw = imax(1, ch / 2);                    /* exactly 1:2 by construction */
    }
    int maxcols = g.dib_w / cw, maxrows = g.dib_h / ch;
    if (maxcols < 1 || maxrows < 1) return;

    /* Fit the source aspect into the grid. cell_aspect is exactly ch/cw. */
    double aspect = (double)ch / cw;
    int cols = maxcols;
    int rows = (int)(cols * (double)g.dec_h / (g.dec_w * aspect) + 0.5);
    if (rows < 1) rows = 1;
    if (rows > maxrows) {
        rows = maxrows;
        cols = (int)(rows * aspect * (double)g.dec_w / g.dec_h + 0.5);
        cols = clampi(cols, 1, maxcols);
    }
    g.cols = cols;
    g.rows = rows;

    int shape = (g.mode == MODE_ASCII) && (ch >= SHAPE_MIN_CELL);
    int sub_w = cols, sub_h = rows;
    if (shape || g.mode == MODE_LINES) { sub_w = cols * GW;  sub_h = rows * GH;  }
    else if (g.mode == MODE_BRAILLE)   { sub_w = cols * BRW; sub_h = rows * BRH; }

    size_t need_sub = (size_t)sub_w * sub_h;
    size_t need_cells = (size_t)cols * rows;
    if ((int)need_sub > g.scratch_sub) {
        free(g.lum);
        free(g.lumb);
        g.lum = (float *)malloc(need_sub * sizeof(float));
        g.lumb = (float *)malloc(need_sub * sizeof(float));
        g.scratch_sub = (int)need_sub;
    }
    if ((int)need_cells > g.scratch_cells) {
        free(g.cellrgb);
        g.cellrgb = (unsigned char *)malloc(need_cells * 3);
        g.scratch_cells = (int)need_cells;
    }
    if (!g.lum || !g.lumb || !g.cellrgb) return;

    resample(sub_w, sub_h, cols, rows);
    build_atlas(cw, ch);
    /* Both atlases must be built before the bands start: they are shared,
       and a worker rebuilding one while another reads it corrupts the frame. */
    if (g.mode == MODE_BRAILLE) build_braille_atlas(cw, ch);
    if (g.mode == MODE_LINES) {
        BlurCtx bc = { sub_w, sub_h };
        parallel_rows(blur_band, &bc, 0, sub_h);
    }

    parallel_rows(clear_band, NULL, 0, g.dib_h);

    int art_w = cols * cw, art_h = rows * ch;
    int ox = (g.dib_w - art_w) / 2, oy = (g.dib_h - art_h) / 2;
    int stride = g.dib_w;

    RowJob job = { 0, rows, cols, rows, cw, ch, ox, oy, stride, sub_w, sub_h, shape };
    render_rows_parallel(job);

    /* After the cells, so it sits on top of the picture. (Splitting render()
       into threaded bands dropped this call; the unused-function warning is
       what caught it.) */
    draw_scrub_bar();
}


/* ------------------------------------------------------------------ buffer */

static void alloc_dib(int w, int h)
{
    w = imax(1, w);
    h = imax(1, h);
    if (g.dib && w == g.dib_w && h == g.dib_h) return;

    if (g.dib) DeleteObject(g.dib);
    BITMAPINFO bi;
    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(NULL);               /* screen DC: works with no window yet */
    g.dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&g.pixels, NULL, 0);
    ReleaseDC(NULL, dc);
    g.dib_w = w;
    g.dib_h = h;
}

static void resize_dib(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    alloc_dib(rc.right - rc.left, rc.bottom - rc.top);
}

static int write_bmp(const wchar_t *name)
{
    if (!g.pixels) return 0;
    int w = g.dib_w, h = g.dib_h;
    int rowbytes = w * 3, pad = (4 - (rowbytes % 4)) % 4;
    unsigned int datasize = (rowbytes + pad) * h;

    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    ZeroMemory(&fh, sizeof fh);
    ZeroMemory(&ih, sizeof ih);
    fh.bfType = 0x4d42;
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + datasize;
    ih.biSize = sizeof ih;
    ih.biWidth = w;
    ih.biHeight = h;              /* bottom-up */
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = datasize;

    FILE *f = _wfopen(name, L"wb");
    if (!f) return 0;
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&ih, sizeof ih, 1, f);
    /* One buffered write per row; a write per pixel costs far more than the
       whole render does. */
    unsigned char *row = (unsigned char *)calloc((size_t)rowbytes + pad, 1);
    if (!row) { fclose(f); return 0; }
    for (int y = h - 1; y >= 0; y--) {
        const unsigned int *src = g.pixels + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            unsigned int p = src[x];
            row[x * 3 + 0] = (unsigned char)(p & 0xff);          /* B */
            row[x * 3 + 1] = (unsigned char)((p >> 8) & 0xff);   /* G */
            row[x * 3 + 2] = (unsigned char)((p >> 16) & 0xff);  /* R */
        }
        fwrite(row, (size_t)rowbytes + pad, 1, f);
    }
    free(row);
    fclose(f);
    return 1;
}

static void save_bmp(void)
{
    /* Name the file after the picture it came from, not "asciiart.bmp". */
    wchar_t name[MAX_PATH];
    const wchar_t *base = wcsrchr(g.path, L'\\');
    base = base ? base + 1 : g.path;
    wcsncpy(name, base[0] ? base : L"asciiart", MAX_PATH - 8);
    name[MAX_PATH - 8] = 0;
    wchar_t *dot = wcsrchr(name, L'.');
    if (dot && dot != name) *dot = 0;
    wcscat(name, L"-ascii.bmp");
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter = L"Bitmap\0*.bmp\0";
    ofn.lpstrFile = name;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"bmp";
    ofn.Flags = OFN_OVERWRITEPROMPT;
    if (GetSaveFileNameW(&ofn)) write_bmp(name);
}

static void open_dialog(void)
{
    wchar_t name[MAX_PATH] = L"";
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter = L"Media\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp;*.mp4;*.mkv;"
                      L"*.mov;*.avi;*.webm\0All files\0*.*\0";
    ofn.lpstrFile = name;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) {
        if (!open_media(name, 0))
            MessageBoxW(g.hwnd, L"Could not decode that file.", L"asciiapp", MB_OK);
        InvalidateRect(g.hwnd, NULL, FALSE);
    }
}

static const wchar_t *mode_name(void)
{
    if (g.mode == MODE_BLOCK) return L"blocks";
    if (g.mode == MODE_BRAILLE) return L"braille";
    if (g.mode == MODE_LINES) return L"line art";
    return g.cell_h >= SHAPE_MIN_CELL ? L"ascii (shape)" : L"ascii (tone)";
}

/* Blocking open, used everywhere except a seek. */
static int open_media(const wchar_t *path, double start_at)
{
    return open_media_ex(path, start_at, 1);
}

/* Jump to another picture in the same folder, for playing the guessing game
   without anyone seeing a file dialog (which would give the answer away). */
/* Advance the video to wherever the wall clock says we should be.
 *
 * Reading exactly one frame per timer tick looks right until a render takes
 * longer than a frame interval - then it never catches up and the video
 * plays in slow motion, dutifully showing every frame. Instead, work out how
 * many frames are due by now and throw away all but the last of them.
 *
 * Returns 1 if there is a new frame to draw, 0 if nothing is due yet.
 */
static int pump_video(void)
{
    if (!(g.is_video && g.playing && g.pipe)) return 0;

    /* ffmpeg is started with -re, so it emits frames at the clip's real rate
       and the pipe is the clock. All this has to do is take whatever has
       piled up and keep only the newest, which costs a memcpy per skipped
       frame and never blocks. Deciding "how many frames are due" from a wall
       clock and then reading that many does not work here: a read blocks
       until ffmpeg produces the frame, so skipping cost as much as drawing
       and playback still slid behind. */
    size_t nbytes = (size_t)g.dec_w * g.dec_h * 3;
    DWORD avail = 0;
    if (!PeekNamedPipe(g.pipe, NULL, 0, NULL, &avail, NULL)) {
        open_media(g.path, 0);                      /* pipe died: loop */
        return 0;
    }
    if (avail < nbytes) {
        /* Nothing complete yet. If ffmpeg has exited and drained, restart. */
        if (g.proc && WaitForSingleObject(g.proc, 0) == WAIT_OBJECT_0 && avail == 0)
            open_media(g.path, 0);
        return 0;
    }

    long long ready = avail / nbytes;
    for (long long i = 0; i < ready; i++) {
        if (!read_exact(g.pipe, g.frame, nbytes)) { open_media(g.path, 0); return 0; }
        g.frames_read++;
        if (i + 1 < ready) g.frames_dropped++;      /* superseded before drawing */
    }
    /* Count from where the seek put us, not from zero - otherwise every
       seek snaps the clock back and '.' keeps jumping to the same spot. */
    g.position = g.play_origin + g.frames_read / (g.fps_num > 0 ? g.fps_num : 25.0);
    return 1;
}

/* Every picture sitting next to the current one, sorted, so stepping through
   a folder is predictable and the guessing game can pick at random without
   anyone seeing a file dialog (which would give the answer away). */
#define MAX_SIBLINGS 512

static int list_siblings(wchar_t (*out)[MAX_PATH], int cap)
{
    static const wchar_t *pats[] = { L"*.png", L"*.jpg", L"*.jpeg", L"*.bmp",
                                     L"*.webp", L"*.gif", NULL };
    wchar_t dir[MAX_PATH];
    if (g.path[0]) {
        wcsncpy(dir, g.path, MAX_PATH - 1);
        dir[MAX_PATH - 1] = 0;
        wchar_t *slash = wcsrchr(dir, L'\\');
        if (slash) *slash = 0; else wcscpy(dir, L".");
    } else {
        /* Nothing open yet: look in the samples folder beside the exe, so
           the game can start from a cold launch. */
        GetModuleFileNameW(NULL, dir, MAX_PATH);
        wchar_t *slash = wcsrchr(dir, L'\\');
        if (slash) *slash = 0;
        wcsncat(dir, L"\\samples", MAX_PATH - wcslen(dir) - 1);
    }

    int n = 0;
    for (int p = 0; pats[p] && n < cap; p++) {
        wchar_t glob[MAX_PATH];
        _snwprintf(glob, MAX_PATH - 1, L"%s\\%s", dir, pats[p]);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(glob, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            _snwprintf(out[n], MAX_PATH - 1, L"%s\\%s", dir, fd.cFileName);
            n++;
        } while (n < cap && FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (int i = 1; i < n; i++) {                 /* insertion sort by name */
        wchar_t key[MAX_PATH];
        wcscpy(key, out[i]);
        int j = i - 1;
        while (j >= 0 && _wcsicmp(out[j], key) > 0) { wcscpy(out[j + 1], out[j]); j--; }
        wcscpy(out[j + 1], key);
    }
    return n;
}

/* Step to the previous/next picture in the folder, wrapping at the ends. */
static void change_picture(int delta)
{
    static wchar_t found[MAX_SIBLINGS][MAX_PATH];
    int n = list_siblings(found, MAX_SIBLINGS);
    if (n == 0) return;

    int cur = -1;
    for (int i = 0; i < n; i++)
        if (_wcsicmp(found[i], g.path) == 0) { cur = i; break; }

    int next = (cur < 0) ? 0 : ((cur + delta) % n + n) % n;
    open_media(found[next], 0);
}

/* A random different one, for the guessing game. */
static void next_picture(void)
{
    static wchar_t found[MAX_SIBLINGS][MAX_PATH];
    int n = list_siblings(found, MAX_SIBLINGS);
    if (n == 0) return;
    for (int tries = 0; tries < 16; tries++) {
        const wchar_t *pick = found[rand() % n];
        if (n == 1 || _wcsicmp(pick, g.path) != 0) {
            open_media(pick, 0);
            return;
        }
    }
}

/* "city.png" -> "City": the stem, capitalised. Used when the game gives
   the answer away, where the raw filename read badly. */
static void pretty_name(wchar_t *out, int cap)
{
    const wchar_t *base = wcsrchr(g.path, L'\\');
    base = base ? base + 1 : g.path;
    wcsncpy(out, base[0] ? base : L"nothing", cap - 1);
    out[cap - 1] = 0;
    wchar_t *dot = wcsrchr(out, L'.');
    if (dot && dot != out) *dot = 0;
    if (out[0] >= L'a' && out[0] <= L'z') out[0] = (wchar_t)(out[0] - L'a' + L'A');
}

/* 1..100, where 100 is maximum zoom (a few giant letters) and 1 is the full
   grid. Logarithmic, because the column count is geometric. */
static int zoom_level(void)
{
    double maxcols = g.dib_w > 2 ? g.dib_w : 2;        /* cw can go down to 1 */
    double c = g.cols < 1 ? 1 : g.cols;
    if (c > maxcols) c = maxcols;
    double lvl = 100.0 - 99.0 * (log(c) / log(maxcols));
    return clampi((int)(lvl + 0.5), 1, 100);
}

static void fmt_time(double secs, wchar_t *out, int cap)
{
    if (secs < 0) secs = 0;
    int t = (int)(secs + 0.5);
    _snwprintf(out, cap - 1, L"%d:%02d", t / 60, t % 60);
}

static void update_title(void)
{
    const wchar_t *base = wcsrchr(g.path, L'\\');
    base = base ? base + 1 : g.path;
    if (g.game) {
        /* The filename would give the answer away, so it stays hidden until
           someone presses Enter. */
        if (g.game_reveal) {
            wchar_t nice[128];
            pretty_name(nice, 128);
            _snwprintf(g.status, 255, L"ASCIIArt - it was: %s - level %d",
                       nice, zoom_level());
        } else {
            _snwprintf(g.status, 255,
                       L"Guess the picture - level %d - Space reveals more, "
                       L"N next, R restart, Enter gives up",
                       zoom_level());
        }
    } else {
        wchar_t extra[128] = L"";
        if (g.is_video) {
            wchar_t a[16], b[16];
            fmt_time(g.position, a, 16);
            fmt_time(g.duration - g.position, b, 16);
            const wchar_t *snd = !g.has_audio ? L", no audio track"
                              : (g.audio_on ? L"" : L", muted");
            _snwprintf(extra, 127, L" - %s elapsed, -%s left%s%s - a/v %+.2fs", a, b,
                       g.playing ? L"" : L" (paused)", snd, g.av_offset);
        }
        _snwprintf(g.status, 255,
                   L"ASCIIArt - %s - %dx%d cells (%dpx, level %d) - %s - %s%s%s"
                   L" - midtone %.2f%s%s",
                   base[0] ? base : L"(nothing open)", g.cols, g.rows, g.cell_h,
                   zoom_level(), mode_name(),
                   g.mono ? L"mono" : L"colour",
                   g.invert ? L" - inverted" : L"",
                   g.levels ? L" - auto-levels" : L"",
                   g.out_gamma, extra,
                   g.slideshow ? L" - slideshow" : L"");
    }
    SetWindowTextW(g.hwnd, g.status);
}

/* Shown when nothing is loaded. The app used to throw a file dialog at you
   the moment it started, which is a rude way to open. */
static void draw_empty(HDC dc)
{
    RECT rc = { 0, 0, g.dib_w, g.dib_h };
    if (g.hwnd) GetClientRect(g.hwnd, &rc);
    HBRUSH bg = CreateSolidBrush(RGB(10, 11, 15));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);

    /* Backdrop: the title screen is itself ASCII art. A smooth field of
       interfering ripples is run through the same kind of density ramp the
       renderer uses, so the front page advertises what the program does. */
    const int cw = 9, chh = 17;
    int cols = rc.right / cw + 1, rows = rc.bottom / chh + 1;
    static const wchar_t *RAMP = L" .'`^\":;!i~+?]tfjvcxunzXYUCLQ0OZmwqpdbkhao*#MW&8%B@$";
    int nramp = (int)wcslen(RAMP);

    HFONT bf = CreateFontW(-chh + 2, cw, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET,
                           OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                           FIXED_PITCH | FF_MODERN, FONT_NAME);
    HGDIOBJ oldf = SelectObject(dc, bf);

    wchar_t *line = (wchar_t *)malloc((cols + 2) * sizeof(wchar_t));
    unsigned char *lev = (unsigned char *)malloc(cols + 2);
    if (line && lev) {
        const int NLEV = 10;
        double ccx = cols * 0.5, ccy = rows * 0.5;
        for (int y = 0; y < rows; y++) {
            for (int x = 0; x < cols; x++) {
                double dx = (x - ccx) / ccx, dy = (y - ccy) / ccy;
                double r = sqrt(dx * dx + dy * dy * 2.4);
                /* two ripples plus a vignette: smooth, so the ramp reads */
                double v = 0.5 + 0.34 * sin(r * 7.5 - 1.1) + 0.16 * sin(dx * 5.0 + dy * 3.0);
                v *= 1.0 - 0.60 * r;
                if (v < 0) v = 0;
                if (v > 1) v = 1;
                int idx = (int)(v * (nramp - 1) + 0.5);
                line[x] = RAMP[idx < 0 ? 0 : (idx >= nramp ? nramp - 1 : idx)];
                lev[x] = (unsigned char)(v * (NLEV - 1) + 0.5);
            }
            line[cols] = 0;

            /* Colour each cell by the field, not the row, and draw it in runs
               of equal brightness. One flat colour per row made the art far
               too faint to read against the background. */
            int x = 0;
            while (x < cols) {
                int x2 = x + 1;
                while (x2 < cols && lev[x2] == lev[x]) x2++;
                double t = lev[x] / (double)(NLEV - 1);
                int rr = (int)(38 + 112 * t);
                int gg = (int)(58 + 160 * t);
                int bb = (int)(78 + 177 * t);
                SetTextColor(dc, RGB(rr, gg, bb));
                TextOutW(dc, x * cw, y * chh, line + x, x2 - x);
                x = x2;
            }
        }
    }
    free(line);
    free(lev);
    SelectObject(dc, oldf);
    DeleteObject(bf);

    /* A panel so the text stays readable over the art. */
    int pw = 520, ph = 230;
    RECT panel = { (rc.right - pw) / 2, (rc.bottom - ph) / 2,
                   (rc.right + pw) / 2, (rc.bottom + ph) / 2 };
    HBRUSH pb = CreateSolidBrush(RGB(13, 15, 20));
    FillRect(dc, &panel, pb);
    DeleteObject(pb);
    HBRUSH edge = CreateSolidBrush(RGB(48, 72, 96));
    FrameRect(dc, &panel, edge);
    DeleteObject(edge);

    HFONT big = CreateFontW(-42, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET,
                            OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                            FIXED_PITCH | FF_MODERN, FONT_NAME);
    HGDIOBJ old = SelectObject(dc, big);
    SetTextColor(dc, RGB(126, 214, 255));
    const wchar_t *t1 = L"ASCIIArt";
    SIZE sz;
    GetTextExtentPoint32W(dc, t1, (int)wcslen(t1), &sz);
    int ty = panel.top + 26;
    TextOutW(dc, (rc.right - sz.cx) / 2, ty, t1, (int)wcslen(t1));
    SelectObject(dc, old);
    DeleteObject(big);

    HFONT small = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET,
                              OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                              FIXED_PITCH | FF_MODERN, FONT_NAME);
    old = SelectObject(dc, small);
    static const wchar_t *msg[] = {
        L"pictures and video, drawn with characters",
        L"",
        L"Ctrl+O   open a file        H   all the keys",
        L"drop a file here            G   guess the picture",
        NULL
    };
    for (int i = 0; msg[i]; i++) {
        SetTextColor(dc, i == 0 ? RGB(150, 160, 175) : RGB(205, 212, 222));
        GetTextExtentPoint32W(dc, msg[i], (int)wcslen(msg[i]), &sz);
        TextOutW(dc, (rc.right - sz.cx) / 2, ty + 62 + i * 26, msg[i], (int)wcslen(msg[i]));
    }
    SelectObject(dc, old);
    DeleteObject(small);
}

/* A key list drawn over the picture, since there is no menu bar. */
static void draw_help(HDC dc)
{
    const wchar_t **lines;
    static const wchar_t *normal_lines[] = {
        L"  + / -  or wheel    zoom  (4-48 px cells)",
        L"  M                  mode: ascii / line art / braille / blocks",
        L"  [  ]               midtones  (line art: edge threshold)",
        L"  left / right       previous / next picture in the folder",
        L"  C                  colour / monochrome",
        L"  I                  invert",

        L"  F                  fullscreen",
        L"  L                  auto-levels: stretch the darkest and",
        L"                     brightest parts out to the full range",
        L"  A                  mute / unmute video sound",
        L"  ;  \x27              nudge audio earlier / later",
        L"  P                  slideshow (next picture every 4s)",
        L"  0                  reset everything to defaults",
        L"  Space              pause video  (click the bar to seek)",
        L"  ,  .               seek back / forward 5s",
        L"  Ctrl+O  or  O      open a file",
        L"  S                  save a .bmp of the view",
        L"  G                  guess-the-picture game",
        L"  H  or  ?           hide this list",
        L"  Q  or  Esc         quit",
        NULL
    };
    lines = normal_lines;
    static const wchar_t *game_lines[] = {
        L"  Space             reveal a bit more",
        L"  N                 another picture",
        L"  R                 start this one over",
        L"  Enter             give up, show what it was",
        L"  G                 leave the game",
        NULL
    };
    if (g.game) lines = game_lines;

    int n = 0;
    while (lines[n]) n++;

    HFONT font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET,
                             OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             FIXED_PITCH | FF_MODERN, FONT_NAME);
    HGDIOBJ oldfont = SelectObject(dc, font);

    RECT box = { 20, 20, 20 + 430, 20 + 22 * (n + 1) + 10 };
    HBRUSH bg = CreateSolidBrush(RGB(16, 16, 20));
    FillRect(dc, &box, bg);
    DeleteObject(bg);
    FrameRect(dc, &box, (HBRUSH)GetStockObject(GRAY_BRUSH));

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(220, 220, 220));
    TextOutW(dc, 32, 30, g.status, (int)wcslen(g.status));
    SetTextColor(dc, RGB(170, 200, 230));
    for (int i = 0; i < n; i++)
        TextOutW(dc, 32, 30 + 22 * (i + 1), lines[i], (int)wcslen(lines[i]));

    SelectObject(dc, oldfont);
    DeleteObject(font);
}

/* -------------------------------------------------------------- window proc */

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        DragAcceptFiles(hwnd, TRUE);
        return 0;

    case WM_DROPFILES: {
        wchar_t name[MAX_PATH];
        DragQueryFileW((HDROP)wp, 0, name, MAX_PATH);
        DragFinish((HDROP)wp);
        open_media(name, 0);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_SIZE:
        resize_dib(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_TIMER:
        if (wp == 3) {                       /* debounced seek */
            KillTimer(hwnd, 3);
            if (g.seek_pending) {
                g.seek_pending = 0;
                open_media_ex(g.path, g.seek_to, 0);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        if (wp == 2) {                       /* slideshow tick */
            if (g.slideshow && !g.is_video) {
                change_picture(1);
                if (g.game) { g.game_cols = 1; g.game_reveal = 0; }
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        if (pump_video() > 0) InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_LBUTTONDOWN: {
        int mx = (short)LOWORD(lp), my = (short)HIWORD(lp);
        if (g.is_video && g.duration > 0 && my >= g.bar_y0 && my <= g.bar_y1) {
            int pad = 10, span = g.dib_w - 2 * pad;
            double frac = span > 0 ? (double)(mx - pad) / span : 0;
            if (frac < 0) frac = 0;
            if (frac > 1) frac = 1;
            open_media(g.path, frac * g.duration);
        } else if (g.game) {
            int next = g.game_cols + 1;
            int grown = (int)(g.game_cols * 1.45f);
            g.game_cols = grown > next ? grown : next;
            if (g.game_cols > 600) g.game_cols = 600;
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }

    case WM_MOUSEWHEEL:
        zoom(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_LEFT || wp == VK_RIGHT) {
            change_picture(wp == VK_RIGHT ? 1 : -1);
            if (g.game) { g.game_cols = 1; g.game_reveal = 0; }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);

    case WM_CHAR:
        switch (wp) {
        case '+': case '=': zoom(1); break;
        case '-': case '_': zoom(-1); break;
        case 'c': case 'C': g.mono = !g.mono; break;
        case 'i': case 'I': g.invert = !g.invert; break;
        case '[':
            if (g.mode == MODE_LINES) {
                g.line_thresh = clampf(g.line_thresh * 0.8f, 0.005f, 1.0f);
            } else {
                g.out_gamma = clampf(g.out_gamma + 0.06f, 0.2f, 2.0f);
                build_tone_tables();
            }
            break;
        case ']':
            if (g.mode == MODE_LINES) {
                g.line_thresh = clampf(g.line_thresh * 1.25f, 0.005f, 1.0f);
            } else {
                g.out_gamma = clampf(g.out_gamma - 0.06f, 0.2f, 2.0f);
                build_tone_tables();
            }
            break;
        case 'f': case 'F': {
            /* Fullscreen: the cell count is limited by window pixels, so the
               way to out-resolve a zoomed-out terminal is more window. */
            static WINDOWPLACEMENT prev = { sizeof prev };
            static int full = 0;
            DWORD style = GetWindowLong(hwnd, GWL_STYLE);
            if (!full) {
                MONITORINFO mi = { sizeof mi };
                if (GetWindowPlacement(hwnd, &prev) &&
                    GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
                    SetWindowLong(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
                    SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                                 mi.rcMonitor.right - mi.rcMonitor.left,
                                 mi.rcMonitor.bottom - mi.rcMonitor.top,
                                 SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
                    full = 1;
                }
            } else {
                SetWindowLong(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
                SetWindowPlacement(hwnd, &prev);
                SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                             SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
                full = 0;
            }
            break;
        }
        case 'l': case 'L':
            g.levels = !g.levels;
            if (g.path[0]) open_media(g.path, 0);
            break;
        case 'm': case 'M':
            g.mode = (g.mode + 1) % MODE_COUNT; break;
        case 15:                      /* Ctrl+O */
            open_dialog(); break;
        case 'h': case 'H': case '?':
            g.show_help = !g.show_help; break;
        case 'g': case 'G':
            g.game = !g.game;
            g.game_cols = 1;
            g.game_reveal = 0;
            g.show_help = 0;
            /* Play with whatever is on screen; if that is nothing, go and
               find a picture rather than starting on a blank window. */
            if (g.game && !g.have_frame) next_picture();
            break;
        case '0': {
            int was_game = g.game;
            g.cell_h = 16;
            g.mode = MODE_ASCII;
            g.mono = g.invert = 0;
            g.levels = 1;
            g.out_gamma = OUT_GAMMA_DEFAULT;
            g.slideshow = 0;
            g.game = 0;
            g.audio_on = 1;
            g.av_offset = AV_OFFSET_DEFAULT;
            g.line_thresh = LINE_THRESH_DEFAULT;
            build_tone_tables();
            if (was_game && g.path[0]) open_media(g.path, 0);
            break;
        }
        case 'a': case 'A':
            g.audio_on = !g.audio_on;
            if (g.is_video && g.playing) start_audio(g.position);
            else stop_audio();
            break;
        case 'p': case 'P':
            g.slideshow = !g.slideshow;
            if (g.slideshow) SetTimer(hwnd, 2, 4000, NULL);
            else KillTimer(hwnd, 2);
            break;
        case 'r': case 'R':
            if (g.game) { g.game_cols = 1; g.game_reveal = 0; }
            break;
        case 13:                      /* Enter: give up, show what it was */
            if (g.game) g.game_reveal = 1;
            break;
        case 'n': case 'N':
            if (g.game) { next_picture(); g.game_cols = 1; g.game_reveal = 0; }
            break;
        case 'o': case 'O':
            open_dialog(); break;
        case 's': case 'S':
            save_bmp(); return 0;
        case ' ':
            if (g.game) {
                /* reveal a little more of the picture */
                int next = g.game_cols + 1;
                int grown = (int)(g.game_cols * 1.45f);
                g.game_cols = grown > next ? grown : next;
                if (g.game_cols > 600) g.game_cols = 600;
            } else if (g.is_video) {
                g.playing = !g.playing;
                if (!g.playing) stop_audio();
                else start_audio(g.position);
                if (g.playing) {
                    /* restart the clock where we left off, so the pause does
                       not count as time we fell behind */
                    LARGE_INTEGER now;
                    QueryPerformanceCounter(&now);
                    double fps = g.fps_num > 0 ? g.fps_num : 25;
                    g.play_t0.QuadPart = now.QuadPart -
                        (LONGLONG)(g.frames_read / fps * g.qpf.QuadPart);
                }
            }
            break;
        case ',': case '.': {
            /* Each seek restarts ffmpeg and ffplay, which costs a couple of
               hundred milliseconds. Holding the key would queue one of those
               per press; instead note the target and act once things go
               quiet, so a burst of presses costs a single reopen. */
            if (!g.is_video) break;
            double from = g.seek_pending ? g.seek_to : g.position;
            double t = from + (wp == '.' ? 5 : -5);
            if (t < 0) t = 0;
            if (g.duration > 0 && t >= g.duration - 0.25) t = 0;
            g.seek_to = t;
            g.seek_pending = 1;
            g.position = t;                 /* title and bar react at once */
            SetTimer(hwnd, 3, 110, NULL);
            break;
        }
        case ';':
            g.av_offset = clampf((float)g.av_offset - 0.05f, -2.0f, 2.0f);
            if (g.is_video && g.playing) start_audio(g.position);
            break;
        case '\'':
            g.av_offset = clampf((float)g.av_offset + 0.05f, -2.0f, 2.0f);
            if (g.is_video && g.playing) start_audio(g.position);
            break;
        case 'q': case 'Q': case 27:
            PostQuitMessage(0); return 0;
        default:
            return 0;
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_ERASEBKGND:
        return 1;                   /* we paint every pixel ourselves */

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (!g.have_frame) {
            draw_empty(dc);
            SetWindowTextW(hwnd, L"ASCIIArt - press Ctrl+O to open something");
            EndPaint(hwnd, &ps);
            return 0;
        }
        render();
        update_title();
        /* Draw the overlay into the back buffer and blit once. Painting it
           onto the window after the blit meant every video frame wiped it
           and redrew it, which read as a flicker. */
        HDC mem = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(mem, g.dib);
        if (g.show_help) draw_help(mem);
        BitBlt(dc, 0, 0, g.dib_w, g.dib_h, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        close_media();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* -------------------------------------------------------------------- main */

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    (void)prev; (void)cmdline;

    enable_dpi_awareness();   /* before any window or DC exists */
    init_child_job();

    SYSTEM_INFO si_cpu;
    GetSystemInfo(&si_cpu);
    g.nthreads = clampi((int)si_cpu.dwNumberOfProcessors, 1, 32);

    g.cell_h = 16;
    g.mode = MODE_ASCII;
    g.levels = 1;
    g.whiten = 0.35f;
    g.out_gamma = OUT_GAMMA_DEFAULT;
    g.show_help = 1;
    g.audio_on = 1;
    g.have_ffplay = 1;   /* assumed; a missing ffplay just never starts */
    g.av_offset = AV_OFFSET_DEFAULT;
    g.line_thresh = LINE_THRESH_DEFAULT;

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"ASCIIArtWindow";
    /* 101 is IDI_APPICON from asciiapp.rc; fall back if built without it */
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(101));
    if (!wc.hIcon) wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    g.hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName,
                             L"ASCIIArt", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1100, 760,
                             NULL, NULL, inst, NULL);
    if (!g.hwnd) die(L"Could not create the window.");

    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    /* Offscreen render:  asciiapp in.png -o out.bmp [-W px] [-H px] [-c cell] [-m block]
       The canvas is not limited by the screen, so this is the way to get a
       grid far larger than any window (or terminal) could show. */
    const wchar_t *out = NULL, *infile = NULL;
    int ow = 1920, oh = 1080;
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"-o") && i + 1 < argc) out = argv[++i];
        else if (!wcscmp(argv[i], L"-W") && i + 1 < argc) ow = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"-H") && i + 1 < argc) oh = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"-c") && i + 1 < argc) g.cell_h = _wtoi(argv[++i]);
        else if (!wcscmp(argv[i], L"-bench") && i + 1 < argc) i++;   /* read below */
        else if (!wcscmp(argv[i], L"-playtest") && i + 1 < argc) i++;
        else if (!wcscmp(argv[i], L"-w") && i + 1 < argc) g.whiten = (float)_wtof(argv[++i]);
        else if (!wcscmp(argv[i], L"-g") && i + 1 < argc) g.out_gamma = (float)_wtof(argv[++i]);
        else if (!wcscmp(argv[i], L"-splash")) { }   /* read below */
        else if (!wcscmp(argv[i], L"-L")) g.levels = 0;
        else if (!wcscmp(argv[i], L"-av") && i + 1 < argc) g.av_offset = _wtof(argv[++i]);
        else if (!wcscmp(argv[i], L"-lt") && i + 1 < argc) g.line_thresh = (float)_wtof(argv[++i]);
        else if (!wcscmp(argv[i], L"-mono")) g.mono = 1;
        else if (!wcscmp(argv[i], L"-inv")) g.invert = 1;
        else if (!wcscmp(argv[i], L"-m") && i + 1 < argc) {
            const wchar_t *m = argv[++i];
            g.mode = !wcscmp(m, L"block") ? MODE_BLOCK
                   : !wcscmp(m, L"braille") ? MODE_BRAILLE
                   : !wcscmp(m, L"lines") ? MODE_LINES : MODE_ASCII;
        }
        else if (argv[i][0] != L'-') infile = argv[i];
    }
    g.cell_h = clampi(g.cell_h, CELL_MIN, CELL_MAX);
    g.out_gamma = clampf(g.out_gamma, 0.2f, 2.0f);
    g.whiten = clampf(g.whiten, 0.0f, 1.0f);

    /* After the options, because the tone tables bake in out_gamma. */
    build_gamma_tables();
    build_bank();
    resize_dib(g.hwnd);

    int bench = 0, splash = 0;
    double playtest = 0;
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"-bench") && i + 1 < argc) bench = _wtoi(argv[i + 1]);
        if (!wcscmp(argv[i], L"-playtest") && i + 1 < argc) playtest = _wtof(argv[i + 1]);
        if (!wcscmp(argv[i], L"-splash")) splash = 1;
    }

    if (out && (infile || splash)) {
        DestroyWindow(g.hwnd);
        g.hwnd = NULL;
        alloc_dib(clampi(ow, 16, 20000), clampi(oh, 16, 20000));
        if (splash) {
            HDC dc = CreateCompatibleDC(NULL);
            HGDIOBJ oldb = SelectObject(dc, g.dib);
            draw_empty(dc);
            GdiFlush();
            SelectObject(dc, oldb);
            DeleteDC(dc);
            int ok = write_bmp(out);
            LocalFree(argv);
            return ok ? 0 : 3;
        }

        if (!open_media(infile, 0)) return 2;

        if (playtest > 0) {
            /* Run the real pacing path with no window, so the frame timing
               can be checked without anyone watching a screen. */
            LARGE_INTEGER t0, now;
            QueryPerformanceFrequency(&g.qpf);
            QueryPerformanceCounter(&t0);
            long long drawn = 0;
            for (;;) {
                QueryPerformanceCounter(&now);
                double el = (double)(now.QuadPart - t0.QuadPart) / g.qpf.QuadPart;
                if (el >= playtest) break;
                if (pump_video() > 0) { render(); drawn++; }
                else Sleep(1);
            }
            QueryPerformanceCounter(&now);
            double el = (double)(now.QuadPart - t0.QuadPart) / g.qpf.QuadPart;
            FILE *f = _wfopen(out, L"w");
            if (f) {
                fprintf(f, "%.2fs wall, %lld frames read, %lld drawn, %lld dropped\n"
                           "source %.1f fps -> consumed %.1f fps, drew %.1f fps\n"
                           "speed %.2fx (1.00 = real time)\n",
                        el, g.frames_read, drawn, g.frames_dropped,
                        (double)g.fps_num, g.frames_read / el, drawn / el,
                        (g.frames_read / el) / (g.fps_num > 0 ? g.fps_num : 25));
                fclose(f);
            }
            LocalFree(argv);
            close_media();
            return 0;
        }

        if (bench > 0) {
            /* Time the renderer itself. Process startup and ffmpeg dwarf a
               single frame, so measuring from outside tells you nothing. */
            render();                                   /* warm the atlas */
            LARGE_INTEGER freq, t0, t1;
            QueryPerformanceFrequency(&freq);
            QueryPerformanceCounter(&t0);
            for (int i = 0; i < bench; i++) render();
            QueryPerformanceCounter(&t1);
            double ms = 1000.0 * (t1.QuadPart - t0.QuadPart) / freq.QuadPart / bench;
            FILE *f = _wfopen(out, L"w");
            if (f) {
                fprintf(f, "%dx%d canvas, %dpx cells, %dx%d cells, %ls\n"
                           "%.2f ms/frame\n%.1f fps\n",
                        g.dib_w, g.dib_h, g.cell_h, g.cols, g.rows, mode_name(),
                        ms, 1000.0 / ms);
                fclose(f);
            }
            LocalFree(argv);
            close_media();
            return 0;
        }

        render();
        int ok = write_bmp(out);
        LocalFree(argv);
        close_media();
        return ok ? 0 : 3;
    }

    if (infile) {
        argv[1] = (LPWSTR)infile;
        argc = 2;
    }
    if (argc > 1) {
        if (!open_media(argv[1], 0))
            MessageBoxW(g.hwnd, L"Could not decode that file.\n\n"
                                L"ffmpeg and ffprobe must be on PATH.",
                        L"asciiapp", MB_OK | MB_ICONWARNING);
    }
    LocalFree(argv);

    ShowWindow(g.hwnd, show);
    UpdateWindow(g.hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    close_media();
    return 0;
}
