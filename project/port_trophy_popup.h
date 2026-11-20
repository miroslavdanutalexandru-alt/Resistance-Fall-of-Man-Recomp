// PS3-style trophy / achievement pop-up, shared by the Resistance 1 / 2 / 3 ports (one copy in each
// repo's project\, kept identical): a compact dark translucent rounded box for the top-right corner
// of the game window with the trophy's icon at the left, "You have earned a trophy." and, under it,
// a small grade cup and the trophy's name.
//
// Everything is drawn here (GDI text into a DIB, the rest in software) into a straight-alpha BGRA
// image; the ports hand it to their swapchain OSD (premultiply() first where that wants it) or to a
// per-pixel-alpha layered window (ulw_show / ulw_move). The icon is the trophy's own TROPnnn.PNG,
// read at run time from the user's TROPHY.TRP (trp_icon); without one a cup is drawn, tinted by
// grade (bronze / silver / gold / platinum; anything else draws bronze). No console assets ship.
//
// PORT_TROPHY_STYLE (or <prefix>_TROPHY_STYLE) = old | classic | 0: the ports' previous toast with
// the grade letter badge instead (style_old). PORT_TROPHY_TEST=1: the ports queue one sample pop-up
// per grade a few seconds after start (each port's *_trophies.cpp).
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <stdint.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace ptp {

struct Image {                     // BGRA (0xAARRGGBB), straight alpha unless premultiply()d
    std::vector<uint32_t> px;
    int w = 0, h = 0;
    bool empty() const { return px.empty() || w <= 0 || h <= 0; }
};

const unsigned LIFE_MS = 6000;     // on screen per pop-up, fade-out included
const unsigned IN_MS = 350;        // slide + fade in
const unsigned OUT_MS = 500;       // fade out

inline bool style_old(const char* prefix)
{
    const char* e = getenv("PORT_TROPHY_STYLE");
    if ((!e || !*e) && prefix) {
        char n[64];
        snprintf(n, sizeof n, "%s_TROPHY_STYLE", prefix);
        e = getenv(n);
    }
    return e && (!_stricmp(e, "old") || !_stricmp(e, "classic") || !strcmp(e, "0"));
}

inline bool test_on()
{
    const char* e = getenv("PORT_TROPHY_TEST");
    return e && *e && strcmp(e, "0") != 0;
}

// Panel height for a game window `ch` pixels tall; ui_pct = an explicit PORT_UI_SCALE percent (0 = auto,
// the panel then just follows the window height).
inline int panel_height(int ch, int ui_pct)
{
    int h = (int)(ch * 0.068f + 0.5f);
    if (ui_pct > 0) h = h * ui_pct / 100;
    return h < 44 ? 44 : h > 240 ? 240 : h;
}

// Gap to the window's top and side edges.
inline int margin(int panel_h) { return (panel_h * 35 + 50) / 100; }

// Slide-in from the outer edge and fade, then a fade-out. Returns the opacity 0..1; *dx is the offset
// towards the outer edge (positive: further right) of the rest position, for a panel w pixels wide.
inline float anim(unsigned long long age, unsigned long long life, int panel_h, int* dx)
{
    *dx = 0;
    if (age >= life) return 0.f;
    float a = 1.f;
    if (age < IN_MS) {
        const float t = (float)age / IN_MS, e = 1.f - (1.f - t) * (1.f - t) * (1.f - t);   // ease-out cubic
        *dx = (int)((1.f - e) * panel_h * 0.6f + 0.5f);
        a = e;
    }
    if (age + OUT_MS > life) a = (float)(life - age) / OUT_MS;
    return a < 0 ? 0 : a > 1 ? 1 : a;
}

// ---- TROPHY.TRP icons (the user's own game files) ----

inline uint32_t trp_be32(const unsigned char* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
inline uint64_t trp_be64(const unsigned char* p) { return (uint64_t)trp_be32(p) << 32 | trp_be32(p + 4); }

// Reads one entry (e.g. "TROP005.PNG") of a TROPHY.TRP without loading the rest of the file.
inline bool trp_entry(const char* trp, const char* name, std::vector<unsigned char>& out)
{
    out.clear();
    if (!trp || !*trp) return false;
    FILE* f = fopen(trp, "rb");
    if (!f) return false;
    bool ok = false;
    unsigned char hd[64];
    if (fread(hd, 1, 64, f) == 64 && trp_be32(hd) == 0xDCA24D00u) {
        const uint32_t count = trp_be32(hd + 16), esz = trp_be32(hd + 20);
        if (esz >= 48 && esz <= 1024 && count <= 1024) {
            std::vector<unsigned char> tab((size_t)count * esz);
            if (fread(tab.data(), 1, tab.size(), f) == tab.size()) {
                for (uint32_t i = 0; i < count; i++) {
                    const unsigned char* e = &tab[(size_t)i * esz];
                    char n[33] = {};
                    memcpy(n, e, 32);
                    if (_stricmp(n, name)) continue;
                    const uint64_t off = trp_be64(e + 32), sz = trp_be64(e + 40);
                    if (sz == 0 || sz > 8u * 1024 * 1024 || _fseeki64(f, (long long)off, SEEK_SET)) break;
                    out.resize((size_t)sz);
                    ok = fread(out.data(), 1, out.size(), f) == out.size();
                    break;
                }
            }
        }
    }
    fclose(f);
    if (!ok) out.clear();
    return ok;
}

// PNG / any WIC-readable image -> straight BGRA.
inline bool decode_image(const unsigned char* d, size_t n, Image& out)
{
    out = Image();
    if (!d || n < 8 || n > 0x7FFFFFFF) return false;
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* fac = nullptr;
    IWICStream* st = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICFormatConverter* cv = nullptr;
    bool ok = false;
    UINT w = 0, h = 0;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac))) &&
        SUCCEEDED(fac->CreateStream(&st)) && SUCCEEDED(st->InitializeFromMemory((BYTE*)d, (DWORD)n)) &&
        SUCCEEDED(fac->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(fac->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(cv->GetSize(&w, &h)) && w > 0 && h > 0 && w <= 2048 && h <= 2048) {
        out.px.resize((size_t)w * h);
        out.w = (int)w; out.h = (int)h;
        ok = SUCCEEDED(cv->CopyPixels(nullptr, w * 4, w * h * 4, (BYTE*)out.px.data()));
    }
    if (cv) cv->Release();
    if (fr) fr->Release();
    if (dec) dec->Release();
    if (st) st->Release();
    if (fac) fac->Release();
    if (SUCCEEDED(ci)) CoUninitialize();
    if (!ok) out = Image();
    return ok;
}

// TROPnnn.PNG of trophy `id` from a TROPHY.TRP.
inline bool trp_icon(const char* trp, int id, Image& out)
{
    out = Image();
    if (id < 0 || id > 999) return false;
    char n[16];
    snprintf(n, sizeof n, "TROP%03d.PNG", id);
    std::vector<unsigned char> png;
    return trp_entry(trp, n, png) && decode_image(png.data(), png.size(), out);
}

// ---- drawing ----

struct Rgb { float r, g, b; };

inline float clamp01(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }

// Straight-alpha "over" of colour c with coverage a onto pixel p.
inline void blend(uint32_t& p, Rgb c, float a)
{
    if (a <= 0.f) return;
    a = clamp01(a);
    const float da = (p >> 24) / 255.f;
    const float oa = a + da * (1.f - a);
    if (oa <= 0.f) { p = 0; return; }
    const float dr = (p >> 16 & 255), dg = (p >> 8 & 255), db = (p & 255);
    const float k = da * (1.f - a);
    const float r = (c.r * a + dr * k) / oa, g = (c.g * a + dg * k) / oa, b = (c.b * a + db * k) / oa;
    auto q = [](float v) { return (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5f); };
    p = q(oa * 255.f) << 24 | q(r) << 16 | q(g) << 8 | q(b);
}

// Signed distance from (x, y) to a rounded box (centre cx, cy; half sizes hw, hh; corner radius r).
inline float sd_round_box(float x, float y, float cx, float cy, float hw, float hh, float r)
{
    const float qx = std::fabs(x - cx) - hw + r, qy = std::fabs(y - cy) - hh + r;
    const float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    const float in = (qx > qy ? qx : qy);
    return std::sqrt(ox * ox + oy * oy) + (in < 0 ? in : 0) - r;
}

// The cup, in a unit square (y down): bowl, rim, two handles, stem and base.
inline bool cup_inside(float x, float y)
{
    const float dx = x - 0.5f;
    if (y >= 0.10f && y <= 0.155f && std::fabs(dx) <= 0.33f) return true;                         // rim
    if (y >= 0.13f) { const float ex = dx / 0.29f, ey = (y - 0.13f) / 0.44f; if (ex * ex + ey * ey <= 1.f) return true; }   // bowl
    for (float hx : { 0.205f, 0.795f }) {                                                          // handles
        const float hdx = x - hx, hdy = y - 0.29f, d = std::sqrt(hdx * hdx + hdy * hdy);
        if (d <= 0.125f && d >= 0.07f && std::fabs(x - 0.5f) >= std::fabs(hx - 0.5f) - 0.02f) return true;
    }
    if (y >= 0.55f && y <= 0.74f && std::fabs(dx) <= 0.045f) return true;                          // stem
    if (y >= 0.70f && y <= 0.79f && std::fabs(dx) <= 0.045f + (y - 0.70f) * 1.4f) return true;     // flare
    if (y >= 0.79f && y <= 0.90f && std::fabs(dx) <= 0.22f) return true;                           // base
    return false;
}

inline void grade_colours(char g, Rgb& dark, Rgb& light)
{
    switch (g) {
    case 'P': dark = { 104, 128, 166 }; light = { 232, 242, 255 }; break;
    case 'G': dark = { 168, 116, 18 };  light = { 255, 226, 112 }; break;
    case 'S': dark = { 120, 124, 134 }; light = { 244, 246, 250 }; break;
    default:  dark = { 132, 74, 36 };   light = { 238, 168, 106 }; break;   // 'B' and unknown
    }
}

// Cup of grade g into a size x size square at (x0, y0) of img (4x4 supersampled).
inline void draw_cup(Image& img, int x0, int y0, int size, char g, float alpha = 1.f)
{
    if (size < 4) return;
    Rgb dk, lt;
    grade_colours(g, dk, lt);
    for (int y = 0; y < size; y++) {
        const int iy = y0 + y;
        if (iy < 0 || iy >= img.h) continue;
        for (int x = 0; x < size; x++) {
            const int ix = x0 + x;
            if (ix < 0 || ix >= img.w) continue;
            int hit = 0;
            float shade = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    const float u = (x + (sx + 0.5f) / 4.f) / size, v = (y + (sy + 0.5f) / 4.f) / size;
                    if (!cup_inside(u, v)) continue;
                    hit++;
                    // metal: a soft highlight left of centre, darker towards the edges and the bottom
                    const float hl = std::cos((u - 0.40f) * 3.14159f * 1.6f);
                    shade += clamp01(0.25f + 0.75f * hl * hl * (hl > 0 ? 1.f : 0.f)) * (1.f - 0.25f * v);
                }
            if (!hit) continue;
            const float t = shade / hit;
            const Rgb c = { dk.r + (lt.r - dk.r) * t, dk.g + (lt.g - dk.g) * t, dk.b + (lt.b - dk.b) * t };
            blend(img.px[(size_t)iy * img.w + ix], c, alpha * hit / 16.f);
        }
    }
}

// Area-averaged (premultiplied) scale of src into a size x size square at (x0, y0), corners rounded.
inline void draw_icon(Image& img, const Image& src, int x0, int y0, int size)
{
    if (src.empty() || size < 2) return;
    const float rad = size * 0.08f;
    for (int y = 0; y < size; y++) {
        const int iy = y0 + y;
        if (iy < 0 || iy >= img.h) continue;
        const int sy0 = y * src.h / size, sy1 = (std::max)(sy0 + 1, (y + 1) * src.h / size);
        for (int x = 0; x < size; x++) {
            const int ix = x0 + x;
            if (ix < 0 || ix >= img.w) continue;
            const int sx0 = x * src.w / size, sx1 = (std::max)(sx0 + 1, (x + 1) * src.w / size);
            float r = 0, g = 0, b = 0, a = 0; int n = 0;
            for (int sy = sy0; sy < sy1 && sy < src.h; sy++)
                for (int sx = sx0; sx < sx1 && sx < src.w; sx++) {
                    const uint32_t p = src.px[(size_t)sy * src.w + sx];
                    const float pa = (p >> 24) / 255.f;
                    r += (p >> 16 & 255) * pa; g += (p >> 8 & 255) * pa; b += (p & 255) * pa; a += pa; n++;
                }
            if (!n || a <= 0.f) continue;
            const float cov = clamp01(0.5f - sd_round_box(x + 0.5f, y + 0.5f, size / 2.f, size / 2.f, size / 2.f, size / 2.f, rad));
            blend(img.px[(size_t)iy * img.w + ix], { r / a, g / a, b / a }, a / n * cov);
        }
    }
}

// GDI text, white on black in a scratch DIB, used as coverage for colour c.
struct TextPainter {
    HDC dc = nullptr; HBITMAP bm = nullptr; HGDIOBJ old = nullptr; uint32_t* bits = nullptr; int w = 0, h = 0;
    bool begin(int w_, int h_)
    {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w_; bi.bmiHeader.biHeight = -h_;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        void* p = nullptr;
        dc = CreateCompatibleDC(nullptr);
        bm = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &p, nullptr, 0) : nullptr;
        if (!bm || !p) return false;
        old = SelectObject(dc, bm);
        bits = (uint32_t*)p; w = w_; h = h_;
        return true;
    }
    ~TextPainter()
    {
        if (dc && old) SelectObject(dc, old);
        if (bm) DeleteObject(bm);
        if (dc) DeleteDC(dc);
    }
    void draw(Image& img, HFONT f, const std::wstring& s, RECT r, UINT fmt, Rgb c)
    {
        if (!bits || s.empty()) return;
        memset(bits, 0, (size_t)w * h * 4);
        HGDIOBJ of = SelectObject(dc, f);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        DrawTextW(dc, s.c_str(), (int)s.size(), &r, fmt | DT_NOPREFIX);
        SelectObject(dc, of);
        GdiFlush();
        for (int y = (std::max)(0, (int)r.top - 2); y < (std::min)(h, (int)r.bottom + 2); y++)
            for (int x = (std::max)(0, (int)r.left - 2); x < (std::min)(w, (int)r.right + 2); x++) {
                const uint32_t p = bits[(size_t)y * w + x];
                const int m = (std::max)((std::max)((int)(p >> 16 & 255), (int)(p >> 8 & 255)), (int)(p & 255));
                if (m) blend(img.px[(size_t)y * img.w + x], c, m / 255.f);
            }
    }
};

inline HFONT make_font(int px, int weight)
{
    return CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

inline int text_width(HFONT f, const std::wstring& s)
{
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return 0;
    HGDIOBJ of = SelectObject(dc, f);
    SIZE sz = {};
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    SelectObject(dc, of);
    DeleteDC(dc);
    return sz.cx;
}

struct Popup {
    std::wstring line1;            // "You have earned a trophy."
    std::wstring name;             // the trophy's name
    char grade = 'B';              // P G S B (anything else draws bronze)
    Image icon;                    // TROPnnn.PNG, or empty for the drawn cup
};

// Renders the pop-up `panel_h` pixels tall (straight alpha) into out; at most max_w wide.
inline bool render(const Popup& p, int panel_h, int max_w, Image& out)
{
    out = Image();
    const int H = panel_h < 24 ? 24 : panel_h;
    const int pad = (H * 14 + 50) / 100, icon = H - 2 * pad, gap = (H * 18 + 50) / 100;
    const int f1px = (std::max)(9, (H * 21 + 50) / 100), f2px = (std::max)(10, (H * 25 + 50) / 100);
    const int cup = (std::max)(8, (H * 27 + 50) / 100), cup_gap = (std::max)(3, (H * 7 + 50) / 100);
    HFONT f1 = make_font(f1px, FW_NORMAL), f2 = make_font(f2px, FW_SEMIBOLD);
    if (!f1 || !f2) { if (f1) DeleteObject(f1); if (f2) DeleteObject(f2); return false; }
    const int tx = pad + icon + gap;
    const int w1 = text_width(f1, p.line1), w2 = cup + cup_gap + text_width(f2, p.name);
    int W = tx + (std::max)(w1, w2) + pad + H / 6;
    W = (std::max)(W, H * 42 / 10);
    W = (std::min)(W, H * 7);
    if (max_w > 0) W = (std::min)(W, max_w);
    out.w = W; out.h = H;
    out.px.assign((size_t)W * H, 0u);

    // box: vertical gradient, rounded, a faint light rim
    const float rad = H * 0.13f;
    for (int y = 0; y < H; y++) {
        const float t = (float)y / (H - 1);
        const Rgb c = { 44 - 28 * t, 44 - 28 * t, 48 - 30 * t };
        for (int x = 0; x < W; x++) {
            const float d = sd_round_box(x + 0.5f, y + 0.5f, W / 2.f, H / 2.f, W / 2.f, H / 2.f, rad);
            const float cov = clamp01(0.5f - d);
            if (cov <= 0.f) continue;
            uint32_t& px = out.px[(size_t)y * W + x];
            blend(px, c, 0.86f * cov);
            const float rim = cov - clamp01(0.5f - (d + 1.0f));
            if (rim > 0.f) blend(px, { 255, 255, 255 }, 0.16f * rim);
        }
    }

    // icon: the trophy's own picture, else a cup on a slightly lighter tile
    if (!p.icon.empty()) draw_icon(out, p.icon, pad, pad, icon);
    else {
        const float r2 = icon * 0.12f;
        for (int y = 0; y < icon; y++)
            for (int x = 0; x < icon; x++) {
                const float d = sd_round_box(x + 0.5f, y + 0.5f, icon / 2.f, icon / 2.f, icon / 2.f, icon / 2.f, r2);
                const float cov = clamp01(0.5f - d);
                const float t = (float)y / icon;
                if (cov > 0.f) blend(out.px[(size_t)(pad + y) * W + pad + x], { 70 - 30 * t, 70 - 30 * t, 76 - 32 * t }, 0.9f * cov);
            }
        const int inset = icon / 8;
        draw_cup(out, pad + inset, pad + inset, icon - 2 * inset, p.grade);
    }

    TextPainter tp;
    if (tp.begin(W, H)) {
        const int y1 = (H * 15 + 50) / 100, y2 = (H * 50 + 50) / 100;
        tp.draw(out, f1, p.line1, RECT{ tx, y1, W - pad, y1 + f1px * 3 / 2 }, DT_SINGLELINE | DT_END_ELLIPSIS, { 222, 222, 226 });
        const int lh2 = f2px * 3 / 2;
        draw_cup(out, tx, y2 + (lh2 - cup) / 2, cup, p.grade);
        tp.draw(out, f2, p.name, RECT{ tx + cup + cup_gap, y2, W - pad, y2 + lh2 }, DT_SINGLELINE | DT_END_ELLIPSIS | DT_VCENTER, { 255, 255, 255 });
    }
    DeleteObject(f1);
    DeleteObject(f2);
    return true;
}

inline void premultiply(Image& img)
{
    for (uint32_t& p : img.px) {
        const uint32_t a = p >> 24;
        if (a == 255) continue;
        p = a << 24 | ((p >> 16 & 255) * a + 127) / 255 << 16 | ((p >> 8 & 255) * a + 127) / 255 << 8 | ((p & 255) * a + 127) / 255;
    }
}

// ---- per-pixel-alpha layered window (no SetLayeredWindowAttributes on it) ----

inline LRESULT CALLBACK ulw_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

inline HWND ulw_create(const wchar_t* cls)
{
    WNDCLASSW wc = {};
    wc.lpfnWndProc = ulw_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = cls;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    return CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                           cls, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
}

// New pixels (straight alpha) at screen (x, y), opacity alpha 0..1; shows the window without activating it.
inline bool ulw_show(HWND h, const Image& img, int x, int y, float alpha)
{
    if (!h || img.empty()) return false;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = img.w; bi.bmiHeader.biHeight = -img.h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC sdc = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(sdc);
    HBITMAP bm = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    bool ok = false;
    if (bm && bits) {
        Image pm = img;
        premultiply(pm);
        memcpy(bits, pm.px.data(), pm.px.size() * 4);
        HGDIOBJ old = SelectObject(dc, bm);
        POINT dst = { x, y }, src = { 0, 0 };
        SIZE sz = { img.w, img.h };
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)(clamp01(alpha) * 255.f + 0.5f), AC_SRC_ALPHA };
        ok = UpdateLayeredWindow(h, sdc, &dst, &sz, dc, &src, 0, &bf, ULW_ALPHA) != FALSE;
        SelectObject(dc, old);
    }
    if (bm) DeleteObject(bm);
    if (dc) DeleteDC(dc);
    ReleaseDC(nullptr, sdc);
    if (ok) {
        if (!IsWindowVisible(h)) ShowWindow(h, SW_SHOWNOACTIVATE);
        SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    return ok;
}

// Move / fade without new pixels.
inline void ulw_move(HWND h, int x, int y, float alpha)
{
    if (!h) return;
    POINT dst = { x, y };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, (BYTE)(clamp01(alpha) * 255.f + 0.5f), AC_SRC_ALPHA };
    UpdateLayeredWindow(h, nullptr, &dst, nullptr, nullptr, nullptr, 0, &bf, ULW_ALPHA);
    if (!IsWindowVisible(h)) ShowWindow(h, SW_SHOWNOACTIVATE);
}

} // namespace ptp
