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
    CHECK(rfom_aspect_present_mul(s.mul, last, 0, t, 2000, 500, 0) == m);
    CHECK(rfom_aspect_present_mul(s.mul, last, 0, t + 1500, 2000, 500, 0) == m);   /* the setter skips some frames */
    CHECK(rfom_aspect_present_mul(s.mul, last, 0, t + 2000, 2000, 500, 0) == 1.0f);   /* no camera, no 3D scene for 2 s */
    CHECK(rfom_aspect_present_mul(s.mul, last, 0, t + 60000, 2000, 500, 0) == 1.0f);
    CHECK(s.on);   /* the next camera (an ADS one) brings the stretch straight back */
    t += 60000;
    if (rfom_aspect_note(&s, (double)(float)A169, W219, t, 1000, 3, &base) != 1.0) last = t;
    CHECK(rfom_aspect_present_mul(s.mul, last, 0, t, 2000, 500, 0) == m);
    CHECK(rfom_aspect_present_mul(s.mul, last, 0, t, 2000, 500, 1) == 1.0f);   /* movie */
    CHECK(rfom_aspect_present_mul(s.mul, last, 0, t + 60000, 0, 500, 0) == m);   /* RFOM_ASPECT_HOLD=0 */
    CHECK(rfom_aspect_present_mul(s.mul, last, 0, last - 5, 2000, 500, 0) == m);   /* stamped after "now" was read */
    CHECK(rfom_aspect_present_mul(s.mul, 0, t, t, 2000, 500, 0) == 1.0f);   /* ended (target back at 16:9) */
}

/* Issue #2 on 0.3.0: the game set its gameplay camera once and kept it, so the stretch timed out a
 * few seconds into gameplay (and came back only while aiming). 3D scene frames keep it on. */
static void test_present_scene_keeps_stretch(void)
{
    rfom_aspect_state s = { 0, 1.0f, 0, 0 };
    double base;
    unsigned long long t = 10000;
    const float m = (float)(W219 / A169);
    CHECK(rfom_aspect_note(&s, A169, W219, t, 1000, 3, &base) != 1.0);
    const unsigned long long last = t;
    for (int i = 0; i < 600; i++, t += 16)   /* ~10 s of gameplay, no camera setter call */
        CHECK(rfom_aspect_present_mul(s.mul, last, t, t, 2000, 500, 0) == m);
    CHECK(rfom_aspect_present_mul(s.mul, last, t - 16, t, 2000, 500, 0) == m);   /* stamped this frame */
    CHECK(rfom_aspect_present_mul(s.mul, last, t + 5, t, 2000, 500, 0) == m);   /* stamped after "now" was read */
    /* a loading screen: no 3D scene; the letterbox comes back once the scene hold runs out */
    const unsigned long long scene = t;
    CHECK(rfom_aspect_present_mul(s.mul, last, scene, t + 499, 2000, 500, 0) == m);
    CHECK(rfom_aspect_present_mul(s.mul, last, scene, t + 500, 2000, 500, 0) == 1.0f);
    CHECK(rfom_aspect_present_mul(s.mul, last, scene, t + 30000, 2000, 500, 0) == 1.0f);
    /* gameplay goes on through the kept camera: the stretch is back with the first 3D frame */
    CHECK(rfom_aspect_present_mul(s.mul, last, t + 30016, t + 30016, 2000, 500, 0) == m);
    CHECK(rfom_aspect_present_mul(s.mul, last, t, t, 2000, 500, 1) == 1.0f);   /* movie */
    CHECK(rfom_aspect_present_mul(s.mul, last, t, t, 2000, 0, 0) == 1.0f);   /* RFOM_ASPECT_SCENE_HOLD=0: 0.3.0 */
    CHECK(rfom_aspect_present_mul(s.mul, 0, t, t, 2000, 500, 0) == 1.0f);   /* target back at 16:9 */
}

/* Issue #2 on 0.3.1: "the menu screen keeps stretching to 21:9 then back to 16:9". Once the stretch
 * is off outside play, stray cameras and short 3D bursts must not bring it back; play or a steady
 * 3D scene does. */
static void test_gate_menus_stay_169(void)
{
    const float m = (float)(W219 / A169);
    unsigned long long t = 100000, play = t, scene = 0, run0 = 0;
    unsigned long long held = 0;
    int flips = 0;
    float prev;
    /* gameplay: look code and 3D scene every frame, camera set once */
    for (int i = 0; i < 120; i++, t += 16) {
        play = t; rfom_aspect_scene_stamp(&scene, &run0, t);
        CHECK(rfom_aspect_gate(&held, rfom_aspect_present_mul(m, 100000, scene, t, 2000, 500, 0), 0, play, scene, run0, t, 1000) == m);
    }
    /* pause: no look code; a camera call every 3 s and a 3D burst of 300 ms every 1.5 s */
    const unsigned long long p0 = t;
    unsigned long long cam = 100000;
    prev = m;
    int off = 0;
    for (int i = 0; i < 1500; i++, t += 16) {
        const unsigned long long e = t - p0;
        if (e % 3000 < 16) cam = t;
        if (e % 1500 < 300) rfom_aspect_scene_stamp(&scene, &run0, t);
        const float g = rfom_aspect_gate(&held, rfom_aspect_present_mul(m, cam, scene, t, 2000, 500, 0), 0, play, scene, run0, t, 1000);
        if (g != prev) flips++;
        if (g == 1.0f) off = 1;
        prev = g;
    }
    CHECK(off && flips == 1);   /* one change to the letterbox, never back */
    /* 0.3.1 (RFOM_ASPECT_REARM_MS=0) flips on the same screen */
    {
        unsigned long long s2 = 0, r2 = 0, c2 = 100000, t2 = p0;
        unsigned long long h2 = 0; int f2 = 0; float pv = m;
        for (int i = 0; i < 1500; i++, t2 += 16) {
            const unsigned long long e = t2 - p0;
            if (e % 3000 < 16) c2 = t2;
            if (e % 1500 < 300) rfom_aspect_scene_stamp(&s2, &r2, t2);
            const float g = rfom_aspect_gate(&h2, rfom_aspect_present_mul(m, c2, s2, t2, 2000, 500, 0), 0, play, s2, r2, t2, 0);
            if (g != pv) f2++;
            pv = g;
        }
        CHECK(f2 > 4);
    }
    /* back to play: the stretch returns with the first look code frame */
    play = t; rfom_aspect_scene_stamp(&scene, &run0, t);
    CHECK(rfom_aspect_gate(&held, rfom_aspect_present_mul(m, cam, scene, t, 2000, 500, 0), 0, play, scene, run0, t, 1000) == m);
    CHECK(!held);
    /* aiming down sights / a long hitch in play: off for a moment, back at once */
    t += 3000;
    CHECK(rfom_aspect_gate(&held, rfom_aspect_present_mul(m, cam, scene, t, 2000, 500, 0), 0, play, scene, run0, t, 1000) == 1.0f);
    play = t; cam = t;
    CHECK(rfom_aspect_gate(&held, rfom_aspect_present_mul(m, cam, scene, t, 2000, 500, 0), 0, play, scene, run0, t, 1000) == m);
}

/* A cutscene after a loading screen (no look code): letterboxed for its first RFOM_ASPECT_REARM_MS,
 * then stretched for good; gaps break the run; a stretch that is on is never held in a cutscene. */
static void test_gate_cutscene_rearms(void)
{
    const float m = (float)(W219 / A169);
    unsigned long long t = 50000, scene = 0, run0 = 0;
    const unsigned long long play = 1000, cam = t;   /* look code last ran long ago */
    unsigned long long held = 1;
    int on_at = -1;
    for (int i = 0; i < 200; i++, t += 33) {   /* 30 fps cutscene */
        rfom_aspect_scene_stamp(&scene, &run0, t);
        const float g = rfom_aspect_gate(&held, rfom_aspect_present_mul(m, cam, scene, t, 2000, 500, 0), 0, play, scene, run0, t, 1000);
        if (g == m && on_at < 0) on_at = (int)(t - 50000);
        if (on_at >= 0) CHECK(g == m);
    }
    CHECK(on_at >= 1000 && on_at < 1100);
    /* frames with fewer 3D draws (gaps over RFOM_ASPECT_SCENE_GAP_MS) restart the run */
    unsigned long long s = 0, r = 0, u = 90000;
    held = 1;
    for (int i = 0; i < 40; i++, u += 300) {
        rfom_aspect_scene_stamp(&s, &r, u);
        CHECK(r == u);
        CHECK(rfom_aspect_gate(&held, m, 0, play, s, r, u, 1000) == 1.0f);
    }
    /* in play -> cutscene: stays on */
    held = 0;
    CHECK(rfom_aspect_gate(&held, m, 0, play, t, t, t + 5000, 1000) == m);
    /* RFOM_ASPECT_REARM_MS=0: as 0.3.1 */
    held = 1;
    CHECK(rfom_aspect_gate(&held, m, 0, 0, 0, 0, t, 0) == m);
    /* narrow targets (16:10, mul < 1) are gated the same way */
    const float n = (float)(1.6 / A169);
    held = 0;
    CHECK(rfom_aspect_gate(&held, 1.0f, 0, 0, 0, 0, t, 1000) == 1.0f && held);
    CHECK(rfom_aspect_gate(&held, n, 0, 0, 0, 0, t, 1000) == 1.0f);
    CHECK(rfom_aspect_gate(&held, n, 0, t, 0, 0, t, 1000) == n);
}

/* A widened camera setter call as stubs.cpp makes it: returns its time, and stamps *foreign when the
 * camera was not used in play. */
static unsigned long long cam_call(rfom_aspect_camset* cs, unsigned cam, double fov, unsigned long long play,
                                   unsigned long long now, unsigned long long* foreign)
{
    const int in_play = play && (now <= play || now - play < RFOM_ASPECT_PLAY_MS);
    if (!rfom_aspect_cam_known(cs, rfom_aspect_cam_sig(cam, fov), in_play)) *foreign = now;
    return now;
}

/* The pause menu (g_rfom_pause_ui) with the 3D world still drawn steadily behind it and the gameplay
 * camera set again every 2 s: locked at 16:9 once the look code stops, for as long as it stays open;
 * play brings it back at once. A steady scene run that began before the hold does not re-arm it early. */
static void test_gate_pause_menu_locked(void)
{
    const float m = (float)(W219 / A169);
    rfom_aspect_camset cs = { {0}, 0 };
    unsigned long long t = 200000, play = t, scene = 0, run0 = 0, cam = t, held = 0, since = 0, foreign = 0;
    int spent = 0;
    for (int i = 0; i < 60; i++, t += 16) {   /* gameplay: view camera (82), ADS (55), a shadow view */
        play = t; rfom_aspect_scene_stamp(&scene, &run0, t);
        cam = cam_call(&cs, 0x30001000u, i % 10 < 7 ? 82.0 : 55.0, play, t, &foreign);
        cam_call(&cs, 0x30002000u, 30.0, play, t, &foreign);
        CHECK(!rfom_aspect_menu(0, &since, &spent, foreign, t));
        CHECK(rfom_aspect_gate(&held, rfom_aspect_present_mul(m, cam, scene, t, 2000, 500, 0), 0, play, scene, run0, t, 1000) == m);
    }
    CHECK(!foreign);
    const unsigned long long p0 = t;   /* the pause menu loads its text: the flag goes up */
    int flips = 0; float prev = m;
    for (int i = 0; i < 1000; i++, t += 16) {   /* 16 s in the pause menu */
        if ((t - p0) % 2000 < 16) { cam = cam_call(&cs, 0x30001000u, 82.0, play, t, &foreign);
                                    cam_call(&cs, 0x30002000u, 30.0, play, t, &foreign); }
        rfom_aspect_scene_stamp(&scene, &run0, t);
        const float m0 = rfom_aspect_present_mul(m, cam, scene, t, 2000, 500, 0);
        CHECK(m0 == m);   /* without the gate the stretch would stay on */
        const float g = rfom_aspect_gate(&held, m0, rfom_aspect_menu(1, &since, &spent, foreign, t), play, scene, run0, t, 1000);
        if (t - p0 >= RFOM_ASPECT_PLAY_MS) CHECK(g == 1.0f);
        if (g != prev) flips++;
        prev = g;
    }
    CHECK(flips == 1 && !spent && !foreign);
    /* RFOM_MENU_ASPECT=0 (rearm 0): the 0.3.1 stretch, menu or not */
    {
        unsigned long long h = 0;
        CHECK(rfom_aspect_gate(&h, m, 1, play, scene, run0, t, 0) == m);
    }
    /* resume: the look code runs again (it also clears the flag) */
    play = t;
    CHECK(rfom_aspect_gate(&held, m, 1, play, scene, run0, t, 1000) == m && !held);
    CHECK(!rfom_aspect_menu(0, &since, &spent, foreign, t) && !since);
    /* held at t while a scene run began 5 s before: re-armed only rearm_ms after the hold */
    held = 0;
    const unsigned long long h0 = t + 5000, r5 = t;
    CHECK(rfom_aspect_gate(&held, 1.0f, 0, 0, h0, r5, h0, 1000) == 1.0f && held == h0);
    CHECK(rfom_aspect_gate(&held, m, 0, 0, h0 + 500, r5, h0 + 500, 1000) == 1.0f);
    CHECK(rfom_aspect_gate(&held, m, 0, 0, h0 + 1000, r5, h0 + 1000, 1000) == m && !held);
}

/* Code review of 9d7b405: a stale pause flag (a text load in play, then an in-engine cutscene with no
 * on-foot play to clear it) and a cutscene that sets its own camera only at 1.5 s and then every 2 s:
 * that camera ends the flag, and the cutscene reaches 21:9 and stays there. A cutscene that re-uses
 * the view camera with a new field of view counts as its own camera too. */
static void test_gate_stale_pause_flag_cutscene(void)
{
    const float m = (float)(W219 / A169);
    for (int variant = 0; variant < 2; variant++) {
        rfom_aspect_camset cs = { {0}, 0 };
        const unsigned long long u0 = 400000, pl = u0 - 1000;   /* the look code stopped a second before */
        unsigned long long u = u0, s = 0, r = 0, h = 0, c = 0, since = 0, foreign = 0;
        int spent = 0, on_at = -1;
        c = cam_call(&cs, 0x30001000u, 82.0, pl, pl, &foreign);   /* the last play frame */
        for (int i = 0; i < 300; i++, u += 33) {   /* 10 s of cutscene at 30 fps */
            if (u - u0 >= 1500 && (u - u0 - 1500) % 2000 < 33)
                c = variant ? cam_call(&cs, 0x30001000u, 60.0, pl, u, &foreign) : cam_call(&cs, 0x30005000u, 82.0, pl, u, &foreign);
            rfom_aspect_scene_stamp(&s, &r, u);
            const float g = rfom_aspect_gate(&h, rfom_aspect_present_mul(m, c, s, u, 2000, 500, 0),
                                             rfom_aspect_menu(1, &since, &spent, foreign, u), pl, s, r, u, 1000);
            if (g == m && on_at < 0) on_at = (int)(u - u0);
            if (on_at >= 0) CHECK(g == m);   /* once on, no flipping back */
        }
        CHECK(spent && on_at >= 1500 && on_at <= 2600);
        /* the flag dropped (play) and raised again (a new pause): it counts again */
        CHECK(!rfom_aspect_menu(0, &since, &spent, foreign, u) && !spent);
        CHECK(rfom_aspect_menu(1, &since, &spent, foreign, u + 16));
    }
}

/* The play camera set: most recent first, bounded; outside play nothing new is learned. */
static void test_camset(void)
{
    rfom_aspect_camset cs = { {0}, 0 };
    for (unsigned k = 0; k < 20; k++) CHECK(rfom_aspect_cam_known(&cs, rfom_aspect_cam_sig(0x1000u + k, 82.0), 1));
    CHECK(cs.n == RFOM_ASPECT_CAMSET_N && cs.sig[0] == rfom_aspect_cam_sig(0x1000u + 19, 82.0));
    CHECK(rfom_aspect_cam_known(&cs, rfom_aspect_cam_sig(0x1000u + 12, 82.0), 0));    /* still in the set */
    CHECK(!rfom_aspect_cam_known(&cs, rfom_aspect_cam_sig(0x1000u + 2, 82.0), 0));    /* pushed out */
    CHECK(!rfom_aspect_cam_known(&cs, rfom_aspect_cam_sig(0x9000u, 82.0), 0));
    CHECK(!rfom_aspect_cam_known(&cs, rfom_aspect_cam_sig(0x9000u, 82.0), 0));        /* not learned */
    CHECK(rfom_aspect_cam_sig(0x1000u, 82.0) == rfom_aspect_cam_sig(0x1000u, 82.001));
    CHECK(rfom_aspect_cam_sig(0x1000u, 82.0) != rfom_aspect_cam_sig(0x1000u, 60.0));
}

int main(void)
{
    test_ads_keeps_stretch();
    test_off_hysteresis();
    test_off_and_narrow_targets();
    test_present_recency();
    test_present_scene_keeps_stretch();
    test_gate_menus_stay_169();
    test_gate_cutscene_rearms();
    test_gate_pause_menu_locked();
    test_gate_stale_pause_flag_cutscene();
    test_camset();
    if (fails) { fprintf(stderr, "aspect_test: %d failure(s)\n", fails); return 1; }
    printf("aspect_test: all passed\n");
    return 0;
}
