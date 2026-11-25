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
 * rfom_aspect_present_mul.
 * Once the present is back at 16:9 outside play (a menu, loading or 2D screen), it
 * stays there until the player is in control again or a 3D scene has been drawn
 * steadily for a while (RFOM_ASPECT_REARM_MS) -- see rfom_aspect_gate. Without that,
 * menus in the game program flipped between 21:9 and 16:9 (issue #2 on 0.3.1). */
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

/* Issue #2 on 0.3.1: "the menu screen keeps stretching to 21:9 then back to 16:9". Screens
 * without player control (the pause menu and its pages, loading / briefing screens) can still
 * get a camera setter call now and then, or a burst of depth-tested draws (the menus' 3D
 * buttons), and each one turned the stretch on for the camera / scene hold: on, off, on ...
 * The gate: once the stretch m (rfom_aspect_present_mul) has been off, *held is set (to that
 * time; 0 = not held) and it stays off until either
 *  - the player is in control: the game's on-foot look code ran within RFOM_ASPECT_PLAY_MS
 *    (play_ms: its last run, 0 = never) -- gameplay and aiming get the stretch back at once;
 *  - or a 3D scene has been drawn steadily for rearm_ms since the hold began (in-engine
 *    cutscenes, vehicles): scene frames (scene_ms: the last, scene_run0: the first of the
 *    current run, in which no two were more than RFOM_ASPECT_SCENE_GAP_MS apart) still coming.
 * A stretch that is on stays on (in play, the gate never turns it off), except in a known
 * menu: menu != 0 (the game's pause menu loaded its text, g_rfom_pause_ui) while the player
 * is not in control locks it at 16:9 even with the 3D world drawn steadily behind it.
 * rearm_ms 0 = off (RFOM_MENU_ASPECT=0 / RFOM_ASPECT_REARM_MS=0, 0.3.1: m as is). The cost:
 * a cutscene that follows a loading screen keeps its letterbox for its first rearm_ms. */
#define RFOM_ASPECT_PLAY_MS 250u
#define RFOM_ASPECT_SCENE_GAP_MS 250u
#define RFOM_ASPECT_MENU_CAM_MS 500u
/* The menu input of rfom_aspect_gate. The pause menu flag (g_rfom_pause_ui) is set by a text load
 * soon after play and cleared only by on-foot play or a level load, so it can outlive the pause
 * menu (a text load in play followed by a cutscene or vehicle section). The paused world keeps
 * re-setting the camera(s) it had in play now and then; a cutscene sets cameras of its own (another
 * camera object or field of view). So the flag counts only until a camera that was not used in play
 * (rfom_aspect_cam_known) is set more than RFOM_ASPECT_MENU_CAM_MS after the flag went up
 * (foreign_ms: the last such camera, 0 = none), and that ends it for good, until the flag drops
 * and is raised again. *since: when the flag was seen raised (0 = down), *spent: ended. */
static inline int rfom_aspect_menu(int pause_ui, unsigned long long* since, int* spent,
                                   unsigned long long foreign_ms, unsigned long long now_ms)
{
    if (!pause_ui) { *since = 0; *spent = 0; return 0; }
    if (!*since) *since = now_ms ? now_ms : 1;
    if (foreign_ms && foreign_ms > *since && foreign_ms - *since >= RFOM_ASPECT_MENU_CAM_MS) *spent = 1;
    return !*spent;
}

/* The cameras widened while the player was in control: their signatures (camera object and the
 * field of view the game gave it), most recent first. */
#define RFOM_ASPECT_CAMSET_N 8
typedef struct rfom_aspect_camset { unsigned sig[RFOM_ASPECT_CAMSET_N]; unsigned n; } rfom_aspect_camset;
static inline unsigned rfom_aspect_cam_sig(unsigned cam, double fov)
{
    return cam * 2654435761u ^ (unsigned)(long long)floor(fov * 100.0 + 0.5);
}
/* One widened camera setter call: in play (in_play), remember it and return 1; outside play,
 * return whether it is one of the play cameras (1) or a foreign one (0, a cutscene's). */
static inline int rfom_aspect_cam_known(rfom_aspect_camset* s, unsigned sig, int in_play)
{
    unsigned i = 0;
    while (i < s->n && s->sig[i] != sig) i++;
    if (i == s->n) {
        if (!in_play) return 0;
        if (s->n < RFOM_ASPECT_CAMSET_N) s->n++;
        i = s->n - 1;
    }
    for (; i > 0; i--) s->sig[i] = s->sig[i - 1];
    s->sig[0] = sig;
    return 1;
}
static inline float rfom_aspect_gate(unsigned long long* held, float m, int menu, unsigned long long play_ms,
                                     unsigned long long scene_ms, unsigned long long scene_run0, unsigned long long now_ms,
                                     unsigned rearm_ms)
{
    if (!rearm_ms) return m;
    const int play = play_ms && (now_ms <= play_ms || now_ms - play_ms < RFOM_ASPECT_PLAY_MS);
    if (menu && !play) { *held = now_ms ? now_ms : 1; return 1.0f; }   /* renewed: scene runs count from the menu's last frame */
    if (m == 1.0f) { if (!*held) *held = now_ms ? now_ms : 1; return 1.0f; }
    if (!*held) return m;
    if (play) { *held = 0; return m; }
    /* a steady scene counts from the hold on: the 3D world still drawn behind a menu does not re-arm at once */
    const unsigned long long r0 = scene_run0 > *held ? scene_run0 : *held;
    if (scene_ms && scene_run0 && scene_ms >= r0 && scene_ms - r0 >= rearm_ms
        && (now_ms <= scene_ms || now_ms - scene_ms < RFOM_ASPECT_SCENE_GAP_MS)) { *held = 0; return m; }
    return 1.0f;
}

/* The replay loop's stamp of a 3D scene frame at now_ms: *last = its time, *run0 = the first frame
 * of the current steady run (restarted after a gap of more than RFOM_ASPECT_SCENE_GAP_MS). */
static inline void rfom_aspect_scene_stamp(volatile unsigned long long* last, volatile unsigned long long* run0,
                                           unsigned long long now_ms)
{
    const unsigned long long l = *last;
    if (!l || !*run0 || now_ms < l || now_ms - l > RFOM_ASPECT_SCENE_GAP_MS) *run0 = now_ms;
    *last = now_ms;
}

#endif
