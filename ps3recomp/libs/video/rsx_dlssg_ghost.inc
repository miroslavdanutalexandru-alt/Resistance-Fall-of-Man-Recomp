/* Frame generation ghosting fixes and diagnostics (ported from the Resistance 2 port's R2_DLSS_FG_HUD_HOLD /
 * R2_DLSS_FG_FX_MV / R2_DLSS_FG_STATS / R2_DLSS_FG_MV_DEBUG). Included by rsx_dlssg.inc before fg_tag_scene.
 *
 *   RFOM_DLSS_FG_HUD_HOLD  1 (default): with the HUD split on, a frame whose HUD could not be separated (classifier
 *                     unsure, a HUD run closed by a scene draw, no HUD draws) keeps the HUD area of the last separated
 *                     frame (under 2 s old) as UI -- that area of the real frame is handed over as the UI layer, so
 *                     the HUD baked into the frame is not warped with the scene -- and, with DLSS-G, the motion
 *                     vectors under it are zeroed (FSR 3 gets the UI part only) | 0: off (the old behaviour).
 *   RFOM_DLSS_FG_HUD_DEBUG 1: the held area in yellow (shown frames and the UI layer; never the screenshot).
 *   RFOM_DLSS_FG_FX_MV  0 (default): motion of the pixels covered by this frame's blended effect draws (smoke, sparks,
 *                     muzzle flashes, glows: the reactive mask, RSX_FSR_REACTIVE > 0) as rendered | 1: camera motion
 *                     only | 2: none. Frame generation's copy only; the upscaler keeps the original. Experimental.
 *   RFOM_DLSS_FG_STATS  1 (or RFOM_FG_DEBUG >= 1): two "[fg-stats]" lines in run.log every 10 s (presents, FG on,
 *                     resets, scene tags per present, HUD split and why not, hold use, GPU-read motion statistics).
 *   RFOM_DLSS_FG_MV_DEBUG 1 (or RFOM_FG_DEBUG=2, which also turns the stats on): tint the shown frame with the
 *                     motion vectors frame generation gets: green = object motion (differs from the camera's by more
 *                     than 1 render pixel; stronger = more), blue = camera motion only, magenta = invalid (NaN,
 *                     behind the camera), untouched = still.
 *
 * DLSS-G: with any of these active the scene tag (rfom_dlssg_tag) moves from the upscaler's command list to the
 * present (fg_present), on private copies of the depth / motion vectors made right after the upscaler (the bridge
 * copies them there anyway: eOnlyValidNow), so the hold can zero motion under the HUD area once the present knows
 * the HUD was not separated. RFOM_DLSS_FG_HUD_HOLD=0 with the others off keeps the old immediate tag exactly. */

static void grave_defer(ID3D12Resource* r);   /* rsx_d3d12_backend.c */
static int f3_is_on(void);                     /* rsx_fsr3fg.inc */
static void wait_for_gpu(void);                /* rsx_d3d12_backend.c */
static int fg_ui_on(void);

#define FGX_RING 64u          /* descriptor sets of 8 in the shader-visible ring */
#define FGX_SLOTS 16u         /* stats slots of 64 bytes (16 words) */

static struct {
    int init, hold, hud_dbg, fx, stats, mv_dbg;
} s_fgxc;

static struct {
    ID3D12RootSignature *crs, *grs;
    ID3D12PipelineState *prep, *stat, *zero;
    ID3D12PipelineState *g_mask, *g_hold, *g_yel, *g_tint;
    DXGI_FORMAT g_fmt;
    ID3D12DescriptorHeap* heap; UINT inc; u32 ring;
    int bad, gbad;
    ID3D12Resource *mv, *z, *dbg, *mask;     /* private depth / motion / tint (input size), hold mask (guest size) */
    D3D12_RESOURCE_STATES mv_st, z_st, dbg_st, mask_st;
    u32 w, h, mw, mh;
    ID3D12Resource *sbuf, *szero, *srb;       /* stats: UAV buffer, zero upload, readback (mapped) */
    D3D12_RESOURCE_STATES sbuf_st;
    const u32* srb_map;
    u32 seq, slot_seq[FGX_SLOTS];
    /* deferred DLSS-G scene tag */
    int pend; unsigned rw, rh; float jx, jy; int reset;
    int dbg_ok; unsigned dbg_rw, dbg_rh;      /* this present's tint image */
    int tags, tag_reset;                       /* scene tags since the previous present */
    rfom_fg_hudhold_t hh;
} s_fgx;

static struct {
    u32 presents, on, resets, tags[3];
    u32 split, unsure, closed, no_draws, split_off, held, paused;
    unsigned long long gpu[11];
    u32 gpu_frames, gpu_lost, gpu_max, fx_frames;
} s_fgxs;

static int fgx_env(const char* n, int def)
{
    const char* e = getenv(n);
    return (e && *e) ? atoi(e) : def;
}

static void fgx_cfg(void)
{
    if (s_fgxc.init) return;
    s_fgxc.init = 1;
    const int dbg = fgx_env("RFOM_FG_DEBUG", 0);
    s_fgxc.hold = fgx_env("RFOM_DLSS_FG_HUD_HOLD", 1) != 0;
    s_fgxc.hud_dbg = fgx_env("RFOM_DLSS_FG_HUD_DEBUG", 0) != 0;
    s_fgxc.fx = fgx_env("RFOM_DLSS_FG_FX_MV", 0);
    if (s_fgxc.fx < 0 || s_fgxc.fx > 2) s_fgxc.fx = 0;
    s_fgxc.mv_dbg = fgx_env("RFOM_DLSS_FG_MV_DEBUG", 0) != 0 || dbg >= 2;
    s_fgxc.stats = fgx_env("RFOM_DLSS_FG_STATS", 0) != 0 || dbg >= 1;
    if (s_fg_ready || s_f3_ready)
        fprintf(stderr, "[FG] ghosting: HUD hold %s, effect motion %s, stats %s, motion-vector tint %s"
                        " (RFOM_DLSS_FG_HUD_HOLD / _FX_MV / _STATS / _MV_DEBUG)%c",
                s_fgxc.hold ? (s_fg_ready ? "on (UI + zeroed motion)" : "on (UI)") : "off",
                s_fgxc.fx == 1 ? "camera only" : s_fgxc.fx == 2 ? "none" : "as rendered",
                s_fgxc.stats ? "on" : "off", s_fgxc.mv_dbg ? "on" : "off", 10);
}

static const char s_fgx_cs_hlsl[] =
"Texture2D<float2> mvi : register(t0);\n"
"Texture2D<float> zi : register(t1);\n"
"Texture2D<float> rxi : register(t2);\n"
"RWTexture2D<float2> mvo : register(u0);\n"
"RWTexture2D<float> zo : register(u1);\n"
"RWTexture2D<float4> dbo : register(u2);\n"
"RWByteAddressBuffer sv : register(u3);\n"
/* m: the frame's camera (row-major, as the camera fill); j: jitter ndc xy, view size; f: rw, rh, flags (1 camera,
 * 2 effect mask, 4 tint out), effect mode; g: stats slot byte offset, stamp, hold mask size */
"cbuffer K : register(b0) { float4 m0, m1, m2, m3; float4 j; uint4 f; uint4 g; };\n"
"bool cam_mv(uint2 p, float d, out float2 cm) {\n"
"    float2 n = float2((p.x + 0.5) / j.z * 2.0 - 1.0, 1.0 - (p.y + 0.5) / j.w * 2.0) - j.xy;\n"
"    float4 c = float4(n, d, 1.0);\n"
"    float4 q = float4(dot(m0, c), dot(m1, c), dot(m2, c), dot(m3, c));\n"
"    cm = float2(0, 0);\n"
"    if (!(q.w > 1e-6)) return false;\n"
"    cm = (q.xy / q.w - n) * float2(0.5, -0.5);\n"
"    return all(abs(cm) < 2.0);\n"
"}\n"
"[numthreads(8, 8, 1)] void prep(uint3 id : SV_DispatchThreadID) {\n"
"    if (id.x >= f.x || id.y >= f.y) return;\n"
"    float2 mv = mvi[id.xy];\n"
"    float z = zi[id.xy];\n"
"    bool fx = (f.z & 2u) != 0u && rxi[id.xy] > 0.0;\n"
"    if (fx && f.w != 0u) {\n"
"        float2 cm = float2(0, 0);\n"
"        if (f.w == 1u && (f.z & 1u) != 0u && !cam_mv(id.xy, saturate(z), cm)) cm = float2(0, 0);\n"
"        mv = cm;\n"
"    }\n"
"    mvo[id.xy] = mv;\n"
"    zo[id.xy] = z;\n"
"    if ((f.z & 4u) != 0u) {\n"
"        float4 o = float4(0, 0, 0, 0);\n"
"        if (any(isnan(mv)) || any(isinf(mv))) o = float4(1, 0, 1, 0.5);\n"
"        else if (length(mv * j.zw) > 0.5) {\n"
"            o = float4(0.1, 0.3, 1.0, 0.35);\n"
"            float d = saturate(z);\n"
"            if (d < 0.9999999 && (f.z & 1u) != 0u) {\n"
"                float2 cm;\n"
"                if (!cam_mv(id.xy, d, cm)) o = float4(1, 0, 1, 0.5);\n"
"                else { float om = length((mv - cm) * j.zw); if (om > 1.0) o = float4(0, 1, 0, clamp(0.3 + om / 32.0, 0.3, 0.75)); }\n"
"            }\n"
"        }\n"
"        dbo[id.xy] = o;\n"
"    }\n"
"}\n"
/* 160 x 90 samples of what frame generation gets: words 1 samples, 2 moving > 0.5 px, 3 object motion (differs from
 * the camera's by > 1 px), 4 effect-covered, 5 effect-covered with object motion, 6 max px * 16, 7 sum px * 4,
 * 8 sky, 9 camera motion valid, 10 sum camera px * 4; word 0 = stamp */
"[numthreads(8, 8, 1)] void stat(uint3 id : SV_DispatchThreadID) {\n"
"    if (id.x >= 160u || id.y >= 90u) return;\n"
"    uint s = g.x;\n"
"    if (id.x == 0u && id.y == 0u) sv.Store(s, g.y);\n"
"    uint2 p = min(uint2((float2(id.xy) + 0.5) / float2(160.0, 90.0) * float2(f.xy)), f.xy - 1u);\n"
"    float d = saturate(zi[p]);\n"
"    float2 mv = mvi[p];\n"
"    float l = min(length(mv * j.zw), 4000.0);\n"
"    if (isnan(l)) l = 4000.0;\n"
"    uint o;\n"
"    sv.InterlockedAdd(s + 4u, 1u, o);\n"
"    if (l > 0.5) sv.InterlockedAdd(s + 8u, 1u, o);\n"
"    sv.InterlockedMax(s + 24u, uint(l * 16.0), o);\n"
"    sv.InterlockedAdd(s + 28u, uint(l * 4.0), o);\n"
"    bool fx = (f.z & 2u) != 0u && rxi[p] > 0.0;\n"
"    if (fx) sv.InterlockedAdd(s + 16u, 1u, o);\n"
"    if (d >= 0.9999999) { sv.InterlockedAdd(s + 32u, 1u, o); return; }\n"
"    if ((f.z & 1u) == 0u) return;\n"
"    float2 cm;\n"
"    if (!cam_mv(p, d, cm)) return;\n"
"    sv.InterlockedAdd(s + 36u, 1u, o);\n"
"    sv.InterlockedAdd(s + 40u, uint(min(length(cm * j.zw), 4000.0) * 4.0), o);\n"
"    if (length((mv - cm) * j.zw) > 1.0) { sv.InterlockedAdd(s + 12u, 1u, o); if (fx) sv.InterlockedAdd(s + 20u, 1u, o); }\n"
"}\n"
/* HUD hold: zero the motion under the held HUD area (t1 = hold mask, guest size g.zw; the render pixel's footprint
 * there plus one pixel) */
"[numthreads(8, 8, 1)] void zero(uint3 id : SV_DispatchThreadID) {\n"
"    if (id.x >= f.x || id.y >= f.y) return;\n"
"    float2 r = float2(g.zw) / float2(f.xy);\n"
"    int2 a = int2(floor(float2(id.xy) * r)) - 1, b = int2(ceil(float2(id.xy + 1u) * r));\n"
"    b = min(b, a + 6);\n"
"    int2 mx = int2(g.zw) - 1;\n"
"    for (int y = a.y; y <= b.y; y++) for (int x = a.x; x <= b.x; x++)\n"
"        if (zi[clamp(int2(x, y), int2(0, 0), mx)] > 0.5) { mvo[id.xy] = float2(0, 0); return; }\n"
"}\n";

static const char s_fgx_gs_hlsl[] =
"Texture2D t0 : register(t0); Texture2D t1 : register(t1);\n"
"SamplerState s0 : register(s0); SamplerState s1 : register(s1);\n"
"cbuffer K : register(b0) { float4 a; float4 b; };\n"
"struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
"V vs(uint id : SV_VertexID) { V o; float2 uv = float2((id << 1) & 2, id & 2);\n"
"  o.uv = uv; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
"float mask(V i) : SV_Target { return t0.Load(int3(i.p.xy, 0)).a > 0.0 ? 1.0 : 0.0; }\n"
"float4 hold(V i) : SV_Target { if (t0.SampleLevel(s1, i.uv, 0).r < 0.01) discard; return float4(t1.Load(int3(i.p.xy, 0)).rgb, 1.0); }\n"
"float4 yel(V i) : SV_Target { if (t0.SampleLevel(s1, i.uv, 0).r < 0.01) discard; return float4(1.0, 0.9, 0.0, 0.45); }\n"
"float4 tint(V i) : SV_Target { float4 c = t0.SampleLevel(s0, i.uv * a.xy, 0); if (c.a <= 0.0) discard; return c; }\n";

static HRESULT fgx_compile(const char* src, size_t n, const char* name, const char* entry, const char* target, ID3DBlob** out)
{
    ID3DBlob* err = NULL;
    HRESULT hr = D3DCompile(src, n, name, NULL, NULL, entry, target, D3DCOMPILE_IEEE_STRICTNESS, 0, out, &err);
    if (FAILED(hr)) fprintf(stderr, "[FG] ghosting shader %s: %s%c", entry, err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "?", 10);
    if (err) err->lpVtbl->Release(err);
    return hr;
}

static ID3D12RootSignature* fgx_rs(const D3D12_ROOT_SIGNATURE_DESC* rd)
{
    ID3DBlob *sig = NULL, *err = NULL; ID3D12RootSignature* rs = NULL;
    if (FAILED(D3D12SerializeRootSignature(rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) { if (err) err->lpVtbl->Release(err); return NULL; }
    if (err) err->lpVtbl->Release(err);
    if (FAILED(s_d3d.device->lpVtbl->CreateRootSignature(s_d3d.device, 0, sig->lpVtbl->GetBufferPointer(sig), sig->lpVtbl->GetBufferSize(sig),
                                                         &IID_ID3D12RootSignature, (void**)&rs))) rs = NULL;
    sig->lpVtbl->Release(sig);
    return rs;
}

/* compute side: root signature (28 constants + a table of 3 SRVs and 4 UAVs), the three pipelines, the ring */
static int fgx_init(void)
{
    if (s_fgx.bad) return 0;   /* any failure is final (partial objects are kept, never used) */
    if (s_fgx.heap) return 1;  /* created last: everything is ready */
    s_fgx.bad = 1;
    ID3D12Device* d = s_d3d.device;
    D3D12_DESCRIPTOR_RANGE rg[2]; memset(rg, 0, sizeof rg);
    rg[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rg[0].NumDescriptors = 3; rg[0].OffsetInDescriptorsFromTableStart = 0;
    rg[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; rg[1].NumDescriptors = 4; rg[1].OffsetInDescriptorsFromTableStart = 3;
    D3D12_ROOT_PARAMETER rp[2]; memset(rp, 0, sizeof rp);
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[0].Constants.Num32BitValues = 28;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[1].DescriptorTable.NumDescriptorRanges = 2;
    rp[1].DescriptorTable.pDescriptorRanges = rg; rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rd = {0}; rd.NumParameters = 2; rd.pParameters = rp;
    if (!(s_fgx.crs = fgx_rs(&rd))) { fg_log("ghosting fixes unavailable (root signature)"); return 0; }
    static const char* ent[3] = { "prep", "stat", "zero" };
    ID3D12PipelineState** pso[3] = { &s_fgx.prep, &s_fgx.stat, &s_fgx.zero };
    for (int i = 0; i < 3; i++) {
        ID3DBlob* cs = NULL;
        if (FAILED(fgx_compile(s_fgx_cs_hlsl, sizeof s_fgx_cs_hlsl - 1, "fg_ghost", ent[i], "cs_5_0", &cs)) || !cs) return 0;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd; memset(&pd, 0, sizeof pd);
        pd.pRootSignature = s_fgx.crs;
        pd.CS.pShaderBytecode = cs->lpVtbl->GetBufferPointer(cs); pd.CS.BytecodeLength = cs->lpVtbl->GetBufferSize(cs);
        const HRESULT hr = d->lpVtbl->CreateComputePipelineState(d, &pd, &IID_ID3D12PipelineState, (void**)pso[i]);
        cs->lpVtbl->Release(cs);
        if (FAILED(hr)) { *pso[i] = NULL; fg_log("ghosting fixes unavailable (pipeline creation failed)"); return 0; }
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd = {0}; hd.NumDescriptors = 8u * FGX_RING; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(d->lpVtbl->CreateDescriptorHeap(d, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_fgx.heap))) {
        s_fgx.heap = NULL; fg_log("ghosting fixes unavailable (descriptor heap)"); return 0;
    }
    s_fgx.inc = d->lpVtbl->GetDescriptorHandleIncrementSize(d, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    s_fgx.bad = 0;
    return 1;
}

/* graphics side (swap-chain format, recreated when it changes): 8 constants + a table of 2 SRVs; s0 point, s1 linear */
static int fgx_ginit(void)
{
    if (s_fgx.g_tint && s_fgx.g_fmt == s_sc_fmt) return 1;
    if (s_fgx.gbad || !fgx_init()) return 0;
    ID3D12Device* d = s_d3d.device;
    if (s_fgx.g_tint) {   /* swap-chain format changed (HDR): earlier frames may still use the old pipelines */
        ID3D12PipelineState** g[4] = { &s_fgx.g_mask, &s_fgx.g_hold, &s_fgx.g_yel, &s_fgx.g_tint };
        wait_for_gpu();
        for (int i = 0; i < 4; i++) if (*g[i]) { (*g[i])->lpVtbl->Release(*g[i]); *g[i] = NULL; }
    }
    s_fgx.gbad = 1;
    if (!s_fgx.grs) {
        D3D12_DESCRIPTOR_RANGE rg = {0}; rg.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rg.NumDescriptors = 2;
        D3D12_ROOT_PARAMETER rp[2]; memset(rp, 0, sizeof rp);
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[0].Constants.Num32BitValues = 8;
        rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[1].DescriptorTable.NumDescriptorRanges = 1;
        rp[1].DescriptorTable.pDescriptorRanges = &rg; rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sm[2]; memset(sm, 0, sizeof sm);
        for (int i = 0; i < 2; i++) {
            sm[i].Filter = i ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
            sm[i].AddressU = sm[i].AddressV = sm[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sm[i].MaxLOD = D3D12_FLOAT32_MAX; sm[i].ShaderRegister = (UINT)i; sm[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        }
        D3D12_ROOT_SIGNATURE_DESC rd = {0}; rd.NumParameters = 2; rd.pParameters = rp; rd.NumStaticSamplers = 2; rd.pStaticSamplers = sm;
        if (!(s_fgx.grs = fgx_rs(&rd))) { fg_log("ghosting debug / hold passes unavailable (root signature)"); return 0; }
    }
    ID3DBlob* vsb = NULL;
    if (FAILED(fgx_compile(s_fgx_gs_hlsl, sizeof s_fgx_gs_hlsl - 1, "fg_ghost_g", "vs", "vs_5_0", &vsb)) || !vsb) return 0;
    static const char* ent[4] = { "mask", "hold", "yel", "tint" };
    ID3D12PipelineState** pso[4] = { &s_fgx.g_mask, &s_fgx.g_hold, &s_fgx.g_yel, &s_fgx.g_tint };
    HRESULT hr = S_OK;
    for (int i = 0; i < 4 && SUCCEEDED(hr); i++) {
        ID3DBlob* psb = NULL;
        hr = fgx_compile(s_fgx_gs_hlsl, sizeof s_fgx_gs_hlsl - 1, "fg_ghost_g", ent[i], "ps_5_0", &psb);
        if (FAILED(hr) || !psb) { hr = E_FAIL; break; }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; memset(&pd, 0, sizeof pd);
        pd.pRootSignature = s_fgx.grs;
        pd.VS.pShaderBytecode = vsb->lpVtbl->GetBufferPointer(vsb); pd.VS.BytecodeLength = vsb->lpVtbl->GetBufferSize(vsb);
        pd.PS.pShaderBytecode = psb->lpVtbl->GetBufferPointer(psb); pd.PS.BytecodeLength = psb->lpVtbl->GetBufferSize(psb);
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        if (i >= 2) {   /* yellow / tint: over the image */
            D3D12_RENDER_TARGET_BLEND_DESC* b = &pd.BlendState.RenderTarget[0];
            b->BlendEnable = TRUE; b->BlendOp = b->BlendOpAlpha = D3D12_BLEND_OP_ADD;
            b->SrcBlend = D3D12_BLEND_SRC_ALPHA; b->DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            b->SrcBlendAlpha = D3D12_BLEND_ZERO; b->DestBlendAlpha = D3D12_BLEND_ONE;
        }
        pd.SampleMask = 0xFFFFFFFFu;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1; pd.RTVFormats[0] = i == 0 ? DXGI_FORMAT_R8_UNORM : s_sc_fmt; pd.SampleDesc.Count = 1;
        hr = d->lpVtbl->CreateGraphicsPipelineState(d, &pd, &IID_ID3D12PipelineState, (void**)pso[i]);
        if (FAILED(hr)) *pso[i] = NULL;
        psb->lpVtbl->Release(psb);
    }
    vsb->lpVtbl->Release(vsb);
    if (FAILED(hr)) { fg_log("ghosting debug / hold passes unavailable (pipeline creation failed)"); return 0; }
    s_fgx.g_fmt = s_sc_fmt;
    s_fgx.gbad = 0;
    return 1;
}

static ID3D12Resource* fgx_tex(u32 w, u32 h, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS fl, D3D12_RESOURCE_STATES st)
{
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d = {0};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = f; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; d.Flags = fl;
    ID3D12Resource* r = NULL;
    if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &d, st, NULL,
                                                              &IID_ID3D12Resource, (void**)&r))) return NULL;
    return r;
}

static ID3D12Resource* fgx_buf(D3D12_HEAP_TYPE t, u32 size, D3D12_RESOURCE_FLAGS fl, D3D12_RESOURCE_STATES st)
{
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = t;
    D3D12_RESOURCE_DESC d = {0};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags = fl;
    ID3D12Resource* r = NULL;
    if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &d, st, NULL,
                                                              &IID_ID3D12Resource, (void**)&r))) return NULL;
    return r;
}

/* next descriptor set of the ring: CPU / GPU handles of its first descriptor */
static void fgx_set(D3D12_CPU_DESCRIPTOR_HANDLE* c, D3D12_GPU_DESCRIPTOR_HANDLE* g)
{
    const u32 base = (s_fgx.ring++ % FGX_RING) * 8u;
    s_fgx.heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_fgx.heap, c);
    s_fgx.heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_fgx.heap, g);
    c->ptr += (SIZE_T)base * s_fgx.inc; g->ptr += (UINT64)base * s_fgx.inc;
}

static void fgx_srv(D3D12_CPU_DESCRIPTOR_HANDLE c, int i, ID3D12Resource* r, DXGI_FORMAT f)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC sv; memset(&sv, 0, sizeof sv);
    sv.Format = f; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
    c.ptr += (SIZE_T)i * s_fgx.inc;
    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, r, &sv, c);
}

static void fgx_uav(D3D12_CPU_DESCRIPTOR_HANDLE c, int i, ID3D12Resource* r, DXGI_FORMAT f)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv; memset(&uv, 0, sizeof uv);
    c.ptr += (SIZE_T)i * s_fgx.inc;
    if (i == 6) {   /* u3: the stats buffer (raw) */
        uv.Format = DXGI_FORMAT_R32_TYPELESS; uv.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uv.Buffer.NumElements = FGX_SLOTS * 16u; uv.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    } else { uv.Format = f; uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; }
    s_d3d.device->lpVtbl->CreateUnorderedAccessView(s_d3d.device, r, NULL, &uv, c);
}

/* a compute set: t0..t2 (NULL = null view), u0..u2, u3 = stats buffer or null */
static D3D12_GPU_DESCRIPTOR_HANDLE fgx_cset(ID3D12Resource* t0, DXGI_FORMAT f0, ID3D12Resource* t1, DXGI_FORMAT f1,
                                            ID3D12Resource* t2, ID3D12Resource* u0, ID3D12Resource* u1, ID3D12Resource* u2, int stats)
{
    D3D12_CPU_DESCRIPTOR_HANDLE c; D3D12_GPU_DESCRIPTOR_HANDLE g;
    fgx_set(&c, &g);
    fgx_srv(c, 0, t0, f0); fgx_srv(c, 1, t1, f1); fgx_srv(c, 2, t2, DXGI_FORMAT_R8_UNORM);
    fgx_uav(c, 3, u0, DXGI_FORMAT_R16G16_FLOAT); fgx_uav(c, 4, u1, DXGI_FORMAT_R32_FLOAT);
    fgx_uav(c, 5, u2, DXGI_FORMAT_R8G8B8A8_UNORM); fgx_uav(c, 6, stats ? s_fgx.sbuf : NULL, DXGI_FORMAT_UNKNOWN);
    return g;
}

/* a graphics pass over the bound render target and viewport: t0 / t1, 8 constants */
static void fgx_draw(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* pso, ID3D12Resource* t0, DXGI_FORMAT f0,
                     ID3D12Resource* t1, DXGI_FORMAT f1, const float* c8)
{
    if (!pso || !s_fgx.heap || !s_fgx.grs) return;
    D3D12_CPU_DESCRIPTOR_HANDLE c; D3D12_GPU_DESCRIPTOR_HANDLE g;
    fgx_set(&c, &g);
    fgx_srv(c, 0, t0, f0); fgx_srv(c, 1, t1, f1);
    static const float z8[8] = {0};
    cl->lpVtbl->SetGraphicsRootSignature(cl, s_fgx.grs);
    cl->lpVtbl->SetDescriptorHeaps(cl, 1, &s_fgx.heap);
    cl->lpVtbl->SetGraphicsRoot32BitConstants(cl, 0, 8, c8 ? c8 : z8, 0);
    cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, 1, g);
    cl->lpVtbl->IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->lpVtbl->SetPipelineState(cl, pso);
    cl->lpVtbl->DrawInstanced(cl, 3, 1, 0, 0);
}

/* back to fg_present's state after fgx_draw: the present pass root signature, its heap and constants */
static void fgx_restore(ID3D12GraphicsCommandList* cl, const float* k)
{
    cl->lpVtbl->SetGraphicsRootSignature(cl, s_blit_rs);
    cl->lpVtbl->SetDescriptorHeaps(cl, 1, &s_fg_srv_heap);
    cl->lpVtbl->SetGraphicsRoot32BitConstants(cl, 0, 8, k, 0);
}

/* DLSS-G: the scene tag is deferred to the present (on private copies) when something needs it */
static int fgx_defer(void)
{
    fgx_cfg();
    return s_fgxc.fx || s_fgxc.stats || s_fgxc.mv_dbg || (s_fgxc.hold && s_fgh.gate);
}

/* reads back a finished stats slot (its stamp = seq) and frees it */
static void fgx_harvest(u32 slot)
{
    const u32 seq = s_fgx.slot_seq[slot];
    if (!seq || !s_fgx.srb_map) return;
    s_fgx.slot_seq[slot] = 0;
    u32 w[16]; memcpy(w, s_fgx.srb_map + slot * 16u, sizeof w);
    if (w[0] != seq) { s_fgxs.gpu_lost++; return; }
    s_fgxs.gpu_frames++;
    for (int i = 1; i <= 10; i++) if (i != 6) s_fgxs.gpu[i] += w[i];
    if (w[6] > s_fgxs.gpu_max) s_fgxs.gpu_max = w[6];
}

static int fgx_stats_res(void)
{
    if (s_fgx.srb_map) return 1;
    if (!s_fgx.sbuf) {
        s_fgx.sbuf = fgx_buf(D3D12_HEAP_TYPE_DEFAULT, FGX_SLOTS * 64u, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        s_fgx.sbuf_st = D3D12_RESOURCE_STATE_COMMON;
    }
    if (!s_fgx.szero) {
        s_fgx.szero = fgx_buf(D3D12_HEAP_TYPE_UPLOAD, 64u, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* p = NULL; D3D12_RANGE rr = {0, 0};
        if (s_fgx.szero && SUCCEEDED(s_fgx.szero->lpVtbl->Map(s_fgx.szero, 0, &rr, &p)) && p) {
            memset(p, 0, 64); s_fgx.szero->lpVtbl->Unmap(s_fgx.szero, 0, NULL);
        } else if (s_fgx.szero) { s_fgx.szero->lpVtbl->Release(s_fgx.szero); s_fgx.szero = NULL; }
    }
    if (!s_fgx.srb) s_fgx.srb = fgx_buf(D3D12_HEAP_TYPE_READBACK, FGX_SLOTS * 64u, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!s_fgx.sbuf || !s_fgx.szero || !s_fgx.srb) return 0;
    void* p = NULL;
    if (FAILED(s_fgx.srb->lpVtbl->Map(s_fgx.srb, 0, NULL, &p)) || !p) return 0;
    s_fgx.srb_map = (const u32*)p;
    return 1;
}

/* After the upscaler, on its command list (z / mv / rx in rd = NON_PIXEL | PIXEL_SHADER_RESOURCE, rx may be NULL):
 * frame generation's private copies (effect motion applied), the tint image and the stats. M: the frame's camera
 * (NULL: none); jv: jitter ndc xy, view size. Returns 1 with s_fgx.z / mv valid (in rd). */
static int fgx_prep(ID3D12GraphicsCommandList* cl, ID3D12Resource* z, ID3D12Resource* mv, ID3D12Resource* rx,
                    unsigned rw, unsigned rh, const double* M, const float* jv)
{
    if (!fgx_init() || !z || !mv || !rw || !rh) return 0;
    const D3D12_RESOURCE_STATES rd = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_DESC dd; mv->lpVtbl->GetDesc(mv, &dd);
    const u32 w = (u32)dd.Width, h = dd.Height;
    if (rw > w || rh > h) return 0;
    if (!s_fgx.mv || s_fgx.w != w || s_fgx.h != h) {
        if (s_fgx.mv) { grave_defer(s_fgx.mv); s_fgx.mv = NULL; }
        if (s_fgx.z) { grave_defer(s_fgx.z); s_fgx.z = NULL; }
        if (s_fgx.dbg) { grave_defer(s_fgx.dbg); s_fgx.dbg = NULL; }
        const D3D12_RESOURCE_FLAGS ua = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        s_fgx.mv = fgx_tex(w, h, DXGI_FORMAT_R16G16_FLOAT, ua, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        s_fgx.z = fgx_tex(w, h, DXGI_FORMAT_R32_FLOAT, ua, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (s_fgxc.mv_dbg) s_fgx.dbg = fgx_tex(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, ua, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        s_fgx.mv_st = s_fgx.z_st = s_fgx.dbg_st = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        s_fgx.w = w; s_fgx.h = h;
        if (!s_fgx.mv || !s_fgx.z) {
            if (s_fgx.mv) { grave_defer(s_fgx.mv); s_fgx.mv = NULL; }
            if (s_fgx.z) { grave_defer(s_fgx.z); s_fgx.z = NULL; }
            if (s_fgx.dbg) { grave_defer(s_fgx.dbg); s_fgx.dbg = NULL; }
            static int n = 0; if (!n++) fg_log("ghosting fixes: out of video memory for the motion copies -- skipped");
            return 0;
        }
    }
    fg_tr(cl, s_fgx.mv, &s_fgx.mv_st, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    fg_tr(cl, s_fgx.z, &s_fgx.z_st, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const int dbg = s_fgxc.mv_dbg && s_fgx.dbg;
    if (dbg) fg_tr(cl, s_fgx.dbg, &s_fgx.dbg_st, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    u32 k[28]; memset(k, 0, sizeof k);
    if (M) for (int i = 0; i < 16; i++) { const float f = (float)M[i]; memcpy(&k[i], &f, 4); }
    memcpy(&k[16], jv, 16);
    k[20] = rw; k[21] = rh; k[22] = (M ? 1u : 0u) | (rx ? 2u : 0u) | (dbg ? 4u : 0u); k[23] = (u32)s_fgxc.fx;
    cl->lpVtbl->SetDescriptorHeaps(cl, 1, &s_fgx.heap);
    cl->lpVtbl->SetComputeRootSignature(cl, s_fgx.crs);
    cl->lpVtbl->SetComputeRoot32BitConstants(cl, 0, 28, k, 0);
    cl->lpVtbl->SetComputeRootDescriptorTable(cl, 1, fgx_cset(mv, DXGI_FORMAT_R16G16_FLOAT, z, DXGI_FORMAT_R32_FLOAT, rx,
                                                              s_fgx.mv, s_fgx.z, dbg ? s_fgx.dbg : NULL, 0));
    cl->lpVtbl->SetPipelineState(cl, s_fgx.prep);
    cl->lpVtbl->Dispatch(cl, (rw + 7u) / 8u, (rh + 7u) / 8u, 1u);
    fg_tr(cl, s_fgx.mv, &s_fgx.mv_st, rd);
    fg_tr(cl, s_fgx.z, &s_fgx.z_st, rd);
    s_fgx.dbg_ok = dbg; s_fgx.dbg_rw = rw; s_fgx.dbg_rh = rh;
    if (s_fgxc.stats && fgx_stats_res()) {   /* RFOM_DLSS_FG_STATS: what frame generation gets, into the next slot */
        s_fgx.seq = s_fgx.seq + 1u ? s_fgx.seq + 1u : 1u;
        const u32 seq = s_fgx.seq, slot = seq % FGX_SLOTS;
        fgx_harvest(slot);
        s_fgx.slot_seq[slot] = seq;
        if (rx) s_fgxs.fx_frames++;
        fg_tr(cl, s_fgx.sbuf, &s_fgx.sbuf_st, D3D12_RESOURCE_STATE_COPY_DEST);
        cl->lpVtbl->CopyBufferRegion(cl, s_fgx.sbuf, (UINT64)slot * 64u, s_fgx.szero, 0, 64u);
        fg_tr(cl, s_fgx.sbuf, &s_fgx.sbuf_st, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        k[24] = slot * 64u; k[25] = seq;
        cl->lpVtbl->SetComputeRoot32BitConstants(cl, 0, 28, k, 0);
        cl->lpVtbl->SetComputeRootDescriptorTable(cl, 1, fgx_cset(s_fgx.mv, DXGI_FORMAT_R16G16_FLOAT, s_fgx.z, DXGI_FORMAT_R32_FLOAT, rx,
                                                                  NULL, NULL, NULL, 1));
        cl->lpVtbl->SetPipelineState(cl, s_fgx.stat);
        cl->lpVtbl->Dispatch(cl, 20u, 12u, 1u);
        fg_tr(cl, s_fgx.sbuf, &s_fgx.sbuf_st, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cl->lpVtbl->CopyBufferRegion(cl, s_fgx.srb, (UINT64)slot * 64u, s_fgx.sbuf, (UINT64)slot * 64u, 64u);
    }
    return 1;
}

/* every scene tag (DLSS-G / FSR 3), for the stats */
static void fgx_note_tag(int reset)
{
    s_fgx.tags++;
    if (reset) s_fgx.tag_reset = 1;
}

/* fg_present, the HUD split's state for this present: 1 = the held HUD area is used (s_fgx.mask valid) */
static int fgx_hold_step(int split, int valid)
{
    fgx_cfg();
    const int on = s_fgxc.hold && (s_fg_ready || s_f3_ready) && fg_ui_on() && fgx_ginit();
    const u32 gw = SC(s_d3d.width), gh = SC(s_d3d.height);
    if (s_fgx.mask && (s_fgx.mw != gw || s_fgx.mh != gh)) {   /* guest size changed: the held area is stale */
        grave_defer(s_fgx.mask); s_fgx.mask = NULL;
        rfom_fg_hudhold_step(&s_fgx.hh, 0, 0, 0, 0, 2000);
    }
    if (on && !s_fgx.mask) {
        s_fgx.mw = gw; s_fgx.mh = gh;
        s_fgx.mask = fgx_tex(gw, gh, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
        s_fgx.mask_st = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (s_fgx.mask) { s_d3d.device->lpVtbl->CreateRenderTargetView(s_d3d.device, s_fgx.mask, NULL, fg_rtv(3)); }
    }
    if (!s_fgx.mask) { rfom_fg_hudhold_step(&s_fgx.hh, 0, 0, 0, 0, 2000); return 0; }
    return rfom_fg_hudhold_step(&s_fgx.hh, on, split, valid, GetTickCount64(), 2000);
}

/* fg_present, split block, valid frame (s_fg_ui in PIXEL_SHADER_RESOURCE): record its HUD area */
static void fgx_hold_record(ID3D12GraphicsCommandList* cl)
{
    if (!s_fgx.mask || !s_fgx.hh.has) return;
    fg_tr(cl, s_fgx.mask, &s_fgx.mask_st, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const D3D12_CPU_DESCRIPTOR_HANDLE mr = fg_rtv(3);
    const u32 gw = SC(s_d3d.width), gh = SC(s_d3d.height);
    const D3D12_VIEWPORT gv = {0, 0, (float)gw, (float)gh, 0, 1};
    const D3D12_RECT gs = {0, 0, (LONG)gw, (LONG)gh};
    cl->lpVtbl->OMSetRenderTargets(cl, 1, &mr, FALSE, NULL);
    cl->lpVtbl->RSSetViewports(cl, 1, &gv);
    cl->lpVtbl->RSSetScissorRects(cl, 1, &gs);
    fgx_draw(cl, s_fgx.g_mask, s_fg_ui, DXGI_FORMAT_R8G8B8A8_UNORM, NULL, DXGI_FORMAT_R8G8B8A8_UNORM, NULL);
    fg_tr(cl, s_fgx.mask, &s_fgx.mask_st, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

/* fg_present, split block, frame without a separated HUD, hold in use: the held area of the real frame (hsc) into
 * the UI layer (usc, bound as the render target with fg_present's viewport); hsc goes to PIXEL_SHADER_RESOURCE */
static void fgx_hold_draw(ID3D12GraphicsCommandList* cl)
{
    fg_tr(cl, s_fg_hsc, &s_fg_hsc_st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    fg_tr(cl, s_fgx.mask, &s_fgx.mask_st, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    fgx_draw(cl, s_fgx.g_hold, s_fgx.mask, DXGI_FORMAT_R8_UNORM, s_fg_hsc, s_sc_fmt, NULL);
    if (s_fgxc.hud_dbg) fgx_draw(cl, s_fgx.g_yel, s_fgx.mask, DXGI_FORMAT_R8_UNORM, NULL, DXGI_FORMAT_R8_UNORM, NULL);
}

/* the tint image over the bound render target (fg_present's viewport) */
static void fgx_tint_draw(ID3D12GraphicsCommandList* cl)
{
    fg_tr(cl, s_fgx.dbg, &s_fgx.dbg_st, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    const float c8[8] = { (float)s_fgx.dbg_rw / (float)s_fgx.w, (float)s_fgx.dbg_rh / (float)s_fgx.h, 0, 0, 0, 0, 0, 0 };
    fgx_draw(cl, s_fgx.g_tint, s_fgx.dbg, DXGI_FORMAT_R8G8B8A8_UNORM, NULL, DXGI_FORMAT_R8G8B8A8_UNORM, c8);
}

static int fgx_tint_ok(void) { return s_fgxc.mv_dbg && s_fgx.dbg_ok && s_fgx.dbg && fgx_ginit(); }

/* fg_present, after the split block: debug overlays on the shown frame (bound: nothing assumed) */
static void fgx_overlays(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE sc_rtv, const D3D12_VIEWPORT* vp,
                         int shot, int hold_use)
{
    const int yel = hold_use && s_fgxc.hud_dbg && s_fgx.mask;
    const int tint = fgx_tint_ok();
    if (shot == 1 || !(yel || tint)) return;
    const D3D12_RECT sc = {0, 0, (LONG)s_sc_w, (LONG)s_sc_h};
    cl->lpVtbl->OMSetRenderTargets(cl, 1, &sc_rtv, FALSE, NULL);
    cl->lpVtbl->RSSetViewports(cl, 1, vp);
    cl->lpVtbl->RSSetScissorRects(cl, 1, &sc);
    if (tint) fgx_tint_draw(cl);
    if (yel) {
        fg_tr(cl, s_fgx.mask, &s_fgx.mask_st, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        fgx_draw(cl, s_fgx.g_yel, s_fgx.mask, DXGI_FORMAT_R8_UNORM, NULL, DXGI_FORMAT_R8_UNORM, NULL);
    }
}

/* fg_present, before the present tags: the deferred DLSS-G scene tag (hold: motion zeroed under the held area) */
static void fgx_tag_present(ID3D12GraphicsCommandList* cl, int hold_use)
{
    if (!s_fgx.pend) return;
    s_fgx.pend = 0;
    if (!s_fgx.mv || !s_fgx.z) return;
    const D3D12_RESOURCE_STATES rd = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (hold_use && s_fgx.mask && s_fgx.zero && s_fgx.heap) {
        fg_tr(cl, s_fgx.mask, &s_fgx.mask_st, rd);
        fg_tr(cl, s_fgx.mv, &s_fgx.mv_st, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        u32 k[28]; memset(k, 0, sizeof k);
        k[20] = s_fgx.rw; k[21] = s_fgx.rh; k[26] = SC(s_d3d.width); k[27] = SC(s_d3d.height);
        cl->lpVtbl->SetDescriptorHeaps(cl, 1, &s_fgx.heap);
        cl->lpVtbl->SetComputeRootSignature(cl, s_fgx.crs);
        cl->lpVtbl->SetComputeRoot32BitConstants(cl, 0, 28, k, 0);
        cl->lpVtbl->SetComputeRootDescriptorTable(cl, 1, fgx_cset(NULL, DXGI_FORMAT_R16G16_FLOAT, s_fgx.mask, DXGI_FORMAT_R8_UNORM, NULL,
                                                                  s_fgx.mv, NULL, NULL, 0));
        cl->lpVtbl->SetPipelineState(cl, s_fgx.zero);
        cl->lpVtbl->Dispatch(cl, (s_fgx.rw + 7u) / 8u, (s_fgx.rh + 7u) / 8u, 1u);
        fg_tr(cl, s_fgx.mv, &s_fgx.mv_st, rd);
    }
    fg_tr(cl, s_fgx.mv, &s_fgx.mv_st, rd);
    fg_tr(cl, s_fgx.z, &s_fgx.z_st, rd);
    p_fg_tag(cl, s_fgx.z, (unsigned)rd, s_fgx.mv, (unsigned)rd, s_fgx.rw, s_fgx.rh, s_fgx.jx, s_fgx.jy, s_fgx.reset);
}

/* fg_present, end: the stats of this present; one pair of [fg-stats] lines per 10 s */
static void fgx_stats_present(int split, int valid, int hold_use, int paused)
{
    const int tags = s_fgx.tags, treset = s_fgx.tag_reset;
    s_fgx.tags = 0; s_fgx.tag_reset = 0; s_fgx.dbg_ok = 0;
    if (!s_fgxc.stats || !(s_fg_ready || s_f3_ready)) return;
    static unsigned long long t0 = 0;
    const unsigned long long t = GetTickCount64();
    if (!t0) t0 = t;
    const int on = s_fg_ready ? s_fg_on != 0 : f3_is_on();
    s_fgxs.presents++;
    if (on) s_fgxs.on++;
    if (on && treset) s_fgxs.resets++;
    s_fgxs.tags[tags > 2 ? 2 : tags]++;
    if (!split) s_fgxs.split_off++;
    else if (valid) s_fgxs.split++;
    else if (s_fgh.unsure) s_fgxs.unsure++;
    else if (s_fgh.runs > 0 && !s_fgh.open) s_fgxs.closed++;
    else s_fgxs.no_draws++;
    if (hold_use) s_fgxs.held++;
    if (paused) s_fgxs.paused++;
    if (t - t0 < 10000) return;
    t0 = t;
    const char* p = s_fg_ready ? "[DLSS-G]" : "[FSR3-FG]";
    const double n = s_fgxs.gpu[1] ? (double)s_fgxs.gpu[1] : 1.0, nc = s_fgxs.gpu[9] ? (double)s_fgxs.gpu[9] : 1.0;
    fprintf(stderr, "%s [fg-stats] 10 s: presents %u, FG on %u, FG resets %u | scene tags per present 0:%u 1:%u 2+:%u | HUD split %u, "
                    "not split: unsure %u, run closed %u, no HUD draws %u, split off %u; HUD hold used %u, paused %u%c",
            p, s_fgxs.presents, s_fgxs.on, s_fgxs.resets, s_fgxs.tags[0], s_fgxs.tags[1], s_fgxs.tags[2], s_fgxs.split,
            s_fgxs.unsure, s_fgxs.closed, s_fgxs.no_draws, s_fgxs.split_off, s_fgxs.held, s_fgxs.paused, 10);
    fprintf(stderr, "%s [fg-stats] motion vectors (%u frames, %u not read back, %u with effects): moving > 0.5 px %.1f%%, object motion %.1f%%, "
                    "effect-covered %.1f%% (with object motion %.1f%%), sky %.1f%% | mean |mv| %.2f px, camera-only %.2f px, max %.1f px (render pixels)%c",
            p, s_fgxs.gpu_frames, s_fgxs.gpu_lost, s_fgxs.fx_frames, 100.0 * (double)s_fgxs.gpu[2] / n, 100.0 * (double)s_fgxs.gpu[3] / nc,
            100.0 * (double)s_fgxs.gpu[4] / n, 100.0 * (double)s_fgxs.gpu[5] / nc, 100.0 * (double)s_fgxs.gpu[8] / n,
            (double)s_fgxs.gpu[7] / 4.0 / n, (double)s_fgxs.gpu[10] / 4.0 / nc, (double)s_fgxs.gpu_max / 16.0, 10);
    memset(&s_fgxs, 0, sizeof s_fgxs);
}
