/* port_dlss_quality.h -- pure (no GPU / no OS) helpers for three opt-in DLSS quality items, shared by the
 * Resistance ports (same file in R1 ps3recomp\libs\video, R2/R3 RPCS3 Emu\RSX\VK\VKR2DlssQuality.h) and unit-tested
 * by scratch\dlss_quality\test_dlss_quality.cpp. C99 and C++ compatible, header only.
 *
 *   1. pdq_is_effect: does a draw belong in the "bias current colour" mask (particles, smoke, muzzle flashes,
 *      additive / alpha-blended effects -- the draws the game writes no motion for)?
 *   2. pdq_reset_reasons: extra history-reset triggers (hitch, resolution change, cut, transition).
 *   3. pdq_preset_check: will this nvngx_dlss.dll honour the requested render preset?
 *
 * Switches (read once by the callers through pdq_env_int / pdq_env_float):
 *   PORT_DLSS_BIAS_MASK       0 = off (default)  1 = mark effect draws in the bias-current-colour mask
 *   PORT_DLSS_RESET_CUTS      0 = off (default)  1 = also reset on resolution changes / transitions (see callers)
 *   PORT_DLSS_RESET_HITCH_MS  0 = off (R1 default; R2/R3 default 250)  N = reset the history on the frame after a gap
 *                             longer than N ms
 *   RSX_AA_A2C / RSX_SSAO_NOISE (R2/R3 only, section 6): noise in the upscaler input; R3 default stable / temporal,
 *                             R2 default noise / fixed (the callers pick the default; the parsers return the old value)
 *   RSX_DLSS_DEPTH_BIAS       (R2/R3 only, VKR2Gfx.cpp) default scale; 0/off = upstream  scale|color|<k> = polygon-offset slope factor
 *                             x the target's resolution scale (decals/carpets z-fighting at grazing angles); _LOG=N logs
 *   RSX_AA_ATEST_MIPSCALE / RSX_DLSS_MASK_FAR (R2/R3 only, section 6): distant cut-out foliage under the upscaler;
 *                             default 0.15 / 60 (the callers pass the default); 0/off = old
 */
#ifndef PORT_DLSS_QUALITY_H
#define PORT_DLSS_QUALITY_H

#include <stdint.h>
#include <stdlib.h>
#include <math.h>

/* ---- 1. effect-draw classifier ---------------------------------------------------------------------- */

/* API-neutral blend factors / ops (callers map D3D12 / RSX enums onto these) */
enum pdq_blend_factor {
    PDQ_BF_OTHER = 0, PDQ_BF_ZERO, PDQ_BF_ONE, PDQ_BF_SRC_COLOR, PDQ_BF_INV_SRC_COLOR, PDQ_BF_SRC_ALPHA,
    PDQ_BF_INV_SRC_ALPHA, PDQ_BF_DST_ALPHA, PDQ_BF_INV_DST_ALPHA, PDQ_BF_DST_COLOR, PDQ_BF_INV_DST_COLOR
};
enum pdq_blend_op { PDQ_OP_OTHER = 0, PDQ_OP_ADD, PDQ_OP_SUB, PDQ_OP_REVSUB, PDQ_OP_MIN, PDQ_OP_MAX };

typedef struct pdq_draw {
    int blend;          /* blending enabled on colour target 0 */
    int src, dst, op;   /* pdq_blend_factor / pdq_blend_op of the RGB channels */
    int color_write;    /* any colour channel of target 0 written */
    int depth_write;    /* the draw writes depth */
} pdq_draw;

/* Flags for pdq_is_effect */
#define PDQ_FX_MODULATE     1  /* also mark modulating blends (R1 RSX_FSR_RX_MUL=1) */
#define PDQ_FX_DEPTH_WRITE  2  /* also mark blended draws that write depth (R1: they get no motion either) */

/* The R1 FSR reactive-mask rule (rsx_fsr.inc fsr_mv_replay): blended, except
 *   - ONE,ZERO (= replace: opaque, e.g. R1's outdoor terrain),
 *   - modulating blends (op ADD, src ZERO/DST_ALPHA/DST_COLOR, dst ZERO/ONE/SRC_COLOR/SRC_ALPHA): they only scale
 *     what is already there (shadow receive passes, decals) and move with the surface below,
 *   - colour writes off (occlusion boxes, depth-only passes),
 *   - (R2/R3) blended draws that write depth: the depth-based camera motion covers them.
 * Returns 1 for an effect draw. */
static inline int pdq_is_effect(const pdq_draw* d, unsigned flags)
{
    if (!d || !d->blend || !d->color_write) return 0;
    if (d->src == PDQ_BF_ONE && d->dst == PDQ_BF_ZERO) return 0;
    if (!(flags & PDQ_FX_MODULATE) && d->op == PDQ_OP_ADD &&
        (d->src == PDQ_BF_ZERO || d->src == PDQ_BF_DST_ALPHA || d->src == PDQ_BF_DST_COLOR) &&
        (d->dst == PDQ_BF_ZERO || d->dst == PDQ_BF_ONE || d->dst == PDQ_BF_SRC_COLOR || d->dst == PDQ_BF_SRC_ALPHA))
        return 0;
    if (d->depth_write && !(flags & PDQ_FX_DEPTH_WRITE)) return 0;
    return 1;
}

/* D3D12_BLEND / D3D12_BLEND_OP values (R1's blend_key fields) -> neutral */
static inline int pdq_bf_from_d3d12(unsigned v)
{
    switch (v) {
    case 1: return PDQ_BF_ZERO;      case 2: return PDQ_BF_ONE;
    case 3: return PDQ_BF_SRC_COLOR; case 4: return PDQ_BF_INV_SRC_COLOR;
    case 5: return PDQ_BF_SRC_ALPHA; case 6: return PDQ_BF_INV_SRC_ALPHA;
    case 7: return PDQ_BF_DST_ALPHA; case 8: return PDQ_BF_INV_DST_ALPHA;
    case 9: return PDQ_BF_DST_COLOR; case 10: return PDQ_BF_INV_DST_COLOR;
    default: return PDQ_BF_OTHER;
    }
}
static inline int pdq_op_from_d3d12(unsigned v)
{
    switch (v) {
    case 1: return PDQ_OP_ADD; case 2: return PDQ_OP_SUB; case 3: return PDQ_OP_REVSUB;
    case 4: return PDQ_OP_MIN; case 5: return PDQ_OP_MAX; default: return PDQ_OP_OTHER;
    }
}
/* CELL_GCM blend factors / equations (RSX registers; R2/R3 rsx::blend_factor / rsx::blend_equation) -> neutral */
static inline int pdq_bf_from_gcm(unsigned v)
{
    switch (v) {
    case 0x0000: return PDQ_BF_ZERO;          case 0x0001: return PDQ_BF_ONE;
    case 0x0300: return PDQ_BF_SRC_COLOR;     case 0x0301: return PDQ_BF_INV_SRC_COLOR;
    case 0x0302: return PDQ_BF_SRC_ALPHA;     case 0x0303: return PDQ_BF_INV_SRC_ALPHA;
    case 0x0304: return PDQ_BF_DST_ALPHA;     case 0x0305: return PDQ_BF_INV_DST_ALPHA;
    case 0x0306: return PDQ_BF_DST_COLOR;     case 0x0307: return PDQ_BF_INV_DST_COLOR;
    default: return PDQ_BF_OTHER;
    }
}
static inline int pdq_op_from_gcm(unsigned v)
{
    switch (v) {
    case 0x8006: case 0xF006: case 0xF007: return PDQ_OP_ADD;   /* FUNC_ADD, ADD_SIGNED, REVERSE_ADD_SIGNED */
    case 0x800A: return PDQ_OP_SUB;
    case 0x800B: case 0xF005: return PDQ_OP_REVSUB;
    case 0x8007: return PDQ_OP_MIN; case 0x8008: return PDQ_OP_MAX;
    default: return PDQ_OP_OTHER;
    }
}

/* ---- 2. history-reset triggers ---------------------------------------------------------------------- */

#define PDQ_RESET_HITCH       1u  /* the previous frame took longer than PORT_DLSS_RESET_HITCH_MS */
#define PDQ_RESET_RESOLUTION  2u  /* render or output size differs from the last evaluation */
#define PDQ_RESET_CUT         4u  /* camera cut reported by the caller's camera logic */
#define PDQ_RESET_TRANSITION  8u  /* load screen / menu -> gameplay (the upscaler skipped frames) */

typedef struct pdq_reset_cfg {
    int cuts;           /* PORT_DLSS_RESET_CUTS */
    float hitch_ms;     /* PORT_DLSS_RESET_HITCH_MS, <= 0 = off */
} pdq_reset_cfg;

typedef struct pdq_reset_in {
    float raw_dt_ms;              /* unclamped time since the last evaluation; <= 0 = unknown (first frame) */
    uint32_t rw, rh, ow, oh;      /* this evaluation's render / output size */
    uint32_t prw, prh, pow_, poh; /* the last evaluation's (0 = none yet) */
    int cut;                      /* caller's camera-cut detector fired */
    uint32_t skipped_frames;      /* frames since the last evaluation minus one */
} pdq_reset_in;

static inline int pdq_hitch(float hitch_ms, float raw_dt_ms)
{
    return hitch_ms > 0.0f && raw_dt_ms > hitch_ms;
}

/* The ports' own pauses (focus loss, quit prompt, extras menu, photo mode, controller lost) bump a pause
 * generation on every pause and resume. An evaluation whose gap spans one is not a hitch: this returns the
 * gap to use as raw_dt_ms (0 when the generation changed since the last evaluation) and records it. */
static inline float pdq_pause_gap(float raw_dt_ms, uint32_t* last_gen, uint32_t gen)
{
    const int paused = *last_gen != gen;
    *last_gen = gen;
    return paused ? 0.0f : raw_dt_ms;
}

/* Bit set of PDQ_RESET_* that fire. With cfg {0, 0} it is always 0 (old behaviour). */
static inline unsigned pdq_reset_reasons(const pdq_reset_cfg* c, const pdq_reset_in* in)
{
    unsigned r = 0;
    if (!c || !in) return 0;
    if (pdq_hitch(c->hitch_ms, in->raw_dt_ms)) r |= PDQ_RESET_HITCH;
    if (c->cuts) {
        if ((in->prw || in->prh || in->pow_ || in->poh) &&
            (in->rw != in->prw || in->rh != in->prh || in->ow != in->pow_ || in->oh != in->poh))
            r |= PDQ_RESET_RESOLUTION;
        if (in->cut) r |= PDQ_RESET_CUT;
        if (in->skipped_frames) r |= PDQ_RESET_TRANSITION;
    }
    return r;
}

/* ---- 3. render preset vs DLL version ---------------------------------------------------------------- */

#define PDQ_PRESET_OK          0  /* honoured (or the default, 0) */
#define PDQ_PRESET_UNSUPPORTED 1  /* this DLL predates the preset: NGX falls back to its default */
#define PDQ_PRESET_DEPRECATED  2  /* deprecated in this DLL generation (DLSS 4, 310.x): may be ignored / remapped */
#define PDQ_PRESET_REMOVED     3  /* removed in this DLL generation: reverts to the default */

/* dll_major/minor: nvngx_dlss.dll file version (3.7.20.0 -> 3, 7; DLSS 4: 310.2.1.0 -> 310, 2).
 * preset: NVSDK_NGX_DLSS_Hint_Render_Preset (A=1 ... F=6, J=10, K=11, L=12, M=13). */
static inline int pdq_preset_check(unsigned dll_major, unsigned dll_minor, unsigned preset)
{
    if (!preset || !dll_major) return PDQ_PRESET_OK;    /* default / version unknown: nothing to say */
    const int dlss4 = dll_major >= 310;
    const int v3 = dll_major == 3 || dlss4;
    if (preset >= 1 && preset <= 6) {
        if (!v3 || (dll_major == 3 && dll_minor < 1)) return PDQ_PRESET_UNSUPPORTED;   /* presets came with 3.1 */
        if (preset == 5 && dll_major == 3 && dll_minor < 7) return PDQ_PRESET_UNSUPPORTED; /* E came with 3.7 */
        if (dlss4) return preset <= 4 ? PDQ_PRESET_REMOVED : PDQ_PRESET_DEPRECATED;
        return PDQ_PRESET_OK;
    }
    if (preset >= 10 && preset <= 13) return dlss4 ? PDQ_PRESET_OK : PDQ_PRESET_UNSUPPORTED; /* transformer: 310+ */
    return dlss4 ? PDQ_PRESET_REMOVED : PDQ_PRESET_UNSUPPORTED;   /* G/H/I/N/O: reserved */
}

static inline char pdq_preset_letter(unsigned preset)
{
    return preset >= 1 && preset <= 15 ? (char)('A' + preset - 1) : '-';
}

/* ---- 4. automatic preset from the GPU architecture (PORT_DLSS_AUTO_ARCH) ---------------------------- */

#define PDQ_ARCH_UNKNOWN    0   /* not NVIDIA, pre-Turing, or an unlisted device ID: callers keep their old auto */
#define PDQ_ARCH_TURING     1   /* RTX 20, GTX 16, Quadro RTX, TITAN RTX */
#define PDQ_ARCH_AMPERE     2   /* RTX 30, RTX A-series (and GA100 / Hopper datacenter parts) */
#define PDQ_ARCH_ADA        3   /* RTX 40, RTX Ada */
#define PDQ_ARCH_BLACKWELL  4   /* RTX 50 */
#define PDQ_ARCH_NEWER      5   /* device IDs past Blackwell's block: assumed FP8-capable like Ada+ */

/* PCI vendor / device ID (VkPhysicalDeviceProperties vendorID / deviceID, DXGI_ADAPTER_DESC1 VendorId /
 * DeviceId) -> architecture. NVIDIA allocates device IDs in contiguous blocks per generation:
 * TU10x 0x1E00-0x1FFF, TU116/117 0x2180-0x21FF, GA100 0x20B0-0x20FF, GA10x (+GH100) 0x2200-0x25FF,
 * AD10x 0x2600-0x28FF, GB10x/GB20x 0x2900-0x2FFF. */
static inline int pdq_nv_arch(unsigned vendor, unsigned dev)
{
    if (vendor != 0x10DEu) return PDQ_ARCH_UNKNOWN;
    if ((dev >= 0x1E00u && dev <= 0x1FFFu) || (dev >= 0x2180u && dev <= 0x21FFu)) return PDQ_ARCH_TURING;
    if ((dev >= 0x20B0u && dev <= 0x20FFu) || (dev >= 0x2200u && dev <= 0x25FFu)) return PDQ_ARCH_AMPERE;
    if (dev >= 0x2600u && dev <= 0x28FFu) return PDQ_ARCH_ADA;
    if (dev >= 0x2900u && dev <= 0x2FFFu) return PDQ_ARCH_BLACKWELL;
    if (dev >= 0x3000u && dev <= 0x3FFFu) return PDQ_ARCH_NEWER;
    return PDQ_ARCH_UNKNOWN;
}
static inline const char* pdq_arch_name(int arch)
{
    switch (arch) {
    case PDQ_ARCH_TURING: return "Turing (RTX 20)";
    case PDQ_ARCH_AMPERE: return "Ampere (RTX 30)";
    case PDQ_ARCH_ADA: return "Ada (RTX 40)";
    case PDQ_ARCH_BLACKWELL: return "Blackwell (RTX 50)";
    case PDQ_ARCH_NEWER: return "post-Blackwell";
    default: return "unknown";
    }
}
/* Auto preset: E (5, CNN) on Turing / Ampere (the transformer costs several times more there); 0 = NVIDIA's
 * default on Ada and newer, which under DLSS 310 is itself mode-dependent (K for DLAA / Quality / Balanced,
 * M for Performance, L for Ultra Performance). -1 = unknown: keep the old auto. */
static inline int pdq_auto_preset(int arch)
{
    if (arch == PDQ_ARCH_TURING || arch == PDQ_ARCH_AMPERE) return 5;
    if (arch == PDQ_ARCH_ADA || arch == PDQ_ARCH_BLACKWELL || arch == PDQ_ARCH_NEWER) return 0;
    return -1;
}
/* PORT_DLSS_AUTO_ARCH (PORT_DLSS_AUTO_K is the old name, read when the new one is unset). def = the port's
 * default: R1 1, R2/R3 0 (their old auto is a name heuristic; device-ID classification differs on a few
 * datacenter / laptop parts, so it is opt-in there). 0 = the port's old auto. */
static inline int pdq_auto_arch_enabled(int def)
{
    const char* e = getenv("PORT_DLSS_AUTO_ARCH");
    if (!e || !*e) e = getenv("PORT_DLSS_AUTO_K");
    return (e && *e) ? atoi(e) != 0 : def;
}

/* ---- 5. texture LOD bias for upscaled rendering (PORT_UPSCALE_LOD_BIAS) ---------------------------- */

#define PDQ_LOD_UNSET   (-1)  /* not set: the port's own default bias logic runs unchanged */
#define PDQ_LOD_FIXED   0     /* "off" / "0" / a number: that bias */
#define PDQ_LOD_OUTPUT  1     /* "auto": log2(render / output) */
#define PDQ_LOD_NV      3     /* "nv" / "auto-1": log2(render / output) - 1 (NVIDIA DLSS guide) */
typedef struct pdq_lod_cfg { int mode; float value; } pdq_lod_cfg;

static inline int pdq_streq_i(const char* a, const char* b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return *a == *b;
}
static inline pdq_lod_cfg pdq_lod_parse(const char* s)
{
    pdq_lod_cfg c; c.mode = PDQ_LOD_UNSET; c.value = 0.0f;
    if (!s) return c;
    while (*s == ' ') s++;
    if (!*s) return c;
    if (pdq_streq_i(s, "off")) { c.mode = PDQ_LOD_FIXED; return c; }
    if (pdq_streq_i(s, "auto") || pdq_streq_i(s, "output")) { c.mode = PDQ_LOD_OUTPUT; return c; }
    if (pdq_streq_i(s, "nv") || pdq_streq_i(s, "auto-1") || pdq_streq_i(s, "output-1")) { c.mode = PDQ_LOD_NV; return c; }
    {
        char* end = NULL;
        const double v = strtod(s, &end);
        if (end != s && (!*end || *end == ' ') && v == v) { c.mode = PDQ_LOD_FIXED; c.value = (float)v; }
    }
    return c;   /* garbage: unset (the port's default) */
}
/* ratio = render width / output width (1 when no upscaler runs). The automatic modes give 0 at native
 * (DLAA / AA-only): there is no lower-resolution sampling to compensate. Result clamped to [lo, hi]. */
static inline float pdq_lod_bias(const pdq_lod_cfg* c, float ratio, float lo, float hi)
{
    float b = 0.0f;
    if (c->mode == PDQ_LOD_FIXED) b = c->value;
    else if ((c->mode == PDQ_LOD_OUTPUT || c->mode == PDQ_LOD_NV) && ratio > 0.0f && ratio < 0.99f)
        b = log2f(ratio) - (c->mode == PDQ_LOD_NV ? 1.0f : 0.0f);
    if (b < lo) b = lo;
    if (b > hi) b = hi;
    return b;
}

/* ---- 6. noise in the upscaler input (R2/R3: RPCS3 alpha-to-coverage emulation, port SSAO) ----------- */

/* RSX_AA_A2C: how RPCS3's alpha-to-coverage emulation (single-sample targets, i.e. under the upscaler) decides.
 * NOISE (default): discard unless alpha > hash(gl_FragCoord) -- white noise fixed to the screen pixel; the jittered
 * geometry slides under it, so A2C edges boil under DLSS/FSR. STABLE: alpha >= 0.5, a plain alpha test that the
 * upscaler anti-aliases like any other edge. */
#define PDQ_A2C_NOISE   0
#define PDQ_A2C_STABLE  1
static inline int pdq_a2c_parse(const char* s)
{
    if (!s) return PDQ_A2C_NOISE;
    while (*s == ' ') s++;
    if (pdq_streq_i(s, "stable") || pdq_streq_i(s, "threshold") || pdq_streq_i(s, "1")) return PDQ_A2C_STABLE;
    return PDQ_A2C_NOISE;
}

/* RSX_SSAO_NOISE: the port SSAO picks one of 16 sample rotations per pixel from a 4x4 Bayer grid; its 4x4 blur
 * averages all 16 on planes, but at depth edges a residue of the pattern remains, fixed to the screen pixel.
 * FIXED (default): grid offset 0 every frame (the old pattern). TEMPORAL: the grid shifts by pdq_ssao_offset(run)
 * each run, so every pixel cycles through all 16 rotations in 16 frames and the upscaler averages the residue. */
#define PDQ_SSAO_FIXED     0
#define PDQ_SSAO_TEMPORAL  1
static inline int pdq_ssao_noise_parse(const char* s)
{
    if (!s) return PDQ_SSAO_FIXED;
    while (*s == ' ') s++;
    if (pdq_streq_i(s, "temporal") || pdq_streq_i(s, "1")) return PDQ_SSAO_TEMPORAL;
    return PDQ_SSAO_FIXED;
}
/* Grid offset (x = o & 3, y = o >> 2) for run n: a Bayer-ordered walk, so any 4 consecutive runs land in
 * different 2x2 quadrants of the grid. */
static inline unsigned pdq_ssao_offset(unsigned n)
{
    static const unsigned char t[16] = { 0, 10, 2, 8, 5, 15, 7, 13, 1, 11, 3, 9, 4, 14, 6, 12 };
    return t[n & 15u];
}
/* The rotation index (0..15) the AO shader uses at pixel (x, y) with grid offset o (CPU mirror of the GLSL). */
static inline unsigned pdq_ssao_rotation(int x, int y, unsigned o)
{
    static const unsigned char bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
    return bayer[(((unsigned)y + (o >> 2)) & 3u) * 4u + (((unsigned)x + (o & 3u)) & 3u)];
}

/* RSX_AA_ATEST_MIPSCALE=k (R2/R3 only; default 0.15 under the upscaler in both since 11-15, R3 was off before):
 * coverage-preserving alpha test for alpha-tested programs (and, RSX_AA_ATEST_MIPSCALE_A2C, for programs using the
 * RSX_AA_A2C=stable alpha >= 0.5 test: g = 1 there). Box-filtered alpha mips average thin leaves below the GEQUAL/GREATER cutoff,
 * so minified cut-out foliage thins out and its few surviving texels flicker under the jitter (distant vegetation
 * boil). The test compares min(1, a * (1 + lod * k * g)), lod = the highest mip level read from the one 2D texture
 * unit whose texels reach col0.a (decompiler data-flow; programs whose alpha is not sourced from exactly one 2D unit
 * are left alone; textureQueryLod, pdq_atest_bias_lod for biased samples; RSXFragmentTextureOps.glsl /
 * RSXROPEpilogue.glsl), applied only on jittered main-scene draws (depth pre-pass and material pass alike; shadow
 * maps and other offscreen passes keep the plain test), g = clamp((ref - 0.3) / 0.2, 0, 1): with low
 * cutoffs box-filtered masks already gain coverage in the mips, so only cutoffs above 0.3 are corrected (a simulation
 * of leaf masks over refs 0.3-0.7 / mips 1-5 cut collapsed coverage 28 -> 8 cases at k 0.15 and lowered the mean
 * coverage error; plain k 0.25 over-thickened). Mip 0 / magnified texels (lod <= 0), other test functions, alpha 0
 * and alpha >= 1 are unchanged; the written colour never changes.
 * Parse: unset/empty -> def; "off"/"0"/negative/garbage -> 0; clamped to [0, 1]. */
static inline float pdq_atest_mipscale_parse(const char* s, float def)
{
    char* end = 0;
    double v;
    if (!s) return def;
    while (*s == ' ') s++;
    if (!*s) return def;
    v = strtod(s, &end);
    if (end == s || !(v > 0.0)) return 0.f;
    return v > 1.0 ? 1.f : (float)v;
}
/* func = RSX alpha function & 7 (0 never, 1 less, 2 equal, 3 lequal, 4 greater, 5 nequal, 6 gequal, 7 always) */
static inline float pdq_atest_ref_gate(float ref)
{
    const float g = (ref - 0.3f) / 0.2f;
    return g > 1.f ? 1.f : (g > 0.f ? g : 0.f);
}
static inline float pdq_atest_mip_alpha(float a, float lod, float k, unsigned func, float ref)
{
    float s;
    if ((func != 4u && func != 6u) || !(a < 1.f)) return a;
    s = (a > 0.f ? a : 0.f) * (1.f + (lod > 0.f ? lod : 0.f) * k * pdq_atest_ref_gate(ref));
    return s < 1.f ? s : 1.f;
}
/* Accessed level of a biased (TXB) sample. The shader queries textureQueryLod at the coordinate scaled by 2^bias
 * (derivatives, hence lambda, grow by exactly the bias), so the hardware applies the sampler's clamps; this mirrors that
 * result: lambda + bias clamped to the sampler's [min_lod, max_lod] (non-mipmap samplers: 0 / 0, so only level 0 is
 * read whatever the bias) and then to the image's [0, levels - 1]. */
static inline float pdq_atest_bias_lod(float lambda, float bias, float min_lod, float max_lod, int levels)
{
    const float hi = levels > 1 ? (float)(levels - 1) : 0.f;
    float l = lambda + bias;
    l = l < min_lod ? min_lod : (l > max_lod ? max_lod : l);
    return l < 0.f ? 0.f : (l > hi ? hi : l);
}

/* RSX_DLSS_MASK_FAR=<distance> (R2/R3; default 60 view-distance units, "off"/"0" = old): the moving-object mask (prep
 * pass: the previous depth around the camera-reprojected position deviates -> DLSS bias-current-colour) fades out with
 * distance. Far away it fires mostly on cut-out foliage, whose sub-pixel leaves and the holes between them come and go
 * with the jitter and the alpha mips (no motion at all): the mask then made DLSS take the current, aliased frame there,
 * the same boil the rejected "reactive mask for alpha-tested draws" gave. Moving characters have their own velocity
 * (RSX_DLSS_OBJMV) and the distant camera parallax is small. The fade works on the prep pass's depth linearisation
 * lin = (1/z - 1/f) / (1/n - 1/f) (1 at near, 0 at far, standard and inverted depth alike; ~n/z): weight
 * smoothstep(L, 2L, lin) with L = lin(distance), i.e. full mask nearer than ~distance/2, none beyond distance.
 * Needs the camera planes (RSX_AA_CAMERA_EXACT); until they are known, and when distance >= far, the mask is unchanged.
 * Parse: unset/empty -> def; "off"/"0"/negative/garbage -> 0; clamped to [1, 1e6]. */
static inline float pdq_mask_far_parse(const char* s, float def)
{
    char* end = 0;
    double v;
    if (!s) return def;
    while (*s == ' ') s++;
    if (!*s) return def;
    v = strtod(s, &end);
    if (end == s || !(v > 0.0)) return 0.f;
    return v < 1.0 ? 1.f : (v > 1e6 ? 1e6f : (float)v);
}
/* L for the shader: lin of view distance z with near n / far f; 0 (= no fade) when unknown or not inside (n, f) */
static inline float pdq_mask_far_lin(float z, float n, float f)
{
    double l;
    if (!(z > 0.f) || !(n > 0.f) || !(f > n) || !(z > n) || !(z < f)) return 0.f;
    l = (1.0 / z - 1.0 / f) / (1.0 / n - 1.0 / f);
    return l > 0.0 ? (float)l : 0.f;
}
/* mask weight at lin value l (CPU mirror of the GLSL smoothstep(L, 2L, l)); L <= 0: 1 */
static inline float pdq_mask_far_weight(float l, float L)
{
    float t;
    if (!(L > 0.f)) return 1.f;
    t = (l - L) / L;
    t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
    return t * t * (3.f - 2.f * t);
}
/* ---- RSX_AA_MV_CONFIDENCE verdict ------------------------------------------------------------------- */

/* Depth-grid check of a camera transform (r2aa_conf_pass): valid samples, samples deviating from the previous depth
   with the transform (bad) and with no motion (bad_id). Returns 1 = reject the transform (more than bad_frac of the
   samples off and clearly worse than no motion), 0 = keep, -1 = too few samples for a verdict. */
static inline int pdq_mv_conf_verdict(unsigned valid, unsigned bad, unsigned bad_id, float bad_frac)
{
    if (valid < 64u) return -1;
    return ((float)bad > (float)valid * bad_frac && bad > bad_id + bad_id / 2u + valid / 16u) ? 1 : 0;
}

/* ---- soft-reset rate limit (RSX_AA_CUT_PARALLAX_GAP, RSX_AA_MV_CONFIDENCE_GAP) -------------------------- */

/* A trigger at `frame` may reset the history only when the last reset of the same kind was at least `gap` frames
   earlier (gap 0: always). Only a trigger that resets restarts the window: a run of false triggers a few frames apart
   (R2 intro: a parallax "cut" every 1-8 frames) resets at most once per `gap` frames, and a continuous run cannot hold
   the window open forever. */
typedef struct pdq_gap
{
    uint32_t last;
    int any;
} pdq_gap;

static inline int pdq_gap_ready(const pdq_gap* g, uint32_t frame, uint32_t gap)
{
    return !gap || !g->any || frame - g->last >= gap;
}

static inline void pdq_gap_mark(pdq_gap* g, uint32_t frame)
{
    g->last = frame;
    g->any = 1;
}

/* ready + mark when the trigger resets */
static inline int pdq_gap_reset(pdq_gap* g, uint32_t frame, uint32_t gap)
{
    const int ok = pdq_gap_ready(g, frame, gap);
    if (ok) pdq_gap_mark(g, frame);
    return ok;
}

/* ---- held-back parallax cuts (RSX_AA_CUT_PARALLAX_GAP) ------------------------------------------------------- */

/* Depth-grid result of a held-back parallax cut: a real cut leaves most samples off with the voted motion AND with no
   motion. Voted motion off but no motion fine = a wrong transform (the RSX_AA_MV_CONFIDENCE reject path), not a cut. */
#define PDQ_CUT_CONFIRM_FRAC 0.70f

static inline int pdq_mv_conf_cut(unsigned valid, unsigned bad, unsigned bad_id, float frac)
{
    return valid >= 64u && (float)bad > (float)valid * frac && (float)bad_id > (float)valid * frac;
}

/* The voted motion of a held-back parallax cut is trusted only with a clear majority and a parallax not far beyond the
   distant motion (R2 intro: 7-9 of 17-50 draws, distant 0.01-0.06 vs parallax 0.22-0.37 of the screen = wrong T). */
static inline int pdq_par_trusted(unsigned members, unsigned matched, double motion, double par)
{
    return members * 2u >= matched && par <= 4.0 * (motion > 0.01 ? motion : 0.01);
}

/* The last trusted camera motion may stand in for a missing / untrusted vote only while it is at most `hold` frames old
   (RSX_AA_VOTE_HOLD; < 0 = unlimited): alternating held / failed frames must not reproject with an ever older motion. */
static inline int pdq_last_t_usable(int hold, uint32_t now, uint32_t last_frame)
{
    return hold < 0 || now - last_frame <= (uint32_t)hold;
}

/* Confirmed real cuts: rate-limited by `gap` frames, but deferred rather than dropped -- a cut seen while the gap is
   closed resets when it opens (at most `gap` frames late). `cut` = a held frame's depth check found a real cut this
   frame. Returns 1 = reset now. The caller clears `pend` when the history was reset for another reason. */
typedef struct pdq_cut_confirm
{
    pdq_gap gap;
    uint32_t pend; /* frame + 1 of the first deferred cut, 0 = none */
} pdq_cut_confirm;

static inline int pdq_cut_confirm_step(pdq_cut_confirm* c, uint32_t now, uint32_t gap, int cut)
{
    if (cut && !c->pend) c->pend = now + 1u;
    if (!c->pend || !pdq_gap_ready(&c->gap, now, gap)) return 0;
    c->pend = 0;
    pdq_gap_mark(&c->gap, now);
    return 1;
}

/* Held frames per conf readback slot (frame & 7): frame + 1, 0 = none. */
typedef struct pdq_held
{
    uint32_t at[8];
} pdq_held;

/* Returns 1 when the slot still held an unchecked frame (dropped). */
static inline int pdq_held_push(pdq_held* h, uint32_t frame)
{
    const int dropped = h->at[frame & 7u] != 0;
    h->at[frame & 7u] = frame + 1u;
    return dropped;
}

/* A history reset (any reason, incl. a confirmed cut): frames held before it must not confirm a cut on the clean
   history afterwards. */
static inline void pdq_held_clear(pdq_held* h)
{
    for (int s = 0; s < 8; ++s) h->at[s] = 0;
}

/* First held slot >= start whose readback may be read at `now` (age lag..7; the slot is reused at age 8). Older entries
   are dropped and counted in *expired. The caller clears a slot once read; one not ready yet is retried next frame. */
static inline int pdq_held_next(pdq_held* h, uint32_t now, uint32_t lag, int start, uint32_t* frame, unsigned* expired)
{
    for (int s = start; s < 8; ++s)
    {
        if (!h->at[s]) continue;
        const uint32_t f = h->at[s] - 1u, age = now - f;
        if (age > 7u) { h->at[s] = 0; ++*expired; continue; }
        if (age < lag) continue;
        *frame = f;
        return s;
    }
    return -1;
}

/* ---- env helpers ------------------------------------------------------------------------------------ */

static inline int pdq_env_int(const char* name, int def)
{
    const char* e = getenv(name);
    return (e && *e) ? atoi(e) : def;
}
static inline float pdq_env_float(const char* name, float def)
{
    const char* e = getenv(name);
    return (e && *e) ? (float)atof(e) : def;
}

#endif /* PORT_DLSS_QUALITY_H */
