/* port_display_sync.h -- "Display sync": is VRR (G-Sync / FreeSync) really following the game's
 * presents, and if not, pace so every shown frame stays on screen for the same number of refreshes.
 * Shared by the Resistance ports (R1: ps3recomp/libs/video/rsx_d3d12_backend.c; R2/R3: the RPCS3 Vulkan
 * renderer, rpcs3/Emu/RSX/VK/VKPresent.cpp) -- keep the copies identical. Header-only; include it in ONE source file
 * (the Win32 part keeps its state in statics). Tests: project/tests/display_sync_test.c.
 *
 * Why: a 120 frames/s stream (60 rendered + 60 generated) or a 60 presents/s movie on a 165 Hz screen
 * whose VRR is not active (hybrid laptop driving the panel through the iGPU, G-Sync set to "full screen
 * only" with a borderless window, VRR off in the monitor's menu...) is shown for 1 or 2 (resp. 2 or 3)
 * refreshes per frame in an uneven pattern: judder and visible duplicates, although the game's own pacing
 * is perfect.
 *
 * Detection (pds_window_verdict): a background thread waits for the display's vertical blanks
 * (D3DKMTWaitForVerticalBlankEvent on the adapter / output that shows the game window; API-independent,
 * so the same for D3D12 and Vulkan) and timestamps them. On a fixed refresh the vblanks come every
 * refresh period whatever the game does; with VRR active the screen waits for the next frame, so the
 * vblanks follow the frames (8.3 ms apart for 120 frames/s on a 165 Hz screen, not 6.06). Over 2 s
 * windows: >= 70% of the vblank gaps within 6% of the nominal period -> FIXED ("VRR effective: no");
 * <= 20% there and the median gap >= 1.12 periods -> VRR ("yes"). It can only tell while the game shows
 * fewer frames than the refresh rate (< 90% of it): at or above, both look the same (SATURATED, "can't
 * tell"). Fewer than 20 frames/s (paused, loading) = IDLE. Windows while the game window is not in the
 * foreground or minimized (the desktop composes the screen then, which turns VRR off) don't count, nor
 * the window after one (pds_thread). Auto mode switches to fixed-refresh pacing after 3 FIXED windows in
 * a row and back after 2 VRR windows in a row; any other verdict between breaks the run (pds_decide), so a
 * VRR user near the refresh rate (SATURATED) is not switched by stray FIXED windows. At most
 * PDS_MAX_SWITCHES changes per run.
 *
 * Pacing on a fixed refresh (pds_make_plan): the shown frame rate becomes refresh / k for the smallest
 * k at which the rendered rate (shown / frame-generation multiplier) is at most the game's limit
 * (pds_max_real: the frame-rate setting; 60 allows up to 75 so 144 Hz gets 72), every frame shown for
 * exactly k refreshes: without frame generation vsync with sync interval k and the frame limiter a hair
 * faster (the vsync wait sets the exact pace); with frame generation (DLSS-G / FSR 3 accept only sync
 * interval 1) vsync interval 1 and the rendered rate limited to refresh / (k * multiplier) exactly
 * (a hair faster when k = 1). Examples (frame-rate setting 60):
 *   165 Hz: no FG 55 fps (interval 3); FG 2x 41.25 rendered -> 82.5 shown.
 *   144 Hz: no FG 72 (interval 2);     FG 2x 72 -> 144.
 *   120 Hz: no FG 60 (interval 2);     FG 2x 60 -> 120.
 *   240 Hz: no FG 60 (interval 4);     FG 2x 60 -> 120 (every 2nd refresh).
 *    60 Hz: no FG 60 (interval 1);     FG 2x 30 -> 60.
 * No plan (no pacing, the frame limit stays the player's) when even k = 1 would render fewer than 30
 * frames/s, where the games slow down (60 Hz + FG 3x = 20, 50 Hz + FG 2x = 25).
 * A PC that can't hold the plan steps to k + 1 (144 Hz: 72 -> 48 rather than a steady 36) and tries the
 * faster plan again later (pds_adapt_frame, PORT_PACING_ADAPT; never faster than the setting allows).
 * Movies (30 fps): vsync interval 1 -- even on 60/120/240 Hz; on 144 / 165 Hz the pattern alternates
 * 5/5/5/5/4 resp. 5/6 refreshes, the least judder a fixed 144 / 165 Hz screen can show for 29.97 fps. */
#ifndef PORT_DISPLAY_SYNC_H
#define PORT_DISPLAY_SYNC_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PDS_V_UNKNOWN = 0, PDS_V_VRR = 1, PDS_V_FIXED = 2, PDS_V_SATURATED = 3, PDS_V_IDLE = 4 };
enum { PDS_MODE_AUTO = 0, PDS_MODE_VRR = 1, PDS_MODE_FIXED = 2, PDS_MODE_OFF = 3 };
#define PDS_WINDOW_MS 2000.0
#define PDS_MAX_SWITCHES 6
#define PDS_RUN_TO_FIXED 3
#define PDS_RUN_TO_VRR 2
#define PDS_MAX_IV 2048

static inline const char* pds_verdict_name(int v)
{
    return v == PDS_V_VRR ? "VRR" : v == PDS_V_FIXED ? "fixed refresh" : v == PDS_V_SATURATED ? "at refresh rate"
         : v == PDS_V_IDLE ? "idle" : "unknown";
}
static inline const char* pds_mode_name(int m)
{
    return m == PDS_MODE_VRR ? "vrr" : m == PDS_MODE_FIXED ? "fixed" : m == PDS_MODE_OFF ? "off" : "auto";
}

/* auto (unset / empty / "auto"), vrr ("vrr", "gsync", "freesync"), fixed ("fixed", "vsync", "on"), off
 * ("off", "0": like vrr, and no measuring thread either). */
static inline int pds_parse_mode(const char* e)
{
    if (!e || !*e) return PDS_MODE_AUTO;
    char b[16]; size_t i = 0;
    for (; e[i] && i < sizeof b - 1; i++) b[i] = (char)((e[i] >= 'A' && e[i] <= 'Z') ? e[i] + 32 : e[i]);
    b[i] = 0;
    if (!strcmp(b, "vrr") || !strcmp(b, "gsync") || !strcmp(b, "g-sync") || !strcmp(b, "freesync")) return PDS_MODE_VRR;
    if (!strcmp(b, "fixed") || !strcmp(b, "vsync") || !strcmp(b, "on") || !strcmp(b, "1")) return PDS_MODE_FIXED;
    if (!strcmp(b, "off") || !strcmp(b, "0")) return PDS_MODE_OFF;
    return PDS_MODE_AUTO;
}

/* One measuring window. iv = gaps between consecutive vblanks (ms), n of them, over win_ms;
 * per_ms = the display mode's refresh period; present_hz = frames shown per second over the window
 * (generated frames included). *fixed_per_ms (may be NULL) = the measured refresh period for FIXED
 * (mean of the gaps within 6% of per_ms), else left alone. */
static inline int pds_window_verdict(const float* iv, int n, double win_ms, double per_ms, double present_hz,
                                     double* fixed_per_ms)
{
    if (!(per_ms > 0.0) || !(win_ms > 0.0)) return PDS_V_UNKNOWN;
    if (present_hz < 20.0) return PDS_V_IDLE;
    if (present_hz >= 0.9 * 1000.0 / per_ms) return PDS_V_SATURATED;
    if (n < 8 || n < (int)(win_ms / (per_ms * 8.0))) return PDS_V_UNKNOWN;   /* the vblank waits are not coming back */
    int near1 = 0, below = 0; double sum = 0.0;
    for (int i = 0; i < n; i++) {
        const double d = iv[i];
        if (d >= per_ms * 0.94 && d <= per_ms * 1.06) { near1++; sum += d; }
        if (d < per_ms * 1.12) below++;
    }
    if (near1 >= n * 0.7) {
        if (fixed_per_ms) *fixed_per_ms = sum / near1;
        return PDS_V_FIXED;
    }
    /* median >= 1.12 periods <=> fewer than half the gaps are below it */
    if (near1 <= n * 0.2 && below * 2 < n) return PDS_V_VRR;
    return PDS_V_UNKNOWN;
}

/* Hysteresis: to fixed-refresh pacing after PDS_RUN_TO_FIXED FIXED windows in a row, back after
 * PDS_RUN_TO_VRR VRR windows in a row; any other verdict (SATURATED, IDLE, UNKNOWN: not foreground...)
 * breaks the run. At most PDS_MAX_SWITCHES changes. Returns 1 when ->fixed changed. */
typedef struct { int mode, fixed, last, streak, switches; } pds_decider;
static inline void pds_decider_init(pds_decider* d, int mode)
{
    memset(d, 0, sizeof *d);
    d->mode = mode;
    d->fixed = mode == PDS_MODE_FIXED;
}
static inline int pds_decide(pds_decider* d, int verdict)
{
    if (d->mode != PDS_MODE_AUTO) return 0;
    const int counts = verdict == PDS_V_VRR || verdict == PDS_V_FIXED;
    d->streak = !counts ? 0 : verdict == d->last ? d->streak + 1 : 1;
    d->last = verdict;
    if (!counts) return 0;
    const int want = verdict == PDS_V_FIXED;
    if (d->streak >= (want ? PDS_RUN_TO_FIXED : PDS_RUN_TO_VRR) && want != d->fixed && d->switches < PDS_MAX_SWITCHES) {
        d->fixed = want; d->switches++; d->streak = 0;
        return 1;
    }
    return 0;
}

/* The highest rendered frame rate fixed-refresh pacing may pick, from the game's frame-rate setting
 * (fps; <= 0 = uncapped): below 60 that setting, 60-75 up to 75 (refresh / k rarely lands on 60:
 * 144 Hz -> 72), above 75 the setting (the player asked for it). */
static inline double pds_max_real(double fps)
{
    if (!(fps > 0.0)) return 1000.0;
    return fps < 59.5 ? fps : fps < 75.0 ? 75.0 : fps;
}

typedef struct {
    int k;            /* refreshes per shown frame */
    int sync;         /* present sync interval: k without frame generation, 1 with it */
    int mult;         /* frames shown per rendered frame (1 = no frame generation) */
    double hz;        /* the refresh rate it was made for */
    double shown;     /* frames shown per second: hz / k */
    double real;      /* frames rendered per second: shown / mult */
    double limiter;   /* the rendered-frame limit to set (real, a hair faster when vsync sets the pace) */
} pds_plan;

/* plan for exactly k refreshes per shown frame */
static inline void pds_plan_fill(double hz, int mult, int k, pds_plan* p)
{
    p->mult = mult; p->hz = hz;
    p->k = k;
    p->sync = mult > 1 ? 1 : k;
    p->shown = hz / k;
    p->real = p->shown / mult;
    /* vsync waits set the pace when every present is one interval: no frame generation (interval k), or
     * frame generation showing every refresh. Otherwise (generated frames every k-th refresh at
     * interval 1) the limiter must hold the exact rate or frames slip to k-1 refreshes. */
    p->limiter = (mult == 1 || k == 1) ? p->real * 1.003 : p->real;
}
/* hz = refresh rate (measured), mult = frame-generation multiplier (1 = off), max_real = pds_max_real().
 * The smallest k whose rendered rate is <= max_real; if that falls below min_real (where the game
 * slows down), one k less when that exists. Still below min_real (k = 1: hz / mult < min_real, e.g.
 * 60 Hz + FG 3x) -> no plan: k = 0 (hz and mult filled in for the log), the caller doesn't pace. */
static inline void pds_make_plan(double hz, int mult, double max_real, double min_real, pds_plan* p)
{
    memset(p, 0, sizeof *p);
    if (mult < 1) mult = 1;
    if (!(hz > 1.0)) return;
    int k = 1;
    while (k < 16 && hz / k / mult > max_real + 0.01) k++;
    if (k > 1 && hz / k / mult < min_real - 0.01) k--;
    p->mult = mult; p->hz = hz;
    if (hz / k / mult < min_real - 0.01) return;
    pds_plan_fill(hz, mult, k, p);
}

static inline int pds_plan_same(const pds_plan* a, const pds_plan* b)
{
    return a->k == b->k && a->sync == b->sync && a->mult == b->mult && fabs(a->hz - b->hz) < 0.05;
}

/* "[display] fixed-refresh pacing: 165.00 Hz, frame generation 2x: 41.25 fps rendered -> 82.50 shown,
 * each frame 2 refreshes (vsync interval 1)" */
static inline void pds_plan_str(const pds_plan* p, char* out, size_t n)
{
    if (p->k < 1)
        snprintf(out, n, "%.2f Hz, frame generation %dx: even every refresh would be only %.2f fps rendered, too slow for "
                         "the game -- not paced (frame limit and vsync as set)", p->hz, p->mult, p->mult > 0 ? p->hz / p->mult : 0.0);
    else if (p->mult > 1)
        snprintf(out, n, "%.2f Hz, frame generation %dx: %.2f fps rendered -> %.2f shown, each frame %d refresh%s (vsync interval %d)",
                 p->hz, p->mult, p->real, p->shown, p->k, p->k == 1 ? "" : "es", p->sync);
    else
        snprintf(out, n, "%.2f Hz: %.2f fps, each frame %d refresh%s (vsync interval %d)",
                 p->hz, p->real, p->k, p->k == 1 ? "" : "es", p->sync);
}

/* ---- Adaptive step-down (PORT_PACING_ADAPT, default on; 0 = always the plan above) ---------------------
 * The plan above comes from the frame-rate setting alone. A PC that can't hold it then shows every late
 * frame for one more k (144 Hz, setting 60: 72 fps planned, a PC doing 40-70 gets a steady 36 -- 4
 * refreshes a frame -- although 48, 3 refreshes, would hold). So, while fixed-refresh pacing runs:
 *  - a frame "misses its slot" when it comes PDS_AD_LATE refreshes or more after the plan period (sync
 *    interval k shows a late frame k refreshes later, Vulkan FIFO one refresh later);
 *  - more than PDS_AD_SLOW_FRAC of the frames of the last PDS_AD_WIN_MS missing -> k + 1 (72 -> 48 at
 *    144 Hz, 55 -> 41.25 at 165 Hz), again if still missing, never below min_real (30);
 *  - after PDS_AD_HOLD_MS with no window above PDS_AD_CLEAN_FRAC misses at the slower plan, the faster
 *    one is tried; if more than PDS_AD_PROBE_FAIL of its frames miss within PDS_AD_PROBE_MS (checked from
 *    PDS_AD_PROBE_MIN_MS on), back at once, and the next try waits twice as long (up to
 *    PDS_AD_HOLD_MAX_MS; also when the faster plan fails within PDS_AD_FAST_OK_MS of being taken). The
 *    present timing can't tell how much headroom a frame had (vsync hides it), so the faster plan is
 *    tried rather than predicted.
 *  - never faster than the plan above (the player's limit); gaps of PDS_AD_STALL_MS or more (loading,
 *    hangs, pause -- port_frametime.h's stall) restart the window; single frames over PDS_AD_HITCH_MS
 *    (hitches, a loading screen's slow frames) don't count, nor do windows under PDS_AD_MIN_FPS frames/s.
 *    Not with VRR or without fixed-refresh pacing. */
#define PDS_AD_WIN_MS 2000.0
#define PDS_AD_STALL_MS 1000.0
#define PDS_AD_HITCH_MS 100.0
#define PDS_AD_LATE 0.5
#define PDS_AD_SLOW_FRAC 0.20
#define PDS_AD_CLEAN_FRAC 0.02
#define PDS_AD_HOLD_MS 10000.0
#define PDS_AD_HOLD_MAX_MS 120000.0
#define PDS_AD_PROBE_MS 3000.0
#define PDS_AD_PROBE_MIN_MS 500.0
#define PDS_AD_PROBE_FAIL 0.25
#define PDS_AD_FAST_OK_MS 30000.0
#define PDS_AD_MIN_FPS 20.0
#define PDS_AD_MAX_EXTRA 4
#define PDS_AD_RING 1024
enum { PDS_AD_NONE = 0, PDS_AD_SLOWER = 1, PDS_AD_TRY_FASTER = 2, PDS_AD_BACK = 3 };

/* PORT_PACING_ADAPT: unset / empty / anything else = on; "0", "off", "no", "false" = off */
static inline int pds_parse_adapt(const char* e)
{
    if (!e || !*e) return 1;
    char b[8]; size_t i = 0;
    for (; e[i] && i < sizeof b - 1; i++) b[i] = (char)((e[i] >= 'A' && e[i] <= 'Z') ? e[i] + 32 : e[i]);
    b[i] = 0;
    return !(!strcmp(b, "0") || !strcmp(b, "off") || !strcmp(b, "no") || !strcmp(b, "false"));
}

/* The plan made slower by `extra` refreshes per frame (0 = pds_make_plan's), as far as min_real allows.
 * Returns the extra steps applied. No plan (k = 0) stays no plan. */
static inline int pds_make_plan_step(double hz, int mult, double max_real, double min_real, int extra, pds_plan* p)
{
    pds_make_plan(hz, mult, max_real, min_real, p);
    if (p->k < 1 || extra <= 0) return 0;
    int k = p->k + (extra > PDS_AD_MAX_EXTRA ? PDS_AD_MAX_EXTRA : extra);
    while (k > p->k && hz / k / p->mult < min_real - 0.01) k--;
    const int got = k - p->k;
    if (got) pds_plan_fill(hz, p->mult, k, p);
    return got;
}
static inline int pds_plan_max_extra(double hz, int mult, double max_real, double min_real)
{
    pds_plan p;
    return pds_make_plan_step(hz, mult, max_real, min_real, PDS_AD_MAX_EXTRA, &p);
}

typedef struct {
    int enabled;
    int extra;           /* refreshes per frame added to the plan (0 = the player's) */
    int probing;         /* trying the faster plan */
    int why;             /* PDS_AD_* of the last change */
    double frac;         /* misses (0..1) behind the last change */
    double hold;         /* clean time at the slower plan before the next try (ms) */
    double last;         /* previous frame (ms), < 0 = none */
    double win0;         /* window (re)started, < 0 = at the next frame */
    double clean0;       /* misses last seen / window restarted */
    double probe0;       /* the try started */
    double fast_t;       /* last step to a faster plan, < 0 = none */
    int pn, pmiss;       /* frames / misses since the try started */
    double rt[PDS_AD_RING];
    unsigned char rm[PDS_AD_RING];
    int head, n, nmiss;  /* ring of the frames in the window: oldest at head */
} pds_adapt;

static inline void pds_adapt_restart(pds_adapt* a)
{
    a->head = a->n = a->nmiss = 0;
    a->last = -1.0; a->win0 = -1.0;
}
static inline void pds_adapt_init(pds_adapt* a, int enabled)
{
    memset(a, 0, sizeof *a);
    a->enabled = enabled;
    a->hold = PDS_AD_HOLD_MS;
    a->fast_t = -1.0;
    pds_adapt_restart(a);
}
/* Back to the player's plan (pacing off, frame generation switched...). */
static inline void pds_adapt_reset(pds_adapt* a) { pds_adapt_init(a, a->enabled); }

/* Once a rendered frame while a plan (k >= 1) paces: now = its present time (ms), period_ms = the plan's
 * rendered-frame period (1000 / real), refresh_ms = 1000 / hz, max_extra = pds_plan_max_extra. Returns 1 when a->extra changed
 * (a->why / a->frac say why): make the plan again with pds_make_plan_step(..., a->extra, ...). */
static inline int pds_adapt_frame(pds_adapt* a, double now, double period_ms, double refresh_ms, int max_extra)
{
    if (!a->enabled || !(period_ms > 0.0) || !(refresh_ms > 0.0)) return 0;
    if (max_extra < 0) max_extra = 0;
    if (a->extra > max_extra) {   /* the refresh rate / limit changed under it */
        a->extra = max_extra; a->probing = 0; a->why = PDS_AD_NONE; a->frac = 0.0;
        pds_adapt_restart(a);
        return 1;
    }
    if (a->win0 < 0.0) { a->win0 = a->clean0 = now; if (a->probing) { a->probe0 = now; a->pn = a->pmiss = 0; } }
    if (a->last < 0.0) { a->last = now; return 0; }
    const double dt = now - a->last;
    a->last = now;
    if (dt >= PDS_AD_STALL_MS || dt < 0.0) {   /* loading / hang / pause: nothing before it counts */
        a->head = a->n = a->nmiss = 0;
        a->win0 = a->clean0 = now;
        if (a->probing) { a->probe0 = now; a->pn = a->pmiss = 0; }
        return 0;
    }
    if (dt > PDS_AD_HITCH_MS) return 0;   /* a hitch (shader compile, streaming) or a loading screen's frame: no frame rate */
    const int miss = dt > period_ms + PDS_AD_LATE * refresh_ms;
    if (a->n == PDS_AD_RING) { a->nmiss -= a->rm[a->head]; a->head = (a->head + 1) % PDS_AD_RING; a->n--; }
    { const int i = (a->head + a->n) % PDS_AD_RING; a->rt[i] = now; a->rm[i] = (unsigned char)miss; a->n++; a->nmiss += miss; }
    while (a->n && a->rt[a->head] < now - PDS_AD_WIN_MS) { a->nmiss -= a->rm[a->head]; a->head = (a->head + 1) % PDS_AD_RING; a->n--; }

    if (a->probing) {
        a->pn++; a->pmiss += miss;
        if (now - a->probe0 >= PDS_AD_PROBE_MIN_MS && a->pn >= 10 && a->pmiss > PDS_AD_PROBE_FAIL * a->pn) {
            a->frac = (double)a->pmiss / a->pn;
            a->extra++; a->probing = 0; a->why = PDS_AD_BACK;
            a->hold = a->hold * 2.0 > PDS_AD_HOLD_MAX_MS ? PDS_AD_HOLD_MAX_MS : a->hold * 2.0;
            pds_adapt_restart(a);
            return 1;
        }
        if (now - a->probe0 < PDS_AD_PROBE_MS) return 0;
        a->probing = 0;   /* held: the normal window from here */
    }
    if (now - a->win0 < PDS_AD_WIN_MS || a->n <= 0) return 0;
    if (a->n * 1000.0 / PDS_AD_WIN_MS < PDS_AD_MIN_FPS) { a->clean0 = now; return 0; }   /* loading screen */
    const double frac = (double)a->nmiss / a->n;
    if (frac > PDS_AD_SLOW_FRAC) {
        a->clean0 = now;
        if (a->extra >= max_extra) return 0;
        /* the faster plan failed soon after it was taken: wait longer before the next try */
        if (a->fast_t >= 0.0 && now - a->fast_t < PDS_AD_FAST_OK_MS)
            a->hold = a->hold * 2.0 > PDS_AD_HOLD_MAX_MS ? PDS_AD_HOLD_MAX_MS : a->hold * 2.0;
        else a->hold = PDS_AD_HOLD_MS;
        a->extra++; a->why = PDS_AD_SLOWER; a->frac = frac;
        pds_adapt_restart(a);
        return 1;
    }
    if (frac > PDS_AD_CLEAN_FRAC) { a->clean0 = now; return 0; }
    if (a->extra > 0 && now - a->clean0 >= a->hold) {
        a->extra--; a->probing = 1; a->why = PDS_AD_TRY_FASTER; a->frac = frac; a->fast_t = now;
        pds_adapt_restart(a);
        return 1;
    }
    return 0;
}

/* The log line for a change: p = the new plan, was = the plan before.
 * "stepping to 48.00 fps (k=3) -- 72.00 fps missed (65% of frames over 2 s)" */
static inline void pds_adapt_str(const pds_adapt* a, const pds_plan* p, const pds_plan* was, char* out, size_t n)
{
    char r[64], w[64];
    if (p->mult > 1) snprintf(r, sizeof r, "%.2f fps rendered (%.2f shown, k=%d)", p->real, p->shown, p->k);
    else snprintf(r, sizeof r, "%.2f fps (k=%d)", p->real, p->k);
    snprintf(w, sizeof w, "%.2f fps", was->real);
    if (a->why == PDS_AD_SLOWER)
        snprintf(out, n, "stepping to %s -- %s missed (%.0f%% of frames over %.0f s); PORT_PACING_ADAPT=0 keeps it",
                 r, w, a->frac * 100.0, PDS_AD_WIN_MS / 1000.0);
    else if (a->why == PDS_AD_TRY_FASTER)
        snprintf(out, n, "trying %s again (%.0f s without misses)", r, a->hold / 1000.0);
    else if (a->why == PDS_AD_BACK)
        snprintf(out, n, "stepping to %s -- %s still misses (%.0f%%); next try in %.0f s", r, w, a->frac * 100.0, a->hold / 1000.0);
    else
        snprintf(out, n, "stepping to %s", r);
}

/* Where in the refresh period a present came, as the phase lock's error: -0.5 .. 0.5 periods, 0 = mid-refresh. */
static inline double pds_phase_err(double since_vblank_ms, double per_ms)
{
    double ph = fmod(since_vblank_ms, per_ms) / per_ms - 0.5;   /* -0.5 .. 0.5: 0 = mid-refresh */
    if (ph < -0.5) ph += 1.0;
    return ph;
}

#if defined(_WIN32) && !defined(PDS_LOGIC_ONLY)   /* PDS_LOGIC_ONLY: the host test */
#include <windows.h>

/* d3dkmthk.h is WDK-only: the three calls, by hand (gdi32 exports). */
typedef struct { WCHAR DeviceName[32]; UINT hAdapter; LUID AdapterLuid; UINT VidPnSourceId; } pds_kmt_open_t;
typedef struct { UINT hAdapter; UINT hDevice; UINT VidPnSourceId; } pds_kmt_wait_t;
typedef struct { UINT hAdapter; } pds_kmt_close_t;
typedef LONG (APIENTRY* pds_kmt_open_fn)(pds_kmt_open_t*);
typedef LONG (APIENTRY* pds_kmt_wait_fn)(const pds_kmt_wait_t*);
typedef LONG (APIENTRY* pds_kmt_close_fn)(const pds_kmt_close_t*);

typedef HWND (*pds_hwnd_fn)(void);
static struct {
    volatile LONG started;
    volatile LONG presents;        /* frames shown (pds_note_presents) */
    volatile LONG fixed;           /* fixed-refresh pacing on */
    volatile LONG verdict;         /* last window's verdict */
    volatile LONG gen;             /* bumped when fixed / hz change */
    volatile double hz_nominal;    /* the display mode's refresh rate (QueryDisplayConfig, exact) */
    volatile double hz_measured;   /* from FIXED windows, 0 = none yet */
    volatile LONGLONG vb_qpc;      /* QPC time of the last vblank seen (the measuring thread) */
    volatile LONG ph_sum, ph_n;    /* present phase errors (1e-4 periods) since pds_pace_correction_ms */
    double qms;                    /* ms per QPC tick */
    int mode;
    char env[48];
    pds_hwnd_fn hwnd;
    pds_decider dec;
} g_pds;

/* Exact refresh rate of the display a GDI device name (\\.\DISPLAY1) shows, from QueryDisplayConfig;
 * EnumDisplaySettings (whole Hz) as the fallback. 0 = unknown. */
static double pds_refresh_of(const WCHAR* gdi_name)
{
    UINT32 np = 0, nm = 0;
    double hz = 0.0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) == ERROR_SUCCESS && np && nm) {
        DISPLAYCONFIG_PATH_INFO* pa = (DISPLAYCONFIG_PATH_INFO*)calloc(np, sizeof *pa);
        DISPLAYCONFIG_MODE_INFO* mo = (DISPLAYCONFIG_MODE_INFO*)calloc(nm, sizeof *mo);
        if (pa && mo && QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, pa, &nm, mo, NULL) == ERROR_SUCCESS) {
            for (UINT32 i = 0; i < np && hz <= 0.0; i++) {
                DISPLAYCONFIG_SOURCE_DEVICE_NAME sn; memset(&sn, 0, sizeof sn);
                sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                sn.header.size = sizeof sn;
                sn.header.adapterId = pa[i].sourceInfo.adapterId;
                sn.header.id = pa[i].sourceInfo.id;
                if (DisplayConfigGetDeviceInfo(&sn.header) != ERROR_SUCCESS || lstrcmpiW(sn.viewGdiDeviceName, gdi_name)) continue;
                const DISPLAYCONFIG_RATIONAL r = pa[i].targetInfo.refreshRate;
                if (r.Denominator && r.Numerator) hz = (double)r.Numerator / (double)r.Denominator;
            }
        }
        free(pa); free(mo);
    }
    if (hz <= 0.0) {
        DEVMODEW dm; memset(&dm, 0, sizeof dm); dm.dmSize = sizeof dm;
        if (EnumDisplaySettingsW(gdi_name, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) hz = dm.dmDisplayFrequency;
    }
    return hz;
}

static DWORD WINAPI pds_thread(LPVOID unused)
{
    (void)unused;
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi) gdi = LoadLibraryW(L"gdi32.dll");
    const pds_kmt_open_fn kopen = gdi ? (pds_kmt_open_fn)(void*)GetProcAddress(gdi, "D3DKMTOpenAdapterFromGdiDisplayName") : NULL;
    const pds_kmt_wait_fn kwait = gdi ? (pds_kmt_wait_fn)(void*)GetProcAddress(gdi, "D3DKMTWaitForVerticalBlankEvent") : NULL;
    const pds_kmt_close_fn kclose = gdi ? (pds_kmt_close_fn)(void*)GetProcAddress(gdi, "D3DKMTCloseAdapter") : NULL;
    if (!kopen || !kwait || !kclose) {
        fprintf(stderr, "[display] VRR detection unavailable (no D3DKMT vblank calls); %s=fixed paces for a fixed refresh\n", g_pds.env);
        return 0;
    }
    static float iv[PDS_MAX_IV];
    LARGE_INTEGER qf; QueryPerformanceFrequency(&qf);
    const double qms = 1000.0 / (double)qf.QuadPart;
    HMONITOR mon = NULL;
    pds_kmt_open_t ad; memset(&ad, 0, sizeof ad);
    int have = 0, ofails = 0, wfails = 0, n = 0, warm = 0, said_unknown = 0, last_logged = -1, away = 0, said_away = 0;
    LONGLONG prev = 0, w0 = 0; LONG p0 = 0;
    for (;;) {
        HWND h = g_pds.hwnd ? g_pds.hwnd() : NULL;
        HMONITOR m = h ? MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST) : MonitorFromWindow(NULL, MONITOR_DEFAULTTOPRIMARY);
        if (m != mon || !have) {
            if (have) { pds_kmt_close_t c; c.hAdapter = ad.hAdapter; kclose(&c); have = 0; }
            MONITORINFOEXW mi; memset(&mi, 0, sizeof mi); mi.cbSize = sizeof mi;
            if (!m || !GetMonitorInfoW(m, (MONITORINFO*)&mi)) { Sleep(500); continue; }
            memset(&ad, 0, sizeof ad);
            lstrcpynW(ad.DeviceName, mi.szDevice, 32);
            const LONG st = kopen(&ad);
            if (st != 0) {
                if (++ofails >= 5) { fprintf(stderr, "[display] VRR detection unavailable (D3DKMTOpenAdapterFromGdiDisplayName 0x%lX)\n", (unsigned long)st); return 0; }
                Sleep(1000); continue;
            }
            have = 1; mon = m; ofails = 0;
            const double hz = pds_refresh_of(mi.szDevice);
            if (fabs(hz - g_pds.hz_nominal) > 0.01) { g_pds.hz_nominal = hz; g_pds.hz_measured = 0.0; InterlockedIncrement(&g_pds.gen); }
            char dn[40]; WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, dn, (int)sizeof dn, NULL, NULL);
            fprintf(stderr, "[display] measuring whether VRR follows the frames on %s (%.2f Hz; %s=%s)\n", dn, hz, g_pds.env, pds_mode_name(g_pds.mode));
            n = 0; prev = 0; w0 = 0; warm = 1;
        }
        pds_kmt_wait_t w; w.hAdapter = ad.hAdapter; w.hDevice = 0; w.VidPnSourceId = ad.VidPnSourceId;
        const LONG st = kwait(&w);
        LARGE_INTEGER t; QueryPerformanceCounter(&t);
        if (st != 0) {
            if (++wfails >= 50) { fprintf(stderr, "[display] VRR detection stopped (D3DKMTWaitForVerticalBlankEvent 0x%lX)\n", (unsigned long)st); return 0; }
            pds_kmt_close_t c; c.hAdapter = ad.hAdapter; kclose(&c);
            Sleep(20); have = 0; continue;   /* reopen (display reconfigured) */
        }
        wfails = 0;
        if (h) {   /* minimized, or another program in front: the desktop composes the screen (no VRR) */
            const HWND fg = GetForegroundWindow();
            DWORD fpid = 0;
            if (fg) GetWindowThreadProcessId(fg, &fpid);
            if (IsIconic(h) || fpid != GetCurrentProcessId()) away = 1;
        }
        if (prev && n < PDS_MAX_IV) iv[n++] = (float)((double)(t.QuadPart - prev) * qms);
        prev = t.QuadPart;
        g_pds.vb_qpc = t.QuadPart;
        if (!w0) { w0 = t.QuadPart; p0 = g_pds.presents; n = 0; continue; }
        const double el = (double)(t.QuadPart - w0) * qms;
        if (el < PDS_WINDOW_MS) continue;
        const LONG p1 = g_pds.presents;
        const double phz = (double)(p1 - p0) * 1000.0 / el;
        const double per = g_pds.hz_nominal > 0.0 ? 1000.0 / g_pds.hz_nominal : 0.0;
        double fper = 0.0;
        const int v = warm || away ? PDS_V_UNKNOWN : pds_window_verdict(iv, n, el, per, phz, &fper);
        if (away && !said_away++)
            fprintf(stderr, "[display] the game window is minimized / not in front: those measuring windows don't count\n");
        if (!warm) {
            int near1 = 0; float med = 0.0f;
            for (int i = 0; i < n; i++) if (iv[i] >= per * 0.94 && iv[i] <= per * 1.06) near1++;
            { float s[PDS_MAX_IV]; memcpy(s, iv, sizeof(float) * (size_t)n);   /* median, for the log */
              for (int i = 1; i < n; i++) { float x = s[i]; int j = i - 1; while (j >= 0 && s[j] > x) { s[j + 1] = s[j]; j--; } s[j + 1] = x; }
              med = n ? s[n / 2] : 0.0f; }
            InterlockedExchange(&g_pds.verdict, v);
            if (v == PDS_V_FIXED && fper > 0.0) {
                const double mh = 1000.0 / fper;
                if (fabs(mh - g_pds.hz_measured) > 0.02) { g_pds.hz_measured = mh; InterlockedIncrement(&g_pds.gen); }
            }
            if ((v == PDS_V_VRR || v == PDS_V_FIXED) && v != last_logged) {
                last_logged = v;
                if (v == PDS_V_FIXED)
                    fprintf(stderr, "[display] VRR effective: no -- the screen refreshes at a fixed %.2f Hz (%d of %d vblanks %.2f ms apart "
                                    "while the game showed %.1f frames/s)\n", 1000.0 / fper, near1, n, fper, phz);
                else
                    fprintf(stderr, "[display] VRR effective: yes -- the refresh follows the frames (median %.2f ms between refreshes, "
                                    "%.1f frames/s, %.2f Hz max)\n", med, phz, g_pds.hz_nominal);
            } else if (v == PDS_V_UNKNOWN && !away && last_logged < 0 && !said_unknown && phz >= 20.0) {
                said_unknown = 1;
                fprintf(stderr, "[display] VRR effective: unclear so far (%d vblanks, %d at %.2f ms, median %.2f ms, %.1f frames/s)\n",
                        n, near1, per, med, phz);
            }
            if (pds_decide(&g_pds.dec, v)) {
                InterlockedExchange(&g_pds.fixed, g_pds.dec.fixed);
                InterlockedIncrement(&g_pds.gen);
                fprintf(stderr, "[display] fixed-refresh pacing %s (%s=auto: %s%s%s)\n", g_pds.dec.fixed ? "ON" : "OFF", g_pds.env,
                        g_pds.dec.fixed ? "VRR is not following the frames; if G-Sync / FreeSync does work here, set " : "VRR follows the frames again",
                        g_pds.dec.fixed ? g_pds.env : "", g_pds.dec.fixed ? "=vrr" : "");
            }
        }
        warm = away; away = 0; n = 0; w0 = t.QuadPart; p0 = p1;   /* the window after one away doesn't count either */
    }
}

/* Once, from the presenting thread. env = the game's setting name (RFOM_DISPLAY_SYNC, R2_DISPLAY_SYNC...);
 * PORT_DISPLAY_SYNC is read when it is unset. hwnd() returns the game window (any thread). */
static void pds_start(const char* env, pds_hwnd_fn hwnd)
{
    if (InterlockedExchange(&g_pds.started, 1)) return;
    const char* e = getenv(env);
    if (!e || !*e) e = getenv("PORT_DISPLAY_SYNC");
    g_pds.mode = pds_parse_mode(e);
    snprintf(g_pds.env, sizeof g_pds.env, "%s", env);
    g_pds.hwnd = hwnd;
    { LARGE_INTEGER qf; QueryPerformanceFrequency(&qf); g_pds.qms = 1000.0 / (double)qf.QuadPart; }
    pds_decider_init(&g_pds.dec, g_pds.mode);
    g_pds.fixed = g_pds.dec.fixed;
    fprintf(stderr, "[display] display sync: %s (%s=%s)%s\n", pds_mode_name(g_pds.mode), env, e && *e ? e : "",
            g_pds.mode == PDS_MODE_AUTO ? " -- VRR presents until the measurement says VRR is not working"
            : g_pds.mode == PDS_MODE_FIXED ? " -- paced for a fixed refresh rate" : "");
    if (g_pds.mode == PDS_MODE_OFF) return;
    HANDLE th = CreateThread(NULL, 0, pds_thread, NULL, 0, NULL);
    if (th) { SetThreadPriority(th, THREAD_PRIORITY_ABOVE_NORMAL); CloseHandle(th); }
}

/* Every present of the game (n = frames that present shows: 1, or the frame-generation multiplier). */
static inline void pds_note_presents(int n) { InterlockedExchangeAdd(&g_pds.presents, n); }
static inline int pds_fixed(void) { return g_pds.fixed != 0; }
static inline int pds_mode(void) { return g_pds.mode; }
static inline LONG pds_gen(void) { return g_pds.gen; }
/* The refresh rate to pace for: measured on a fixed refresh, else the mode's (0 = unknown yet). */
static inline double pds_hz(void)
{
    const double m = g_pds.hz_measured;
    if (m > 0.0 && fabs(m - g_pds.hz_nominal) < g_pds.hz_nominal * 0.02) return m;
    return g_pds.hz_nominal;
}
/* Phase lock for fixed-refresh pacing where nothing else ties the frames to the refresh (Vulkan FIFO, or
 * frame generation showing every k-th refresh): a frame limit of exactly refresh / k still drifts against
 * the real refresh by the measurement error, and every refresh period of drift one frame is shown for
 * k-1 or k+1 refreshes. pds_note_phase at each real present records where in the refresh period it came
 * (from the measuring thread's last vblank); pds_pace_correction_ms, called by the frame limiter once a
 * frame, returns how far to move its next deadline (ms, + = later) so the presents settle mid-refresh,
 * the point farthest from both vblanks. 0 when fixed-refresh pacing is off or nothing was recorded. */
static inline void pds_note_phase(void)
{
    if (!g_pds.fixed) return;
    const LONGLONG vb = g_pds.vb_qpc;
    const double hz = pds_hz();
    if (!vb || !(hz > 1.0) || !(g_pds.qms > 0.0)) return;
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    const double per = 1000.0 / hz, d = (double)(t.QuadPart - vb) * g_pds.qms;
    if (d < 0.0 || d > per * 16.0) return;   /* the measuring thread stalled / stopped */
    InterlockedExchangeAdd(&g_pds.ph_sum, (LONG)(pds_phase_err(d, per) * 10000.0));
    InterlockedIncrement(&g_pds.ph_n);
}
static inline double pds_pace_correction_ms(void)
{
    const LONG n = InterlockedExchange(&g_pds.ph_n, 0);
    const LONG s = InterlockedExchange(&g_pds.ph_sum, 0);
    const double hz = pds_hz();
    if (n <= 0 || !g_pds.fixed || !(hz > 1.0)) return 0.0;
    return -((double)s / (double)n / 10000.0) * (1000.0 / hz) * 0.2;
}
#endif /* _WIN32 && !PDS_LOGIC_ONLY */

#endif /* PORT_DISPLAY_SYNC_H */
