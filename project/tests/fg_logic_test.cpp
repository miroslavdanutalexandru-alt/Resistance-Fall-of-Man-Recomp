// Unit test for the frame generation decision logic shared by the three ports (no GPU, no game).
// R2/R3: clang-cl /nologo /O1 /EHsc /std:c++17 /I ps3recomp\third_party\rpcs3\rpcs3\Emu\RSX\VK project\tests\fg_logic_test.cpp /Fe:scratch\fg_logic_test.exe /Fo:scratch\fg_logic_test.obj
// R1:    clang-cl /nologo /O1 /EHsc /std:c++17 /DPFG_R1 /I ps3recomp\libs\video project\tests\fg_logic_test.cpp /Fe:scratch\fg_logic_test.exe /Fo:scratch\fg_logic_test.obj
#ifdef PFG_R1
#include "port_fg_logic.h"
#include "rfom_fg_cam.h"
#include "rsx_vp_cpu.h"
#else
#include "VKR2FrameGenLogic.h"
#endif
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>

static int g_fail, g_checks;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

// ---- the R2/R3 fgcfg() parse before PORT_FG, verbatim (penv returns nullptr for unset or empty)
static bool same_word(const char* a, const char* b)
{
	if (!a || !b) return false;
	for (; *a && *b; ++a, ++b)
		if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) return false;
	return *a == *b;
}
static void old_parse(const char* fg, const char* b, int& want, int& backend, int& bad_mult, int& bad_backend)
{
	if (fg && !*fg) fg = nullptr;
	if (b && !*b) b = nullptr;
	const bool fsr_word = same_word(fg, "fsr3") || same_word(fg, "fsr");
	const int v = fsr_word ? 2 : (fg ? std::atoi(fg) : 0);
	want = (v >= 2 && v <= 4) ? v : 0;
	bad_mult = (v && !want) ? v : 0;
	backend = fsr_word ? 2 : same_word(b, "dlss") ? 1 : (same_word(b, "fsr3") || same_word(b, "fsr")) ? 2 : 0;
	bad_backend = b && !backend && !same_word(b, "auto");
}

static void test_legacy_equivalence()
{
	const char* fgs[] = { nullptr, "", "0", "1", "2", "3", "4", "5", "-1", "fsr3", "FSR3", "fsr", "Fsr", "fsr4", "on", "2x", " 2" };
	const char* bes[] = { nullptr, "", "auto", "AUTO", "dlss", "DLSS", "fsr3", "fsr", "xess", "dlssg" };
	for (const char* fg : fgs)
		for (const char* b : bes)
		{
			int w0, b0, m0, x0, w1, b1, m1, x1;
			old_parse(fg, b, w0, b0, m0, x0);
			pfg_parse_vk_legacy(fg, b, &w1, &b1, &m1, &x1);
			CHECK(w0 == w1 && b0 == b1 && m0 == m1 && x0 == x1);
		}
}

static void test_selector()
{
	CHECK(pfg_parse_sel(nullptr) == PFG_SEL_LEGACY);
	CHECK(pfg_parse_sel("") == PFG_SEL_LEGACY);
	CHECK(pfg_parse_sel("auto") == PFG_SEL_LEGACY);
	CHECK(pfg_parse_sel("dlssg") == PFG_SEL_DLSSG);
	CHECK(pfg_parse_sel("DLSS-G") == PFG_SEL_DLSSG);
	CHECK(pfg_parse_sel("dlss") == PFG_SEL_DLSSG);
	CHECK(pfg_parse_sel("FSR3") == PFG_SEL_FSR3);
	CHECK(pfg_parse_sel("fsr") == PFG_SEL_FSR3);
	CHECK(pfg_parse_sel("off") == PFG_SEL_OFF);
	CHECK(pfg_parse_sel("0") == PFG_SEL_OFF);
	CHECK(pfg_parse_sel("none") == PFG_SEL_OFF);
	CHECK(pfg_parse_sel("xess") == PFG_SEL_BAD);
	CHECK(pfg_parse_sel("fsr31") == PFG_SEL_BAD);

	// legacy untouched
	int w = 3, b = 0;
	pfg_apply_sel(PFG_SEL_LEGACY, &w, &b); CHECK(w == 3 && b == 0);
	pfg_apply_sel(PFG_SEL_BAD, &w, &b); CHECK(w == 3 && b == 0);
	// dlssg keeps 3x / 4x, else 2x, DLSS-G only
	w = 3; b = 2; pfg_apply_sel(PFG_SEL_DLSSG, &w, &b); CHECK(w == 3 && b == PFG_BE_DLSS);
	w = 0; b = 0; pfg_apply_sel(PFG_SEL_DLSSG, &w, &b); CHECK(w == 2 && b == PFG_BE_DLSS);
	// fsr3 is 2x on FSR 3, even when R?_DLSS_FG=4
	w = 4; b = 1; pfg_apply_sel(PFG_SEL_FSR3, &w, &b); CHECK(w == 2 && b == PFG_BE_FSR3);
	w = 0; b = 0; pfg_apply_sel(PFG_SEL_FSR3, &w, &b); CHECK(w == 2 && b == PFG_BE_FSR3);
	// off wins over R?_DLSS_FG=fsr3
	w = 2; b = 2; pfg_apply_sel(PFG_SEL_OFF, &w, &b); CHECK(w == 0);

	CHECK(pfg_hudless(nullptr, 1) == 1);
	CHECK(pfg_hudless(nullptr, 0) == 0);
	CHECK(pfg_hudless("", 0) == 0);
	CHECK(pfg_hudless("1", 0) == 1);
	CHECK(pfg_hudless("0", 1) == 0);
	CHECK(pfg_hudless("off", 1) == 0);
	CHECK(pfg_hudless("on", 0) == 1);

	CHECK(pfg_parse_pace(nullptr) == PFG_PACE_VRR);
	CHECK(pfg_parse_pace("vrr") == PFG_PACE_VRR);
	CHECK(pfg_parse_pace("junk") == PFG_PACE_VRR);
	CHECK(pfg_parse_pace("auto") == PFG_PACE_AUTO);
	CHECK(pfg_parse_pace("VSYNC") == PFG_PACE_AUTO);
	CHECK(pfg_parse_pace("off") == PFG_PACE_OFF);
}

// Rendered presents per second for a game capped at cap_fps, through the FSR 3 pacer (x2 = output fps).
// Mirrors fx_queue_present: wait until next, then next = pfg_pace_next(now, next, step). Units: microseconds.
static double sim_rendered_fps(double cap_fps, double step_s, double seconds = 10.0, long long stall_at = -1, long long stall_us = 0)
{
	const long long frame = (long long)std::llround(1e6 / cap_fps), step = (long long)std::llround(step_s * 1e6);
	const long long end = (long long)(seconds * 1e6);
	long long t = 0, next = 0, n = 0, game_next = 0;
	while (t < end)
	{
		t = std::max(t, game_next);                    // the game's own limiter
		if (stall_at >= 0 && n == stall_at) t += stall_us;
		if (step > 0)
		{
			if (next > t) t = next;
			next = pfg_pace_next(t, next, step);
		}
		game_next = t + frame;
		++n;
	}
	return n / seconds;
}

static void test_pacing()
{
	// default (vrr): 0.95 margin, like before PORT_FG_PACE
	CHECK(std::fabs(pfg_fx_step_s(1, PFG_PACE_VRR, 120.0, 1) - 2.0 / 114.0) < 1e-12);
	CHECK(pfg_fx_step_s(0, PFG_PACE_VRR, 120.0, 1) == 0.0);        // R?_DLSS_FG_CAP=0
	CHECK(pfg_fx_step_s(1, PFG_PACE_VRR, 0.0, 0) == 0.0);          // unknown refresh
	CHECK(pfg_fx_step_s(1, PFG_PACE_OFF, 120.0, 0) == 0.0);
	CHECK(pfg_fx_step_s(1, PFG_PACE_AUTO, 120.0, 1) == 0.0);       // vsync: the presenter paces
	CHECK(pfg_fx_step_s(1, PFG_PACE_AUTO, 120.0, 0) > 0.0);        // immediate / mailbox: still paced

	double fps = sim_rendered_fps(60.0, pfg_fx_step_s(1, PFG_PACE_VRR, 120.0, 0));
	std::printf("  60 cap, 120 Hz, vrr : %.1f rendered -> %.1f output\n", fps, fps * 2);
	CHECK(fps > 56.5 && fps < 57.5);
	fps = sim_rendered_fps(60.0, pfg_fx_step_s(1, PFG_PACE_AUTO, 120.0, 1));
	std::printf("  60 cap, 120 Hz, auto: %.1f rendered -> %.1f output\n", fps, fps * 2);
	CHECK(fps > 59.5 && fps < 60.5);
	fps = sim_rendered_fps(60.0, pfg_fx_step_s(1, PFG_PACE_VRR, 144.0, 0));
	std::printf("  60 cap, 144 Hz, vrr : %.1f rendered -> %.1f output\n", fps, fps * 2);
	CHECK(fps > 59.5 && fps < 60.5);
	fps = sim_rendered_fps(60.0, pfg_fx_step_s(1, PFG_PACE_VRR, 60.0, 0));
	std::printf("  60 cap,  60 Hz, vrr : %.1f rendered -> %.1f output\n", fps, fps * 2);
	CHECK(fps > 28.0 && fps < 29.0);

	// catch-up: after a stall the deadline restarts from now (no burst of back-to-back presents)
	CHECK(pfg_pace_next(1000, 900, 50) == 1050);      // fell behind by 100 > step: now + step
	CHECK(pfg_pace_next(1000, 980, 50) == 1030);      // within one step: keep the cadence
	CHECK(pfg_pace_next(1000, 1000, 50) == 1050);
	fps = sim_rendered_fps(60.0, pfg_fx_step_s(1, PFG_PACE_VRR, 144.0, 0), 10.0, 100, 500000);
	CHECK(fps > 56.5 && fps < 60.5);

	CHECK(pfg_reflex_limit_us(1, 120.0, 1, 0) == (unsigned)(1e6 / 114.0));
	CHECK(pfg_reflex_limit_us(1, 120.0, 0, 1) == (unsigned)(1e6 / 114.0));
	CHECK(pfg_reflex_limit_us(1, 120.0, 0, 0) == 0);
	CHECK(pfg_reflex_limit_us(0, 120.0, 1, 0) == 0);
	CHECK(pfg_reflex_limit_us(1, 1.0, 1, 0) == 0);
}

static void test_hold()
{
	unsigned hold = 0;
	int prev = 0;
	// first frame on: enabled, reset (no history)
	int gen = pfg_fx_hold_step(&hold, 1, prev, 1);
	CHECK(gen && hold == PFG_FX_HOLD && pfg_fx_reset(prev, 0, 0, 1));
	prev = gen;
	gen = pfg_fx_hold_step(&hold, 1, prev, 1);
	CHECK(gen && !pfg_fx_reset(prev, 0, 0, 1));
	CHECK(pfg_fx_reset(prev, 1, 0, 1));   // new swapchain (resize / alt-tab fullscreen)
	CHECK(pfg_fx_reset(prev, 0, 1, 1));   // camera cut
	prev = gen;
	// pause / short gap: held enabled (as reset frames) for PFG_FX_HOLD - 1 frames, then off
	int held = 0;
	for (int i = 0; i < 40; ++i)
	{
		gen = pfg_fx_hold_step(&hold, 0, prev, 1);
		if (gen) { ++held; CHECK(pfg_fx_reset(prev, 0, 0, 0)); }
		prev = gen;
	}
	CHECK(held == (int)PFG_FX_HOLD - 1 && !gen && hold == 0);
	// once off it stays off until wanted again (prev_gen = 0)
	hold = 5;
	CHECK(!pfg_fx_hold_step(&hold, 0, 0, 1));
	// losing depth / motion vectors (movie, loading) ends the hold at once
	hold = 0; prev = pfg_fx_hold_step(&hold, 1, 0, 1);
	CHECK(!pfg_fx_hold_step(&hold, 0, prev, 0));
	// back on: enabled immediately, reset first frame
	CHECK(pfg_fx_hold_step(&hold, 1, 0, 1) && pfg_fx_reset(0, 0, 0, 1));
}

static void test_gate()
{
	pfg_gate_in g{};
	g.scene = 1; g.local_players = 1; g.now_ms = 10000; g.activity_ms = 9900; g.streak = 3; g.menu_off = 1;
	CHECK(pfg_gate(&g) == PFG_GATE_ON);
	{ auto h = g; h.streak = 2; CHECK(pfg_gate(&h) == PFG_GATE_WARMUP); }
	{ auto h = g; h.scene = 0; h.paused = 1; CHECK(pfg_gate(&h) == PFG_GATE_NO_SCENE); }        // order: scene first
	{ auto h = g; h.paused = 1; h.local_players = 2; CHECK(pfg_gate(&h) == PFG_GATE_PAUSED); }
	{ auto h = g; h.local_players = 2; CHECK(pfg_gate(&h) == PFG_GATE_SPLITSCREEN); }
	{ auto h = g; h.scrgb = 1; h.srgb = 1; CHECK(pfg_gate(&h) == PFG_GATE_SCRGB); }
	{ auto h = g; h.srgb = 1; CHECK(pfg_gate(&h) == PFG_GATE_SRGB); }
	{ auto h = g; h.fx_sc_bad = 1; h.pause_ui = 1; CHECK(pfg_gate(&h) == PFG_GATE_FX_SWAPCHAIN); }
	{ auto h = g; h.pause_ui = 1; h.bad_status = 4; CHECK(pfg_gate(&h) == PFG_GATE_MENU); }
	{ auto h = g; h.activity_ms = 8500; CHECK(pfg_gate(&h) == PFG_GATE_ON); }                   // exactly 1.5 s: still on
	{ auto h = g; h.activity_ms = 8499; CHECK(pfg_gate(&h) == PFG_GATE_MENU); }
	{ auto h = g; h.activity_ms = 0; CHECK(pfg_gate(&h) == PFG_GATE_ON); }                      // never had input
	{ auto h = g; h.activity_ms = 1; h.menu_off = 0; h.pause_ui = 1; CHECK(pfg_gate(&h) == PFG_GATE_ON); }
	{ auto h = g; h.bad_status = 1; h.streak = 0; CHECK(pfg_gate(&h) == PFG_GATE_BAD_STATUS); }

	// streak: a scene-less frame (movie / loading) restarts the warm-up
	unsigned s = 0;
	s = pfg_streak(s, 1); s = pfg_streak(s, 1); CHECK(s == 2);
	s = pfg_streak(s, 0); CHECK(s == 0);
	for (int i = 0; i < 3; ++i) s = pfg_streak(s, 1);
	CHECK(s == PFG_WARMUP_FRAMES);
}

#ifdef PFG_R1
// R1: the solved camera M (column vectors, this frame's ndc + depth -> last frame's clip) as Streamline's
// row-vector clipToPrevClip must move static points exactly as the camera fill's motion vectors do, and
// prevClipToClip must bring them back.
static void mul4(const double* a, const double* b, double* o)
{
	double t[16];
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c)
		{
			t[r * 4 + c] = 0;
			for (int k = 0; k < 4; ++k) t[r * 4 + c] += a[r * 4 + k] * b[k * 4 + c];
		}
	std::memcpy(o, t, sizeof t);
}
static void view_proj(double yaw, double pitch, double px, double py, double pz, double* vp)
{
	const double n = 0.25, f = 4000.0, fov = 1.1, asp = 16.0 / 9.0, ys = 1.0 / std::tan(fov * 0.5), xs = ys / asp;
	const double P[16] = { xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, f / (f - n), -n * f / (f - n), 0, 0, 1, 0 };
	const double cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
	const double Ry[16] = { cy, 0, -sy, 0, 0, 1, 0, 0, sy, 0, cy, 0, 0, 0, 0, 1 };
	const double Rx[16] = { 1, 0, 0, 0, 0, cp, -sp, 0, 0, sp, cp, 0, 0, 0, 0, 1 };
	const double T[16] = { 1, 0, 0, -px, 0, 1, 0, -py, 0, 0, 1, -pz, 0, 0, 0, 1 };
	double V[16];
	mul4(Rx, Ry, V);
	mul4(V, T, V);
	mul4(P, V, vp);
}
static void row_mul(const double* v, const float* m, double* o)   // o = v * m (row vector)
{
	for (int j = 0; j < 4; ++j) o[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j] + v[3] * m[12 + j];
}
static void test_cam_hold()
{
    rfom_fgcam_hold_t h; std::memset(&h, 0, sizeof h);
    double A[16] = {0}, B[16] = {0};
    for (int i = 0; i < 16; ++i) { A[i] = i + 1; B[i] = 100 + i; }
    CHECK(rfom_fgcam_hold(&h, nullptr, 0, 0, 3) == nullptr);               // nothing solved yet: identity
    CHECK(rfom_fgcam_hold(&h, A, 0, 0, 3) == A);
    const double* p = rfom_fgcam_hold(&h, nullptr, 0, 0, 3);               // unsolved: the last camera
    CHECK(p && p[0] == 1.0 && p[15] == 16.0);
    CHECK(rfom_fgcam_hold(&h, nullptr, 0, 0, 3) && rfom_fgcam_hold(&h, nullptr, 0, 0, 3));
    CHECK(rfom_fgcam_hold(&h, nullptr, 0, 0, 3) == nullptr);               // older than 3 frames: identity
    CHECK(rfom_fgcam_hold(&h, nullptr, 0, 0, 3) == nullptr);
    CHECK(rfom_fgcam_hold(&h, B, 0, 0, 3) == B);
    p = rfom_fgcam_hold(&h, nullptr, 0, 0, 3); CHECK(p && p[0] == 100.0);   // a new solve restarts the age
    CHECK(rfom_fgcam_hold(&h, B, 0, 0, 3) == B);
    CHECK(rfom_fgcam_hold(&h, nullptr, 1, 0, 3) == nullptr);               // reset / cut: never across it
    CHECK(rfom_fgcam_hold(&h, nullptr, 0, 0, 3) == nullptr);
    CHECK(rfom_fgcam_hold(&h, A, 0, 0, 0) == A);
    CHECK(rfom_fgcam_hold(&h, nullptr, 0, 0, 0) == nullptr);               // RFOM_DLSS_FG_CAM_HOLD=0: old behaviour
    CHECK(rfom_fgcam_hold(&h, A, 0, 0, 3) == A);
    CHECK(rfom_fgcam_hold(&h, B, 0, 1, 3) == B);                        // cut: the jump is used this frame ...
    CHECK(rfom_fgcam_hold(&h, nullptr, 0, 0, 3) == nullptr);            // ... but never held (nor the camera before it)
}

static void test_real_camera()
{
	double cur[16], prev[16], ci[16], M[16];
	view_proj(0.30, 0.05, 1.0, 1.7, -3.0, cur);
	view_proj(0.27, 0.06, 0.9, 1.7, -3.3, prev);   // turning, looking up, walking
	CHECK(rfom_fgcam_inv(cur, ci));
	mul4(prev, ci, M);
	float c2p[16], p2c[16];
	CHECK(rfom_fgcam_sl(M, c2p, p2c) == 1);
	const double pts[][3] = { { 0, 1, 5 }, { 3, 0, 12 }, { -2, 4, 40 }, { 1.5, 1.2, 0.8 }, { 10, -1, 300 } };
	int seen = 0;
	for (const auto& w : pts)
	{
		const double X[4] = { w[0], w[1], w[2], 1.0 };
		double c[4], q[4];
		for (int r = 0; r < 4; ++r)
		{
			c[r] = cur[r * 4] * X[0] + cur[r * 4 + 1] * X[1] + cur[r * 4 + 2] * X[2] + cur[r * 4 + 3];
			q[r] = prev[r * 4] * X[0] + prev[r * 4 + 1] * X[1] + prev[r * 4 + 2] * X[2] + prev[r * 4 + 3];
		}
		if (c[3] <= 0.0 || q[3] <= 0.0) continue;
		++seen;
		const double nd[4] = { c[0] / c[3], c[1] / c[3], c[2] / c[3], 1.0 };
		// the camera fill (rsx_fsr.inc s_cf_hlsl): p = M * (ndc, depth, 1); mv = (p.xy / p.w - ndc) * (0.5, -0.5)
		double p[4];
		for (int r = 0; r < 4; ++r) p[r] = M[r * 4] * nd[0] + M[r * 4 + 1] * nd[1] + M[r * 4 + 2] * nd[2] + M[r * 4 + 3];
		const double mvx = (p[0] / p[3] - nd[0]) * 0.5, mvy = (p[1] / p[3] - nd[1]) * -0.5;
		const double tx = (q[0] / q[3] - nd[0]) * 0.5, ty = (q[1] / q[3] - nd[1]) * -0.5;   // the point's true motion
		CHECK(std::fabs(mvx - tx) < 1e-9 && std::fabs(mvy - ty) < 1e-9);
		// Streamline: prevClip = clip * clipToPrevClip, the same motion
		double s[4];
		row_mul(nd, c2p, s);
		CHECK(s[3] > 0.0);
		CHECK(std::fabs((s[0] / s[3] - nd[0]) * 0.5 - mvx) < 2e-5 && std::fabs((s[1] / s[3] - nd[1]) * -0.5 - mvy) < 2e-5);
		CHECK(std::fabs(s[2] / s[3] - q[2] / q[3]) < 1e-4);   // last frame's depth too
		// round trip: prevClipToClip brings it back
		double b[4];
		row_mul(s, p2c, b);
		CHECK(std::fabs(b[0] / b[3] - nd[0]) < 1e-4 && std::fabs(b[1] / b[3] - nd[1]) < 1e-4 && std::fabs(b[2] / b[3] - nd[2]) < 1e-4);
	}
	CHECK(seen == 5);
	// still camera: M = identity -> zero motion, identity matrices
	{
		const double I[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
		CHECK(rfom_fgcam_sl(I, c2p, p2c) == 1);
		for (int i = 0; i < 16; ++i) CHECK(c2p[i] == (i % 5 == 0 ? 1.0f : 0.0f) && p2c[i] == c2p[i]);
	}
	// no camera / singular / non-finite: identity, reported as such
	CHECK(rfom_fgcam_sl(nullptr, c2p, p2c) == 0);
	for (int i = 0; i < 16; ++i) CHECK(c2p[i] == (i % 5 == 0 ? 1.0f : 0.0f) && p2c[i] == c2p[i]);
	{
		double Z[16] = {};
		CHECK(rfom_fgcam_sl(Z, c2p, p2c) == 0 && c2p[0] == 1.0f && c2p[1] == 0.0f);
		double Nn[16];
		std::memcpy(Nn, M, sizeof Nn);
		Nn[5] = std::nan("");
		CHECK(rfom_fgcam_sl(Nn, c2p, p2c) == 0 && c2p[5] == 1.0f);
	}
	// RFOM_DLSS_FG_REAL_CAM: default on, "0" off
	CHECK(rfom_fgcam_env(nullptr) == 1 && rfom_fgcam_env("") == 1 && rfom_fgcam_env("1") == 1 && rfom_fgcam_env("0") == 0);
}
// RFOM_DLSS_CAM_MEDOID (rsx_vp_cpu.h vpcpu_medoid): candidates that miss the 2e-3 vote by a few pixels
static void cam_m(double y0, double p0, double x0, double z0, double y1, double p1, double x1, double z1, double M[16])
{
	double cur[16], prev[16], ci[16];
	view_proj(y0, p0, x0, 1.7, z0, cur);
	view_proj(y1, p1, x1, 1.7, z1, prev);
	rfom_fgcam_inv(cur, ci);
	mul4(prev, ci, M);
}
static void test_medoid()
{
	static const double I[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	double truth[16];
	cam_m(0.30, 0.05, 1.0, -3.0, 0.26, 0.06, 0.9, -3.3, truth);   // turning + walking
	const double tmo = vpcpu_mdist(truth, I);
	CHECK(tmo > 0.03);
	// 8 candidates: 5 near the truth (prev camera off by up to ~0.2 degrees / 2 cm, a few pixels), 2 identity-like
	// (rows that did not change), 1 far off; no group of 4 within 2e-3 -> the vote fails
	double Ms[8][16];
	const double jit[5][3] = { { 0, 0, 0 }, { 0.002, 0, 0 }, { -0.0015, 0.001, 0.01 }, { 0, -0.002, -0.02 }, { 0.003, 0.0015, 0 } };
	for (int i = 0; i < 5; ++i) cam_m(0.30, 0.05, 1.0, -3.0, 0.26 + jit[i][0], 0.06 + jit[i][1], 0.9 + jit[i][2], -3.3, Ms[i]);
	std::memcpy(Ms[5], I, sizeof I);
	std::memcpy(Ms[6], I, sizeof I);
	cam_m(0.30, 0.05, 1.0, -3.0, 0.10, 0.2, 0.0, -2.0, Ms[7]);
	int agree = 0;
	CHECK(vpcpu_vote(Ms, 8, 2e-3, &agree) < 0);
	double sp, mo;
	const int m = vpcpu_medoid(Ms, 8, 0.02, 1.0 / 3.0, &sp, &mo);
	CHECK(m >= 0 && m < 5);
	if (m >= 0)
	{
		const double err = vpcpu_mdist(Ms[m], truth);
		CHECK(err < 0.25 * tmo);                          // far nearer than "no camera motion"
		CHECK(sp <= 0.02 && sp * 3.0 <= mo);
	}
	// still camera: candidates around identity plus outliers -> no medoid better than identity by 3x -> -1
	{
		double S[6][16];
		for (int i = 0; i < 4; ++i) cam_m(0.3, 0.05, 1.0, -3.0, 0.3 + 0.001 * i, 0.05, 1.0, -3.0, S[i]);
		cam_m(0.3, 0.05, 1.0, -3.0, 0.2, 0.05, 1.0, -3.0, S[4]);
		cam_m(0.3, 0.05, 1.0, -3.0, 0.4, 0.1, 1.0, -3.0, S[5]);
		CHECK(vpcpu_medoid(S, 6, 0.02, 1.0 / 3.0, &sp, &mo) < 0);
	}
	// candidates all over the place: spread over max -> -1 (the old zero-motion fallback)
	{
		double S[4][16];
		for (int i = 0; i < 4; ++i) cam_m(0.3, 0.05, 1.0, -3.0, 0.3 - 0.08 * (i + 1), 0.05 + 0.05 * i, 1.0, -3.0 - 0.3 * i, S[i]);
		CHECK(vpcpu_medoid(S, 4, 0.02, 1.0 / 3.0, &sp, &mo) < 0);
	}
	// fewer than 3 candidates: never
	CHECK(vpcpu_medoid(Ms, 2, 0.02, 1.0 / 3.0, &sp, &mo) < 0);
}
// RFOM_DLSS_CAM_MEDOID_REL (0.75, cap 0.05): r1_1500 shake -- 6 candidates scattered by ~25% of the motion,
// 2 far outliers; the old 1/3 rule leaves it unsolved, the relative rule takes a medoid much nearer than identity
static void test_unsure_pause()
{
    // no HUD runs (cutscene, hidden HUD) is not unsure; a closed run or the classifier giving up is
    CHECK(!rfom_fg_unsure_frame(0, 0, 0));
    CHECK(!rfom_fg_unsure_frame(0, 2, 1));
    CHECK(rfom_fg_unsure_frame(0, 1, 0));
    CHECK(rfom_fg_unsure_frame(1, 0, 0));
    rfom_fg_upause_t s = {0, 0};
    // isolated / alternating unsure frames never pause
    int any = 0;
    for (int i = 0; i < 40; ++i) any |= rfom_fg_upause_step(&s, i % 3 != 2, 3, 15);
    CHECK(!any);
    // 3 in a row pause; stays paused 15 frames after the last unsure one, then resumes
    s = {0, 0};
    CHECK(!rfom_fg_upause_step(&s, 1, 3, 15));
    CHECK(!rfom_fg_upause_step(&s, 1, 3, 15));
    CHECK(rfom_fg_upause_step(&s, 1, 3, 15));
    for (int i = 0; i < 100; ++i) CHECK(rfom_fg_upause_step(&s, 1, 3, 15));
    int off = 0;
    while (rfom_fg_upause_step(&s, 0, 3, 15)) ++off;
    CHECK(off == 15);
    // a single unsure frame during the hold does not extend it beyond 15 after the pause trigger
    s = {0, 0};
    for (int i = 0; i < 3; ++i) rfom_fg_upause_step(&s, 1, 3, 15);
    for (int i = 0; i < 5; ++i) rfom_fg_upause_step(&s, 0, 3, 15);
    CHECK(rfom_fg_upause_step(&s, 1, 3, 15));
    off = 1;
    while (rfom_fg_upause_step(&s, 0, 3, 15)) ++off;
    CHECK(off == 10);
}

// RFOM_DLSS_FG_HUD_HOLD: a frame without a separated HUD keeps the last separated area for < 2 s
static void test_hud_hold()
{
    rfom_fg_hudhold_t h = {0, 0};
    // nothing separated yet: nothing to hold
    CHECK(!rfom_fg_hudhold_step(&h, 1, 1, 0, 100, 2000));
    CHECK(!rfom_fg_hudhold_step(&h, 1, 1, 1, 1000, 2000));
    CHECK(rfom_fg_hudhold_step(&h, 1, 1, 0, 1016, 2000));
    CHECK(rfom_fg_hudhold_step(&h, 1, 1, 0, 2999, 2000));
    CHECK(!rfom_fg_hudhold_step(&h, 1, 1, 0, 3000, 2000));
    // frames without the split neither use nor refresh the hold
    CHECK(!rfom_fg_hudhold_step(&h, 1, 1, 1, 4000, 2000));
    CHECK(!rfom_fg_hudhold_step(&h, 1, 0, 1, 5000, 2000));
    CHECK(rfom_fg_hudhold_step(&h, 1, 1, 0, 5500, 2000));
    CHECK(!rfom_fg_hudhold_step(&h, 1, 1, 0, 6000, 2000));
    // turning it off forgets the area
    CHECK(!rfom_fg_hudhold_step(&h, 1, 1, 1, 7000, 2000));
    CHECK(!rfom_fg_hudhold_step(&h, 0, 1, 0, 7010, 2000));
    CHECK(!rfom_fg_hudhold_step(&h, 1, 1, 0, 7020, 2000));
}

static void test_medoid_rel()
{
	static const double I[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	double truth[16], S[8][16], sp = 0, mo = 0;
	cam_m(0.30, 0.05, 1.0, -3.0, 0.26, 0.06, 0.9, -3.3, truth);
	const double tmo = vpcpu_mdist(truth, I);
	const double e[6][2] = { { 0.010, 0 }, { -0.008, 0.004 }, { 0.006, -0.006 }, { -0.011, -0.003 }, { 0.002, 0.008 }, { -0.004, -0.009 } };
	for (int i = 0; i < 6; ++i) cam_m(0.30, 0.05, 1.0, -3.0, 0.26 + e[i][0], 0.06 + e[i][1], 0.9, -3.3, S[i]);
	cam_m(0.30, 0.05, 1.0, -3.0, -0.2, 0.3, 0.0, -2.0, S[6]);
	cam_m(0.30, 0.05, 1.0, -3.0, 0.9, -0.2, 2.0, -4.0, S[7]);
	int a = 0;
	CHECK(vpcpu_vote_majority(S, 8, 0.01, 3, &a) < 0);           // the loose vote misses it too
	CHECK(vpcpu_medoid(S, 8, 0.02, 1.0 / 3.0, &sp, &mo) < 0);   // old rule: unsolved
	CHECK(sp > mo / 3.0 && sp <= 0.75 * mo);
	const int m = vpcpu_medoid(S, 8, 0.05, 0.75, &sp, &mo);
	CHECK(m >= 0 && m < 6);
	if (m >= 0) CHECK(vpcpu_mdist(S[m], truth) < 0.5 * tmo);    // identity would be off by tmo
	// noise as large as the motion: still unsolved (hold / identity)
	for (int i = 0; i < 6; ++i) cam_m(0.30, 0.05, 1.0, -3.0, 0.26 + 4.0 * e[i][0], 0.06 + 4.0 * e[i][1], 0.9, -3.3, S[i]);
	CHECK(vpcpu_medoid(S, 8, 0.05, 0.75, &sp, &mo) < 0);
}
// RFOM_DLSS_CAM_TOL2 (vpcpu_vote_majority): strict majority, unique group
static void test_vote_majority()
{
	double A[16], B[16], S[8][16];
	cam_m(0.30, 0.05, 1.0, -3.0, 0.26, 0.06, 0.9, -3.3, A);
	cam_m(0.30, 0.05, 1.0, -3.0, 0.36, 0.02, 1.1, -2.8, B);
	CHECK(vpcpu_mdist(A, B) > 0.05);
	auto fill = [&](int na, int nb) {
		for (int i = 0; i < na; ++i) cam_m(0.30, 0.05, 1.0, -3.0, 0.26 + 0.002 * i, 0.06, 0.9, -3.3, S[i]);
		for (int i = 0; i < nb; ++i) std::memcpy(S[na + i], B, sizeof B);
	};
	int a = 0;
	fill(4, 4);   // even split: vpcpu_vote would pass it, the majority vote must not
	CHECK(vpcpu_vote_majority(S, 8, 0.01, 3, &a) < 0 && a == 4);
	// exact half, no tie: 4 agree, 4 isolated outliers -> rejected by the strict majority alone
	fill(4, 0);
	for (int i = 0; i < 4; ++i) cam_m(0.30, 0.05, 1.0, -3.0, 0.40 + 0.1 * i, 0.02 + 0.05 * i, 1.1, -2.8 + 0.2 * i, S[4 + i]);
	for (int i = 4; i < 8; ++i) for (int j = 0; j < 8; ++j) if (i != j) CHECK(vpcpu_mdist(S[i], S[j]) > 0.01);
	CHECK(vpcpu_vote_majority(S, 8, 0.01, 3, &a) < 0 && a == 4);
	fill(5, 3);
	const int b = vpcpu_vote_majority(S, 8, 0.01, 3, &a);
	CHECK(b >= 0 && b < 5 && a == 5);
	CHECK(vpcpu_vote(S, 8, 2e-3, &a) < 0);   // the strict vote misses it
	fill(3, 3);
	CHECK(vpcpu_vote_majority(S, 6, 0.01, 3, &a) < 0);
	fill(2, 1);   // below min_agree
	CHECK(vpcpu_vote_majority(S, 3, 0.01, 3, &a) < 0);
}
// RSX_DLSS_MATCH_RATIO: nearest-by-constants matching with a distinct-runner-up ambiguity test
static void test_nn_ambiguous()
{
	auto run = [](const float* d, int n, float* bd, float* bd2) {
		*bd = *bd2 = 3.0e38f; int best = -1;
		for (int i = 0; i < n; ++i) if (rfom_nn_feed(d[i], bd, bd2)) best = i;
		return best;
	};
	float bd, bd2;
	const float clear[] = { 5.0f, 1.0f, 9.0f };          // own instance clearly nearest: kept
	CHECK(run(clear, 3, &bd, &bd2) == 1 && bd == 1.0f && bd2 == 5.0f);
	CHECK(!rfom_nn_ambiguous(bd, bd2, 0.5f));
	const float turn[] = { 1.3f, 1.0f, 7.0f };           // a turn: the neighbouring arch nearly as close -> camera motion
	CHECK(run(turn, 3, &bd, &bd2) == 1 && bd2 == 1.3f);
	CHECK(rfom_nn_ambiguous(bd, bd2, 0.5f));
	CHECK(!rfom_nn_ambiguous(bd, bd2, 0.0f));            // RSX_DLSS_MATCH_RATIO=0: off
	const float dup[] = { 2.0f, 2.0f, 10.0f };           // the same draw twice is not a rival
	CHECK(run(dup, 3, &bd, &bd2) == 0 && bd2 == 10.0f && !rfom_nn_ambiguous(bd, bd2, 0.5f));
	const float one[] = { 3.0f };                        // a unique object (no runner-up): kept
	CHECK(run(one, 1, &bd, &bd2) == 0 && !rfom_nn_ambiguous(bd, bd2, 0.5f));
	const float zero[] = { 0.0f, 0.0001f };              // identical constants: always kept
	CHECK(run(zero, 2, &bd, &bd2) == 0 && !rfom_nn_ambiguous(bd, bd2, 0.5f));
	const float late[] = { 4.0f, 6.0f, 1.0f, 1.8f };    // runner-up found after the best
	CHECK(run(late, 4, &bd, &bd2) == 2 && bd2 == 1.8f && rfom_nn_ambiguous(bd, bd2, 0.5f));
	// a fast turn adds the shared-row (camera) change C = 50 to every candidate: an enemy 0.2 from its own last
	// draw and 4 from the other one is a clear match (0.2 vs 4) though 50.2 / 54 is near 1 -- kept
	const float fast[] = { 54.0f, 50.2f };
	CHECK(run(fast, 2, &bd, &bd2) == 1 && rfom_nn_ambiguous(bd, bd2, 0.5f));
	CHECK(!rfom_nn_ambiguous_c(bd, bd2, 50.0f, 0.5f));
	// a real near-tie in the per-instance rows stays ambiguous under the same C
	const float tie[] = { 51.3f, 51.0f };
	CHECK(run(tie, 2, &bd, &bd2) == 1 && rfom_nn_ambiguous_c(bd, bd2, 50.0f, 0.5f));
	CHECK(!rfom_nn_ambiguous_c(bd, bd2, 50.0f, 0.0f));
	CHECK(!rfom_nn_ambiguous_c(bd, bd2, 60.0f, 0.5f));   // C over-estimated past bd: clamped, kept
	// default: an ambiguous match is dropped (-> per-draw camera split), never kept as the guessed neighbour
	CHECK(rfom_nn_ambig_action(1, 0, 0) == -1);
	CHECK(rfom_nn_ambig_action(1, 1, 0) == 5);
	CHECK(rfom_nn_ambig_action(0, 0, 0) == 3 && rfom_nn_ambig_action(0, 1, 0) == 3);
	// a view-attached piece (carbine) keeps its match: camera-only motion is wrong for it on either path
	CHECK(rfom_nn_ambig_action(1, 0, 1) == 3 && rfom_nn_ambig_action(1, 1, 1) == 3);
	// RSX_DLSS_MATCH_COMMON: a camera-following row learned as per-instance puts the same distance on every static
	// draw of the program (vs 4: 112 x 183.977); from the 4th such draw of the frame on, that match is not a guess
	{
		RfomNnCommon cs = {};
		const float d = 183.977f;
		CHECK(!rfom_nn_common_hit(&cs, 7u, d, 3u));
		for (int i = 0; i < 3; ++i) { CHECK(!rfom_nn_common_hit(&cs, 7u, d, 3u)); rfom_nn_common_feed(&cs, 7u, d); }
		CHECK(rfom_nn_common_hit(&cs, 7u, d, 3u));
		CHECK(!rfom_nn_common_hit(&cs, 7u, 140.0f, 3u));      // a moving instance at its own distance: still tested
		rfom_nn_common_feed(&cs, 7u, 140.0f);
		CHECK(rfom_nn_common_hit(&cs, 7u, d, 3u));             // one outlier does not reset an established term
		CHECK(!rfom_nn_common_hit(&cs, 8u, d, 3u));            // next frame starts over
		CHECK(!rfom_nn_common_hit(&cs, 7u, 0.0f, 3u));
		RfomNnCommon mv = {};                                   // animated instances: all different, never common
		const float an[] = { 5.39f, 6.36f, 8.10f, 6.16f, 5.97f, 1.17f };
		for (float x : an) { CHECK(!rfom_nn_common_hit(&mv, 3u, x, 3u)); rfom_nn_common_feed(&mv, 3u, x); }
	}
	// view-attached = object origin within thr of the eye (clip x, y, w)
	CHECK(rfom_viewmodel_near(0.05, -0.2, 0.4, 1.0));
	CHECK(rfom_viewmodel_near(0.0, 0.0, -0.3, 1.0));      // rooted just behind the eye
	CHECK(!rfom_viewmodel_near(0.1, 0.1, 12.0, 1.0));     // York arch 12 m away: still dropped
	CHECK(!rfom_viewmodel_near(5.0, 0.0, 0.5, 1.0));      // close in depth but far to the side
	CHECK(!rfom_viewmodel_near(0.0, 0.0, 0.4, 0.0));      // RSX_DLSS_VIEWMODEL_W=0: off
	volatile double z0 = 0.0;
	CHECK(!rfom_viewmodel_near(0.0, 0.0, z0 / z0, 1.0));  // NaN
}
// RFOM_DLSS_CAM_REP: a program whose candidates keep missing the solved camera is left out, then let back in
static void test_camrep()
{
	rfom_camrep_t r; std::memset(&r, 0, sizeof r);
	CHECK(rfom_camrep_step(&r, 0.45, 0.003, 0.01) == 0 && r.score == 2);   // York 397F3C37: 0.45 ndc off a 0.003 pan
	CHECK(rfom_camrep_step(&r, 0.003, 0.003, 0.01) == 0 && r.score == 1);  // one close frame takes one back
	CHECK(rfom_camrep_step(&r, 1e9, 0.003, 0.01) == 0);                     // behind the camera counts as far
	CHECK(rfom_camrep_step(&r, 0.2, 0.003, 0.01) == 0);
	CHECK(rfom_camrep_step(&r, 0.2, 0.003, 0.01) == 0 && r.score == 7);
	CHECK(rfom_camrep_step(&r, 0.2, 0.003, 0.01) == 1 && r.out);            // 9: left out
	CHECK(rfom_camrep_step(&r, 0.2, 0.003, 0.01) == 0 && r.out);
	for (int i = 0; i < 7; ++i) CHECK(rfom_camrep_step(&r, 0.002, 0.003, 0.01) == 0 && r.out);
	CHECK(rfom_camrep_step(&r, 0.002, 0.003, 0.01) == -1 && !r.out);        // 3: back in
	rfom_camrep_t g; std::memset(&g, 0, sizeof g);
	for (int i = 0; i < 40; ++i) rfom_camrep_step(&g, 0.025, 0.14, 0.01);  // fast turn: 0.025 off a 0.14 motion is close
	CHECK(!g.out && g.score == 0);
	rfom_camrep_step(&g, 0.2, 0.14, 0.01);
	CHECK(g.score == 2);                                                   // 0.2 off it is far
	rfom_camrep_step(&g, 0.03, 0.14, 0.01);
	CHECK(g.score == 2);                                                   // 0.03 is in between: no change
	volatile double z0 = 0.0;
	for (int i = 0; i < 4; ++i) rfom_camrep_step(&g, z0 / z0, 0.01, 0.01);  // NaN counts as far
	CHECK(g.out);
	int probes = 0;
	for (unsigned f = 0; f < 64; ++f) probes += rfom_camrep_probe(0x397F3C37u, f);
	CHECK(probes == 4);
	// slow walk (R1 1847): 0.03 off a 0.004 motion is far now (was under the old fixed 0.05)
	rfom_camrep_t w; std::memset(&w, 0, sizeof w);
	for (int i = 0; i < 3; ++i) CHECK(rfom_camrep_step(&w, 0.03, 0.004, 0.01) == 0);
	CHECK(rfom_camrep_step(&w, 0.03, 0.004, 0.01) == 1 && w.out);
	CHECK(rfom_camrep_step(&w, 0.015, 0.004, 0.01) == 0 && w.score == 8);  // between near and far: no change
	CHECK(rfom_camrep_step(&w, 0.02, 0.004, 0.01) == 0 && w.score == 8);   // at the far threshold: not far
	for (int i = 0; i < 4; ++i) rfom_camrep_step(&w, 0.008, 0.004, 0.01);
	CHECK(w.out && w.score == 4);
	CHECK(rfom_camrep_step(&w, 0.008, 0.004, 0.01) == -1 && !w.out);        // within the loose vote: back in
	rfom_camrep_t t; std::memset(&t, 0, sizeof t);
	for (int i = 0; i < 8; ++i) rfom_camrep_step(&t, 0.03, 0.004, 0.002);  // TOL2 off: floor stays 0.02
	CHECK(t.out);
	rfom_camrep_t u; std::memset(&u, 0, sizeof u);
	for (int i = 0; i < 8; ++i) rfom_camrep_step(&u, 0.045, 0.004, 0.05);  // TOL2 0.05: floor capped at the old 0.05
	CHECK(!u.out && u.score == 0);
}
// RSX_DLSS_MV_REMATCH / _VIEWLOCK / _LOOSE: probe points and the candidate pick
static void test_mv_rematch()
{
	const float a[4] = { 0.5f, -0.25f, 0.3f, 2.0f }, b[4] = { 0.5f, -0.25f, 0.3f, 4.0f };
	RfomMvPt pa = rfom_mv_pt(a), pb = rfom_mv_pt(b);
	CHECK(pa.ok && pa.x == 0.25 && pa.y == -0.125 && pa.w == 2.0);
	CHECK(rfom_mv_pt_dist(pa, pa) == 0.0);
	CHECK(rfom_mv_pt_dist(pa, pb) > 0.6);                  // same direction, twice as far: the depth term
	const float behind[4] = { 0.1f, 0.1f, 0.1f, -1.0f };
	CHECK(!rfom_mv_pt(behind).ok && rfom_mv_pt_dist(rfom_mv_pt(behind), pa) < 0.0);
	volatile float z0 = 0.0f; const float nan4[4] = { z0 / z0, 0.0f, 0.0f, 1.0f };
	CHECK(!rfom_mv_pt(nan4).ok);
	RfomMvPt cam = pa; cam.w = 0.0; cam.x += 0.01;           // through the solved camera: no depth term
	CHECK(rfom_mv_pt_dist(pa, cam) < 0.02);
	const double s1[4] = { 0.01, 0.3, -1.0, 0.4 };           // a door near where it was, its copies elsewhere
	CHECK(rfom_mv_rematch_pick(s1, 4, 0.5, 0.5) == 0);
	const double s2[3] = { 0.2, 0.25, 0.9 };                 // two copies equally plausible: no pick
	CHECK(rfom_mv_rematch_pick(s2, 3, 0.5, 0.5) == -1);
	const double s3[2] = { 0.6, 5.0 };                       // clear but too far
	CHECK(rfom_mv_rematch_pick(s3, 2, 0.5, 0.5) == -1);
	const double s4[2] = { 0.0, 0.00001 };                   // drawn twice on the same spot last frame
	CHECK(rfom_mv_rematch_pick(s4, 2, 0.5, 0.5) == 0);
	const double s5[1] = { 0.04 };                           // single candidate (loose): limit only
	CHECK(rfom_mv_rematch_pick(s5, 1, 0.5, 0.05) == 0);
	CHECK(rfom_mv_rematch_pick(s5, 1, 0.5, 0.03) == -1);
	const double s6[2] = { -1.0, -1.0 };
	CHECK(rfom_mv_rematch_pick(s6, 2, 0.5, 0.5) == -1);
	// view-still: an unmatched near-eye draw that stayed where last frame's view-attached pieces were
	CHECK(rfom_va_still(0.01, 0.3) == 1);    // gun steady on screen during a fast turn
	CHECK(rfom_va_still(0.07, 0.4) == 1);    // sway/bob, still far less than the camera moved
	CHECK(rfom_va_still(0.3, 0.3) == 0);     // near world object: moved with the camera
	CHECK(rfom_va_still(0.05, 0.02) == 0);   // slow turn: not clearly closer to the anchor than the camera path
	CHECK(rfom_va_still(0.01, 0.0) == 1);    // camera still
	CHECK(rfom_va_still(-1.0, 0.3) == 0);    // no anchor last frame
	CHECK(rfom_va_still(0.02, -1.0) == 1);   // no solved camera: tight limit only
	CHECK(rfom_va_still(0.05, -1.0) == 0);
	CHECK(rfom_va_still(0.09, 2.0) == 0);    // hard limit
}
#endif

int main()
{
	test_legacy_equivalence();
	test_selector();
	test_pacing();
	test_hold();
	test_gate();
#ifdef PFG_R1
	test_real_camera();
	test_cam_hold();
	test_medoid();
	test_medoid_rel();
	test_unsure_pause();
	test_hud_hold();
	test_vote_majority();
	test_nn_ambiguous();
	test_camrep();
	test_mv_rematch();
#endif
	std::printf("fg_logic_test: %d checks, %d failed\n", g_checks, g_fail);
	return g_fail ? 1 : 0;
}
