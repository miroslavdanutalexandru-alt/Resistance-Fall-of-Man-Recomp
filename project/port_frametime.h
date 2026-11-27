/* Resistance PC ports: frame-time (stutter) recorder. Shared by the R1/R2/R3
 * ports -- keep the copies identical.
 *
 * The renderer calls port_ft_present() once per presented frame (R1:
 * rsx_d3d12_backend.c present_blit; R2/R3: RSXDisplayEngine.cpp flip). That
 * costs one QueryPerformanceCounter, a histogram increment and a ring-buffer
 * store: no lock (a busy flag makes a second, concurrent caller skip the
 * frame), no allocation, no I/O. Time spent paused (focus loss, PC menu, quit
 * prompt) is left out; gaps of 1 s or more (loading, hangs) are counted as
 * stalls, apart from the frame statistics.
 *
 * logs\frametimes_<yyyymmdd_hhmmss>[_<exe>].txt, written at exit (also after an
 * ExitProcess, via a TLS callback), after a crash or hang report (port_crash.h
 * after_report) and after each marker: frames, average, 1% / 0.1% lows (the
 * average of the slowest 1% / 0.1% of frames) with p99 / p99.9, a histogram,
 * the worst 20 spikes and the longest stalls with their time, and the markers.
 * The newest 10 such files are kept. "Report a problem" adds them to the zip.
 *
 * Marker key (default Scroll Lock; F8 is the mouse-release key): writes a
 * "[frametime] MARKER" line with the time and the last 2 s of frame times to
 * the log (run.log) and into the frametimes file, so a tester can flag the
 * moment a stutter happened. Polled every 50 ms on the recorder's own thread,
 * only while one of the game's windows has the focus; that thread also does
 * the marker file writes.
 *   PORT_FRAMETIME=0             recorder off
 *   PORT_FT_MARKER_KEY=<key>     scroll (default), pause, f1..f24, off
 *
 * The statistics half (port_ft::Stats, stats_add, stats_low, format_report,
 * format_marker, prune_plan) is plain C++ for the unit tests; the recorder is
 * compiled where PORT_FRAMETIME_IMPL is defined (the port's crash .inc). */
#ifndef PORT_FRAMETIME_H
#define PORT_FRAMETIME_H
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <stdlib.h>

namespace port_ft {

enum { NB = 2500, TOP = 20, STALL_TOP = 10, RING = 8192, MARKS = 32, MARK_LEN = 2048, KEEP_FILES = 10 };
static const double BUCKET_MS = 0.1;      /* histogram resolution; the last bucket takes 249.9 ms .. 1 s */
static const double STALL_MS = 1000.0;    /* a gap this long is a stall (loading / hang), not a frame */
static const double MARK_WINDOW_MS = 2000.0;

struct Spike { float ms; float t; };      /* t: seconds since the recorder started */

struct Stats {
    uint64_t n;                /* frames counted */
    double sum, max;
    uint32_t hist[NB];
    double hsum[NB];           /* sum of ms per bucket: exact lows from the histogram */
    Spike top[TOP]; int ntop; float top_min;
    Spike stall[STALL_TOP]; int nstall; float stall_min;
    uint32_t stalls; double stall_ms;
    uint32_t pauses;
};

static inline void spike_add(Spike* a, int* n, float* mn, int cap, float ms, float t)
{
    if (*n < cap) {
        a[(*n)++] = Spike{ ms, t };
    } else {
        int mi = 0;
        for (int i = 1; i < cap; i++) if (a[i].ms < a[mi].ms) mi = i;
        a[mi] = Spike{ ms, t };
    }
    float m = a[0].ms;
    for (int i = 1; i < *n; i++) if (a[i].ms < m) m = a[i].ms;
    *mn = *n < cap ? 0.0f : m;
}

/* One present-to-present interval. Constant time except when it makes the top 20. */
static inline void stats_add(Stats* s, double ms, double t)
{
    if (!(ms >= 0.0)) return;
    if (ms >= STALL_MS) {
        s->stalls++;
        s->stall_ms += ms;
        if (s->nstall < STALL_TOP || ms > s->stall_min) spike_add(s->stall, &s->nstall, &s->stall_min, STALL_TOP, (float)ms, (float)t);
        return;
    }
    s->n++;
    s->sum += ms;
    if (ms > s->max) s->max = ms;
    int b = (int)(ms / BUCKET_MS);
    if (b >= NB) b = NB - 1;
    s->hist[b]++;
    s->hsum[b] += ms;
    if (s->ntop < TOP || ms > s->top_min) spike_add(s->top, &s->ntop, &s->top_min, TOP, (float)ms, (float)t);
}

/* The slowest `frac` of the frames (0.01 = 1%): their average (the "1% low",
 * as a frame time) and the percentile frame time (p99), from the histogram. */
static inline void stats_low(const Stats* s, double frac, double* avg_ms, double* pct_ms)
{
    *avg_ms = 0.0; *pct_ms = 0.0;
    if (!s->n) return;
    uint64_t k = (uint64_t)ceil((double)s->n * frac);
    if (k < 1) k = 1;
    uint64_t took = 0;
    double sum = 0.0;
    for (int b = NB - 1; b >= 0 && took < k; b--) {
        const uint32_t c = s->hist[b];
        if (!c) continue;
        const uint64_t t = (k - took) < c ? (k - took) : c;
        sum += s->hsum[b] * (double)t / (double)c;
        took += t;
        if (took >= k) {
            const double edge = b == NB - 1 ? s->hsum[b] / c : (b + 1) * BUCKET_MS;
            *pct_ms = edge < s->max ? edge : s->max;
        }
    }
    *avg_ms = sum / (double)k;
}

/* Text appender over a fixed buffer (never allocates). */
struct Out {
    char* p; size_t n, cap;
    void f(const char* fmt, ...)
    {
        if (n + 1 >= cap) return;
        va_list ap; va_start(ap, fmt);
        const int w = vsnprintf(p + n, cap - n, fmt, ap);
        va_end(ap);
        if (w > 0) n += (size_t)w < cap - n ? (size_t)w : cap - n - 1;
    }
};

static inline void clock_at(time_t start, double t, char* out, size_t cap)
{
    if (!start) { snprintf(out, cap, "--:--:--"); return; }
    const time_t at = start + (time_t)t;
    struct tm lt;
#ifdef _WIN32
    if (localtime_s(&lt, &at)) { snprintf(out, cap, "--:--:--"); return; }
#else
    if (!localtime_r(&at, &lt)) { snprintf(out, cap, "--:--:--"); return; }
#endif
    strftime(out, cap, "%H:%M:%S", &lt);
}

struct Info {
    const char* title;     /* "Resistance 2" */
    const char* exe;       /* "R2.exe" */
    const char* session;   /* yyyymmdd_hhmmss */
    const char* reasons;   /* "marker, exit" */
    const char* marker_key;
    time_t start;          /* wall clock at t = 0 */
    double duration_s;
};

/* Histogram rows: upper edges in ms, a little above the nominal frame times so
 * vsync jitter around 16.7 / 33.3 ms stays in one row. */
static const double HIST_EDGES[] = { 8.5, 11.5, 17.5, 21.0, 26.0, 34.5, 51.0, 100.0, 250.0, 1000.0 };
enum { HIST_ROWS = sizeof(HIST_EDGES) / sizeof(HIST_EDGES[0]) };

static inline void hist_rows(const Stats* s, uint64_t rows[HIST_ROWS])
{
    for (int r = 0; r < HIST_ROWS; r++) rows[r] = 0;
    for (int b = 0; b < NB; b++) {
        if (!s->hist[b]) continue;
        const double lo = b * BUCKET_MS;
        int r = 0;
        while (r < HIST_ROWS - 1 && lo + BUCKET_MS * 0.5 >= HIST_EDGES[r]) r++;
        rows[r] += s->hist[b];
    }
}

static inline void sort_spikes(Spike* a, int n, int by_time)
{
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && (by_time ? a[j].t < a[j - 1].t : a[j].ms > a[j - 1].ms); j--) {
            const Spike x = a[j]; a[j] = a[j - 1]; a[j - 1] = x;
        }
}

/* The frametimes_*.txt summary. markers: the marker lines ("" = none). */
static inline size_t format_report(const Stats* s, const Info* in, const char* markers, char* buf, size_t cap)
{
    Out o{ buf, 0, cap };
    if (cap) buf[0] = 0;
    o.f("%s PC port -- frame-time report (PORT_FRAMETIME=0 turns this off)\n", in->title ? in->title : "Resistance");
    o.f("session:   %s  %s\n", in->session ? in->session : "", in->exe ? in->exe : "");
    o.f("written:   %s\n", in->reasons && *in->reasons ? in->reasons : "-");
    o.f("recorded:  %.1f s, %llu frames (pauses left out: %u; stalls >= 1 s: %u, %.1f s, not in the numbers below)\n",
        in->duration_s, (unsigned long long)s->n, s->pauses, s->stalls, s->stall_ms / 1000.0);
    if (!s->n) { o.f("\nno frames recorded\n"); }
    else {
        const double avg = s->sum / (double)s->n;
        double l1, p99, l01, p999;
        stats_low(s, 0.01, &l1, &p99);
        stats_low(s, 0.001, &l01, &p999);
        o.f("\n1%% / 0.1%% low = the average of the slowest 1%% / 0.1%% of frames (CapFrameX / PresentMon style);\n"
            "p99 / p99.9 = the percentile frame time\n");
        o.f("average    %7.2f ms  %6.1f fps\n", avg, 1000.0 / avg);
        o.f("1%% low     %7.2f ms  %6.1f fps   (p99   %6.2f ms)\n", l1, 1000.0 / l1, p99);
        o.f("0.1%% low   %7.2f ms  %6.1f fps   (p99.9 %6.2f ms)\n", l01, 1000.0 / l01, p999);
        o.f("worst      %7.2f ms  %6.1f fps\n", s->max, 1000.0 / s->max);

        uint64_t rows[HIST_ROWS];
        hist_rows(s, rows);
        o.f("\nhistogram (frame time)         frames       %%\n");
        double lo = 0.0;
        for (int r = 0; r < HIST_ROWS; r++) {
            const double hi = HIST_EDGES[r];
            const double pct = 100.0 * (double)rows[r] / (double)s->n;
            char bar[41]; int nb = (int)(pct * 0.4 + 0.5); if (nb > 40) nb = 40;
            memset(bar, '#', (size_t)nb); bar[nb] = 0;
            o.f("  %6.1f - %6.1f ms  %12llu  %6.2f  %s\n", lo, hi, (unsigned long long)rows[r], pct, bar);
            lo = hi;
        }

        Spike top[TOP];
        const int nt = s->ntop;
        memcpy(top, s->top, sizeof(Spike) * (size_t)nt);
        sort_spikes(top, nt, 0);
        o.f("\nworst %d frames               at (s)    clock\n", nt);
        for (int i = 0; i < nt; i++) {
            char c[16]; clock_at(in->start, top[i].t, c, sizeof c);
            o.f("  %2d  %8.2f ms  %12.3f  %s\n", i + 1, top[i].ms, top[i].t, c);
        }
    }
    if (s->nstall) {
        Spike st[STALL_TOP];
        const int ns = s->nstall;
        memcpy(st, s->stall, sizeof(Spike) * (size_t)ns);
        sort_spikes(st, ns, 0);
        o.f("\nlongest stalls (>= 1 s: loading screens, hangs)\n");
        for (int i = 0; i < ns; i++) {
            char c[16]; clock_at(in->start, st[i].t, c, sizeof c);
            o.f("  %2d  %8.1f ms  %12.3f  %s\n", i + 1, st[i].ms, st[i].t, c);
        }
    }
    o.f("\nmarkers (%s)\n%s", in->marker_key ? in->marker_key : "marker key", markers && *markers ? markers : "  none\n");
    return o.n;
}

/* One marker: the frames presented in the MARK_WINDOW_MS before `head` (ring
 * entries [head - RING, head) hold the frame times in ms). */
static inline size_t format_marker(int idx, double now_t, time_t start, const float* ring_ms, uint64_t head,
                                   char* buf, size_t cap)
{
    Out o{ buf, 0, cap };
    if (cap) buf[0] = 0;
    uint64_t first = head;
    double span = 0.0, sum = 0.0, mx = 0.0;
    int slow = 0;
    while (first > 0 && head - first < RING) {
        const float ms = ring_ms[(first - 1) & (RING - 1)];
        if (span + ms > MARK_WINDOW_MS && first != head) break;
        span += ms; sum += ms;
        if (ms > mx) mx = ms;
        if (ms > 25.0f) slow++;
        first--;
    }
    const uint64_t n = head - first;
    char c[16]; clock_at(start, now_t, c, sizeof c);
    o.f("[frametime] MARKER #%d at %s (%.3f s): last %.1f s = %llu frames, avg %.2f ms, worst %.2f ms, %d over 25 ms\n",
        idx, c, now_t, span / 1000.0, (unsigned long long)n, n ? sum / (double)n : 0.0, mx, slow);
    o.f("[frametime]   ms:");
    for (uint64_t i = first; i < head; i++) {
        if (o.n + 16 >= cap) { o.f(" ..."); break; }
        o.f(" %.1f", ring_ms[i & (RING - 1)]);
    }
    o.f("\n");
    return o.n;
}

/* names[0..n) of frametimes_*.txt: which to delete so `keep` remain (the
 * oldest by name: the time stamp follows the prefix). Returns the count. */
static inline int prune_plan(const char* const* names, int n, int keep, int* del, int maxdel)
{
    int order[512];
    if (n > 512) n = 512;
    for (int i = 0; i < n; i++) order[i] = i;
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && strcmp(names[order[j]], names[order[j - 1]]) < 0; j--) {
            const int x = order[j]; order[j] = order[j - 1]; order[j - 1] = x;
        }
    int nd = 0;
    for (int i = 0; i < n - keep && nd < maxdel; i++) del[nd++] = order[i];
    return nd;
}

/* At exit: the recorder ran for a while but port_ft_present() was never
 * called, i.e. the renderer's present hook is gone (a renderer patch was
 * regenerated or a different present path is in use). Writes the warning
 * line into buf and returns its length; 0 = nothing to say. */
static const double HOOK_CHECK_S = 60.0;
static inline size_t hook_warning(double run_s, uint64_t presents, char* buf, size_t cap)
{
    if (cap) buf[0] = 0;
    if (presents || !(run_s >= HOOK_CHECK_S)) return 0;
    Out o{ buf, 0, cap };
    o.f("[frametime] WARNING: no frame was presented in %.0f s: the present hook (port_ft_present) is missing "
        "from the renderer, so no frame-time report was recorded\n", run_s);
    return o.n;
}

} /* namespace port_ft */

#if defined(PORT_FRAMETIME_IMPL) && defined(_WIN32)
#include <windows.h>
#pragma comment(lib, "user32.lib")   /* marker key: GetAsyncKeyState, GetForegroundWindow */

struct PortFtConfig {
    const char* title;           /* "Resistance 2" */
    const char* exe_suffix;      /* optional, appended to the file name (R1: the exe name) */
    int (*paused)(void);         /* nonzero: paused on purpose (not recorded) */
    long (*pause_gen)(void);     /* bumped on every pause / resume (NULL = none) */
};

namespace port_ft {
static PortFtConfig g_cfg;
static Stats g_s, g_snap;
static float g_ring_ms[RING];
static volatile LONG64 g_head, g_presents;
static volatile long g_on, g_busy, g_dumping, g_exit_done;
static LONGLONG g_freq, g_t0, g_last;
static long g_last_gen;
static int g_was_paused;
static time_t g_start;
static char g_path[MAX_PATH], g_logs[MAX_PATH], g_stamp[32], g_exe[64], g_reasons[96], g_key_name[32];
static int g_vk;
static char g_marks[MARKS * 1024];         /* the marker lines for the file (as in the log, shortened) */
static volatile long g_nmarks;
static char g_mark_line[MARK_LEN];
static char g_buf[64 * 1024];

static double now_s(LONGLONG* q)
{
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    if (q) *q = c.QuadPart;
    return (double)(c.QuadPart - g_t0) / (double)g_freq;
}

static void add_reason(const char* r)
{
    if (strstr(g_reasons, r)) return;
    const size_t L = strlen(g_reasons);
    snprintf(g_reasons + L, sizeof g_reasons - L, "%s%s", L ? ", " : "", r);
}

/* Write the summary file (any thread; static buffers only, no heap). */
static void dump(const char* reason)
{
    if (!g_on) return;
    for (int i = 0; InterlockedExchange(&g_dumping, 1); i++) {
        if (i >= 200) break;   /* the holder died (ExitProcess killed it): go ahead */
        Sleep(1);
    }
    add_reason(reason);
    memcpy(&g_snap, &g_s, sizeof g_snap);
    if (g_snap.n >= 300 || g_nmarks || g_snap.nstall) {
        Info in{};
        in.title = g_cfg.title;
        in.exe = g_exe;
        in.session = g_stamp;
        in.reasons = g_reasons;
        in.marker_key = g_key_name;
        in.start = g_start;
        in.duration_s = now_s(NULL);
        const size_t n = format_report(&g_snap, &in, g_marks, g_buf, sizeof g_buf);
        HANDLE h = CreateFileA(g_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w; WriteFile(h, g_buf, (DWORD)n, &w, NULL);
            CloseHandle(h);
        }
    }
    InterlockedExchange(&g_dumping, 0);
}

static void marker(void)
{
    const long idx = g_nmarks + 1;
    const uint64_t head = (uint64_t)InterlockedCompareExchange64(&g_head, 0, 0);
    const double t = now_s(NULL);
    const size_t n = format_marker((int)idx, t, g_start, g_ring_ms, head, g_mark_line, sizeof g_mark_line);
    HANDLE e = GetStdHandle(STD_ERROR_HANDLE);
    if (e && e != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(e, g_mark_line, (DWORD)n, &w, NULL); }
    if (idx <= MARKS) {
        /* into the file: each line without the "[frametime] " prefix, at most 1 KB per marker */
        size_t L = strlen(g_marks);
        const size_t end = L + 1000 < sizeof g_marks ? L + 1000 : sizeof g_marks - 1;
        for (const char* p = g_mark_line; *p && L + 3 < end; ) {
            if (!strncmp(p, "[frametime] ", 12)) { p += 12; g_marks[L++] = ' '; g_marks[L++] = ' '; continue; }
            g_marks[L++] = *p++;
        }
        if (L && g_marks[L - 1] != '\n') g_marks[L++] = '\n';
        g_marks[L] = 0;
    }
    InterlockedExchange(&g_nmarks, idx);
    dump("marker");
}

static int our_window_focused(void)
{
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

static void prune_files(int keep)
{
    static char names[512][MAX_PATH];
    static const char* ptrs[512];
    int n = 0;
    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%s\\frametimes_*.txt", g_logs);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (n < 512) { snprintf(names[n], MAX_PATH, "%s", fd.cFileName); ptrs[n] = names[n]; n++; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    int del[512];
    const int nd = prune_plan(ptrs, n, keep, del, 512);
    for (int i = 0; i < nd; i++) {
        char p[MAX_PATH];
        snprintf(p, sizeof p, "%s\\%s", g_logs, names[del[i]]);
        DeleteFileA(p);
    }
}

static DWORD WINAPI writer(LPVOID)
{
    prune_files(KEEP_FILES - 1);   /* this session's file makes it 10 */
    int prev = 0;
    for (;;) {
        Sleep(50);
        if (!g_vk) continue;
        const int down = (GetAsyncKeyState(g_vk) & 0x8000) != 0;
        if (down && !prev && our_window_focused()) marker();
        prev = down;
    }
}

static int parse_key(const char* v, char* name, size_t cap)
{
    if (!v || !*v || !_stricmp(v, "scroll") || !_stricmp(v, "scrolllock")) { snprintf(name, cap, "Scroll Lock"); return VK_SCROLL; }
    if (!_stricmp(v, "pause")) { snprintf(name, cap, "Pause"); return VK_PAUSE; }
    if ((v[0] == 'f' || v[0] == 'F') && v[1] >= '1' && v[1] <= '9') {
        const int f = atoi(v + 1);
        if (f >= 1 && f <= 24) { snprintf(name, cap, "F%d", f); return VK_F1 + f - 1; }
    }
    snprintf(name, cap, "no marker key");
    return 0;
}

static void on_exit_once(void)
{
    if (InterlockedExchange(&g_exit_done, 1)) return;
    if (!g_on) return;
    char w[256];
    const size_t n = hook_warning(now_s(NULL), (uint64_t)g_presents, w, sizeof w);
    HANDLE e = GetStdHandle(STD_ERROR_HANDLE);
    if (n && e && e != INVALID_HANDLE_VALUE) { DWORD wr; WriteFile(e, w, (DWORD)n, &wr, NULL); }
    dump("exit");
}
static void __cdecl atexit_cb(void) { on_exit_once(); }
} /* namespace port_ft */

/* The renderer's per-present hook (declared extern "C" there). */
extern "C" void port_ft_present(void)
{
    using namespace port_ft;
    if (!g_on || InterlockedExchange(&g_busy, 1)) return;
    g_presents++;   /* under g_busy; only the exit check reads it */
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    const LONGLONG now = c.QuadPart;
    const int paused = g_cfg.paused ? g_cfg.paused() : 0;
    const long gen = g_cfg.pause_gen ? g_cfg.pause_gen() : 0;
    if (paused) {
        if (!g_was_paused) g_s.pauses++;
        g_was_paused = 1;
        g_last = 0;
    } else if (!g_last || gen != g_last_gen) {
        if (g_last && !g_was_paused) g_s.pauses++;   /* a pause that began and ended between two frames */
        g_was_paused = 0;
        g_last = now;
    } else {
        const double ms = (double)(now - g_last) * 1000.0 / (double)g_freq;
        const double t = (double)(now - g_t0) / (double)g_freq;
        stats_add(&g_s, ms, t);
        const uint64_t h = (uint64_t)g_head;
        g_ring_ms[h & (RING - 1)] = (float)ms;
        InterlockedExchange64(&g_head, (LONG64)(h + 1));
        g_last = now;
    }
    g_last_gen = gen;
    InterlockedExchange(&g_busy, 0);
}

/* port_crash.h after_report: a crash or hang report was written. */
static void port_ft_on_report(int kind)
{
    port_ft::dump(kind == 0 ? "crash" : "hang");
}

static void NTAPI port_ft_tls(PVOID, DWORD reason, PVOID)
{
    if (reason == DLL_PROCESS_DETACH) port_ft::on_exit_once();
}
/* ExitProcess skips atexit: a TLS callback still runs at DLL_PROCESS_DETACH. */
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:port_ft_tls_cb")
#pragma const_seg(".CRT$XLF")
extern "C" const PIMAGE_TLS_CALLBACK port_ft_tls_cb = port_ft_tls;
#pragma const_seg()

static void port_ft_init(const PortFtConfig& cfg)
{
    using namespace port_ft;
    const char* e = getenv("PORT_FRAMETIME");
    if (e && e[0] == '0') return;
    g_cfg = cfg;
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    g_freq = f.QuadPart; g_t0 = c.QuadPart;
    g_start = time(NULL);
    struct tm lt; localtime_s(&lt, &g_start);
    strftime(g_stamp, sizeof g_stamp, "%Y%m%d_%H%M%S", &lt);

    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    const char* b = strrchr(exe, '\\');
    snprintf(g_exe, sizeof g_exe, "%s", b ? b + 1 : exe);
    /* <port>\build\X.exe -> <port>\logs (as port_crash.h) */
    char d[MAX_PATH]; snprintf(d, sizeof d, "%s", exe);
    if (char* s = strrchr(d, '\\')) *s = 0;
    char up[MAX_PATH]; snprintf(up, sizeof up, "%s\\..\\logs", d);
    if (!GetFullPathNameA(up, sizeof g_logs, g_logs, NULL)) snprintf(g_logs, sizeof g_logs, "%s", up);
    CreateDirectoryA(g_logs, NULL);
    if (cfg.exe_suffix && *cfg.exe_suffix)
        snprintf(g_path, sizeof g_path, "%s\\frametimes_%s_%s.txt", g_logs, g_stamp, cfg.exe_suffix);
    else
        snprintf(g_path, sizeof g_path, "%s\\frametimes_%s.txt", g_logs, g_stamp);

    g_vk = parse_key(getenv("PORT_FT_MARKER_KEY"), g_key_name, sizeof g_key_name);
    g_on = 1;
    atexit(atexit_cb);
    HANDLE th = CreateThread(NULL, 64 * 1024, writer, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (th) {
        SetThreadPriority(th, THREAD_PRIORITY_BELOW_NORMAL);
        CloseHandle(th);
    }
    fprintf(stderr, "[frametime] recording frame times -> logs\\frametimes_%s%s%s.txt (marker key: %s; PORT_FRAMETIME=0 turns it off)\n",
            g_stamp, cfg.exe_suffix && *cfg.exe_suffix ? "_" : "", cfg.exe_suffix && *cfg.exe_suffix ? cfg.exe_suffix : "", g_key_name);
}
#endif /* PORT_FRAMETIME_IMPL && _WIN32 */

#endif /* PORT_FRAMETIME_H */
