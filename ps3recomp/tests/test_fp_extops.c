/* RSX_FP_EXTOPS: fragment-program ops ported from RPCS3's
 * FragmentProgramDecompiler (DDX/DDY, TXD, DST, LIT, STR, SFL, BEM, DP2A, RFL,
 * TEXBEM/TXPBEM). String checks on real rsx_fp_decompile output, every program
 * compiled as ps_5_0, and REFL/LIT/DST executed on WARP against reference
 * values. */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "ps3emu/ps3types.h"
#include "rsx_fp_decompiler.h"

static int g_fail, g_pass;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

enum { T_TEMP = 0, T_INPUT = 1, T_CONST = 2 };
#define SWZ_ID ((0u << 9) | (1u << 11) | (2u << 13) | (3u << 15))
#define UNCOND ((1u << 18) | (1u << 19) | (1u << 20) | (0u << 21) | (1u << 23) | (2u << 25) | (3u << 27))

static void put(u8* p, u32 w)
{
    const u32 be = (w << 16) | (w >> 16);
    p[0] = (u8)(be >> 24); p[1] = (u8)(be >> 16); p[2] = (u8)(be >> 8); p[3] = (u8)be;
}
static u32 src(u32 type, u32 reg) { return type | (reg << 2) | SWZ_ID; }

/* One instruction r0.xyzw = op(s0, s1, s2) (END), plus the inline constant k. */
static int decompile(u32 op, u32 s0, u32 s1, u32 s2, u32 input_src, u32 unit,
                     const float k[4], char* out, u32 cap)
{
    u8 uc[32];
    const u32 w0 = 1u | (0u << 1) | (0xFu << 9) | (input_src << 13) | (unit << 17) | (op << 24);
    put(uc + 0, w0);
    put(uc + 4, s0 | UNCOND);
    put(uc + 8, s1);
    put(uc + 12, s2);
    for (int i = 0; i < 4; i++) { u32 b; memcpy(&b, &k[i], 4); put(uc + 16 + 4 * i, b); }
    return rsx_fp_decompile(uc, sizeof uc, 0, out, cap);
}

static int compiles(const char* hlsl, const char* target, ID3DBlob** code)
{
    ID3DBlob* err = NULL; ID3DBlob* c = NULL;
    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), "t", NULL, NULL, "main", target, 0, 0, &c, &err);
    if (FAILED(hr) && err) printf("%s\n%s\n", (const char*)ID3D10Blob_GetBufferPointer(err), hlsl);
    if (err) ID3D10Blob_Release(err);
    if (SUCCEEDED(hr) && code) *code = c; else if (c) ID3D10Blob_Release(c);
    return SUCCEEDED(hr);
}

static ID3D11Device* g_dev;
static ID3D11DeviceContext* g_ctx;

/* Run the instruction's emitted statement in a compute shader with input.tc0 = in. */
static int exec(const char* fp_hlsl, const float in[4], float res[4])
{
    const char* s = strstr(fp_hlsl, "{ float4 _v = ");
    if (!s) return 0;
    const char* e = strstr(s, "}\n");
    if (!e) return 0;
    char cs[4096];
    snprintf(cs, sizeof cs,
             "struct I { float4 tc0; };\n"
             "RWStructuredBuffer<float4> o : register(u0);\n"
             "[numthreads(1,1,1)] void main() {\n"
             "    I input; input.tc0 = float4(%.9g, %.9g, %.9g, %.9g);\n"
             "    float4 r[2]; r[0] = 0; r[1] = 0;\n"
             "    %.*s}\n"
             "    o[0] = r[0];\n}\n", in[0], in[1], in[2], in[3], (int)(e - s), s);
    ID3DBlob* code = NULL;
    if (!compiles(cs, "cs_5_0", &code)) return 0;
    ID3D11ComputeShader* sh = NULL;
    ID3D11Buffer* buf = NULL; ID3D11Buffer* rb = NULL; ID3D11UnorderedAccessView* uav = NULL;
    int ok = 0;
    D3D11_BUFFER_DESC bd = { 16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0,
                             D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16 };
    D3D11_BUFFER_DESC rd = { 16, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0 };
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud; memset(&ud, 0, sizeof ud);
    ud.Format = DXGI_FORMAT_UNKNOWN; ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; ud.Buffer.NumElements = 1;
    if (SUCCEEDED(ID3D11Device_CreateComputeShader(g_dev, ID3D10Blob_GetBufferPointer(code),
                  ID3D10Blob_GetBufferSize(code), NULL, &sh)) &&
        SUCCEEDED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &buf)) &&
        SUCCEEDED(ID3D11Device_CreateBuffer(g_dev, &rd, NULL, &rb)) &&
        SUCCEEDED(ID3D11Device_CreateUnorderedAccessView(g_dev, (ID3D11Resource*)buf, &ud, &uav))) {
        ID3D11DeviceContext_CSSetShader(g_ctx, sh, NULL, 0);
        ID3D11DeviceContext_CSSetUnorderedAccessViews(g_ctx, 0, 1, &uav, NULL);
        ID3D11DeviceContext_Dispatch(g_ctx, 1, 1, 1);
        ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource*)rb, (ID3D11Resource*)buf);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource*)rb, 0, D3D11_MAP_READ, 0, &m))) {
            memcpy(res, m.pData, 16);
            ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource*)rb, 0);
            ok = 1;
        }
    }
    if (uav) ID3D11UnorderedAccessView_Release(uav);
    if (rb) ID3D11Buffer_Release(rb);
    if (buf) ID3D11Buffer_Release(buf);
    if (sh) ID3D11ComputeShader_Release(sh);
    ID3D10Blob_Release(code);
    return ok;
}

static int near4(const float a[4], float x, float y, float z, float w)
{
    const float b[4] = { x, y, z, w };
    for (int i = 0; i < 4; i++) if (fabsf(a[i] - b[i]) > 1e-4f) return 0;
    return 1;
}

static void check_exec(const char* name, u32 op, const float in[4], const float k[4],
                       float x, float y, float z, float w, int check_w)
{
    char h[16384];
    CHECK(decompile(op, src(T_INPUT, 0), src(T_CONST, 0), src(T_CONST, 0), 4, 0, k, h, sizeof h) >= 0,
          "%s decompile", name);
    float r[4] = { 0 };
    CHECK(exec(h, in, r), "%s exec", name);
    if (!check_w) r[3] = w;
    CHECK(near4(r, x, y, z, w), "%s = (%g %g %g %g), want (%g %g %g %g)", name,
          r[0], r[1], r[2], r[3], x, y, z, w);
}

int main(void)
{
    static const float zero[4] = { 0, 0, 0, 0 };
    char h[16384];
    struct { u32 op; const char* name; const char* want; } str[] = {
        { 0x15, "DDX",  "_rsx_ddx(" },
        { 0x16, "DDY",  "_rsx_ddy(" },
        { 0x19, "TXD",  ".SampleGrad(rsx_samp[0], ((input.tc0).xyzw).xy, _rsx_grad(" },
        { 0x07, "DST",  "float4(1.0, ((input.tc0).xyzw).y * " },
        { 0x1E, "LIT",  "exp2(((input.tc0).xyzw).w * log2(max(((input.tc0).xyzw).y, 1e-10)))" },
        { 0x20, "STR",  "(float4)1.0" },
        { 0x21, "SFL",  "(float4)0.0" },
        { 0x2B, "BEM",  ".xyxy + (" },
        { 0x2E, "DP2A", "(dot(((input.tc0).xyzw).xy, " },
        { 0x36, "RFL",  "2.0 * dot(((input.tc0).xyzw).xyz" },
        { 0x33, "TEXBEM", ".Sample(rsx_samp[0], ((((input.tc0).xyzw).xyxy + " },
        { 0x34, "TXPBEM", ".Sample(rsx_samp[0], ((((input.tc0).xyzw).xyxy + " },
    };
    for (unsigned i = 0; i < sizeof str / sizeof str[0]; i++) {
        const int n = decompile(str[i].op, src(T_INPUT, 0), src(T_TEMP, 1), src(T_TEMP, 2), 4, 0, zero, h, sizeof h);
        CHECK(n >= 0, "%s decompile", str[i].name);
        CHECK(strstr(h, str[i].want) != NULL, "%s: missing '%s'\n%s", str[i].name, str[i].want, h);
        CHECK(strstr(h, "unhandled FP opcode") == NULL, "%s still unhandled", str[i].name);
        CHECK(compiles(h, "ps_5_0", NULL), "%s ps_5_0 compile", str[i].name);
    }
    /* DDY flips with the guest window origin, like WPOS; DDX/DDY declare rsx_shscale. */
    decompile(0x16, src(T_INPUT, 0), src(T_TEMP, 1), src(T_TEMP, 2), 4, 0, zero, h, sizeof h);
    CHECK(strstr(h, "(rsx_shscale.z > 0.5) ? -ddy(v) : ddy(v)") != NULL, "DDY origin flip\n%s", h);
    CHECK(strstr(h, "rsx_shscale;") != NULL || strstr(h, "rsx_shscale =") != NULL, "DDY declares rsx_shscale");

    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION,
                                 &g_dev, NULL, &g_ctx))) {
        printf("SKIP exec: no WARP device\n");
    } else {
        /* LIT (RPCS3 _builtin_lit): (1, max(x,0), x > 0 ? y^w : 0, 1). */
        { const float in[4] = { 0.5f, 0.25f, 9.0f, 2.0f };
          check_exec("LIT", 0x1E, in, zero, 1.0f, 0.5f, 0.0625f, 1.0f, 1); }
        { const float in[4] = { -0.5f, 0.25f, 9.0f, 2.0f };
          check_exec("LIT x<0", 0x1E, in, zero, 1.0f, 0.0f, 0.0f, 1.0f, 1); }
        { const float in[4] = { 0.5f, 0.0f, 9.0f, 0.0f };   /* pow(0, 0) == 1 */
          check_exec("LIT y=w=0", 0x1E, in, zero, 1.0f, 0.5f, 1.0f, 1.0f, 1); }
        /* DST: (1, a.y*b.y, a.z, b.w). */
        { const float in[4] = { 7.0f, 2.0f, 3.0f, 9.0f }, k[4] = { 6.0f, 4.0f, 8.0f, 5.0f };
          check_exec("DST", 0x07, in, k, 1.0f, 8.0f, 3.0f, 5.0f, 1); }
        /* RFL (NV_fragment_program): 2*dot(a,b)/dot(a,a)*a - b, axis a not unit length. */
        { const float in[4] = { 0.0f, 2.0f, 0.0f, 0.0f }, k[4] = { 1.0f, 1.0f, 0.0f, 0.0f };
          check_exec("RFL", 0x36, in, k, -1.0f, 1.0f, 0.0f, 0.0f, 0); }
        { const float in[4] = { 3.0f, 0.0f, 4.0f, 0.0f }, k[4] = { 0.0f, 1.0f, 5.0f, 0.0f };
          /* 2*20/25*(3,0,4) - (0,1,5) = (4.8, -1, 1.4) */
          check_exec("RFL 2", 0x36, in, k, 4.8f, -1.0f, 1.4f, 0.0f, 0); }
        /* DP2A: dot(a.xy, b.xy) + c.x. */
        { const float in[4] = { 2.0f, 3.0f, 0.0f, 0.0f }, k[4] = { 4.0f, 5.0f, 0.0f, 0.0f };
          check_exec("DP2A", 0x2E, in, k, 27.0f, 27.0f, 27.0f, 27.0f, 1); }
        ID3D11DeviceContext_Release(g_ctx);
        ID3D11Device_Release(g_dev);
    }
    printf("test_fp_extops: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
