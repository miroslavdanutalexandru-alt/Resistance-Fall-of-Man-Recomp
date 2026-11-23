/* Movie pacing (30 fps FMV in flip mode 2), shared by the frame clock's vblank lock
 * (project/main.cpp, RFOM_MOVIE_VBLANK_LOCK) and the movie present
 * (ps3recomp/libs/video/rsx_d3d12_backend.c present_blit, RFOM_MOVIE_VSYNC), so both
 * take the same decision: a refresh rate that shows a 59.94 Hz tick (and so a 30 fps
 * frame) as a whole number of refreshes -- 60/120/240/360/480 Hz -- gets the lock plus vsync
 * presents; any other rate (144, 165 Hz...) with tearing presents available is left
 * to VRR (interval 0 + tearing, like gameplay), since vsync there alternates frames
 * between two refresh counts (5/6 at 165 Hz = judder). Tests: project/tests/movie_pace_test.c. */
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

#endif
