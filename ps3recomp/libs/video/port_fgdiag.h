/* Frame generation diagnostics (RFOM_FG_DIAG), the R1 port of R2/R3's VKR2FgDiag.h. Plain C, Win32 only;
 * included once per program (rsx_dlssg.inc), and by standalone tests.
 *
 *   RFOM_FG_DIAG          0 / unset: off (no cost beyond one static int test per hook) |
 *                         1: a "[fg-diag]" line per second in run.log; the marker key (and Ctrl+Shift+F2) appends the
 *                            previous 30 and next RFOM_FG_DIAG_FRAMES real presents, one row each, to logs\fgdiag.txt |
 *                         2: as 1, and the dump also starts by itself on a suspect event (FG reset / off, generated
 *                            frames not presented, big spike; at most RFOM_FG_DIAG_AUTO_MAX, 20 s apart) |
 *                         3: the per-second lines, and every real present as a row in logs\fgdiag.txt until exit, with
 *                            "--- event ---" lines (resets with their reason, FG on / off, generated frames not
 *                            presented, DLSS-G status changes, spikes, user markers) and a "# second" line per second.
 *   RFOM_FG_DIAG_KEY      pause (default) | scroll | f1..f24 | none: the marker key (game window in front).
 *   RFOM_FG_DIAG_FRAMES   rows per dump at levels 1-2 (default 120).
 *   RFOM_FG_DIAG_AUTO_MAX automatic dumps at level 2 (default 10).
 *   RFOM_FG_DIAG_MAX_MB   size limit of fgdiag.txt (default 512); recording stops there.
 *   RFOM_FG_DIAG_DIR      log folder (default <exe folder>\..\logs; for the test).
 *
 * File: the previous session's fgdiag.txt becomes fgdiag_prev.txt; when that rename fails (file open in another
 * program) the old file is kept and this session writes fgdiag_<pid>.txt (named in run.log). The opened path is put
 * in the environment (RFOM_FG_DIAG_FILE), so a program started by this one (R1 switches between ResistanceFoM.exe
 * and ResistanceFoMGame.exe on level loads) appends to the same file instead of rotating it away.
 * Threads: the present thread only copies a row into a preallocated queue under a brief spin lock (double buffer of
 * FGD_QCAP items; a full queue drops rows, counted); one long-lived writer thread swaps the queues and writes every
 * 250 ms. Exit: atexit never waits on the writer: between its passes it finishes the file itself (bounded ~20 ms of
 * try-lock, otherwise the writer is told to finish); after an ExitProcess (other threads already gone) a TLS
 * callback writes what is queued without waiting on anything (bounded spin on the queue lock). Never hangs exit.
 * Marker key: the window procedure forwards WM_KEYDOWN (fgd_key_msg), so Pause (make + break sent together) is never
 * missed; without that, GetAsyncKeyState is polled per present with the "pressed since the last call" bit. */
#ifndef PORT_FGDIAG_H
#define PORT_FGDIAG_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FGD_QCAP 8192     /* items per queue (the writer empties one every 250 ms) */
#define FGD_PREV 30       /* rows kept before a dump (levels 1-2) */

/* reset: why the frame generator's history was reset for this frame */
enum { FGD_R_GAP = 1, FGD_R_CUT = 2, FGD_R_HITCH = 4, FGD_R_RES = 8, FGD_R_RESUME = 16, FGD_R_RESIZE = 32,
       FGD_R_SPLIT = 64, FGD_R_UNTAG = 128 };
static const char fgd_r_chr[8] = { 'G', 'C', 'H', 'R', 'O', 'Z', 'S', 'U' };
static const char* const fgd_r_name[8] = { "frame gap / first frame", "camera cut", "hitch", "resolution change",
                                           "generation was off", "window resize", "HUD split changed", "no DLSS inputs" };
/* off: why generation is off for this present */
enum { FGD_W_INPUTS = 1, FGD_W_BG = 2, FGD_W_MIN = 4, FGD_W_HUDP = 8, FGD_W_WARM = 16, FGD_W_NOCTX = 32,
       FGD_W_HUD = 64, FGD_W_RESIZE = 128 };
static const char fgd_w_chr[8] = { 'T', 'B', 'M', 'P', 'W', 'X', 'H', 'Z' };
static const char* const fgd_w_name[8] = { "no 3D scene (menu / movie / loading)", "window in the background", "minimised",
                                           "HUD not separated (unsure pause)", "warming up", "no FSR 3 context",
                                           "HUD split unavailable", "resize" };

typedef struct fgd_row {
    double t_ms;                 /* Present call start (the glue passes QPC ms; rebased to the first row) */
    float dt, cpu, call, sleep;  /* present interval, render-thread time since the last Present returned, Present call,
                                  * Reflex sleep (game thread) since the last row; ms */
    float sp, mo;                /* camera candidates' medoid spread / camera motion (ndc); < 0 = n/a */
    unsigned idx, up, status;    /* real present index (set by fgd_frame), upscaler frame ([FSR] frame N; 0 = none),
                                  * DLSS-G status bits */
    unsigned short m[7];         /* scene draws matched: exact, instance, nearest, camera-split, ambiguous, unmatched,
                                  * common camera term kept (approximate: recorded one frame ahead) */
    unsigned short hud_runs, hud_draws;
    unsigned char reset, off;    /* FGD_R_*, FGD_W_* */
    unsigned char on, n, shown;  /* generating this present; frames per rendered frame when on (1 off); frames
                                  * Streamline actually presented per rendered frame (0 = unknown) */
    char type;                   /* 'D' DLSS-G, 'F' FSR 3, '-' none */
    char cam;                    /* S solved, H held (last solved), U unsolved, N no candidates, R no record, - none */
    char hud;                    /* V separated, F not separated (whole frame generated), H not separated (last HUD area held), P paused, - no split */
    char mk[4];                  /* set by fgd_frame: * spike, J pace jump, ! generated frames not presented */
} fgd_row;

/* ---------------------------------------------------------------- config */
typedef struct { int level, frames, auto_max, vk; unsigned long long cap; char key[16]; } fgd_cfg_t;
static fgd_cfg_t g_fgd_cfg;
static int g_fgd_cfg_read;

static int fgd_env_int(const char* n, int def, int lo, int hi)
{
    const char* e = getenv(n);
    if (!e || !*e) return def;
    int v = atoi(e);
    return v < lo ? lo : v > hi ? hi : v;
}

static int fgd_parse_key(const char* v, char* name, size_t cap)
{
    if (!v || !*v || !_stricmp(v, "pause") || !_stricmp(v, "break")) { snprintf(name, cap, "Pause"); return VK_PAUSE; }
    if (!_stricmp(v, "scroll") || !_stricmp(v, "scrolllock")) { snprintf(name, cap, "Scroll Lock"); return VK_SCROLL; }
    if ((v[0] == 'f' || v[0] == 'F') && v[1] >= '1' && v[1] <= '9') {
        const int f = atoi(v + 1);
        if (f >= 1 && f <= 24) { snprintf(name, cap, "F%d", f); return VK_F1 + f - 1; }
    }
    snprintf(name, cap, "none");
    return 0;
}

static const fgd_cfg_t* fgd_cfg(void)
{
    if (!g_fgd_cfg_read) {
        g_fgd_cfg.level = fgd_env_int("RFOM_FG_DIAG", 0, 0, 3);
        g_fgd_cfg.frames = fgd_env_int("RFOM_FG_DIAG_FRAMES", 120, 10, 100000);
        g_fgd_cfg.auto_max = fgd_env_int("RFOM_FG_DIAG_AUTO_MAX", 10, 0, 1000);
        g_fgd_cfg.cap = (unsigned long long)fgd_env_int("RFOM_FG_DIAG_MAX_MB", 512, 1, 1 << 20) << 20;
        g_fgd_cfg.vk = fgd_parse_key(getenv("RFOM_FG_DIAG_KEY"), g_fgd_cfg.key, sizeof g_fgd_cfg.key);
        g_fgd_cfg_read = 1;
    }
    return &g_fgd_cfg;
}
static int fgd_level(void) { return fgd_cfg()->level; }

/* marker key presses from the window procedure (any thread); g_fgd_key_msgs: the program forwards its key messages */
static volatile LONG g_fgd_key_hits, g_fgd_key_msgs;
static void fgd_key_msgs_on(void) { InterlockedExchange(&g_fgd_key_msgs, 1); }
static void fgd_key_msg(UINT msg, WPARAM wp, LPARAM lp)
{
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && !(lp & (1 << 30)) && g_fgd_cfg_read && g_fgd_cfg.vk && (int)wp == g_fgd_cfg.vk)
        InterlockedIncrement(&g_fgd_key_hits);
}

/* run.log (stderr) without the CRT stream: also usable from the TLS callback */
static void fgd_elog(const char* fmt, ...)
{
    char b[640];
    va_list a; va_start(a, fmt);
    int n = vsnprintf(b, sizeof b - 1, fmt, a);
    va_end(a);
    if (n < 0) return;
    if (n > (int)sizeof b - 2) n = (int)sizeof b - 2;
    b[n++] = 10;
    HANDLE e = GetStdHandle(STD_ERROR_HANDLE);
    if (e && e != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(e, b, (DWORD)n, &w, NULL); }
}

/* ---------------------------------------------------------------- row text */
static void fgd_bits(char* o, unsigned bits, const char* chr)
{
    int n = 0;
    for (int i = 0; i < 8; i++) if (bits & (1u << i)) o[n++] = chr[i];
    if (!n) o[n++] = '-';
    o[n] = 0;
}

static void fgd_names(char* o, size_t cap, unsigned bits, const char* const* nm)
{
    size_t n = 0; o[0] = 0;
    for (int i = 0; i < 8 && n + 2 < cap; i++)
        if (bits & (1u << i)) { int k = snprintf(o + n, cap - n, "%s%s", n ? ", " : "", nm[i]); if (k > 0) n += (size_t)k; }
    if (n >= cap) o[cap - 1] = 0;
    if (!o[0]) snprintf(o, cap, "-");
}

static const char fgd_legend[] =
    "# One row per real (rendered) present. idx: present number in this program | t_ms: since the first row | up: upscaler\n"
    "# frame (\"[FSR] frame N\", [FG-HUD] / [FG-MV] lines; 0 = no 3D scene) | dt_ms: real present interval | cpu_ms: render\n"
    "# thread from the last Present's return to this Present | call: Present call | sleep: Reflex sleep (game thread)\n"
    "# fg: D DLSS-G, F FSR 3 | on: generating | n: frames shown per rendered frame when on | shown: frames Streamline\n"
    "# actually presented per rendered frame (0 = unknown) | status: DLSS-G status bits (1 resolution too low, 2 Reflex not\n"
    "# detected, 4 HDR format, 8 camera constants invalid, 16 GetCurrentBackBufferIndex not called)\n"
    "# reset (history reset sent this frame): G frame gap / first, C camera cut, H hitch, R resolution, O generation was off\n"
    "#   (resume), Z resize, S HUD split changed, U no DLSS inputs\n"
    "# off (why not generating): T no 3D scene (menu / movie / loading), B background, M minimised, P HUD-unsure pause,\n"
    "#   W warming up (2 frames with inputs), X no FSR 3 context, H HUD split unavailable, Z resize\n"
    "# cam: S solved, H last solved held, U unsolved, N no candidates, R no record, - none | spread / motion: medoid spread and\n"
    "#   camera motion (ndc) | ex in nn cs amb un: scene draws matched exact / instance / nearest / camera-split / ambiguous /\n"
    "#   unmatched (as [FG-MV]) | com: common camera term kept (approximate) | hud: V separated, F not separated, H not separated\n"
    "#   (last split's HUD area held), P paused, - no split; runs / draws of the HUD split\n"
    "#   | mk: * spike (> 1.6x average), J pace jump, ! generated frames not presented\n"
    "# --- lines: events before the row they belong to; # second: per-second summary (also [fg-diag] in run.log)\n";
static const char fgd_columns[] =
    "#   idx       t_ms      up  dt_ms cpu_ms  call sleep fg on n shown status reset    off      cam  spread  motion"
    "    ex   in   nn   cs  amb   un  com hud runs draws mk\n";

static int fgd_fmt_row(char* o, size_t cap, const fgd_row* r)
{
    char rs[12], ws[12], sp[16], mo[16];
    fgd_bits(rs, r->reset, fgd_r_chr);
    fgd_bits(ws, r->off, fgd_w_chr);
    if (r->sp >= 0.0f) snprintf(sp, sizeof sp, "%.3g", r->sp); else snprintf(sp, sizeof sp, "-");
    if (r->mo >= 0.0f) snprintf(mo, sizeof mo, "%.3g", r->mo); else snprintf(mo, sizeof mo, "-");
    return snprintf(o, cap, "%7u %10.1f %7u %6.2f %6.2f %5.2f %5.2f  %c  %u %u %5u %6X %-8s %-8s %c %7s %7s"
                            " %5u %4u %4u %4u %4u %4u %4u   %c %4u %5u %s\n",
                    r->idx, r->t_ms, r->up, r->dt, r->cpu, r->call, r->sleep, r->type, r->on, r->n, r->shown, r->status,
                    rs, ws, r->cam ? r->cam : '-', sp, mo, r->m[0], r->m[1], r->m[2], r->m[3], r->m[4], r->m[5], r->m[6],
                    r->hud ? r->hud : '-', r->hud_runs, r->hud_draws, r->mk[0] ? r->mk : "-");
}

/* ---------------------------------------------------------------- stream (writer thread) */
typedef struct { unsigned char kind; fgd_row r; char text[200]; } fgd_item;   /* kind 0 row, 1 text line */

typedef struct {
    volatile LONG lock;
    fgd_item* q[2];
    volatile LONG qn[2];
    volatile LONG fi;                     /* queue the present thread fills */
    volatile LONG accepting, stop, done, busy, dropped, owner;
    HANDLE wake, file;
    unsigned long long bytes, cap;
    unsigned rows, lines;
    LONG drop_seen;
    DWORD drop_log_t;
    int cont, limit_hit;
    char path[MAX_PATH], dir[MAX_PATH], exe[64];
    char buf[256 * 1024];                 /* writer thread */
    char xbuf[64 * 1024];                 /* exit path */
} fgd_stream;

static fgd_stream* volatile g_fgd_s;      /* never freed */
static volatile LONG g_fgd_s_tried;

typedef struct { fgd_stream* s; char* b; size_t cap, n; } fgd_sink;

static void fgd_sink_flush(fgd_sink* k)
{
    if (k->n && k->s->file && k->s->file != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(k->s->file, k->b, (DWORD)k->n, &w, NULL);
        k->s->bytes += w;
    }
    k->n = 0;
}

static void fgd_sink_put(fgd_sink* k, const char* p, size_t n)
{
    if (k->n + n > k->cap) fgd_sink_flush(k);
    if (n > k->cap) n = k->cap;
    memcpy(k->b + k->n, p, n);
    k->n += n;
}

static void fgd_sink_printf(fgd_sink* k, const char* fmt, ...)
{
    char t[1024];
    va_list a; va_start(a, fmt);
    int n = vsnprintf(t, sizeof t, fmt, a);
    va_end(a);
    if (n < 0) return;
    if (n >= (int)sizeof t) n = (int)sizeof t - 1;
    fgd_sink_put(k, t, (size_t)n);
}

static void fgd_lock(fgd_stream* s) { while (InterlockedExchange(&s->lock, 1)) YieldProcessor(); }
static int fgd_lock_timed(fgd_stream* s)
{
    for (int i = 0; i < 100; i++) { if (!InterlockedExchange(&s->lock, 1)) return 1; Sleep(1); }
    return 0;
}
static void fgd_unlock(fgd_stream* s) { InterlockedExchange(&s->lock, 0); }

/* any thread; drops (counted) when the queue is full */
static void fgd_push(int kind, const fgd_row* r, const char* text)
{
    fgd_stream* s = g_fgd_s;
    if (!s || !s->accepting) return;
    fgd_lock(s);
    const LONG f = s->fi, n = s->qn[f];
    if (n < FGD_QCAP) {
        fgd_item* it = &s->q[f][n];
        it->kind = (unsigned char)kind;
        if (r) it->r = *r;
        if (text) { size_t l = strlen(text); if (l >= sizeof it->text) l = sizeof it->text - 1; memcpy(it->text, text, l); it->text[l] = 0; }
        s->qn[f] = n + 1;
    } else s->dropped++;
    fgd_unlock(s);
}

static void fgd_write_items(fgd_stream* s, fgd_sink* k, const fgd_item* it, LONG n)
{
    char line[512];
    for (LONG i = 0; i < n; i++) {
        if (s->bytes + k->n >= s->cap) {
            if (!s->limit_hit) {
                s->limit_hit = 1; s->accepting = 0;
                fgd_sink_printf(k, "=== size limit reached (RFOM_FG_DIAG_MAX_MB=%llu): recording stopped ===\n", s->cap >> 20);
                fgd_elog("[fg-diag] %s reached RFOM_FG_DIAG_MAX_MB=%llu -- recording stopped", s->path, s->cap >> 20);
            }
            return;
        }
        if (it[i].kind == 0) {
            int l = fgd_fmt_row(line, sizeof line, &it[i].r);
            if (l > 0) fgd_sink_put(k, line, (size_t)(l < (int)sizeof line ? l : (int)sizeof line - 1));
            s->rows++;
        } else {
            fgd_sink_put(k, it[i].text, strlen(it[i].text));
            fgd_sink_put(k, "\n", 1);
            s->lines++;
        }
    }
}

static void fgd_drop_note(fgd_stream* s, fgd_sink* k)
{
    const LONG d = s->dropped;
    if (d == s->drop_seen) return;
    fgd_sink_printf(k, "--- %ld rows / lines dropped (queue full; %ld in total) ---\n", (long)(d - s->drop_seen), (long)d);
    s->drop_seen = d;
    const DWORD now = GetTickCount();
    if (!s->drop_log_t || now - s->drop_log_t >= 5000) {
        s->drop_log_t = now ? now : 1;
        fgd_elog("[fg-diag] %ld rows dropped so far (writer behind; queue full)", (long)d);
    }
}

static void fgd_trailer(fgd_stream* s, fgd_sink* k, const char* how)
{
    fgd_sink_printf(k, "=== fg-diag session end (%s): %s pid %lu, %u rows, %u event / comment lines, %ld dropped, %.1f MB ===\n",
                    how, s->exe, (unsigned long)GetCurrentProcessId(), s->rows, s->lines, (long)s->dropped,
                    (double)(s->bytes + k->n) / 1048576.0);
}

static void fgd_logs_dir(char* d, size_t cap)
{
    const char* e = getenv("RFOM_FG_DIAG_DIR");
    if (e && *e) snprintf(d, cap, "%s", e);
    else {
        char exe[MAX_PATH], up[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
        if (!n || n >= sizeof exe) exe[0] = 0;
        char* s = strrchr(exe, '\\'); if (s) *s = 0;
        snprintf(up, sizeof up, "%s\\..\\logs", exe);
        if (!GetFullPathNameA(up, (DWORD)cap, d, NULL)) snprintf(d, cap, "%s", up);
    }
    CreateDirectoryA(d, NULL);
}

static const char* fgd_base(const char* p) { const char* b = strrchr(p, '\\'); return b ? b + 1 : p; }

static int fgd_open(fgd_stream* s)
{
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    const unsigned long pid = (unsigned long)GetCurrentProcessId();
    HANDLE h = INVALID_HANDLE_VALUE;
    char cont[MAX_PATH];
    const DWORD cn = GetEnvironmentVariableA("RFOM_FG_DIAG_FILE", cont, sizeof cont);
    if (cn && cn < sizeof cont) {   /* started by a program that already records: append to its file */
        h = CreateFileA(cont, FILE_APPEND_DATA, share, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz; sz.QuadPart = 0; GetFileSizeEx(h, &sz);
            s->bytes = (unsigned long long)sz.QuadPart; s->cont = 1;
            snprintf(s->path, sizeof s->path, "%s", cont);
        }
    }
    if (h == INVALID_HANDLE_VALUE) {
        char prev[MAX_PATH];
        fgd_logs_dir(s->dir, sizeof s->dir);
        snprintf(s->path, sizeof s->path, "%s\\fgdiag.txt", s->dir);
        snprintf(prev, sizeof prev, "%s\\fgdiag_prev.txt", s->dir);
        if (!MoveFileExA(s->path, prev, MOVEFILE_REPLACE_EXISTING)) {
            const DWORD e = GetLastError();
            if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND) {
                snprintf(s->path, sizeof s->path, "%s\\fgdiag_%lu.txt", s->dir, pid);
                fgd_elog("[fg-diag] could not rename the previous logs\\fgdiag.txt to fgdiag_prev.txt (error %lu; open in another "
                         "program?) -- it is kept; this session writes logs\\%s", (unsigned long)e, fgd_base(s->path));
            }
        }
        HANDLE c = CreateFileA(s->path, GENERIC_WRITE, share, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (c != INVALID_HANDLE_VALUE) {
            CloseHandle(c);
            h = CreateFileA(s->path, FILE_APPEND_DATA, share, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        }
        if (h == INVALID_HANDLE_VALUE) {
            fgd_elog("[fg-diag] cannot create %s (error %lu) -- no frame log", s->path, (unsigned long)GetLastError());
            return 0;
        }
        SetEnvironmentVariableA("RFOM_FG_DIAG_FILE", s->path);   /* programs started from here append to it */
    }
    s->file = h;
    SYSTEMTIME lt; GetLocalTime(&lt);
    const fgd_cfg_t* c = fgd_cfg();
    fgd_sink k = { s, s->buf, sizeof s->buf, 0 };
    fgd_sink_printf(&k, "=== fg-diag %s: %s pid %lu, %04u-%02u-%02u %02u:%02u:%02u, RFOM_FG_DIAG=%d, marker key %s%s ===\n",
                    s->cont ? "continues in the next program" : "session start", s->exe, pid, lt.wYear, lt.wMonth, lt.wDay,
                    lt.wHour, lt.wMinute, lt.wSecond, c->level, c->key, c->level < 3 ? " (dumps only)" : "");
    if (!s->cont) fgd_sink_put(&k, fgd_legend, sizeof fgd_legend - 1);
    fgd_sink_put(&k, fgd_columns, sizeof fgd_columns - 1);
    fgd_sink_flush(&k);
    if (c->level >= 3)
        fgd_elog("[fg-diag] RFOM_FG_DIAG=3: recording every frame to logs\\%s%s until exit (marker key %s, Ctrl+Shift+F2 "
                 "also marks; limit %llu MB)", fgd_base(s->path), s->cont ? " (appending)" : "", c->key, c->cap >> 20);
    return 1;
}

/* The final write: the queued items, the trailer, close. The caller owns the file: the writer is gone (ExitProcess) or
 * not in a pass (atexit took s->owner). Never waits on another thread (bounded try-lock on the queue). */
static void fgd_final(fgd_stream* s, const char* how)
{
    if (s->done || !s->file || s->file == INVALID_HANDLE_VALUE) { s->done = 1; return; }
    s->accepting = 0;
    fgd_sink k = { s, s->xbuf, sizeof s->xbuf, 0 };
    if (fgd_lock_timed(s)) {
        const LONG w = s->fi;
        if (s->busy) {   /* the writer died between taking the other queue and clearing it: write that queue first */
            LONG n = s->qn[w ^ 1]; if (n > FGD_QCAP) n = FGD_QCAP;
            fgd_sink_printf(&k, "--- writer stopped mid-write: its %ld queued items follow (some rows may repeat or be missing) ---\n", (long)n);
            fgd_write_items(s, &k, s->q[w ^ 1], n);
            s->qn[w ^ 1] = 0;
        }
        LONG n = s->qn[w]; if (n > FGD_QCAP) n = FGD_QCAP;
        fgd_write_items(s, &k, s->q[w], n);
        s->qn[w] = 0;
        fgd_unlock(s);
    } else fgd_sink_printf(&k, "--- queue locked at exit: last rows not written ---\n");
    fgd_drop_note(s, &k);
    fgd_trailer(s, &k, how);
    fgd_sink_flush(&k);
    CloseHandle(s->file);
    s->file = NULL;
    s->done = 1;
}

/* s->owner: 1 while the writer opens the file or runs a pass, 2 once an exit path took the file for good */
static DWORD WINAPI fgd_writer(LPVOID p)
{
    fgd_stream* s = (fgd_stream*)p;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    if (!fgd_open(s)) { s->accepting = 0; s->done = 1; InterlockedExchange(&s->owner, 0); return 0; }
    InterlockedExchange(&s->owner, 0);
    fgd_sink k = { s, s->buf, sizeof s->buf, 0 };
    for (;;) {
        WaitForSingleObject(s->wake, 250);
        if (InterlockedCompareExchange(&s->owner, 1, 0) != 0) return 0;   /* an exit path owns the file */
        if (s->done) { InterlockedExchange(&s->owner, 0); return 0; }
        const LONG stopping = s->stop;
        for (int pass = 0; pass < (stopping ? 2 : 1); pass++) {   /* stopping: both queues */
            InterlockedExchange(&s->busy, 1);
            fgd_lock(s);
            const LONG w = s->fi;
            s->fi = w ^ 1;
            fgd_unlock(s);
            LONG n = s->qn[w]; if (n > FGD_QCAP) n = FGD_QCAP;
            fgd_write_items(s, &k, s->q[w], n);
            s->qn[w] = 0;
            fgd_drop_note(s, &k);
            fgd_sink_flush(&k);
            InterlockedExchange(&s->busy, 0);
        }
        if (stopping) {
            fgd_trailer(s, &k, "exit");
            fgd_sink_flush(&k);
            CloseHandle(s->file);
            s->file = NULL;
            s->done = 1;
            InterlockedExchange(&s->owner, 0);
            return 0;
        }
        InterlockedExchange(&s->owner, 0);
    }
}

/* exit(): no waiting on the writer. Between its passes (almost always) the file is taken and finished right here;
 * if a pass is running (a few ms), up to ~20 ms of tries, then the writer is told to stop and finishes it itself,
 * or the TLS callback does at ExitProcess. */
static void fgd_atexit(void)
{
    fgd_stream* s = g_fgd_s;
    if (!s || s->done) return;
    s->accepting = 0;
    for (int i = 0; i < 20; i++) {
        if (InterlockedCompareExchange(&s->owner, 2, 0) == 0) { fgd_final(s, "exit"); return; }
        Sleep(1);
    }
    s->stop = 1;
    SetEvent(s->wake);
}

/* TLS callback, DLL_PROCESS_DETACH: other threads are gone (ExitProcess), or an exit path already finished the file */
static void fgd_exit_dead(void)
{
    fgd_stream* s = g_fgd_s;
    if (!s || s->done) return;
    fgd_final(s, "ExitProcess");
}
static void NTAPI fgd_tls(PVOID h, DWORD reason, PVOID r)
{
    (void)h; (void)r;
    if (reason == DLL_PROCESS_DETACH) fgd_exit_dead();
}
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:port_fgdiag_tls_cb")
#pragma section(".CRT$XLF", long, read)
#ifdef __cplusplus
extern "C"
#endif
__declspec(allocate(".CRT$XLF")) const PIMAGE_TLS_CALLBACK port_fgdiag_tls_cb = fgd_tls;

/* once; the present thread (first row at level 3, first dump at levels 1-2) */
static int fgd_stream_start(void)
{
    if (g_fgd_s) return 1;
    if (InterlockedExchange(&g_fgd_s_tried, 1)) return 0;
    fgd_stream* s = (fgd_stream*)calloc(1, sizeof *s);
    fgd_item* q0 = (fgd_item*)calloc(FGD_QCAP, sizeof *q0);
    fgd_item* q1 = (fgd_item*)calloc(FGD_QCAP, sizeof *q1);
    if (!s || !q0 || !q1) { free(s); free(q0); free(q1); fgd_elog("[fg-diag] out of memory -- no frame log"); return 0; }
    s->q[0] = q0; s->q[1] = q1;
    s->cap = fgd_cfg()->cap;
    s->accepting = 1;
    { char exe[MAX_PATH]; DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
      if (!n || n >= sizeof exe) exe[0] = 0;
      snprintf(s->exe, sizeof s->exe, "%s", fgd_base(exe)); }
    s->wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!s->wake) { fgd_elog("[fg-diag] CreateEvent failed -- no frame log"); return 0; }   /* s leaks: once */
    s->owner = 1;   /* the writer opens the file first */
    g_fgd_s = s;
    HANDLE t = CreateThread(NULL, 0, fgd_writer, s, 0, NULL);
    if (!t) { s->accepting = 0; s->done = 1; s->owner = 0; fgd_elog("[fg-diag] cannot start the writer thread -- no frame log"); return 0; }
    CloseHandle(t);
    atexit(fgd_atexit);
    return 1;
}

/* ---------------------------------------------------------------- present thread */
typedef struct {
    int active, said_off;
    double t0, ema;
    float prev_dt;
    fgd_row last; int have_last;
    unsigned idx, marks, dumps, autos;
    int key_was, dump_left;
    double last_auto_t;
    fgd_row ring[FGD_PREV]; unsigned ring_n;
    /* the current second */
    double sec_t0; unsigned sec_no;
    unsigned s_rows, s_on, s_shown, s_notpres, s_resets, s_rc[8], s_offs, s_oc[8], s_spikes, s_jumps, s_cam[6], s_hudv,
             s_hudn, s_m[7], s_hist[8], s_ndt;
    double s_cpu, s_call, s_sleep, s_dtmax;
    float s_dt[2048];
} fgd_state_t;
static fgd_state_t g_fgd;

/* rows / lines go to the file: always at level 3, during a dump at levels 1-2 */
static int fgd_recording(void) { return g_fgd_s && g_fgd_s->accepting && (fgd_cfg()->level >= 3 || g_fgd.dump_left > 0); }

static void fgd_note(const char* fmt, ...)
{
    if (!fgd_recording()) return;
    char t[200];
    va_list a; va_start(a, fmt);
    vsnprintf(t, sizeof t, fmt, a);
    va_end(a);
    fgd_push(1, NULL, t);
}

static void fgd_dump_start(const char* why)
{
    if (g_fgd.dump_left > 0) { fgd_note("--- %s during the dump ---", why); return; }
    if (!fgd_stream_start()) return;
    g_fgd.dumps++;
    g_fgd.dump_left = fgd_cfg()->frames;
    const unsigned np = g_fgd.ring_n < FGD_PREV ? g_fgd.ring_n : FGD_PREV;
    fgd_note("--- dump #%u (%s): previous %u and next %d presents ---", g_fgd.dumps, why, np, fgd_cfg()->frames);
    for (unsigned i = 0; i < np; i++) fgd_push(0, &g_fgd.ring[(g_fgd.ring_n - np + i) % FGD_PREV], NULL);
    fgd_elog("[fg-diag] dump #%u (%s) -> logs\\fgdiag.txt", g_fgd.dumps, why);
}

/* the marker key, Ctrl+Shift+F2 (rsx_dlssg_hud.inc): a marker line in the stream (level 3) or a dump (1-2) */
static void fgd_user_mark(const char* what, unsigned up)
{
    if (!g_fgd.active) return;
    g_fgd.marks++;
    const unsigned u = up ? up : g_fgd.last.up;
    if (fgd_cfg()->level >= 3) {
        fgd_note("--- user marker --- #%u (%s) after present %u, upscaler frame %u, t %.1f ms", g_fgd.marks, what,
                 g_fgd.last.idx, u, g_fgd.last.t_ms);
        fgd_elog("[fg-diag] user marker #%u (%s) after present %u, upscaler frame %u", g_fgd.marks, what, g_fgd.last.idx, u);
    } else {
        char w[96]; snprintf(w, sizeof w, "user marker #%u: %s at upscaler frame %u", g_fgd.marks, what, u);
        fgd_dump_start(w);
    }
}

static void fgd_auto(const char* why, double t)
{
    if (fgd_cfg()->level != 2 || g_fgd.dump_left > 0 || g_fgd.autos >= (unsigned)fgd_cfg()->auto_max) return;
    if (g_fgd.autos && t - g_fgd.last_auto_t < 20000.0) return;
    g_fgd.autos++; g_fgd.last_auto_t = t;
    char w[120]; snprintf(w, sizeof w, "auto %u: %s", g_fgd.autos, why);
    fgd_dump_start(w);
}

static int fgd_fcmp(const void* a, const void* b) { const float x = *(const float*)a, y = *(const float*)b; return (x > y) - (x < y); }

static void fgd_second(double t)
{
    fgd_state_t* g = &g_fgd;
    if (g->s_rows) {
        float p50 = 0.0f;
        if (g->s_ndt) { qsort(g->s_dt, g->s_ndt, sizeof g->s_dt[0], fgd_fcmp); p50 = g->s_dt[g->s_ndt / 2]; }
        const double secs = (t - g->sec_t0) / 1000.0;
        char rc[64], oc[64]; int rn = 0, on = 0;
        rc[0] = oc[0] = 0;
        for (int i = 0; i < 8; i++) {
            if (g->s_rc[i]) rn += snprintf(rc + rn, sizeof rc - (size_t)rn, "%s%c%u", rn ? " " : "", fgd_r_chr[i], g->s_rc[i]);
            if (g->s_oc[i]) on += snprintf(oc + on, sizeof oc - (size_t)on, "%s%c%u", on ? " " : "", fgd_w_chr[i], g->s_oc[i]);
            if (rn >= (int)sizeof rc) rn = (int)sizeof rc - 1;
            if (on >= (int)sizeof oc) on = (int)sizeof oc - 1;
        }
        char line[1024];
        snprintf(line, sizeof line,
                 "second %u: %u real presents (%.1f/s), %s on %u, shown %u (generated frames not presented in %u), resets %u [%s],"
                 " FG off %u [%s], dt p50 %.1f max %.1f ms (<8 %u <12 %u <16 %u <20 %u <25 %u <33 %u <50 %u 50+ %u), spikes %u,"
                 " pace jumps %u, cpu %.2f call %.2f Reflex sleep %.2f ms, camera S%u H%u U%u N%u R%u, draws ex %u in %u nn %u"
                 " cs %u amb %u un %u com %u, HUD separated %u/%u, upscaler frame %u",
                 g->sec_no, g->s_rows, secs > 0.0 ? g->s_rows / secs : 0.0,
                 g->last.type == 'D' ? "DLSS-G" : g->last.type == 'F' ? "FSR 3" : "FG none", g->s_on, g->s_shown, g->s_notpres,
                 g->s_resets, rc[0] ? rc : "-", g->s_offs, oc[0] ? oc : "-", p50, g->s_dtmax,
                 g->s_hist[0], g->s_hist[1], g->s_hist[2], g->s_hist[3], g->s_hist[4], g->s_hist[5], g->s_hist[6], g->s_hist[7],
                 g->s_spikes, g->s_jumps, g->s_cpu / g->s_rows, g->s_call / g->s_rows, g->s_sleep / g->s_rows,
                 g->s_cam[0], g->s_cam[1], g->s_cam[2], g->s_cam[3], g->s_cam[4],
                 g->s_m[0], g->s_m[1], g->s_m[2], g->s_m[3], g->s_m[4], g->s_m[5], g->s_m[6], g->s_hudv, g->s_hudn, g->last.up);
        fgd_elog("[fg-diag] %s", line);
        if (fgd_recording()) {   /* "# " lines of at most ~190 characters, split after a comma */
            const char* p = line;
            while (*p) {
                size_t n = strlen(p);
                if (n > 190) { n = 190; while (n > 40 && p[n - 1] != ',') n--; if (p[n - 1] != ',') n = 190; }
                char cl[200]; snprintf(cl, sizeof cl, "# %.*s", (int)n, p);
                fgd_push(1, NULL, cl);
                p += n; while (*p == ' ') p++;
            }
        }
    }
    g->sec_no++;
    memset(&g->s_rows, 0, (size_t)((char*)&g->s_dt - (char*)&g->s_rows));
    g->sec_t0 = t;
}

/* The present thread, once per real present, after Present returned. key_ok: the game window is in front. */
static void fgd_frame(fgd_row* r, int key_ok)
{
    fgd_state_t* g = &g_fgd;
    const fgd_cfg_t* c = fgd_cfg();
    if (!c->level) return;
    if (!g->active) {
        InterlockedExchange(&g_fgd_key_hits, 0);   /* presses before recording starts don't count */
        if (r->type == '-') {
            if (!g->said_off) { g->said_off = 1;
                fgd_elog("[fg-diag] RFOM_FG_DIAG=%d: frame generation does not run in this program (front end, or frame generation "
                         "off / unavailable) -- nothing recorded here", c->level); }
            return;
        }
        g->active = 1;
        g->t0 = r->t_ms;
        g->sec_t0 = 0.0;
        fgd_elog("[fg-diag] RFOM_FG_DIAG=%d: per-second [fg-diag] lines%s; marker key %s%s", c->level,
                 c->level >= 3 ? ", every frame to logs\\fgdiag.txt" : c->level == 2 ? ", dumps to logs\\fgdiag.txt on a key or a suspect event"
                                                                     : ", dumps to logs\\fgdiag.txt on a key",
                 c->key, c->vk ? " (Ctrl+Shift+F2 also marks)" : "");
        if (c->level >= 3) fgd_stream_start();
    }
    r->t_ms -= g->t0;
    r->idx = ++g->idx;
    /* marks */
    int n = 0;
    const int big = g->ema > 0.0 && r->dt > 2.0 * g->ema && r->dt > 4.0f;
    if (g->ema > 0.0 && r->dt > 1.6 * g->ema) r->mk[n++] = '*';
    if (g->prev_dt > 0.0f && r->dt > 1.5f * g->prev_dt && r->dt - g->prev_dt > 4.0f) r->mk[n++] = 'J';
    const int notpres = r->on && r->shown && r->shown < r->n;
    if (notpres) r->mk[n++] = '!';
    r->mk[n] = 0;
    const double ema_before = g->ema;
    if (r->dt > 0.0f) g->ema = g->ema > 0.0 ? 0.9 * g->ema + 0.1 * r->dt : r->dt;
    /* events (written when recording; the flags also drive the level-2 automatic dump) */
    const int rec = fgd_recording();
    const fgd_row* l = g->have_last ? &g->last : NULL;
    char t[200], nm[160];
    const int ev_off = !r->on && l && l->on, ev_reset = r->on && r->reset;
    if (rec) {
        if (l && r->type != l->type) fgd_note("--- frame generation backend: %s ---", r->type == 'D' ? "DLSS-G" : r->type == 'F' ? "FSR 3" : "none");
        if (r->on && (!l || !l->on)) fgd_note("--- FG on: %ux (%s) ---", r->n, r->type == 'D' ? "DLSS-G" : "FSR 3");
        if (ev_off) {
            fgd_names(nm, sizeof nm, r->off, fgd_w_name);
            fgd_note("--- FG off (paused): %s ---", nm);
        }
        if (ev_reset) {
            fgd_names(nm, sizeof nm, r->reset, fgd_r_name);
            snprintf(t, sizeof t, "--- FG reset: %s (upscaler frame %u, camera %c", nm, r->up, r->cam ? r->cam : '-');
            size_t k = strlen(t);
            if (r->mo >= 0.0f) k += (size_t)snprintf(t + k, sizeof t - k, ", motion %.3g", r->mo);
            snprintf(t + k, sizeof t - k, ") ---");
            fgd_push(1, NULL, t);
        }
        if (notpres && (!l || l->shown != r->shown || !l->on))
            fgd_note("--- generated frames not presented: Streamline shows %u of %u frames per rendered frame ---", r->shown, r->n);
        if (r->type == 'D' && l && l->type == 'D' && r->status != l->status) {
            static const char* const sn[5] = { "resolution too low", "Reflex not detected", "HDR format", "camera constants invalid",
                                               "GetCurrentBackBufferIndex not called" };
            size_t k = 0; nm[0] = 0;
            for (int i = 0; i < 5; i++) if (r->status & (1u << i)) k += (size_t)snprintf(nm + k, sizeof nm - k, "%s%s", k ? ", " : "", sn[i]);
            fgd_note("--- DLSS-G status 0x%X -> 0x%X (%s) ---", l->status, r->status, nm[0] ? nm : "ok");
        }
        if (big) fgd_note("--- spike: %.1f ms (average %.1f ms) ---", r->dt, ema_before);
    }
    /* the row */
    if (c->level >= 3) fgd_push(0, r, NULL);
    else {
        if (g->dump_left > 0) {
            fgd_push(0, r, NULL);
            if (--g->dump_left == 0) { snprintf(t, sizeof t, "--- dump #%u end ---", g->dumps); fgd_push(1, NULL, t); }
        }
        g->ring[g->ring_n++ % FGD_PREV] = *r;
        if (c->level == 2) {
            if (ev_reset) fgd_auto("FG reset", r->t_ms);
            else if (ev_off) fgd_auto("FG off", r->t_ms);
            else if (notpres) fgd_auto("generated frames not presented", r->t_ms);
            else if (big) fgd_auto("spike", r->t_ms);
        }
    }
    /* marker key */
    if (c->vk) {
        int press;
        if (g_fgd_key_msgs) press = InterlockedExchange(&g_fgd_key_hits, 0) > 0;   /* only sent while the window has focus */
        else {   /* polled: Pause sends make and break together, so also the "pressed since the last call" bit; a held key once */
            const SHORT ks = GetAsyncKeyState(c->vk);
            press = key_ok && !g->key_was && (ks & 0x8001) != 0;
            g->key_was = (ks & 0x8000) != 0;
        }
        if (press) { g->last = *r; g->have_last = 1; fgd_user_mark(c->key, r->up); }
    }
    /* the second */
    g->s_rows++;
    if (r->on) g->s_on++;
    g->s_shown += r->on ? (r->shown ? r->shown : r->n) : 1u;
    if (notpres) g->s_notpres++;
    if (r->on && r->reset) { g->s_resets++; for (int i = 0; i < 8; i++) if (r->reset & (1u << i)) g->s_rc[i]++; }
    if (ev_off) { g->s_offs++; for (int i = 0; i < 8; i++) if (r->off & (1u << i)) g->s_oc[i]++; }
    if (r->mk[0] && strchr(r->mk, '*')) g->s_spikes++;
    if (r->mk[0] && strchr(r->mk, 'J')) g->s_jumps++;
    { const char* cs = "SHUNR"; const char* p = r->cam ? strchr(cs, r->cam) : NULL; g->s_cam[p ? (int)(p - cs) : 5]++; }
    if (r->hud == 'V') g->s_hudv++;
    if (r->hud && r->hud != '-') g->s_hudn++;
    for (int i = 0; i < 7; i++) g->s_m[i] += r->m[i];
    if (r->dt > 0.0f) {
        static const float edge[7] = { 8.0f, 12.0f, 16.0f, 20.0f, 25.0f, 33.4f, 50.0f };
        int b = 0; while (b < 7 && r->dt >= edge[b]) b++;
        g->s_hist[b]++;
        if (g->s_ndt < sizeof g->s_dt / sizeof g->s_dt[0]) g->s_dt[g->s_ndt++] = r->dt;
        if (r->dt > g->s_dtmax) g->s_dtmax = r->dt;
    }
    g->s_cpu += r->cpu; g->s_call += r->call; g->s_sleep += r->sleep;
    g->prev_dt = r->dt;
    g->last = *r; g->have_last = 1;
    if (r->t_ms - g->sec_t0 >= 1000.0) fgd_second(r->t_ms);
}

#endif /* PORT_FGDIAG_H */
