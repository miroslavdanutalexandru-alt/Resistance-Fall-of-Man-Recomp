/* rsx_ssao.inc -- screen-space ambient occlusion for Fall of Man (included by
 * rsx_d3d12_backend.c after rsx_fsr.inc). The game has no AO of its own; this
 * is the same effect, option and look as the Resistance 2 / 3 ports
 * (RSX_SSAO, launcher Graphics > Quality > Ambient occlusion):
 *   RSX_SSAO=1               on (opt-in, experimental)
 *   RSX_SSAO_STRENGTH=0..1   darkening at full occlusion (0.6)
 *   RSX_SSAO_RADIUS=0.1..8   sample radius, relative (1.0)
 *
 * The scene surface is the display-sized render target with the most draws in
 * the previous frame (with an upscaler on, the one FSR / DLSS / XeSS takes).
 * Its depth is the zeta (or per-target) buffer most of those draws used. When
 * the first draw of the frame samples the finished scene -- the bloom / post
 * chain, the same point the upscaler runs -- two passes run first:
 *   1. depth -> AO (R8): pairs of opposite samples on a golden-angle spiral,
 *      rotated on a 4x4 Bayer grid (identical to R2's r2gx_ao_pass);
 *   2. depth-aware 4x4 blur of the AO, multiplied into the scene colour
 *      (alpha kept: the game keeps a mask there).
 * With an upscaler the scene is drawn into the top-left render fraction of the
 * surface, so both passes cover that region only (before upscaling).
 *
 * RFOM_AO_MODE (RSX_AO_MODE; launcher Graphics > Quality > Ambient occlusion):
 *   game / off   no AO (Fall of Man has none of its own; the default)
 *   port         the passes above (= RSX_SSAO=1, which is still honoured)
 *   cacao        FidelityFX CACAO (rsx_cacao.inc) at the same point: its four passes
 *                replace pass 1, and its result is multiplied in without the blur
 * RFOM_AO_QUALITY=low|medium|high: CACAO quality preset (default medium). */

static int s_ao_on = -1;
static float s_ao_strength = 0.6f, s_ao_radius = 1.0f;
static u32 s_ao_cnt[MAX_OFF_RTS];          /* non-clear draws per display-sized target, this frame */
static int s_ao_slot = -1;                 /* scene surface (picked at the previous frame's end) */
static int s_ao_done = 0;                  /* applied this frame */
static u16 s_ao_zcnt[MAX_ZETA];            /* scene draws per zeta buffer, this frame */
static u32 s_ao_rtz_cnt = 0;               /* scene draws on the scene target's own depth */
static u32 s_ao_runs = 0, s_ao_skips[4];   /* skips: no depth, size mismatch, setup failed, depth format */
static ID3D12DescriptorHeap *s_ao_heap, *s_ao_rtv_heap;
static ID3D12RootSignature* s_ao_rs;
static ID3D12PipelineState* s_ao_pso_ao;
static ID3D12PipelineState* s_ao_pso_apply; static u32 s_ao_apply_fmt;
static ID3D12Resource* s_ao_tex; static u32 s_ao_tw, s_ao_th; static D3D12_RESOURCE_STATES s_ao_tex_st;
static int s_ao_fail = 0;
enum { AO_MODE_GAME, AO_MODE_PORT, AO_MODE_CACAO, AO_MODE_OFF };
static int s_ao_mode = AO_MODE_GAME;
static ID3D12PipelineState* s_ao_pso_apply1; static u32 s_ao_apply1_fmt;   /* CACAO: multiply without the blur */
static int s_cc_quality;
static int ao_env_eq(const char* a, const char* b) { for (; *a && *b; a++, b++) if ((*a | 0x20) != (*b | 0x20)) return 0; return !*a && !*b; }

static int ssao_on(void)
{
    if (s_ao_on < 0) {
        const char* e = getenv("RSX_SSAO");
        s_ao_mode = port_native_parse_bool(e, 0) ? AO_MODE_PORT : AO_MODE_GAME;
        if (!((e = getenv("RFOM_AO_MODE")) && *e)) e = getenv("RSX_AO_MODE");
        if (e && *e) {
            if (ao_env_eq(e, "game") || ao_env_eq(e, "0")) s_ao_mode = AO_MODE_GAME;
            else if (ao_env_eq(e, "port") || ao_env_eq(e, "ssao") || ao_env_eq(e, "1")) s_ao_mode = AO_MODE_PORT;
            else if (ao_env_eq(e, "cacao") || ao_env_eq(e, "2")) s_ao_mode = AO_MODE_CACAO;
            else if (ao_env_eq(e, "off") || ao_env_eq(e, "none") || ao_env_eq(e, "3")) s_ao_mode = AO_MODE_OFF;
        }
        s_cc_quality = 1;
        if (!((e = getenv("RFOM_AO_QUALITY")) && *e)) e = getenv("RSX_AO_QUALITY");
        if (e && *e) { const int c = *e | 0x20; s_cc_quality = (c == 'l' || c == '0') ? 0 : (c == 'h' || c == '2') ? 2 : 1; }
        s_ao_on = s_ao_mode == AO_MODE_PORT || s_ao_mode == AO_MODE_CACAO;
        if (s_ao_mode == AO_MODE_CACAO)
            fprintf(stderr, "[SSAO] ambient occlusion: FidelityFX CACAO, quality %s (RFOM_AO_MODE)%c",
                    s_cc_quality <= 0 ? "Low" : s_cc_quality == 1 ? "Medium" : "High", 10);
        if ((e = getenv("RSX_SSAO_STRENGTH")) && *e) { const float v = (float)atof(e); if (v >= 0.0f && v <= 1.0f) s_ao_strength = v; }
        if ((e = getenv("RSX_SSAO_RADIUS")) && *e) { const float v = (float)atof(e); if (v >= 0.1f && v <= 8.0f) s_ao_radius = v; }
        if (s_ao_mode == AO_MODE_PORT) fprintf(stderr, "[SSAO] ambient occlusion on (RSX_SSAO): strength %.2f, radius %.2f%c", s_ao_strength, s_ao_radius, 10);
    }
    return s_ao_on;
}
/* one line for the FSR / DLSS stats report */
static const char* ssao_status(void)
{
    static char b[96];
    if (!ssao_on()) return "off";
    snprintf(b, sizeof b, "%s, %u runs, skipped %u/%u/%u/%u", s_ao_mode == AO_MODE_CACAO ? "CACAO" : "on", s_ao_runs, s_ao_skips[0], s_ao_skips[1], s_ao_skips[2], s_ao_skips[3]);
    return b;
}

static const char s_ao_hlsl[] =
"Texture2D<float> zt : register(t0);\n"
"Texture2D<float> aot : register(t1);\n"
"cbuffer K : register(b0) { float4 a; float4 b; };\n"   /* a: region w, h, strength, radius; b: max radius px, depth / colour scale x, y */
"float rawz(float2 p) { p = clamp(p, 0.5, a.xy - 0.5); return zt.Load(int3(int2(p * b.yz), 0)); }\n"
"float vz(float2 p) { return 1.0 / max(1.0 - rawz(p), 1e-7); }\n"
"float4 vs(uint i : SV_VertexID) : SV_Position { float2 t = float2((i << 1) & 2, i & 2); return float4(t * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
"static const float bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };\n"
"float ps_ao(float4 sp : SV_Position) : SV_Target {\n"
"    float2 p = sp.xy; int2 ip = int2(p);\n"
"    float d = rawz(p);\n"
"    if (d >= 0.99999 || d <= 0.0) return 1.0;\n"
"    float zc = 1.0 / max(1.0 - d, 1e-7);\n"
"    float rpx = clamp(a.w * a.y * 40.0 / zc, 2.0, b.x);\n"
"    float rel = rpx / (0.87 * a.y);\n"
"    float ang = 6.2831853 * (bayer[(ip.y & 3) * 4 + (ip.x & 3)] + 0.5) / 16.0;\n"
"    float occ = 0.0;\n"
"    [unroll] for (int i = 0; i < 8; ++i) {\n"
"        float t = (i + 0.5) / 8.0;\n"
"        float r = rpx * (0.25 + 0.75 * t);\n"
"        float an = ang + i * 2.39996323;\n"
"        float2 o = float2(cos(an), sin(an)) * r;\n"
"        float d1 = (zc - vz(p + o)) / zc;\n"
"        float d2 = (zc - vz(p - o)) / zc;\n"
"        float crease = 0.5 * (d1 + d2) / rel;\n"
"        float range = (1.0 - smoothstep(2.0, 4.0, d1 / rel)) * (1.0 - smoothstep(2.0, 4.0, d2 / rel));\n"
"        occ += saturate(crease * 2.0) * range;\n"
"    }\n"
"    return saturate(1.0 - a.z * occ / 8.0);\n"
"}\n"
"float4 ps_apply(float4 sp : SV_Position) : SV_Target {\n"
"    float2 p = sp.xy; float zc = vz(p); float s = 0.0, ws = 0.0;\n"
"    [unroll] for (int y = -2; y < 2; ++y)\n"
"    [unroll] for (int x = -2; x < 2; ++x) {\n"
"        float2 q = clamp(p + float2(x, y), 0.5, a.xy - 0.5);\n"
"        float w = 1.0 / (1e-3 + abs(vz(q) - zc) / zc * 50.0);\n"
"        s += aot.Load(int3(int2(q), 0)) * w; ws += w;\n"
"    }\n"
"    float ao = ws > 0.0 ? s / ws : 1.0;\n"
"    return float4(ao, ao, ao, 1.0);\n"
"}\n"
"float4 ps_apply1(float4 sp : SV_Position) : SV_Target { float ao = aot.Load(int3(int2(sp.xy), 0)); return float4(ao, ao, ao, 1.0); }\n";

static ID3D12PipelineState* ssao_pso(const char* entry, DXGI_FORMAT fmt, int multiply)
{
    ID3D12Device* dv = s_d3d.device;
    ID3DBlob *vb = NULL, *pb = NULL, *e = NULL;
    if (FAILED(D3DCompile(s_ao_hlsl, sizeof s_ao_hlsl - 1, "rsx_ssao", NULL, NULL, "vs", "vs_5_0", 0, 0, &vb, &e)) ||
        FAILED(D3DCompile(s_ao_hlsl, sizeof s_ao_hlsl - 1, "rsx_ssao", NULL, NULL, entry, "ps_5_0", 0, 0, &pb, &e))) {
        fprintf(stderr, "[SSAO] shader: %s%c", e ? (const char*)e->lpVtbl->GetBufferPointer(e) : "?", 10);
        if (e) e->lpVtbl->Release(e);
        if (vb) vb->lpVtbl->Release(vb);
        return NULL;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; memset(&pd, 0, sizeof pd);
    pd.pRootSignature = s_ao_rs;
    pd.VS.pShaderBytecode = vb->lpVtbl->GetBufferPointer(vb); pd.VS.BytecodeLength = vb->lpVtbl->GetBufferSize(vb);
    pd.PS.pShaderBytecode = pb->lpVtbl->GetBufferPointer(pb); pd.PS.BytecodeLength = pb->lpVtbl->GetBufferSize(pb);
    D3D12_RENDER_TARGET_BLEND_DESC* bt = &pd.BlendState.RenderTarget[0];
    if (multiply) {   /* scene *= AO, alpha untouched */
        bt->BlendEnable = TRUE;
        bt->SrcBlend = D3D12_BLEND_ZERO; bt->DestBlend = D3D12_BLEND_SRC_COLOR; bt->BlendOp = D3D12_BLEND_OP_ADD;
        bt->SrcBlendAlpha = D3D12_BLEND_ZERO; bt->DestBlendAlpha = D3D12_BLEND_ONE; bt->BlendOpAlpha = D3D12_BLEND_OP_ADD;
        bt->LogicOp = D3D12_LOGIC_OP_NOOP;
        bt->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
    } else bt->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = fmt; pd.SampleDesc.Count = 1;
    ID3D12PipelineState* pso = NULL;
    if (FAILED(dv->lpVtbl->CreateGraphicsPipelineState(dv, &pd, &IID_ID3D12PipelineState, (void**)&pso))) pso = NULL;
    vb->lpVtbl->Release(vb); pb->lpVtbl->Release(pb);
    if (!pso) fprintf(stderr, "[SSAO] pipeline (%s, format %d) failed%c", entry, (int)fmt, 10);
    return pso;
}

static int ssao_setup(void)
{
    ID3D12Device* dv = s_d3d.device;
    if (s_ao_fail) return 0;
    if (s_ao_rs) return 1;
    D3D12_DESCRIPTOR_HEAP_DESC hd = {0}; hd.NumDescriptors = 2 * NPAR; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    D3D12_DESCRIPTOR_HEAP_DESC rh = {0}; rh.NumDescriptors = 1; rh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    D3D12_DESCRIPTOR_RANGE rg = {0}; rg.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rg.NumDescriptors = 2;
    D3D12_ROOT_PARAMETER rp[2]; memset(rp, 0, sizeof rp);
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[0].DescriptorTable.NumDescriptorRanges = 1; rp[0].DescriptorTable.pDescriptorRanges = &rg;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[1].Constants.ShaderRegister = 0; rp[1].Constants.Num32BitValues = 8;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rd = {0}; rd.NumParameters = 2; rd.pParameters = rp;
    ID3DBlob *sig = NULL, *err = NULL;
    ID3D12RootSignature* rs = NULL;
    if (FAILED(dv->lpVtbl->CreateDescriptorHeap(dv, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_ao_heap)) ||
        FAILED(dv->lpVtbl->CreateDescriptorHeap(dv, &rh, &IID_ID3D12DescriptorHeap, (void**)&s_ao_rtv_heap)) ||
        FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
        FAILED(dv->lpVtbl->CreateRootSignature(dv, 0, sig->lpVtbl->GetBufferPointer(sig), sig->lpVtbl->GetBufferSize(sig),
                                               &IID_ID3D12RootSignature, (void**)&rs))) {
        fprintf(stderr, "[SSAO] setup failed: ambient occlusion stays off%c", 10);
        if (err) err->lpVtbl->Release(err);
        if (sig) sig->lpVtbl->Release(sig);
        s_ao_fail = 1; return 0;
    }
    sig->lpVtbl->Release(sig);
    s_ao_rs = rs;
    if (!(s_ao_pso_ao = ssao_pso("ps_ao", DXGI_FORMAT_R8_UNORM, 0))) { s_ao_fail = 1; return 0; }
    return 1;
}

/* The R8 AO target, at least w x h (kept at the largest size seen). */
static int ssao_tex(u32 w, u32 h)
{
    if (s_ao_tex && s_ao_tw >= w && s_ao_th >= h) return 1;
    if (s_ao_tex) { grave_defer(s_ao_tex); s_ao_tex = NULL; }
    if (w < s_ao_tw) w = s_ao_tw;
    if (h < s_ao_th) h = s_ao_th;
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC dd = {0};
    dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    dd.Width = w; dd.Height = h; dd.DepthOrArraySize = 1; dd.MipLevels = 1;
    dd.Format = DXGI_FORMAT_R8_UNORM; dd.SampleDesc.Count = 1;
    dd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv = {0}; cv.Format = DXGI_FORMAT_R8_UNORM; cv.Color[0] = 1.0f;
    if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &dd,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, &IID_ID3D12Resource, (void**)&s_ao_tex))) {
        s_ao_tex = NULL; s_ao_tw = s_ao_th = 0; return 0;
    }
    s_ao_tw = w; s_ao_th = h; s_ao_tex_st = D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_CPU_DESCRIPTOR_HANDLE r; s_ao_rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_ao_rtv_heap, &r);
    s_d3d.device->lpVtbl->CreateRenderTargetView(s_d3d.device, s_ao_tex, NULL, r);
    return 1;
}
static void ssao_barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES* st, D3D12_RESOURCE_STATES to)
{
    if (*st == to) return;
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r; b.Transition.StateBefore = *st; b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    *st = to;
}
/* SRV format of a typeless depth resource (0 = not readable) */
static DXGI_FORMAT ssao_depth_srv_fmt(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R24G8_TYPELESS:    return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32_TYPELESS:      return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS:      return DXGI_FORMAT_R16_UNORM;
    default:                            return DXGI_FORMAT_UNKNOWN;
    }
}

/* Render thread, after the target of non-clear draw `want` is bound (cur_zi: its
 * zeta slot or -1; own_depth: it uses the target's own depth buffer). */
static void ssao_note_draw(int want, int cur_zi, int own_depth)
{
    if (want < 0 || want >= MAX_OFF_RTS || !ssao_on()) return;
    const OffRT* r = &s_d3d.off_rt[want];
    if (r->w != s_d3d.width || r->h != s_d3d.height) return;
    s_ao_cnt[want]++;
    if (want != s_ao_slot) return;
    if (cur_zi >= 0 && cur_zi < MAX_ZETA) { if (s_ao_zcnt[cur_zi] < 0xFFFFu) s_ao_zcnt[cur_zi]++; }
    else if (own_depth) s_ao_rtz_cnt++;
}

#include "rsx_cacao.inc"   /* RFOM_AO_MODE=cacao */

/* Render thread, before non-clear draw `dr` (target `want`) binds its target:
 * if it is the first draw to sample the finished scene, apply AO to the scene.
 * Returns 1 when the passes ran (the caller re-binds its own state). */
static int ssao_maybe_apply(const D3D12DrawRecord* dr, int want)
{
    const int slot = s_ao_slot;
    if (s_ao_done || slot < 0 || dr->is_clear || want == slot || !ssao_on()) return 0;
    int samp = 0;
    for (int u = 0; u < RSX_UNITS; u++) if (dr->tex_rt[u] == slot) samp = 1;
    if (!samp) return 0;
    s_ao_done = 1;
    OffRT* rt = &s_d3d.off_rt[slot];
    if (!rt->res) return 0;
    /* the depth most scene draws used */
    int zbest = -1; u32 zn = 0;
    for (int z = 0; z < MAX_ZETA; z++) if (s_ao_zcnt[z] > zn) { zn = s_ao_zcnt[z]; zbest = z; }
    ID3D12Resource* dres = NULL; D3D12_RESOURCE_STATES* dst = NULL;
    if (zbest >= 0 && zn >= s_ao_rtz_cnt && s_zt[zbest].res && s_zt[zbest].w == rt->w && s_zt[zbest].h == rt->h) {
        dres = s_zt[zbest].res; dst = &s_zt[zbest].st;
    } else if (s_ao_rtz_cnt && s_rtdepth[slot]) {
        dres = s_rtdepth[slot]; dst = &s_rtdepth_st[slot];
    }
    if (!dres) { s_ao_skips[0]++; return 0; }
    D3D12_RESOURCE_DESC cd, zd;
    rt->res->lpVtbl->GetDesc(rt->res, &cd);
    dres->lpVtbl->GetDesc(dres, &zd);
    const DXGI_FORMAT zf = ssao_depth_srv_fmt(zd.Format);
    if (zf == DXGI_FORMAT_UNKNOWN) { s_ao_skips[3]++; return 0; }
    const float uf = (fsr_mode() && s_fsr_slot == slot && s_fsr_up_r > 0.0f && s_fsr_up_r < 1.0f) ? s_fsr_up_r : 1.0f;
    const u32 cw = (u32)cd.Width, ch = cd.Height;
    u32 rw = (u32)((float)cw * uf + 0.5f), rh = (u32)((float)ch * uf + 0.5f);
    if (rw > cw) rw = cw;
    if (rh > ch) rh = ch;
    const float zsx = (float)zd.Width / (float)cw, zsy = (float)zd.Height / (float)ch;
    if (rw < 8 || rh < 8 || zsx < 0.99f || zsy < 0.99f) { s_ao_skips[1]++; return 0; }
    const int cacao = s_ao_mode == AO_MODE_CACAO;
    if (!ssao_setup() || !ssao_tex(rw, rh) || (cacao && !cacao_ready(rw, rh))) { s_ao_skips[2]++; return 0; }
    ID3D12PipelineState** ap = cacao ? &s_ao_pso_apply1 : &s_ao_pso_apply;
    u32* af = cacao ? &s_ao_apply1_fmt : &s_ao_apply_fmt;
    if (!*ap || *af != rt->dxgi) {
        ID3D12PipelineState* p = ssao_pso(cacao ? "ps_apply1" : "ps_apply", (DXGI_FORMAT)rt->dxgi, 1);
        if (!p) { s_ao_skips[2]++; return 0; }
        if (*ap) grave_defer((ID3D12Resource*)*ap);
        *ap = p; *af = rt->dxgi;
    }
    ID3D12Device* dv = s_d3d.device;
    ID3D12GraphicsCommandList* cl = s_d3d.cmd_list;
    /* descriptors: t0 depth, t1 AO; one pair per upload half (the other frame may still be in flight) */
    const UINT inc = dv->lpVtbl->GetDescriptorHandleIncrementSize(dv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT base = (UINT)T_PARITY * 2u;   /* 2 SRVs per parity slot: not rewritten while that batch is in flight */
    D3D12_CPU_DESCRIPTOR_HANDLE c0; s_ao_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_ao_heap, &c0);
    c0.ptr += (SIZE_T)base * inc;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
    sv.Format = zf;
    dv->lpVtbl->CreateShaderResourceView(dv, dres, &sv, c0);
    D3D12_CPU_DESCRIPTOR_HANDLE c1 = c0; c1.ptr += inc;
    sv.Format = DXGI_FORMAT_R8_UNORM;
    dv->lpVtbl->CreateShaderResourceView(dv, s_ao_tex, &sv, c1);
    D3D12_GPU_DESCRIPTOR_HANDLE g0; s_ao_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(s_ao_heap, &g0);
    g0.ptr += (UINT64)base * inc;

    ssao_barrier(dres, dst, D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ssao_barrier(s_ao_tex, &s_ao_tex_st, D3D12_RESOURCE_STATE_RENDER_TARGET);
    /* CACAO: depth -> AO into s_ao_tex instead of pass 1 (the depth texels covering the region: zsx / zsy per colour pixel) */
    if (cacao) cacao_run(dres, zf, rw, rh, (u32)((float)rw * zsx + 0.5f), (u32)((float)rh * zsy + 0.5f));
    const float k[8] = { (float)rw, (float)rh, s_ao_strength, s_ao_radius, (float)rh * 0.06f, zsx, zsy, 0.0f };
    D3D12_VIEWPORT vp = { 0, 0, (float)rw, (float)rh, 0.0f, 1.0f };
    D3D12_RECT sc = { 0, 0, (LONG)rw, (LONG)rh };
    ID3D12DescriptorHeap* hh[] = { s_ao_heap };
    cl->lpVtbl->SetDescriptorHeaps(cl, 1, hh);
    cl->lpVtbl->SetGraphicsRootSignature(cl, s_ao_rs);
    cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, 0, g0);
    cl->lpVtbl->SetGraphicsRoot32BitConstants(cl, 1, 8, k, 0);
    cl->lpVtbl->IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->lpVtbl->RSSetViewports(cl, 1, &vp); cl->lpVtbl->RSSetScissorRects(cl, 1, &sc);
    /* pass 1: depth -> AO */
    if (!cacao) {
        D3D12_CPU_DESCRIPTOR_HANDLE r; s_ao_rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(s_ao_rtv_heap, &r);
        cl->lpVtbl->OMSetRenderTargets(cl, 1, &r, FALSE, NULL);
        cl->lpVtbl->SetPipelineState(cl, s_ao_pso_ao);
        cl->lpVtbl->DrawInstanced(cl, 3, 1, 0, 0);
    }
    /* pass 2: blurred AO (CACAO: as it is) multiplied into the scene */
    ssao_barrier(s_ao_tex, &s_ao_tex_st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    off_rt_transition(slot, D3D12_RESOURCE_STATE_RENDER_TARGET);
    { D3D12_CPU_DESCRIPTOR_HANDLE r = off_rt_rtv(slot); cl->lpVtbl->OMSetRenderTargets(cl, 1, &r, FALSE, NULL); }
    cl->lpVtbl->SetPipelineState(cl, *ap);
    cl->lpVtbl->DrawInstanced(cl, 3, 1, 0, 0);
    /* back to the backend's own bindings (as fsr_merge) */
    vp_set_heaps(cl);   /* + the guest sampler heap (Phase 2) */
    cl->lpVtbl->SetGraphicsRootSignature(cl, s_d3d.vp_root_sig);
    if (s_gf_buf)
        cl->lpVtbl->SetGraphicsRootShaderResourceView(cl, 3, s_gf_buf->lpVtbl->GetGPUVirtualAddress(s_gf_buf) + (u64)T_PARITY * GF_CAP);
    off_rt_transition(slot, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    s_rs_epoch++;
    s_ao_runs++;
    { static int n = 0; if (n < 3) { n++;
        fprintf(stderr, "[SSAO] %s applied: scene slot %d (0x%08X %ux%u, %ux%u drawn), depth %s %ux%u, format %u%c", cacao ? "CACAO" : "SSAO",
                slot, rt->off, rt->w, rt->h, rw, rh, dres == s_rtdepth[slot] ? "own" : "zeta", (u32)zd.Width, zd.Height, rt->dxgi, 10); } }
    return 1;
}

/* Render thread, end of a presented frame: pick next frame's scene surface. */
static void ssao_frame_end(void)
{
    if (!ssao_on()) return;
    int best = -1; u32 bn = 63;   /* a 3D scene: at least 64 draws (menus and movies stay untouched) */
    for (int i = 0; i < MAX_OFF_RTS; i++) {
        if (s_ao_cnt[i] > bn) { bn = s_ao_cnt[i]; best = i; }
        s_ao_cnt[i] = 0;
    }
    if (best != s_ao_slot) {
        static int n = 0;
        if (n < 8 && best >= 0) { n++; fprintf(stderr, "[SSAO] scene surface: slot %d (0x%08X), %u draws%c", best, s_d3d.off_rt[best].off, bn, 10); }
    }
    s_ao_slot = best;
    s_ao_done = 0;
    memset(s_ao_zcnt, 0, sizeof s_ao_zcnt);
    s_ao_rtz_cnt = 0;
}
