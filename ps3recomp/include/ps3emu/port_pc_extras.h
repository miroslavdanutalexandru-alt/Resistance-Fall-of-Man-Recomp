/* PC-port extras: the pure logic behind the "true PC port" options, shared by the pad layer,
 * the audio mixer, the overlays and the launcher (identical copy in all three Resistance ports).
 * Header-only, no state of its own; tests: ps3recomp/tests/test_pc_extras.c.
 *
 *   PS3_ENTER_BUTTON=circle        confirm with Circle (system setting the game reads)
 *   PAD_RUMBLE=0..100|off          rumble strength in percent (default 100)
 *   PAD_BACKGROUND_INPUT=auto|1|0  controllers while the window is in the background
 *   PORT_VOLUME=0..100             master volume, PORT_AUDIO_MONO=1 mono downmix
 *   PAD_OUTER_DEADZONE / PAD_ANTI_DEADZONE / PAD_TRIGGER_DEADZONE (percent), PAD_TURN_BOOST
 *   PAD_CROUCH_MODE / PAD_SPRINT_MODE = game|toggle|hold
 *   PORT_UI_SCALE=auto|75..200     overlay / PC menu scale
 */
#ifndef PORT_PC_EXTRAS_H
#define PORT_PC_EXTRAS_H

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline int pcx_ieq(const char* a, const char* b)
{
    if (!a || !b) return 0;
    while (*a && *b) { if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0; a++; b++; }
    return *a == *b;
}

/* ---- 1. confirm button: 1 = Cross (CELL_SYSUTIL_ENTER_BUTTON_ASSIGN_CROSS), 0 = Circle ---- */
static inline int pcx_enter_button(const char* s)
{
    if (s && (pcx_ieq(s, "circle") || pcx_ieq(s, "o") || pcx_ieq(s, "0") || pcx_ieq(s, "jp"))) return 0;
    return 1;
}
/* keyboard menu keys mean confirm (Enter) / back (Esc, Backspace): with Circle as the confirm button
 * (enter_cross == 0) the Cross / Circle bits they produce are swapped, so the keys keep their meaning */
static inline unsigned pcx_swap_confirm(unsigned bits, unsigned cross, unsigned circle, int enter_cross)
{
    unsigned o;
    if (enter_cross) return bits;
    o = bits & ~(cross | circle);
    if (bits & cross) o |= circle;
    if (bits & circle) o |= cross;
    return o;
}

/* ---- percent settings: "" / NULL -> def, "off" -> 0, clamped to [lo, hi] ---- */
static inline int pcx_parse_pct(const char* s, int def, int lo, int hi)
{
    int v;
    if (!s || !*s) return def;
    if (pcx_ieq(s, "off") || pcx_ieq(s, "none")) return 0;
    if (pcx_ieq(s, "on")) return def;
    if (!isdigit((unsigned char)*s) && *s != '-') return def;
    v = atoi(s);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

/* ---- 2. rumble strength ---- */
static inline unsigned short pcx_rumble_scale(unsigned short v, int pct)
{
    unsigned int r;
    if (pct <= 0) return 0;
    if (pct >= 100) return v;
    r = (unsigned int)v * (unsigned int)pct / 100u;
    return (unsigned short)r;
}

/* ---- 3. background input: mode -1 auto (= when the game keeps running in the background),
 *      0 never, 1 always ---- */
static inline int pcx_bg_mode(const char* s)
{
    if (!s || !*s || pcx_ieq(s, "auto")) return -1;
    return (*s == '0' || pcx_ieq(s, "off")) ? 0 : 1;
}
/* the "in the background" setting (R2_BACKGROUND / the launcher's focus): "run" and "mute" keep the game running */
static inline int pcx_bg_keeps_running(const char* s)
{
    return pcx_ieq(s, "run") || pcx_ieq(s, "mute");
}
static inline int pcx_pad_input_allowed(int mode, int focused, int bg_run)
{
    if (focused) return 1;
    if (mode < 0) return bg_run != 0;
    return mode != 0;
}

/* ---- 4. master volume + mono, on interleaved stereo float ---- */
static inline void pcx_audio_apply(float* lr, unsigned frames, float gain, int mono)
{
    unsigned i;
    if (gain >= 0.999f && gain <= 1.001f && !mono) return;
    for (i = 0; i < frames; i++) {
        float l = lr[2 * i] * gain, r = lr[2 * i + 1] * gain;
        if (mono) { const float m = (l + r) * 0.5f; l = r = m; }
        lr[2 * i] = l; lr[2 * i + 1] = r;
    }
}

/* ---- 5. stick shaping. mag in [0,1] (radial), inner/outer/anti in [0,1), curve 0.5..3 (1 = linear).
 *      Inner deadzone -> 0; at or past (1 - outer) -> 1; between, |t|^curve rescaled to [anti, 1]. ---- */
static inline float pcx_stick_mag(float mag, float inner, float outer, float anti, float curve)
{
    float lim = 1.0f - outer, t;
    if (mag <= inner) return 0.0f;
    if (lim <= inner + 0.01f) lim = inner + 0.01f;
    if (mag >= lim) return 1.0f;
    t = (mag - inner) / (lim - inner);
    if (curve > 0.0f && (curve < 0.999f || curve > 1.001f)) t = powf(t, curve);
    return anti + (1.0f - anti) * t;
}
/* rescale a 0..255 trigger so values at or below dz% read 0 and full stays 255 */
static inline int pcx_trigger_dz(int v, int dz_pct)
{
    int dz;
    if (dz_pct <= 0) return v;
    if (dz_pct > 90) dz_pct = 90;
    dz = 255 * dz_pct / 100;
    if (v <= dz) return 0;
    return (v - dz) * 255 / (255 - dz);
}

/* turn boost: the stick held past 90% horizontally ramps an extra yaw factor 0..boost over
 * ramp_ms; dropping below resets it. Returns the current extra factor. */
typedef struct { unsigned held_ms; } PcxTurnBoost;
static inline float pcx_turn_boost(PcxTurnBoost* st, float x, unsigned dt_ms, float boost, unsigned ramp_ms)
{
    float a = x < 0 ? -x : x;
    if (boost <= 0.0f || a < 0.9f) { st->held_ms = 0; return 0.0f; }
    st->held_ms += dt_ms;
    if (st->held_ms > 60000u) st->held_ms = 60000u;
    if (!ramp_ms || st->held_ms >= ramp_ms) return boost;
    return boost * (float)st->held_ms / (float)ramp_ms;
}

/* ---- 6. hold/toggle conversion of one button ---- */
enum { PCX_BTN_GAME = 0, PCX_BTN_TOGGLE = 1, PCX_BTN_HOLD = 2 };
static inline int pcx_btn_mode(const char* s)
{
    if (pcx_ieq(s, "toggle")) return PCX_BTN_TOGGLE;
    if (pcx_ieq(s, "hold")) return PCX_BTN_HOLD;
    return PCX_BTN_GAME;
}
#define PCX_TAP_MS 80u
typedef struct {
    int prev;                 /* physical state last call */
    int latched;              /* toggle: the synthesized held state */
    int held;                 /* hold: parity of the taps sent = the toggle state the game believes */
    unsigned long long t0;    /* hold: start of the current tap phase */
    int phase;                /* hold: 0 idle, 1 down, 2 gap */
} PcxBtnConv;
/* the button state the game should see; reset = in a menu (raw button, no synthesis) */
static inline int pcx_btn_conv(PcxBtnConv* c, int mode, int pressed, unsigned long long now_ms, int reset)
{
    const int edge_dn = pressed && !c->prev;
    c->prev = pressed;
    if (mode == PCX_BTN_GAME) { c->latched = 0; c->held = 0; c->phase = 0; return pressed; }
    /* a menu: a tap cut short already reached the game, so held (the game's state) is kept; once out,
     * a press / release missed meanwhile is caught up below by one corrective tap */
    if (reset) { c->latched = 0; c->phase = 0; return pressed; }
    if (mode == PCX_BTN_TOGGLE) { if (edge_dn) c->latched = !c->latched; return c->latched; }
    /* hold on a game toggle: tap whenever the game's state differs from the button */
    if (c->phase == 1 && now_ms - c->t0 >= PCX_TAP_MS) { c->phase = 2; c->t0 = now_ms; }
    if (c->phase == 2 && now_ms - c->t0 >= PCX_TAP_MS) c->phase = 0;
    if (c->phase == 0 && c->held != (pressed != 0)) { c->phase = 1; c->t0 = now_ms; c->held = !c->held; }
    return c->phase == 1;
}
/* Sprint toggle on a button the game shares with crouch (Resistance 2 L2: a tap crouches, holding it while
 * moving sprints). Once the button has been held PCX_SPRINT_LATCH_MS while moving (the game is sprinting),
 * letting go keeps it held for the game until the player stops moving for PCX_SPRINT_STOP_MS or presses the
 * button again; that press is swallowed, so the game sees the long hold end, not a new tap. Short taps reach
 * the game unchanged (crouch). reset = in a menu: raw button, latch dropped. */
#define PCX_SPRINT_LATCH_MS 350u
#define PCX_SPRINT_STOP_MS 250u
typedef struct {
    int prev;                 /* physical state last call */
    int latched;              /* the game is kept sprinting */
    int swallow;              /* the press that ended the latch: hidden until released */
    int still;                /* not moving since still_t */
    unsigned long long t0;    /* start of the current press */
    unsigned long long still_t;
} PcxSprintLatch;
static inline int pcx_sprint_latch(PcxSprintLatch* s, int pressed, int moving, unsigned long long now_ms, int reset)
{
    const int edge_dn = pressed && !s->prev;
    s->prev = pressed;
    if (edge_dn) s->t0 = now_ms;
    if (reset) { s->latched = 0; s->swallow = 0; s->still = 0; return pressed; }
    if (moving) s->still = 0;
    else if (!s->still) { s->still = 1; s->still_t = now_ms; }
    if (s->swallow) { if (!pressed) s->swallow = 0; return 0; }
    if (s->latched) {
        if (edge_dn) { s->latched = 0; s->swallow = 1; return 0; }
        if (s->still && now_ms - s->still_t >= PCX_SPRINT_STOP_MS) { s->latched = 0; return pressed; }
        return 1;
    }
    if (pressed && moving && now_ms - s->t0 >= PCX_SPRINT_LATCH_MS) s->latched = 1;
    return pressed;
}
/* RPCS3 import: two saves map to the same slot; does candidate a win over b? This copy's own region
 * first, then the newer PARAM.SFO */
static inline int pcx_import_prefer(int a_local, unsigned long long a_mtime, int b_local, unsigned long long b_mtime)
{
    if (a_local != b_local) return a_local != 0;
    return a_mtime > b_mtime;
}

/* ---- 7. UI scale in percent: pct <= 0 = auto (by output height) ---- */
static inline int pcx_ui_scale_parse(const char* s)
{
    int v;
    if (!s || !*s || pcx_ieq(s, "auto")) return 0;
    v = atoi(s);
    if (v <= 0) return 0;
    if (v < 75) v = 75;
    if (v > 200) v = 200;
    return v;
}
static inline int pcx_ui_scale_pct(int pct, int out_h)
{
    if (pct > 0) return pct;
    if (out_h >= 2000) return 150;
    if (out_h >= 1400) return 125;
    return 100;
}
static inline int pcx_ui_px(int px, int pct) { return (px * pct + 50) / 100; }

/* ---- 8. frame-time graph: ring of the last PCX_FT_N frame times (ms) ---- */
#define PCX_FT_N 120
typedef struct { float ms[PCX_FT_N]; unsigned head, count; } PcxFtRing;
static inline void pcx_ft_push(PcxFtRing* r, float ms)
{
    r->ms[r->head] = ms;
    r->head = (r->head + 1) % PCX_FT_N;
    if (r->count < PCX_FT_N) r->count++;
}
/* i-th oldest sample (0 = oldest) */
static inline float pcx_ft_at(const PcxFtRing* r, unsigned i)
{
    return r->ms[(r->head + PCX_FT_N - r->count + i) % PCX_FT_N];
}
/* graph scale ceiling: 33.3 ms, or the next 16.7 ms step above the worst frame (max 100) */
static inline float pcx_ft_ceiling(const PcxFtRing* r)
{
    float mx = 0.0f, c = 33.3f;
    unsigned i;
    for (i = 0; i < r->count; i++) { const float v = pcx_ft_at(r, i); if (v > mx) mx = v; }
    while (c < mx && c < 100.0f) c += 16.7f;
    return c;
}

/* ---- 9. Nintendo labels for PS3 buttons (index as the glyph tables: 0 Circle 1 Square 2 Cross
 *      3 Triangle 4 Select 5 Start 6 L1 7 L2 8 L3 9 LS 10 R1 11 R2 12 R3 13 RS). Positional:
 *      Cross (bottom) = B, Circle (right) = A, Square (left) = Y, Triangle (top) = X. With the
 *      face buttons swapped by label (PAD_NINTENDO_SWAP), Cross = A etc. like Xbox. ---- */
static inline const char* pcx_nx_label(int idx, int label_swap)
{
    static const char* const pos[14] = { "A", "Y", "B", "X", "-", "+", "L", "ZL", "LS", "LS", "R", "ZR", "RS", "RS" };
    static const char* const lab[4] = { "B", "X", "A", "Y" };
    if (idx < 0 || idx >= 14) return "";
    if (label_swap && idx <= 3) return lab[idx];
    return pos[idx];
}

/* ---- 10. PARAM.SFO lookup (little-endian container). Copies the string/int value of key into out.
 *      Returns 1 when found. ---- */
static inline unsigned pcx_le32_(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }
static inline unsigned pcx_le16_(const unsigned char* p) { return p[0] | (p[1] << 8); }
static inline int pcx_sfo_get(const unsigned char* b, size_t n, const char* key, char* out, size_t on)
{
    unsigned ktab, dtab, cnt, i;
    if (!b || n < 20 || !out || !on || memcmp(b, "\0PSF", 4)) return 0;
    ktab = pcx_le32_(b + 8); dtab = pcx_le32_(b + 12); cnt = pcx_le32_(b + 16);
    if (ktab >= n || dtab > n || cnt > 4096 || 20 + (size_t)cnt * 16 > n) return 0;
    for (i = 0; i < cnt; i++) {
        const unsigned char* e = b + 20 + i * 16;
        const unsigned ko = ktab + pcx_le16_(e), fmt = pcx_le16_(e + 2), len = pcx_le32_(e + 4), off = dtab + pcx_le32_(e + 12);
        size_t k = 0;
        if (ko >= n) return 0;
        while (ko + k < n && b[ko + k]) k++;
        if (ko + k >= n || strlen(key) != k || memcmp(b + ko, key, k)) continue;
        if (off > n || len > n - off) return 0;
        if (fmt == 0x0404) {
            unsigned v;
            if (len < 4) return 0;
            v = pcx_le32_(b + off);
            { char t[16]; int j = 0, q = 0; char r[16];
              do { t[j++] = (char)('0' + v % 10); v /= 10; } while (v && j < 15);
              while (j && q < (int)on - 1) r[q++] = t[--j];
              r[q] = 0; memcpy(out, r, (size_t)q + 1); }
            return 1;
        }
        { size_t m = len;
          while (m && b[off + m - 1] == 0) m--;
          if (m >= on) m = on - 1;
          memcpy(out, b + off, m); out[m] = 0; }
        return 1;
    }
    return 0;
}
/* does a savedata folder / TITLE_ID belong to this game? prefixes = NULL-terminated list of
 * title IDs (e.g. "BCUS98232"); a folder name starts with one */
static inline int pcx_title_match(const char* name, const char* const* ids)
{
    if (!name) return 0;
    for (; ids && *ids; ids++) { const size_t k = strlen(*ids); if (k && !strncmp(name, *ids, k)) return 1; }
    return 0;
}

/* byte offset / length of key's value in a PARAM.SFO (for patching a string in place); 1 when found */
static inline int pcx_sfo_find(const unsigned char* b, size_t n, const char* key, size_t* off_out, size_t* len_out)
{
    unsigned ktab, dtab, cnt, i;
    if (!b || n < 20 || memcmp(b, "\0PSF", 4)) return 0;
    ktab = pcx_le32_(b + 8); dtab = pcx_le32_(b + 12); cnt = pcx_le32_(b + 16);
    if (ktab >= n || dtab > n || cnt > 4096 || 20 + (size_t)cnt * 16 > n) return 0;
    for (i = 0; i < cnt; i++) {
        const unsigned char* e = b + 20 + i * 16;
        const unsigned ko = ktab + pcx_le16_(e), len = pcx_le32_(e + 4), off = dtab + pcx_le32_(e + 12);
        size_t k = 0;
        if (ko >= n) return 0;
        while (ko + k < n && b[ko + k]) k++;
        if (ko + k >= n || strlen(key) != k || memcmp(b + ko, key, k)) continue;
        if (off > n || len > n - off) return 0;
        *off_out = off; *len_out = len;
        return 1;
    }
    return 0;
}
/* an imported save folder for another region of the game: name with its title-ID prefix replaced by
 * local_id (same length, 9 characters). 1 = out holds the new name, 0 = keep the name as it is */
static inline int pcx_save_retarget(const char* name, const char* const* ids, const char* local_id, char* out, size_t on)
{
    const size_t k = local_id ? strlen(local_id) : 0;
    if (!name || !out || !k || !pcx_title_match(name, ids) || !strncmp(name, local_id, k)) return 0;
    for (; *ids; ids++) {
        const size_t m = strlen(*ids);
        if (m == k && !strncmp(name, *ids, m)) {
            if (strlen(name) + 1 > on) return 0;
            memcpy(out, local_id, k); memcpy(out + k, name + m, strlen(name + m) + 1);
            return 1;
        }
    }
    return 0;
}

/* ---- 11. mods order file: one folder per line, "-" prefix = disabled, "#" comments.
 *      Rank of a folder: 0.. in file order, -1 disabled, -2 not listed. ---- */
static inline int pcx_mod_rank(const char* order_txt, const char* folder)
{
    const char* p = order_txt;
    int rank = 0;
    size_t fl;
    if (!p || !folder) return -2;
    fl = strlen(folder);
    while (*p) {
        const char* e = p; const char* s; size_t k; int dis = 0;
        while (*e && *e != '\n') e++;
        s = p;
        while (s < e && (*s == ' ' || *s == '\t')) s++;
        k = (size_t)(e - s);
        while (k && (s[k - 1] == '\r' || s[k - 1] == ' ' || s[k - 1] == '\t')) k--;
        if (k && *s != '#') {
            if (*s == '-') { dis = 1; s++; k--; }
            if (k == fl) {
                size_t j = 0;
                while (j < k && tolower((unsigned char)s[j]) == tolower((unsigned char)folder[j])) j++;
                if (j == k) return dis ? -1 : rank;
            }
            if (!dis) rank++;
        }
        p = *e ? e + 1 : e;
    }
    return -2;
}

#ifdef __cplusplus
}
#endif
#endif
