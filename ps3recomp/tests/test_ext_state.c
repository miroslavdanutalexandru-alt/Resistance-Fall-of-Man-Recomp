/* rsx_ext_state.h: alpha-to-coverage, user clip planes, point sprites and the
 * per-texture sampler decode; the VS variant text patch on real decompiler
 * output (compiled with D3DCompile when d3dcompiler_47.dll is present). */
#define _CRT_SECURE_NO_WARNINGS
#include "rsx_ext_state.h"
#include "rsx_vp_decompiler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

static int g_fail, g_compiled;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define APPROX(a, b) ((a) - (b) < 1e-4f && (b) - (a) < 1e-4f)

static void test_a2c(void)
{
    const uint32_t off = (0u << 16) | (7u << 8) | 0u;        /* test disabled */
    CHECK(rsx_a2c_alpha_ctl(off, 0xFFFF0000u) == off);      /* A2C off: untouched */
    CHECK(rsx_a2c_alpha_ctl(off, 0xFFFF0010u) == ((1u << 16) | (6u << 8) | 128u));
    /* GREATER 0.1 is weaker than coverage at 0.5: raised */
    CHECK(rsx_a2c_alpha_ctl((1u << 16) | (4u << 8) | 25u, 0x10u) == ((1u << 16) | (6u << 8) | 128u));
    /* GEQUAL 200 already stricter: kept */
    CHECK(rsx_a2c_alpha_ctl((1u << 16) | (6u << 8) | 200u, 0x10u) == ((1u << 16) | (6u << 8) | 200u));
    /* an EQUAL test is the game's own decision */
    CHECK(rsx_a2c_alpha_ctl((1u << 16) | (2u << 8) | 9u, 0x10u) == ((1u << 16) | (2u << 8) | 9u));
}

static void test_clip(void)
{
    float f[6];
    CHECK(rsx_clip_factors(0, f) == 0 && f[0] == 0.0f && f[5] == 0.0f);
    /* plane0 LT, plane2 GE, plane5 LT */
    CHECK(rsx_clip_factors(0x100201u, f) == 3);
    CHECK(f[0] == -1.0f && f[1] == 0.0f && f[2] == 1.0f && f[3] == 0.0f && f[4] == 0.0f && f[5] == -1.0f);
    float t[12];
    rsx_vsx_tail(t, 0x21u, 4.0f, 0, 0, 640.0f, -360.0f);
    CHECK(t[0] == -1.0f && t[1] == 1.0f && t[2] == 0.0f);
    CHECK(t[8] == 4.0f && APPROX(t[9], 1.0f / 640.0f) && APPROX(t[10], 1.0f / 360.0f));
}

static void test_psprite(void)
{
    CHECK(rsx_psprite_mask(0x00000100u) == 0);               /* sprite disabled: no replacement */
    CHECK(rsx_psprite_mask(0x00000301u) == 3u);              /* TEX0 | TEX1 */
    CHECK(rsx_psprite_mask(0x00020001u) == 0x200u);          /* TEX9 */
    CHECK(rsx_psprite_rmode(0x5u) == 2u);
    CHECK(rsx_point_size_clamp(0.25f) == 1.0f && rsx_point_size_clamp(100.0f) == 64.0f &&
          rsx_point_size_clamp(8.0f) == 8.0f);
    /* six vertices cover the square: corners 0 1 2 / 2 1 3, both triangles the same winding */
    int seen[4] = {0};
    for (uint32_t k = 0; k < 6; k++) seen[rsx_psprite_corner(k)]++;
    CHECK(seen[0] == 1 && seen[1] == 2 && seen[2] == 2 && seen[3] == 1);
    float x[6], y[6];
    for (uint32_t k = 0; k < 6; k++) { const uint32_t c = rsx_psprite_corner(k);
        x[k] = (c & 1u) ? 1.0f : -1.0f; y[k] = (c & 2u) ? -1.0f : 1.0f; }
    const float a0 = (x[1] - x[0]) * (y[2] - y[0]) - (y[1] - y[0]) * (x[2] - x[0]);
    const float a1 = (x[4] - x[3]) * (y[5] - y[3]) - (y[4] - y[3]) * (x[5] - x[3]);
    CHECK(a0 != 0.0f && (a0 > 0.0f) == (a1 > 0.0f));
    float s, t;
    rsx_psprite_uv(0, &s, &t); CHECK(s == 0.0f && t == 0.0f);   /* top-left */
    rsx_psprite_uv(3, &s, &t); CHECK(s == 1.0f && t == 1.0f);
    float tail[12]; uint32_t fl;
    rsx_vsx_tail(tail, 0, 0.0f, 0x00000305u, 0x20u, 640.0f, 360.0f);
    memcpy(&fl, &tail[11], 4);
    CHECK(fl == (1u | (3u << 8) | (2u << 4)));
    CHECK(tail[8] == 1.0f);
}

static void test_sampler(void)
{
    /* LINEAR_LINEAR min, LINEAR mag, bias -0.5 (0x1F80), aniso 8x, LOD 1..4 */
    const uint32_t filt = (6u << 16) | (2u << 24) | 0x1F80u;
    const uint32_t c0 = 0x80000000u | (4u << 4) | ((4u * 256u) << 7) | ((1u * 256u) << 19);
    rsx_gsamp g = rsx_gsamp_decode(filt, c0, 0x00030201u, 0, 0.0f, 1);
    CHECK(g.min_linear && g.mag_linear && g.mip == 2 && g.aniso == 8);
    CHECK(APPROX(g.bias, -0.5f) && APPROX(g.min_lod, 1.0f) && APPROX(g.max_lod, 4.0f));
    CHECK(g.addr[0] == 1 && g.addr[1] == 2 && g.addr[2] == 3);
    /* overrides: RSX_ANISO=16, upscaler bias -1 */
    g = rsx_gsamp_decode(filt, c0, 0, 16, -1.0f, 0);
    CHECK(g.aniso == 16 && APPROX(g.bias, -1.5f) && g.addr[0] == 1 && g.addr[1] == 1);
    /* NEAREST everything, no mips: base level only, no anisotropy */
    g = rsx_gsamp_decode((1u << 16) | (1u << 24), c0, 0, 16, 0.0f, 0);
    CHECK(!g.min_linear && !g.mag_linear && g.mip == 0 && g.aniso == 1);
    CHECK(g.min_lod == 0.0f && g.max_lod == 0.0f);
    /* LINEAR_NEAREST: point mip */
    g = rsx_gsamp_decode((4u << 16) | (2u << 24), 0, 0, 0, 0.0f, 0);
    CHECK(g.min_linear && g.mip == 1);
    /* unprogrammed registers decode as trilinear over the whole chain */
    g = rsx_gsamp_decode(0, 0, 0, 0, 0.0f, 0);
    CHECK(g.min_linear && g.mag_linear && g.mip == 2 && g.aniso == 1);
    CHECK(g.min_lod == 0.0f && g.max_lod >= 15.9f && rsx_gsamp_d3d_filter(&g) == 0x15u);
    /* D3D filter encodings: aniso, LINEAR_NEAREST (min/mag linear, point mip), NEAREST */
    g = rsx_gsamp_decode(filt, c0, 0, 0, 0.0f, 0); CHECK(rsx_gsamp_d3d_filter(&g) == 0x55u);
    g = rsx_gsamp_decode((4u << 16) | (2u << 24), c0 & ~0x70u, 0, 0, 0.0f, 0); CHECK(rsx_gsamp_d3d_filter(&g) == 0x14u);
    g = rsx_gsamp_decode((1u << 16) | (1u << 24), c0, 0, 0, 0.0f, 0); CHECK(rsx_gsamp_d3d_filter(&g) == 0x0u);
    /* the key tells the cases apart */
    const rsx_gsamp a = rsx_gsamp_decode(filt, c0, 0, 0, 0.0f, 0);
    const rsx_gsamp b = rsx_gsamp_decode(filt ^ 0x100u, c0, 0, 0, 0.0f, 0);
    const rsx_gsamp c = rsx_gsamp_decode(filt, c0 ^ (1u << 4), 0, 0, 0.0f, 0);
    CHECK(rsx_gsamp_key(&a) != rsx_gsamp_key(&b) && rsx_gsamp_key(&a) != rsx_gsamp_key(&c));
    CHECK(rsx_gsamp_key(&a) == rsx_gsamp_key(&a));
    /* max LOD 16.0 (CONTROL0 unwritten) must not alias max LOD 0 (base level only) */
    const rsx_gsamp full = rsx_gsamp_decode(filt, 0, 0, 0, 0.0f, 0);
    const rsx_gsamp base = rsx_gsamp_decode(filt, 0x80000000u, 0, 0, 0.0f, 0);
    CHECK(full.max_lod >= 15.9f && base.max_lod == 0.0f && rsx_gsamp_key(&full) != rsx_gsamp_key(&base));
    /* the bias still separates keys at the top of the LOD range */
    const rsx_gsamp fb = rsx_gsamp_decode(filt, 0, 0, 0, 0.5f, 0);
    CHECK(rsx_gsamp_key(&full) != rsx_gsamp_key(&fb));
}

#ifdef _WIN32
typedef HRESULT (WINAPI *PFN_Compile)(const void*, SIZE_T, const char*, const void*, void*, const char*,
                                      const char*, UINT, UINT, void**, void**);
static int hlsl_compiles_p(const char* src, const char* prof)
{
    static PFN_Compile fn; static int tried;
    if (!tried) { tried = 1; HMODULE m = LoadLibraryA("d3dcompiler_47.dll");
                  if (m) fn = (PFN_Compile)GetProcAddress(m, "D3DCompile"); }
    if (!fn) return -1;
    void* code = NULL; void* err = NULL;
    const HRESULT hr = fn(src, strlen(src), "vsx", NULL, NULL, "main", prof, 0, 0, &code, &err);
    if (FAILED(hr) && err) {
        /* ID3DBlob vtable: QueryInterface, AddRef, Release, GetBufferPointer, GetBufferSize */
        void** vt = *(void***)err;
        const char* msg = ((const char* (WINAPI*)(void*))vt[3])(err);
        printf("D3DCompile: %s\n", msg ? msg : "?");
    }
    if (code) ((ULONG (WINAPI*)(void*))(*(void***)code)[2])(code);
    if (err) ((ULONG (WINAPI*)(void*))(*(void***)err)[2])(err);
    return SUCCEEDED(hr) ? 1 : 0;
}
static int hlsl_compiles(const char* src) { return hlsl_compiles_p(src, "vs_5_0"); }
#endif

static void test_vs_patch(void)
{
    uint8_t uc[16] = {0};
    uc[12] = 1;                                   /* one NOP with the end bit */
    static char base[65536], h[65536];
    int n = rsx_vp_decompile(uc, sizeof uc, base, sizeof base);
    CHECK(n >= 0);
    /* RSX_VS_STRICT: temporaries, outputs and position precise; the rest byte-identical */
    {
        static char pt[65536 + 64];
        const int pn = rsx_vs_precise_text(base, pt, sizeof pt);
        CHECK(pn == (int)strlen(base) + 24 && pn == (int)strlen(pt));
        CHECK(strstr(pt, "    precise float4 r[32]; precise float4 o[16];\n") != NULL);
        CHECK(strstr(pt, "    precise float4 _p = o[0];\n") != NULL);
        CHECK(rsx_vs_precise_text(base, pt, strlen(base) + 8) == -1);           /* too small */
        CHECK(rsx_vs_precise_text("float4 main() : SV_Position { return 0; }", pt, sizeof pt) == -1);
#ifdef _WIN32
        const int c = hlsl_compiles(pt);
        if (c >= 0) { CHECK(c == 1); g_compiled++; }
#endif
    }
    /* flags 0: byte-identical */
    strcpy(h, base); CHECK(rsx_vsx_patch(h, sizeof h, 0) == 0 && strcmp(h, base) == 0);
    for (uint32_t fl = 1; fl <= 7; fl++) {
        strcpy(h, base);
        CHECK(rsx_vsx_patch(h, sizeof h, fl) == 0);
        CHECK(strstr(h, "float4 vp_clipf[2];") != NULL);
        CHECK((strstr(h, "SV_ClipDistance0") != NULL) == ((fl & RSX_VSX_CLIP) != 0));
        CHECK((strstr(h, "SV_VertexID") != NULL) == ((fl & RSX_VSX_PSPRITE) != 0));
        CHECK(strstr(h, "VSOutput main(VSInput input) {") == NULL || !(fl & RSX_VSX_PSPRITE));
        CHECK((strstr(h, "Out.vsx_bc0 = o[3]; Out.vsx_bc1 = o[4];") != NULL) == ((fl & RSX_VSX_BCOL) != 0));
        if ((fl & RSX_VSX_BCOL) && (fl & RSX_VSX_CLIP))   /* back colours before the clip distances */
            CHECK(strstr(h, "vsx_bc1:COLOR3;") < strstr(h, "vsx_clip0:SV_ClipDistance0"));
#ifdef _WIN32
        const int c = hlsl_compiles(h);
        if (c >= 0) { CHECK(c == 1); g_compiled++; }
#endif
    }
    /* the tail sits right after vp_gfx: vp_c 8192 + 3 x 16 + vp_gfa 256 + vp_gfc 256 + vp_gfx 16 */
    CHECK(RSX_VSX_CB_OFF == 8192u + 48u + 256u + 256u + 16u);
    /* compact input with no attributes declares its own SV_VertexID: reused, not duplicated */
    n = rsx_vp_decompile_compact_ex(uc, sizeof uc, 0, 0, base, sizeof base);
    if (n >= 0 && strstr(base, "uint vertex_id:SV_VertexID;")) {
        strcpy(h, base);
        CHECK(rsx_vsx_patch(h, sizeof h, RSX_VSX_PSPRITE | RSX_VSX_CLIP) == 0);
        CHECK(strstr(h, "input.vertex_id % 6u") != NULL && strstr(h, "vsx_vid") == NULL);
#ifdef _WIN32
        const int c = hlsl_compiles(h);
        if (c >= 0) { CHECK(c == 1); g_compiled++; }
#endif
    }
    /* missing anchors / no room: refused, text untouched */
    strcpy(h, "float4 main() : SV_Position { return 0; }");
    CHECK(rsx_vsx_patch(h, sizeof h, RSX_VSX_CLIP) == -1 && strcmp(h, "float4 main() : SV_Position { return 0; }") == 0);
    strcpy(h, base);
    CHECK(rsx_vsx_patch(h, strlen(base) + 16, RSX_VSX_CLIP) == -1 && strcmp(h, base) == 0);
}

static void test_ext2_state(void)
{
    /* MRT: unwritten -> mirror RT0 (no bits) */
    CHECK(rsx_x_mrt(0xF, 1, 0, 0, 0, 0) == 0);
    /* COLOR_MASK_MRT nibble: bit0 A bit1 R bit2 G bit3 B */
    CHECK(rsx_mrt_nibble_d3d(0x2u) == 1u && rsx_mrt_nibble_d3d(0x1u) == 8u && rsx_mrt_nibble_d3d(0xFu) == 0xFu);
    {   /* RT1 = R only, RT2 = off, RT3 = all; blend on RT3 only */
        const uint32_t x = rsx_x_mrt(0xF, 1, 0x8u, 1, (0xFu << 12) | (0x2u << 4), 1);
        CHECK(x & RSX_X_MRT_VALID);
        CHECK((x & 0xFu) == 1u && ((x >> 4) & 0xFu) == 0u && ((x >> 8) & 0xFu) == 0xFu);
        CHECK(((x >> 12) & 7u) == 4u);
        /* only the blend register written: masks mirror RT0 */
        const uint32_t y = rsx_x_mrt(0x7, 0, 0x2u, 1, 0, 0);
        CHECK((y & 0xFFFu) == 0x777u && ((y >> 12) & 7u) == 1u);
    }
    CHECK(rsx_x_logic(0, 0x1500u) == RSX_LOGIC_NONE && rsx_x_logic(1, 0x1500u) == RSX_LOGIC_CLEAR);
    CHECK(rsx_x_logic(1, 0x1505u) == RSX_LOGIC_NOOP && rsx_x_logic(1, 0x1503u) == RSX_LOGIC_COPY);
    CHECK(rsx_x_logic(1, 0x1506u) == RSX_LOGIC_OTHER);
    CHECK(rsx_x_fill(0x1B02u, 0x1B02u, 0, 0) == 0 && rsx_x_fill(0x1B01u, 0x1B01u, 0, 0) == RSX_X_WIRE);
    CHECK(rsx_x_fill(0x1B02u, 0x1B01u, 1, 0x0404u) == RSX_X_WIRE);   /* front culled: back's mode */
    CHECK(rsx_x_fill(0x1B02u, 0x1B01u, 1, 0x0405u) == 0);
    CHECK(rsx_x_fill(0x1B00u, 0x1B00u, 0, 0) == RSX_X_POINTFILL);
    {   /* window offset (x=16, y=8) moves the viewport; out of the target -> full-target fallback */
        float xy[2] = { -1.0f, -1.0f };
        CHECK(rsx_window_viewport(0, 0, 640, 360, (8u << 16) | 16u, 1280.0f, 720.0f, xy) && xy[0] == 16.0f && xy[1] == 8.0f);
        CHECK(rsx_window_viewport(4, 2, 640, 360, 0u, 1280.0f, 720.0f, xy) && xy[0] == 4.0f && xy[1] == 2.0f);
        CHECK(!rsx_window_viewport(0, 0, 1280, 720, 16u, 1280.0f, 720.0f, xy));
        CHECK(!rsx_window_viewport(0, 0, 1, 720, 0u, 1280.0f, 720.0f, xy));
    }
    CHECK(rsx_depth_clip(1u) && !rsx_depth_clip(0x11u) && !rsx_depth_clip(0u) && !rsx_depth_clip(0x10u));
    {
        float s = 1.0f, o = 0.0f;
        CHECK(!rsx_depth_range_xform(0.0f, 1.0f, &s, &o) && s == 1.0f);
        CHECK(!rsx_depth_range_xform(0.5f, 0.5f, &s, &o));
        CHECK(rsx_depth_range_xform(0.25f, 0.75f, &s, &o));
        /* window z 0.5 -> 0.5 normalised, mapped back to 0.25 + 0.5 * 0.5 = 0.5 */
        CHECK(APPROX(0.5f * s + o, 0.5f) && APPROX(0.25f * s + o, 0.0f) && APPROX(0.75f * s + o, 1.0f));
        /* RSX_POLY_OFFSET=3: a VS z bias added before the range xform lands in window z exactly */
        float s2 = 1.0f, o2 = rsx_zbias_window(3.0f, 2u);
        CHECK(rsx_depth_range_xform(0.25f, 0.75f, &s2, &o2));
        CHECK(((0.5f * s2 + o2) * 0.5f + 0.25f) - 0.5f > 1.5f / 16777215.0f &&
              ((0.5f * s2 + o2) * 0.5f + 0.25f) - 0.5f < 4.5f / 16777215.0f);   /* float ulp at 0.5 ~ 1 unit */
    }
    /* polygon-offset bias units: Z24 = 1/16777215, Z16 = 1/65535 of the range */
    CHECK(rsx_zbias_window(16777215.0f, 2u) == 1.0f && rsx_zbias_window(65535.0f, 1u) == 1.0f);
    CHECK(rsx_zbias_window(-2.0f, 1u) == -2.0f / 65535.0f && rsx_zbias_window(0.0f, 2u) == 0.0f);
    CHECK(rsx_zbias_window(1.0f, 0u) == 1.0f / 16777215.0f);   /* no zeta / unset: D24 units */
    CHECK(rsx_fog_code(0x2601u) == 1u && rsx_fog_code(0x0800u) == 2u && rsx_fog_code(0x0804u) == 6u && rsx_fog_code(7u) == 0u);
    CHECK(APPROX(rsx_fog_eval(1u, 1.5f, 0.5f, 0.4f), 0.7f));
    CHECK(APPROX(rsx_fog_eval(6u, 1.5f, 0.5f, -0.4f), 0.7f));
    CHECK(rsx_fog_eval(1u, 3.0f, 1.0f, 1.0f) == 1.0f && rsx_fog_eval(0u, 3.0f, 1.0f, 1.0f) == 0.0f);
    CHECK(APPROX(rsx_fog_eval(2u, 1.5f, 0.0f, 0.0f), 1.0f) && APPROX(rsx_fog_eval(3u, 1.5f, 0.0f, 0.0f), 1.0f));
    float c[4];
    rsx_border_rgba(0x80FF4000u, c);
    CHECK(c[0] == 1.0f && APPROX(c[1], 64.0f / 255.0f) && c[2] == 0.0f && APPROX(c[3], 128.0f / 255.0f));
    CHECK(rsx_addr_border(0x0104u) && rsx_addr_border(0x0701u) && !rsx_addr_border(0x0303u));
    /* per axis: U=WRAP V=BORDER flags V only; mode 7 is mirror-once-border */
    CHECK(rsx_addr_border_axes(0x0401u) == 2u && rsx_addr_border_axes(0x0104u) == 1u && rsx_addr_border_axes(0x0707u) == 3u);
    CHECK(rsx_addr_mirror_once_border_axes(0x0704u) == 2u && rsx_addr_mirror_once_border_axes(0x0407u) == 1u);
    /* one helper per unit: mode 7 and mirror + border units take the border path */
    CHECK(rsx_mirror_flags(0x0602u) == 9u && rsx_mirror_flags(0x0404u) == 0u);
    CHECK(rsx_sampb_code(0x0707u, 0) == (3u | (0xCu << 2)) && rsx_sampb_code(0x0707u, 1) == (3u | (0xCu << 2)));
    CHECK(rsx_sampb_code(0x0402u, 0) == (2u | (1u << 2)));      /* U mirror, V border */
    CHECK(rsx_sampb_code(0x0402u, 1) == 0u && rsx_sampb_code(0x0202u, 0) == 0u);
    CHECK(rsx_border_emul_axes(0x0704u, 0) == 3u && rsx_border_emul_axes(0x0704u, 1) == 2u && rsx_border_emul_axes(0x0404u, 1) == 0u);
    {
        int o = 0;
        float t = rsx_border_axis(-0.25f, 1, 4, &o);          /* mirrored: inside */
        CHECK(!o && APPROX(t, 0.25f));
        t = rsx_border_axis(-1.5f, 1, 4, &o); CHECK(o && APPROX(t, 0.875f));
        t = rsx_border_axis(-0.25f, 0, 4, &o); CHECK(o && APPROX(t, 0.125f));   /* plain border: outside */
        t = rsx_border_axis(0.5f, 0, 4, &o);  CHECK(!o && APPROX(t, 0.5f));
    }
    {   /* pipeline manifest header: other device caps are discarded, not replayed */
        const uint8_t want[8] = { 'R', 'F', 'P', 'S', 'O', 3, 56, rsx_psom_caps(1, 0, 1) };
        uint8_t h8[8]; memcpy(h8, want, 8);
        CHECK(want[7] == 0x85u && rsx_psom_hdr_check(h8, want) == 0);
        h8[7] = rsx_psom_caps(0, 0, 1); CHECK(rsx_psom_hdr_check(h8, want) == 1);
        h8[7] = 0; CHECK(rsx_psom_hdr_check(h8, want) == 1);            /* pre-caps v3 manifest */
        h8[5] = 2; CHECK(rsx_psom_hdr_check(h8, want) == -1);
    }
    {   /* CONSTANT_ALPHA without device ALPHA_FACTOR: Ac splat */
        float f[4] = { 0.1f, 0.2f, 0.3f, 0.75f };
        CHECK(rsx_blend_const_alpha_fixup(0x8003u, 0x8004u, 1, f) == 0 && f[0] == 0.1f);
        CHECK(rsx_blend_const_alpha_fixup(0x0302u, 0x0303u, 0, f) == 0 && f[0] == 0.1f);
        CHECK(rsx_blend_const_alpha_fixup(0x80010001u, 0x0001u, 0, f) == 0 && f[0] == 0.1f);   /* alpha half only */
        CHECK(rsx_blend_const_alpha_fixup(0x8001u, 0x8004u, 0, f) == -1 && f[0] == 0.1f);
        CHECK(rsx_blend_const_alpha_fixup(0x0302u, 0x8004u, 0, f) == 1 && f[0] == 0.75f && f[1] == 0.75f && f[2] == 0.75f && f[3] == 0.75f);
    }
    rsx_blend_factor(0x40003C00u, 0x3C000000u, 1, c);   /* R 1.0, G 2.0, B 0, A 1.0 */
    CHECK(c[0] == 1.0f && c[1] == 2.0f && c[2] == 0.0f && c[3] == 1.0f);
    rsx_blend_factor(0xFF0000FFu, 0, 0, c);
    CHECK(c[0] == 0.0f && c[2] == 1.0f && c[3] == 1.0f);
    {
        float r[4] = { 0, 0, 1280, 720 };
        CHECK(!rsx_scissor_clip(r, 0, 0, 4096, 4096) && r[2] == 1280.0f);
        CHECK(!rsx_scissor_clip(r, 0, 0, 0, 0));
        CHECK(rsx_scissor_clip(r, 100, 50, 200, 4000) && r[0] == 100.0f && r[1] == 50.0f && r[2] == 300.0f && r[3] == 720.0f);
        float e[4] = { 0, 0, 64, 64 };
        CHECK(rsx_scissor_clip(e, 100, 100, 10, 10) && e[2] == e[0] && e[3] == e[1]);
    }
    {   /* scissored clears: split-screen halves keep the other player's half */
        float r[4];
        CHECK(rsx_clear_rect(0, 0, 4096, 4096, 0, 1280, 720, r) == 0 && r[2] == 1280.0f);
        CHECK(rsx_clear_rect(0, 0, 0, 0, 0, 1280, 720, r) == 0);
        CHECK(rsx_clear_rect(0, 0, 1280, 720, 0, 1280, 720, r) == 0);
        CHECK(rsx_clear_rect(640, 0, 640, 720, 0, 1280, 720, r) == 1 && r[0] == 640.0f && r[2] == 1280.0f && r[3] == 720.0f);
        CHECK(rsx_clear_rect(0, 0, 640, 720, 0, 1280, 720, r) == 1 && r[0] == 0.0f && r[2] == 640.0f);
        CHECK(rsx_clear_rect(0, 0, 640, 720, 640u, 1280, 720, r) == 1 && r[0] == 640.0f && r[2] == 1280.0f);
        CHECK(rsx_clear_rect(2000, 0, 100, 100, 0, 1280, 720, r) == -1);
    }
}

static const char k_ps[] =
    "struct PSInput {\n"
    "    float4 position : SV_POSITION; float4 col0 : COLOR0; float4 col1 : COLOR1;\n"
    "    float4 fog : FOG;\n"
    "    float4 tc0:TEXCOORD0; float4 tc1:TEXCOORD1; float4 tc2:TEXCOORD2; float4 tc3:TEXCOORD3;\n"
    "    float4 tc4:TEXCOORD4; float4 tc5:TEXCOORD5; float4 tc6:TEXCOORD6; float4 tc7:TEXCOORD7;\n"
    "};\n"
    "Texture2D    rsx_tex[16] : register(t0);\n"
    "SamplerState rsx_samp[16] : register(s0);\n"
    "float4 rsx_sampc(Texture2D t, SamplerState s, float2 uv) { return t.Sample(s, saturate(uv)); }\n"
    "%s"
    "float4 main(PSInput input) : SV_TARGET {\n"
    "    float4 r0 = rsx_tex[0].Sample(rsx_samp[0], ((input.tc0).xyzw).xy) * saturate(input.col0);\n"
    "    float4 r1 = rsx_sampc(rsx_tex[2], rsx_samp[2], input.tc1.xy) + input.col1;\n"
    "    return lerp(r1, r0, input.fog.y) + rsx_alphatest.x;\n"
    "}\n";

static void test_fpx_patch(void)
{
    static const char* const b1[] = {
        "cbuffer RsxAlphaTest : register(b1) { float4 _rsx_atpad[4]; float4 rsx_alphatest; };\n",
        "cbuffer RsxB1 : register(b1) { float4 rsx_texscale[4]; float4 rsx_alphatest; float4 fp_k[64]; float4 rsx_shscale; };\n",
    };
    static char base[16384], h[16384];
    for (int f = 0; f < 2; f++) {
        snprintf(base, sizeof base, k_ps, b1[f]);
        strcpy(h, base);
        CHECK(rsx_fpx_patch(h, sizeof h, 0) == 0 && strcmp(h, base) == 0);
        /* fog: every input.fog read goes through the fog equation */
        strcpy(h, base);
        CHECK(rsx_fpx_patch(h, sizeof h, RSX_FPX_FOG) == 1);
        CHECK(strstr(h, "_rsx_fogv(input.fog).y") != NULL && strstr(h, "float4 rsx_ext[5];") != NULL);
        CHECK(strstr(h, f ? "float4 rsx_shscale; float4 rsx_ext[5]; };" : "float4 rsx_alphatest; float4 _rsx_xpad[65]; float4 rsx_ext[5]; };") != NULL);
        /* everything: two-sided colours, flat, border on unit 0 (U, V) and 2 (V only) */
        strcpy(h, base);
        const uint64_t all = RSX_FPX_FOG | RSX_FPX_TWOSIDE | RSX_FPX_FLAT | (1ull << RSX_FPX_BORDER_SHIFT)
                           | (5ull << RSX_FPX_BORDERV_SHIFT);
        CHECK(rsx_fpx_patch(h, sizeof h, all) == 1);
        CHECK(strstr(h, "(input._rsx_front ? input.col0 : input.bcol0)") != NULL);
        CHECK(strstr(h, "(input._rsx_front ? input.col1 : input.bcol1)") != NULL);
        CHECK(strstr(h, "float4 tc8:TEXCOORD8; float4 tc9:TEXCOORD9;\n    nointerpolation float4 bcol0 : COLOR2;") != NULL);
        CHECK(strstr(h, "bool _rsx_front : SV_IsFrontFace;") != NULL);
        CHECK(strstr(h, "nointerpolation float4 col0 : COLOR0;") != NULL);
        CHECK(strstr(h, "rsx_sampb(rsx_tex[0], rsx_samp[0], 0u, 3u, ((input.tc0).xyzw).xy)") != NULL);
        CHECK(strstr(h, "rsx_sampb(rsx_tex[2], rsx_samp[2], 2u, 2u, input.tc1.xy)") != NULL);
#ifdef _WIN32
        int c = hlsl_compiles_p(h, "ps_5_0");
        if (c >= 0) { CHECK(c == 1); g_compiled++; }
#endif
        /* mode-7 unit 0 and U-mirror / V-border unit 2: rsx_sampb carries the mirror flags */
        {
            uint8_t mir[16] = { 0 };
            mir[0] = (uint8_t)(rsx_sampb_code(0x0707u, 0) >> 2); mir[2] = (uint8_t)(rsx_sampb_code(0x0402u, 0) >> 2);
            const uint64_t bx = (1ull << RSX_FPX_BORDER_SHIFT) | (1ull << RSX_FPX_BORDERV_SHIFT) | (4ull << RSX_FPX_BORDERV_SHIFT);
            strcpy(h, base);
            CHECK(rsx_fpx_patch_m(h, sizeof h, bx, mir) == 1);
            CHECK(strstr(h, "rsx_sampb(rsx_tex[0], rsx_samp[0], 0u, 51u, ((input.tc0).xyzw).xy)") != NULL);
            CHECK(strstr(h, "rsx_sampb(rsx_tex[2], rsx_samp[2], 2u, 6u, input.tc1.xy)") != NULL);
            CHECK(strstr(h, "rsx_tex[0].Sample(rsx_samp[0], (") == NULL);
#ifdef _WIN32
            int c = hlsl_compiles_p(h, "ps_5_0");
            if (c >= 0) { CHECK(c == 1); g_compiled++; }
#endif
        }
        /* border bit on a unit the program never samples: untouched */
        strcpy(h, base);
        CHECK(rsx_fpx_patch(h, sizeof h, 2u << RSX_FPX_BORDER_SHIFT) == 0 && strcmp(h, base) == 0);
        CHECK(rsx_fpx_patch(h, sizeof h, 2ull << RSX_FPX_BORDERV_SHIFT) == 0 && strcmp(h, base) == 0);
    }
    /* no b1 declared at all: RsxExt added */
    snprintf(base, sizeof base, k_ps, "");
    char* at = strstr(base, " + rsx_alphatest.x"); if (at) memmove(at, at + 18, strlen(at + 18) + 1);
    strcpy(h, base);
    CHECK(rsx_fpx_patch(h, sizeof h, RSX_FPX_FOG) == 1 && strstr(h, "cbuffer RsxExt : register(b1) { float4 _rsx_xpad[70];") != NULL);
#ifdef _WIN32
    { int c = hlsl_compiles_p(h, "ps_5_0"); if (c >= 0) { CHECK(c == 1); g_compiled++; } }
#endif
    /* buffered constant layout: refused */
    strcpy(h, "cbuffer PSConstants : register(b1) { float4 x; };\nfloat4 main() : SV_TARGET { return 0; }");
    CHECK(rsx_fpx_patch(h, sizeof h, RSX_FPX_FOG) == -1);
}

int main(void)
{
    test_a2c();
    test_clip();
    test_psprite();
    test_sampler();
    test_vs_patch();
    test_ext2_state();
    test_fpx_patch();
    if (g_fail) { printf("%d failure(s)\n", g_fail); return 1; }
    printf("ext_state: all passed (%d VS variants compiled)\n", g_compiled);
    return 0;
}
