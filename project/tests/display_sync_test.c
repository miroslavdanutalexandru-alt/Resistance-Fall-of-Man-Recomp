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

    if (fails) { fprintf(stderr, "display_sync_test: %d failure(s)\n", fails); return 1; }
    printf("display_sync_test: OK\n");
    return 0;
}
