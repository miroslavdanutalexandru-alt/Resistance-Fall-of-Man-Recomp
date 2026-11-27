/* R2PORT: Resistance 1/2/3 port addition (not upstream RPCS3).
 * Frame generation decisions without any graphics API: the PORT_FG / PORT_FG_HUDLESS / PORT_FG_PACE selectors,
 * the FSR 3 present pacer, the Reflex frame cap, the FSR 3 enable hold and the per-frame on/off gate.
 * C99 and C++; byte-identical copies:
 *   R2/R3  ps3recomp/third_party/rpcs3/rpcs3/Emu/RSX/VK/VKR2FrameGenLogic.h (used by VKR2FrameGen.cpp)
 *   R1     ps3recomp/libs/video/port_fg_logic.h (used by rsx_dlssg.inc)
 * Unit test: project/tests/fg_logic_test.cpp. Design: scratch/framegen_fsr3.md. */
#ifndef PORT_FG_LOGIC_H
#define PORT_FG_LOGIC_H

#include <stdlib.h>
#include <ctype.h>

/* case-insensitive equality; NULL never matches */
static inline int pfg_word(const char* a, const char* b)
{
    if (!a || !b) return 0;
    for (; *a && *b; ++a, ++b)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

/* ---------------------------------------------------------------- PORT_FG=dlssg|fsr3|off */
enum { PFG_SEL_LEGACY = 0, PFG_SEL_DLSSG, PFG_SEL_FSR3, PFG_SEL_OFF, PFG_SEL_BAD };

/* unset, empty or "auto": the per-game switches decide (old behaviour) */
static inline int pfg_parse_sel(const char* v)
{
    if (!v || !*v || pfg_word(v, "auto")) return PFG_SEL_LEGACY;
    if (pfg_word(v, "dlssg") || pfg_word(v, "dlss-g") || pfg_word(v, "dlss")) return PFG_SEL_DLSSG;
    if (pfg_word(v, "fsr3") || pfg_word(v, "fsr")) return PFG_SEL_FSR3;
    if (pfg_word(v, "off") || pfg_word(v, "0") || pfg_word(v, "none")) return PFG_SEL_OFF;
    return PFG_SEL_BAD;
}

/* backend: 0 auto (DLSS-G, else FSR 3), 1 DLSS-G only, 2 FSR 3 only; want: 0 off or the multiplier 2..4 */
enum { PFG_BE_AUTO = 0, PFG_BE_DLSS = 1, PFG_BE_FSR3 = 2 };

/* R2/R3 per-game switches (R?_DLSS_FG, R?_FG_BACKEND), exactly as VKR2FrameGen.cpp parsed them before PORT_FG.
 * bad_mult: the ignored numeric value (0 = none); bad_backend: R?_FG_BACKEND was not understood. */
static inline void pfg_parse_vk_legacy(const char* dlss_fg, const char* fg_backend, int* want, int* backend, int* bad_mult, int* bad_backend)
{
    const int fsr_word = pfg_word(dlss_fg, "fsr3") || pfg_word(dlss_fg, "fsr");
    const int v = fsr_word ? 2 : (dlss_fg && *dlss_fg) ? atoi(dlss_fg) : 0;
    *want = (v >= 2 && v <= 4) ? v : 0;
    *bad_mult = (v && !*want) ? v : 0;
    const char* b = (fg_backend && *fg_backend) ? fg_backend : 0;
    *backend = fsr_word ? PFG_BE_FSR3 : pfg_word(b, "dlss") ? PFG_BE_DLSS : (pfg_word(b, "fsr3") || pfg_word(b, "fsr")) ? PFG_BE_FSR3 : PFG_BE_AUTO;
    *bad_backend = b && !*backend && !pfg_word(b, "auto");
}

/* PORT_FG wins over the per-game switches. dlssg keeps a 3x / 4x request (else 2x); fsr3 is 2x. */
static inline void pfg_apply_sel(int sel, int* want, int* backend)
{
    switch (sel)
    {
    case PFG_SEL_DLSSG: *backend = PFG_BE_DLSS; if (*want < 2 || *want > 4) *want = 2; break;
    case PFG_SEL_FSR3: *backend = PFG_BE_FSR3; *want = 2; break;
    case PFG_SEL_OFF: *want = 0; break;
    default: break;
    }
}

/* PORT_FG_HUDLESS: 1 = hand the generator the HUD-less scene + the UI layer, 0 = whole frame; unset = legacy */
static inline int pfg_hudless(const char* port_hudless, int legacy)
{
    if (!port_hudless || !*port_hudless) return legacy;
    return !(pfg_word(port_hudless, "0") || pfg_word(port_hudless, "off"));
}

/* ---------------------------------------------------------------- pacing */
/* PORT_FG_PACE (FSR 3 present pacer): vrr (default, old) = rendered fps <= 0.95 x refresh / 2 always;
 * auto = no software pacer on a FIFO (vsync) swapchain, vrr otherwise; off = no pacer (the game cap only) */
enum { PFG_PACE_VRR = 0, PFG_PACE_AUTO, PFG_PACE_OFF };

static inline int pfg_parse_pace(const char* v)
{
    if (pfg_word(v, "auto") || pfg_word(v, "vsync")) return PFG_PACE_AUTO;
    if (pfg_word(v, "off") || pfg_word(v, "0")) return PFG_PACE_OFF;
    return PFG_PACE_VRR;
}

/* seconds between two rendered presents for the FSR 3 2x pacer; 0 = no pacer */
static inline double pfg_fx_step_s(int cap, int pace, double refresh_hz, int fifo)
{
    if (!cap || pace == PFG_PACE_OFF || refresh_hz <= 1.0) return 0.0;
    if (pace == PFG_PACE_AUTO && fifo) return 0.0;
    return 2.0 / (refresh_hz * 0.95);
}

/* next deadline (any time unit): one step after the previous deadline, or after now when it fell behind */
static inline long long pfg_pace_next(long long now, long long next, long long step)
{
    return (now - next > step) ? now + step : next + step;
}

/* DLSS-G Reflex frame limit in us (applies to presented frames); 0 = none */
static inline unsigned pfg_reflex_limit_us(int cap, double refresh_hz, int on, int immediate)
{
    return (cap && refresh_hz > 1.0 && (on || immediate)) ? (unsigned)(1e6 / (refresh_hz * 0.95)) : 0u;
}

/* ---------------------------------------------------------------- FSR 3 enable hold */
#define PFG_FX_HOLD 30u
/* Generation stays enabled (as reset frames) for PFG_FX_HOLD frames after it stops being wanted, so short gaps do
 * not toggle it (each toggle waits for the presenter). Returns whether generation is enabled this frame. */
static inline int pfg_fx_hold_step(unsigned* hold, int on, int prev_gen, int inputs_ok)
{
    *hold = on ? PFG_FX_HOLD : (*hold ? *hold - 1u : 0u);
    return on || (*hold && prev_gen && inputs_ok);
}

/* R2_DLSS_FG_CAM_RESET (R1: RFOM_DLSS_FG_CAM_RESET): a frame whose camera motion is not a measurement (the upscaler's
   camera vote failed, or its depth check rejected the motion and zeroed it) while the camera moves resets frame
   generation, which would otherwise interpolate the whole scene with a wrong or zero camera motion (a double image of
   everything while turning; R2 Chicago Leviathan). src: PFG_CAM_HELD (failed vote, last frame's motion reused),
   PFG_CAM_GUESSED (failed vote, the largest agreeing group of draws or no motion), PFG_CAM_ZEROED (depth check reject).
   moving_px: pfg_cam_moving_px. Never the rejected motion (usually an object's).
   mode: 0 off, 1 guessed / zeroed (default), 2 held ones too. */
enum { PFG_CAM_TRUSTED = 0, PFG_CAM_HELD = 1, PFG_CAM_GUESSED = 2, PFG_CAM_ZEROED = 3 };
#define PFG_CAM_MOVING_PX 1.0
#define PFG_CAM_LATCH 8 /* frames a measured camera motion stands for the camera's movement */
static inline int pfg_cam_reset(int mode, int src, double moving_px)
{
    if (mode <= 0 || src <= PFG_CAM_TRUSTED) return 0;
    if (src == PFG_CAM_HELD && mode < 2) return 0;
    return moving_px >= PFG_CAM_MOVING_PX;
}

/* moving_px for pfg_cam_reset. vp_px: the game's view-projection motion (< 0: unknown); stand_px: the stand-in motion
   frame generation would warp by when it is one (held, or a plausible largest agreeing group: near zero = still;
   < 0: none, the identity or a zeroed reject); latch_px / latch_age: the last measured motion and its age in frames,
   used for PFG_CAM_LATCH frames only (a run of unmeasured frames that outlasts the turn must not keep resetting). */
static inline double pfg_cam_moving_px(double vp_px, double stand_px, double latch_px, unsigned latch_age)
{
    if (vp_px >= 0.0) return stand_px > vp_px ? stand_px : vp_px;
    if (stand_px >= 0.0) return stand_px;
    return latch_age <= PFG_CAM_LATCH ? latch_px : 0.0;
}

/* the generated frame must not blend with history: first frame, new swapchain, camera cut or a held (off) frame */
static inline int pfg_fx_reset(int prev_gen, int sc_new, int reset, int on)
{
    return !prev_gen || sc_new || reset || !on;
}

/* ---------------------------------------------------------------- per-frame gate (both backends) */
enum
{
    PFG_GATE_ON = 0, PFG_GATE_WARMUP, PFG_GATE_NO_SCENE, PFG_GATE_PAUSED, PFG_GATE_SPLITSCREEN, PFG_GATE_SCRGB,
    PFG_GATE_SRGB, PFG_GATE_FX_SWAPCHAIN, PFG_GATE_MENU, PFG_GATE_BAD_STATUS
};
#define PFG_WARMUP_FRAMES 3u
#define PFG_MENU_IDLE_MS 1500ull

typedef struct
{
    int scene;          /* the upscaler handed over depth + motion vectors this frame */
    int paused;         /* PC pause / Alt-Tab pause */
    int local_players;
    int scrgb;
    int srgb;
    int fx_sc_bad;      /* FSR 3 selected but its swapchain is not in use */
    int menu_off;       /* R?_DLSS_FG_MENU_OFF */
    int pause_ui;       /* the game's pause menu is up */
    unsigned long long activity_ms, now_ms; /* newest look / play / camera / input time (0 = never) */
    unsigned bad_status;  /* frames left after a backend error */
    unsigned streak;      /* consecutive frames with a scene, this one included */
} pfg_gate_in;

static inline unsigned pfg_streak(unsigned streak, int scene) { return scene ? streak + 1u : 0u; }

static inline int pfg_gate(const pfg_gate_in* g)
{
    if (!g->scene) return PFG_GATE_NO_SCENE;
    if (g->paused) return PFG_GATE_PAUSED;
    if (g->local_players > 1) return PFG_GATE_SPLITSCREEN;
    if (g->scrgb) return PFG_GATE_SCRGB;
    if (g->srgb) return PFG_GATE_SRGB;
    if (g->fx_sc_bad) return PFG_GATE_FX_SWAPCHAIN;
    if (g->menu_off && (g->pause_ui || (g->activity_ms && g->now_ms - g->activity_ms > PFG_MENU_IDLE_MS))) return PFG_GATE_MENU;
    if (g->bad_status) return PFG_GATE_BAD_STATUS;
    if (g->streak < PFG_WARMUP_FRAMES) return PFG_GATE_WARMUP;
    return PFG_GATE_ON;
}

#endif
