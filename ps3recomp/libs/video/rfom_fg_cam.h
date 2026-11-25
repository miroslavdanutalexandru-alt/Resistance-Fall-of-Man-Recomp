/* R1 port: the solved camera (rsx_fsr.inc cam_solve) as DLSS Frame Generation camera constants.
 * C99 and C++, no graphics API. Unit test: project/tests/fg_logic_test.cpp (PFG_R1).
 *
 * cam_solve's M is row-major for column vectors: prev_clip = M * (ndc_x, ndc_y, depth, 1) with this frame's
 * un-jittered ndc (y up) and the scene depth as stored in the DLSS depth input; the camera fill writes the
 * motion (prev_ndc - ndc) * (0.5, -0.5) from it. Streamline's clipToPrevClip is the same map for row vectors
 * (prev = clip * C): the transpose. prevClipToClip is its inverse. */
#ifndef RFOM_FG_CAM_H
#define RFOM_FG_CAM_H

#include "port_fg_logic.h"   /* PFG_CAM_* / pfg_cam_moving_px (RFOM_DLSS_FG_CAM_RESET) */

/* 4x4 inverse (Gauss-Jordan, partial pivoting); 0 when singular */
static inline int rfom_fgcam_inv(const double* m, double* out)
{
    double a[4][8];
    int i, j, c;
    for (i = 0; i < 4; ++i)
        for (j = 0; j < 8; ++j) a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i ? 1.0 : 0.0);
    for (c = 0; c < 4; ++c) {
        int p = c;
        double d;
        for (i = c + 1; i < 4; ++i)
            if ((a[i][c] < 0 ? -a[i][c] : a[i][c]) > (a[p][c] < 0 ? -a[p][c] : a[p][c])) p = i;
        if ((a[p][c] < 0 ? -a[p][c] : a[p][c]) < 1e-12) return 0;
        if (p != c)
            for (j = 0; j < 8; ++j) { const double t = a[p][j]; a[p][j] = a[c][j]; a[c][j] = t; }
        d = a[c][c];
        for (j = 0; j < 8; ++j) a[c][j] /= d;
        for (i = 0; i < 4; ++i) {
            double s;
            if (i == c || a[i][c] == 0.0) continue;
            s = a[i][c];
            for (j = 0; j < 8; ++j) a[i][j] -= s * a[c][j];
        }
    }
    for (i = 0; i < 4; ++i)
        for (j = 0; j < 4; ++j) out[i * 4 + j] = a[i][j + 4];
    return 1;
}

/* M (NULL = no camera motion) -> Streamline clipToPrevClip / prevClipToClip, row-major float[16] each.
 * Returns 1 with M, 0 when identity was written (no M, non-finite or singular M). */
static inline int rfom_fgcam_sl(const double* M, float* c2p, float* p2c)
{
    double Mi[16];
    int i, ok = M != 0;
    for (i = 0; ok && i < 16; ++i)
        if (!(M[i] == M[i]) || M[i] > 1e30 || M[i] < -1e30) ok = 0;
    if (ok && !rfom_fgcam_inv(M, Mi)) ok = 0;
    for (i = 0; i < 16; ++i) {
        const int r = i / 4, c = i % 4;
        c2p[i] = ok ? (float)M[c * 4 + r] : (r == c ? 1.0f : 0.0f);
        p2c[i] = ok ? (float)Mi[c * 4 + r] : (r == c ? 1.0f : 0.0f);
    }
    return ok;
}

/* RFOM_DLSS_FG_REAL_CAM (default 1): the solved camera goes to DLSS-G; 0 = identity (camera never moves) */
static inline int rfom_fgcam_env(const char* v) { return !(v && v[0] == '0' && !v[1]); }

/* cam_solve finds no camera on some frames (candidates disagree). Telling DLSS-G "identity" on those frames
 * while the motion vectors still carry the camera motion makes the camera jump between real and still; the
 * last solved camera (constant-velocity guess) is held instead for up to max_age frames. A reset drops it. */
typedef struct { double M[16]; int has, age; } rfom_fgcam_hold_t;

static inline const double* rfom_fgcam_hold(rfom_fgcam_hold_t* h, const double* M, int reset, int cut, int max_age)
{
    int i;
    if (reset || cut) h->has = 0;
    if (M && cut) return M;   /* the cross-cut jump: used this frame, never held */
    if (M) {
        for (i = 0; i < 16; ++i) h->M[i] = M[i];
        h->has = 1; h->age = 0;
        return M;
    }
    if (max_age > 0 && h->has && h->age < max_age) { h->age++; return h->M; }
    h->has = 0;
    return 0;
}

/* RFOM_DLSS_FG_CAM_RESET (rsx_fsr.inc fsr_dispatch; R2's R2_DLSS_FG_CAM_RESET): is this frame's camera a measurement?
 * cs: cam_solve's result (1 solved, 2 unsolved: candidates disagree, 0 no candidates: no program's camera rows
 * changed, i.e. a still camera, 3 no record data); gap: a frame gap (the history resets anyway); cut: a camera cut;
 * plur: the solve was the opt-in RFOM_DLSS_CAM_PLURALITY guess (a largest agreeing group); m_px: the solved camera's
 * motion (render px); h_px: the held camera's motion (RFOM_DLSS_FG_CAM_HOLD), < 0 = none (identity). Solved frames
 * and still ones latch their motion. Returns the pfg_cam_reset src (PFG_CAM_TRUSTED = nothing to check) and sets
 * *moving to pfg_cam_moving_px: the stand-in's own motion, else the latched motion for PFG_CAM_LATCH frames. */
typedef struct { double px; unsigned fr; } rfom_fgcam_latch_t;

static inline int rfom_fgcam_untrusted(rfom_fgcam_latch_t* l, unsigned fr, int cs, int gap, int cut, int plur,
                                       double m_px, double h_px, double* moving)
{
    *moving = 0.0;
    if (gap) return PFG_CAM_TRUSTED;
    if ((cs == 1 && !plur) || cs == 0) {
        if (!cut) { l->px = cs == 1 ? m_px : 0.0; l->fr = fr; }
        return PFG_CAM_TRUSTED;
    }
    if (cs == 1) { *moving = pfg_cam_moving_px(-1.0, m_px, l->px, fr - l->fr); return PFG_CAM_GUESSED; }
    if (cs == 2) {
        *moving = pfg_cam_moving_px(-1.0, h_px, l->px, fr - l->fr);
        return h_px >= 0.0 ? PFG_CAM_HELD : PFG_CAM_GUESSED;
    }
    return PFG_CAM_TRUSTED;
}

/* RFOM_DLSS_CAM_REP (rsx_fsr.inc cam_solve / cam_note): a camera candidate comes from a program whose shared
 * (non per-instance) rows changed since last frame -- normally the camera, but some programs change those rows
 * for their own reasons and give a camera nowhere near the real one (York logs: programs 397F3C37 / 8F6F6F59,
 * 0.15-10 ndc off). Two such programs took 2 of the 8 candidate slots each on nearly every unsolved frame, so a
 * tight group of 4 good candidates was no majority. Per program, on solved frames: d = its candidate's distance
 * to the solved camera (ndc), motion = the solved camera's own motion, good = the loose vote's tolerance.
 * Far (> max(2 * good clamped to 0.02..0.05, motion / 2)) counts +2, close (<= max(good, motion / 5)) -1; left
 * out at 8, back in at 3 (it is still probed now and then). The far floor was a fixed 0.05: during a slow walk
 * programs 0.02-0.05 off were never left out yet broke the 0.01 loose vote (R1 1847: 50-frame unsolved streak).
 * Returns 1 when the program was just left out, -1 when just let back in, else 0. */
typedef struct { unsigned char score, out; } rfom_camrep_t;
static inline int rfom_camrep_step(rfom_camrep_t* r, double d, double motion, double good)
{
    const double floor_t = good * 2.0 < 0.02 ? 0.02 : good * 2.0 > 0.05 ? 0.05 : good * 2.0;
    const double far_t = motion * 0.5 > floor_t ? motion * 0.5 : floor_t, near_t = motion * 0.2 > good ? motion * 0.2 : good;
    int s = r->score;
    if (!(d <= far_t)) s += 2;   /* NaN / behind (1e9) count as far */
    else if (d <= near_t && s > 0) s--;
    if (s > 16) s = 16;
    r->score = (unsigned char)s;
    if (!r->out && s >= 8) { r->out = 1; return 1; }
    if (r->out && s <= 3) { r->out = 0; return -1; }
    return 0;
}
/* a left-out program is still evaluated (not voted with) on one frame in 16, so it can earn its way back */
static inline int rfom_camrep_probe(unsigned hash, unsigned fr) { return ((hash >> 5 ^ fr) & 15u) == 0u; }

/* HUD split unsure-frame pause (rsx_dlssg.inc, RFOM_FG_UNSURE_PAUSE). A frame is truly unsure when the
 * classifier gave up (unsure) or a HUD run was started and then closed by a later scene draw; a frame
 * without HUD runs is fine (empty UI layer). Generation pauses after n_on consecutive unsure frames and
 * stays paused for min_off frames after the last one, so an intermittent HUD does not toggle DLSS-G (each
 * toggle is a Streamline mode change, which stalls the present). Returns 1 while paused. */
typedef struct { int bad_run, off_left; } rfom_fg_upause_t;

static inline int rfom_fg_unsure_frame(int unsure, int runs, int open) { return unsure || (runs > 0 && !open); }

static inline int rfom_fg_upause_step(rfom_fg_upause_t* s, int unsure, int n_on, int min_off)
{
    s->bad_run = unsure ? (s->bad_run < n_on ? s->bad_run + 1 : n_on) : 0;
    if (s->bad_run >= n_on) { s->off_left = min_off; return 1; }
    if (s->off_left > 0) { s->off_left--; return 1; }
    return 0;
}

/* HUD hold (rsx_dlssg_ghost.inc, RFOM_DLSS_FG_HUD_HOLD; the R2 port's R2_DLSS_FG_HUD_HOLD). Per present with the
 * HUD split running (split): a frame whose HUD was separated (valid) records its HUD area and the time; a frame
 * whose HUD was not separated keeps that area as UI while the last separated frame is less than max_ms old. Only
 * separated frames refresh the time, so a long stretch without one (fade, overlay) lets the hold expire. Frames
 * without the split leave the state alone; on = 0 (hold off or unavailable) forgets the area. Returns 1 when the
 * held area is used for this present. */
typedef struct { unsigned long long ms; int has; } rfom_fg_hudhold_t;

static inline int rfom_fg_hudhold_step(rfom_fg_hudhold_t* h, int on, int split, int valid, unsigned long long now_ms,
                                       unsigned max_ms)
{
    if (!on) { h->has = 0; return 0; }
    if (!split) return 0;
    if (valid) { h->has = 1; h->ms = now_ms; return 0; }
    return h->has && now_ms - h->ms < (unsigned long long)max_ms;
}

/* Nearest-by-constants draw matching (rsx_fsr.inc, RSX_DLSS_MATCH_RATIO): feed each candidate's signature distance
 * d; keeps the best *bd and the nearest DISTINCT runner-up *bd2 (a candidate at the best's own distance is the same
 * draw drawn twice, not a rival). Returns 1 when d is the new best. Start with *bd = *bd2 = 3e38f. */
static inline int rfom_nn_feed(float d, float* bd, float* bd2)
{
    if (d < *bd) { if (*bd < 3.0e38f && *bd - d > 1e-6f * *bd) *bd2 = *bd; *bd = d; return 1; }
    if (d < *bd2 && d - *bd > 1e-6f * *bd) *bd2 = d;
    return 0;
}
/* 1 = the nearest match is ambiguous (a distinct runner-up within 1/ratio of it): use camera motion instead. */
static inline int rfom_nn_ambiguous(float bd, float bd2, float ratio)
{
    return ratio > 0.0f && bd > 0.0f && bd < 3.0e38f && bd2 < 3.0e38f && !(bd < ratio * bd2);
}
/* The same on the per-instance part only: c = the distance term from the shared (camera / projection) rows, the
 * same for every candidate; it grows with a turn and would push any ratio towards 1. */
static inline int rfom_nn_ambiguous_c(float bd, float bd2, float c, float ratio)
{
    if (!(c > 0.0f)) c = 0.0f;
    if (c > bd) c = bd;   /* a not-yet-learned per-instance row counted as shared: never below 0 */
    return rfom_nn_ambiguous(bd - c, bd2 - c, ratio);
}
/* Common camera term of a program's nearest matches (RSX_DLSS_MATCH_COMMON). Some programs carry a row that follows
 * the camera yet differs between their draws (so it is learned as per-instance): while the camera moves, every
 * static instance then misses its exact-instance match, and its nearest match's per-instance distance is the SAME
 * value for every draw of the program that frame (log r1_fgdiag_1907: 112 draws of vs 4 at 183.977, 505 of vs 24 at
 * 0.139054). That uniform term is the camera, not a rival instance: a draw whose per-instance distance repeats the
 * value already seen on min_n other draws of its program this frame matched itself, so it is not ambiguous. */
typedef struct { unsigned frame; float x; unsigned n; } RfomNnCommon;
static inline int rfom_nn_common_same(float a, float b)
{
    const float m = a > b ? a : b, d = a > b ? a - b : b - a;
    return m > 0.0f && d <= 1e-5f * m;
}
/* 1 = inst (> 0) equals the common term established by at least min_n earlier draws of this frame. */
static inline int rfom_nn_common_hit(const RfomNnCommon* s, unsigned fr, float inst, unsigned min_n)
{
    return s->frame == fr && s->n >= min_n && inst > 0.0f && rfom_nn_common_same(inst, s->x);
}
static inline void rfom_nn_common_feed(RfomNnCommon* s, unsigned fr, float inst)
{
    if (!(inst > 0.0f) || !(inst < 3.0e38f)) return;
    if (s->frame != fr) { s->frame = fr; s->x = inst; s->n = 1; return; }
    if (rfom_nn_common_same(inst, s->x)) s->n++;
    else if (s->n <= 1) { s->x = inst; s->n = 1; }
}
/* What an ambiguous nearest match becomes. Default (keep = 0): dropped, so the draw takes its own exact camera-only
 * motion (fsr_cam_prev, match type 4), else the camera fill (0). keep = 1 (RSX_DLSS_AMBIG_KEEP, the r1_fg12 path):
 * the match is kept as type 5 (camera fill if the frame has a camera, else that guessed match). Returns the match
 * type to record, or -1 = drop the match. vmnear = the draw is view-attached (rfom_viewmodel_near): the camera-only
 * motion of either path is wrong for it (the first-person carbine follows the view), so the nearest match is kept. */
static inline int rfom_nn_ambig_action(int ambiguous, int keep, int vmnear)
{
    if (!ambiguous || vmnear) return 3;
    return keep ? 5 : -1;
}
/* The object's origin, run through the draw's vertex program (clip x, y, w), lies within thr world units of the
 * eye: a first-person viewmodel (weapon pieces) rather than world geometry. A pivot just behind the eye counts too
 * (a weapon mesh rooted at the shoulder). thr <= 0: off (RSX_DLSS_VIEWMODEL_W=0). */
static inline int rfom_viewmodel_near(double x, double y, double w, double thr)
{
    if (!(thr > 0.0) || !(w == w) || !(x == x) || !(y == y)) return 0;
    return (w < 0 ? -w : w) < thr && (x < 0 ? -x : x) < 2.0 * thr && (y < 0 ? -y : y) < 2.0 * thr;
}

#endif
