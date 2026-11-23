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
    CHECK(rfom_movie_lock_k(hz(144.0)) == 0);
    CHECK(rfom_movie_lock_k(hz(165.0)) == 0);
    CHECK(rfom_movie_lock_k(hz(75.0)) == 0);
    CHECK(rfom_movie_lock_k(hz(50.0)) == 0);
    CHECK(rfom_movie_lock_k(0.0) == 0);

    /* lockable rates keep vsync, tearing or not */
    CHECK(rfom_movie_vsync_default(hz(60.0), 1) == 1);
    CHECK(rfom_movie_vsync_default(hz(120.0), 1) == 1);
    CHECK(rfom_movie_vsync_default(hz(240.0), 1) == 1);
    /* 144 / 165 Hz: VRR when tearing presents work, else vsync as before */
    CHECK(rfom_movie_vsync_default(hz(165.0), 1) == 0);
    CHECK(rfom_movie_vsync_default(hz(144.0), 1) == 0);
    CHECK(rfom_movie_vsync_default(hz(165.0), 0) == 1);
    /* unknown refresh rate: vsync */
    CHECK(rfom_movie_vsync_default(0.0, 1) == 1);

    if (fails) { fprintf(stderr, "movie_pace_test: %d failure(s)\n", fails); return 1; }
    printf("movie_pace_test: all passed\n");
    return 0;
}
