/* rsx_pso_cache.c (ID3D12PipelineLibrary, RSX_PSO_LIBRARY) on WARP: the file checks, a library that round-trips
 * through shadercache-style files, and damaged / truncated / mismatched / driver-rejected files that are
 * ignored (empty library, pipelines still created). Skipped (pass) without WARP or library support. */
#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "rsx_pso_cache.h"

static int g_checks, g_fail;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char* k_path = "pso_cache_test.bin";

static void test_file_checks(void)
{
    const uint8_t payload[100] = { 1, 2, 3, 4, 5 };
    size_t n = 0, off = 0, pn = 0;
    uint8_t* f = rsx_pso_lib_wrap(payload, sizeof payload, 11, 22, 33, &n);
    CHECK(f && n == RSX_PSOLIB_HEADER_BYTES + sizeof payload, "wrap size %zu", n);
    CHECK(rsx_pso_lib_check(f, n, 11, 22, &off, &pn) == RSX_PSOLIB_OK && off == RSX_PSOLIB_HEADER_BYTES &&
          pn == sizeof payload && !memcmp(f + off, payload, pn), "round trip");
    CHECK(rsx_pso_lib_check(f, n, 12, 22, &off, &pn) == RSX_PSOLIB_OTHER_GPU && pn == 0, "other GPU / driver");
    CHECK(rsx_pso_lib_check(f, n, 11, 23, &off, &pn) == RSX_PSOLIB_OTHER_BUILD && pn == 0, "other build");
    CHECK(rsx_pso_lib_check(NULL, 0, 11, 22, &off, &pn) == RSX_PSOLIB_EMPTY, "no file");
    CHECK(rsx_pso_lib_check(f, 0, 11, 22, &off, &pn) == RSX_PSOLIB_EMPTY, "empty file");
    for (size_t k = 1; k < n; k += 7)
        CHECK(rsx_pso_lib_check(f, k, 11, 22, &off, &pn) == RSX_PSOLIB_CORRUPT, "truncated to %zu", k);
    for (size_t k = 0; k < n; k++) {
        f[k] ^= 0x10;
        const int v = rsx_pso_lib_check(f, n, 11, 22, &off, &pn);
        /* the header check covers the GPU / build hashes too: a flip is damage, not a mismatch */
        CHECK(v == RSX_PSOLIB_CORRUPT, "bit flip at %zu: %s", k, rsx_pso_lib_verdict_text(v));
        f[k] ^= 0x10;
    }
    uint8_t* big = (uint8_t*)malloc(n + 1);
    memcpy(big, f, n); big[n] = 0;
    CHECK(rsx_pso_lib_check(big, n + 1, 11, 22, &off, &pn) == RSX_PSOLIB_CORRUPT, "trailing byte");
    free(big);
    free(f);
    f = rsx_pso_lib_wrap(NULL, 0, 11, 22, 33, &n);
    CHECK(f && rsx_pso_lib_check(f, n, 11, 22, &off, &pn) == RSX_PSOLIB_OK && pn == 0, "empty payload");
    free(f);
}

static ID3D12Device* warp_device(void)
{
    IDXGIFactory4* fac = NULL;
    IDXGIAdapter* ad = NULL;
    ID3D12Device* dev = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory4, (void**)&fac))) return NULL;
    if (SUCCEEDED(IDXGIFactory4_EnumWarpAdapter(fac, &IID_IDXGIAdapter, (void**)&ad)))
        if (FAILED(D3D12CreateDevice((IUnknown*)ad, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void**)&dev))) dev = NULL;
    if (ad) IDXGIAdapter_Release(ad);
    IDXGIFactory4_Release(fac);
    return dev;
}

static ID3DBlob* compile(const char* src, const char* target)
{
    ID3DBlob* code = NULL; ID3DBlob* err = NULL;
    if (FAILED(D3DCompile(src, strlen(src), "t", NULL, NULL, "main", target, 0, 0, &code, &err))) {
        if (err) { printf("%s\n", (const char*)ID3D10Blob_GetBufferPointer(err)); ID3D10Blob_Release(err); }
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

static ID3D12Device* g_dev;
static ID3D12RootSignature* g_rs;
static ID3DBlob *g_vs, *g_ps;
static D3D12_INPUT_ELEMENT_DESC g_il[1] = { { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 } };

/* variant v: three different pipelines (cull mode / blend / render target format) */
static void desc(D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, int v)
{
    memset(d, 0, sizeof *d);
    d->pRootSignature = g_rs;
    d->VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(g_vs); d->VS.BytecodeLength = ID3D10Blob_GetBufferSize(g_vs);
    d->PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(g_ps); d->PS.BytecodeLength = ID3D10Blob_GetBufferSize(g_ps);
    d->BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d->BlendState.RenderTarget[0].BlendEnable = v == 1;
    d->BlendState.RenderTarget[0].SrcBlend = d->BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_SRC_ALPHA;
    d->BlendState.RenderTarget[0].DestBlend = d->BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    d->BlendState.RenderTarget[0].BlendOp = d->BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    d->BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    d->SampleMask = 0xFFFFFFFFu;
    d->RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d->RasterizerState.CullMode = v == 0 ? D3D12_CULL_MODE_NONE : D3D12_CULL_MODE_BACK;
    d->RasterizerState.DepthClipEnable = TRUE;
    d->DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    d->InputLayout.pInputElementDescs = g_il; d->InputLayout.NumElements = 1;
    d->PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d->NumRenderTargets = 1;
    d->RTVFormats[0] = v == 2 ? DXGI_FORMAT_R16G16B16A16_FLOAT : v == 3 ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    d->SampleDesc.Count = 1;
}

static int create_all(int count)
{
    int ok = 0;
    for (int v = 0; v < count; v++) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d; desc(&d, v);
        ID3D12PipelineState* p = NULL;
        if (SUCCEEDED(rsx_pso_cache_create(g_dev, &d, &p)) && p) { ok++; ID3D12PipelineState_Release(p); }
    }
    return ok;
}

static size_t read_file(const char* path, uint8_t** out)
{
    *out = NULL;
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
    *out = (uint8_t*)malloc(n > 0 ? (size_t)n : 1);
    const size_t r = fread(*out, 1, (size_t)n, f);
    fclose(f);
    return r;
}
static void write_file(const char* path, const uint8_t* p, size_t n)
{
    FILE* f = fopen(path, "wb");
    if (f) { fwrite(p, 1, n, f); fclose(f); }
}

/* a damaged / mismatched file: no pipelines from it, the library still works and the file is left alone
 * until something new is stored */
static void reject_case(const char* what, const char* stamp, uint64_t build)
{
    long loaded = -1, stored = -1, created = -1;
    const int n = rsx_pso_cache_init_ex(g_dev, k_path, stamp, 0, build, 0);
    CHECK(n == 0, "%s: %d pipelines taken from the file", what, n);
    CHECK(rsx_pso_cache_active(), "%s: an empty library is used", what);
    CHECK(create_all(3) == 3, "%s: pipelines still created", what);
    rsx_pso_cache_stats(&loaded, &stored, &created);
    CHECK(loaded == 0 && created == 3 && stored == 3, "%s: loaded %ld created %ld stored %ld", what, loaded, created, stored);
    rsx_pso_cache_shutdown();
    CHECK(!rsx_pso_cache_active(), "%s: shut down", what);
}

static DWORD WINAPI saver_once(LPVOID unused) { (void)unused; rsx_pso_cache_save(); return 0; }

static void test_warp(void)
{
    g_dev = warp_device();
    if (!g_dev) { printf("SKIP: no WARP device\n"); return; }
    D3D12_FEATURE_DATA_SHADER_CACHE sc = { 0 };
    if (FAILED(ID3D12Device_CheckFeatureSupport(g_dev, D3D12_FEATURE_SHADER_CACHE, &sc, sizeof sc)) ||
        !(sc.SupportFlags & D3D12_SHADER_CACHE_SUPPORT_LIBRARY)) {
        printf("SKIP: WARP has no pipeline library support\n");
        ID3D12Device_Release(g_dev);
        return;
    }
    D3D12_ROOT_SIGNATURE_DESC rd = { 0 };
    rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob* rsb = NULL;
    CHECK(SUCCEEDED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, NULL)), "root signature");
    CHECK(SUCCEEDED(ID3D12Device_CreateRootSignature(g_dev, 0, ID3D10Blob_GetBufferPointer(rsb), ID3D10Blob_GetBufferSize(rsb),
                                                     &IID_ID3D12RootSignature, (void**)&g_rs)), "root signature");
    ID3D10Blob_Release(rsb);
    g_vs = compile("float4 main(float4 p : POSITION) : SV_Position { return p; }", "vs_5_0");
    g_ps = compile("float4 main() : SV_Target { return float4(1, 0.5, 0, 1); }", "ps_5_0");
    if (!g_rs || !g_vs || !g_ps) { CHECK(0, "shaders"); return; }
    DeleteFileA(k_path);
    long loaded, stored, created;

    /* 1: no file -> empty library, three pipelines created and stored, a repeat is loaded from it */
    CHECK(rsx_pso_cache_init_ex(g_dev, k_path, "warp A", 7, 0x1234, 0) == 0, "first run: nothing cached");
    CHECK(rsx_pso_cache_active(), "library on");
    CHECK(create_all(3) == 3, "three pipelines");
    CHECK(create_all(1) == 1, "repeat");
    rsx_pso_cache_stats(&loaded, &stored, &created);
    CHECK(created == 3 && stored == 3 && loaded == 1, "first run: created %ld stored %ld loaded %ld", created, stored, loaded);
    CHECK(rsx_pso_cache_save() == 1, "saved");
    CHECK(rsx_pso_cache_save() == 0, "nothing new: not saved again");
    rsx_pso_cache_shutdown();
    uint8_t* good = NULL;
    const size_t good_n = read_file(k_path, &good);
    CHECK(good_n > RSX_PSOLIB_HEADER_BYTES, "file written (%zu bytes)", good_n);

    /* 2: the next run loads all three from the file, creates none, writes nothing */
    CHECK(rsx_pso_cache_init_ex(g_dev, k_path, "warp A", 8 /* new LUID after a reboot */, 0x1234, 0) == 3, "second run: 3 cached");
    CHECK(create_all(3) == 3, "second run pipelines");
    rsx_pso_cache_stats(&loaded, &stored, &created);
    CHECK(loaded == 3 && created == 0 && stored == 0, "second run: loaded %ld created %ld stored %ld", loaded, created, stored);
    CHECK(rsx_pso_cache_save() == 0, "second run: nothing to save");
    rsx_pso_cache_shutdown();

    /* 3: rejected files */
    reject_case("other GPU / driver", "warp B", 0x1234);
    write_file(k_path, good, good_n);
    reject_case("other build", "warp A", 0x9999);
    write_file(k_path, good, good_n);
    good[good_n / 2] ^= 0x40;
    write_file(k_path, good, good_n);
    reject_case("flipped payload byte", "warp A", 0x1234);
    good[good_n / 2] ^= 0x40;
    write_file(k_path, good, good_n / 2);
    reject_case("truncated", "warp A", 0x1234);
    write_file(k_path, good, 10);
    reject_case("header cut", "warp A", 0x1234);
    {
        /* garbage with a valid header and checksum: the driver itself must reject it */
        uint8_t junk[4096];
        for (size_t i = 0; i < sizeof junk; i++) junk[i] = (uint8_t)(i * 131u + 7u);
        size_t fn = 0;
        const char* stamp = "warp A";
        uint8_t* f = rsx_pso_lib_wrap(junk, sizeof junk, rsx_pso_lib_hash(stamp, strlen(stamp), 0), 0x1234, 0, &fn);
        write_file(k_path, f, fn);
        free(f);
        reject_case("garbage blob", "warp A", 0x1234);
    }

    /* 4: the run after a GPU change rebuilt the library and its shutdown wrote it for the new GPU */
    write_file(k_path, good, good_n);
    reject_case("other GPU, then saved", "warp C", 0x1234);
    CHECK(rsx_pso_cache_init_ex(g_dev, k_path, "warp C", 0, 0x1234, 0) == 3, "shutdown saved the rebuilt library");
    rsx_pso_cache_shutdown();

    /* 4b: the exit-time save while the saver thread is half-way through a save (dirty already cleared, file not
     * yet renamed): it must wait for that save, so what is on disk when it returns -- ExitProcess comes next --
     * has every pipeline */
    {
        DeleteFileA(k_path);
        CHECK(rsx_pso_cache_init_ex(g_dev, k_path, "warp D", 0, 0x1234, 0) == 0, "4b: empty start");
        CHECK(create_all(4) == 4, "4b: four pipelines");
        InterlockedExchange(&rsx_pso_cache_test_write_delay_ms, 400);
        HANDLE t = CreateThread(NULL, 0, saver_once, NULL, 0, NULL);
        Sleep(100);   /* the thread is inside the save now */
        rsx_pso_cache_save();   /* the exit path */
        uint8_t* f = NULL;
        const size_t fn = read_file(k_path, &f);   /* what survives ExitProcess */
        size_t off = 0, pn = 0;
        const char* stamp = "warp D";
        CHECK(fn && rsx_pso_lib_check(f, fn, rsx_pso_lib_hash(stamp, strlen(stamp), 0), 0x1234, &off, &pn) == RSX_PSOLIB_OK,
              "4b: the library is on disk when the exit save returns (%zu bytes)", fn);
        uint32_t cnt = 0;
        if (fn >= RSX_PSOLIB_HEADER_BYTES) memcpy(&cnt, f + 56, 4);
        CHECK(cnt == 4, "4b: with all 4 pipelines (%u)", cnt);
        free(f);
        if (t) { WaitForSingleObject(t, 10000); CloseHandle(t); }
        InterlockedExchange(&rsx_pso_cache_test_write_delay_ms, 0);
        rsx_pso_cache_shutdown();
    }

    /* 5: RSX_PSO_LIBRARY=0 */
    _putenv("RSX_PSO_LIBRARY=off");
    CHECK(rsx_pso_cache_init_ex(g_dev, k_path, "warp C", 0, 0x1234, 0) == 0 && !rsx_pso_cache_active(), "switched off");
    CHECK(create_all(3) == 3, "plain creation when off");
    _putenv("RSX_PSO_LIBRARY=");

    free(good);
    DeleteFileA(k_path);
    ID3D10Blob_Release(g_vs); ID3D10Blob_Release(g_ps);
    ID3D12RootSignature_Release(g_rs);
    ID3D12Device_Release(g_dev);
}

int main(void)
{
    test_file_checks();
    test_warp();
    printf("test_pso_cache: %d checks, %d failed -- %s\n", g_checks, g_fail, g_fail ? "FAIL" : "PASS");
    return g_fail ? 1 : 0;
}
