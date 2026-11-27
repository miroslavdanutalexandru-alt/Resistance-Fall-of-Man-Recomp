// rfom_dlssg.dll: NVIDIA DLSS Frame Generation (DLSS-G, incl. DLSS 4 Multi Frame Generation) for the
// Resistance port, through NVIDIA Streamline (MIT, tools/streamline). The game loads it at run time
// (RFOM_DLSS_FG=2|3|4, RSX_FSR=2); without it, or anywhere DLSS-G is unsupported, frame generation stays off.
// Streamline is used with manual hooking (PreferenceFlags::eUseManualHooking): the renderer keeps its native
// D3D12 interfaces and only the hooked calls (CreateCommandQueue, swap-chain creation and the swap chain itself)
// go through Streamline proxies. Reflex markers and the Reflex sleep go through Streamline as well (DLSS-G
// requires Reflex), routed here from project/rfom_reflex.cpp.
// Built by build.bat: clang-cl /MT /LD rfom_dlssg.cpp /Itools\streamline\include (no import library: the
// Streamline interposer is loaded with LoadLibrary after its signature has been verified).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <mutex>
#include <string>

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
#include <sl_security.h>

#include "../ps3recomp/libs/video/rfom_fg_cam.h"   // rfom_fgpose_step (RFOM_DLSS_FG_REAL_CAMERA)

typedef void (*RfomFgLog)(const char* msg);

namespace {
constexpr char ProjectId[] = "6b3f9c2e-5a41-4d8e-9b7a-2f0e1c7d4a93";   // same as rfom_dlss.dll
constexpr char EngineVersion[] = "ResistanceFoM-ps3recomp";

RfomFgLog g_log;
HMODULE g_mod;
bool g_init;
std::wstring g_dir;
const sl::ViewportHandle g_vp(0u);
std::mutex g_tok_mu;
unsigned long long g_render_id;   // frame id of the RENDERSUBMIT_START marker (render thread)
sl::ReflexMode g_reflex_mode = sl::ReflexMode::eLowLatency;

PFun_slInit* p_slInit;
PFun_slShutdown* p_slShutdown;
PFun_slIsFeatureSupported* p_slIsFeatureSupported;
PFun_slSetTagForFrame* p_slSetTagForFrame;
PFun_slSetConstants* p_slSetConstants;
PFun_slUpgradeInterface* p_slUpgradeInterface;
PFun_slGetNativeInterface* p_slGetNativeInterface;
PFun_slGetFeatureFunction* p_slGetFeatureFunction;
PFun_slGetNewFrameToken* p_slGetNewFrameToken;
PFun_slSetD3DDevice* p_slSetD3DDevice;
PFun_slDLSSGSetOptions* p_slDLSSGSetOptions;
PFun_slDLSSGGetState* p_slDLSSGGetState;
PFun_slReflexSleep* p_slReflexSleep;
PFun_slReflexSetOptions* p_slReflexSetOptions;
PFun_slReflexGetState* p_slReflexGetState;
PFun_slPCLSetMarker* p_slPCLSetMarker;

void Log(const char* fmt, ...)
{
    if (!g_log) return;
    char buf[1024];
    va_list a; va_start(a, fmt); vsnprintf(buf, sizeof buf, fmt, a); va_end(a);
    g_log(buf);
}

const char* ResStr(sl::Result r)
{
    switch (r) {
    case sl::Result::eOk: return "ok";
    case sl::Result::eErrorDriverOutOfDate: return "the NVIDIA driver is too old";
    case sl::Result::eErrorOSOutOfDate: return "Windows is too old (needs Windows 10 20H1 or newer)";
    case sl::Result::eErrorOSDisabledHWS: return "hardware-accelerated GPU scheduling is off (Windows Settings > Display > Graphics)";
    case sl::Result::eErrorNoSupportedAdapterFound: return "no supported NVIDIA GPU (needs an RTX 40 or RTX 50 series)";
    case sl::Result::eErrorAdapterNotSupported: return "this GPU does not support DLSS Frame Generation (needs an RTX 40 or RTX 50 series)";
    case sl::Result::eErrorFeatureMissing: return "a Streamline plugin is missing";
    case sl::Result::eErrorFeatureNotSupported: return "not supported on this system";
    default: break;
    }
    static char b[32];
    snprintf(b, sizeof b, "error %d", (int)r);
    return b;
}

void SlLog(sl::LogType type, const char* msg)
{
    if (type == sl::LogType::eInfo || !msg) return;
    char b[512];
    size_t n = strlen(msg);
    while (n && (msg[n - 1] == '\n' || msg[n - 1] == '\r')) n--;
    if (n > sizeof b - 1) n = sizeof b - 1;
    memcpy(b, msg, n); b[n] = 0;
    // Some messages repeat every frame (RSYNC "Inconsistency detected" when the NVIDIA profile forces VSync:
    // ~3 per frame, tens of thousands per session). Compare them without the leading [time][streamline]
    // [level][tid][elapsed] tags; log the first, then one line per 1000 repeats or 10 s with the count.
    const char* key = b;
    while (*key == '[') { const char* e = strchr(key, ']'); if (!e) break; key = e + 1; }
    struct Rep { std::string key; unsigned long long last_ms; unsigned skipped, use; };
    static Rep reps[16];
    static unsigned use_clock;
    static std::mutex mu;
    unsigned skipped = 0;
    {
        std::lock_guard<std::mutex> lk(mu);
        const unsigned long long now = GetTickCount64();
        Rep* r = nullptr;
        Rep* lru = &reps[0];
        for (Rep& x : reps) {
            if (x.use && x.key == key) { r = &x; break; }
            if (x.use < lru->use) lru = &x;
        }
        if (r) {
            r->use = ++use_clock;
            if (r->skipped + 1 < 1000 && now - r->last_ms < 10000) { r->skipped++; return; }
            skipped = r->skipped;
            r->skipped = 0;
            r->last_ms = now;
        } else {
            lru->key = key; lru->last_ms = now; lru->skipped = 0; lru->use = ++use_clock;
        }
    }
    if (skipped)
        Log("Streamline %s: %s (%u identical messages not logged)", type == sl::LogType::eError ? "error" : "warning", b, skipped);
    else
        Log("Streamline %s: %s", type == sl::LogType::eError ? "error" : "warning", b);
}

sl::FrameToken* Token(unsigned long long id)
{
    if (!g_init || !p_slGetNewFrameToken) return nullptr;
    const uint32_t idx = (uint32_t)id;
    sl::FrameToken* t = nullptr;
    std::lock_guard<std::mutex> lk(g_tok_mu);
    if (p_slGetNewFrameToken(t, &idx) != sl::Result::eOk) return nullptr;
    return t;
}

template <class T> bool GetFn(sl::Feature f, const char* name, T*& out)
{
    void* p = nullptr;
    if (p_slGetFeatureFunction(f, name, p) != sl::Result::eOk || !p) { Log("missing Streamline function %s", name); return false; }
    out = reinterpret_cast<T*>(p);
    return true;
}

// NGX snippets (nvngx_dlssg.dll) carry only NVIDIA's ordinary Authenticode signature, not the secondary
// Streamline signature sl::security::verifyEmbeddedSignature requires of the sl.* plugins (which rejected
// every shipped nvngx_dlssg.dll). Check them the standard way: valid for the OS, signed by NVIDIA Corporation.
bool SignedByNvidiaAuthenticode(const wchar_t* path)
{
    WINTRUST_FILE_INFO fi{};
    fi.cbStruct = sizeof fi;
    fi.pcwszFilePath = path;
    WINTRUST_DATA wd{};
    wd.cbStruct = sizeof wd;
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    GUID act = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    bool ok = WinVerifyTrust(nullptr, &act, &wd) == ERROR_SUCCESS;
    if (ok) {
        wchar_t org[128] = L"";
        CRYPT_PROVIDER_DATA* pd = WTHelperProvDataFromStateData(wd.hWVTStateData);
        CRYPT_PROVIDER_SGNR* sg = pd ? WTHelperGetProvSignerFromChain(pd, 0, FALSE, 0) : nullptr;
        CRYPT_PROVIDER_CERT* pc = sg ? WTHelperGetProvCertFromChain(sg, 0) : nullptr;
        if (pc && pc->pCert)
            CertGetNameStringW(pc->pCert, CERT_NAME_ATTR_TYPE, 0, (void*)szOID_ORGANIZATION_NAME, org, 128);
        ok = wcscmp(org, L"NVIDIA Corporation") == 0;
    }
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &act, &wd);
    return ok;
}

void SetIdentity(sl::float4x4& m)
{
    for (uint32_t r = 0; r < 4; r++) m.setRow(r, sl::float4(r == 0, r == 1, r == 2, r == 3));
}
} // namespace

extern "C" {

// dir: folder holding sl.interposer.dll and the other Streamline / NGX DLLs.
// reflex: 1 Reflex On, 2 On + Boost. Returns 1 when DLSS Frame Generation can be used on this device.
__declspec(dllexport) int rfom_dlssg_init(ID3D12Device* dev, const wchar_t* dir, RfomFgLog log, int reflex)
{
    g_log = log;
    if (g_init) return 1;
    if (!dev) return 0;
    g_dir = dir ? dir : L".";
    const std::wstring interposer = g_dir + L"\\sl.interposer.dll";
    if (GetFileAttributesW(interposer.c_str()) == INVALID_FILE_ATTRIBUTES) { Log("sl.interposer.dll not found"); return 0; }
    static const wchar_t* const kSigned[] = { L"sl.interposer.dll", L"sl.common.dll", L"sl.dlss_g.dll", L"sl.reflex.dll",
                                             L"sl.pcl.dll", L"nvngx_dlssg.dll" };
    for (const wchar_t* f : kSigned) {
        const std::wstring p = g_dir + L"\\" + f;
        if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) { Log("%ls not found", f); return 0; }
        const bool ngx = wcsncmp(f, L"nvngx_", 6) == 0;
        if (!(ngx ? SignedByNvidiaAuthenticode(p.c_str()) : sl::security::verifyEmbeddedSignature(p.c_str()))) {
            Log("%ls is not signed by NVIDIA -- refusing to load it", f); return 0; }
    }
    g_mod = LoadLibraryW(interposer.c_str());
    if (!g_mod) { Log("cannot load sl.interposer.dll (error %lu)", GetLastError()); return 0; }
#define RFOM_SL_FN(n) p_##n = reinterpret_cast<PFun_##n*>(GetProcAddress(g_mod, #n)); if (!p_##n) { Log("sl.interposer.dll lacks " #n); return 0; }
    RFOM_SL_FN(slInit) RFOM_SL_FN(slShutdown) RFOM_SL_FN(slIsFeatureSupported) RFOM_SL_FN(slSetTagForFrame)
    RFOM_SL_FN(slSetConstants) RFOM_SL_FN(slUpgradeInterface) RFOM_SL_FN(slGetNativeInterface)
    RFOM_SL_FN(slGetFeatureFunction) RFOM_SL_FN(slGetNewFrameToken) RFOM_SL_FN(slSetD3DDevice)
#undef RFOM_SL_FN

    static const sl::Feature features[] = { sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };
    static const wchar_t* paths[1];
    paths[0] = g_dir.c_str();
    sl::Preferences pref{};
    pref.showConsole = false;
    { const char* e = getenv("RFOM_FG_SLLOG"); pref.logLevel = e && atoi(e) > 0 ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault; }
    pref.pathsToPlugins = paths;
    pref.numPathsToPlugins = 1;
    pref.logMessageCallback = SlLog;
    // no eAllowOTA / eLoadDownloadedPlugins: only the shipped, signature-checked plugins are used
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking |
                 sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = 3;
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = EngineVersion;
    pref.projectId = ProjectId;
    pref.renderAPI = sl::RenderAPI::eD3D12;
    sl::Result r = p_slInit(pref, sl::kSDKVersion);
    if (r != sl::Result::eOk) { Log("slInit failed: %s", ResStr(r)); return 0; }
    g_init = true;
    r = p_slSetD3DDevice(dev);
    if (r != sl::Result::eOk) { Log("slSetD3DDevice failed: %s", ResStr(r)); return 0; }

    LUID luid = dev->GetAdapterLuid();
    sl::AdapterInfo ai{};
    ai.deviceLUID = reinterpret_cast<uint8_t*>(&luid);
    ai.deviceLUIDSizeInBytes = sizeof luid;
    r = p_slIsFeatureSupported(sl::kFeatureDLSS_G, ai);
    if (r != sl::Result::eOk) { Log("DLSS Frame Generation unavailable: %s", ResStr(r)); return 0; }
    r = p_slIsFeatureSupported(sl::kFeatureReflex, ai);
    if (r != sl::Result::eOk) { Log("NVIDIA Reflex unavailable (%s); DLSS Frame Generation needs it", ResStr(r)); return 0; }
    if (!GetFn(sl::kFeatureDLSS_G, "slDLSSGSetOptions", p_slDLSSGSetOptions) ||
        !GetFn(sl::kFeatureDLSS_G, "slDLSSGGetState", p_slDLSSGGetState) ||
        !GetFn(sl::kFeatureReflex, "slReflexSleep", p_slReflexSleep) ||
        !GetFn(sl::kFeatureReflex, "slReflexSetOptions", p_slReflexSetOptions) ||
        !GetFn(sl::kFeatureReflex, "slReflexGetState", p_slReflexGetState) ||
        !GetFn(sl::kFeaturePCL, "slPCLSetMarker", p_slPCLSetMarker))
        return 0;
    sl::ReflexOptions ro{};
    g_reflex_mode = reflex >= 2 ? sl::ReflexMode::eLowLatencyWithBoost : sl::ReflexMode::eLowLatency;
    ro.mode = g_reflex_mode;
    ro.frameLimitUs = 0;                 // no limit until rfom_dlssg_set_limit (the game's frame cap, rfom_reflex.cpp)
    ro.useMarkersToOptimize = true;
    r = p_slReflexSetOptions(ro);
    if (r != sl::Result::eOk) { Log("slReflexSetOptions failed: %s", ResStr(r)); return 0; }
    sl::DLSSGOptions o{};
    o.mode = sl::DLSSGMode::eOff;
    // never free on off: re-allocation stutters. RFOM_DLSSG_INIT_RETAIN=0: old (flagless) init call.
    { const char* e = getenv("RFOM_DLSSG_INIT_RETAIN"); if (!(e && *e == '0')) o.flags = sl::DLSSGFlags::eRetainResourcesWhenOff; }
    p_slDLSSGSetOptions(g_vp, o);
    return 1;
}

// slUpgradeInterface: *iface (a native IDXGIFactory / ID3D12Device) becomes its Streamline proxy.
__declspec(dllexport) int rfom_dlssg_upgrade(void** iface)
{
    if (!g_init || !iface || !*iface) return 0;
    return p_slUpgradeInterface(iface) == sl::Result::eOk;
}

// The renderer's command queue, created through the proxy device (the one hooked D3D12 call).
// *queue_for_sc is what the swap chain is created with, *queue_native what the renderer submits to.
__declspec(dllexport) int rfom_dlssg_create_queue(ID3D12Device* dev, const D3D12_COMMAND_QUEUE_DESC* desc,
                                                  void** proxy_dev_out, ID3D12CommandQueue** queue_for_sc,
                                                  ID3D12CommandQueue** queue_native)
{
    if (!g_init || !dev) return 0;
    dev->AddRef();
    void* pd = dev;
    if (p_slUpgradeInterface(&pd) != sl::Result::eOk || !pd) { dev->Release(); Log("slUpgradeInterface(device) failed"); return 0; }
    ID3D12Device* proxy = static_cast<ID3D12Device*>(pd);
    ID3D12CommandQueue* q = nullptr;
    if (FAILED(proxy->CreateCommandQueue(desc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&q))) || !q) {
        proxy->Release(); Log("CreateCommandQueue through Streamline failed"); return 0;
    }
    void* nq = nullptr;
    if (p_slGetNativeInterface(q, &nq) == sl::Result::eOk && nq) {
        // slGetNativeInterface adds a reference to the native queue
    } else {
        q->AddRef(); nq = q;
    }
    *proxy_dev_out = proxy;
    *queue_for_sc = q;
    *queue_native = static_cast<ID3D12CommandQueue*>(nq);
    return 1;
}

// Latency markers (rfom_reflex.cpp's NVAPI marker ids: SIMULATION_START 0 .. PRESENT_END 5; INPUT_SAMPLE 6 is
// deprecated in Streamline and dropped). The render side's frame id is remembered for the tags.
__declspec(dllexport) void rfom_dlssg_marker(int type, unsigned long long id)
{
    if (!g_init || !p_slPCLSetMarker || !id || type < 0 || type > 5) return;
    if (type == 2) g_render_id = id;
    sl::FrameToken* t = Token(id);
    if (t) p_slPCLSetMarker(static_cast<sl::PCLMarker>(type), *t);
}

// Game thread, after the port's frame limiter: the Reflex sleep for frame `id` (the one about to be simulated).
__declspec(dllexport) void rfom_dlssg_sleep(unsigned long long id)
{
    if (!g_init || !p_slReflexSleep) return;
    sl::FrameToken* t = Token(id);
    if (t) p_slReflexSleep(*t);
}

// Game thread (rfom_reflex.cpp, before the sleep): Reflex's frame limit in microseconds per rendered frame
// (before frame generation), 0 = no limit. Returns 1 when Streamline accepted it.
// RFOM_REFLEX_FG_DIV=1 (default 0 = old behaviour): while generation is on, Reflex gets the limit for
// *presented* frames, us / (generated + 1). Measured (r1_fmd_run, r1_fmd2_run, 0.3.1 MVQ2 run): with
// frameLimitUs = 16667 and 2x generation the rendered rate locks at exactly 30.0 fps (33.33 ms, worst 33.7)
// against ~58 fps with generation off -- the limit paces the output, so the real frames halve and every
// generated frame spans twice the motion. The port's own limiter still caps rendered frames (stubs.cpp).
static std::mutex g_lim_mu;
static unsigned g_lim_base = 0, g_lim_sent = ~0u;
static int g_lim_gen = 0, g_lim_div = -1;
static bool g_lim_have = false;   // rfom_reflex.cpp has asked for a limit
static int LimitApplyLocked()
{
    if (g_lim_div < 0) { const char* e = getenv("RFOM_REFLEX_FG_DIV"); g_lim_div = e && *e == '1'; }
    const unsigned us = g_lim_div && g_lim_gen > 0 && g_lim_base ? g_lim_base / (unsigned)(g_lim_gen + 1) : g_lim_base;
    if (us == g_lim_sent) return 1;
    sl::ReflexOptions ro{};
    ro.mode = g_reflex_mode;
    ro.frameLimitUs = us;
    ro.useMarkersToOptimize = true;
    const sl::Result r = p_slReflexSetOptions(ro);
    if (r != sl::Result::eOk) { Log("slReflexSetOptions (frame limit %u us) failed: %s", us, ResStr(r)); g_lim_sent = ~0u; return 0; }
    if (g_lim_div && us != g_lim_base)
        Log("Reflex frame limit %u us per presented frame (%u us per rendered frame, %dx generation; RFOM_REFLEX_FG_DIV=1)",
            us, g_lim_base, g_lim_gen + 1);
    else if (g_lim_div && g_lim_sent != ~0u)
        Log("Reflex frame limit %u us (generation off)", us);
    g_lim_sent = us;
    return 1;
}

__declspec(dllexport) int rfom_dlssg_set_limit(unsigned us)
{
    if (!g_init || !p_slReflexSetOptions) return 0;
    std::lock_guard<std::mutex> lk(g_lim_mu);
    g_lim_base = us;
    g_lim_have = true;
    return LimitApplyLocked();
}

// F3 overlay: average latency over the driver's last frames (same arithmetic as rfom_reflex.cpp).
__declspec(dllexport) int rfom_dlssg_latency(double* total_ms, double* render_ms, double* gpu_ms)
{
    if (!g_init || !p_slReflexGetState) return 0;
    static sl::ReflexState st;
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    st = sl::ReflexState{};
    if (p_slReflexGetState(st) != sl::Result::eOk || !st.latencyReportAvailable) return 0;
    double tot = 0, ren = 0, gpu = 0; int n = 0;
    for (const auto& f : st.frameReport) {
        if (!f.frameID || !f.simStartTime || f.gpuRenderEndTime <= f.simStartTime) continue;
        tot += (double)(f.gpuRenderEndTime - f.simStartTime) / 1000.0;
        if (f.renderSubmitEndTime > f.renderSubmitStartTime) ren += (double)(f.renderSubmitEndTime - f.renderSubmitStartTime) / 1000.0;
        gpu += f.gpuActiveRenderTimeUs / 1000.0;
        n++;
    }
    if (!n) return 0;
    *total_ms = tot / n; *render_ms = ren / n; *gpu_ms = gpu / n;
    return 1;
}

// Render thread, right after the DLSS Super Resolution dispatch: the frame's camera constants plus the DLSS
// depth / motion-vector inputs (render resolution rw x rh, top-left of the surfaces; motion vectors in UV units,
// current -> previous, camera motion included). There are no camera matrices in a recompiled PS3 renderer:
// the projection is a synthetic perspective one (DLSS-G uses the matrices only for its depth-based
// disocclusion heuristics; the motion vectors carry the real motion).
// Render thread, right before rfom_dlssg_tag: the frame's camera (rsx_fsr.inc cam_solve through rfom_fg_cam.h):
// Streamline's row-vector clipToPrevClip / prevClipToClip and the real near / far / vertical fov. Used by the next
// tag only; a frame without it gets the identity (camera never moves) and the placeholder projection.
namespace { bool g_cam_set; float g_cam_c2p[16], g_cam_p2c[16], g_cam_n, g_cam_f, g_cam_fov; }
__declspec(dllexport) void rfom_dlssg_camera(const float* c2p, const float* p2c, float n, float f, float fov)
{
    if (!c2p || !p2c) return;
    memcpy(g_cam_c2p, c2p, sizeof g_cam_c2p);
    memcpy(g_cam_p2c, p2c, sizeof g_cam_p2c);
    g_cam_n = n; g_cam_f = f; g_cam_fov = fov;
    g_cam_set = true;
}

__declspec(dllexport) int rfom_dlssg_tag(ID3D12GraphicsCommandList* cl, ID3D12Resource* depth, unsigned dstate,
                                         ID3D12Resource* mv, unsigned mstate, unsigned rw, unsigned rh,
                                         float jx, float jy, int reset)
{
    if (!g_init || !cl || !depth || !mv || !rw || !rh) return 0;
    sl::FrameToken* t = Token(g_render_id);
    if (!t) return 0;
    const bool cam = g_cam_set && g_cam_n > 0.0f && g_cam_f > g_cam_n && g_cam_fov > 0.0f && g_cam_fov < 3.1f;
    g_cam_set = false;
    const float n = cam ? g_cam_n : 0.1f, f = cam ? g_cam_f : 1000.0f, fov = cam ? g_cam_fov : 1.0f, asp = (float)rw / (float)rh;
    const float ys = 1.0f / tanf(fov * 0.5f), xs = ys / asp;
    sl::Constants c{};
    for (uint32_t r = 0; r < 4; r++) c.cameraViewToClip.setRow(r, sl::float4(0, 0, 0, 0));
    c.cameraViewToClip[0].x = xs;
    c.cameraViewToClip[1].y = ys;
    c.cameraViewToClip[2].z = f / (f - n);
    c.cameraViewToClip[2].w = 1.0f;
    c.cameraViewToClip[3].z = -n * f / (f - n);
    for (uint32_t r = 0; r < 4; r++) c.clipToCameraView.setRow(r, sl::float4(0, 0, 0, 0));
    c.clipToCameraView[0].x = 1.0f / xs;
    c.clipToCameraView[1].y = 1.0f / ys;
    c.clipToCameraView[2].w = (f - n) / (-n * f);
    c.clipToCameraView[3].z = 1.0f;
    c.clipToCameraView[3].w = 1.0f / n;
    SetIdentity(c.clipToLensClip);
    SetIdentity(c.clipToPrevClip);
    SetIdentity(c.prevClipToClip);
    if (cam)
        for (uint32_t r = 0; r < 4; r++) {
            c.clipToPrevClip.setRow(r, sl::float4(g_cam_c2p[r * 4], g_cam_c2p[r * 4 + 1], g_cam_c2p[r * 4 + 2], g_cam_c2p[r * 4 + 3]));
            c.prevClipToClip.setRow(r, sl::float4(g_cam_p2c[r * 4], g_cam_p2c[r * 4 + 1], g_cam_p2c[r * 4 + 2], g_cam_p2c[r * 4 + 3]));
        }
    c.jitterOffset = sl::float2(jx, jy);
    c.mvecScale = sl::float2(1.0f, 1.0f);
    c.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    c.cameraPos = sl::float3(0.0f, 0.0f, 0.0f);
    c.cameraUp = sl::float3(0.0f, 1.0f, 0.0f);
    c.cameraRight = sl::float3(1.0f, 0.0f, 0.0f);
    c.cameraFwd = sl::float3(0.0f, 0.0f, 1.0f);
    // RFOM_DLSS_FG_REAL_CAMERA (default 1; 0 = fixed): position / axes accumulated from the solved camera motion (rfom_fgpose_step)
    // instead of a camera that never moves; DLSS-G's scene change detection and disocclusion heuristics read them
    static const bool pose_on = [] { const char* e = getenv("RFOM_DLSS_FG_REAL_CAMERA"); return !(e && *e && atoi(e) <= 0); }();
    static rfom_fgpose_t pose{};
    if (pose_on) {
        if (cam) rfom_fgpose_step(&pose, g_cam_c2p, n, f, xs, ys);
        if (pose.init) {
            c.cameraPos = sl::float3((float)pose.pos[0], (float)pose.pos[1], (float)pose.pos[2]);
            c.cameraUp = sl::float3((float)pose.up[0], (float)pose.up[1], (float)pose.up[2]);
            c.cameraRight = sl::float3((float)pose.right[0], (float)pose.right[1], (float)pose.right[2]);
            c.cameraFwd = sl::float3((float)pose.fwd[0], (float)pose.fwd[1], (float)pose.fwd[2]);
        }
    }
    c.cameraNear = n;
    c.cameraFar = f;
    c.cameraFOV = fov;
    c.cameraAspectRatio = asp;
    c.motionVectorsInvalidValue = 0.0f;
    c.depthInverted = sl::Boolean::eFalse;
    c.cameraMotionIncluded = sl::Boolean::eTrue;
    c.motionVectors3D = sl::Boolean::eFalse;
    c.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    c.orthographicProjection = sl::Boolean::eFalse;
    c.motionVectorsDilated = sl::Boolean::eFalse;
    c.motionVectorsJittered = sl::Boolean::eFalse;
    if (p_slSetConstants(c, *t, g_vp) != sl::Result::eOk) return 0;
    // eOnlyValidNow: Streamline copies them here, on cl -- the surfaces are reused before the present
    sl::Resource dr(sl::ResourceType::eTex2d, depth, dstate);
    sl::Resource mr(sl::ResourceType::eTex2d, mv, mstate);
    sl::Extent ex{};
    ex.top = 0; ex.left = 0; ex.width = rw; ex.height = rh;
    const sl::ResourceTag tags[2] = {
        sl::ResourceTag(&dr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &ex),
        sl::ResourceTag(&mr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &ex),
    };
    return p_slSetTagForFrame(*t, g_vp, tags, 2, reinterpret_cast<sl::CommandBuffer*>(cl)) == sl::Result::eOk;
}

// Present time: the backbuffer sub-rectangle the game image occupies (letterbox) and, when the renderer could
// separate it, the HUD-less scene and the premultiplied UI (both swap-chain sized, in `state` until present).
// hudless / ui NULL: only the sub-rectangle (generated frames then interpolate the HUD with the scene).
__declspec(dllexport) int rfom_dlssg_present_tags(ID3D12GraphicsCommandList* cl, ID3D12Resource* hudless,
                                                  ID3D12Resource* ui, unsigned state,
                                                  unsigned left, unsigned top, unsigned w, unsigned h)
{
    if (!g_init || !cl || !w || !h) return 0;
    sl::FrameToken* t = Token(g_render_id);
    if (!t) return 0;
    sl::Extent ex{};
    ex.top = top; ex.left = left; ex.width = w; ex.height = h;
    sl::Resource hr(sl::ResourceType::eTex2d, hudless, state);
    sl::Resource ur(sl::ResourceType::eTex2d, ui, state);
    sl::ResourceTag tags[3] = {
        sl::ResourceTag(nullptr, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent, &ex),
        sl::ResourceTag(&hr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &ex),
        sl::ResourceTag(&ur, sl::kBufferTypeUIColorAndAlpha, sl::ResourceLifecycle::eValidUntilPresent, &ex),
    };
    const uint32_t n = hudless && ui ? 3u : 1u;   // tags are per frame: an untagged buffer is simply absent
    return p_slSetTagForFrame(*t, g_vp, tags, n, reinterpret_cast<sl::CommandBuffer*>(cl)) == sl::Result::eOk;
}

// Frames without valid depth / motion vectors (video cutscenes, loading screens, menus without a 3D scene):
// the DLSS-G guide asks for all tags to be set to null pointers rather than left at an older frame's.
__declspec(dllexport) int rfom_dlssg_null_tags(ID3D12GraphicsCommandList* cl)
{
    if (!g_init || !cl) return 0;
    sl::FrameToken* t = Token(g_render_id);
    if (!t) return 0;
    const sl::ResourceTag tags[4] = {
        sl::ResourceTag(nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent),
        sl::ResourceTag(nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent),
        sl::ResourceTag(nullptr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent),
        sl::ResourceTag(nullptr, sl::kBufferTypeUIColorAndAlpha, sl::ResourceLifecycle::eValidUntilPresent),
    };
    return p_slSetTagForFrame(*t, g_vp, tags, 4, reinterpret_cast<sl::CommandBuffer*>(cl)) == sl::Result::eOk;
}

// on: 0 off (resources kept), else frames to generate per rendered frame (1 = 2x, 2 = 3x, 3 = 4x).
__declspec(dllexport) int rfom_dlssg_set_options(int gen, int ui_recomp, unsigned scw, unsigned sch, unsigned scfmt)
{
    if (!g_init || !p_slDLSSGSetOptions) return 0;
    sl::DLSSGOptions o{};
    o.mode = gen > 0 ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
    o.numFramesToGenerate = gen > 0 ? (uint32_t)gen : 1u;
    // RFOM_DLSSG_MENU_DETECT (default auto: off while the HUD split keeps the UI out (ui_recomp), on otherwise --
    // HDR or RFOM_FG_HUDSPLIT=0; 0 / 1 force it): DLSS-G's own full-screen menu detection. R1 r1_1448 / r1_1510:
    // every 105-175 ms gameplay hitch (in off/on pairs ~1 s apart) is the render thread sleeping inside Streamline
    // with a "setFlipConfig FC feedback" warning on the same frame, while the port never toggled generation --
    // the detector switching generation off and on in combat. The pause menu is UI, which the HUD split should
    // keep out of generated frames; menus without a 3D scene get no DLSS inputs (generation off).
    static int forced = -2, last = -1;
    if (forced == -2) { const char* e = getenv("RFOM_DLSSG_MENU_DETECT"); forced = e && *e == '1' ? 1 : e && *e == '0' ? 0 : -1; }
    const int menu = forced >= 0 ? forced : !ui_recomp;
    if (menu != last) {
        last = menu;
        Log("full-screen menu detection %s (%s)", menu ? "on" : "off",
            forced >= 0 ? "forced by RFOM_DLSSG_MENU_DETECT" : menu ? "auto: HUD split not active" : "auto: HUD split keeps the UI out");
    }
    o.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    if (menu) o.flags = o.flags | sl::DLSSGFlags::eEnableFullscreenMenuDetection;
    o.numBackBuffers = 2;
    o.colorWidth = scw;
    o.colorHeight = sch;
    o.colorBufferFormat = scfmt;
    o.enableUserInterfaceRecomposition = ui_recomp ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    const sl::Result r = p_slDLSSGSetOptions(g_vp, o);
    if (r != sl::Result::eOk) { Log("slDLSSGSetOptions failed: %s", ResStr(r)); return 0; }
    if (p_slReflexSetOptions) {
        std::lock_guard<std::mutex> lk(g_lim_mu);
        g_lim_gen = gen > 0 ? gen : 0;
        if (g_lim_have) LimitApplyLocked();
    }
    return 1;
}

// status: DLSSGStatus bits; max_gen: frames the GPU can generate (1 = 2x only, 3 = up to 4x);
// vsync_ok: DLSS-G can run with a vsync'd present; presented: frames presented per rendered frame.
__declspec(dllexport) int rfom_dlssg_state(unsigned* status, unsigned* max_gen, int* vsync_ok, unsigned* presented,
                                           unsigned long long* vram)
{
    if (!g_init || !p_slDLSSGGetState) return 0;
    sl::DLSSGState s{};
    if (p_slDLSSGGetState(g_vp, s, nullptr) != sl::Result::eOk) return 0;
    if (status) *status = (unsigned)s.status;
    if (max_gen) *max_gen = s.numFramesToGenerateMax;
    if (vsync_ok) *vsync_ok = s.bIsVsyncSupportAvailable == sl::Boolean::eTrue;
    if (presented) *presented = s.numFramesActuallyPresented;
    if (vram) *vram = s.estimatedVRAMUsageInBytes;
    return 1;
}

// Before the swap chain, queue and device are released.
__declspec(dllexport) void rfom_dlssg_shutdown(void)
{
    if (!g_init) return;
    if (p_slDLSSGSetOptions) { sl::DLSSGOptions o{}; o.mode = sl::DLSSGMode::eOff; p_slDLSSGSetOptions(g_vp, o); }
    { std::lock_guard<std::mutex> lk(g_lim_mu); g_lim_gen = 0; g_lim_sent = ~0u; }
    p_slShutdown();
    g_init = false;
}

} // extern "C"
