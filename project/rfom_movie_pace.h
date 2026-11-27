/* Movie pacing (30 fps FMV in flip mode 2), shared by the frame clock's vblank lock
 * (project/main.cpp, RFOM_MOVIE_VBLANK_LOCK) and the movie present
 * (ps3recomp/libs/video/rsx_d3d12_backend.c present_blit, RFOM_MOVIE_VSYNC), so both
 * take the same decision: a refresh rate that shows a 59.94 Hz tick (and so a 30 fps
 * frame) as a whole number of refreshes -- 60/120/240/360/480 Hz -- gets the lock plus vsync
 * presents; any other rate (144, 165 Hz...) with tearing presents available is left
 * to VRR (interval 0 + tearing, like gameplay), since vsync there alternates frames
 * between two refresh counts (5/6 at 165 Hz = judder). On that VRR path each movie
 * frame is presented 2x (or more), evenly spaced, to stay inside the VRR range
 * (rfom_movie_lfc_count, RFOM_MOVIE_LFC). Tests: project/tests/movie_pace_test.c. */
#ifndef RFOM_MOVIE_PACE_H
#define RFOM_MOVIE_PACE_H

#define RFOM_MOVIE_TICK_US 16683.0   /* one 59.94 Hz vblank tick */
#define RFOM_MOVIE_LOCK_TOL_US 400.0

/* Refreshes per 59.94 Hz tick (1-8: 60/120/240/300/360/480 Hz...) when k refreshes
 * land within the tolerance of the tick, else 0 (the lock cannot engage). per_us =
 * the display refresh period. */
static inline int rfom_movie_lock_k(double per_us)
{
    if (!(per_us > 0.0)) return 0;
    const int k = (int)(RFOM_MOVIE_TICK_US / per_us + 0.5);
    const double d = k * per_us - RFOM_MOVIE_TICK_US;
    return (k >= 1 && k <= 8 && d > -RFOM_MOVIE_LOCK_TOL_US && d < RFOM_MOVIE_LOCK_TOL_US) ? k : 0;
}

/* Default movie sync interval (RFOM_MOVIE_VSYNC unset, RFOM_FPS not vrr): 0 when the
 * refresh period is known, cannot be locked and tearing presents work (VRR paces the
 * movie); 1 otherwise (60/120/240/360 Hz..., unknown rate, or no tearing support). */
static inline int rfom_movie_vsync_default(double per_us, int tearing)
{
    return (tearing && per_us > 0.0 && !rfom_movie_lock_k(per_us)) ? 0 : 1;
}

/* Vblank lock phase (main.cpp): the synthetic tick is placed lead us after a real
 * vblank and only moved when it drifts more than band us from there. lead +- band
 * stays inside one refresh at any rate: 1500 +- 1000 us up to 375 Hz (60/120/240 Hz
 * unchanged), then 3p/8 +- p/4 = [p/8, 5p/8] (420/480 Hz). */
static inline long long rfom_movie_phase_lead_us(long long p)
{
    const long long s = p * 3 / 8;
    return s < 1500 ? s : 1500;
}
static inline long long rfom_movie_phase_band_us(long long p)
{
    const long long s = p / 4;
    return s < 1000 ? s : 1000;
}

/* Amount to subtract from next_tick (0 = inside the band). since_vblank_us = time
 * from the last real vblank to now, p = refresh period. The shift is the phase
 * error in (-p/2, p/2], never whole periods (a due tick must not be pushed back a
 * refresh). */
static inline long long rfom_movie_phase_shift(long long next_tick, long long now,
                                               long long since_vblank_us, long long p)
{
    if (p <= 0) p = 1;
    const long long ph = ((since_vblank_us % p) + p) % p;
    const long long tgt = now - ph + rfom_movie_phase_lead_us(p);
    long long off = ((next_tick - tgt) % p + p) % p;
    if (off > p / 2) off -= p;
    const long long band = rfom_movie_phase_band_us(p);
    return (off > band || off < -band) ? off : 0;
}

/* Movie low-framerate compensation (RFOM_MOVIE_LFC, rsx_d3d12_backend.c): on the VRR movie
 * path (interval 0 + tearing) a 30 fps movie is below the VRR range of most screens
 * (~48 Hz and up), so without the driver's own LFC the panel falls back to its fixed
 * refresh and shows the frames for uneven refresh counts. Presenting each movie frame
 * k times, evenly spaced, keeps the present rate inside the range. */
#define RFOM_MOVIE_LFC_MIN_HZ 48.0
#define RFOM_MOVIE_LFC_MAX 8
#define RFOM_MOVIE_FRAME_US (1e6 / 29.97)   /* until the movie's own cadence is measured */

/* Presents per movie frame. fps = movie frame rate, hz = display refresh, env = RFOM_MOVIE_LFC
 * (-1 unset = auto, 0 = off, N = N presents per frame, at most RFOM_MOVIE_LFC_MAX).
 * Auto: the smallest k with fps * k >= 48 Hz that the display can still show
 * (fps * k <= hz); 1 (no repeat) when fps or hz is unknown or no k fits. */
static inline int rfom_movie_lfc_count(double fps, double hz, int env)
{
    if (env == 0) return 1;
    if (env > 0) return env > RFOM_MOVIE_LFC_MAX ? RFOM_MOVIE_LFC_MAX : env;
    if (!(fps > 0.0) || !(hz > 0.0)) return 1;
    for (int k = 1; k <= RFOM_MOVIE_LFC_MAX; k++) {
        if (fps * k > hz + 0.5) return 1;
        if (fps * k >= RFOM_MOVIE_LFC_MIN_HZ) return k;
    }
    return 1;
}

/* Movie frame period estimate (us) from the gap between two movie presents: a running
 * average (1/8 weight); gaps outside 12-100 ms (a held or doubled frame, a pause, the
 * movie starting) are left out. est <= 0 = nothing measured yet. */
static inline double rfom_movie_period_update(double est, double gap_us)
{
    if (!(gap_us >= 12000.0 && gap_us <= 100000.0)) return est;
    return est > 0.0 ? est + (gap_us - est) * 0.125 : gap_us;
}

/* Repeat i (1..k-1) of a movie frame presented at t0 is due at t0 + i * period / k. A
 * repeat that comes more than half a slot late is dropped (and the rest of the frame's
 * repeats with it): returns 1 = present now, 0 = not yet, -1 = drop. */
static inline int rfom_movie_lfc_due(long long t0, long long now, double period, int k, int i)
{
    if (k < 2 || i < 1 || i >= k || !(period > 0.0)) return -1;
    const double slot = period / k;
    const double due = (double)t0 + slot * i;
    if ((double)now < due) return 0;
    return (double)now - due <= slot * 0.5 ? 1 : -1;
}

#endif
