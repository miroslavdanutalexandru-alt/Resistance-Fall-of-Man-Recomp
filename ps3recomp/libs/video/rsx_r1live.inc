/* R1_LIVE=1 (diagnostic, default off): live A/B of the frame-generation motion fixes while the game runs.
 * A 4 KB named mapping "Local\R1Live_<pid>" (tools/r1live.py reads it and writes the request word):
 *   u32 [0] magic 'R1LV' (0x564C3152), [1] version, [2] pid, [3] presents, [4] frame generation on,
 *       [5] match tint on, [6] active variant (char), [7] request (client writes: 'a'..'h' variant, '+' / '-' tint),
 *       [8] counter count; u32 counters from byte 64 (R1C_*, cumulative, never reset).
 * Variants (R1_LIVE_VAR = start variant, default a):
 *   a  6b5bbd7 fix: view-attached keys (weapon / arms) re-matched past the re-match caps
 *   b  old (6b84476): no key probes, only the 64-a-frame near-eye check once over the time cap; draw cap applies
 *   c  view-lock off            d  re-match + loose off (camera-only for ambiguous / material-changed draws)
 *   e  fix, no re-match caps    f  camera-only motion for every draw (RSX_DLSS_OBJMV=0)
 *   g  frame generation off     h  upscaler jitter off
 * The active letter is drawn bottom-left (OSD slot 2), and every switch is logged ([R1LIVE] lines).
 * Included by rsx_d3d12_backend.c before rsx_dlssg.inc; the counters are filled by rsx_fsr.inc (r1l_collect). */

enum { R1C_FRAMES, R1C_FG_FRAMES, R1C_KEY_NEAR, R1C_KEY_PROBE, R1C_NEARX, R1C_RM_BUDGET, R1C_NEAR_CAM, R1C_NEAR_BUDGET,
       R1C_NEAR_UNCLEAR, R1C_NEAR_NOLOOSE, R1C_NEAR_OTHER, R1C_RM_OK, R1C_RM_VL, R1C_LOOSE_OK, R1C_LOOSE_FAIL, R1C_NOKEY,
       R1C_NOHIST, R1C_CAM_ONLY, R1C_AMB_DROP, R1C_RM_UNCLEAR, R1C_LATE, R1C_NOPSO, R1C_REC, R1C_N };
#define R1L_MAGIC 0x564C3152u
#define R1L_VARS "abcdefgh"

static volatile int s_r1l_on;     /* R1_LIVE=1 and the mapping exists */
static volatile int s_r1l_var;    /* active variant letter (0 = live off: every R1L() test is false) */
static volatile int s_r1l_tint;   /* RSX_DLSS_DEBUG=match forced on */
static volatile u32* s_r1l_blk;
static int s_r1l_osd_ok;          /* the letter reached the OSD (it may come up after the block) */
#define R1L(c) (s_r1l_var == (c))

static const char* r1l_name(int v)
{
    switch (v) {
    case 'a': return "fix (6b5bbd7)";
    case 'b': return "old (6b84476)";
    case 'c': return "view-lock off";
    case 'd': return "re-match+loose off";
    case 'e': return "fix, no caps";
    case 'f': return "camera-only MV";
    case 'g': return "FG off";
    case 'h': return "SR jitter off";
    }
    return "?";
}

/* the variant letter, bottom-left of the swap chain */
static void r1l_osd(void)
{
    enum { W = 640, H = 48 };
    char txt[96];
    const int n = snprintf(txt, sizeof txt, "FG A/B: %c  %s%s", s_r1l_var, r1l_name(s_r1l_var), s_r1l_tint ? "  [tint]" : "");
    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) return;
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bm || !bits) { if (bm) DeleteObject(bm); DeleteDC(dc); return; }
    HGDIOBJ ob = SelectObject(dc, bm);
    memset(bits, 0, (size_t)W * H * 4);
    HFONT f = CreateFontA(-26, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, NONANTIALIASED_QUALITY, 0, "Consolas");
    HGDIOBJ of = f ? SelectObject(dc, f) : NULL;
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
    SIZE sz = { 0, 0 }; GetTextExtentPoint32A(dc, txt, n, &sz);
    TextOutA(dc, 8, 4, txt, n);
    GdiFlush();
    int w = sz.cx + 16, h = sz.cy + 8;
    if (w > W) w = W;
    if (h > H) h = H;
    u32* p = (u32*)bits;
    for (int i = 0; i < W * H; i++) p[i] = (p[i] & 0xFFFFFFu) ? 0xFFFFFF40u : 0xB0000000u;   /* yellow text on 70% black */
    const int y = s_sc_h > (u32)(h + 24) ? (int)s_sc_h - h - 24 : 0;
    rsx_osd_set(2, bits, w, h, W * 4, 16, y, 1.0f);
    s_r1l_osd_ok = rsx_osd_available();
    if (of) SelectObject(dc, of);
    if (f) DeleteObject(f);
    SelectObject(dc, ob); DeleteObject(bm); DeleteDC(dc);
}

static void r1l_switch(int v, int tint, const char* why)
{
    s_r1l_var = v; s_r1l_tint = tint;
    if (s_r1l_blk) { s_r1l_blk[5] = (u32)tint; s_r1l_blk[6] = (u32)v; }
    fprintf(stderr, "[R1LIVE] variant %c = %s, tint %s (%s, present %u)%c", v, r1l_name(v), tint ? "on" : "off", why,
            s_r1l_blk ? s_r1l_blk[3] : 0u, 10);
    r1l_osd();
}

/* render thread, once per present (fg_present): create the block, take the client's request, count, redraw the letter */
static void r1l_poll(int fg_on)
{
    static int init = 0; static u32 osd_h;
    if (!init) {
        init = 1;
        const char* e = getenv("R1_LIVE");
        if (!(e && *e == '1')) return;
        wchar_t nm[64]; swprintf(nm, 64, L"Local\\R1Live_%lu", GetCurrentProcessId());
        HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, nm);
        void* m = h ? MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 4096) : NULL;
        if (!m) { fprintf(stderr, "[R1LIVE] shared block failed (%lu)%c", GetLastError(), 10); return; }
        s_r1l_blk = (volatile u32*)m;
        memset((void*)s_r1l_blk, 0, 4096);
        s_r1l_blk[1] = 1; s_r1l_blk[2] = GetCurrentProcessId(); s_r1l_blk[8] = R1C_N;
        int v = 'a';
        { const char* ve = getenv("R1_LIVE_VAR"); if (ve && *ve && strchr(R1L_VARS, ve[0] | 0x20)) v = ve[0] | 0x20; }
        int t = 0;
        { const char* te = getenv("RSX_DLSS_DEBUG"); t = te && !_stricmp(te, "match"); }
        s_r1l_blk[0] = R1L_MAGIC;
        s_r1l_on = 1;
        fprintf(stderr, "[R1LIVE] live A/B on: Local\\R1Live_%lu (tools/r1live.py)%c", GetCurrentProcessId(), 10);
        r1l_switch(v, t, "start");
        osd_h = s_sc_h;
    }
    if (!s_r1l_on) return;
    volatile u32* b = s_r1l_blk;
    b[3]++;
    b[16 + R1C_FRAMES]++;
    if (fg_on) b[16 + R1C_FG_FRAMES]++;
    b[4] = (u32)(fg_on != 0);
    const u32 rq = b[7];
    if (rq) {
        b[7] = 0;
        if (rq == '+' || rq == '-') r1l_switch(s_r1l_var, rq == '+', "client");
        else if (rq < 128 && strchr(R1L_VARS, (int)(rq | 0x20))) r1l_switch((int)(rq | 0x20), s_r1l_tint, "client");
    }
    if (osd_h != s_sc_h || (!s_r1l_osd_ok && (b[3] & 63u) == 0u)) { osd_h = s_sc_h; r1l_osd(); }   /* resized: keep it bottom-left */
}
