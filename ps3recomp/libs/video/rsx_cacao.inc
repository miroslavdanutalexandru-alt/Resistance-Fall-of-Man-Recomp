/* rsx_cacao.inc -- FidelityFX CACAO ambient occlusion for Fall of Man (included by
 * rsx_ssao.inc, which decides where it runs: RFOM_AO_MODE=cacao).
 *
 * A port of AMD FidelityFX CACAO (Combined Adaptive Compute Ambient Occlusion), the
 * same as the Resistance 2 / 3 ports' VKR2Cacao.h, as full-screen pixel passes:
 *   1. depth -> view-space Z and normals, deinterleaved into 2x2 half-size layers
 *   2. AO per layer (quality Low / Medium / High: 3 / 5 / 12 tap pairs; Medium and
 *      High add detail AO and edge-aware fading, High normal-based edges)
 *   3. edge-aware blur inside each layer (1 pass Low, 2 Medium / High)
 *   4. the four layers back to full resolution -> the R8 AO target of rsx_ssao.inc,
 *      which then multiplies it into the scene (no further blur).
 *
 * FidelityFX CACAO license (https://github.com/GPUOpen-Effects/FidelityFX-CACAO):
 * Modifications Copyright (c) 2020-2021 Advanced Micro Devices, Inc. All rights reserved.
 * Copyright (c) 2016, Intel Corporation
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this
 * software and associated documentation files (the "Software"), to deal in the Software
 * without restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to the following
 * conditions: The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 * PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
 * FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE. */

static const char s_cc_hlsl[] =
"cbuffer K : register(b0) { float4 D; float4 Z; float4 N; float4 E0; float4 E1; float4 E2; float4 M; };\n"
/* D: output w, h, layer w, h; Z: depth region w, h, linearize mul, add; N: NDC -> view;
 * E0..E2: CACAO effect constants; M.x: 1 = write 1.0 (no AO) */
"Texture2D<float4> T0 : register(t0);\n"
"Texture2D<float4> T1 : register(t1);\n"
"float4 vs(uint i : SV_VertexID) : SV_Position { float2 t = float2((i << 1) & 2, i & 2); return float4(t * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
"float3 vpos(float2 uv, float z) { return float3((N.xy * uv + N.zw) * z, z); }\n"
"float4 cacao_edges(float c, float l, float r, float t, float b)\n"
"{\n"
"    float4 e = float4(l, r, t, b) - c;\n"
"    float4 s = e + e.yxwz;\n"
"    e = min(abs(e), abs(s));\n"
"    return saturate(1.3 - e / (c * 0.040));\n"
"}\n"
"float pack_edges(float4 e) { e = round(saturate(e) * 3.05); return dot(e, float4(64.0 / 255.0, 16.0 / 255.0, 4.0 / 255.0, 1.0 / 255.0)); }\n"
"float4 unpack_edges(float v)\n"
"{\n"
"    uint p = uint(v * 255.5);\n"
"    float4 e = float4(float((p >> 6) & 3u), float((p >> 4) & 3u), float((p >> 2) & 3u), float(p & 3u)) / 3.0;\n"
"    return saturate(e + E2.y);\n"
"}\n"
/* atlas: layer l (0..1, 0..1) at l * L; layer pixel q covers the full-resolution pixel 2q + l */
"int2 LS() { return int2(D.zw); }\n"
"int2 lclamp(int2 q) { return clamp(q, int2(0, 0), LS() - 1); }\n"
"float3 nrm(float3 v) { return v * rsqrt(max(dot(v, v), 1e-20)); }\n"
/* ---- 1. prepare ---- */
"float vzd(int2 p)\n"
"{\n"
"    p = clamp(p, int2(0, 0), int2(D.xy) - 1);\n"
"    int2 t = min(int2((float2(p) + 0.5) * Z.xy / D.xy), int2(Z.xy) - 1);\n"
"    float d = T0.Load(int3(t, 0)).r;\n"
"    return Z.w > 0.0 ? Z.z / max(Z.w - d, 1e-7) : max(d * Z.z, 1e-4);\n"
"}\n"
"struct PrepOut { float z : SV_Target0; float4 n : SV_Target1; };\n"
"PrepOut ps_prepare(float4 sp : SV_Position)\n"
"{\n"
"    int2 a = int2(sp.xy);\n"
"    int2 L = LS();\n"
"    int2 l = int2(a.x >= L.x ? 1 : 0, a.y >= L.y ? 1 : 0);\n"
"    int2 p = min((a - l * L) * 2 + l, int2(D.xy) - 1);\n"
"    float zc = vzd(p), zl = vzd(p + int2(-1, 0)), zr = vzd(p + int2(1, 0)), zt = vzd(p + int2(0, -1)), zb = vzd(p + int2(0, 1));\n"
"    float2 px = 1.0 / D.xy;\n"
"    float2 uv = (float2(p) + 0.5) * px;\n"
"    float4 e = cacao_edges(zc, zl, zr, zt, zb);\n"
"    float3 C = vpos(uv, zc);\n"
"    float3 Lp = nrm(vpos(uv - float2(px.x, 0.0), zl) - C);\n"
"    float3 Rp = nrm(vpos(uv + float2(px.x, 0.0), zr) - C);\n"
"    float3 Tp = nrm(vpos(uv - float2(0.0, px.y), zt) - C);\n"
"    float3 Bp = nrm(vpos(uv + float2(0.0, px.y), zb) - C);\n"
"    float4 acc = float4(e.x * e.z, e.z * e.y, e.y * e.w, e.w * e.x);\n"
"    float3 n = float3(0.0, 0.0, -0.0005);\n"
"    n += acc.x * cross(Lp, Tp);\n"
"    n += acc.y * cross(Tp, Rp);\n"
"    n += acc.z * cross(Rp, Bp);\n"
"    n += acc.w * cross(Bp, Lp);\n"
"    n = nrm(n);\n"
"    PrepOut o;\n"
"    o.z = zc;\n"
"    o.n = float4(n * 0.5 + 0.5, 0.0);\n"
"    return o;\n"
"}\n"
/* ---- 2. generate (FFX_CACAO_GenerateSSAOShadowsInternal; Q, NUM_TAPS from the compile macros) ---- */
"static const float4 pat[32] = {\n"
"    float4( 0.78488064,  0.56661671, 1.500000, -0.126083), float4( 0.26022232, -0.29575172, 1.500000, -1.064030),\n"
"    float4( 0.10459357,  0.08372527, 1.110000, -2.730563), float4(-0.68286800,  0.04963045, 1.090000, -0.498827),\n"
"    float4(-0.13570161, -0.64190155, 1.250000, -0.532765), float4(-0.26193795, -0.08205118, 0.670000, -1.783245),\n"
"    float4(-0.61177456,  0.66664219, 0.710000, -0.044234), float4( 0.43675563,  0.25119025, 0.610000, -1.167283),\n"
"    float4( 0.07884444,  0.86618668, 0.640000, -0.459002), float4(-0.12790935, -0.29869005, 0.600000, -1.729424),\n"
"    float4(-0.04031125,  0.02413622, 0.600000, -4.792042), float4( 0.16201244, -0.52851415, 0.790000, -1.067055),\n"
"    float4(-0.70991218,  0.47301072, 0.640000, -0.335236), float4( 0.03277707, -0.22349690, 0.600000, -1.982384),\n"
"    float4( 0.68921727,  0.36800742, 0.630000, -0.266718), float4( 0.29251814,  0.37775412, 0.610000, -1.422520),\n"
"    float4(-0.12224089,  0.96582592, 0.600000, -0.426142), float4( 0.11071457, -0.16131058, 0.600000, -2.165947),\n"
"    float4( 0.46562141, -0.59747696, 0.600000, -0.189760), float4(-0.51548797,  0.11804193, 0.600000, -1.246800),\n"
"    float4( 0.89141309, -0.42090443, 0.600000,  0.028192), float4(-0.32402530, -0.01591529, 0.600000, -1.543018),\n"
"    float4( 0.60771245,  0.41635221, 0.600000, -0.605411), float4( 0.02379565, -0.08239821, 0.600000, -3.809046),\n"
"    float4( 0.48951152, -0.23657045, 0.600000, -1.189011), float4(-0.17611565, -0.81696892, 0.600000, -0.513724),\n"
"    float4(-0.33930185, -0.20732205, 0.600000, -1.698047), float4(-0.91974425,  0.05403209, 0.600000,  0.062246),\n"
"    float4(-0.15064627, -0.14949332, 0.600000, -1.896062), float4( 0.53180975, -0.35210401, 0.600000, -0.758838),\n"
"    float4( 0.41487166,  0.81442589, 0.600000, -0.505648), float4(-0.24106961, -0.32721516, 0.600000, -1.665244) };\n"
"static const int spmap[5] = { 0, 1, 4, 3, 2 };\n"
"static int2 g_l;\n"
"float zat(int2 q) { return T0.Load(int3(g_l * LS() + lclamp(q), 0)).r; }\n"
"float3 nat(int2 q) { return T1.Load(int3(g_l * LS() + lclamp(q), 0)).xyz * 2.0 - 1.0; }\n"
"float2 uvof(int2 q) { return (float2(lclamp(q) * 2 + g_l) + 0.5) / D.xy; }\n"
"float obscurance(float3 n, float3 d, float fall)\n"
"{\n"
"    float l2 = dot(d, d);\n"
"    float ndd = dot(n, d) * rsqrt(max(l2, 1e-20));\n"
"    float fm = max(0.0, l2 * fall + 1.0);\n"
"    return max(0.0, ndd - E1.z) * fm;\n"
"}\n"
"void tap(int2 q, float3 C, float3 n, float fall, float wmod, inout float os, inout float ws)\n"
"{\n"
"    float3 hd = vpos(uvof(q), zat(q)) - C;\n"
"    float o = obscurance(n, hd, fall);\n"
"    float w = 1.0;\n"
"#if Q >= 1\n"
"    float red = saturate(max(0.0, -hd.z) * E2.x + 2.0);\n"
"    w = 0.6 * red + 0.4;\n"
"#endif\n"
"    w *= wmod;\n"
"    os += o * w;\n"
"    ws += w;\n"
"}\n"
/* FFX_CACAO_UpdatePerPassConstants: rotation / scale per layer and sub-pass */
"float4 rotf(int layer, int sub)\n"
"{\n"
"    float b = (float)spmap[sub];\n"
"    float sc = 1.0 + ((float)layer - 1.5 + (b - 2.0) / 5.0) * 0.07;\n"
"    float s, c;\n"
"    sincos(((float)layer + b / 5.0) * 1.57079633, s, c);\n"
"    return float4(sc * c, sc * -s, -sc * s, -sc * c);\n"
"}\n"
"float2 ps_gen(float4 sp : SV_Position) : SV_Target\n"
"{\n"
"    int2 a = int2(sp.xy);\n"
"    int2 L = LS();\n"
"    g_l = int2(a.x >= L.x ? 1 : 0, a.y >= L.y ? 1 : 0);\n"
"    int2 q = a - g_l * L;\n"
"    int layer = g_l.x + g_l.y * 2;\n"
"    float pz = zat(q), lz = zat(q + int2(-1, 0)), rz = zat(q + int2(1, 0)), tz = zat(q + int2(0, -1)), bz = zat(q + int2(0, 1));\n"
"    float3 C = vpos(uvof(q), pz);\n"
"    float3 n = nat(q);\n"
"    float2 pdir = pz * N.xy / float2(L);\n"
"    float er = E0.x * (saturate(length(C) * E1.w) * 0.8 + 0.2);\n"
"    float lmod = (0.85 * er) / pdir.x;\n"
"    float fall = -1.0 / (er * er);\n"
"    float4 rs = rotf(layer, int(uint(q.y * 2 + q.x) % 5u)) * lmod;\n"
"    float os = 0.0, ws = 0.0;\n"
"    float4 e = float4(1.0, 1.0, 1.0, 1.0);\n"
"    C *= E2.w;\n"
"#if Q >= 1\n"
"    e = cacao_edges(pz, lz, rz, tz, bz);\n"
"    {\n"
"        float3 vd = float3(C.xy / C.zz, 1.0);\n"
"        float3 dl = float3(-pdir.x, 0.0, 0.0) + vd * (lz - C.z);\n"
"        float3 dr = float3(pdir.x, 0.0, 0.0) + vd * (rz - C.z);\n"
"        float3 dt = float3(0.0, -pdir.y, 0.0) + vd * (tz - C.z);\n"
"        float3 db = float3(0.0, pdir.y, 0.0) + vd * (bz - C.z);\n"
"        float mf = 4.0 * fall;\n"
"        float4 add = float4(obscurance(n, dl, mf), obscurance(n, dr, mf), obscurance(n, dt, mf), obscurance(n, db, mf));\n"
"        os += E2.z * dot(add, e);\n"
"    }\n"
"#endif\n"
"#if Q >= 2\n"
"    {\n"
"        float4 ne = float4(dot(n, nat(q + int2(-1, 0))), dot(n, nat(q + int2(1, 0))), dot(n, nat(q + int2(0, -1))), dot(n, nat(q + int2(0, 1))));\n"
"        e *= saturate(ne + 0.5);\n"
"    }\n"
"#endif\n"
"    [unroll] for (int i = 0; i < NUM_TAPS; ++i)\n"
"    {\n"
"        float4 pt = pat[i];\n"
"        float2 off = round(float2(rs.x * pt.x + rs.y * pt.y, rs.z * pt.x + rs.w * pt.y));\n"
"        tap(q + int2(off), C, n, fall, pt.z, os, ws);\n"
"        tap(q - int2(off), C, n, fall, pt.z, os, ws);\n"
"    }\n"
"    float ob = os / ws;\n"
"    float fade = saturate(C.z * E1.x + E1.y);\n"
"#if Q >= 1\n"
"    float ef = saturate((1.0 - e.x - e.y) * 0.35) + saturate((1.0 - e.z - e.w) * 0.35);\n"
"    fade *= saturate(1.0 - ef);\n"
"#endif\n"
"    ob = min(E0.y * ob, E0.w) * fade;\n"
"    float occ = pow(saturate(1.0 - ob), E0.z);\n"
"#if Q == 0\n"
"    e = float4(1.0, 1.0, 1.0, 1.0);\n"
"#endif\n"
"    return float2(occ, pack_edges(e));\n"
"}\n"
/* ---- 3. blur (FFX_CACAO_CalcBlurredSample), edges kept ---- */
"float2 ps_blur(float4 sp : SV_Position) : SV_Target\n"
"{\n"
"    int2 a = int2(sp.xy);\n"
"    int2 L = LS();\n"
"    int2 l = int2(a.x >= L.x ? 1 : 0, a.y >= L.y ? 1 : 0);\n"
"    int2 q = a - l * L, o = l * L;\n"
"    float2 c = T0.Load(int3(a, 0)).rg;\n"
"    float le = T0.Load(int3(o + lclamp(q + int2(-1, 0)), 0)).r;\n"
"    float ri = T0.Load(int3(o + lclamp(q + int2(1, 0)), 0)).r;\n"
"    float to = T0.Load(int3(o + lclamp(q + int2(0, -1)), 0)).r;\n"
"    float bo = T0.Load(int3(o + lclamp(q + int2(0, 1)), 0)).r;\n"
"    float4 e = unpack_edges(c.y);\n"
"    float s = c.x * 0.5 + le * e.x + ri * e.y + to * e.z + bo * e.w;\n"
"    float w = 0.5 + e.x + e.y + e.z + e.w;\n"
"    return float2(s / w, c.y);\n"
"}\n"
/* ---- 4. apply (FFX_CACAO_Apply): layers -> full resolution ---- */
"float bil(int2 l, float2 uv)\n"
"{\n"
"    float2 t = uv * float2(LS()) - 0.5;\n"
"    int2 i = int2(floor(t));\n"
"    float2 f = t - float2(i);\n"
"    int2 o = l * LS();\n"
"    float a = T0.Load(int3(o + lclamp(i), 0)).r;\n"
"    float b = T0.Load(int3(o + lclamp(i + int2(1, 0)), 0)).r;\n"
"    float c = T0.Load(int3(o + lclamp(i + int2(0, 1)), 0)).r;\n"
"    float d = T0.Load(int3(o + lclamp(i + int2(1, 1)), 0)).r;\n"
"    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);\n"
"}\n"
"float ps_apply(float4 sp : SV_Position) : SV_Target\n"
"{\n"
"    int2 p = int2(sp.xy);\n"
"    if (M.x > 0.5) return 1.0;\n"
"    int mx = p.x & 1, my = p.y & 1;\n"
"    float2 c = T0.Load(int3(int2(mx, my) * LS() + p / 2, 0)).rg;\n"
"    float4 e = unpack_edges(c.y);\n"
"    float fmx = float(mx), fmy = float(my);\n"
"    float fmxe = e.y - e.x, fmye = e.w - e.z;\n"
"    float2 inv = 0.5 / float2(LS());\n"
"    float2 pp = float2(p);\n"
"    float aoH = bil(int2(1 - mx, my), (pp + float2(fmx + fmxe - 0.5, 0.5 - fmy)) * inv);\n"
"    float aoV = bil(int2(mx, 1 - my), (pp + float2(0.5 - fmx, fmy - 0.5 + fmye)) * inv);\n"
"    float aoD = bil(int2(1 - mx, 1 - my), (pp + float2(fmx - 0.5 + fmxe, fmy - 0.5 + fmye)) * inv);\n"
"    float4 bw;\n"
"    bw.x = 1.0;\n"
"    bw.y = (e.x + e.y) * 0.5;\n"
"    bw.z = (e.z + e.w) * 0.5;\n"
"    bw.w = (bw.y + bw.z) * 0.5;\n"
"    return dot(float4(c.x, aoH, aoV, aoD), bw) / dot(bw, float4(1.0, 1.0, 1.0, 1.0));\n"
"}\n";

enum { CC_Z, CC_N, CC_A, CC_B, CC_IMGS };
static ID3D12RootSignature* s_cc_rs;
static ID3D12DescriptorHeap *s_cc_heap, *s_cc_rtv;   /* SRVs: 6 per upload half; RTVs: one per atlas */
static ID3D12PipelineState *s_cc_prep, *s_cc_gen, *s_cc_blur, *s_cc_apply;
static ID3D12Resource* s_cc_img[CC_IMGS]; static D3D12_RESOURCE_STATES s_cc_st[CC_IMGS];
static u32 s_cc_lw, s_cc_lh;
static int s_cc_fail;

static ID3D12PipelineState* cacao_pso(const char* entry, int q, const DXGI_FORMAT* fmts, UINT n)
{
    static const int taps[3] = { 3, 5, 12 };
    char qs[4], ts[4];
    snprintf(qs, sizeof qs, "%d", q);
    snprintf(ts, sizeof ts, "%d", taps[q]);
    const D3D_SHADER_MACRO mac[] = { { "Q", qs }, { "NUM_TAPS", ts }, { NULL, NULL } };
    ID3DBlob *vb = NULL, *pb = NULL, *e = NULL;
    if (FAILED(D3DCompile(s_cc_hlsl, sizeof s_cc_hlsl - 1, "rsx_cacao", mac, NULL, "vs", "vs_5_0", 0, 0, &vb, &e)) ||
        FAILED(D3DCompile(s_cc_hlsl, sizeof s_cc_hlsl - 1, "rsx_cacao", mac, NULL, entry, "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pb, &e))) {
        fprintf(stderr, "[CACAO] shader %s: %s%c", entry, e ? (const char*)e->lpVtbl->GetBufferPointer(e) : "?", 10);
        if (e) e->lpVtbl->Release(e);
        if (vb) vb->lpVtbl->Release(vb);
        return NULL;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; memset(&pd, 0, sizeof pd);
    pd.pRootSignature = s_cc_rs;
    pd.VS.pShaderBytecode = vb->lpVtbl->GetBufferPointer(vb); pd.VS.BytecodeLength = vb->lpVtbl->GetBufferSize(vb);
    pd.PS.pShaderBytecode = pb->lpVtbl->GetBufferPointer(pb); pd.PS.BytecodeLength = pb->lpVtbl->GetBufferSize(pb);
    for (UINT i = 0; i < n; i++) { pd.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL; pd.RTVFormats[i] = fmts[i]; }
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = n; pd.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = NULL;
    if (FAILED(s_d3d.device->lpVtbl->CreateGraphicsPipelineState(s_d3d.device, &pd, &IID_ID3D12PipelineState, (void**)&pso))) pso = NULL;
    vb->lpVtbl->Release(vb); pb->lpVtbl->Release(pb);
    if (!pso) fprintf(stderr, "[CACAO] pipeline %s failed%c", entry, 10);
    return pso;
}

static int cacao_setup(void)
{
    ID3D12Device* dv = s_d3d.device;
    if (s_cc_fail) return 0;
    if (s_cc_rs) return 1;
    D3D12_DESCRIPTOR_HEAP_DESC hd = {0}; hd.NumDescriptors = 6 * NPAR; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    D3D12_DESCRIPTOR_HEAP_DESC rh = {0}; rh.NumDescriptors = CC_IMGS; rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    D3D12_DESCRIPTOR_RANGE rg = {0}; rg.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rg.NumDescriptors = 2;
    D3D12_ROOT_PARAMETER rp[2]; memset(rp, 0, sizeof rp);
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 1; rp[0].DescriptorTable.pDescriptorRanges = &rg;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[1].Constants.ShaderRegister = 0; rp[1].Constants.Num32BitValues = 28;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rd = {0}; rd.NumParameters = 2; rd.pParameters = rp;
    ID3DBlob *sig = NULL, *err = NULL;
    ID3D12RootSignature* rs = NULL;
    if (FAILED(dv->lpVtbl->CreateDescriptorHeap(dv, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_cc_heap)) ||
        FAILED(dv->lpVtbl->CreateDescriptorHeap(dv, &rh, &IID_ID3D12DescriptorHeap, (void**)&s_cc_rtv)) ||
        FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
        FAILED(dv->lpVtbl->CreateRootSignature(dv, 0, sig->lpVtbl->GetBufferPointer(sig), sig->lpVtbl->GetBufferSize(sig),
                                               &IID_ID3D12RootSignature, (void**)&rs))) {
        fprintf(stderr, "[CACAO] setup failed: ambient occlusion stays off%c", 10);
        if (err) err->lpVtbl->Release(err);
        if (sig) sig->lpVtbl->Release(sig);
        s_cc_fail = 1; return 0;
    }
    sig->lpVtbl->Release(sig);
    s_cc_rs = rs;
    const DXGI_FORMAT fp[2] = { DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM };
    const DXGI_FORMAT fa = DXGI_FORMAT_R8G8_UNORM, fo = DXGI_FORMAT_R8_UNORM;
    if (!(s_cc_prep = cacao_pso("ps_prepare", 0, fp, 2)) || !(s_cc_gen = cacao_pso("ps_gen", s_cc_quality, &fa, 1)) ||
        !(s_cc_blur = cacao_pso("ps_blur", 0, &fa, 1)) || !(s_cc_apply = cacao_pso("ps_apply", 0, &fo, 1))) {
        s_cc_fail = 1; return 0;
    }
    return 1;
}

/* the four atlases, 2 lw x 2 lh each (recreated when the layer size changes) */
static int cacao_images(u32 lw, u32 lh)
{
    if (s_cc_img[0] && s_cc_lw == lw && s_cc_lh == lh) return 1;
    static const DXGI_FORMAT f[CC_IMGS] = { DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_UNORM };
    D3D12_CPU_DESCRIPTOR_HANDLE r; s_cc_rtv->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_cc_rtv, &r);
    const UINT inc = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (int i = 0; i < CC_IMGS; i++) {
        if (s_cc_img[i]) { grave_defer(s_cc_img[i]); s_cc_img[i] = NULL; }
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC dd = {0};
        dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        dd.Width = 2 * lw; dd.Height = 2 * lh; dd.DepthOrArraySize = 1; dd.MipLevels = 1;
        dd.Format = f[i]; dd.SampleDesc.Count = 1;
        dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &dd,
                D3D12_RESOURCE_STATE_RENDER_TARGET, NULL, &IID_ID3D12Resource, (void**)&s_cc_img[i]))) {
            s_cc_img[i] = NULL;
            for (int j = 0; j < i; j++) { grave_defer(s_cc_img[j]); s_cc_img[j] = NULL; }
            s_cc_lw = s_cc_lh = 0;
            return 0;
        }
        s_cc_st[i] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        D3D12_CPU_DESCRIPTOR_HANDLE ri = r; ri.ptr += (SIZE_T)i * inc;
        s_d3d.device->lpVtbl->CreateRenderTargetView(s_d3d.device, s_cc_img[i], NULL, ri);
    }
    s_cc_lw = lw; s_cc_lh = lh;
    fprintf(stderr, "[CACAO] buffers: 4 layers of %ux%u (%s)%c", lw, lh, s_cc_quality <= 0 ? "Low" : s_cc_quality == 1 ? "Medium" : "High", 10);
    return 1;
}

/* FFX_CACAO_UpdateConstants for a perspective camera (standard depth), generateNormals */
static void cacao_constants(float k[28], u32 w, u32 h, u32 dw, u32 dh)
{
    static float radius = -1.0f, strength = 1.0f;
    if (radius < 0.0f) {
        const char* e;
        radius = 1.2f;
        if ((e = getenv("RSX_CACAO_RADIUS")) && *e) { const float v = (float)atof(e); if (v >= 0.05f && v <= 20.0f) radius = v; }
        if ((e = getenv("RSX_CACAO_STRENGTH")) && *e) { const float v = (float)atof(e); if (v >= 0.0f && v <= 5.0f) strength = v; }
    }
    /* the view camera from the camera setter hook (rsx_fsr.inc); measured Fall of Man defaults until it is known */
    float n = 0.125f, f = 1000.0f, fov = 52.3f * 3.14159265f / 180.0f;
    { const float cn = g_rfom_cam_near, cf = g_rfom_cam_far, cv = g_rfom_cam_vfov;
      if (cn > 0.0f && cf > cn && cv > 0.1f && cv < 2.5f) { n = cn; f = cf; fov = cv; } }
    const float lw = (float)((w + 1) / 2), lh = (float)((h + 1) / 2);
    const float tan_y = tanf(fov * 0.5f), tan_x = tan_y * (16.0f / 9.0f);
    const float shadow_power = 1.5f, shadow_clamp = 0.98f, horizon = 0.06f, fade_from = 50.0f, fade_to = 300.0f, sharpness = 0.98f, detail = 0.5f;
    float near_limit = radius * 1.2f;
    if (s_cc_quality <= 0) near_limit *= 1.5f;
    near_limit /= tan_y;
    const float v[28] = {
        (float)w, (float)h, lw, lh,
        (float)dw, (float)dh, f * n / (f - n), f / (f - n),
        tan_x * 2.0f, tan_y * -2.0f, -tan_x, tan_y,
        radius, fminf(strength * 4.3f, 10.0f), shadow_power, shadow_clamp,
        -1.0f / (fade_to - fade_from), fade_from / (fade_to - fade_from) + 1.0f, horizon, 1.0f / near_limit,
        -1.0f / radius, 1.0f - sharpness, detail, 0.9992f,
        0.0f, 0.0f, 0.0f, 0.0f };
    memcpy(k, v, sizeof v);
}

/* resources for a w x h run; call before any barrier of the AO passes */
static int cacao_ready(u32 w, u32 h)
{
    return cacao_setup() && cacao_images((w + 1) / 2, (h + 1) / 2);
}

/* CACAO of the dw x dh top-left depth texels of dres (in a shader-readable state, SRV format zf) into the
 * top-left w x h of s_ao_tex (in RENDER_TARGET). Leaves the backend's heaps / root signature unset. */
static void cacao_run(ID3D12Resource* dres, DXGI_FORMAT zf, u32 w, u32 h, u32 dw, u32 dh)
{
    ID3D12Device* dv = s_d3d.device;
    ID3D12GraphicsCommandList* cl = s_d3d.cmd_list;
    const UINT inc = dv->lpVtbl->GetDescriptorHandleIncrementSize(dv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT rinc = dv->lpVtbl->GetDescriptorHandleIncrementSize(dv, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    /* per upload half: 0 depth, 1 Z, 2 N, 3 A, 4 B, 5 Z (t1 filler when B is t0) */
    const UINT base = (UINT)T_PARITY * 6u;
    D3D12_CPU_DESCRIPTOR_HANDLE c0; s_cc_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_cc_heap, &c0);
    c0.ptr += (SIZE_T)base * inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
    ID3D12Resource* const src[6] = { dres, s_cc_img[CC_Z], s_cc_img[CC_N], s_cc_img[CC_A], s_cc_img[CC_B], s_cc_img[CC_Z] };
    const DXGI_FORMAT sf[6] = { zf, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R32_FLOAT };
    for (int i = 0; i < 6; i++) {
        D3D12_CPU_DESCRIPTOR_HANDLE ci = c0; ci.ptr += (SIZE_T)i * inc;
        sv.Format = sf[i];
        dv->lpVtbl->CreateShaderResourceView(dv, src[i], &sv, ci);
    }
    D3D12_GPU_DESCRIPTOR_HANDLE g0; s_cc_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_cc_heap, &g0);
    g0.ptr += (UINT64)base * inc;
    D3D12_CPU_DESCRIPTOR_HANDLE r0; s_cc_rtv->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_cc_rtv, &r0);
    float k[28];
    cacao_constants(k, w, h, dw, dh);
    const u32 aw = 2 * s_cc_lw, ah = 2 * s_cc_lh;
    D3D12_VIEWPORT vpa = { 0, 0, (float)aw, (float)ah, 0.0f, 1.0f };
    D3D12_RECT sca = { 0, 0, (LONG)aw, (LONG)ah };
    ID3D12DescriptorHeap* hh[] = { s_cc_heap };
    cl->lpVtbl->SetDescriptorHeaps(cl, 1, hh);
    cl->lpVtbl->SetGraphicsRootSignature(cl, s_cc_rs);
    cl->lpVtbl->SetGraphicsRoot32BitConstants(cl, 1, 28, k, 0);
    cl->lpVtbl->IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->lpVtbl->RSSetViewports(cl, 1, &vpa); cl->lpVtbl->RSSetScissorRects(cl, 1, &sca);
#define CC_TABLE(i) do { D3D12_GPU_DESCRIPTOR_HANDLE g = g0; g.ptr += (UINT64)(i) * inc; cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, 0, g); } while (0)
#define CC_RTV(i) ((D3D12_CPU_DESCRIPTOR_HANDLE){ r0.ptr + (SIZE_T)(i) * rinc })
    /* 1. prepare: depth -> Z, N */
    ssao_barrier(s_cc_img[CC_Z], &s_cc_st[CC_Z], D3D12_RESOURCE_STATE_RENDER_TARGET);
    ssao_barrier(s_cc_img[CC_N], &s_cc_st[CC_N], D3D12_RESOURCE_STATE_RENDER_TARGET);
    { const D3D12_CPU_DESCRIPTOR_HANDLE r[2] = { CC_RTV(CC_Z), CC_RTV(CC_N) }; cl->lpVtbl->OMSetRenderTargets(cl, 2, r, FALSE, NULL); }
    CC_TABLE(0);
    cl->lpVtbl->SetPipelineState(cl, s_cc_prep);
    cl->lpVtbl->DrawInstanced(cl, 3, 1, 0, 0);
    /* 2. generate: Z, N -> A */
    ssao_barrier(s_cc_img[CC_Z], &s_cc_st[CC_Z], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ssao_barrier(s_cc_img[CC_N], &s_cc_st[CC_N], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ssao_barrier(s_cc_img[CC_A], &s_cc_st[CC_A], D3D12_RESOURCE_STATE_RENDER_TARGET);
    { const D3D12_CPU_DESCRIPTOR_HANDLE r = CC_RTV(CC_A); cl->lpVtbl->OMSetRenderTargets(cl, 1, &r, FALSE, NULL); }
    CC_TABLE(1);
    cl->lpVtbl->SetPipelineState(cl, s_cc_gen);
    cl->lpVtbl->DrawInstanced(cl, 3, 1, 0, 0);
    /* 3. blur: A <-> B */
    int cur = CC_A;
    cl->lpVtbl->SetPipelineState(cl, s_cc_blur);
    for (int i = 0, nb = s_cc_quality <= 0 ? 1 : 2; i < nb; i++) {
        const int dst = cur == CC_A ? CC_B : CC_A;
        ssao_barrier(s_cc_img[cur], &s_cc_st[cur], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ssao_barrier(s_cc_img[dst], &s_cc_st[dst], D3D12_RESOURCE_STATE_RENDER_TARGET);
        { const D3D12_CPU_DESCRIPTOR_HANDLE r = CC_RTV(dst); cl->lpVtbl->OMSetRenderTargets(cl, 1, &r, FALSE, NULL); }
        CC_TABLE(cur == CC_A ? 3 : 4);
        cl->lpVtbl->DrawInstanced(cl, 3, 1, 0, 0);
        cur = dst;
    }
    /* 4. apply: layers -> s_ao_tex (w x h) */
    ssao_barrier(s_cc_img[cur], &s_cc_st[cur], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    { D3D12_CPU_DESCRIPTOR_HANDLE r; s_ao_rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_ao_rtv_heap, &r);
      cl->lpVtbl->OMSetRenderTargets(cl, 1, &r, FALSE, NULL); }
    D3D12_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0.0f, 1.0f };
    D3D12_RECT sc = { 0, 0, (LONG)w, (LONG)h };
    cl->lpVtbl->RSSetViewports(cl, 1, &vp); cl->lpVtbl->RSSetScissorRects(cl, 1, &sc);
    CC_TABLE(cur == CC_A ? 3 : 4);
    cl->lpVtbl->SetPipelineState(cl, s_cc_apply);
    cl->lpVtbl->DrawInstanced(cl, 3, 1, 0, 0);
#undef CC_TABLE
#undef CC_RTV
}
