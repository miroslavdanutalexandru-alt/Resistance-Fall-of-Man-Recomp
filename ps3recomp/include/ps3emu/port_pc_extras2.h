/*
 * port_pc_extras2.h -- pure logic of the second wave of PC-port features (shared, identical in R1 / R2 / R3):
 *   1. benchmark statistics (average, 1% / 0.1% low, percentiles, stutter count) and the capture hotkey name,
 *   2. extra-crosshair colour and size,
 *   3. Steam Deck detection from the board / product name (Wine fills the BIOS registry from DMI),
 *   4. "remember window position": the window_pos.txt line and the on-screen check.
 * No Windows or C++ dependencies: tests/test_pc_extras2.c checks all of it.
 */
#ifndef PORT_PC_EXTRAS2_H
#define PORT_PC_EXTRAS2_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static inline int pcx2_ieq(const char* a, const char* b)
{
    if (!a || !b) return 0;
    while (*a && *b) { if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0; a++; b++; }
    return *a == *b;
}

/* ---- 1. benchmark ---------------------------------------------------------------------------------------- */
typedef struct {
    unsigned frames, stutters;
    double secs, avg_fps, low1_fps, low01_fps, median_ms, p99_ms, max_ms;
} PcxBenchStats;

/* A stutter is a sudden hitch: a frame that took more than twice the average of the (up to) 30 frames before it
 * and at least 4 ms longer than that average. A run of such frames (a step to a slower scene) counts once. */
#define PCX_STUTTER_WIN 30
static inline unsigned pcx_bench_stutters(const float* ft, unsigned n)
{
    unsigned s = 0;
    int prev = 0;
    double sum = 0;
    for (unsigned i = 0; i < n; i++) {
        const unsigned k = i < PCX_STUTTER_WIN ? i : PCX_STUTTER_WIN;
        int hit = 0;
        if (k >= 5) {
            const double avg = sum / k;
            hit = ft[i] > 2.0 * avg && ft[i] - avg >= 4.0;
        }
        if (hit && !prev) s++;
        prev = hit;
        sum += ft[i];
        if (i >= PCX_STUTTER_WIN) sum -= ft[i - PCX_STUTTER_WIN];
    }
    return s;
}
static inline int pcx2_fcmp(const void* a, const void* b)
{
    const float x = *(const float*)a, y = *(const float*)b;
    return x < y ? -1 : x > y;
}
/* ft: frame times in ms in capture order; scratch: room for n floats (sorted copy); secs: wall time of the capture
 * (<= 0: the sum of the frame times). "1% low" = the average fps over the slowest 1% of frames (at least one). */
static inline void pcx_bench_stats(const float* ft, unsigned n, double secs, float* scratch, PcxBenchStats* o)
{
    memset(o, 0, sizeof *o);
    o->frames = n;
    if (!n) return;
    double sum = 0;
    for (unsigned i = 0; i < n; i++) { scratch[i] = ft[i]; sum += ft[i]; }
    o->secs = secs > 0 ? secs : sum / 1000.0;
    qsort(scratch, n, sizeof(float), pcx2_fcmp);
    o->avg_fps = o->secs > 0 ? n / o->secs : 0;
    o->median_ms = scratch[(n - 1) / 2];
    o->p99_ms = scratch[(unsigned)((n - 1) * 0.99)];
    o->max_ms = scratch[n - 1];
    for (int pass = 0; pass < 2; pass++) {
        unsigned k = (unsigned)(n * (pass ? 0.001 : 0.01));
        if (k < 1) k = 1;
        double s = 0;
        for (unsigned i = n - k; i < n; i++) s += scratch[i];
        const double fps = s > 0 ? 1000.0 * k / s : 0;
        if (pass) o->low01_fps = fps; else o->low1_fps = fps;
    }
    o->stutters = pcx_bench_stutters(ft, n);
}

/* The capture hotkey: "" / NULL = F11, "off" / "0" = none, "F1".."F24" = that key. Returns the Windows
 * virtual-key code (VK_F1 = 0x70) or 0. */
static inline int pcx_bench_key(const char* s)
{
    if (!s || !*s) return 0x7A;   /* VK_F11 */
    if (pcx2_ieq(s, "off") || !strcmp(s, "0")) return 0;
    if ((s[0] == 'F' || s[0] == 'f') && isdigit((unsigned char)s[1])) {
        const int n = atoi(s + 1);
        if (n >= 1 && n <= 24) return 0x70 + n - 1;
    }
    return 0x7A;
}

/* ---- 2. extra crosshair ------------------------------------------------------------------------------------ */
/* colour name -> 0xRRGGBB. Never the overlay's colour key (pure magenta 0xFF00FF). Default white. */
static inline unsigned pcx_xhair_rgb(const char* s)
{
    static const struct { const char* n; unsigned rgb; } t[] = {
        { "white", 0xF0F0F0 }, { "green", 0x00FF40 }, { "yellow", 0xFFFF00 }, { "cyan", 0x00FFFF },
        { "red", 0xFF2020 }, { "pink", 0xFF60C0 }, { "orange", 0xFF9000 } };
    if (s) for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (pcx2_ieq(s, t[i].n)) return t[i].rgb;
    return 0xF0F0F0;
}
/* size name -> scale in percent of the original 24 px marker */
static inline int pcx_xhair_scale(const char* s)
{
    if (!s || !*s || pcx2_ieq(s, "small")) return 100;
    if (pcx2_ieq(s, "medium")) return 150;
    if (pcx2_ieq(s, "large")) return 200;
    if (pcx2_ieq(s, "huge")) return 300;
    { const int p = atoi(s); if (p >= 50 && p <= 400) return p; }
    return 100;
}

/* ---- 3. Steam Deck ------------------------------------------------------------------------------------------ */
/* DMI board / product names of the Steam Deck: "Jupiter" (LCD), "Galileo" (OLED); Valve as the vendor. */
static inline int pcx_is_deck_board(const char* product)
{
    if (!product) return 0;
    return pcx2_ieq(product, "Jupiter") || pcx2_ieq(product, "Galileo");
}

/* ---- 4. remember window position ---------------------------------------------------------------------------- */
/* window_pos.txt: "x y w h base", base = the RSX_WINDOW the size belongs to ("-" when none). w / h are the outer
 * window size. A size saved for another RSX_WINDOW is not reused (the launcher's Window size wins), only the place. */
typedef struct { int x, y, w, h; char base[32]; } PcxWinPos;
static inline int pcx_winpos_parse(const char* line, PcxWinPos* p)
{
    memset(p, 0, sizeof *p);
    if (!line) return 0;
    if (sscanf(line, "%d %d %d %d %31s", &p->x, &p->y, &p->w, &p->h, p->base) < 4) return 0;
    if (!p->base[0]) { p->base[0] = '-'; p->base[1] = 0; }
    return p->w >= 160 && p->h >= 120 && p->w <= 32768 && p->h <= 32768;
}
static inline void pcx_winpos_format(const PcxWinPos* p, char* out, size_t n)
{
    snprintf(out, n, "%d %d %d %d %s\n", p->x, p->y, p->w, p->h, p->base[0] ? p->base : "-");
}
/* base matches the current RSX_WINDOW (NULL / "" = "-") */
static inline int pcx_winpos_same_base(const PcxWinPos* p, const char* rsx_window)
{
    const char* b = rsx_window && *rsx_window ? rsx_window : "-";
    return pcx2_ieq(p->base, b);
}
/* mons: n rectangles {left, top, right, bottom} (monitor work areas). The window may be used when at least
 * 120 x 40 px of its top edge (the title bar) is on one of them, so it can always be grabbed and moved. */
static inline int pcx_winpos_visible(const PcxWinPos* p, const int (*mons)[4], int n)
{
    for (int i = 0; i < n; i++) {
        const int l = p->x > mons[i][0] ? p->x : mons[i][0];
        const int r = p->x + p->w < mons[i][2] ? p->x + p->w : mons[i][2];
        const int t = p->y > mons[i][1] ? p->y : mons[i][1];
        const int b = p->y + 40 < mons[i][3] ? p->y + 40 : mons[i][3];
        if (r - l >= 120 && b - t >= 40) return 1;
    }
    return 0;
}

/* Fit a restored window rect inside a monitor work area {l,t,r,b}: shrink to the
 * work area if it is bigger, then shift so every edge is on screen. */
static inline void pcx_winpos_fit(PcxWinPos* p, const int work[4])
{
    const int ww = work[2] - work[0], wh = work[3] - work[1];
    if (ww <= 0 || wh <= 0) return;
    if (p->w > ww) p->w = ww;
    if (p->h > wh) p->h = wh;
    if (p->x + p->w > work[2]) p->x = work[2] - p->w;
    if (p->y + p->h > work[3]) p->y = work[3] - p->h;
    if (p->x < work[0]) p->x = work[0];
    if (p->y < work[1]) p->y = work[1];
}

/* Level switch (rfom_switch.cpp's black cover window over the old/new process).
 * The new game window counts as in the background (focus pause) only when the
 * user really left it: not while the switch cover holds the foreground. A
 * paused game does not flip, the cover waits for flips, and only the cover hands
 * the foreground back -> a black screen until the 90 s timeout. */
static inline int pcx_focus_background(int have_window, int app_active, int iconic, int fg_is_switch_cover)
{
    if (!have_window) return 0;
    if (iconic) return 1;
    return !app_active && !fg_is_switch_cover;
}

/* New process side of the switch: release the cover after two presented
 * frames, at once if the game is paused (it would never flip), or on timeout. */
static inline int pcx_cover_release_due(unsigned flips_since_start, int paused, unsigned long long elapsed_ms,
                                        unsigned long long timeout_ms)
{
    return flips_since_start >= 2 || paused || elapsed_ms >= timeout_ms;
}

#endif /* PORT_PC_EXTRAS2_H */
