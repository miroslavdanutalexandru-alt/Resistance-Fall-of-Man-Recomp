/* kbm_stick.h: mouse -> virtual right stick (vehicles, turrets). Slow, steady mouse
 * movement must give a steady stick whose average is the exact deflection, never
 * dropping back to centre or jumping (Fall of Man issue #9: tank turret jitter). */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../libs/input/kbm_stick.h"
#include "../libs/input/kbm_drive.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* The continuous stick position (units past centre) the mapping aims for. */
static double want(double a, double dz) { return dz * 127.0 + (127.0 - dz * 127.0) * a; }

static void test_idle_and_full(void)
{
    kbm_stick_carry c = { 0.37, 1 };
    CHECK(kbm_stick_axis(0.0, 0.28, &c) == 128 && c.r == 0.0 && c.dir == 0);
    c.r = 0.5; c.dir = 1;
    CHECK(kbm_stick_axis(0.001, 0.28, &c) == 128 && c.r == 0.0);
    CHECK(kbm_stick_axis(1.0, 0.28, &c) == 255);
    CHECK(kbm_stick_axis(5.0, 0.28, &c) == 255);
    CHECK(fabs(c.r) <= 1.0);
    CHECK(kbm_stick_axis(-1.0, 0.28, &c) == 1);
    CHECK(kbm_stick_axis(-5.0, 0.28, &c) == 1);
    CHECK(fabs(c.r) <= 1.0);
    CHECK(kbm_stick_axis(NAN, 0.28, &c) == 128);
}

/* Constant deflections: two adjacent bytes only, never centre, mean = target. */
static void test_mean_and_steadiness(void)
{
    const double dzs[3] = { 0.0, 0.28, 0.5 };
    for (int d = 0; d < 3; d++) {
        for (double a = 0.003; a < 1.0; a += 0.0137) {
            for (int sg = -1; sg <= 1; sg += 2) {
                kbm_stick_carry c = { 0.0, 0 };
                double sum = 0.0;
                int lo = 999, hi = -999;
                const int n = 600;
                for (int i = 0; i < n; i++) {
                    const int o = ((int)kbm_stick_axis(sg * a, dzs[d], &c) - 128) * sg;
                    CHECK(o > 0 || dzs[d] * 127.0 < 1.0);   /* never back to centre */
                    sum += o; if (o < lo) lo = o; if (o > hi) hi = o;
                }
                CHECK(hi - lo <= 1);
                CHECK(fabs(sum / n - want(a, dzs[d])) < 0.01);
            }
        }
    }
}

/* A slow ramp of the mouse speed: the average stick follows it monotonically, in
 * steps far finer than one byte (the old mapping truncated to whole bytes and its
 * first live step sat right at the deadzone). */
static void test_fine_resolution(void)
{
    double prev = -1.0;
    for (int k = 1; k <= 20; k++) {
        const double a = 0.002 + k * 0.0004;   /* 0.05 of a byte apart */
        kbm_stick_carry c = { 0.0, 0 };
        double sum = 0.0;
        for (int i = 0; i < 2000; i++) sum += kbm_stick_axis(a, 0.28, &c) - 128;
        const double m = sum / 2000.0;
        CHECK(m > prev);
        CHECK(fabs(m - want(a, 0.28)) < 0.002);
        prev = m;
    }
    /* the lowest byte used is the last one inside the deadzone, not an extra live step */
    kbm_stick_carry c = { 0.0, 0 }; int mn = 999;
    for (int i = 0; i < 100; i++) { const int o = kbm_stick_axis(0.0021, 0.28, &c) - 128; if (o < mn) mn = o; }
    CHECK(mn == (int)floor(0.28 * 127.0));
}

static void test_reversal_and_bounds(void)
{
    kbm_stick_carry c = { 0.0, 0 };
    for (int i = 0; i < 7; i++) kbm_stick_axis(0.3, 0.28, &c);
    CHECK(c.r != 0.0 && c.dir == 1);
    const int o = kbm_stick_axis(-0.3, 0.28, &c);   /* the carry of the other direction is dropped */
    kbm_stick_carry c2 = { 0.0, 0 };
    CHECK(o == kbm_stick_axis(-0.3, 0.28, &c2) && c.r == c2.r && c.dir == -1);
    c.r = 0.0; c.dir = 0;
    for (int i = 0; i < 1000; i++) { kbm_stick_axis(i % 2 ? 1.0 : 0.95, 0.9, &c); CHECK(fabs(c.r) <= 1.0); }
}

/* Issue #14: W/S drive the jeep (R2 / L2), A/D steer, only while the look code is off. */
static void test_drive(void)
{
    const unsigned R2 = 0x0200u, L2 = 0x0100u;
    int st = 7;
    CHECK(kbm_drive_map(0, 1, 0, 1, 0, R2, L2, &st) == 0u && st == 0);
    CHECK(kbm_drive_map(1, 1, 0, 0, 0, R2, L2, &st) == R2 && st == 0);
    CHECK(kbm_drive_map(1, 0, 1, 0, 0, R2, L2, &st) == L2);
    CHECK(kbm_drive_map(1, 1, 1, 0, 0, R2, L2, &st) == 0u);
    CHECK(kbm_drive_map(1, 0, 0, 1, 0, R2, L2, &st) == 0u && st == -1);
    CHECK(kbm_drive_map(1, 1, 0, 0, 1, R2, L2, &st) == R2 && st == 1);
    CHECK(kbm_drive_map(1, 0, 0, 1, 1, R2, L2, &st) == 0u && st == 0);
    CHECK(kbm_drive_rx(-1, 200) == 0 && kbm_drive_rx(1, 60) == 255 && kbm_drive_rx(0, 77) == 77);

    /* driving = look code off AND the driver input code ran within 200 ms */
    CHECK(kbm_drive_active(1, 5000, 4950, 1, 0, 0) == 1);
    CHECK(kbm_drive_active(0, 5000, 4950, 1, 0, 0) == 0);   /* RFOM_KBM_DRIVE=0 */
    CHECK(kbm_drive_active(1, 5000, 4800, 1, 0, 0) == 0);   /* left the vehicle 200 ms ago */
    CHECK(kbm_drive_active(1, 5000, 0, 1, 0, 0) == 0);      /* never drove */
    CHECK(kbm_drive_active(1, 5000, 4950, 0, 0, 0) == 0);   /* look code on: turret / gunner / on foot */
    CHECK(kbm_drive_active(1, 5000, 4950, 1, 1, 0) == 0);   /* pause menu */
    CHECK(kbm_drive_active(1, 5000, 4950, 1, 0, 1) == 0);   /* front end */
    CHECK(kbm_drive_active(1, 4000, 4950, 1, 0, 0) == 0);   /* clock went back */

    /* the review's loop: on foot, W held through a hitch. The look code stops
     * (menu mode), the driver code never runs, so R2 (the weapon wheel on
     * foot) must never be pressed -- frame after frame, however long it lasts. */
    {
        const unsigned long long play_ms = 1000, drive_ms = 0;
        unsigned pressed = 0; int steer_any = 0;
        for (unsigned long long now = 1000; now < 4000; now += 16) {
            const int look_off = now - play_ms > 200;
            pressed |= kbm_drive_map(kbm_drive_active(1, now, drive_ms, look_off, 0, 0), 1, 0, 1, 0, R2, L2, &st);
            steer_any |= st != 0;
        }
        CHECK(pressed == 0u && steer_any == 0);
    }
    /* got out of the jeep holding W: the drive mapping ends within 200 ms */
    {
        const unsigned long long out_ms = 2000;
        unsigned late = 0;
        for (unsigned long long now = 1500; now < 3000; now += 16) {
            const unsigned long long drive_ms = now < out_ms ? now : out_ms;
            const unsigned b = kbm_drive_map(kbm_drive_active(1, now, drive_ms, 1, 0, 0), 1, 0, 0, 0, R2, L2, &st);
            if (now < out_ms) CHECK(b == R2);
            else if (now - out_ms >= 200) late |= b;
        }
        CHECK(late == 0u);
    }
}

/* Weapon wheel (PORT_KBM_WHEEL): the stick the wheel reads as the game sees it. */
static double wmag(unsigned char x, unsigned char y)
{
    const double a = (x - 128) / 127.0, b = (y - 128) / 127.0;
    return sqrt(a * a + b * b);
}
#define WSTEP(w, key, mx, my, kx, ky, t, d) kbm_wheel_step(&(w), key, 0, mx, my, kx, ky, t, d, 100.0, 150, &sx, &sy)

static void test_wheel(void)
{
    unsigned char sx, sy;
    /* a tap stays a tap: the sticks are never taken */
    { kbm_wheel w = { 0 };
      CHECK(!WSTEP(w, 1, 300, 300, 0, 0, 1000, 0));
      CHECK(!WSTEP(w, 1, 0, 0, 0, 0, 1100, 1));
      CHECK(!WSTEP(w, 0, 0, 0, 0, 0, 1120, 2)); }
    /* a short mouse flick to the lower right (the Marksman slot): full length on the
     * diagonal, and it stays there when the mouse stops */
    { kbm_wheel w = { 0 };
      CHECK(!WSTEP(w, 1, 0, 0, 0, 0, 1000, 0));
      CHECK(WSTEP(w, 1, 0, 0, 0, 0, 1200, 1) && sx == 128 && sy == 128);   /* nothing moved: centred */
      CHECK(WSTEP(w, 1, 20, 20, 0, 0, 1216, 2) && sx == 128 && sy == 128);   /* under 0.4 x radius */
      CHECK(WSTEP(w, 1, 15, 15, 0, 0, 1232, 3) && sx > 128 && sx == sy && wmag(sx, sy) > 0.98 && wmag(sx, sy) < 1.02);
      for (int i = 0; i < 20; i++) CHECK(WSTEP(w, 1, 0, 0, 0, 0, 1248 + 16 * i, 4 + i) && sx > 200 && sx == sy);
      /* the other way: the direction swings round once the mouse has gone far enough */
      CHECK(WSTEP(w, 1, -300, 0, 0, 0, 1600, 30) && sx < 10 && abs(sy - 128) < 20);
      /* let go: kept for two more reports, then centre and the sticks are given back */
      CHECK(WSTEP(w, 0, 0, 0, 0, 0, 1616, 30) && sx < 10);
      CHECK(WSTEP(w, 0, 0, 0, 0, 0, 1632, 31) && sx < 10);
      CHECK(!WSTEP(w, 0, 0, 0, 0, 0, 1648, 32));
      CHECK(!WSTEP(w, 0, 50, 50, 0, 0, 1664, 33)); }
    /* keys: a diagonal is full length (not 1.41), a single key straight out */
    { kbm_wheel w = { 0 };
      WSTEP(w, 1, 0, 0, 0, 0, 0, 0);
      CHECK(WSTEP(w, 1, 0, 0, 1, 1, 200, 0) && sx == sy && sx > 128 && wmag(sx, sy) > 0.98 && wmag(sx, sy) < 1.02);
      CHECK(WSTEP(w, 1, 0, 0, 0, -1, 216, 0) && sx == 128 && sy == 1);
      CHECK(WSTEP(w, 1, 0, 0, 0, 0, 232, 0) && sx == 128 && sy == 1);    /* key let go: kept */
      /* no reports taken after the release (game paused): 250 ms at most */
      CHECK(WSTEP(w, 0, 0, 0, 0, 0, 400, 0));
      CHECK(!WSTEP(w, 0, 0, 0, 0, 0, 482, 0)); }
    /* pressed in a menu: ignored */
    { kbm_wheel w = { 0 };
      kbm_wheel_step(&w, 1, 1, 0, 0, 0, 0, 0, 0, 100.0, 150, &sx, &sy);
      CHECK(!kbm_wheel_step(&w, 1, 0, 100, 100, 1, 1, 500, 0, 100.0, 150, &sx, &sy)); }
}

int main(void)
{
    test_drive();
    test_idle_and_full();
    test_mean_and_steadiness();
    test_fine_resolution();
    test_reversal_and_bounds();
    test_wheel();
    if (fails) { fprintf(stderr, "test_kbm_stick: %d failure(s)\n", fails); return 1; }
    printf("test_kbm_stick: all passed\n");
    return 0;
}
