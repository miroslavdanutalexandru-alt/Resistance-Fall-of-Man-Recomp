/*
 * ps3_ftrace -- a timeline of who waits for whom, frame by frame.
 *
 * PS3_FTRACE=<start s>:<seconds>:<path> records timestamped spans from the PPU
 * syscall dispatcher, the SPU channel waits, the RSX FIFO walker and the Metal
 * backend for <seconds> starting <start s> after the first hook ran (~boot),
 * then writes them to <path> as text for tools/ftrace_report.py:
 *   # ps3_ftrace start_us=<n> end_us=<n> spans=<n> dropped=<n>
 *   T <tid> <name>                         PPU thread names
 *   <kind> <who> <a hex> <b hex> <t0 us> <t1 us>
 *
 * A sampling profile says where CPU time goes; it cannot say why a frame takes
 * 130 ms when no thread is busy for 130 ms. That is a latency chain -- the PPU
 * waiting on SPU tasks waiting on the RSX waiting on the GPU -- and only a
 * timeline shows it.
 *
 * Header-only on purpose: the hooked files are also compiled on their own by
 * self-contained tests, and a new link dependency would break each of them.
 * The state is one weak/selectany object the linker folds into a single copy;
 * the functions are static inline. Off (the default), every hook is a load
 * and a predictable branch. Knobs: PS3_FTRACE_MIN_US (shortest span kept,
 * default 20), PS3_FTRACE_MERGE_US (usleep polls of one thread at one call
 * site closer than this become one span, default 300), PS3_FTRACE_CAP (spans
 * kept, default 4M = 128 MB, allocated only when recording).
 *
 * Trigger-only mode: a <start s> of PS3_FT_TRIGGER_ONLY_S (3600) or more
 * (run.bat's 99999:6:<path>) records nothing on its own. The recorder stays
 * disarmed -- every hook is the same one load and branch as "off", and no
 * buffer exists -- until ps3_ft_trigger() (logs\ftrace.now: F7 / F11 / the
 * auto-profile) allocates the buffer on first use and opens a window from
 * "now"; writing that window disarms it again. A trigger while a window is
 * recording or being written is ignored (logged), so a capture is never cut
 * short or mixed with the next one. Spans that began before the window was
 * armed (hook state left over from an earlier window) are not recorded.
 */
#ifndef PS3EMU_PS3_FTRACE_H
#define PS3EMU_PS3_FTRACE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#  include <windows.h>
#else
#  include <time.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum ps3_ft_kind {
    PS3_FT_SYSCALL     = 1,  /* who = PPU tid, a = syscall number, b = guest lr        */
    PS3_FT_USLEEP      = 2,  /* who = PPU tid, a = usleep calls merged, b = guest lr   */
    PS3_FT_SPU_WAIT    = 3,  /* who = SPU id,  a = channel,      b = LS pc             */
    PS3_FT_SPU_BUSY    = 4,  /* who = SPU id,  a = lifted image, b = SPURS wid (LS 0x1DC) */
    PS3_FT_SEMA        = 5,  /* RSX blocked on ACQUIRE: a = label offset, b = value    */
    PS3_FT_SEMA_GAVEUP = 6,  /* same, ended by a bounded acquire giving up            */
    PS3_FT_DRAIN       = 7,  /* one RSX FIFO drain pass, a = 1 if it ended on an ACQUIRE */
    PS3_FT_FLIP_FIFO   = 8,  /* instant: the RSX reached a flip, a = buffer            */
    PS3_FT_FLIP_REQ    = 9,  /* instant: the guest asked for a flip, a = buffer        */
    PS3_FT_PRESENT     = 10, /* the host presented a guest frame                       */
    PS3_FT_GPU_ENCODE  = 11, /* Metal: CPU encode of the recorded draws, a = 1 present / 0 flush, b = records */
    PS3_FT_GPU_WAIT    = 12, /* Metal: CPU waits for the GPU, a = 1 present / 0 flush / 2 in-flight bound, b = flush reason */
    PS3_FT_READBACK    = 13, /* Metal: render target read back to the CPU, a = w, b = h */
    PS3_FT_GPU_EXEC    = 14, /* Metal: a command buffer's own GPU time, ending when the CPU saw it
                              * complete; who = render passes, a = depth resolves | colour copies << 16,
                              * b = 1 present / 0 flush */
};

#if defined(_MSC_VER)
#  define PS3_FT_SHARED __declspec(selectany)
#else
#  define PS3_FT_SHARED __attribute__((weak))
#endif

typedef struct { uint64_t t0, t1; uint32_t a, b, who, kind; } ps3_ft_rec;

enum { PS3_FT_TIDS = 256 };

enum { PS3_FT_TRIGGER_ONLY_S = 3600 };

typedef struct {
    int on;              /* 0 unknown, 1 initialising, 2 recording, 3 off/done/disarmed,
                          * 4 busy: a window is being written or (re-)armed */
    int dumped;          /* a window has been written */
    int cfg;             /* PS3_FTRACE parsed: thread names kept, ps3_ft_trigger() can arm */
    uint32_t n, cap, dropped;
    uint64_t base, start, end, min, merge;
    uint64_t armed;      /* when the window was armed; spans starting earlier are stale */
    ps3_ft_rec* buf;
    char path[1024];
    struct { uint64_t t0, t1; uint32_t lr, n; } us[PS3_FT_TIDS];   /* usleep run being merged */
    char names[PS3_FT_TIDS][28];
} ps3_ft_state;

PS3_FT_SHARED ps3_ft_state g_ps3_ft = { 0 };

static inline uint64_t ps3_ft_now(void)
{
#ifdef _WIN32
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static inline void ps3_ft_dump(void)
{
    ps3_ft_state* s = &g_ps3_ft;
    if (!s->buf) return;   /* trigger-only mode, never triggered: nothing recorded */
    /* Only a recording window is written, once: 2 -> 4 (busy) until the file is closed. */
    int expect = 2;
    if (!__atomic_compare_exchange_n(&s->on, &expect, 4, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    /* Close the open usleep runs; their threads may still be in them. */
    for (uint32_t t = 0; t < PS3_FT_TIDS; t++) {
        if (!s->us[t].n) continue;
        const uint32_t i = __atomic_fetch_add(&s->n, 1, __ATOMIC_RELAXED);
        if (i >= s->cap) break;
        ps3_ft_rec r = { s->us[t].t0, s->us[t].t1, s->us[t].n, s->us[t].lr, t, PS3_FT_USLEEP };
        s->buf[i] = r;
    }
    uint32_t n = __atomic_load_n(&s->n, __ATOMIC_ACQUIRE);
    if (n > s->cap) n = s->cap;
    FILE* f = fopen(s->path, "w");
    if (!f) {
        fprintf(stderr, "[ftrace] cannot write %s\n", s->path);
        __atomic_store_n(&s->dumped, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&s->on, 3, __ATOMIC_RELEASE);
        return;
    }
    fprintf(f, "# ps3_ftrace start_us=%.3f end_us=%.3f spans=%u dropped=%u\n",
            (double)(s->start - s->base) / 1e3, (double)(s->end - s->base) / 1e3, n, s->dropped);
    for (uint32_t t = 0; t < PS3_FT_TIDS; t++)
        if (s->names[t][0]) fprintf(f, "T %u %s\n", t, s->names[t]);
    for (uint32_t i = 0; i < n; i++) {
        const ps3_ft_rec* r = &s->buf[i];
        if (!r->kind || r->t0 < s->armed) continue;   /* claimed but never filled, or a straggler from an earlier window */
        fprintf(f, "%u %u %x %x %.3f %.3f\n", r->kind, r->who, r->a, r->b,
                (double)(r->t0 - s->base) / 1e3, (double)(r->t1 - s->base) / 1e3);
    }
    fclose(f);
    fprintf(stderr, "[ftrace] wrote %u spans (%u dropped) to %s\n", n, s->dropped, s->path);
    __atomic_store_n(&s->dumped, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&s->on, 3, __ATOMIC_RELEASE);   /* disarmed; a trigger may re-arm now */
}

static inline void ps3_ft_dump_atexit(void) { ps3_ft_dump(); }

static inline int ps3_ft_alloc(ps3_ft_state* s)
{
    s->buf = (ps3_ft_rec*)calloc(s->cap, sizeof(ps3_ft_rec));
    if (!s->buf) { fprintf(stderr, "[ftrace] no memory for %u spans\n", s->cap); return 0; }
    atexit(ps3_ft_dump_atexit);   /* a fast guest exit that skips atexit calls ps3_ft_dump itself (sysPrxForUser.c) */
    return 1;
}

/* First hook: read PS3_FTRACE once. Hooks see "off" while another thread is
 * still setting up, which only loses spans before the window anyway. */
static inline int ps3_ft_init(void)
{
    ps3_ft_state* s = &g_ps3_ft;
    int expect = 0;
    if (!__atomic_compare_exchange_n(&s->on, &expect, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return expect == 2;
    int on = 3;
    const char* e = getenv("PS3_FTRACE");
    double start = 0, secs = 0;
    int used = 0;
    if (e && *e) {
        if (sscanf(e, "%lf:%lf:%n", &start, &secs, &used) < 2 || !used || secs <= 0) {
            fprintf(stderr, "[ftrace] PS3_FTRACE=<start s>:<seconds>:<path>, got '%s'\n", e);
        } else {
            snprintf(s->path, sizeof s->path, "%s", e + used);
            const char* m = getenv("PS3_FTRACE_MIN_US");
            s->min = (m ? strtoull(m, 0, 10) : 20ull) * 1000ull;
            const char* g = getenv("PS3_FTRACE_MERGE_US");
            s->merge = (g ? strtoull(g, 0, 10) : 300ull) * 1000ull;
            const char* c = getenv("PS3_FTRACE_CAP");
            s->cap = c ? (uint32_t)strtoul(c, 0, 10) : 4u << 20;
            s->base = ps3_ft_now();
            s->cfg = 1;
            if (start >= PS3_FT_TRIGGER_ONLY_S) {
                fprintf(stderr, "[ftrace] idle until triggered (logs\\ftrace.now: F7 / F11); %u-span buffer allocated then\n", s->cap);
            } else if (ps3_ft_alloc(s)) {
                s->armed = s->base;
                s->start = s->base + (uint64_t)(start * 1e9);
                s->end = s->start + (uint64_t)(secs * 1e9);
                fprintf(stderr, "[ftrace] recording %.1f s from t=%.1f s into %s\n", secs, start, s->path);
                on = 2;
            }
        }
    }
    __atomic_store_n(&s->on, on, __ATOMIC_RELEASE);
    return on == 2;
}

/* The hook guard: one load when off. When recording, the acquire fence pairs
 * with the release store of on = 2 so start/end/buf of the window are visible
 * (no instruction on x86). */
static inline int ps3_ft_on(void)
{
    const int on = __atomic_load_n(&g_ps3_ft.on, __ATOMIC_RELAXED);
    if (__builtin_expect(on == 2, 0)) { __atomic_thread_fence(__ATOMIC_ACQUIRE); return 1; }
    if (__builtin_expect(on != 0, 1)) return 0;
    return ps3_ft_init();
}
#define PS3_FT_ON() ps3_ft_on()

static inline uint64_t ps3_ft_min_ns(void) { return g_ps3_ft.min; }

/* Open a <secs> window from now into <path> (the logs\ftrace.now trigger).
 * Allocates the buffer on the first trigger; re-arms after a written window.
 * Returns 0 when PS3_FTRACE is not set, there is no memory, or a window is
 * still recording / being written (that capture keeps its path and window). */
static inline int ps3_ft_trigger(double secs, const char* path)
{
    ps3_ft_state* s = &g_ps3_ft;
    (void)ps3_ft_on();
    if (!s->cfg) { fprintf(stderr, "[ftrace] not armed -- set PS3_FTRACE=99999:6:<path>\n"); return 0; }
    int expect = 3;
    if (!__atomic_compare_exchange_n(&s->on, &expect, 4, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        if (expect == 2) fprintf(stderr, "[ftrace] already recording -> %s; trigger ignored\n", s->path);
        else             fprintf(stderr, "[ftrace] busy writing the previous capture; trigger ignored\n");
        return 0;
    }
    /* on == 4: hooks are off, no other trigger or dump touches the state. */
    if (!s->buf && !ps3_ft_alloc(s)) { __atomic_store_n(&s->on, 3, __ATOMIC_RELEASE); return 0; }
    uint32_t used = __atomic_load_n(&s->n, __ATOMIC_ACQUIRE);
    if (used > s->cap) used = s->cap;
    memset(s->buf, 0, (size_t)used * sizeof(ps3_ft_rec));   /* no stale kind in claimed-but-unfilled slots */
    __atomic_store_n(&s->n, 0u, __ATOMIC_RELAXED);
    s->dropped = 0;
    memset(s->us, 0, sizeof s->us);
    __atomic_store_n(&s->dumped, 0, __ATOMIC_RELAXED);
    snprintf(s->path, sizeof s->path, "%s", path);
    const uint64_t now = ps3_ft_now();
    s->armed = now; s->start = now; s->end = now + (uint64_t)(secs * 1e9);
    __atomic_store_n(&s->on, 2, __ATOMIC_RELEASE);
    fprintf(stderr, "[ftrace] recording %.1f s now -> %s\n", secs, s->path);
    return 1;
}

static inline void ps3_ft_span(int kind, uint32_t who, uint32_t a, uint32_t b,
                               uint64_t t0, uint64_t t1)
{
    ps3_ft_state* s = &g_ps3_ft;
    if (__atomic_load_n(&s->on, __ATOMIC_ACQUIRE) != 2) return;
    if (t1 < s->start || t0 < s->armed) return;   /* before the window / stale hook state (e.g. t_ft_busy0) */
    if (t0 > s->end) { ps3_ft_dump(); return; }
    const uint32_t i = __atomic_fetch_add(&s->n, 1, __ATOMIC_RELAXED);
    if (i >= s->cap) { __atomic_fetch_add(&s->dropped, 1, __ATOMIC_RELAXED); return; }
    ps3_ft_rec r = { t0, t1, a, b, who, (uint32_t)kind };
    s->buf[i] = r;
}

/* sys_timer_usleep polls come tens of thousands a second; consecutive sleeps of
 * one thread from one call site within PS3_FTRACE_MERGE_US become one span.
 * Only the sleeping thread touches its slot. */
static inline void ps3_ft_usleep(uint32_t tid, uint32_t lr, uint64_t t0, uint64_t t1)
{
    ps3_ft_state* s = &g_ps3_ft;
    if (tid >= PS3_FT_TIDS || __atomic_load_n(&s->on, __ATOMIC_ACQUIRE) != 2) return;
    if (s->us[tid].n && s->us[tid].lr == lr && t0 - s->us[tid].t1 <= s->merge) {
        s->us[tid].t1 = t1; s->us[tid].n++;
        return;
    }
    if (s->us[tid].n)
        ps3_ft_span(PS3_FT_USLEEP, tid, s->us[tid].n, s->us[tid].lr, s->us[tid].t0, s->us[tid].t1);
    s->us[tid].t0 = t0; s->us[tid].t1 = t1; s->us[tid].lr = lr; s->us[tid].n = 1;
}

/* A PPU thread's name, for the report's labels. */
static inline void ps3_ft_name(uint32_t tid, const char* name)
{
    if (tid >= PS3_FT_TIDS || !name) return;
    (void)PS3_FT_ON();
    if (!g_ps3_ft.cfg) return;   /* also while disarmed: a triggered window needs the labels */
    snprintf(g_ps3_ft.names[tid], sizeof g_ps3_ft.names[tid], "%s", name);
}

#ifdef __cplusplus
}
#endif

#endif
