/* rfom_aspect.h -- RFOM_ASPECT (ultrawide / Hor+) decisions, pure and testable
 * (project/stubs.cpp rfom_fov_adjust; project/tests/aspect_test.cpp).
 *
 * The camera setter is given each camera's aspect. A 16:9 camera is widened to
 * the target (window) aspect, and the D3D12 present stretches the 16:9 guest
 * frame to it. Aiming down sights / scopes set their own camera, which is a
 * plain 16:9 one too (GitHub issue #2: the picture flipped to 16:9 with the wide
 * view squeezed in). So the stretch is not decided per camera any more:
 *  - it is the target aspect's (target / 16:9), the same for every camera;
 *  - it turns on with the first widened camera;
 *  - it turns off only when the target itself is back at 16:9 (window resized,
 *    RFOM_ASPECT=off) -- never because some camera asked for 16:9 -- and only
 *    after that has held for off_ms and off_n camera calls running (hysteresis:
 *    a window being resized or a one-off query cannot flip it).
 * Screens without a 3D camera (loading screens, 2D briefing / stats screens,
 * menus) set no camera at all, and are full-frame 16:9 art: the present shows
 * them letterboxed. So the stretch also needs a recent widened camera -- one
 * within recent_ms (RFOM_ASPECT_HOLD, default 2000; ADS / scope cameras count),
 * or 3D scene frames still being drawn since (RFOM_ASPECT_SCENE_HOLD) -- see
 * rfom_aspect_present_mul. */
#ifndef RFOM_ASPECT_H
#define RFOM_ASPECT_H

#include <math.h>

#define RFOM_ASPECT_169 (16.0 / 9.0)

typedef struct rfom_aspect_state {
    int on;                       /* the present stretches */
    float mul;                    /* its horizontal stretch (target / 16:9) */
    unsigned long long off_since; /* first "no widening wanted" call of the current run, 0 = none */
    unsigned off_n;               /* such calls running */
} rfom_aspect_state;

/* Is this aspect a 16:9 one (with the slack of the game's float maths)? */
static inline int rfom_aspect_is_169(double a) { return a > 1.70 && a < 1.86; }

/* Does the target ask for a wider (or narrower) picture than 16:9? t <= 0 = off. */
static inline int rfom_aspect_wants(double t) { return t > 0.0 && fabs(t / RFOM_ASPECT_169 - 1.0) > 0.01; }

/* The 16:9 aspect a camera stands for: a camera that already carries the target aspect
 * (read back from one the setter widened) is a 16:9 one too. 0 = not a 16:9 camera. */
static inline double rfom_aspect_base(double a, double t)
{
    if (rfom_aspect_wants(t) && fabs(a / t - 1.0) < 0.002) return RFOM_ASPECT_169;
    return rfom_aspect_is_169(a) ? a : 0.0;
}

/* One camera setter call: camera aspect a, target t, time now_ms. Returns the
 * horizontal widening for this camera (> 1: tan(hfov/2) x this, the aspect becomes t;
 * 1 = unchanged), sets *base_out to the 16:9 aspect it stood for, and updates the
 * present stretch in *s. Returns 1 for cameras that are not 16:9 ones (shadow /
 * monitor views), which never change the stretch. */
static inline double rfom_aspect_note(rfom_aspect_state* s, double a, double t, unsigned long long now_ms,
                                      unsigned off_ms, unsigned off_n, double* base_out)
{
    const double base = rfom_aspect_base(a, t);
    if (base_out) *base_out = base;
    if (base == 0.0) return 1.0;
    if (rfom_aspect_wants(t)) {
        s->on = 1;
        s->mul = (float)(t / RFOM_ASPECT_169);
        s->off_since = 0; s->off_n = 0;
        return t / base;
    }
    if (s->on) {
        if (!s->off_since) s->off_since = now_ms ? now_ms : 1;
        s->off_n++;
        if (now_ms - s->off_since >= off_ms && s->off_n >= off_n) {
            s->on = 0; s->mul = 1.0f; s->off_since = 0; s->off_n = 0;
        }
    }
    return 1.0;
}

/* The present's horizontal stretch: mul while a widened camera was set within recent_ms
 * (last_ms = its time, 0 = none or ended; recent_ms 0 = no timeout) and no movie plays.
 * The game does not set its gameplay camera every frame: it keeps the one it set (already
 * widened) until something changes it, such as aiming. On 0.3.0 the stretch timed out a few
 * seconds into gameplay (issue #2 again) and came back only while aiming. So after
 * recent_ms the stretch also stays while the guest frames still draw a 3D scene: scene_ms
 * is the last frame that did (enough depth-tested, depth-writing draws; 0 = none), within
 * scene_hold_ms (0 = off: the camera timeout alone, as in 0.3.0). 2D screens draw no
 * 3D scene, so they still go back to their 16:9 letterbox. */
static inline float rfom_aspect_present_mul(float mul, unsigned long long last_ms, unsigned long long scene_ms,
                                            unsigned long long now_ms, unsigned recent_ms, unsigned scene_hold_ms, int movie)
{
    if (!last_ms || movie) return 1.0f;
    if (!recent_ms || now_ms <= last_ms || now_ms - last_ms < recent_ms) return mul;
    if (scene_hold_ms && scene_ms && (now_ms <= scene_ms || now_ms - scene_ms < scene_hold_ms)) return mul;
    return 1.0f;
}

#endif
