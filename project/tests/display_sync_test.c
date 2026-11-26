/* port_display_sync.h: the VRR verdict from vblank gaps, the hysteresis, and the fixed-refresh plans
 * (ps3recomp/tools/test_rfom_display_sync.py). */
#include <stdio.h>
#define PDS_LOGIC_ONLY
#include "port_display_sync.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
#define NEAR(a, b) (fabs((a) - (b)) < 0.01)

static float g_iv[PDS_MAX_IV];
/* vblank gaps over a 2 s window: the period `gap` with +-jit ms of alternating jitter */
static int fill(double gap, double jit)
{
    int n = 0;
    for (double t = 0.0; t + gap <= 2000.0 && n < PDS_MAX_IV; t += gap, n++) g_iv[n] = (float)(gap + ((n & 1) ? jit : -jit));
    return n;
}
static int verdict(double hz, double present_hz, double* fper)
{
    return pds_window_verdict(g_iv, (int)(2000.0 / (1000.0 / hz)), 2000.0, 1000.0 / hz, present_hz, fper);
}

/* ---- adaptive step-down: a simulated PC (each frame needs work_ms, then waits for its next k-refresh
 * slot with sync interval k, or for the next refresh after the plan period with Vulkan FIFO (g_fifo)) fed
 * through pds_adapt_frame with the plan it picks ---- */

static pds_adapt g_ad;
static int g_fifo;
static double g_t;
static pds_plan g_p;
static int g_nchg, g_why[64];
static double g_chg_t[64];
static void sim(double hz, int mult, double max_real, double work_ms, double ms)
{
    const int me = pds_plan_max_extra(hz, mult, max_real, 30.0);
    const double end = g_t + ms;
    while (g_t < end) {
        pds_make_plan_step(hz, mult, max_real, 30.0, g_ad.extra, &g_p);
        const double slot = 1000.0 / g_p.real;
        const double ref = 1000.0 / hz;
        if (g_fifo) { const double r = ceil(work_ms / ref - 1e-9) * ref; g_t += r > slot ? r : slot; }
        else { const double n = ceil(work_ms / slot - 1e-9); g_t += slot * (n < 1.0 ? 1.0 : n); }
        if (pds_adapt_frame(&g_ad, g_t, slot, ref, me)) {
            if (g_nchg < 64) { g_why[g_nchg] = g_ad.why; g_chg_t[g_nchg] = g_t; }
            g_nchg++;
        }
    }
    pds_make_plan_step(hz, mult, max_real, 30.0, g_ad.extra, &g_p);
}
static void sim_reset(int enabled) { pds_adapt_init(&g_ad, enabled); g_t = 1000.0; g_nchg = 0; g_fifo = 0; }

static void test_adapt(void)
{
    pds_plan p, q;
    const double m60 = pds_max_real(60.0);
    /* stepped plans: one refresh more per frame, never under 30 rendered frames/s */
    CHECK(pds_make_plan_step(144.0, 1, m60, 30.0, 0, &p) == 0 && p.k == 2 && NEAR(p.real, 72.0));
    pds_make_plan(144.0, 1, m60, 30.0, &q);
    CHECK(pds_plan_same(&p, &q) && p.limiter == q.limiter);
    CHECK(pds_make_plan_step(144.0, 1, m60, 30.0, 1, &p) == 1 && p.k == 3 && p.sync == 3 && NEAR(p.real, 48.0) && p.limiter > p.real);
    CHECK(pds_make_plan_step(144.0, 1, m60, 30.0, 2, &p) == 2 && p.k == 4 && NEAR(p.real, 36.0));
    CHECK(pds_make_plan_step(144.0, 1, m60, 30.0, 3, &p) == 2 && p.k == 4);   /* 28.8 would slow the game */
    CHECK(pds_plan_max_extra(144.0, 1, m60, 30.0) == 2);
    CHECK(pds_make_plan_step(165.0, 1, m60, 30.0, 1, &p) == 1 && p.k == 4 && NEAR(p.real, 41.25));
    CHECK(pds_make_plan_step(165.0, 1, pds_max_real(90.0), 30.0, 1, &p) == 1 && p.k == 3 && NEAR(p.real, 55.0));   /* 82.5 -> 55 */
    CHECK(pds_make_plan_step(144.0, 2, m60, 30.0, 1, &p) == 1 && p.k == 2 && p.sync == 1 && NEAR(p.real, 36.0)
          && NEAR(p.shown, 72.0) && p.limiter == p.real);
    CHECK(pds_plan_max_extra(144.0, 2, m60, 30.0) == 1);
    CHECK(pds_plan_max_extra(60.0, 1, m60, 30.0) == 1);    /* 60 -> 30 */
    CHECK(pds_make_plan_step(60.0, 3, m60, 30.0, 1, &p) == 0 && p.k == 0);   /* no plan stays no plan */
    CHECK(pds_plan_max_extra(60.0, 2, m60, 30.0) == 0);

    CHECK(pds_parse_adapt(NULL) && pds_parse_adapt("") && pds_parse_adapt("1") && pds_parse_adapt("on"));
    CHECK(!pds_parse_adapt("0") && !pds_parse_adapt("OFF") && !pds_parse_adapt("no") && !pds_parse_adapt("False"));

    /* #38: 144 Hz without VRR, setting 60 (72 planned), the PC does ~62 fps: 72 misses every slot (a steady
     * 36 before); within one window it steps to 48 and stays there */
    sim_reset(1);
    sim(144.0, 1, m60, 16.0, 2500.0);
    CHECK(g_nchg == 1 && g_why[0] == PDS_AD_SLOWER && g_ad.extra == 1 && g_p.k == 3 && NEAR(g_p.real, 48.0));
    CHECK(g_chg_t[0] - 1000.0 < 2200.0);
    { pds_plan was; pds_make_plan(144.0, 1, m60, 30.0, &was); char b[192]; pds_adapt_str(&g_ad, &g_p, &was, b, sizeof b);
      CHECK(strstr(b, "stepping to 48.00 fps (k=3) -- 72.00 fps missed") != NULL); }
    sim(144.0, 1, m60, 16.0, 7000.0);
    CHECK(g_nchg == 1);
    /* 10 s clean at 48: tries 72, which fails within a second; back to 48, next try after 20 s */
    sim(144.0, 1, m60, 16.0, 4000.0);
    CHECK(g_nchg == 3 && g_why[1] == PDS_AD_TRY_FASTER && g_why[2] == PDS_AD_BACK && g_ad.extra == 1);
    CHECK(g_chg_t[1] - g_chg_t[0] >= 10000.0 && g_chg_t[2] - g_chg_t[1] < 1200.0);
    CHECK(g_ad.hold == 20000.0);
    { pds_plan was; pds_make_plan(144.0, 1, m60, 30.0, &was); char b[192]; pds_adapt_str(&g_ad, &g_p, &was, b, sizeof b);
      CHECK(strstr(b, "still misses") != NULL && strstr(b, "next try in 20 s") != NULL); }
    sim(144.0, 1, m60, 16.0, 19000.0);
    CHECK(g_nchg == 3);
    sim(144.0, 1, m60, 16.0, 3000.0);
    CHECK(g_nchg == 5 && g_ad.hold == 40000.0 && g_ad.extra == 1);
    /* the waits double up to 2 minutes */
    sim(144.0, 1, m60, 16.0, 600000.0);
    CHECK(g_ad.hold == PDS_AD_HOLD_MAX_MS && g_ad.extra == 1);
    { const int n = g_nchg; sim(144.0, 1, m60, 16.0, 115000.0); CHECK(g_nchg == n || g_nchg == n + 2); }

    /* the PC gets faster (lighter scene): back to 72 after 10 s, and it stays */
    sim_reset(1);
    sim(144.0, 1, m60, 16.0, 3000.0);
    CHECK(g_ad.extra == 1);
    sim(144.0, 1, m60, 10.0, 12000.0);
    CHECK(g_ad.extra == 0 && g_why[g_nchg - 1] == PDS_AD_TRY_FASTER && g_p.k == 2);
    sim(144.0, 1, m60, 10.0, 60000.0);
    CHECK(g_ad.extra == 0 && g_nchg == 2 && !g_ad.probing);
    /* ... slower again soon after (within 30 s): the next try waits twice as long */
    sim(144.0, 1, m60, 16.0, 3000.0);
    CHECK(g_ad.extra == 1 && g_ad.hold == 10000.0);   /* 60+ s later: a fresh start */
    sim(144.0, 1, m60, 10.0, 15000.0);
    CHECK(g_ad.extra == 0);
    sim(144.0, 1, m60, 16.0, 3000.0);
    CHECK(g_ad.extra == 1 && g_ad.hold == 20000.0);

    /* a PC that holds the plan: never touched; never faster than the setting */
    sim_reset(1);
    sim(144.0, 1, m60, 10.0, 120000.0);
    CHECK(g_nchg == 0 && g_ad.extra == 0 && g_p.k == 2);
    /* a few late frames (< 20%): no step */
    sim_reset(1);
    for (int i = 0; i < 30; i++) { sim(144.0, 1, m60, 10.0, 950.0); sim(144.0, 1, m60, 16.0, 50.0); }
    CHECK(g_nchg == 0 && g_ad.extra == 0);
    /* still too slow at 48 (~38 fps): on to 36, not below 30 */
    sim_reset(1);
    sim(144.0, 1, m60, 26.0, 10000.0);
    CHECK(g_ad.extra == 2 && g_p.k == 4 && NEAR(g_p.real, 36.0));
    sim(144.0, 1, m60, 40.0, 10000.0);
    CHECK(g_ad.extra == 2);

    /* loading: a stall of 1 s or more and a slow loading screen (under 20 frames/s) change nothing */
    sim_reset(1);
    sim(144.0, 1, m60, 10.0, 3000.0);
    g_t += 1500.0;
    sim(144.0, 1, m60, 10.0, 3000.0);
    sim(144.0, 1, m60, 100.0, 8000.0);
    for (int i = 0; i < 5; i++) { g_t += 1200.0; sim(144.0, 1, m60, 30.0, 900.0); }
    CHECK(g_nchg == 0 && g_ad.extra == 0);
    /* ... nor does a stall during a try count against it */
    sim_reset(1);
    sim(144.0, 1, m60, 16.0, 3000.0);
    CHECK(g_ad.extra == 1);
    for (int i = 0; i < 200 && !g_ad.probing; i++) sim(144.0, 1, m60, 10.0, 100.0);
    CHECK(g_ad.probing && g_ad.extra == 0);
    { const int n = g_nchg;
      g_t += 2000.0;
      sim(144.0, 1, m60, 10.0, 200.0);
      g_t += 1100.0;
      sim(144.0, 1, m60, 10.0, 4000.0);
      CHECK(g_ad.extra == 0 && !g_ad.probing && g_nchg == n); }

    /* frame generation 2x at 144 Hz: 72 rendered -> 36 rendered (72 shown) */
    sim_reset(1);
    sim(144.0, 2, m60, 16.0, 3000.0);
    CHECK(g_ad.extra == 1 && g_p.k == 2 && NEAR(g_p.real, 36.0));
    /* 165 Hz: 55 -> 41.25 */
    sim_reset(1);
    sim(165.0, 1, m60, 20.0, 3000.0);
    CHECK(g_ad.extra == 1 && g_p.k == 4 && NEAR(g_p.real, 41.25));
    /* the refresh rate / limit changes so fewer steps fit: clamped */
    CHECK(pds_adapt_frame(&g_ad, g_t + 10.0, 1000.0 / 60.0, 1000.0 / 165.0, 0) == 1 && g_ad.extra == 0);
    /* R2 / R3 (Vulkan FIFO): a late frame shows one refresh later (2 or 3 at 144 Hz: judder, not 36);
     * a PC doing ~60 still steps to an even 48, and one doing ~100 is left at 72 */
    sim_reset(1); g_fifo = 1;
    sim(144.0, 1, m60, 16.0, 3000.0);
    CHECK(g_ad.extra == 1 && g_p.k == 3 && g_nchg == 1);
    sim(144.0, 1, m60, 16.0, 9000.0);
    CHECK(g_nchg == 1);
    sim_reset(1); g_fifo = 1;
    sim(144.0, 1, m60, 10.0, 60000.0);
    CHECK(g_nchg == 0 && g_ad.extra == 0);
    /* 240 Hz, k = 4 (60 fps): one refresh late is caught too */
    sim_reset(1); g_fifo = 1;
    sim(240.0, 1, m60, 18.0, 3000.0);
    CHECK(g_ad.extra == 1 && g_p.k == 5 && NEAR(g_p.real, 48.0));
    /* PORT_PACING_ADAPT=0: the old behaviour */    sim_reset(0);
    sim(144.0, 1, m60, 16.0, 30000.0);
    CHECK(g_nchg == 0 && g_ad.extra == 0 && g_p.k == 2);
}

int main(void)
{
    double fper = 0.0;
    /* 165 Hz without VRR, 120 frames/s shown: the vblanks keep the refresh period */
    fill(1000.0 / 165.0, 0.05);
    CHECK(verdict(165.0, 120.0, &fper) == PDS_V_FIXED);
    CHECK(fabs(fper - 1000.0 / 165.0) < 0.01);
    /* a few late / missed vblank waits don't change it */
    g_iv[3] = 12.1f; g_iv[50] = 12.1f; g_iv[90] = 3.0f;
    CHECK(verdict(165.0, 120.0, NULL) == PDS_V_FIXED);
    /* 165 Hz with VRR: the refresh follows 120 frames/s (8.33 ms), 82.5 (12.1 ms), a 60/s movie (16.7 ms) */
    { const int n = fill(1000.0 / 120.0, 0.3);
      CHECK(pds_window_verdict(g_iv, n, 2000.0, 1000.0 / 165.0, 120.0, &fper) == PDS_V_VRR); }
    { const int n = fill(1000.0 / 82.5, 0.3);
      CHECK(pds_window_verdict(g_iv, n, 2000.0, 1000.0 / 165.0, 82.5, NULL) == PDS_V_VRR); }
    { const int n = fill(1000.0 / 59.94, 0.5);
      CHECK(pds_window_verdict(g_iv, n, 2000.0, 1000.0 / 165.0, 59.94, NULL) == PDS_V_VRR); }
    /* 144 Hz fixed, 60 frames/s */
    { const int n = fill(1000.0 / 144.0, 0.1);
      CHECK(pds_window_verdict(g_iv, n, 2000.0, 1000.0 / 144.0, 60.0, &fper) == PDS_V_FIXED); }
    /* at (or near) the refresh rate both look alike; paused / loading: nothing to tell */
    fill(1000.0 / 165.0, 0.05);
    CHECK(verdict(165.0, 160.0, NULL) == PDS_V_SATURATED);
    CHECK(verdict(165.0, 10.0, NULL) == PDS_V_IDLE);
    /* the vblank waits stopped coming back: unknown */
    CHECK(pds_window_verdict(g_iv, 5, 2000.0, 1000.0 / 165.0, 120.0, NULL) == PDS_V_UNKNOWN);
    /* half and half: unknown */
    { const int n = fill(1000.0 / 165.0, 0.0);
      for (int i = 0; i < n; i += 2) g_iv[i] = 8.33f;
      CHECK(pds_window_verdict(g_iv, n, 2000.0, 1000.0 / 165.0, 120.0, NULL) == PDS_V_UNKNOWN); }
    CHECK(pds_window_verdict(g_iv, 100, 2000.0, 0.0, 120.0, NULL) == PDS_V_UNKNOWN);

    /* hysteresis: 3 FIXED in a row to switch on, 2 VRR in a row back; any other verdict breaks the run */
    { pds_decider d; pds_decider_init(&d, PDS_MODE_AUTO);
      CHECK(!d.fixed);
      CHECK(!pds_decide(&d, PDS_V_FIXED));
      CHECK(!pds_decide(&d, PDS_V_FIXED));
      CHECK(!pds_decide(&d, PDS_V_SATURATED));   /* broke the run */
      CHECK(!pds_decide(&d, PDS_V_FIXED));
      CHECK(!pds_decide(&d, PDS_V_FIXED));
      CHECK(pds_decide(&d, PDS_V_FIXED) && d.fixed);
      CHECK(!pds_decide(&d, PDS_V_FIXED));
      CHECK(!pds_decide(&d, PDS_V_VRR));
      CHECK(!pds_decide(&d, PDS_V_FIXED));   /* broke the VRR run */
      CHECK(!pds_decide(&d, PDS_V_VRR));
      CHECK(!pds_decide(&d, PDS_V_IDLE));    /* so does IDLE */
      CHECK(!pds_decide(&d, PDS_V_VRR) && d.fixed);
      CHECK(pds_decide(&d, PDS_V_VRR) && !d.fixed);
      int sw = 2;   /* FIXED, then VRR again */
      for (int i = 0; i < 60; i++) sw += pds_decide(&d, (i / 3) & 1 ? PDS_V_VRR : PDS_V_FIXED);
      CHECK(sw == PDS_MAX_SWITCHES); }
    /* a VRR user near the refresh rate (SATURATED) with stray FIXED windows (alt-tab) is never switched */
    { pds_decider d; pds_decider_init(&d, PDS_MODE_AUTO);
      int sw = 0;
      for (int i = 0; i < 200; i++) sw += pds_decide(&d, i % 5 == 0 || i % 7 == 0 ? PDS_V_FIXED : i % 11 == 0 ? PDS_V_UNKNOWN : PDS_V_SATURATED);
      for (int i = 0; i < 50; i++) sw += pds_decide(&d, i % 3 == 2 ? PDS_V_SATURATED : PDS_V_FIXED);   /* 2 in a row at most */
      CHECK(sw == 0 && !d.fixed); }
    { pds_decider d; pds_decider_init(&d, PDS_MODE_FIXED);
      CHECK(d.fixed);
      CHECK(!pds_decide(&d, PDS_V_VRR) && !pds_decide(&d, PDS_V_VRR) && d.fixed); }
    { pds_decider d; pds_decider_init(&d, PDS_MODE_VRR);
      CHECK(!pds_decide(&d, PDS_V_FIXED) && !pds_decide(&d, PDS_V_FIXED) && !d.fixed); }

    CHECK(pds_parse_mode(NULL) == PDS_MODE_AUTO && pds_parse_mode("") == PDS_MODE_AUTO && pds_parse_mode("Auto") == PDS_MODE_AUTO);
    CHECK(pds_parse_mode("VRR") == PDS_MODE_VRR && pds_parse_mode("gsync") == PDS_MODE_VRR && pds_parse_mode("freesync") == PDS_MODE_VRR);
    CHECK(pds_parse_mode("fixed") == PDS_MODE_FIXED && pds_parse_mode("vsync") == PDS_MODE_FIXED);
    CHECK(pds_parse_mode("off") == PDS_MODE_OFF && pds_parse_mode("0") == PDS_MODE_OFF);
    CHECK(pds_parse_mode("bogus") == PDS_MODE_AUTO);

    CHECK(pds_max_real(60.0) == 75.0 && pds_max_real(30.0) == 30.0 && pds_max_real(90.0) == 90.0 && pds_max_real(0.0) == 1000.0);

    /* the plans of the header's table (frame-rate setting 60) */
    pds_plan p;
    pds_make_plan(165.0, 1, pds_max_real(60.0), 30.0, &p);
    CHECK(p.k == 3 && p.sync == 3 && NEAR(p.real, 55.0) && p.limiter > 55.0 && p.limiter < 55.3);
    pds_make_plan(165.0, 2, pds_max_real(60.0), 30.0, &p);
    CHECK(p.k == 2 && p.sync == 1 && NEAR(p.shown, 82.5) && NEAR(p.real, 41.25) && p.limiter == p.real);
    pds_make_plan(144.0, 1, pds_max_real(60.0), 30.0, &p);
    CHECK(p.k == 2 && p.sync == 2 && NEAR(p.real, 72.0));
    pds_make_plan(144.0, 2, pds_max_real(60.0), 30.0, &p);
    CHECK(p.k == 1 && p.sync == 1 && NEAR(p.shown, 144.0) && NEAR(p.real, 72.0) && p.limiter > p.real);
    pds_make_plan(120.0, 1, pds_max_real(60.0), 30.0, &p);
    CHECK(p.k == 2 && p.sync == 2 && NEAR(p.real, 60.0));
    pds_make_plan(120.0, 2, pds_max_real(60.0), 30.0, &p);
    CHECK(p.k == 1 && NEAR(p.shown, 120.0) && NEAR(p.real, 60.0));
    pds_make_plan(240.0, 1, pds_max_real(60.0), 30.0, &p);
    CHECK(p.k == 4 && p.sync == 4 && NEAR(p.real, 60.0));
    pds_make_plan(240.0, 2, pds_max_real(60.0), 30.0, &p);
    CHECK(p.k == 2 && p.sync == 1 && NEAR(p.shown, 120.0) && NEAR(p.real, 60.0) && p.limiter == p.real);
    pds_make_plan(60.0, 1, pds_max_real(60.0), 30.0, &p);
    CHECK(p.k == 1 && p.sync == 1 && NEAR(p.real, 60.0));
    pds_make_plan(60.0, 2, pds_max_real(60.0), 30.0, &p);
    CHECK(p.k == 1 && NEAR(p.shown, 60.0) && NEAR(p.real, 30.0));
    pds_make_plan(165.0, 3, pds_max_real(60.0), 30.0, &p);   /* DLSS-G 3x: 55 -> 165 */
    CHECK(p.k == 1 && NEAR(p.real, 55.0) && NEAR(p.shown, 165.0));
    /* the measured rate, not the nominal one */
    pds_make_plan(164.98, 1, pds_max_real(60.0), 30.0, &p);
    CHECK(p.k == 3 && fabs(p.real - 164.98 / 3.0) < 1e-9);
    /* frame-rate setting 30 at 165 Hz: 27.5 would slow the game down, 33 instead */
    pds_make_plan(165.0, 1, pds_max_real(30.0), 30.0, &p);
    CHECK(p.k == 5 && NEAR(p.real, 33.0));
    /* uncapped / above 75: the player's choice */
    pds_make_plan(165.0, 1, pds_max_real(0.0), 30.0, &p);
    CHECK(p.k == 1 && NEAR(p.real, 165.0));
    pds_make_plan(165.0, 2, pds_max_real(90.0), 30.0, &p);
    CHECK(p.k == 1 && NEAR(p.real, 82.5));
    /* even k = 1 too slow for the game: no plan (not paced), hz / mult kept for the log */
    pds_make_plan(60.0, 3, pds_max_real(60.0), 30.0, &p);   /* 20 fps */
    CHECK(p.k == 0 && p.mult == 3 && NEAR(p.hz, 60.0));
    { char b[192]; pds_plan_str(&p, b, sizeof b); CHECK(strstr(b, "not paced") != NULL); }
    pds_make_plan(50.0, 2, pds_max_real(60.0), 30.0, &p);   /* 25 fps */
    CHECK(p.k == 0);
    pds_make_plan(50.0, 1, pds_max_real(30.0), 30.0, &p);   /* 50, the only even rate */
    CHECK(p.k == 1 && NEAR(p.real, 50.0));
    /* unknown refresh: no plan */
    pds_make_plan(0.0, 1, 75.0, 30.0, &p);
    CHECK(p.k == 0);
    { pds_plan a, b; pds_make_plan(165.0, 2, 75.0, 30.0, &a); pds_make_plan(165.01, 2, 75.0, 30.0, &b);
      CHECK(pds_plan_same(&a, &b));
      pds_make_plan(165.0, 1, 75.0, 30.0, &b);
      CHECK(!pds_plan_same(&a, &b));
      char s[192]; pds_plan_str(&a, s, sizeof s);
      CHECK(strstr(s, "41.25 fps rendered -> 82.50 shown") != NULL); }

    /* the phase lock's error: mid-refresh = 0, late in the period > 0, just after a vblank < 0 */
    CHECK(fabs(pds_phase_err(3.0, 6.0)) < 1e-9);
    CHECK(fabs(pds_phase_err(4.5, 6.0) - 0.25) < 1e-9);
    CHECK(fabs(pds_phase_err(0.6, 6.0) + 0.4) < 1e-9);
    CHECK(fabs(pds_phase_err(13.5, 6.0) + 0.25) < 1e-9);   /* a missed vblank wake: same phase */

    test_adapt();

    if (fails) { fprintf(stderr, "display_sync_test: %d failure(s)\n", fails); return 1; }
    printf("display_sync_test: OK\n");
    return 0;
}
