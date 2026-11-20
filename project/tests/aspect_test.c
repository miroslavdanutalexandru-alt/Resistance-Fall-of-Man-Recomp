/* rfom_aspect.h: the ultrawide (Hor+) present stretch must not follow the camera
 * asked about (aiming down sights flipped it to 16:9, issue #2), and must only end
 * once the target aspect has been 16:9 for a while. */
#include <stdio.h>
#include <math.h>
#include "rfom_aspect.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static const double W219 = 2560.0 / 1080.0, A169 = 16.0 / 9.0;

static void test_ads_keeps_stretch(void)
{
    rfom_aspect_state s = { 0, 1.0f, 0, 0 };
    double base = 0.0;
    unsigned long long t = 1000;
    double w = rfom_aspect_note(&s, A169, W219, t, 1000, 3, &base);
    CHECK(s.on && fabs(w - W219 / A169) < 1e-9 && base == A169);
    CHECK(fabs(s.mul - (float)(W219 / A169)) < 1e-6f);
    /* gameplay camera, ADS camera (the game's float 16:9), read-back widened camera,
     * shadow camera -- for 10 s: the stretch never drops or changes */
    for (int i = 0; i < 600; i++) {
        t += 16;
        const double cams[4] = { A169, (double)(float)A169, W219, 1.0 };
        const double a = cams[i % 4];
        w = rfom_aspect_note(&s, a, W219, t, 1000, 3, &base);
        CHECK(s.on);
        CHECK(fabs(s.mul - (float)(W219 / A169)) < 1e-6f);
        if (a == 1.0) CHECK(w == 1.0 && base == 0.0);
        else CHECK(fabs(w - W219 / A169) < 1e-5 && fabs(base - A169) < 1e-6);
    }
}

static void test_off_hysteresis(void)
{
    rfom_aspect_state s = { 0, 1.0f, 0, 0 };
    double base;
    unsigned long long t = 5000;
    rfom_aspect_note(&s, A169, W219, t, 1000, 3, &base);
    CHECK(s.on);
    /* a 400 ms blip at 16:9 (window being resized), then wide again: stays on */
    for (int i = 0; i < 25; i++) { t += 16; CHECK(rfom_aspect_note(&s, A169, A169, t, 1000, 3, &base) == 1.0); CHECK(s.on); }
    t += 16; rfom_aspect_note(&s, A169, W219, t, 1000, 3, &base);
    CHECK(s.on && s.off_since == 0 && s.off_n == 0);
    /* the target stays 16:9: off after 1000 ms, not before */
    const unsigned long long t0 = t + 16;
    int off_at = -1;
    for (int i = 0; i < 100 && off_at < 0; i++) { t += 16; rfom_aspect_note(&s, A169, A169, t, 1000, 3, &base); if (!s.on) off_at = (int)(t - t0); }
    CHECK(off_at >= 1000 && off_at < 1100);
    CHECK(s.mul == 1.0f);
    /* long enough but too few calls: still on */
    rfom_aspect_state q = { 0, 1.0f, 0, 0 };
    rfom_aspect_note(&q, A169, W219, 100, 1000, 3, &base);
    rfom_aspect_note(&q, A169, A169, 200, 1000, 3, &base);
    rfom_aspect_note(&q, A169, A169, 5000, 1000, 3, &base);
    CHECK(q.on);
    rfom_aspect_note(&q, A169, A169, 5016, 1000, 3, &base);
    CHECK(!q.on);
    /* non-16:9 cameras never count towards turning it off */
    rfom_aspect_state r = { 0, 1.0f, 0, 0 };
    rfom_aspect_note(&r, A169, W219, 100, 0, 1, &base);
    for (int i = 0; i < 10; i++) rfom_aspect_note(&r, 1.0, A169, 200 + i * 1000, 0, 1, &base);
    CHECK(r.on);
}

static void test_off_and_narrow_targets(void)
{
    rfom_aspect_state s = { 0, 1.0f, 0, 0 };
    double base;
    for (int i = 0; i < 100; i++) CHECK(rfom_aspect_note(&s, A169, 0.0, 100 + i * 16, 1000, 3, &base) == 1.0);   /* RFOM_ASPECT=off */
    CHECK(!s.on && s.mul == 1.0f);
    for (int i = 0; i < 100; i++) rfom_aspect_note(&s, A169, A169, 100 + i * 16, 1000, 3, &base);   /* 16:9 window */
    CHECK(!s.on);
    /* 16:10: the camera gains height (ratio < 1), the present narrows by the same ratio */
    const double w = rfom_aspect_note(&s, A169, 1.6, 9000, 1000, 3, &base);
    CHECK(s.on && fabs(w - 1.6 / A169) < 1e-9 && fabs(s.mul - (float)(1.6 / A169)) < 1e-6f);
}

/* 2D screens (loading, briefing / stats, menus) set no camera: the present goes back to its
 * 16:9 letterbox once no widened camera has been set for the hold; ADS cameras refresh it. */
static void test_present_recency(void)
{
    rfom_aspect_state s = { 0, 1.0f, 0, 0 };
    double base;
    unsigned long long t = 10000, last = 0;
    const float m = (float)(W219 / A169);
    for (int i = 0; i < 60; i++, t += 16)   /* gameplay and ADS cameras alternate */
        if (rfom_aspect_note(&s, i & 1 ? (double)(float)A169 : A169, W219, t, 1000, 3, &base) != 1.0) last = t;
    CHECK(s.on && last == t - 16);
    CHECK(rfom_aspect_present_mul(s.mul, last, t, 2000, 0) == m);
    CHECK(rfom_aspect_present_mul(s.mul, last, t + 1500, 2000, 0) == m);   /* the setter skips some frames */
    CHECK(rfom_aspect_present_mul(s.mul, last, t + 2000, 2000, 0) == 1.0f);   /* no camera for 2 s */
    CHECK(rfom_aspect_present_mul(s.mul, last, t + 60000, 2000, 0) == 1.0f);
    CHECK(s.on);   /* the next camera (an ADS one) brings the stretch straight back */
    t += 60000;
    if (rfom_aspect_note(&s, (double)(float)A169, W219, t, 1000, 3, &base) != 1.0) last = t;
    CHECK(rfom_aspect_present_mul(s.mul, last, t, 2000, 0) == m);
    CHECK(rfom_aspect_present_mul(s.mul, last, t, 2000, 1) == 1.0f);   /* movie */
    CHECK(rfom_aspect_present_mul(s.mul, last, t + 60000, 0, 0) == m);   /* RFOM_ASPECT_HOLD=0 */
    CHECK(rfom_aspect_present_mul(s.mul, last, last - 5, 2000, 0) == m);   /* stamped after "now" was read */
    CHECK(rfom_aspect_present_mul(s.mul, 0, t, 2000, 0) == 1.0f);   /* ended */
}

int main(void)
{
    test_ads_keeps_stretch();
    test_off_hysteresis();
    test_off_and_narrow_targets();
    test_present_recency();
    if (fails) { fprintf(stderr, "aspect_test: %d failure(s)\n", fails); return 1; }
    printf("aspect_test: all passed\n");
    return 0;
}
