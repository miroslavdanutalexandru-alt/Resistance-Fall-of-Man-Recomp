/* rfom_movie_pace.h: 30 fps movies keep vsync (and the vblank lock) on 60/120/240 Hz
 * and on displays without tearing presents; other rates with tearing go to VRR. */
#include <stdio.h>
#include "rfom_movie_pace.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static double hz(double h) { return 1e6 / h; }

int main(void)
{
    CHECK(rfom_movie_lock_k(hz(60.0)) == 1);
    CHECK(rfom_movie_lock_k(hz(59.94)) == 1);
    CHECK(rfom_movie_lock_k(hz(120.0)) == 2);
    CHECK(rfom_movie_lock_k(hz(119.88)) == 2);
    CHECK(rfom_movie_lock_k(hz(240.0)) == 4);
    CHECK(rfom_movie_lock_k(hz(300.0)) == 5);
    CHECK(rfom_movie_lock_k(hz(360.0)) == 6);
    CHECK(rfom_movie_lock_k(hz(480.0)) == 8);
    CHECK(rfom_movie_lock_k(hz(540.0)) == 0);   /* k = 9: past the range */
    CHECK(rfom_movie_lock_k(hz(280.0)) == 0);
    CHECK(rfom_movie_lock_k(hz(144.0)) == 0);
    CHECK(rfom_movie_lock_k(hz(165.0)) == 0);
    CHECK(rfom_movie_lock_k(hz(75.0)) == 0);
    CHECK(rfom_movie_lock_k(hz(50.0)) == 0);
    CHECK(rfom_movie_lock_k(0.0) == 0);

    /* lockable rates keep vsync, tearing or not */
    CHECK(rfom_movie_vsync_default(hz(60.0), 1) == 1);
    CHECK(rfom_movie_vsync_default(hz(120.0), 1) == 1);
    CHECK(rfom_movie_vsync_default(hz(240.0), 1) == 1);
    CHECK(rfom_movie_vsync_default(hz(360.0), 1) == 1);
    CHECK(rfom_movie_vsync_default(hz(480.0), 1) == 1);
    /* 144 / 165 Hz: VRR when tearing presents work, else vsync as before */
    CHECK(rfom_movie_vsync_default(hz(165.0), 1) == 0);
    CHECK(rfom_movie_vsync_default(hz(144.0), 1) == 0);
    CHECK(rfom_movie_vsync_default(hz(165.0), 0) == 1);
    /* unknown refresh rate: vsync */
    CHECK(rfom_movie_vsync_default(0.0, 1) == 1);

    /* vblank lock phase: 60/120/240 Hz keep 1500 +- 1000 us */
    {
        const double rates[3] = { 60.0, 120.0, 240.0 };
        for (int i = 0; i < 3; i++) {
            const long long p = (long long)hz(rates[i]);
            CHECK(rfom_movie_phase_lead_us(p) == 1500 && rfom_movie_phase_band_us(p) == 1000);
        }
    }
    /* at every lockable rate, wherever the tick starts, it ends up inside
     * (vblank, next vblank) after the correction: never on a real vblank */
    for (int k = 1; k <= 8; k++) {
        const long long p = (long long)(RFOM_MOVIE_TICK_US / k);
        const long long now = 1000000, since = 1234;   /* last vblank at now - since */
        for (long long t = 0; t < 2 * p; t += 7) {
            long long next_tick = now + t;
            next_tick -= rfom_movie_phase_shift(next_tick, now, since, p);
            const long long phase = ((next_tick - (now - since)) % p + p) % p;
            CHECK(phase > 0 && phase < p);
            CHECK(phase >= rfom_movie_phase_lead_us(p) - rfom_movie_phase_band_us(p));
            CHECK(phase <= rfom_movie_phase_lead_us(p) + rfom_movie_phase_band_us(p));
            CHECK(next_tick >= now + t - p / 2 && next_tick <= now + t + p / 2);   /* never whole periods */
        }
    }

    if (fails) { fprintf(stderr, "movie_pace_test: %d failure(s)\n", fails); return 1; }
    printf("movie_pace_test: all passed\n");
    return 0;
}
