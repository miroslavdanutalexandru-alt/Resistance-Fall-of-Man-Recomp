/*
 * rsx_mt_record.c (RSX_MT_RECORD): the render-thread offload of the D3D12
 * frame prepass.
 *   queue   - many rounds of streamed pushes / publishes / finishes with 1-4
 *             workers: every item runs exactly once, none after finish
 *             returns, overflow past the capacity runs inline.
 *   memo    - texture checksums listed ahead and computed by the pool equal
 *             the direct computation (render thread racing the workers for
 *             the same entries, unlisted keys, generations).
 *   warp    - D3D12 on WARP: a synthetic heavy frame stream (6144 draws, each
 *             sampling 4 textures through its own descriptor window, random
 *             views / swizzles / null views, order-dependent blending) is
 *             rendered with the windows filled inline and through the pool's
 *             view-creation jobs, and the read-back images must be identical
 *             byte for byte, frame after frame. With the D3D12 debug layer
 *             available (argument 1 not "none" and the Agility SDK's
 *             D3D12Core.dll + d3d12SDKLayers.dll in .\mtr_d3d12\) the device runs with the
 *             debug layer and GPU-based validation and any error fails.
 *   timing  - render-thread CPU time of the descriptor and checksum phases,
 *             inline vs. the pool, on a plain WARP device / a 64 MB buffer.
 */
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "rsx_mt_record.h"
#include "rsx_mt_replay.h"

/* Agility SDK opt-in: with the package's D3D12Core.dll + d3d12SDKLayers.dll in .\mtr_d3d12\ (the build copies them from
   RSX_D3D12_SDK_LAYERS_DIR) the debug layer and GPU-based validation are available without Graphics Tools. */
__declspec(dllexport) extern const UINT D3D12SDKVersion;
__declspec(dllexport) extern const char* D3D12SDKPath;
#ifndef MTR_AGILITY_VER
#define MTR_AGILITY_VER 620
#endif
const UINT D3D12SDKVersion = MTR_AGILITY_VER;
const char* D3D12SDKPath = ".\\mtr_d3d12\\";

static int g_checks, g_fail;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                           printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t g_rng = 12345u;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static double now_us(void)
{
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e6 / (double)f.QuadPart;
}
/* the rest of a draw's prepass (PSO lookup, state): keeps the producer busy like the backend's */
static volatile uint32_t g_sink;
static void other_work(int ns)
{
    const double t = now_us() + ns / 1000.0;
    uint32_t x = g_sink;
    while (now_us() < t) x = x * 1664525u + 1013904223u;
    g_sink = x;
}

/* ---- queue ---- */
static volatile long g_hits[20000];
static void count_job(void* ctx, void* item)
{
    (void)ctx;
    const unsigned i = *(unsigned*)item;
    if ((i & 63) == 0) { volatile int s = 0; for (int k = 0; k < 200; k++) s += k; }   /* uneven job length */
    InterlockedIncrement(&g_hits[i]);
}
static void queue_test(int threads)
{
    rsx_mtr_configure(1, threads, 1, 1);
    CHECK(rsx_mtr_on(), "pool did not start with %d threads", threads);
    CHECK(rsx_mtr_workers() == threads, "%d workers, wanted %d", rsx_mtr_workers(), threads);
    rsx_mtr_queue q;
    CHECK(rsx_mtr_queue_init(&q, count_job, NULL, sizeof(unsigned), 16384u), "queue init");
    for (int round = 0; round < 300; round++) {
        const unsigned n = round == 7 ? 20000u : rnd() % 6000u;   /* round 7 overflows the capacity */
        memset((void*)g_hits, 0, sizeof g_hits);
        rsx_mtr_begin(&q);
        for (unsigned i = 0; i < n; i++) {
            rsx_mtr_push(&q, &i);
            if ((rnd() & 255) == 0) rsx_mtr_publish(&q);
            if ((rnd() & 1023) == 0) Sleep(0);
        }
        rsx_mtr_finish(&q);
        int bad = 0;
        for (unsigned i = 0; i < 20000u; i++) if (g_hits[i] != (i < n ? 1 : 0)) bad++;
        CHECK(!bad, "round %d (%u items, %d threads): %d items ran != once", round, n, threads, bad);
        CHECK(q.done == q.n, "round %d: done %ld of %ld", round, q.done, q.n);
        if (round % 50 == 0) Sleep(2);   /* let the workers fall asleep between some rounds */
    }
    CHECK(q.by_workers > 0, "workers never ran an item");
    CHECK(q.inline_runs >= 20000u - 16384u, "overflow did not run inline");
    rsx_mtr_queue_free(&q);
    rsx_mtr_shutdown();
}
static void queue_off_test(void)
{
    rsx_mtr_configure(0, 2, 1, 1);
    CHECK(!rsx_mtr_on() && !rsx_mtr_srv_on() && !rsx_mtr_csum_on(), "RSX_MT_RECORD=0 still on");
    rsx_mtr_queue q;
    rsx_mtr_queue_init(&q, count_job, NULL, sizeof(unsigned), 64u);
    memset((void*)g_hits, 0, sizeof g_hits);
    rsx_mtr_begin(&q);
    for (unsigned i = 0; i < 100; i++) { rsx_mtr_push(&q, &i); CHECK(g_hits[i] == 1, "off: item %u not run inline", i); }
    rsx_mtr_finish(&q);
    rsx_mtr_queue_free(&q);
    rsx_mtr_shutdown();
}

/* ---- checksum memo (the backend's two sums) ---- */
static unsigned csum_n(const unsigned char* base, unsigned nbytes, unsigned samples)
{
    unsigned h = 2166136261u;
    unsigned step = nbytes > samples * 4u ? nbytes / samples : 4u;
    for (unsigned i = 0; i + 3 < nbytes; i += step) { unsigned w; memcpy(&w, base + i, 4); h ^= w; h *= 16777619u; }
    return h;
}
static unsigned csum_full(const unsigned char* base, unsigned nbytes)
{
    unsigned h = 2166136261u;
    unsigned step = nbytes > 4096u ? nbytes / 1024u : 4u;
    for (unsigned i = 0; i + 3 < nbytes; i += step) { unsigned w; memcpy(&w, base + i, 4); h ^= w; h *= 16777619u; }
    return h;
}
static unsigned csum_any(const unsigned char* b, unsigned nb, int full) { return full ? csum_full(b, nb) : csum_n(b, nb, 128u); }

#define VM_BYTES (64u << 20)
#define NTEX 900
typedef struct { unsigned off, nb; int full; } TexRef;
static void make_textures(TexRef* t, unsigned char* vm)
{
    for (unsigned i = 0; i < VM_BYTES; i += 4) { unsigned v = rnd(); memcpy(vm + i, &v, 4); }
    for (int i = 0; i < NTEX; i++) {
        t[i].nb = (16u << 10) << (rnd() % 7);              /* 16 KB - 1 MB */
        t[i].off = (rnd() % (VM_BYTES - t[i].nb)) & ~127u;
        t[i].full = (rnd() % 16) == 0;                      /* every 16th check of a slot is the full sum */
    }
}
static void memo_test(int threads)
{
    unsigned char* vm = (unsigned char*)malloc(VM_BYTES);
    static TexRef tex[NTEX];
    make_textures(tex, vm);
    rsx_mtr_configure(1, threads, 1, 1);
    rsx_mtr_on();
    rsx_mtr_memo m;
    CHECK(rsx_mtr_memo_init(&m, 8192u, csum_any), "memo init");
    unsigned v;
    CHECK(!rsx_mtr_memo_get(&m, vm, 4096u, 0, &v), "closed memo answered");
    for (int round = 0; round < 200; round++) {
        rsx_mtr_memo_begin(&m);
        const int listed = 200 + (int)(rnd() % 700);
        for (int i = 0; i < listed; i++) rsx_mtr_memo_add(&m, vm + tex[i].off, tex[i].nb, tex[i].full);
        for (int i = 0; i < 50; i++) rsx_mtr_memo_add(&m, vm + tex[i].off, tex[i].nb, tex[i].full);   /* duplicates */
        rsx_mtr_memo_start(&m);
        int bad = 0, unl = 0;
        const int asked = (round & 3) == 0 ? listed / 3 : listed;   /* some rounds end with sums still pending */
        for (int k = 0; k < asked + 40; k++) {
            const int i = k < asked ? (int)((k * 7919u + (unsigned)round) % (unsigned)listed) : listed + k - asked;
            if (i >= NTEX) continue;
            if (rsx_mtr_memo_get(&m, vm + tex[i].off, tex[i].nb, tex[i].full, &v)) {
                if (v != csum_any(vm + tex[i].off, tex[i].nb, tex[i].full)) bad++;
                if (i >= listed) unl++;
            } else if (i < listed) bad++;
        }
        CHECK(!bad, "round %d: %d wrong / missing sums", round, bad);
        CHECK(!unl, "round %d: %d unlisted keys answered", round, unl);
        CHECK(!rsx_mtr_memo_get(&m, vm + tex[0].off, tex[0].nb + 4u, tex[0].full, &v), "other byte count matched");
        CHECK(!rsx_mtr_memo_get(&m, vm + tex[0].off, tex[0].nb, !tex[0].full, &v), "other kind matched");
        rsx_mtr_memo_end(&m);
        CHECK(!rsx_mtr_memo_get(&m, vm + tex[0].off, tex[0].nb, tex[0].full, &v), "ended memo answered");
        for (int i = 0; i < 64; i++) vm[rnd() % VM_BYTES] ^= 0x5A;   /* the guest changes some texels */
    }
    rsx_mtr_memo_free(&m);
    rsx_mtr_shutdown();
    free(vm);
}
/* Render-thread time of a York-like frame's checksums: 2600 draws, ~2 units
 * each over the frame's textures, first bind of a texture checks it. */
static void csum_timing(void)
{
    unsigned char* vm = (unsigned char*)malloc(VM_BYTES);
    static TexRef tex[NTEX];
    make_textures(tex, vm);
    enum { DRAWS = 2600, UNITS = 2, FRAMES = 30 };
    static int bind[DRAWS][UNITS];
    for (int d = 0; d < DRAWS; d++) for (int u = 0; u < UNITS; u++) bind[d][u] = (int)(rnd() % 600);
    static unsigned ref[NTEX]; static int used[NTEX];
    double st = 0, mt = 0;
    for (int mode = 0; mode < 2; mode++) {
        rsx_mtr_memo m; memset(&m, 0, sizeof m);
        if (mode) { rsx_mtr_configure(1, 0, 1, 1); rsx_mtr_on(); rsx_mtr_memo_init(&m, 8192u, csum_any); }
        int bad = 0;
        for (int f = 0; f < FRAMES; f++) {
            /* evict the previous frame's lines like a real frame's other work does */
            for (unsigned i = 0; i < VM_BYTES; i += 4096u) g_sink += vm[(i * 2654435761u) % VM_BYTES];
            memset(used, 0, sizeof used);
            const double t0 = now_us();
            if (mode) {
                static unsigned seen[NTEX]; static unsigned gen = 0; gen++;
                rsx_mtr_memo_begin(&m);
                for (int d = 0; d < DRAWS; d++) for (int u = 0; u < UNITS; u++) {
                    const int i = bind[d][u];
                    if (seen[i] == gen) continue;
                    seen[i] = gen;
                    rsx_mtr_memo_add(&m, vm + tex[i].off, tex[i].nb, tex[i].full);
                }
                rsx_mtr_memo_start(&m);
            }
            for (int d = 0; d < DRAWS; d++) {
                other_work(400);
                for (int u = 0; u < UNITS; u++) {
                    const int i = bind[d][u];
                    if (used[i]) continue;
                    used[i] = 1;
                    unsigned v;
                    if (!(mode && rsx_mtr_memo_get(&m, vm + tex[i].off, tex[i].nb, tex[i].full, &v)))
                        v = csum_any(vm + tex[i].off, tex[i].nb, tex[i].full);
                    if (!mode) ref[i] = v; else if (v != ref[i]) bad++;
                }
            }
            if (mode) rsx_mtr_memo_end(&m);
            const double el = now_us() - t0 - DRAWS * 0.4;   /* minus the simulated other work */
            if (f >= 5) { if (mode) mt += el; else st += el; }
        }
        CHECK(!bad, "timing run: %d sums differ from inline", bad);
        if (mode) {
            printf("  csum memo: %llu ready, %llu waited, %llu render thread per %d frames\n",
                   m.hits, m.waits, m.self, FRAMES);
            rsx_mtr_memo_free(&m); rsx_mtr_shutdown();
        }
    }
    printf("TIMING texture checksums (600 textures, 2600 draws): render thread %.0f us/frame inline -> %.0f us/frame with the pool\n",
           st / (FRAMES - 5), mt / (FRAMES - 5));
    free(vm);
}

/* ---- D3D12 on WARP ---- */
static ID3D12Device* make_device(const char* agility, int* debug)
{
    IDXGIFactory4* fac = NULL; IDXGIAdapter* ad = NULL; ID3D12Device* dev = NULL;
    *debug = 0;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory4, (void**)&fac))) return NULL;
    if (FAILED(IDXGIFactory4_EnumWarpAdapter(fac, &IID_IDXGIAdapter, (void**)&ad))) { IDXGIFactory4_Release(fac); return NULL; }
    if (agility && *agility) {
        /* The Agility runtime + layers in .\mtr_d3d12\ (copied there by the build, see the exports above);
           without that folder the inbox runtime loads and the layers are missing unless Graphics Tools are installed. */
        ID3D12Debug1* d1 = NULL;
        HRESULT hr = D3D12GetDebugInterface(&IID_ID3D12Debug1, (void**)&d1);
        if (SUCCEEDED(hr)) {
            ID3D12Debug1_EnableDebugLayer(d1);
            ID3D12Debug1_SetEnableGPUBasedValidation(d1, TRUE);
            if (SUCCEEDED(hr = D3D12CreateDevice((IUnknown*)ad, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void**)&dev))) *debug = 2;
            else dev = NULL;
            ID3D12Debug1_Release(d1);
        }
        if (!dev) {
            char core[MAX_PATH] = "(not loaded)"; HMODULE cm = GetModuleHandleA("D3D12Core.dll");
            if (cm) GetModuleFileNameA(cm, core, sizeof core);
            printf("  debug layer: not available (hr 0x%08lX; runtime %s) -- running without it\n", (unsigned long)hr, core);
        }
    }
    if (!dev && FAILED(D3D12CreateDevice((IUnknown*)ad, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void**)&dev))) dev = NULL;
    IDXGIAdapter_Release(ad); IDXGIFactory4_Release(fac);
    return dev;
}
static int debug_errors(ID3D12Device* dev)
{
    ID3D12InfoQueue* iq = NULL; int n = 0;
    if (FAILED(ID3D12Device_QueryInterface(dev, &IID_ID3D12InfoQueue, (void**)&iq))) return 0;
    const UINT64 m = ID3D12InfoQueue_GetNumStoredMessages(iq);
    for (UINT64 i = 0; i < m; i++) {
        SIZE_T len = 0; ID3D12InfoQueue_GetMessage(iq, i, NULL, &len);
        D3D12_MESSAGE* msg = (D3D12_MESSAGE*)malloc(len);
        if (msg && SUCCEEDED(ID3D12InfoQueue_GetMessage(iq, i, msg, &len)) &&
            (msg->Severity == D3D12_MESSAGE_SEVERITY_ERROR || msg->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)) {
            if (n < 5) printf("  debug layer: %s\n", msg->pDescription);
            n++;
        }
        free(msg);
    }
    ID3D12InfoQueue_ClearStoredMessages(iq);
    ID3D12InfoQueue_Release(iq);
    return n;
}

static const char k_hlsl[] =
    "cbuffer C : register(b0) { uint did; };\n"
    "struct VO { float4 p : SV_Position; };\n"
    "VO vs(uint vid : SV_VertexID) {\n"
    "  uint cell = did % 4096u; float2 c = float2(cell % 64u, cell / 64u);\n"
    "  float2 q = float2((vid == 1 || vid == 2 || vid == 4) ? 1 : 0, (vid == 2 || vid == 4 || vid == 5) ? 1 : 0);\n"
    "  float s = did >= 4096u ? 2.5 : 1.0;\n"
    "  float2 px = (c + q * s) * 4.0;\n"
    "  VO o; o.p = float4(px.x / 128.0 - 1.0, 1.0 - px.y / 128.0, 0, 1); return o; }\n"
    "Texture2D t0 : register(t0); Texture2D t1 : register(t1); Texture2D t2 : register(t2); Texture2D t3 : register(t3);\n"
    "float4 ps(VO i) : SV_Target {\n"
    "  int3 c = int3(int2(i.p.xy) & 7, 0);\n"
    "  float4 r = t0.Load(c) * 0.4 + t1.Load(c) * 0.3 + t2.Load(c) * 0.2 + t3.Load(c) * 0.1;\n"
    "  r.a = 0.6; return r; }\n";

#define W_RT 256
#define W_NTEX 96
#define W_DRAWS 6144
#define W_UNITS 4
typedef struct { int tex; UINT map; DXGI_FORMAT fmt; } Bind;

typedef struct {
    ID3D12Device* dev; ID3D12CommandQueue* q; ID3D12CommandAllocator* al; ID3D12GraphicsCommandList* cl;
    ID3D12Fence* fence; UINT64 fv; HANDLE ev;
    ID3D12RootSignature* rs; ID3D12PipelineState* pso;
    ID3D12Resource* rt; ID3D12Resource* rb; ID3D12Resource* tex[W_NTEX];
    ID3D12DescriptorHeap* heap; ID3D12DescriptorHeap* rtvh; UINT inc;
} Warp;

static void gpu_wait(Warp* w)
{
    ID3D12CommandQueue_Signal(w->q, w->fence, ++w->fv);
    if (ID3D12Fence_GetCompletedValue(w->fence) < w->fv) {
        ID3D12Fence_SetEventOnCompletion(w->fence, w->fv, w->ev);
        WaitForSingleObject(w->ev, INFINITE);
    }
}
static int warp_init(Warp* w, ID3D12Device* dev)
{
    memset(w, 0, sizeof *w); w->dev = dev;
    D3D12_COMMAND_QUEUE_DESC qd = {0}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void**)&w->q))) return 0;
    ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void**)&w->al);
    ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, w->al, NULL, &IID_ID3D12GraphicsCommandList, (void**)&w->cl);
    ID3D12GraphicsCommandList_Close(w->cl);
    ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&w->fence);
    w->ev = CreateEventA(NULL, FALSE, FALSE, NULL);

    /* root: b0 = draw id, t0-t3 = the draw's window */
    D3D12_DESCRIPTOR_RANGE rg = {0};
    rg.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rg.NumDescriptors = W_UNITS;
    D3D12_ROOT_PARAMETER rp[2]; memset(rp, 0, sizeof rp);
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[0].Constants.Num32BitValues = 1;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1; rp[1].DescriptorTable.pDescriptorRanges = &rg;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd = {0}; rsd.NumParameters = 2; rsd.pParameters = rp;
    ID3DBlob *sig = NULL, *err = NULL, *vsb = NULL, *psb = NULL;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) return 0;
    if (FAILED(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(sig), ID3D10Blob_GetBufferSize(sig),
                                                &IID_ID3D12RootSignature, (void**)&w->rs))) return 0;
    ID3D10Blob_Release(sig);
    if (FAILED(D3DCompile(k_hlsl, sizeof k_hlsl - 1, "mtr", NULL, NULL, "vs", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(k_hlsl, sizeof k_hlsl - 1, "mtr", NULL, NULL, "ps", "ps_5_0", 0, 0, &psb, &err))) {
        printf("  shader: %s\n", err ? (const char*)ID3D10Blob_GetBufferPointer(err) : "?"); return 0;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; memset(&pd, 0, sizeof pd);
    pd.pRootSignature = w->rs;
    pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vsb); pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vsb);
    pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(psb); pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(psb);
    pd.BlendState.RenderTarget[0].BlendEnable = TRUE;
    pd.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA; pd.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    pd.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    pd.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE; pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    pd.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = 0xFFFFFFFFu;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; pd.SampleDesc.Count = 1;
    if (FAILED(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void**)&w->pso))) return 0;
    ID3D10Blob_Release(vsb); ID3D10Blob_Release(psb);

    D3D12_HEAP_PROPERTIES hd = {0}; hd.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {0};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = W_RT; td.Height = W_RT; td.DepthOrArraySize = 1;
    td.MipLevels = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv = {0}; cv.Format = td.Format;
    if (FAILED(ID3D12Device_CreateCommittedResource(dev, &hd, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
                                                    &IID_ID3D12Resource, (void**)&w->rt))) return 0;
    D3D12_HEAP_PROPERTIES hr = {0}; hr.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_HEAP_PROPERTIES hu = {0}; hu.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {0};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = W_RT * W_RT * 4; bd.Height = 1; bd.DepthOrArraySize = 1;
    bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(ID3D12Device_CreateCommittedResource(dev, &hr, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                                    &IID_ID3D12Resource, (void**)&w->rb))) return 0;
    /* W_NTEX 8x8 textures with random texels, uploaded once */
    ID3D12Resource* up = NULL; bd.Width = (UINT64)W_NTEX * 8 * 256;
    if (FAILED(ID3D12Device_CreateCommittedResource(dev, &hu, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                                    &IID_ID3D12Resource, (void**)&up))) return 0;
    uint8_t* p = NULL; ID3D12Resource_Map(up, 0, NULL, (void**)&p);
    for (int i = 0; i < W_NTEX * 8 * 256; i++) p[i] = (uint8_t)rnd();
    ID3D12Resource_Unmap(up, 0, NULL);
    ID3D12CommandAllocator_Reset(w->al); ID3D12GraphicsCommandList_Reset(w->cl, w->al, NULL);
    td.Width = 8; td.Height = 8; td.Flags = D3D12_RESOURCE_FLAG_NONE;
    for (int i = 0; i < W_NTEX; i++) {
        if (FAILED(ID3D12Device_CreateCommittedResource(dev, &hd, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                                        &IID_ID3D12Resource, (void**)&w->tex[i]))) return 0;
        D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
        dst.pResource = w->tex[i]; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = up; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = (UINT64)i * 8 * 256;
        src.PlacedFootprint.Footprint.Format = td.Format; src.PlacedFootprint.Footprint.Width = 8;
        src.PlacedFootprint.Footprint.Height = 8; src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = 256;
        ID3D12GraphicsCommandList_CopyTextureRegion(w->cl, &dst, 0, 0, 0, &src, NULL);
        D3D12_RESOURCE_BARRIER b = {0};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = w->tex[i];
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ID3D12GraphicsCommandList_ResourceBarrier(w->cl, 1, &b);
    }
    ID3D12GraphicsCommandList_Close(w->cl);
    ID3D12CommandQueue_ExecuteCommandLists(w->q, 1, (ID3D12CommandList**)&w->cl);
    gpu_wait(w);
    ID3D12Resource_Release(up);

    D3D12_DESCRIPTOR_HEAP_DESC dh = {0};
    dh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; dh.NumDescriptors = W_DRAWS * W_UNITS;
    dh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(ID3D12Device_CreateDescriptorHeap(dev, &dh, &IID_ID3D12DescriptorHeap, (void**)&w->heap))) return 0;
    w->inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    dh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; dh.NumDescriptors = 1; dh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ID3D12Device_CreateDescriptorHeap(dev, &dh, &IID_ID3D12DescriptorHeap, (void**)&w->rtvh);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv; ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(w->rtvh, &rtv);
    ID3D12Device_CreateRenderTargetView(dev, w->rt, NULL, rtv);
    return 1;
}
static void view_desc(D3D12_SHADER_RESOURCE_VIEW_DESC* sv, const Bind* b)
{
    memset(sv, 0, sizeof *sv);
    sv->Format = b->fmt; sv->ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv->Shader4ComponentMapping = b->map; sv->Texture2D.MipLevels = (UINT)-1;
}
/* Fill every draw's window (inline, or through the pool) and render; 0 on a GPU/readback failure. */
static int warp_frame(Warp* w, Bind (*bind)[W_UNITS], int pooled, rsx_mtr_queue* q, uint8_t* out, double* cpu_us, int work_ns)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h0; ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(w->heap, &h0);
    /* a stale window would show: every slot starts as a view of texture 0 with a constant mapping */
    { Bind z = { 0, D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(5, 4, 5, 4), DXGI_FORMAT_R8G8B8A8_UNORM };
      D3D12_SHADER_RESOURCE_VIEW_DESC sv; view_desc(&sv, &z);
      for (int i = 0; i < W_DRAWS * W_UNITS; i++) {
          D3D12_CPU_DESCRIPTOR_HANDLE h = { h0.ptr + (SIZE_T)i * w->inc };
          ID3D12Device_CreateShaderResourceView(w->dev, w->tex[0], &sv, h);
      } }
    const double t0 = now_us();
    if (pooled) rsx_mtr_begin(q);
    for (int d = 0; d < W_DRAWS; d++) {
        if (work_ns) other_work(work_ns);
        for (int u = 0; u < W_UNITS; u++) {
            const Bind* b = &bind[d][u];
            D3D12_SHADER_RESOURCE_VIEW_DESC sv; view_desc(&sv, b);
            ID3D12Resource* r = b->tex >= 0 ? w->tex[b->tex] : NULL;
            const SIZE_T hp = h0.ptr + (SIZE_T)(d * W_UNITS + u) * w->inc;
            if (pooled) rsx_mtr_srv_push(q, w->dev, r, hp, &sv);
            else { D3D12_CPU_DESCRIPTOR_HANDLE h = { hp }; ID3D12Device_CreateShaderResourceView(w->dev, r, &sv, h); }
        }
    }
    if (pooled) rsx_mtr_srv_finish(q);
    *cpu_us = now_us() - t0 - (double)W_DRAWS * work_ns / 1000.0;
    if (!out) return 1;

    ID3D12CommandAllocator_Reset(w->al); ID3D12GraphicsCommandList_Reset(w->cl, w->al, w->pso);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv; ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(w->rtvh, &rtv);
    const float clr[4] = { 0.1f, 0.2f, 0.3f, 1.0f };
    ID3D12GraphicsCommandList_ClearRenderTargetView(w->cl, rtv, clr, 0, NULL);
    ID3D12GraphicsCommandList_OMSetRenderTargets(w->cl, 1, &rtv, FALSE, NULL);
    D3D12_VIEWPORT vp = { 0, 0, W_RT, W_RT, 0, 1 }; D3D12_RECT sc = { 0, 0, W_RT, W_RT };
    ID3D12GraphicsCommandList_RSSetViewports(w->cl, 1, &vp); ID3D12GraphicsCommandList_RSSetScissorRects(w->cl, 1, &sc);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(w->cl, 1, &w->heap);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(w->cl, w->rs);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(w->cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_GPU_DESCRIPTOR_HANDLE g0; ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(w->heap, &g0);
    for (int d = 0; d < W_DRAWS; d++) {
        D3D12_GPU_DESCRIPTOR_HANDLE g = { g0.ptr + (UINT64)d * W_UNITS * w->inc };
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstant(w->cl, 0, (UINT)d, 0);
        ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(w->cl, 1, g);
        ID3D12GraphicsCommandList_DrawInstanced(w->cl, 6, 1, 0, 0);
    }
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = w->rt;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ID3D12GraphicsCommandList_ResourceBarrier(w->cl, 1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
    src.pResource = w->rt; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = w->rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = W_RT; dst.PlacedFootprint.Footprint.Height = W_RT;
    dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = W_RT * 4;
    ID3D12GraphicsCommandList_CopyTextureRegion(w->cl, &dst, 0, 0, 0, &src, NULL);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    ID3D12GraphicsCommandList_ResourceBarrier(w->cl, 1, &b);
    if (FAILED(ID3D12GraphicsCommandList_Close(w->cl))) return 0;
    ID3D12CommandQueue_ExecuteCommandLists(w->q, 1, (ID3D12CommandList**)&w->cl);
    gpu_wait(w);
    if (FAILED(ID3D12Device_GetDeviceRemovedReason(w->dev))) return 0;
    void* p = NULL; D3D12_RANGE rr = { 0, W_RT * W_RT * 4 };
    if (FAILED(ID3D12Resource_Map(w->rb, 0, &rr, &p))) return 0;
    memcpy(out, p, W_RT * W_RT * 4);
    D3D12_RANGE wr = { 0, 0 }; ID3D12Resource_Unmap(w->rb, 0, &wr);
    return 1;
}
static void random_binds(Bind (*bind)[W_UNITS])
{
    for (int d = 0; d < W_DRAWS; d++) for (int u = 0; u < W_UNITS; u++) {
        Bind* b = &bind[d][u];
        /* consecutive draws mostly repeat their textures, like the game's */
        if (d && (rnd() % 4)) { *b = bind[d - 1][u]; continue; }
        b->tex = (rnd() % 10) == 0 ? -1 : (int)(rnd() % W_NTEX);
        b->map = (rnd() % 3) ? D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING
                             : D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(rnd() % 6, rnd() % 6, rnd() % 6, rnd() % 6);
        b->fmt = (rnd() % 5) ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    }
}
static int warp_test(const char* agility, int threads, int frames)
{
    int debug = 0;
    ID3D12Device* dev = make_device(agility, &debug);
    if (!dev) { printf("  no WARP device -- skipped\n"); return 0; }
    printf("  WARP device%s, %d worker threads, %d frames x %d draws\n",
           debug == 2 ? " + debug layer + GPU-based validation" : debug == 1 ? " + debug layer" : "", threads, frames, W_DRAWS);
    Warp w;
    if (!warp_init(&w, dev)) { CHECK(0, "WARP setup failed"); return 0; }
    CHECK(!debug_errors(dev), "debug layer errors during setup");
    rsx_mtr_configure(1, threads, 1, 1);
    CHECK(rsx_mtr_srv_on(), "pool off");
    rsx_mtr_queue q;
    CHECK(rsx_mtr_srv_queue_init(&q, 8192u), "srv queue init");   /* smaller than a frame: overflow runs inline too */
    static Bind bind[W_DRAWS][W_UNITS];
    uint8_t* a = (uint8_t*)malloc(W_RT * W_RT * 4); uint8_t* b = (uint8_t*)malloc(W_RT * W_RT * 4);
    double ta, tb;
    int diff_frames = 0, nonblank = 0;
    for (int f = 0; f < frames; f++) {
        random_binds(bind);
        if (!warp_frame(&w, bind, 0, &q, a, &ta, 0) || !warp_frame(&w, bind, 1, &q, b, &tb, (f & 1) ? 300 : 0)) {
            CHECK(0, "frame %d: GPU failure", f); break;
        }
        int nd = 0, first = -1;
        for (int i = 0; i < W_RT * W_RT * 4; i++) if (a[i] != b[i]) { if (first < 0) first = i; nd++; }
        if (nd) { diff_frames++; printf("  frame %d: %d bytes differ (first at pixel %d,%d)\n", f, nd, (first / 4) % W_RT, (first / 4) / W_RT); }
        for (int i = 0; i < W_RT * W_RT * 4; i += 4) if (a[i] != a[4]) { nonblank = 1; break; }
        const int de = debug ? debug_errors(dev) : 0;
        CHECK(!de, "frame %d: %d debug layer errors", f, de);
    }
    CHECK(!diff_frames, "%d of %d frames differ between inline and pooled view creation", diff_frames, frames);
    CHECK(nonblank, "rendered frames are blank -- the comparison proves nothing");
    CHECK(q.by_workers > 0, "the workers created no views");
    /* every job dropped the reference it took */
    for (int i = 0; i < W_NTEX; i++) {
        ID3D12Resource_AddRef(w.tex[i]);
        const ULONG rc = ID3D12Resource_Release(w.tex[i]);
        CHECK(rc == 1, "texture %d holds %lu references after the frames", i, rc);
    }
    printf("  %d frames pixel-exact (%llu views, %.0f%% created on workers)\n", frames - diff_frames,
           q.jobs, q.jobs ? 100.0 * (double)q.by_workers / (double)q.jobs : 0.0);
    rsx_mtr_queue_free(&q);
    rsx_mtr_shutdown();
    free(a); free(b);
    ID3D12Device_Release(dev);   /* the rest is released with the process */
    return 1;
}
/* Render-thread time of the descriptor phase of a York-sized frame (2600 draws). */
static void noop_job(void* ctx, void* item) { (void)ctx; (void)item; }
static void srv_timing(void)
{
    int debug = 0;
    ID3D12Device* dev = make_device(NULL, &debug);
    if (!dev) return;
    Warp w;
    if (!warp_init(&w, dev)) return;
    rsx_mtr_configure(1, 0, 1, 1);
    rsx_mtr_on();
    rsx_mtr_queue q; rsx_mtr_srv_queue_init(&q, 32768u);
    static Bind bind[W_DRAWS][W_UNITS];
    random_binds(bind);
    double st = 0, mt = 0, t;
    enum { FR = 40 };
    for (int f = 0; f < FR; f++) {
        warp_frame(&w, bind, 0, &q, NULL, &t, 400); if (f >= 5) st += t;
        warp_frame(&w, bind, 1, &q, NULL, &t, 400); if (f >= 5) mt += t;
    }
    const double scale = 2600.0 / W_DRAWS;
    printf("TIMING SRV creation (%d views/frame scaled to 2600 draws x 4): render thread %.0f us/frame inline -> %.0f us/frame with %d workers\n",
           W_DRAWS * W_UNITS, st / (FR - 5) * scale, mt / (FR - 5) * scale, rsx_mtr_workers());
    {   /* breakdown: the raw view creation on this device, and the queue's own cost per item */
        D3D12_CPU_DESCRIPTOR_HANDLE h0; ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(w.heap, &h0);
        D3D12_SHADER_RESOURCE_VIEW_DESC sv; view_desc(&sv, &bind[0][0]);
        double t0 = now_us();
        for (int i = 0; i < W_DRAWS * W_UNITS; i++) {
            D3D12_CPU_DESCRIPTOR_HANDLE h = { h0.ptr + (SIZE_T)i * w.inc };
            ID3D12Device_CreateShaderResourceView(dev, w.tex[i % W_NTEX], &sv, h);
        }
        const double create = (now_us() - t0) / (W_DRAWS * W_UNITS);
        rsx_mtr_queue nq; rsx_mtr_queue_init(&nq, noop_job, NULL, q.item_size, 32768u);
        unsigned char item[128] = {0};
        t0 = now_us();
        for (int r = 0; r < 10; r++) {
            rsx_mtr_begin(&nq);
            for (int i = 0; i < W_DRAWS * W_UNITS; i++) rsx_mtr_push(&nq, item);
            rsx_mtr_finish(&nq);
        }
        const double push = (now_us() - t0) / (10.0 * W_DRAWS * W_UNITS);
        printf("  per view: CreateShaderResourceView %.3f us (WARP), queue push %.3f us\n", create, push);
        rsx_mtr_queue_free(&nq);
    }
    rsx_mtr_queue_free(&q);
    rsx_mtr_shutdown();
    ID3D12Device_Release(dev);
}

/* ---- RSX_MT_REPLAY: the same command stream through one list and split across worker lists ---- */
#define R_RT    128
#define R_NRT   4
#define R_NTEX  16
#define R_WIN   24          /* SRV windows per render target they must not sample */
#define R_MAXEV 12000
#define R_NPSO  5
#define R_SLOTS 3

static const char k_rp_hlsl[] =
    "cbuffer A : register(b0) { float4 xf; float4 col; float4 zz; };\n"
    "cbuffer B : register(b1) { float4 tint; };\n"
    "ByteAddressBuffer jit : register(t4);\n"
    "Texture2D t0 : register(t0); Texture2D t1 : register(t1); Texture2D t2 : register(t2); Texture2D t3 : register(t3);\n"
    "struct VO { float4 p : SV_Position; float4 c : COLOR; };\n"
    "VO vs(float2 p : POSITION, uint vid : SV_VertexID) {\n"
    "  float2 j = asfloat(jit.Load2((vid % 64u) * 8u));\n"
    "  VO o; o.p = float4(p * xf.xy + xf.zw + j, zz.x, 1); o.c = col; return o; }\n"
    "float4 ps(VO i) : SV_Target {\n"
    "  int3 c = int3(int2(i.p.xy) & 7, 0); int3 r = int3(int2(i.p.xy) & 127, 0);\n"
    "  float4 s = t0.Load(c) * 0.3 + t1.Load(r) * 0.3 + t2.Load(c) * 0.2 + t3.Load(r) * 0.2;\n"
    "  return float4(saturate(i.c.rgb * 0.5 + s.rgb * 0.5 + tint.rgb * 0.25), i.c.a); }\n";

enum { EV_PASS, EV_DRAW, EV_CLEARRECT, EV_STCLEAR, EV_DIRECT };
typedef struct {
    unsigned char k, pso, idx, sref_on, t, clear;
    unsigned short win, cb;
    UINT sref; D3D12_VIEWPORT vp; D3D12_RECT rect; float clr[4];
} REv;

typedef struct {
    ID3D12Device* dev; ID3D12CommandQueue* q;
    ID3D12CommandAllocator* al[R_SLOTS]; UINT64 slot_fv[R_SLOTS]; ID3D12GraphicsCommandList* cl;
    ID3D12Fence* fence; UINT64 fv; HANDLE ev;
    ID3D12RootSignature* rs; ID3D12PipelineState* pso[R_NPSO];
    ID3D12Resource *rt[R_NRT], *ds, *tex[R_NTEX], *up, *rb;
    uint8_t* upp;
    ID3D12DescriptorHeap *heap, *rtvh, *dsvh; UINT inc, rinc;
} RW;
#define R_CB0 0u
#define R_CB2 ((UINT64)R_MAXEV * 256u)
#define R_VB  (2u * (UINT64)R_MAXEV * 256u)
#define R_IB  (R_VB + 256u)
#define R_JIT (R_VB + 512u)
#define R_UPB (R_VB + 1024u)

static void rw_wait(RW* w, UINT64 v)
{
    if (ID3D12Fence_GetCompletedValue(w->fence) < v) {
        ID3D12Fence_SetEventOnCompletion(w->fence, v, w->ev);
        WaitForSingleObject(w->ev, INFINITE);
    }
}
static void rw_barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER br = {0};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; br.Transition.pResource = r;
    br.Transition.StateBefore = a; br.Transition.StateAfter = b;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &br);
}
static D3D12_CPU_DESCRIPTOR_HANDLE rw_rtv(RW* w, int t)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h; ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(w->rtvh, &h);
    h.ptr += (SIZE_T)t * w->rinc; return h;
}
static D3D12_CPU_DESCRIPTOR_HANDLE rw_dsv(RW* w)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h; ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(w->dsvh, &h); return h;
}

static int rw_init(RW* w, ID3D12Device* dev)
{
    memset(w, 0, sizeof *w); w->dev = dev;
    D3D12_COMMAND_QUEUE_DESC qd = {0}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void**)&w->q))) return 0;
    for (int s = 0; s < R_SLOTS; s++)
        ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void**)&w->al[s]);
    ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, w->al[0], NULL, &IID_ID3D12GraphicsCommandList, (void**)&w->cl);
    ID3D12GraphicsCommandList_Close(w->cl);
    ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&w->fence);
    w->ev = CreateEventA(NULL, FALSE, FALSE, NULL);

    /* the backend's VP root layout: b0 per draw, t0-t3 window, b1 per draw, t4 root SRV */
    D3D12_DESCRIPTOR_RANGE rg = {0};
    rg.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rg.NumDescriptors = 4;
    D3D12_ROOT_PARAMETER rp[4]; memset(rp, 0, sizeof rp);
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; rp[0].Descriptor.ShaderRegister = 0;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1; rp[1].DescriptorTable.pDescriptorRanges = &rg;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; rp[2].Descriptor.ShaderRegister = 1;
    rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; rp[3].Descriptor.ShaderRegister = 4;
    rp[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC rsd = {0}; rsd.NumParameters = 4; rsd.pParameters = rp;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob *sig = NULL, *err = NULL, *vsb = NULL, *psb = NULL;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) return 0;
    if (FAILED(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(sig), ID3D10Blob_GetBufferSize(sig),
                                                &IID_ID3D12RootSignature, (void**)&w->rs))) return 0;
    ID3D10Blob_Release(sig);
    if (FAILED(D3DCompile(k_rp_hlsl, sizeof k_rp_hlsl - 1, "rp", NULL, NULL, "vs", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(k_rp_hlsl, sizeof k_rp_hlsl - 1, "rp", NULL, NULL, "ps", "ps_5_0", 0, 0, &psb, &err))) {
        printf("  shader: %s\n", err ? (const char*)ID3D10Blob_GetBufferPointer(err) : "?"); return 0;
    }
    D3D12_INPUT_ELEMENT_DESC ie = { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
    for (int i = 0; i < R_NPSO; i++) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; memset(&pd, 0, sizeof pd);
        pd.pRootSignature = w->rs; pd.InputLayout.pInputElementDescs = &ie; pd.InputLayout.NumElements = 1;
        pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vsb); pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vsb);
        pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(psb); pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(psb);
        D3D12_RENDER_TARGET_BLEND_DESC* bl = &pd.BlendState.RenderTarget[0];
        bl->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        bl->BlendOp = bl->BlendOpAlpha = D3D12_BLEND_OP_ADD;
        bl->SrcBlendAlpha = D3D12_BLEND_ONE; bl->DestBlendAlpha = D3D12_BLEND_ZERO;
        if (i == 1 || i == 3) { bl->BlendEnable = TRUE; bl->SrcBlend = D3D12_BLEND_SRC_ALPHA; bl->DestBlend = D3D12_BLEND_INV_SRC_ALPHA; }
        if (i == 2) { bl->BlendEnable = TRUE; bl->SrcBlend = D3D12_BLEND_ONE; bl->DestBlend = D3D12_BLEND_ONE; }
        D3D12_DEPTH_STENCIL_DESC* z = &pd.DepthStencilState;
        z->DepthEnable = i <= 1; z->DepthWriteMask = i == 0 ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        z->DepthFunc = i == 0 ? D3D12_COMPARISON_FUNC_LESS : D3D12_COMPARISON_FUNC_LESS_EQUAL;
        if (i >= 3) {   /* 3: draw where stencil == ref and count; 4: stamp ref */
            z->StencilEnable = TRUE; z->StencilReadMask = z->StencilWriteMask = 0xFF;
            z->FrontFace.StencilFunc = i == 3 ? D3D12_COMPARISON_FUNC_EQUAL : D3D12_COMPARISON_FUNC_ALWAYS;
            z->FrontFace.StencilPassOp = i == 3 ? D3D12_STENCIL_OP_INCR_SAT : D3D12_STENCIL_OP_REPLACE;
            z->FrontFace.StencilFailOp = z->FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
            z->BackFace = z->FrontFace;
        }
        pd.SampleMask = 0xFFFFFFFFu;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
        pd.SampleDesc.Count = 1;
        if (FAILED(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void**)&w->pso[i]))) return 0;
    }
    ID3D10Blob_Release(vsb); ID3D10Blob_Release(psb);

    D3D12_DESCRIPTOR_HEAP_DESC dh = {0};
    dh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; dh.NumDescriptors = R_NRT;
    ID3D12Device_CreateDescriptorHeap(dev, &dh, &IID_ID3D12DescriptorHeap, (void**)&w->rtvh);
    w->rinc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    dh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV; dh.NumDescriptors = 1;
    ID3D12Device_CreateDescriptorHeap(dev, &dh, &IID_ID3D12DescriptorHeap, (void**)&w->dsvh);
    dh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; dh.NumDescriptors = R_NRT * R_WIN * 4;
    dh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(ID3D12Device_CreateDescriptorHeap(dev, &dh, &IID_ID3D12DescriptorHeap, (void**)&w->heap))) return 0;
    w->inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_HEAP_PROPERTIES hd = {0}; hd.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES hu = {0}; hu.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES hr = {0}; hr.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC td = {0};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = R_RT; td.Height = R_RT; td.DepthOrArraySize = 1;
    td.MipLevels = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv = {0}; cv.Format = td.Format;
    for (int t = 0; t < R_NRT; t++) {
        if (FAILED(ID3D12Device_CreateCommittedResource(dev, &hd, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                        &cv, &IID_ID3D12Resource, (void**)&w->rt[t]))) return 0;
        ID3D12Device_CreateRenderTargetView(dev, w->rt[t], NULL, rw_rtv(w, t));
    }
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    cv.Format = td.Format; cv.DepthStencil.Depth = 1.0f;
    if (FAILED(ID3D12Device_CreateCommittedResource(dev, &hd, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                    &cv, &IID_ID3D12Resource, (void**)&w->ds))) return 0;
    ID3D12Device_CreateDepthStencilView(dev, w->ds, NULL, rw_dsv(w));

    D3D12_RESOURCE_DESC bd = {0};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Height = 1; bd.DepthOrArraySize = 1;
    bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Width = R_UPB;
    if (FAILED(ID3D12Device_CreateCommittedResource(dev, &hu, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                                    &IID_ID3D12Resource, (void**)&w->up))) return 0;
    ID3D12Resource_Map(w->up, 0, NULL, (void**)&w->upp);
    bd.Width = (UINT64)R_NRT * R_RT * R_RT * 4;
    if (FAILED(ID3D12Device_CreateCommittedResource(dev, &hr, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                                    &IID_ID3D12Resource, (void**)&w->rb))) return 0;
    {   /* a unit quad: 4 indexed corners, then the same quad as 6 list vertices; jitter */
        float* v = (float*)(w->upp + R_VB);
        static const float q[10][2] = { {0,0},{1,0},{1,1},{0,1}, {0,0},{1,0},{1,1},{0,0},{1,1},{0,1} };
        for (int i = 0; i < 10; i++) { v[i * 2] = q[i][0]; v[i * 2 + 1] = q[i][1]; }
        UINT* ix = (UINT*)(w->upp + R_IB); ix[0] = 0; ix[1] = 1; ix[2] = 2; ix[3] = 0; ix[4] = 2; ix[5] = 3;
        float* j = (float*)(w->upp + R_JIT);
        for (int i = 0; i < 128; i++) j[i] = (float)((int)(rnd() % 201) - 100) * 0.00002f;
    }

    /* 8x8 textures */
    ID3D12Resource* up = NULL; bd.Width = (UINT64)R_NTEX * 8 * 256;
    if (FAILED(ID3D12Device_CreateCommittedResource(dev, &hu, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                                    &IID_ID3D12Resource, (void**)&up))) return 0;
    uint8_t* p = NULL; ID3D12Resource_Map(up, 0, NULL, (void**)&p);
    for (int i = 0; i < R_NTEX * 8 * 256; i++) p[i] = (uint8_t)rnd();
    ID3D12Resource_Unmap(up, 0, NULL);
    ID3D12CommandAllocator_Reset(w->al[0]); ID3D12GraphicsCommandList_Reset(w->cl, w->al[0], NULL);
    td.Width = 8; td.Height = 8; td.Flags = D3D12_RESOURCE_FLAG_NONE; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    for (int i = 0; i < R_NTEX; i++) {
        if (FAILED(ID3D12Device_CreateCommittedResource(dev, &hd, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                                        &IID_ID3D12Resource, (void**)&w->tex[i]))) return 0;
        D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
        dst.pResource = w->tex[i]; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = up; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = (UINT64)i * 8 * 256;
        src.PlacedFootprint.Footprint.Format = td.Format; src.PlacedFootprint.Footprint.Width = 8;
        src.PlacedFootprint.Footprint.Height = 8; src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = 256;
        ID3D12GraphicsCommandList_CopyTextureRegion(w->cl, &dst, 0, 0, 0, &src, NULL);
        rw_barrier(w->cl, w->tex[i], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    ID3D12GraphicsCommandList_Close(w->cl);
    ID3D12CommandQueue_ExecuteCommandLists(w->q, 1, (ID3D12CommandList**)&w->cl);
    ID3D12CommandQueue_Signal(w->q, w->fence, ++w->fv); rw_wait(w, w->fv);
    ID3D12Resource_Release(up);

    /* windows: t0/t2 textures, t1/t3 another render target (never the one drawn), a texture, or a null view */
    D3D12_CPU_DESCRIPTOR_HANDLE h0; ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(w->heap, &h0);
    for (int t = 0; t < R_NRT; t++)
        for (int k = 0; k < R_WIN; k++)
            for (int u = 0; u < 4; u++) {
                D3D12_SHADER_RESOURCE_VIEW_DESC sv; memset(&sv, 0, sizeof sv);
                sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
                ID3D12Resource* r = w->tex[rnd() % R_NTEX];
                if (u & 1) {
                    const unsigned c = rnd() % 4;
                    if (c < 2) r = w->rt[(t + 1 + rnd() % (R_NRT - 1)) % R_NRT];
                    else if (c == 3) r = NULL;
                }
                D3D12_CPU_DESCRIPTOR_HANDLE h = { h0.ptr + (SIZE_T)((t * R_WIN + k) * 4 + u) * w->inc };
                ID3D12Device_CreateShaderResourceView(dev, r, &sv, h);
            }
    return 1;
}

/* A frame: passes over the targets (each one sampled by the ones after it), runs of draws with per-draw
 * pipeline / viewport / window / constants / stencil ref, and now and then something the replay may not
 * defer (a rect clear, a stencil clear, a draw emitted straight into the list). */
static int rp_gen(REv* ev, int heavy)
{
    int n = 0;
    const int passes = heavy ? 6 : 4 + (int)(rnd() % 6);
    unsigned cb = 0;
    for (int p = 0; p < passes && n < R_MAXEV - 8; p++) {
        REv* e = &ev[n++]; memset(e, 0, sizeof *e);
        e->k = EV_PASS; e->t = (unsigned char)(rnd() % R_NRT); e->clear = (unsigned char)(rnd() % 3 != 0);
        for (int i = 0; i < 4; i++) e->clr[i] = (float)(rnd() % 256) / 255.0f;
        const int t = e->t;
        const int nd = heavy ? 500 : (rnd() % 4 == 0) ? 1 + (int)(rnd() % 40) : 20 + (int)(rnd() % 900);
        int pso = (int)(rnd() % R_NPSO);
        for (int d = 0; d < nd && n < R_MAXEV - 2 && cb < R_MAXEV; d++) {
            e = &ev[n++]; memset(e, 0, sizeof *e);
            const unsigned x = rnd() % (heavy ? 2000 : 140);
            if (x == 0) {
                e->k = EV_CLEARRECT;
                e->rect.left = (LONG)(rnd() % 100); e->rect.top = (LONG)(rnd() % 100);
                e->rect.right = e->rect.left + 1 + (LONG)(rnd() % 28); e->rect.bottom = e->rect.top + 1 + (LONG)(rnd() % 28);
                for (int i = 0; i < 4; i++) e->clr[i] = (float)(rnd() % 256) / 255.0f;
                continue;
            }
            if (x == 1) {
                e->k = EV_STCLEAR; e->sref = rnd() % 4;
                e->rect.left = (LONG)(rnd() % 64); e->rect.top = (LONG)(rnd() % 64);
                e->rect.right = e->rect.left + 64; e->rect.bottom = e->rect.top + 64;
                continue;
            }
            e->k = x == 2 ? EV_DIRECT : EV_DRAW;
            if (rnd() % 6 == 0) pso = (int)(rnd() % R_NPSO);
            e->pso = (unsigned char)pso; e->idx = (unsigned char)(rnd() & 1);
            e->sref_on = (unsigned char)(pso >= 3 || rnd() % 8 == 0); e->sref = rnd() % 4;
            e->win = (unsigned short)(t * R_WIN + (int)(rnd() % R_WIN));
            e->cb = (unsigned short)cb++;
            if (rnd() % 5 == 0) {
                const float x0 = (float)(rnd() % 64), y0 = (float)(rnd() % 64);
                const D3D12_VIEWPORT v = { x0, y0, 16.0f + (float)(rnd() % 64), 16.0f + (float)(rnd() % 64), 0.0f, 1.0f };
                e->vp = v;
            } else {
                const D3D12_VIEWPORT v = { 0, 0, R_RT, R_RT, 0.0f, 1.0f }; e->vp = v;
            }
        }
    }
    return n;
}
/* the per-draw constants a frame's draws read (written once, rendered both ways) */
static void rp_fill_cbs(RW* w, const REv* ev, int n)
{
    for (int i = 0; i < n; i++) {
        if (ev[i].k != EV_DRAW && ev[i].k != EV_DIRECT) continue;
        float* a = (float*)(w->upp + R_CB0 + (UINT64)ev[i].cb * 256u);
        const float s = 0.05f + (float)(rnd() % 100) * 0.006f;
        a[0] = s; a[1] = s * (0.5f + (float)(rnd() % 100) * 0.01f);
        a[2] = (float)(rnd() % 200) * 0.01f - 1.0f - s * 0.5f; a[3] = (float)(rnd() % 200) * 0.01f - 1.0f - s * 0.5f;
        for (int c = 0; c < 4; c++) a[4 + c] = (float)(rnd() % 256) / 255.0f;
        a[8] = (float)(rnd() % 1000) / 1000.0f;
        float* b = (float*)(w->upp + R_CB2 + (UINT64)ev[i].cb * 256u);
        for (int c = 0; c < 4; c++) b[c] = (float)(rnd() % 256) / 255.0f;
    }
}

typedef struct { unsigned min_split; int split, work_ns; } RpMode;
static double g_rp_submit_us;
/* Record and submit one frame the way render_frame_impl does: clean draws through the emitters inside a
 * deferred run, everything else directly after a flush. out: every target read back (NULL: no readback,
 * no wait -- slots are reused once the GPU is done with them). Returns the render thread's recording us. */
static double rp_frame(RW* w, rsx_mtr_rp* r, const REv* ev, int n, const RpMode* m, unsigned frame, uint8_t* out)
{
    const unsigned slot = frame % R_SLOTS;
    rw_wait(w, w->slot_fv[slot]);
    const double t0 = now_us();
    ID3D12CommandAllocator_Reset(w->al[slot]);
    ID3D12GraphicsCommandList_Reset(w->cl, w->al[slot], NULL);
    rsx_mtr_rp_frame_begin(r, slot, w->cl);
    ID3D12GraphicsCommandList* cl = w->cl;
    /* every frame starts from the same target contents, so both renders of a stream see the same inputs */
    for (int t = 0; t < R_NRT; t++) {
        const float c[4] = { 0.1f * (float)t, (float)(n % 97) / 97.0f, 0.5f, 1.0f };
        rw_barrier(cl, w->rt[t], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        ID3D12GraphicsCommandList_ClearRenderTargetView(cl, rw_rtv(w, t), c, 0, NULL);
        rw_barrier(cl, w->rt[t], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    const UINT64 ua = ID3D12Resource_GetGPUVirtualAddress(w->up);
    rsx_mtr_rp_base b; memset(&b, 0, sizeof b);
    b.rs = w->rs; b.heap = w->heap; b.topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    b.has_vb = 1; b.vbv.BufferLocation = ua + R_VB; b.vbv.SizeInBytes = 80; b.vbv.StrideInBytes = 8;
    b.has_ib = 1; b.ibv.BufferLocation = ua + R_IB; b.ibv.SizeInBytes = 24; b.ibv.Format = DXGI_FORMAT_R32_UINT;
    b.srv_param = 3; b.srv = ua + R_JIT;
    b.nrt = 1; b.has_dsv = 1; b.dsv = rw_dsv(w);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, b.rs);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 1, &w->heap);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, b.topo);
    ID3D12GraphicsCommandList_IASetVertexBuffers(cl, 0, 1, &b.vbv);
    ID3D12GraphicsCommandList_IASetIndexBuffer(cl, &b.ibv);
    ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(cl, 3, b.srv);
    D3D12_GPU_DESCRIPTOR_HANDLE g0; ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(w->heap, &g0);
    int cur = -1, f_ok = 0, f_pso = -1; UINT f_sref = ~0u; D3D12_VIEWPORT f_vp; UINT64 f_tbl = 0, f_c0 = 0, f_c2 = 0;
    memset(&f_vp, 0, sizeof f_vp);
    for (int i = 0; i < n; i++) {
        const REv* e = &ev[i];
        const int clean = e->k == EV_DRAW;
        if (m->work_ns) other_work(m->work_ns);
        if (m->split && clean && !rsx_mtr_rp_deferring(r)) {
            b.rtv[0] = rw_rtv(w, cur);
            rsx_mtr_rp_defer(r, &b);
            f_ok = 0; f_sref = ~0u;   /* the run's first draw sets all of its state */
        }
        if (!clean && rsx_mtr_rp_deferring(r)) cl = rsx_mtr_rp_flush(r, m->min_split);
        switch (e->k) {
        case EV_PASS:
            if (cur >= 0) rw_barrier(cl, w->rt[cur], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            cur = e->t;
            rw_barrier(cl, w->rt[cur], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            { D3D12_CPU_DESCRIPTOR_HANDLE rt = rw_rtv(w, cur), ds = rw_dsv(w);
              ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &rt, FALSE, &ds);
              ID3D12GraphicsCommandList_ClearDepthStencilView(cl, ds, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, NULL);
              if (e->clear) ID3D12GraphicsCommandList_ClearRenderTargetView(cl, rt, e->clr, 0, NULL); }
            break;
        case EV_CLEARRECT:
            ID3D12GraphicsCommandList_ClearRenderTargetView(cl, rw_rtv(w, cur), e->clr, 1, &e->rect);
            break;
        case EV_STCLEAR:
            ID3D12GraphicsCommandList_ClearDepthStencilView(cl, rw_dsv(w), D3D12_CLEAR_FLAG_STENCIL, 1.0f, (UINT8)e->sref, 1, &e->rect);
            break;
        case EV_DIRECT: case EV_DRAW: {
            D3D12_RECT sc = { (LONG)e->vp.TopLeftX, (LONG)e->vp.TopLeftY,
                              (LONG)(e->vp.TopLeftX + e->vp.Width), (LONG)(e->vp.TopLeftY + e->vp.Height) };
            D3D12_GPU_DESCRIPTOR_HANDLE g = { g0.ptr + (UINT64)e->win * 4 * w->inc };
            const UINT64 c0 = ua + R_CB0 + (UINT64)e->cb * 256u, c2 = ua + R_CB2 + (UINT64)e->cb * 256u;
            if (e->k == EV_DIRECT) {   /* a helper's own draw, straight into the list; the next draw sets everything */
                ID3D12GraphicsCommandList_SetPipelineState(cl, w->pso[e->pso]);
                ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &e->vp); ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sc);
                ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 1, g);
                ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(cl, 0, c0);
                ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(cl, 2, c2);
                ID3D12GraphicsCommandList_DrawInstanced(cl, 6, 1, 4, 0);
                f_ok = 0;
                break;
            }
            if (!f_ok || f_pso != e->pso) { rsx_mtr_rp_pso(r, w->pso[e->pso]); f_pso = e->pso; }
            if (!f_ok || memcmp(&f_vp, &e->vp, sizeof f_vp)) { rsx_mtr_rp_vpsc(r, &e->vp, &sc); f_vp = e->vp; }
            if (!f_ok || f_tbl != g.ptr) { rsx_mtr_rp_table(r, 1, g); f_tbl = g.ptr; }
            if (!f_ok || f_c0 != c0) { rsx_mtr_rp_cbv(r, 0, c0); f_c0 = c0; }
            if (!f_ok || f_c2 != c2) { rsx_mtr_rp_cbv(r, 2, c2); f_c2 = c2; }
            if (e->sref_on && e->sref != f_sref) { rsx_mtr_rp_sref(r, e->sref); f_sref = e->sref; }
            if (e->idx) rsx_mtr_rp_draw_indexed(r, 6, 0, 0);
            else rsx_mtr_rp_draw(r, 6, 4);
            f_ok = 1;
        } break;
        }
    }
    cl = rsx_mtr_rp_flush(r, m->min_split);
    if (cur >= 0) rw_barrier(cl, w->rt[cur], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (out)
        for (int t = 0; t < R_NRT; t++) {
            rw_barrier(cl, w->rt[t], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
            src.pResource = w->rt[t]; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.pResource = w->rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Offset = (UINT64)t * R_RT * R_RT * 4;
            dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            dst.PlacedFootprint.Footprint.Width = R_RT; dst.PlacedFootprint.Footprint.Height = R_RT;
            dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = R_RT * 4;
            ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
            rw_barrier(cl, w->rt[t], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
    rsx_mtr_rp_stats s0; rsx_mtr_rp_get_stats(r, &s0);
    rsx_mtr_rp_submit(r, w->q, m->min_split);
    rsx_mtr_rp_stats s1; rsx_mtr_rp_get_stats(r, &s1);
    g_rp_submit_us += s1.wait_us - s0.wait_us;
    const double us = now_us() - t0 - (s1.exec_us - s0.exec_us);   /* WARP executes inside ExecuteCommandLists */
    ID3D12CommandQueue_Signal(w->q, w->fence, ++w->fv);
    w->slot_fv[slot] = w->fv;
    if (out) {
        rw_wait(w, w->fv);
        void* p = NULL; D3D12_RANGE rr = { 0, (SIZE_T)R_NRT * R_RT * R_RT * 4 };
        if (FAILED(ID3D12Device_GetDeviceRemovedReason(w->dev)) || FAILED(ID3D12Resource_Map(w->rb, 0, &rr, &p))) return -1.0;
        memcpy(out, p, (size_t)R_NRT * R_RT * R_RT * 4);
        D3D12_RANGE wr = { 0, 0 }; ID3D12Resource_Unmap(w->rb, 0, &wr);
    }
    return us;
}

static REv g_ev[R_MAXEV];
static void replay_test(const char* agility, int frames_per)
{
    int debug = 0;
    ID3D12Device* dev = make_device(agility, &debug);
    if (!dev) { printf("  no WARP device -- skipped\n"); return; }
    RW w;
    if (!rw_init(&w, dev)) { CHECK(0, "replay WARP setup failed"); return; }
    CHECK(!debug_errors(dev), "debug layer errors during setup");
    const size_t nb = (size_t)R_NRT * R_RT * R_RT * 4;
    uint8_t* a = (uint8_t*)malloc(nb); uint8_t* bb = (uint8_t*)malloc(nb);
    int diff = 0, frames = 0, nonblank = 0;
    rsx_mtr_rp_stats tot; memset(&tot, 0, sizeof tot);
    static const unsigned k_min[] = { 1, 24, 160 };
    for (int threads = 1; threads <= 4; threads++) {
        rsx_mtr_configure(1, threads, 1, 1);
        CHECK(rsx_mtr_on() && rsx_mtr_workers() == threads, "pool with %d workers", threads);
        rsx_mtr_rp* r = rsx_mtr_rp_create(dev);
        CHECK(r != NULL, "rsx_mtr_rp_create");
        if (!r) break;
        for (int f = 0; f < frames_per; f++, frames++) {
            const int n = rp_gen(g_ev, 0);
            rp_fill_cbs(&w, g_ev, n);
            const RpMode one = { 0, 0, 0 }, split = { k_min[f % 3], 1, (f & 1) ? 200 : 0 };
            if (rp_frame(&w, r, g_ev, n, &one, (unsigned)frames * 2, a) < 0 ||
                rp_frame(&w, r, g_ev, n, &split, (unsigned)frames * 2 + 1, bb) < 0) { CHECK(0, "frame %d: GPU failure", frames); break; }
            const int de = debug ? debug_errors(dev) : 0;
            CHECK(!de, "frame %d: %d debug layer errors", frames, de);
            int nd = 0;
            for (size_t i = 0; i < nb; i++) if (a[i] != bb[i]) nd++;
            if (nd) { diff++; if (diff <= 4) printf("  frame %d (%d workers, min %u): %d bytes differ\n", frames, threads, split.min_split, nd); }
            for (size_t i = 4; i < nb; i += 4) if (a[i] != a[0]) { nonblank = 1; break; }
        }
        rsx_mtr_rp_stats st; rsx_mtr_rp_get_stats(r, &st);
        tot.runs += st.runs; tot.runs_split += st.runs_split; tot.chunks += st.chunks;
        tot.chunks_by_workers += st.chunks_by_workers; tot.draws_split += st.draws_split; tot.fallbacks += st.fallbacks;
        tot.lists += st.lists;
        rsx_mtr_rp_destroy(r);
        rsx_mtr_shutdown();
    }
    CHECK(!diff, "%d of %d frames differ between one list and the split replay", diff, frames);
    CHECK(nonblank, "rendered targets are blank -- the comparison proves nothing");
    CHECK(tot.runs_split > 0 && tot.chunks_by_workers > 0, "nothing was split / recorded on a worker");
    printf("  %d frames pixel-exact over %d targets: %llu runs, %llu split into %llu chunks (%llu on workers, %llu lists), %llu draws split\n",
           frames - diff, R_NRT, tot.runs, tot.runs_split, tot.chunks, tot.chunks_by_workers, tot.lists, tot.draws_split);
    free(a); free(bb);
    ID3D12Device_Release(dev);
}

/* Command lists that fail to reset/close (injected; the list is left open, so submitting it removes the
 * device): a failed worker chunk is recorded again on the render thread and the frame stays pixel-exact; one
 * that cannot be is left out (draws only). A continuation that fails to close loses the whole batch -- not
 * one list of it is executed -- and calls the fatal handler (the game aborts with a crash report; the test
 * carries on to check nothing was submitted). Either way the replay drops to one list, identical frames after. */
static int g_rp_fatal;
static void rp_fatal_hook(void* ctx, const char* why) { (void)ctx; (void)why; g_rp_fatal++; }   /* the game aborts here */
static void replay_fault_test(const char* agility)
{
    int debug = 0;
    ID3D12Device* dev = make_device(agility, &debug);
    if (!dev) { printf("  no WARP device -- skipped\n"); return; }
    RW w;
    if (!rw_init(&w, dev)) { CHECK(0, "fault WARP setup failed"); return; }
    const size_t nb = (size_t)R_NRT * R_RT * R_RT * 4;
    uint8_t* a = (uint8_t*)malloc(nb); uint8_t* bb = (uint8_t*)malloc(nb);
    static const struct { const char* name; int k1; long at1, n1; int k2; long at2, n2; int drop, lost, gives_up; } k_sc[] = {
        { "chunk reset",            RSX_MTR_RP_F_CHUNK_RESET, 2, 3, -1, 0, 0, 0, 0, 0 },
        { "chunk close",            RSX_MTR_RP_F_CHUNK_CLOSE, 2, 3, -1, 0, 0, 0, 0, 0 },
        { "continuation reset",     RSX_MTR_RP_F_CONT_RESET,  1, 1, -1, 0, 0, 0, 0, 1 },
        { "continuation close",     RSX_MTR_RP_F_CONT_CLOSE,  1, 1, -1, 0, 0, 0, 1, 1 },
        { "chunk + re-record reset", RSX_MTR_RP_F_CHUNK_RESET, 1, 1, RSX_MTR_RP_F_RERECORD, 0, 1, 1, 0, 1 },
    };
    unsigned fr = 0;
    rsx_mtr_configure(1, 2, 1, 1);
    CHECK(rsx_mtr_on(), "pool on");
    for (int sc = 0; sc < (int)(sizeof k_sc / sizeof k_sc[0]); sc++) {
        rsx_mtr_rp* r = rsx_mtr_rp_create(dev);
        if (!r) { CHECK(0, "rsx_mtr_rp_create"); break; }
        rsx_mtr_rp_set_fatal(r, rp_fatal_hook, NULL);
        g_rp_fatal = 0;
        rsx_mtr_rp_inject_fault(r, k_sc[sc].k1, k_sc[sc].at1, k_sc[sc].n1);
        if (k_sc[sc].k2 >= 0) rsx_mtr_rp_inject_fault(r, k_sc[sc].k2, k_sc[sc].at2, k_sc[sc].n2);
        int diff = 0, gpu_fail = 0, compared = 0, partial = 0;
        for (int f = 0; f < 5; f++) {
            const int n = rp_gen(g_ev, 0);
            rp_fill_cbs(&w, g_ev, n);
            const RpMode one = { 0, 0, 0 }, split = { 24, 1, 0 };
            const int was_failed = rsx_mtr_rp_failed(r);
            if (rp_frame(&w, r, g_ev, n, &one, fr++, a) < 0) { gpu_fail = 1; break; }
            rsx_mtr_rp_stats e0; rsx_mtr_rp_get_stats(r, &e0);
            if (rp_frame(&w, r, g_ev, n, &split, fr++, bb) < 0) { gpu_fail = 1; break; }
            rsx_mtr_rp_stats e1; rsx_mtr_rp_get_stats(r, &e1);
            if (e1.batches_lost != e0.batches_lost && e1.exec_lists != e0.exec_lists) partial = 1;
            if ((k_sc[sc].drop || k_sc[sc].lost) && !was_failed && rsx_mtr_rp_failed(r)) continue;   /* the frame that lost a list / the batch */
            compared++;
            if (memcmp(a, bb, nb)) diff++;
        }
        rsx_mtr_rp_stats st; rsx_mtr_rp_get_stats(r, &st);
        const unsigned long long want_errs = (unsigned long long)k_sc[sc].n1;
        CHECK(!gpu_fail, "%s: device removed -- a bad list was submitted", k_sc[sc].name);
        CHECK(!diff, "%s: %d of %d frames differ", k_sc[sc].name, diff, compared);
        CHECK(compared >= 4, "%s: only %d frames compared", k_sc[sc].name, compared);
        CHECK(st.list_errs == want_errs, "%s: %llu failed lists, want %llu", k_sc[sc].name, st.list_errs, want_errs);
        CHECK(st.dropped == (unsigned long long)k_sc[sc].drop, "%s: %llu lists left out", k_sc[sc].name, st.dropped);
        CHECK(st.batches_lost == (unsigned long long)k_sc[sc].lost, "%s: %llu batches not submitted", k_sc[sc].name, st.batches_lost);
        CHECK(!partial, "%s: part of a lost batch was executed", k_sc[sc].name);
        CHECK(g_rp_fatal == k_sc[sc].lost, "%s: fatal handler called %d times", k_sc[sc].name, g_rp_fatal);
        CHECK(st.rerecorded == (k_sc[sc].gives_up ? 0ull : want_errs), "%s: %llu re-recorded", k_sc[sc].name, st.rerecorded);
        CHECK(rsx_mtr_rp_failed(r) == k_sc[sc].gives_up, "%s: failed flag %d", k_sc[sc].name, rsx_mtr_rp_failed(r));
        printf("  %s: %llu lists failed, %llu recorded again, %llu left out, %llu batches not submitted, %d frames pixel-exact%s\n",
               k_sc[sc].name, st.list_errs, st.rerecorded, st.dropped, st.batches_lost, compared - diff,
               rsx_mtr_rp_failed(r) ? ", one list after" : "");
        ID3D12CommandQueue_Signal(w.q, w.fence, ++w.fv); rw_wait(&w, w.fv);
        rsx_mtr_rp_destroy(r);
    }
    rsx_mtr_shutdown();
    free(a); free(bb);
    ID3D12Device_Release(dev);
}

static void replay_timing(void)
{
    int debug = 0;
    ID3D12Device* dev = make_device(NULL, &debug);
    if (!dev) return;
    RW w;
    if (!rw_init(&w, dev)) return;
    rsx_mtr_configure(1, 0, 1, 1);
    rsx_mtr_on();
    rsx_mtr_rp* r = rsx_mtr_rp_create(dev);
    if (!r) return;
    const int n = rp_gen(g_ev, 1);
    rp_fill_cbs(&w, g_ev, n);
    int draws = 0;
    for (int i = 0; i < n; i++) draws += g_ev[i].k == EV_DRAW;
    enum { FR = 60 };
    for (int wk = 0; wk < 2; wk++) {
        const int work = wk ? 400 : 0;   /* 400 ns: the per-draw decisions that stay on the render thread */
        const RpMode one = { 0, 0, work }, split = { 128, 1, work };
        double t1 = 0, t2 = 0, s1 = 0, s2 = 0;
        unsigned fr = 0;
        for (int f = 0; f < FR; f++) {
            g_rp_submit_us = 0;
            const double a = rp_frame(&w, r, g_ev, n, &one, fr++, NULL);
            const double sa = g_rp_submit_us; g_rp_submit_us = 0;
            const double b = rp_frame(&w, r, g_ev, n, &split, fr++, NULL);
            if (f >= 10) { t1 += a; t2 += b; s1 += sa; s2 += g_rp_submit_us; }
        }
        const double s = 2600.0 / draws;
        printf("TIMING replay (%d draws/frame scaled to 2600%s): render thread %.0f us/frame one list -> %.0f us/frame split, %d workers"
               " (of which waiting for chunks %.0f -> %.0f); ExecuteCommandLists excluded\n",
               draws, work ? ", +400 ns/draw other work" : "", t1 / (FR - 10) * s - (work ? 2600 * 0.4 : 0),
               t2 / (FR - 10) * s - (work ? 2600 * 0.4 : 0), rsx_mtr_workers(), s1 / (FR - 10) * s, s2 / (FR - 10) * s);
    }
    ID3D12CommandQueue_Signal(w.q, w.fence, ++w.fv); rw_wait(&w, w.fv);
    rsx_mtr_rp_destroy(r);
    rsx_mtr_shutdown();
    ID3D12Device_Release(dev);
}

int main(int argc, char** argv)
{
    const char* agility = argc > 1 ? argv[1] : NULL;
    if (agility && GetFileAttributesA(agility) == INVALID_FILE_ATTRIBUTES) agility = NULL;
    const int timing = argc > 2 && !strcmp(argv[2], "timing");
    printf("queue:\n");
    queue_off_test();
    for (int t = 1; t <= 4; t++) queue_test(t);
    printf("memo:\n");
    for (int t = 1; t <= 4; t += 3) memo_test(t);
    printf("warp:\n");
    warp_test(agility, 3, agility ? 6 : 24);
    if (agility) warp_test(NULL, 2, 24);   /* also without the validation layer's serialisation */
    printf("replay:\n");
    replay_test(agility, agility ? 4 : 12);
    printf("replay faults:\n");
    replay_fault_test(agility);
    if (timing) {
        printf("timing:\n");
        csum_timing();
        srv_timing();
        replay_timing();
    }
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
