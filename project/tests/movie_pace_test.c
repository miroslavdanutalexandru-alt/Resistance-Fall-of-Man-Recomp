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

    /* movie LFC: 30 fps -> 2 presents per frame (60/s) wherever the screen can show them */
    CHECK(rfom_movie_lfc_count(29.97, 165.0, -1) == 2);
    CHECK(rfom_movie_lfc_count(29.97, 144.0, -1) == 2);
    CHECK(rfom_movie_lfc_count(30.0, 75.0, -1) == 2);
    CHECK(rfom_movie_lfc_count(29.97, 50.0, -1) == 1);    /* 60/s is above a 50 Hz screen */
    CHECK(rfom_movie_lfc_count(24.0, 165.0, -1) == 2);    /* 48/s */
    CHECK(rfom_movie_lfc_count(15.0, 165.0, -1) == 4);
    CHECK(rfom_movie_lfc_count(60.0, 165.0, -1) == 1);    /* already in range */
    CHECK(rfom_movie_lfc_count(0.0, 165.0, -1) == 1);
    CHECK(rfom_movie_lfc_count(29.97, 0.0, -1) == 1);
    CHECK(rfom_movie_lfc_count(29.97, 165.0, 0) == 1);    /* RFOM_MOVIE_LFC=0 */
    CHECK(rfom_movie_lfc_count(29.97, 165.0, 3) == 3);    /* RFOM_MOVIE_LFC=3 */
    CHECK(rfom_movie_lfc_count(29.97, 165.0, 99) == RFOM_MOVIE_LFC_MAX);

    /* period estimate: first gap taken as is, outliers left out, converges */
    {
        double e = rfom_movie_period_update(0.0, 33367.0);
        CHECK(e == 33367.0);
        CHECK(rfom_movie_period_update(e, 250000.0) == e);   /* pause */
        CHECK(rfom_movie_period_update(e, 3000.0) == e);     /* two presents back to back */
        CHECK(rfom_movie_period_update(0.0, 500000.0) == 0.0);
        for (int i = 0; i < 200; i++) e = rfom_movie_period_update(e, (i & 1) ? 41700.0 : 41716.0);
        CHECK(e > 41600.0 && e < 41800.0);                  /* 23.976 fps */
    }

    /* repeat timing: the midpoint of a 33.37 ms frame, dropped when half a slot late */
    {
        const double p = 33367.0;
        CHECK(rfom_movie_lfc_due(1000000, 1000000 + 16000, p, 2, 1) == 0);
        CHECK(rfom_movie_lfc_due(1000000, 1000000 + 16684, p, 2, 1) == 1);
        CHECK(rfom_movie_lfc_due(1000000, 1000000 + 16684 + 8000, p, 2, 1) == 1);
        CHECK(rfom_movie_lfc_due(1000000, 1000000 + 16684 + 8500, p, 2, 1) == -1);
        CHECK(rfom_movie_lfc_due(1000000, 1000000 + 16684, p, 2, 2) == -1);   /* only k-1 repeats */
        CHECK(rfom_movie_lfc_due(1000000, 1000000 + 16684, p, 1, 1) == -1);
        CHECK(rfom_movie_lfc_due(0, 22245, p, 3, 2) == 1);                    /* 2/3 of the frame */
    }

    if (fails) { fprintf(stderr, "movie_pace_test: %d failure(s)\n", fails); return 1; }
    printf("movie_pace_test: all passed\n");
    return 0;
}
