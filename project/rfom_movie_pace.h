/* Movie pacing (30 fps FMV in flip mode 2), shared by the frame clock's vblank lock
 * (project/main.cpp, RFOM_MOVIE_VBLANK_LOCK) and the movie present
 * (ps3recomp/libs/video/rsx_d3d12_backend.c present_blit, RFOM_MOVIE_VSYNC), so both
 * take the same decision: a refresh rate that shows a 59.94 Hz tick (and so a 30 fps
 * frame) as a whole number of refreshes -- 60/120/240 Hz -- gets the lock plus vsync
 * presents; any other rate (144, 165 Hz...) with tearing presents available is left
 * to VRR (interval 0 + tearing, like gameplay), since vsync there alternates frames
 * between two refresh counts (5/6 at 165 Hz = judder). Tests: project/tests/movie_pace_test.c. */
#ifndef RFOM_MOVIE_PACE_H
#define RFOM_MOVIE_PACE_H

#define RFOM_MOVIE_TICK_US 16683.0   /* one 59.94 Hz vblank tick */
#define RFOM_MOVIE_LOCK_TOL_US 400.0

/* Refreshes per 59.94 Hz tick (1-4) when k refreshes land within the tolerance of
 * the tick, else 0 (the lock cannot engage). per_us = the display refresh period. */
static inline int rfom_movie_lock_k(double per_us)
{
    if (!(per_us > 0.0)) return 0;
    const int k = (int)(RFOM_MOVIE_TICK_US / per_us + 0.5);
    const double d = k * per_us - RFOM_MOVIE_TICK_US;
    return (k >= 1 && k <= 4 && d > -RFOM_MOVIE_LOCK_TOL_US && d < RFOM_MOVIE_LOCK_TOL_US) ? k : 0;
}

/* Default movie sync interval (RFOM_MOVIE_VSYNC unset, RFOM_FPS not vrr): 0 when the
 * refresh period is known, cannot be locked and tearing presents work (VRR paces the
 * movie); 1 otherwise (60/120/240 Hz, unknown rate, or no tearing support). */
static inline int rfom_movie_vsync_default(double per_us, int tearing)
{
    return (tearing && per_us > 0.0 && !rfom_movie_lock_k(per_us)) ? 0 : 1;
}

#endif
