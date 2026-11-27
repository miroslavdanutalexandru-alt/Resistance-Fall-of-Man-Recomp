/* rsx_pso_cache.c -- persistent D3D12 pipeline library for the guest pipelines (see rsx_pso_cache.h). */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsx_pso_cache.h"
#include "../../../project/port_native_summary.h"   /* port_native_parse_bool: the shared switch rules */

/* ---- file format --------------------------------------------------------------------------------------
 *  0 magic "R1PSOLB1"   8 u32 version   12 u32 header bytes (64)   16 u64 GPU/driver hash   24 u64 build hash
 * 32 u64 adapter LUID (log only)   40 u64 payload bytes   48 u64 payload FNV-1a 64   56 u32 pipelines
 * 60 u32 header check (low 32 bits of FNV-1a 64 over bytes 0..59), then the ID3D12PipelineLibrary blob. */
static const char k_magic[8] = { 'R', '1', 'P', 'S', 'O', 'L', 'B', '1' };
#define PSOLIB_VERSION 1u

uint64_t rsx_pso_lib_hash(const void* p, size_t n, uint64_t h)
{
    const uint8_t* b = (const uint8_t*)p;
    if (!h) h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
static void put32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t* p, uint64_t v) { memcpy(p, &v, 8); }
static uint32_t get32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t get64(const uint8_t* p) { uint64_t v; memcpy(&v, p, 8); return v; }

const char* rsx_pso_lib_verdict_text(int v)
{
    switch (v) {
    case RSX_PSOLIB_OK: return "valid";
    case RSX_PSOLIB_EMPTY: return "empty";
    case RSX_PSOLIB_CORRUPT: return "damaged or truncated";
    case RSX_PSOLIB_OTHER_GPU: return "built for another GPU or driver";
    case RSX_PSOLIB_OTHER_BUILD: return "written by another build of the game";
    }
    return "?";
}

static uint8_t* wrap_count(const void* payload, size_t payload_n, uint64_t gpu_hash, uint64_t build_hash,
                           uint64_t luid, uint32_t count, size_t* out_n)
{
    uint8_t* f = (uint8_t*)malloc(RSX_PSOLIB_HEADER_BYTES + payload_n);
    if (!f) return NULL;
    memcpy(f, k_magic, 8);
    put32(f + 8, PSOLIB_VERSION);
    put32(f + 12, RSX_PSOLIB_HEADER_BYTES);
    put64(f + 16, gpu_hash);
    put64(f + 24, build_hash);
    put64(f + 32, luid);
    put64(f + 40, (uint64_t)payload_n);
    put64(f + 48, rsx_pso_lib_hash(payload, payload_n, 0));
    put32(f + 56, count);
    put32(f + 60, (uint32_t)rsx_pso_lib_hash(f, 60, 0));
    if (payload_n) memcpy(f + RSX_PSOLIB_HEADER_BYTES, payload, payload_n);
    *out_n = RSX_PSOLIB_HEADER_BYTES + payload_n;
    return f;
}
uint8_t* rsx_pso_lib_wrap(const void* payload, size_t payload_n, uint64_t gpu_hash, uint64_t build_hash,
                          uint64_t luid, size_t* out_n)
{
    return wrap_count(payload, payload_n, gpu_hash, build_hash, luid, 0, out_n);
}

int rsx_pso_lib_check(const void* file, size_t n, uint64_t gpu_hash, uint64_t build_hash,
                      size_t* payload_off, size_t* payload_n)
{
    const uint8_t* f = (const uint8_t*)file;
    *payload_off = 0; *payload_n = 0;
    if (!f || !n) return RSX_PSOLIB_EMPTY;
    if (n < RSX_PSOLIB_HEADER_BYTES || memcmp(f, k_magic, 8) || get32(f + 8) != PSOLIB_VERSION ||
        get32(f + 12) != RSX_PSOLIB_HEADER_BYTES || get32(f + 60) != (uint32_t)rsx_pso_lib_hash(f, 60, 0))
        return RSX_PSOLIB_CORRUPT;
    const uint64_t pn = get64(f + 40);
    if (pn != (uint64_t)(n - RSX_PSOLIB_HEADER_BYTES) || get64(f + 48) != rsx_pso_lib_hash(f + RSX_PSOLIB_HEADER_BYTES, (size_t)pn, 0))
        return RSX_PSOLIB_CORRUPT;
    if (get64(f + 16) != gpu_hash) return RSX_PSOLIB_OTHER_GPU;
    if (get64(f + 24) != build_hash) return RSX_PSOLIB_OTHER_BUILD;
    *payload_off = RSX_PSOLIB_HEADER_BYTES;
    *payload_n = (size_t)pn;
    return RSX_PSOLIB_OK;
}

uint64_t rsx_pso_lib_build_hash(void)
{
    const uint8_t* base = (const uint8_t*)GetModuleHandleA(NULL);
    if (!base) return 0;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    uint32_t v[3] = { nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage, nt->OptionalHeader.CheckSum };
    return rsx_pso_lib_hash(v, sizeof v, 0);
}

/* ---- pipeline names: a hash of everything in the description but the root signature pointer ---------- */
typedef struct { uint64_t a, b; } NameHash;
static void nh(NameHash* h, const void* p, size_t n)
{
    const uint8_t* s = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) {
        h->a ^= s[i]; h->a *= 1099511628211ull;
        h->b ^= (uint8_t)(s[i] ^ 0x5Au); h->b *= 1099511628211ull;
    }
}
static void nh_u(NameHash* h, uint32_t v) { nh(h, &v, 4); }
static void nh_str(NameHash* h, const char* s) { if (s) nh(h, s, strlen(s) + 1); else nh_u(h, 0xFFFFFFFFu); }
static void nh_code(NameHash* h, D3D12_SHADER_BYTECODE c)
{
    nh_u(h, (uint32_t)c.BytecodeLength);
    if (c.pShaderBytecode && c.BytecodeLength) nh(h, c.pShaderBytecode, c.BytecodeLength);
}
static void nh_stencil(NameHash* h, const D3D12_DEPTH_STENCILOP_DESC* s)
{
    nh_u(h, s->StencilFailOp); nh_u(h, s->StencilDepthFailOp); nh_u(h, s->StencilPassOp); nh_u(h, s->StencilFunc);
}
static ID3D12RootSignature* volatile s_tag_rs;
static volatile uint32_t s_tag;
void rsx_pso_cache_root_tag(ID3D12RootSignature* rs, uint32_t tag) { s_tag = tag; s_tag_rs = rs; }
static void pso_name(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* d, WCHAR* name, uint64_t* stripe)
{
    NameHash h = { 1469598103934665603ull, 0x84222325CBF29CE4ull };
    if (s_tag && d->pRootSignature && d->pRootSignature == s_tag_rs) nh_u(&h, s_tag);
    nh_code(&h, d->VS); nh_code(&h, d->PS); nh_code(&h, d->DS); nh_code(&h, d->HS); nh_code(&h, d->GS);
    nh_u(&h, d->StreamOutput.NumEntries);
    for (UINT i = 0; d->StreamOutput.pSODeclaration && i < d->StreamOutput.NumEntries; i++) {
        const D3D12_SO_DECLARATION_ENTRY* e = &d->StreamOutput.pSODeclaration[i];
        nh_u(&h, e->Stream); nh_str(&h, e->SemanticName); nh_u(&h, e->SemanticIndex);
        nh_u(&h, e->StartComponent); nh_u(&h, e->ComponentCount); nh_u(&h, e->OutputSlot);
    }
    nh_u(&h, d->StreamOutput.NumStrides);
    if (d->StreamOutput.pBufferStrides && d->StreamOutput.NumStrides)
        nh(&h, d->StreamOutput.pBufferStrides, d->StreamOutput.NumStrides * sizeof(UINT));
    nh_u(&h, d->StreamOutput.RasterizedStream);
    nh_u(&h, d->BlendState.AlphaToCoverageEnable); nh_u(&h, d->BlendState.IndependentBlendEnable);
    for (int i = 0; i < 8; i++) {
        const D3D12_RENDER_TARGET_BLEND_DESC* b = &d->BlendState.RenderTarget[i];
        nh_u(&h, b->BlendEnable); nh_u(&h, b->LogicOpEnable); nh_u(&h, b->SrcBlend); nh_u(&h, b->DestBlend);
        nh_u(&h, b->BlendOp); nh_u(&h, b->SrcBlendAlpha); nh_u(&h, b->DestBlendAlpha); nh_u(&h, b->BlendOpAlpha);
        nh_u(&h, b->LogicOp); nh_u(&h, b->RenderTargetWriteMask);
    }
    nh_u(&h, d->SampleMask);
    nh(&h, &d->RasterizerState, sizeof d->RasterizerState);   /* 4-byte fields only: no padding */
    nh_u(&h, d->DepthStencilState.DepthEnable); nh_u(&h, d->DepthStencilState.DepthWriteMask);
    nh_u(&h, d->DepthStencilState.DepthFunc); nh_u(&h, d->DepthStencilState.StencilEnable);
    nh_u(&h, d->DepthStencilState.StencilReadMask); nh_u(&h, d->DepthStencilState.StencilWriteMask);
    nh_stencil(&h, &d->DepthStencilState.FrontFace); nh_stencil(&h, &d->DepthStencilState.BackFace);
    nh_u(&h, d->InputLayout.NumElements);
    for (UINT i = 0; d->InputLayout.pInputElementDescs && i < d->InputLayout.NumElements; i++) {
        const D3D12_INPUT_ELEMENT_DESC* e = &d->InputLayout.pInputElementDescs[i];
        nh_str(&h, e->SemanticName); nh_u(&h, e->SemanticIndex); nh_u(&h, e->Format); nh_u(&h, e->InputSlot);
        nh_u(&h, e->AlignedByteOffset); nh_u(&h, e->InputSlotClass); nh_u(&h, e->InstanceDataStepRate);
    }
    nh_u(&h, d->IBStripCutValue); nh_u(&h, d->PrimitiveTopologyType); nh_u(&h, d->NumRenderTargets);
    for (int i = 0; i < 8; i++) nh_u(&h, d->RTVFormats[i]);
    nh_u(&h, d->DSVFormat); nh_u(&h, d->SampleDesc.Count); nh_u(&h, d->SampleDesc.Quality);
    nh_u(&h, d->NodeMask); nh_u(&h, d->Flags);
    swprintf(name, 40, L"g%016llx%016llx", (unsigned long long)h.a, (unsigned long long)h.b);
    *stripe = h.a;
}

/* ---- the library ------------------------------------------------------------------------------------ */
/* Load / Store take s_lock shared, Serialize exclusive. ID3D12PipelineLibrary synchronises itself except
 * for two threads loading the same pipeline, hence the per-name stripes. */
#define STRIPES 64
static SRWLOCK s_lock = SRWLOCK_INIT;
static SRWLOCK s_stripe[STRIPES];
static ID3D12PipelineLibrary* volatile s_lib;
static uint8_t* s_file;                    /* the loaded file: the library reads its blob until released */
static char s_path[MAX_PATH];
static uint64_t s_gpu_hash, s_build_hash, s_luid;
static volatile LONG s_dirty, s_count, s_loaded, s_stored, s_created, s_stop, s_save_now;
static HANDLE s_wake, s_thread;
/* Held across serialize + write + rename: a save that finds nothing dirty (the exit-time save) waits here for one
 * in flight on the saver thread, so ExitProcess cannot kill it half-way and lose the newest pipelines. */
static SRWLOCK s_save_lock = SRWLOCK_INIT;
volatile LONG rsx_pso_cache_test_write_delay_ms;   /* tests only: widens the serialize -> rename window */

int rsx_pso_cache_active(void) { return s_lib != NULL; }
void rsx_pso_cache_stats(long* loaded, long* stored, long* created)
{
    if (loaded) *loaded = s_loaded;
    if (stored) *stored = s_stored;
    if (created) *created = s_created;
}

HRESULT rsx_pso_cache_create(ID3D12Device* dev, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd,
                             ID3D12PipelineState** out)
{
    *out = NULL;
    if (!s_lib) return dev->lpVtbl->CreateGraphicsPipelineState(dev, pd, &IID_ID3D12PipelineState, (void**)out);
    WCHAR name[40]; uint64_t sh;
    pso_name(pd, name, &sh);
    SRWLOCK* st = &s_stripe[sh % STRIPES];
    HRESULT hr = E_INVALIDARG;
    AcquireSRWLockShared(&s_lock);
    AcquireSRWLockExclusive(st);
    ID3D12PipelineLibrary* lib = s_lib;   /* read under the lock: shutdown clears it exclusively */
    if (lib) hr = lib->lpVtbl->LoadGraphicsPipeline(lib, name, pd, &IID_ID3D12PipelineState, (void**)out);
    ReleaseSRWLockExclusive(st);
    ReleaseSRWLockShared(&s_lock);
    if (SUCCEEDED(hr) && *out) { InterlockedIncrement(&s_loaded); return hr; }
    *out = NULL;
    hr = dev->lpVtbl->CreateGraphicsPipelineState(dev, pd, &IID_ID3D12PipelineState, (void**)out);
    if (FAILED(hr) || !*out) return hr;
    InterlockedIncrement(&s_created);
    AcquireSRWLockShared(&s_lock);
    AcquireSRWLockExclusive(st);
    lib = s_lib;
    const HRESULT hs = lib ? lib->lpVtbl->StorePipeline(lib, name, *out) : E_FAIL;   /* E_INVALIDARG: the name is taken */
    ReleaseSRWLockExclusive(st);
    ReleaseSRWLockShared(&s_lock);
    if (SUCCEEDED(hs)) { InterlockedIncrement(&s_stored); InterlockedIncrement(&s_count); InterlockedExchange(&s_dirty, 1); }
    return hr;
}

static double ms_since(LARGE_INTEGER q0)
{
    LARGE_INTEGER q1, qf; QueryPerformanceCounter(&q1); QueryPerformanceFrequency(&qf);
    return (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)qf.QuadPart;
}

int rsx_pso_cache_save(void)
{
    if (!s_lib || !s_path[0]) return 0;
    AcquireSRWLockExclusive(&s_save_lock);
    LARGE_INTEGER q0; QueryPerformanceCounter(&q0);
    AcquireSRWLockExclusive(&s_lock);
    ID3D12PipelineLibrary* lib = s_lib;
    if (!lib || !InterlockedExchange(&s_dirty, 0)) {
        ReleaseSRWLockExclusive(&s_lock);
        ReleaseSRWLockExclusive(&s_save_lock);
        return 0;
    }
    const SIZE_T n = lib->lpVtbl->GetSerializedSize(lib);
    void* blob = n ? malloc(n) : NULL;
    const HRESULT hr = blob ? lib->lpVtbl->Serialize(lib, blob, n) : E_OUTOFMEMORY;
    const uint32_t count = (uint32_t)s_count;
    ReleaseSRWLockExclusive(&s_lock);
    size_t fn = 0;
    uint8_t* f = SUCCEEDED(hr) ? wrap_count(blob, n, s_gpu_hash, s_build_hash, s_luid, count, &fn) : NULL;
    free(blob);
    if (rsx_pso_cache_test_write_delay_ms) Sleep((DWORD)rsx_pso_cache_test_write_delay_ms);
    int ok = 0;
    if (f) {
        char tmp[MAX_PATH + 8];
        snprintf(tmp, sizeof tmp, "%s.tmp", s_path);
        FILE* o = fopen(tmp, "wb");
        if (o) {
            ok = fwrite(f, 1, fn, o) == fn;
            ok = (fclose(o) == 0) && ok;
            ok = ok && MoveFileExA(tmp, s_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            if (!ok) DeleteFileA(tmp);
        }
        free(f);
    }
    if (ok) fprintf(stderr, "[shaders] pipeline library saved: %u pipelines, %.1f MB in %.0f ms (%s)\n",
                    count, (double)fn / 1048576.0, ms_since(q0), s_path);
    else { InterlockedExchange(&s_dirty, 1); fprintf(stderr, "[shaders] pipeline library: could not write %s (0x%08lX)\n", s_path, (unsigned long)hr); }
    ReleaseSRWLockExclusive(&s_save_lock);
    return ok;
}

void rsx_pso_cache_request_save(void)
{
    InterlockedExchange(&s_save_now, 1);
    if (s_wake) SetEvent(s_wake);
}

static DWORD WINAPI saver_thread(LPVOID unused)
{
    (void)unused;
    ULONGLONG last = GetTickCount64();
    while (!s_stop) {
        WaitForSingleObject(s_wake, 5000);
        if (s_stop) break;
        const ULONGLONG now = GetTickCount64();
        if (s_dirty && (InterlockedExchange(&s_save_now, 0) || now - last >= 30000)) { rsx_pso_cache_save(); last = GetTickCount64(); }
    }
    return 0;
}

int rsx_pso_cache_init_ex(ID3D12Device* dev, const char* path, const char* gpu_stamp, uint64_t luid,
                          uint64_t build_hash, int saver)
{
    if (s_lib || !dev || !path) return 0;
    for (int i = 0; i < STRIPES; i++) InitializeSRWLock(&s_stripe[i]);
    if (!port_native_parse_bool(getenv("RSX_PSO_LIBRARY"), 1)) {
        fprintf(stderr, "[shaders] pipeline library: off (RSX_PSO_LIBRARY=0)\n");
        return 0;
    }
    ID3D12Device1* d1 = NULL;
    if (FAILED(dev->lpVtbl->QueryInterface(dev, &IID_ID3D12Device1, (void**)&d1)) || !d1) {
        fprintf(stderr, "[shaders] pipeline library: not available (no ID3D12Device1)\n");
        return 0;
    }
    D3D12_FEATURE_DATA_SHADER_CACHE sc = { 0 };
    if (FAILED(dev->lpVtbl->CheckFeatureSupport(dev, D3D12_FEATURE_SHADER_CACHE, &sc, sizeof sc)) ||
        !(sc.SupportFlags & D3D12_SHADER_CACHE_SUPPORT_LIBRARY)) {
        fprintf(stderr, "[shaders] pipeline library: not supported by this driver\n");
        d1->lpVtbl->Release(d1);
        return 0;
    }
    snprintf(s_path, sizeof s_path, "%s", path);
    s_gpu_hash = rsx_pso_lib_hash(gpu_stamp ? gpu_stamp : "", gpu_stamp ? strlen(gpu_stamp) : 0, 0);
    s_build_hash = build_hash;
    s_luid = luid;
    LARGE_INTEGER q0; QueryPerformanceCounter(&q0);
    size_t n = 0;
    uint8_t* file = NULL;
    FILE* f = fopen(path, "rb");
    if (f) {
        if (_fseeki64(f, 0, SEEK_END) == 0) {
            const long long len = _ftelli64(f);
            if (len > 0 && len < (2ll << 30) && _fseeki64(f, 0, SEEK_SET) == 0 && (file = (uint8_t*)malloc((size_t)len)) != NULL) {
                n = fread(file, 1, (size_t)len, f);
                if (n != (size_t)len) n = 0;
            }
        }
        fclose(f);
    }
    size_t off = 0, pn = 0;
    const int v = rsx_pso_lib_check(file, n, s_gpu_hash, s_build_hash, &off, &pn);
    ID3D12PipelineLibrary* lib = NULL;
    HRESULT hr = E_FAIL;
    if (v == RSX_PSOLIB_OK && pn) {
        hr = d1->lpVtbl->CreatePipelineLibrary(d1, file + off, pn, &IID_ID3D12PipelineLibrary, (void**)&lib);
        if (FAILED(hr) || !lib) {
            lib = NULL;
            fprintf(stderr, "[shaders] pipeline library: %s rejected by the driver (0x%08lX%s), starting empty\n", path,
                    (unsigned long)hr, hr == D3D12_ERROR_DRIVER_VERSION_MISMATCH ? ", driver changed"
                                       : hr == D3D12_ERROR_ADAPTER_NOT_FOUND ? ", another GPU" : "");
        }
    } else if (v != RSX_PSOLIB_EMPTY && v != RSX_PSOLIB_OK)
        fprintf(stderr, "[shaders] pipeline library: %s is %s, starting empty\n", path, rsx_pso_lib_verdict_text(v));
    if (lib) {
        s_file = file;   /* stays alive with the library */
        s_count = (LONG)get32(file + 56);
        fprintf(stderr, "[shaders] pipeline library: %s, %ld pipelines (%.1f MB) opened in %.0f ms\n", path, s_count,
                (double)pn / 1048576.0, ms_since(q0));
    } else {
        free(file);
        s_count = 0;
        hr = d1->lpVtbl->CreatePipelineLibrary(d1, NULL, 0, &IID_ID3D12PipelineLibrary, (void**)&lib);
        if (FAILED(hr) || !lib) {
            d1->lpVtbl->Release(d1);
            fprintf(stderr, "[shaders] pipeline library: CreatePipelineLibrary failed (0x%08lX), not used\n", (unsigned long)hr);
            return 0;
        }
    }
    d1->lpVtbl->Release(d1);
    s_lib = lib;
    if (saver) {
        s_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
        s_thread = s_wake ? CreateThread(NULL, 256u << 10, saver_thread, NULL, 0, NULL) : NULL;
        if (s_thread) SetThreadPriority(s_thread, THREAD_PRIORITY_BELOW_NORMAL);
    }
    return (int)s_count;
}

int rsx_pso_cache_init(ID3D12Device* dev, const char* path, const char* gpu_stamp, uint64_t luid)
{
    return rsx_pso_cache_init_ex(dev, path, gpu_stamp, luid, rsx_pso_lib_build_hash(), 1);
}

void rsx_pso_cache_shutdown(void)
{
    if (!s_lib) return;
    InterlockedExchange(&s_stop, 1);
    if (s_thread) { SetEvent(s_wake); WaitForSingleObject(s_thread, 10000); CloseHandle(s_thread); s_thread = NULL; }
    if (s_wake) { CloseHandle(s_wake); s_wake = NULL; }
    rsx_pso_cache_save();
    AcquireSRWLockExclusive(&s_lock);
    ID3D12PipelineLibrary* lib = s_lib;
    s_lib = NULL;
    ReleaseSRWLockExclusive(&s_lock);
    lib->lpVtbl->Release(lib);
    free(s_file); s_file = NULL;
    s_dirty = s_count = s_loaded = s_stored = s_created = s_stop = s_save_now = 0;
}
#endif
