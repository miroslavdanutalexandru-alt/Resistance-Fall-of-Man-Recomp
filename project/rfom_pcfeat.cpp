// Resistance: Fall of Man PC port: performance overlay (F3, RFOM_OVERLAY) and toast
// notifications (screenshot saved, trophies). Both are painted with GDI on their own thread and
// drawn by the D3D12 renderer inside the swap chain (rsx_osd.inc), so they show in every display
// mode and in captures of the game window. RFOM_OSD=window (or a renderer without the OSD) uses
// small topmost layered Win32 windows over the game window's client area instead. Either way
// they are hidden while the game is not the foreground window and never take focus or input.
//
// RFOM_OVERLAY   unset: off, F3 toggles | 1: on at start, F3 toggles | 0: off and F3 does nothing
// RFOM_OVERLAY_POS  tl (default) | tr | bl | br  corner of the game window
// PORT_OVERLAY_GRAPH  1 (default): a frame-time graph (last 120 frames) under the text | 0: text only
// PORT_UI_SCALE   auto (by output height) | 75..200: size of the overlay, toasts and the status panel
//
// Lines: game fps and frame time (avg / worst over the last 0.5 s, from the game's frame
// limiter, stubs.cpp rfom_frame_wait), RSX frames per second, draws per frame and the average
// CPU time per draw (the D3D12 backend's RSX_DRAWPROF counters, switched on only while the
// overlay is shown; with RSX_DRAWPROF=1 the window is the log's 5 s one).
//
// Exports: rfpf_overlay_get / rfpf_overlay_set, rfpf_toast(title, text, badge),
// rfpf_trophy(line1, name, grade) (PS3-style pop-up in the toast queue, port_trophy_popup.h).
//
// rfpf_status(text): a centred status panel ("Compiling shaders N / M" with a progress bar) shown
// while the D3D12 backend builds every known pipeline at start-up (RSX_PSO_PRECOMPILE,
// rsx_d3d12_backend.c); NULL or "" hides it. Shown only after 300 ms, so a quick one does not flash.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <atomic>
#include <algorithm>
#include "ps3emu/port_pc_extras.h"   // frame-time ring, UI scale
#include "port_trophy_popup.h"

extern "C" void* g_kbm_hwnd;                                                        // cellPad.c
extern "C" void rsx_prof_enable(int on);                                            // rsx_d3d12_backend.c
extern "C" int rsx_prof_sample(double* fps, double* draws_per_frame, double* us_per_draw);
extern "C" int rfom_reflex_stats(double* total_ms, double* render_ms, double* gpu_ms);
extern "C" int rsx_osd_available(void);                                             // rsx_osd.inc
extern "C" void rsx_osd_set(int slot, const void* px, int w, int h, int pitch, int x, int y, float alpha);

namespace {

std::atomic<int> s_overlay{0};
std::atomic<int> s_overlay_allowed{1};
std::atomic<int> s_thread_started{0};

// Game frame times (QPC ticks between rfom_frame_wait calls)
std::mutex s_ft_mtx;
double s_ft_sum = 0, s_ft_max = 0; int s_ft_n = 0;
PcxFtRing s_ft_ring = {};            // F3 frame-time graph (PORT_OVERLAY_GRAPH), under s_ft_mtx
std::atomic<int> s_graph{1};
int s_ui_pct = 100;                  // PORT_UI_SCALE for the fixed-pixel panels, ui thread only
int U(int px) { return pcx_ui_px(px, s_ui_pct); }
LARGE_INTEGER s_qpf;

// trophy: a PS3-style pop-up (port_trophy_popup.h; title = its first line, text = the name, badge = the grade)
struct Toast { std::wstring title, text; char badge; bool trophy = false; };
std::mutex s_toast_mtx;
std::deque<Toast> s_toasts;
// Toasts are dequeued in push order: s_toast_pushed counts pushes, s_toast_shown dequeues (on screen).
unsigned long long s_toast_pushed = 0, s_toast_shown = 0;   // under s_toast_mtx

std::wstring widen(const char* s)
{
    if (!s || !*s) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    w.resize((size_t)n - 1);
    return w;
}

HWND game_hwnd() { return (HWND)g_kbm_hwnd; }

bool game_visible(RECT& client_screen)
{
    HWND g = game_hwnd();
    if (!g || !IsWindow(g) || IsIconic(g) || GetForegroundWindow() != g) return false;
    RECT rc;
    if (!GetClientRect(g, &rc) || rc.right <= 0 || rc.bottom <= 0) return false;
    POINT a{ rc.left, rc.top }, b{ rc.right, rc.bottom };
    ClientToScreen(g, &a); ClientToScreen(g, &b);
    client_screen = { a.x, a.y, b.x, b.y };
    return true;
}

// ---- overlay ----
HWND s_ov = nullptr, s_tw = nullptr, s_pw = nullptr;   // s_pw: trophy pop-up (per-pixel alpha)
HFONT s_font = nullptr, s_font_b = nullptr, s_font_badge = nullptr;
wchar_t s_ov_text[512] = L"";
int s_ov_w = 0, s_ov_h = 0;

void ov_measure()
{
    HDC dc = GetDC(s_ov);
    HGDIOBJ of = SelectObject(dc, s_font);
    RECT r{ 0, 0, 1000, 1000 };
    DrawTextW(dc, s_ov_text, -1, &r, DT_CALCRECT | DT_NOPREFIX);
    SelectObject(dc, of);
    ReleaseDC(s_ov, dc);
    s_ov_w = r.right + 16; s_ov_h = r.bottom + 10;
    if (s_graph) s_ov_h += U(48) + 6;
}

void ov_paint(HDC dc, const RECT& rc)
{
    HBRUSH bg = CreateSolidBrush(RGB(12, 14, 18));
    FillRect(dc, &rc, bg); DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(150, 255, 150));
    HGDIOBJ of = SelectObject(dc, s_font);
    RECT t{ 8, 5, rc.right - 4, rc.bottom - 2 };
    DrawTextW(dc, s_ov_text, -1, &t, DT_NOPREFIX);
    SelectObject(dc, of);
    if (!s_graph) return;
    // frame-time graph: one bar per frame, green / yellow (> 20 ms) / red (> 40 ms); lines at 16.7 and 33.3 ms
    PcxFtRing r;
    { std::lock_guard<std::mutex> lk(s_ft_mtx); r = s_ft_ring; }
    const int gh = U(48), gx = 8, gy = rc.bottom - gh - 6, gw = rc.right - 16;
    if (gw < 8 || gh < 8) return;
    HBRUSH gb = CreateSolidBrush(RGB(24, 28, 34));
    RECT g{ gx, gy, gx + gw, gy + gh };
    FillRect(dc, &g, gb); DeleteObject(gb);
    const float top = pcx_ft_ceiling(&r);
    HBRUSH ln = CreateSolidBrush(RGB(70, 76, 86));
    for (float m : { 16.7f, 33.3f }) {
        if (m >= top) continue;
        const int ly = gy + gh - (int)(gh * m / top);
        RECT l{ gx, ly, gx + gw, ly + 1 };
        FillRect(dc, &l, ln);
    }
    DeleteObject(ln);
    HBRUSH col[3] = { CreateSolidBrush(RGB(110, 220, 110)), CreateSolidBrush(RGB(235, 200, 70)), CreateSolidBrush(RGB(240, 80, 70)) };
    const unsigned n = r.count;
    for (unsigned i = 0; i < n; i++) {
        const float v = pcx_ft_at(&r, i);
        int bh = (int)(gh * std::min(v, top) / top + 0.5f);
        if (bh < 1) bh = 1;
        const int x0 = gx + (int)((long long)gw * (PCX_FT_N - n + i) / PCX_FT_N);
        const int x1 = std::max(x0 + 1, gx + (int)((long long)gw * (PCX_FT_N - n + i + 1) / PCX_FT_N));
        RECT b{ x0, gy + gh - bh, x1, gy + gh };
        FillRect(dc, &b, col[v > 40.0f ? 2 : v > 20.0f ? 1 : 0]);
    }
    for (HBRUSH b : col) DeleteObject(b);
}

LRESULT CALLBACK ov_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        ov_paint(dc, rc);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

// ---- start-up status panel (rfpf_status) ----
std::mutex s_st_mtx;
std::wstring s_st_text;
unsigned s_st_done = 0, s_st_total = 0;
ULONGLONG s_st_t0 = 0;
HWND s_sw = nullptr;
int SW_W = 440, SW_H = 78;   // times PORT_UI_SCALE (ui_thread)

LRESULT CALLBACK status_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(20, 22, 28));
        FillRect(dc, &rc, bg); DeleteObject(bg);
        std::wstring text; unsigned done, total;
        { std::lock_guard<std::mutex> lk(s_st_mtx); text = s_st_text; done = s_st_done; total = s_st_total; }
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ of = SelectObject(dc, s_font_b);
        SetTextColor(dc, RGB(235, 235, 240));
        // "title\nsecond line": the second line replaces the default one (the shader precompile's skip hint)
        const size_t brk = text.find(L'\n');
        const std::wstring line2 = brk == std::wstring::npos ? L"Preparing the graphics for this PC, the game starts when done"
                                                             : text.substr(brk + 1);
        if (brk != std::wstring::npos) text.resize(brk);
        RECT t1{ U(14), U(8), rc.right - U(14), U(30) };
        DrawTextW(dc, text.c_str(), -1, &t1, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, s_font);
        SetTextColor(dc, RGB(175, 180, 190));
        RECT t2{ U(14), U(32), rc.right - U(14), U(50) };
        DrawTextW(dc, line2.c_str(), -1, &t2, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        RECT bar{ U(14), U(56), rc.right - U(14), U(66) };
        HBRUSH back = CreateSolidBrush(RGB(50, 54, 64)); FillRect(dc, &bar, back); DeleteObject(back);
        if (total) {
            bar.right = bar.left + (LONG)((bar.right - bar.left) * (double)std::min(done, total) / total);
            HBRUSH fill = CreateSolidBrush(RGB(90, 160, 230)); FillRect(dc, &bar, fill); DeleteObject(fill);
        }
        SelectObject(dc, of);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

// The game window is shown (also while it is not yet the foreground window during start-up)
bool game_shown(RECT& client_screen)
{
    HWND g = game_hwnd();
    if (!g || !IsWindow(g) || !IsWindowVisible(g) || IsIconic(g)) return false;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg && fg != g) GetWindowThreadProcessId(fg, &pid);
    if (fg && fg != g && pid != GetCurrentProcessId()) return false;
    RECT rc;
    if (!GetClientRect(g, &rc) || rc.right <= 0 || rc.bottom <= 0) return false;
    POINT a{ rc.left, rc.top }, b{ rc.right, rc.bottom };
    ClientToScreen(g, &a); ClientToScreen(g, &b);
    client_screen = { a.x, a.y, b.x, b.y };
    return true;
}

// ---- toast ----
Toast s_cur; bool s_cur_on = false; ULONGLONG s_cur_t0 = 0, s_prev_now = 0;
int TW = 360, TH = 72;              // times PORT_UI_SCALE (ui_thread)

COLORREF badge_color(char b)
{
    switch (b) {
    case 'P': return RGB(170, 200, 235);
    case 'G': return RGB(230, 185, 60);
    case 'S': return RGB(190, 195, 205);
    case 'B': return RGB(200, 125, 70);
    default:  return RGB(90, 160, 230);
    }
}

void toast_paint(HDC dc, const RECT& rc)
{
    {
        HBRUSH bg = CreateSolidBrush(RGB(20, 22, 28));
        FillRect(dc, &rc, bg); DeleteObject(bg);
        const COLORREF bc = badge_color(s_cur.badge);
        HBRUSH bar = CreateSolidBrush(bc);
        RECT strip{ 0, 0, U(4), rc.bottom }; FillRect(dc, &strip, bar);
        // badge: a filled circle with the grade letter
        HGDIOBJ ob = SelectObject(dc, bar);
        HPEN pen = CreatePen(PS_SOLID, 1, bc); HGDIOBJ op = SelectObject(dc, pen);
        const int cy = rc.bottom / 2;
        Ellipse(dc, U(16), cy - U(20), U(56), cy + U(20));
        SelectObject(dc, op); DeleteObject(pen);
        SelectObject(dc, ob); DeleteObject(bar);
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ of = SelectObject(dc, s_font_badge);
        SetTextColor(dc, RGB(20, 22, 28));
        wchar_t letter[2] = { (wchar_t)(s_cur.badge ? s_cur.badge : L'i'), 0 };
        RECT br{ U(16), cy - U(20), U(56), cy + U(20) };
        DrawTextW(dc, letter, 1, &br, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, s_font_b);
        SetTextColor(dc, RGB(235, 235, 240));
        RECT t1{ U(68), U(10), rc.right - U(10), U(32) };
        DrawTextW(dc, s_cur.title.c_str(), -1, &t1, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, s_font);
        SetTextColor(dc, RGB(175, 180, 190));
        RECT t2{ U(68), U(34), rc.right - U(10), rc.bottom - U(6) };
        DrawTextW(dc, s_cur.text.c_str(), -1, &t2, DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, of);
    }
}

LRESULT CALLBACK toast_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        toast_paint(dc, rc);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

HWND make_layer(const wchar_t* cls, WNDPROC proc)
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = cls;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    HWND h = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                             cls, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, wc.hInstance, nullptr);
    if (h) SetLayeredWindowAttributes(h, 0, 220, LWA_ALPHA);
    return h;
}

void place(HWND h, int x, int y, int w, int hh)
{
    SetWindowPos(h, HWND_TOPMOST, x, y, w, hh, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void hide(HWND h) { if (IsWindowVisible(h)) ShowWindow(h, SW_HIDE); }

// ---- in-swapchain OSD (rsx_osd.inc): the same GDI painting into a DIB, drawn by the renderer ----
enum { OSD_TOAST = 0, OSD_OVERLAY = 1 };

// Trophy pop-up pixels of s_cur (straight alpha for the layered window, premultiplied for the OSD),
// rendered when it comes up; empty for plain toasts.
ptp::Image s_pop, s_pop_pm;

void render_trophy(int ch)
{
    ptp::Popup p;
    p.line1 = s_cur.title; p.name = s_cur.text; p.grade = s_cur.badge;
    const int pct = pcx_ui_scale_parse(getenv("PORT_UI_SCALE"));   // 0 = auto (by window height)
    if (!ptp::render(p, ptp::panel_height(ch, pct > 0 ? pct : 0), 768, s_pop)) { s_pop = s_pop_pm = ptp::Image(); return; }   // 768: OSD_MAX_W
    s_pop_pm = s_pop;
    ptp::premultiply(s_pop_pm);
}
const float PANEL_ALPHA = 220.0f / 255.0f;   // the layered windows' opacity

struct Dib {
    HDC dc = nullptr; HBITMAP bm = nullptr; HGDIOBJ old = nullptr; uint32_t* bits = nullptr; int w = 0, h = 0;
};
Dib s_dib_toast, s_dib_ov;

// Paint w x h with `paint` and publish it to an OSD slot. The panels are opaque rectangles, so the
// alpha GDI leaves undefined is forced to 255 (premultiplied = the colour itself).
void osd_publish(Dib& d, int slot, int w, int h, void (*paint)(HDC, const RECT&), int x, int y, float alpha)
{
    if (w <= 0 || h <= 0) return;
    if (w > d.w || h > d.h) {
        if (d.dc) { SelectObject(d.dc, d.old); DeleteObject(d.bm); DeleteDC(d.dc); d = Dib{}; }
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        d.dc = CreateCompatibleDC(nullptr);
        d.bm = d.dc ? CreateDIBSection(d.dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
        if (!d.bm) { if (d.dc) DeleteDC(d.dc); d = Dib{}; return; }
        d.old = SelectObject(d.dc, d.bm); d.bits = (uint32_t*)bits; d.w = w; d.h = h;
    }
    RECT rc{ 0, 0, w, h };
    paint(d.dc, rc);
    GdiFlush();
    for (int r = 0; r < h; r++) {
        uint32_t* p = d.bits + (size_t)r * d.w;
        for (int c = 0; c < w; c++) p[c] |= 0xFF000000u;
    }
    rsx_osd_set(slot, d.bits, w, h, d.w * 4, x, y, alpha);
}

void update_overlay_text(bool fresh)
{
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (!fresh && now - last < 500) return;
    const double dt = last ? (now - last) / 1000.0 : 0.5;
    last = now;
    double sum, mx; int n;
    {
        std::lock_guard<std::mutex> lk(s_ft_mtx);
        sum = s_ft_sum; mx = s_ft_max; n = s_ft_n;
        s_ft_sum = s_ft_max = 0; s_ft_n = 0;
    }
    std::wstring t;
    wchar_t b[160];
    if (n > 0) {
        const double avg = sum / n;
        swprintf(b, 160, L"Game  %5.1f fps   %5.2f ms (max %5.2f)", avg > 0 ? 1000.0 / avg : 0.0, avg, mx);
    } else swprintf(b, 160, L"Game  --");
    t += b;
    double fr = 0, dpf = 0, upd = 0;
    (void)dt;
    if (rsx_prof_sample(&fr, &dpf, &upd)) {
        swprintf(b, 160, L"\nRSX   %5.1f fps   %4.0f draws/frame   %5.2f us/draw", fr, dpf, upd);
        t += b;
    }
    {   // NVIDIA Reflex (rfom_reflex.cpp): PC latency from the driver's latency reports
        double lt = 0, lr = 0, lg = 0;
        if (rfom_reflex_stats(&lt, &lr, &lg)) {
            swprintf(b, 160, L"\nReflex  latency %5.1f ms   submit %5.2f ms   GPU %5.2f ms", lt, lr, lg);
            t += b;
        }
    }
    wcsncpy(s_ov_text, t.c_str(), 511);
    s_ov_text[511] = 0;
}

void ui_thread()
{
    QueryPerformanceFrequency(&s_qpf);
    while (!g_kbm_hwnd) Sleep(250);
    {   // PORT_UI_SCALE, fixed at start: the fonts and panel sizes below
        RECT gr{};
        int gh = GetClientRect(game_hwnd(), &gr) ? gr.bottom - gr.top : 0;
        if (gh < 1) gh = GetSystemMetrics(SM_CYSCREEN);
        s_ui_pct = pcx_ui_scale_pct(pcx_ui_scale_parse(getenv("PORT_UI_SCALE")), gh);
        TW = U(360); TH = U(72); SW_W = U(440); SW_H = U(78);
    }
    s_font = CreateFontW(-U(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    s_font_b = CreateFontW(-U(17), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    s_font_badge = CreateFontW(-U(22), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    s_ov = make_layer(L"RFOMPortPerfOverlay", ov_proc);
    s_sw = make_layer(L"RFOMPortShaderStatus", status_proc);
    s_tw = make_layer(L"RFOMPortToast", toast_proc);
    s_pw = ptp::ulw_create(L"RFOMPortTrophy");
    const char* pos = getenv("RFOM_OVERLAY_POS");
    const bool right = pos && (pos[1] == 'r' || pos[1] == 'R'), bottom = pos && (pos[0] == 'b' || pos[0] == 'B');
    int was_f3 = 0, prof_on = 0;
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        RECT cs{};
        const bool vis = game_visible(cs);
        const bool osd = rsx_osd_available() != 0;
        const int cw = cs.right - cs.left, ch = cs.bottom - cs.top;
        // F3 toggles (only while the game has focus)
        const int f3 = vis && s_overlay_allowed && (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
        if (f3 && !was_f3) s_overlay = !s_overlay;
        was_f3 = f3;
        if (s_overlay != prof_on) {
            prof_on = s_overlay;
            rsx_prof_enable(prof_on);
            if (prof_on) update_overlay_text(true);
        }
        if (vis && s_overlay && s_ov) {
            const ULONGLONG before = GetTickCount64();
            static ULONGLONG painted = 0;
            const bool fresh = before - painted >= (s_graph ? 100u : 500u);   // the graph scrolls at 10 Hz, the text every 0.5 s
            if (fresh) { update_overlay_text(false); ov_measure(); InvalidateRect(s_ov, nullptr, FALSE); painted = before; }
            if (osd) {
                hide(s_ov);
                const int x = right ? cw - s_ov_w - 8 : 8, y = bottom ? ch - s_ov_h - 8 : 8;
                if (fresh) osd_publish(s_dib_ov, OSD_OVERLAY, s_ov_w, s_ov_h, ov_paint, x, y, PANEL_ALPHA);
                else rsx_osd_set(OSD_OVERLAY, nullptr, 0, 0, 0, x, y, PANEL_ALPHA);
            } else {
                const int x = right ? cs.right - s_ov_w - 8 : cs.left + 8, y = bottom ? cs.bottom - s_ov_h - 8 : cs.top + 8;
                place(s_ov, x, y, s_ov_w, s_ov_h);
            }
        } else {
            if (s_ov) hide(s_ov);
            if (osd) rsx_osd_set(OSD_OVERLAY, nullptr, 0, 0, 0, 0, 0, 0.0f);
        }

        // start-up shader compile status, centred in the lower third of the game window
        bool st_on = false;
        if (s_sw) {
            ULONGLONG t0; { std::lock_guard<std::mutex> lk(s_st_mtx); t0 = s_st_t0; }
            RECT ss{};
            if (t0 && GetTickCount64() - t0 >= 300 && game_shown(ss)) {
                st_on = true;
                InvalidateRect(s_sw, nullptr, FALSE);
                place(s_sw, (ss.left + ss.right - SW_W) / 2, ss.top + (ss.bottom - ss.top) * 2 / 3, SW_W, SW_H);
            } else hide(s_sw);
        }

        // toasts: 5 s each, the last 0.6 s fading out; trophy pop-ups ptp::LIFE_MS, sliding in from the
        // window edge; queued while the game is in the background
        bool fading = false;
        if (s_tw) {
            const ULONGLONG now = GetTickCount64();
            const ULONGLONG life = s_cur.trophy && !s_pop.empty() ? ptp::LIFE_MS : 5000;
            if (s_cur_on && now - s_cur_t0 > life) { s_cur_on = false; hide(s_tw); hide(s_pw); if (osd) rsx_osd_set(OSD_TOAST, nullptr, 0, 0, 0, 0, 0, 0.0f); }
            bool fresh = false;
            if (!s_cur_on && vis) {
                {
                    std::lock_guard<std::mutex> lk(s_toast_mtx);
                    if (!s_toasts.empty()) { s_cur = s_toasts.front(); s_toasts.pop_front(); ++s_toast_shown; s_cur_on = true; s_cur_t0 = now; fresh = true; }
                }
                if (fresh) {
                    s_pop = ptp::Image();
                    if (s_cur.trophy) render_trophy(ch);
                    InvalidateRect(s_tw, nullptr, TRUE);
                }
            }
            static bool pop_pub = false;   // s_pop is in the OSD slot
            if (fresh || !osd) pop_pub = false;
            if (s_cur_on && vis && !s_pop.empty()) {
                const ULONGLONG age = now - s_cur_t0;
                int dx = 0;
                const float fa = ptp::anim(age, ptp::LIFE_MS, s_pop.h, &dx);
                fading = age < ptp::IN_MS || age + ptp::OUT_MS > ptp::LIFE_MS;
                const int m = ptp::margin(s_pop.h);
                const int x = right ? m - dx : cw - s_pop.w - m + dx, y = m;   // opposite corner to the overlay
                if (osd) {
                    hide(s_tw); hide(s_pw);
                    if (!pop_pub) { rsx_osd_set(OSD_TOAST, s_pop_pm.px.data(), s_pop_pm.w, s_pop_pm.h, s_pop_pm.w * 4, x, y, fa); pop_pub = true; }
                    else rsx_osd_set(OSD_TOAST, nullptr, 0, 0, 0, x, y, fa);
                } else if (fresh || !IsWindowVisible(s_pw)) ptp::ulw_show(s_pw, s_pop, cs.left + x, cs.top + y, fa);
                else ptp::ulw_move(s_pw, cs.left + x, cs.top + y, fa);
            } else if (s_cur_on && vis) {
                const ULONGLONG age = now - s_cur_t0;
                const int a = age > 4400 ? (int)(230 * (5000 - std::min<ULONGLONG>(age, 5000)) / 600) : age < 200 ? (int)(230 * age / 200) : 230;
                if (osd) {
                    hide(s_tw);
                    const int x = right ? 16 : cw - TW - 16;
                    const float fa = (float)std::max(a, 0) / 255.0f;
                    static bool published = false;
                    if (fresh || !published || pop_pub) { pop_pub = false; osd_publish(s_dib_toast, OSD_TOAST, TW, TH, toast_paint, x, 16, fa); published = true; }
                    else rsx_osd_set(OSD_TOAST, nullptr, 0, 0, 0, x, 16, fa);
                } else {
                    SetLayeredWindowAttributes(s_tw, 0, (BYTE)std::max(a, 0), LWA_ALPHA);
                    const int x = right ? cs.left + 16 : cs.right - TW - 16;   // opposite corner to the overlay
                    place(s_tw, x, cs.top + 16, TW, TH);
                }
            } else if (!vis && s_cur_on) {   // paused in the background
                s_cur_t0 += now - s_prev_now; hide(s_tw); hide(s_pw);
                if (osd) rsx_osd_set(OSD_TOAST, nullptr, 0, 0, 0, 0, 0, 0.0f);
            }
            s_prev_now = now;
        }
        Sleep(fading ? 16 : st_on ? 50 : s_overlay || s_cur_on ? 33 : 100);
    }
}

void start_thread()
{
    if (s_thread_started.exchange(1)) return;
    std::thread(ui_thread).detach();
}

struct PcFeatInit {
    PcFeatInit()
    {
        const char* e = getenv("RFOM_OVERLAY");
        if (e && *e == '0') s_overlay_allowed = 0;
        else if (e && *e == '1') s_overlay = 1;
        const char* g = getenv("PORT_OVERLAY_GRAPH");
        s_graph = !(g && *g == '0');
        start_thread();
    }
} s_init;

} // namespace

// Called once per game frame (stubs.cpp rfom_frame_wait).
extern "C" void rfpf_frame_tick(void)
{
    static LARGE_INTEGER last{};
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (last.QuadPart && s_qpf.QuadPart && s_overlay) {
        const double ms = (now.QuadPart - last.QuadPart) * 1000.0 / s_qpf.QuadPart;
        if (ms < 1000.0) {
            std::lock_guard<std::mutex> lk(s_ft_mtx);
            s_ft_sum += ms; s_ft_n++; if (ms > s_ft_max) s_ft_max = ms;
            pcx_ft_push(&s_ft_ring, (float)ms);
        }
    }
    last = now;
}

extern "C" int rfpf_overlay_get(void) { return s_overlay; }
extern "C" void rfpf_overlay_set(int on) { if (s_overlay_allowed) s_overlay = on ? 1 : 0; }
extern "C" void rfpf_overlay_graph_set(int on) { s_graph = on ? 1 : 0; }

// Start-up status panel: "Compiling shaders 120 / 1500" (UTF-8). An "N / M" in the first line also drives the
// progress bar; "\n<text>" replaces the panel's second line. NULL or "" hides the panel.
extern "C" void rfpf_status(const char* text)
{
    std::lock_guard<std::mutex> lk(s_st_mtx);
    if (!text || !*text) { s_st_text.clear(); s_st_t0 = 0; s_st_done = s_st_total = 0; return; }
    s_st_text = widen(text);
    unsigned d = 0, t = 0;
    const char* p = std::strrchr(text, '/');
    if (const char* br = std::strchr(text, '\n')) {   // the progress is in the first line
        p = nullptr;
        for (const char* c = text; c < br; c++) if (*c == '/') p = c;
    }
    if (p && p > text) {
        const char* q = p - 1;
        while (q > text && *q == ' ') --q;
        while (q > text && q[-1] >= '0' && q[-1] <= '9') --q;
        d = (unsigned)std::strtoul(q, nullptr, 10);
        t = (unsigned)std::strtoul(p + 1, nullptr, 10);
    }
    s_st_done = d; s_st_total = t;
    if (!s_st_t0) s_st_t0 = GetTickCount64();
}

// A toast at the game window's top-right corner for 5 s. title/text are UTF-8; badge is a
// letter in a coloured circle ('P' 'G' 'S' 'B' have their own colours), 0 = a plain 'i' notice.
// A PS3-style pop-up (port_trophy_popup.h) in the toast queue: line1 ("You have earned an
// achievement."), the name (UTF-8) and grade 'P' 'G' 'S' 'B' (a cup in its colour).
// Returns the pop-up's ticket for rfpf_toast_shown(), or 0 when the queue is full (dropped).
extern "C" unsigned long long rfpf_trophy_ticket(const char* line1, const char* name, char grade)
{
    Toast t{ widen(line1), widen(name), grade };
    t.trophy = true;
    std::lock_guard<std::mutex> lk(s_toast_mtx);
    if (s_toasts.size() >= 32) return 0;
    s_toasts.push_back(std::move(t));
    return ++s_toast_pushed;
}

extern "C" void rfpf_trophy(const char* line1, const char* name, char grade)
{
    rfpf_trophy_ticket(line1, name, grade);
}

// The pop-up with this ticket has been taken off the queue and put on screen.
extern "C" int rfpf_toast_shown(unsigned long long ticket)
{
    std::lock_guard<std::mutex> lk(s_toast_mtx);
    return ticket && ticket <= s_toast_shown;
}

extern "C" unsigned long long rfpf_toast_ticket(const char* title, const char* text, char badge)
{
    std::lock_guard<std::mutex> lk(s_toast_mtx);
    if (s_toasts.size() >= 32) return 0;
    s_toasts.push_back({ widen(title), widen(text), badge });
    return ++s_toast_pushed;
}

extern "C" void rfpf_toast(const char* title, const char* text, char badge)
{
    rfpf_toast_ticket(title, text, badge);
}
