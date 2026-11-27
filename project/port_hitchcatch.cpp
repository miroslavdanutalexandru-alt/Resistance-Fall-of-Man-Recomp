/*
 * port_hitchcatch.cpp -- PORT_HITCHCATCH=<ms>, the frame-hitch stack catcher (see port_hitchcatch.h).
 *
 * Identical in the R1 / R2 / R3 ports. Adapted from the approach of the inFAMOUS 2
 * port's hitch catcher (https://github.com/SamiEnazi/infamous2-port, whose
 * toolkit https://github.com/SamiEnazi/ps3recomp branch infamous2 has
 * runtime/diag/ps3_hitchcatch.c, MIT License, Copyright (c) 2026
 * Ned Heller / sp00nznet): QueryThreadCycleTime to skip threads that did not
 * run, the target stopped only for GetThreadContext + a stack copy, and the
 * unwind (RtlVirtualUnwind) done on the copy after the thread runs again.
 *
 * Threads
 *   sampler  every PORT_HITCHCATCH_PERIOD_US (2000) samples the LIVE slots into
 *            their rings, and copies a hitch window out of the rings when the
 *            frame hook asks for one. THREAD_PRIORITY_HIGHEST: it runs ~1% of
 *            the time, and must not be preempted while a target is stopped (a
 *            BELOW_NORMAL sampler would also starve under exactly the load that
 *            makes hitches). It never allocates, takes no lock, prints nothing
 *            and calls no DbgHelp / loader / heap function -- so a target it
 *            stops while that target holds the heap, loader or CRT lock cannot
 *            block it. Between SuspendThread and ResumeThread it only calls
 *            GetThreadContext and copies at most 2 KB of committed stack
 *            ([Rsp, StackBase), never below the TEB's StackLimit, SEH-guarded).
 *   writer   THREAD_PRIORITY_BELOW_NORMAL: thread discovery (OpenThread, ring
 *            allocation), unwinding, guest mapping, DbgHelp, file and log
 *            output. It is never sampled. If it blocks on a lock a stopped
 *            target holds, it waits the few microseconds until the sampler
 *            resumes the target; the sampler never waits for the writer.
 *   frame    the game's frame-loop thread calls port_hitchcatch_frame(): a QPC
 *            read and, on a hitch, a store into a 4-entry mailbox.
 *
 * Environment
 *   PORT_HITCHCATCH=<ms>             frames over <ms> are caught (unset / 0: off; 1..9 -> 40)
 *   PORT_HITCHCATCH_PERIOD_US=<us>   sampling period (default 2000, 250..20000)
 *   PORT_HITCHCATCH_GAP_MS=<ms>      at most one dump per <ms> (default 1000)
 *   PORT_HITCHCATCH_MAX=<n>          dumps per run (default 100)
 *   PORT_HITCHCATCH_MAXMS=<ms>       longer frames are loads / pauses, ignored (default 2000)
 *   PORT_HITCHCATCH_SPURS=0          do not sample the SPURS lane threads
 *   PORT_HITCHCATCH_FILE=<path>      default <exe dir>\..\logs\hitchcatch.txt
 *
 * Build switches: PORT_HC_NO_DISCOVERY (standalone tests) drops the references to
 * the ports' thread-id accessors and g_ps3_host_paused; threads then come only
 * from port_hitchcatch_register() and the frame hook.
 */
#include "port_hitchcatch.h"

#ifndef _WIN32
extern "C" {
volatile int g_port_hc_state = -1;
void port_hitchcatch_frame_slow(long, int) {}
void port_hitchcatch_register(unsigned long, const char*) {}
int port_hitchcatch_stats(port_hc_stats*) { return 0; }
}
#else

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#include <dbghelp.h>
#include "port_dbghelp_lock.h"
#include <intrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

extern "C" volatile int g_port_hc_state = 0;

/* host -> guest: the lifted PPU table (recompiled/ppu_recomp.h layout) and the SPU registry */
struct port_hc_func_entry { uint64_t addr; void* func; const char* name; };
extern "C" const port_hc_func_entry function_table[];
extern "C" const uint64_t function_table_count;
extern "C" uint32_t spu_registry_size(void);
extern "C" int spu_registry_entry(uint32_t i, void** host, uint32_t* ls_addr);

#ifndef PORT_HC_NO_DISCOVERY
extern "C" unsigned long rsx_render_host_tid(void);
extern "C" unsigned long rsx_fifo_host_tid(void);
extern "C" unsigned long spu_raw_host_tid(unsigned n);
extern "C" unsigned long spurs_lane_host_tid(unsigned lane);
extern "C" volatile int g_ps3_host_paused;
#endif

namespace {

constexpr int      HC_SLOTS      = 16;
constexpr int      HC_RING       = 112;          /* x 2 ms = 224 ms of history */
constexpr unsigned HC_STK        = 2048;         /* stack bytes copied per sample */
constexpr unsigned HC_PAD        = 4096;         /* zeroes after a copy: an unwind past it reads 0 */
constexpr int      HC_FORCE_REAL = 16;           /* an idle thread is re-sampled every 16 passes */
constexpr int      HC_REQS       = 4;
constexpr int      HC_DEPTH      = 24;
[[maybe_unused]] constexpr int HC_SPURS_LANES = 4;

struct hc_ent {
    int64_t  t;                 /* QPC */
    uint64_t rip, rsp, rbp;
    uint32_t len;               /* bytes in stk */
    uint8_t  st;                /* 'r' ran since the last pass, 'q' first sample, 'i' did not run but
                                   re-sampled, 'w' did not run, not sampled (its stack is the last
                                   sampled one), 'x' could not be sampled */
    uint8_t  pad[3];
    uint8_t  stk[HC_STK];
};

enum { S_FREE = 0, S_LIVE = 1, S_RETIRE = 2, S_RETIRED = 3 };

struct hc_slot {                /* published by the writer, read by the sampler */
    volatile LONG state;
    volatile LONG serial;
    DWORD         tid;
    HANDLE        h;
    const NT_TIB* tib;          /* read only inside hc_copy_stack's SEH guard */
    char          role[16];
    hc_ent*       ring;         /* allocated once per slot, reused */
};
struct hc_local {               /* sampler only */
    LONG     serial;
    unsigned head;              /* entries written */
    ULONG64  cyc_last;
    int      have_cyc, perturbed, since_real, have_real;
};

struct hc_req { int64_t start, end; long frame; double ms; };

struct hc_dump_thr { char role[16]; DWORD tid; unsigned first, n; };
struct hc_dump {
    hc_req      req;
    int         nthr;
    hc_dump_thr thr[HC_SLOTS];
    hc_ent*     ents;           /* HC_SLOTS * HC_RING */
};

struct hc_reg { volatile LONG st; DWORD tid; char role[16]; };

hc_slot   s_slot[HC_SLOTS];
hc_local  s_loc[HC_SLOTS];
hc_reg    s_regq[32];

hc_req        s_req[HC_REQS];
volatile LONG s_req_w, s_req_r;
hc_dump       s_dump;
volatile LONG s_dump_state;     /* 0 free, 1 ready for the writer */

/* config */
double   s_thr_ms = 40.0, s_max_ms = 2000.0;
unsigned s_period_us = 2000, s_gap_ms = 1000, s_max_dumps = 100;
int      s_spurs = 1;
wchar_t  s_file_path[MAX_PATH];

/* frame hook (frame thread only) */
LARGE_INTEGER s_qpf;
int64_t  s_last_frame, s_last_dump, s_t0;
long     s_frames;
unsigned s_dumps_posted;

volatile DWORD   s_main_tid, s_sampler_tid, s_writer_tid;
volatile int64_t s_pause_qpc;
HANDLE   s_sampler_h, s_writer_ev;
ULONG64  s_cyc_thr = 20000;
uint64_t s_tsc0;
volatile int* s_pause_flag;

port_hc_stats s_st;

int64_t qpc() { LARGE_INTEGER q; QueryPerformanceCounter(&q); return q.QuadPart; }
double  qms(int64_t d) { return (double)d * 1000.0 / (double)s_qpf.QuadPart; }

unsigned env_u(const char* n, unsigned def, unsigned lo, unsigned hi)
{
    const char* e = getenv(n);
    if (!e || !*e) return def;
    const unsigned long v = strtoul(e, NULL, 10);
    return v < lo ? lo : v > hi ? hi : (unsigned)v;
}

void copy_role(char* d, const char* s)
{
    size_t i = 0;
    for (; s && s[i] && i < 15; i++) d[i] = (s[i] == ' ' || s[i] == ';' || s[i] == ':') ? '_' : s[i];
    d[i] = 0;
}

/* ---- sampler ------------------------------------------------------------------ */

/* Out of line so the SEH guard covers a fault inside it. */
/* The TEB is read here, under the guard, on every sample: a thread that is
 * exiting may already have freed it. */
__declspec(noinline) size_t hc_copy_stack(const NT_TIB* tib, uintptr_t rsp, uint8_t* dst)
{
    size_t len = 0;
    __try {
        const uintptr_t base = (uintptr_t)*(void* volatile*)&tib->StackBase;
        const uintptr_t lim = (uintptr_t)*(void* volatile*)&tib->StackLimit;
        if (rsp >= lim && rsp < base) {
            len = base - rsp;
            if (len > HC_STK) len = HC_STK;
            len &= ~(size_t)7;
            const volatile uint64_t* s = (const volatile uint64_t*)rsp;
            uint64_t* d = (uint64_t*)dst;
            for (size_t i = 0; i < len / 8; i++) d[i] = s[i];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        len = 0;
    }
    return len;
}

alignas(16) CONTEXT s_ctx;

/* Stops the thread for GetThreadContext + the stack copy only. */
bool hc_sample(hc_slot* s, hc_ent* e)
{
    if (SuspendThread(s->h) == (DWORD)-1) return false;
    const int64_t t0 = qpc();
    s_ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    const BOOL ok = GetThreadContext(s->h, &s_ctx);
    size_t len = 0;
    if (ok) len = hc_copy_stack(s->tib, (uintptr_t)s_ctx.Rsp, e->stk);
    ResumeThread(s->h);
    const int64_t t1 = qpc();
    const uint64_t ns = (uint64_t)((double)(t1 - t0) * 1e9 / (double)s_qpf.QuadPart);
    s_st.suspends++;
    s_st.susp_ns_total += ns;
    if (ns > s_st.susp_ns_max) s_st.susp_ns_max = ns;
    if (!ok) return false;
    e->t = t0; e->rip = s_ctx.Rip; e->rsp = s_ctx.Rsp; e->rbp = s_ctx.Rbp; e->len = (uint32_t)len;
    return true;
}

void hc_pass()
{
    const int64_t p0 = qpc();
    if (s_pause_flag && *s_pause_flag) s_pause_qpc = p0;
    const DWORD self = s_sampler_tid, writer = s_writer_tid;
    for (int i = 0; i < HC_SLOTS; i++) {
        hc_slot* s = &s_slot[i];
        const LONG st = s->state;
        if (st == S_RETIRE) { InterlockedExchange(&s->state, S_RETIRED); continue; }
        if (st != S_LIVE || s->tid == self || s->tid == writer) continue;
        hc_local* l = &s_loc[i];
        if (l->serial != s->serial) { memset(l, 0, sizeof *l); l->serial = s->serial; }
        hc_ent* e = &s->ring[l->head % HC_RING];
        ULONG64 cyc = 0;
        const BOOL cok = QueryThreadCycleTime(s->h, &cyc);
        const ULONG64 dc = cyc - l->cyc_last;
        /* right after a sample, the suspend itself ran on the target (the kernel
         * delivers it as an APC): that much movement is not "ran" */
        const int ran = cok && l->have_cyc && dc != 0 && !(l->perturbed && dc <= s_cyc_thr);
        if (cok) { l->cyc_last = cyc; l->have_cyc = 1; }
        if (cok && !ran && l->have_real && l->since_real < HC_FORCE_REAL) {
            e->t = qpc(); e->st = 'w'; e->len = 0;
            l->head++; l->since_real++; l->perturbed = 0;
            s_st.skipped_idle++;
            continue;
        }
        if (!hc_sample(s, e)) {
            e->t = qpc(); e->st = 'x'; e->len = 0;
            l->head++; l->perturbed = 1;
            s_st.failed++;
            continue;
        }
        e->st = (!cok || !l->have_real) ? 'q' : ran ? 'r' : 'i';
        if (cok) QueryThreadCycleTime(s->h, &l->cyc_last);   /* the suspend's own cost is not the next pass's run */
        l->head++; l->have_real = 1; l->since_real = 0; l->perturbed = 1;
    }
    const uint64_t ns = (uint64_t)((double)(qpc() - p0) * 1e9 / (double)s_qpf.QuadPart);
    s_st.passes++;
    s_st.pass_ns_total += ns;
    if (ns > s_st.pass_ns_max) s_st.pass_ns_max = ns;
}

/* The window [start, end] of every live ring into s_dump, plus each thread's
 * last real sample before it (the stack of leading 'w' entries). */
void hc_snapshot(const hc_req& r)
{
    if (s_dump_state != 0) { s_st.dumps_busy++; return; }
    s_dump.req = r;
    s_dump.nthr = 0;
    unsigned out = 0;
    for (int i = 0; i < HC_SLOTS; i++) {
        hc_slot* s = &s_slot[i];
        hc_local* l = &s_loc[i];
        if (s->state != S_LIVE || l->serial != s->serial || !l->head) continue;
        const unsigned avail = l->head < (unsigned)HC_RING ? l->head : (unsigned)HC_RING;
        unsigned k = 0, oldest = l->head;
        for (; k < avail; k++) {
            const unsigned idx = l->head - 1 - k;
            const hc_ent* e = &s->ring[idx % HC_RING];
            if (e->t > r.end) continue;
            oldest = idx;
            if (e->t < r.start && e->st != 'w') break;   /* the sampled entry leading 'w's refer to */
        }
        if (oldest == l->head) continue;
        hc_dump_thr* d = &s_dump.thr[s_dump.nthr++];
        memcpy(d->role, s->role, sizeof d->role);
        d->tid = s->tid; d->first = out; d->n = 0;
        for (unsigned idx = oldest; idx < l->head; idx++) {
            const hc_ent* e = &s->ring[idx % HC_RING];
            if (e->t > r.end) break;
            hc_ent* o = &s_dump.ents[out++];
            memcpy(o, e, offsetof(hc_ent, stk) + e->len);
            d->n++;
        }
    }
    InterlockedExchange(&s_dump_state, 1);
    SetEvent(s_writer_ev);
}

void hc_calibrate()
{
    /* what one suspend + resume adds to a sleeping thread's cycle count */
    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ev) return;
    HANDLE h = CreateThread(NULL, 64 * 1024, [](LPVOID p) -> DWORD { WaitForSingleObject((HANDLE)p, INFINITE); return 0; },
                            ev, 0, NULL);
    if (!h) { CloseHandle(ev); return; }
    Sleep(5);
    ULONG64 mx = 0;
    for (int i = 0; i < 16; i++) {
        ULONG64 a = 0, b = 0;
        QueryThreadCycleTime(h, &a);
        if (SuspendThread(h) != (DWORD)-1) {
            CONTEXT c; c.ContextFlags = CONTEXT_CONTROL;
            GetThreadContext(h, &c);
            ResumeThread(h);
        }
        Sleep(1);
        QueryThreadCycleTime(h, &b);
        if (b - a > mx) mx = b - a;
    }
    SetEvent(ev);
    WaitForSingleObject(h, 1000);
    CloseHandle(h); CloseHandle(ev);
    s_cyc_thr = 2 * mx + 2000;
}

DWORD WINAPI hc_sampler_main(LPVOID)
{
    s_sampler_tid = GetCurrentThreadId();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    SetThreadDescription(GetCurrentThread(), L"hitchcatch sampler");
    hc_calibrate();
    HANDLE tmr = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!tmr) tmr = CreateWaitableTimerW(NULL, FALSE, NULL);
    for (;;) {
        LARGE_INTEGER due; due.QuadPart = -(LONGLONG)s_period_us * 10;
        if (tmr && SetWaitableTimer(tmr, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(tmr, INFINITE);
        else Sleep(s_period_us / 1000 ? s_period_us / 1000 : 1);
        hc_pass();
        while (s_req_r != s_req_w) {
            const hc_req r = s_req[s_req_r % HC_REQS];
            _ReadWriteBarrier();
            InterlockedIncrement(&s_req_r);
            hc_snapshot(r);
        }
    }
}

/* ---- writer: threads ---------------------------------------------------------- */

typedef LONG (WINAPI* nqit_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);
struct hc_tbi { LONG exit; PVOID teb; HANDLE pid, tid; ULONG_PTR aff; LONG prio, base; };

void hc_add_thread(DWORD tid, const char* role_in)
{
    if (!tid || tid == s_sampler_tid || tid == s_writer_tid) return;
    char role[16];
    copy_role(role, role_in);
    for (int i = 0; i < HC_SLOTS; i++)
        if (s_slot[i].state == S_LIVE && s_slot[i].tid == tid) return;
    int free_i = -1;
    for (int i = 0; i < HC_SLOTS; i++) {
        hc_slot* s = &s_slot[i];
        if (s->state == S_LIVE && !strcmp(s->role, role)) InterlockedExchange(&s->state, S_RETIRE);   /* the role moved to tid */
        if (s->state == S_FREE && free_i < 0) free_i = i;
    }
    if (free_i < 0) return;
    static nqit_t nqit = (nqit_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION | SYNCHRONIZE, FALSE, tid);
    if (!h) return;
    hc_tbi bi = {};
    if (!nqit || nqit(h, 0 /* ThreadBasicInformation */, &bi, sizeof bi, NULL) != 0 || !bi.teb ||
        WaitForSingleObject(h, 0) == WAIT_OBJECT_0) { CloseHandle(h); return; }
    hc_slot* s = &s_slot[free_i];
    if (!s->ring) {
        s->ring = (hc_ent*)VirtualAlloc(NULL, sizeof(hc_ent) * HC_RING, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!s->ring) { CloseHandle(h); return; }
    }
    s->tid = tid; s->h = h;
    s->tib = (const NT_TIB*)bi.teb;
    copy_role(s->role, role);
    InterlockedIncrement(&s->serial);
    InterlockedExchange(&s->state, S_LIVE);
}

void hc_reap()
{
    for (int i = 0; i < HC_SLOTS; i++) {
        hc_slot* s = &s_slot[i];
        if (s->state == S_LIVE && WaitForSingleObject(s->h, 0) == WAIT_OBJECT_0) InterlockedExchange(&s->state, S_RETIRE);
        if (s->state == S_RETIRED) {
            CloseHandle(s->h); s->h = NULL; s->tid = 0;
            InterlockedExchange(&s->state, S_FREE);
        }
    }
}

#ifndef PORT_HC_NO_DISCOVERY
/* RPCS3's RSX thread (R2 / R3) and the FIFO walker are found by thread name. */
void hc_scan_names(DWORD* rsx, DWORD* walker)
{
    typedef HRESULT (WINAPI* gtd_t)(HANDLE, PWSTR*);
    static gtd_t gtd = (gtd_t)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
    if (!gtd) return;
    typedef LONG (WINAPI* nqsi_t)(ULONG, PVOID, ULONG, PULONG);
    static nqsi_t nqsi = (nqsi_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
    if (!nqsi) return;
    /* only this process's threads: walk SystemProcessInformation */
    static std::vector<uint8_t> buf(1 << 20);
    ULONG need = 0;
    LONG st;
    while ((st = nqsi(5, buf.data(), (ULONG)buf.size(), &need)) == (LONG)0xC0000004) buf.resize(need + (1 << 20));
    if (st != 0) return;
    const DWORD pid = GetCurrentProcessId();
    for (uint8_t* p = buf.data();;) {
        const ULONG next = *(const ULONG*)p, nthr = *(const ULONG*)(p + 4);
        if ((DWORD)(uintptr_t)*(HANDLE const*)(p + 0x50) == pid) {
            for (ULONG i = 0; i < nthr; i++) {
                const uint8_t* t = p + 0x100 + (size_t)i * 0x50;
                const DWORD tid = (DWORD)(uintptr_t)*(HANDLE const*)(t + 0x30);
                HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
                if (!h) continue;
                PWSTR d = NULL;
                if (SUCCEEDED(gtd(h, &d)) && d) {
                    if (!wcscmp(d, L"rsx::thread")) *rsx = tid;
                    else if (!wcscmp(d, L"FIFO walker")) *walker = tid;
                }
                if (d) LocalFree(d);
                CloseHandle(h);
            }
            break;
        }
        if (!next) break;
        p += next;
    }
}
#endif

void hc_housekeep(bool scan)
{
    for (auto& r : s_regq)
        if (r.st == 2) { hc_add_thread(r.tid, r.role); InterlockedExchange(&r.st, 0); }
    hc_reap();
    if (s_main_tid) hc_add_thread(s_main_tid, "main");
#ifndef PORT_HC_NO_DISCOVERY
    const DWORD rt = (DWORD)rsx_render_host_tid(), ft = (DWORD)rsx_fifo_host_tid();
    hc_add_thread(rt, "render");
    if (ft != rt) hc_add_thread(ft, "fifo");
    hc_add_thread((DWORD)spu_raw_host_tid(0), "spu0");
    hc_add_thread((DWORD)spu_raw_host_tid(1), "spu1");
    if (s_spurs)
        for (unsigned k = 0; k < HC_SPURS_LANES; k++) {
            char nm[16]; snprintf(nm, sizeof nm, "spurs%u", k);
            hc_add_thread((DWORD)spurs_lane_host_tid(k), nm);
        }
    if (scan) {
        DWORD rsx = 0, walker = 0;
        hc_scan_names(&rsx, &walker);
        hc_add_thread(rsx, "rsx");
        hc_add_thread(walker, "walker");
    }
#else
    (void)scan;
#endif
}

/* ---- writer: symbols ---------------------------------------------------------- */

struct hc_gmap { uintptr_t h; uint32_t g; int spu; };
std::vector<hc_gmap> s_gmap;
bool s_gmap_built;

void hc_build_gmap()
{
    if (s_gmap_built) return;
    s_gmap_built = true;
    for (uint64_t k = 0; k < function_table_count; k++)
        if (function_table[k].func) s_gmap.push_back({ (uintptr_t)function_table[k].func, (uint32_t)function_table[k].addr, 0 });
    const uint32_t n = spu_registry_size();
    for (uint32_t k = 0; k < n; k++) {
        void* host = NULL; uint32_t ls = 0;
        if (spu_registry_entry(k, &host, &ls) && host) s_gmap.push_back({ (uintptr_t)host, ls, 1 });
    }
    std::sort(s_gmap.begin(), s_gmap.end(), [](const hc_gmap& a, const hc_gmap& b) { return a.h < b.h; });
}

/* The lifted function containing pc, or NULL. .pdata decides when it describes
 * pc (exact: a function that starts at a table entry); without it the
 * [entry, next entry) extent, capped, as PS3_SAMPLE does. */
const hc_gmap* hc_guest(uint64_t pc)
{
    if (s_gmap.empty()) return NULL;
    auto it = std::upper_bound(s_gmap.begin(), s_gmap.end(), (uintptr_t)pc, [](uintptr_t v, const hc_gmap& e) { return v < e.h; });
    if (it == s_gmap.begin()) return NULL;
    const hc_gmap* g = &*(it - 1);
    DWORD64 base = 0;
    if (PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(pc, &base, NULL)) {
        const uintptr_t b = (uintptr_t)(base + f->BeginAddress);
        return b == g->h ? g : NULL;
    }
    const uintptr_t next = it != s_gmap.end() ? it->h : g->h + 0x20000;
    return pc < next && pc - g->h < 0x20000 ? g : NULL;
}

typedef DWORD   (WINAPI* sso_t)(DWORD);
typedef DWORD   (WINAPI* sgo_t)(void);
typedef BOOL    (WINAPI* siw_t)(HANDLE, PCWSTR, BOOL);
typedef DWORD64 (WINAPI* slm_t)(HANDLE, HANDLE, PCWSTR, PCWSTR, DWORD64, DWORD, PMODLOAD_DATA, DWORD);
typedef BOOL    (WINAPI* sfa_t)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
HANDLE const s_symh = (HANDLE)(uintptr_t)0x48434843;   /* our own DbgHelp session (not the crash handler's) */
slm_t s_slm; sfa_t s_sfa;
int   s_sym_ok = -1;
std::vector<uintptr_t> s_sym_mods;
uintptr_t s_exe_lo, s_exe_hi;

struct hc_sym { uint64_t start; std::string name; bool exe; };
std::unordered_map<uint64_t, hc_sym> s_symcache;

void hc_sym_init()
{
    if (s_sym_ok >= 0) return;
    s_sym_ok = 0;
    const uint8_t* b = (const uint8_t*)GetModuleHandleW(NULL);
    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(b + ((const IMAGE_DOS_HEADER*)b)->e_lfanew);
    s_exe_lo = (uintptr_t)b; s_exe_hi = s_exe_lo + nt->OptionalHeader.SizeOfImage;
    HMODULE dh = LoadLibraryW(L"dbghelp.dll");
    if (!dh) return;
    if (g_port_dbghelp_report) { s_sym_ok = -1; return; }   /* retried on the next dump */
    sso_t sso = (sso_t)GetProcAddress(dh, "SymSetOptions");
    sgo_t sgo = (sgo_t)GetProcAddress(dh, "SymGetOptions");
    siw_t siw = (siw_t)GetProcAddress(dh, "SymInitializeW");
    s_slm = (slm_t)GetProcAddress(dh, "SymLoadModuleExW");
    s_sfa = (sfa_t)GetProcAddress(dh, "SymFromAddr");
    if (!sso || !sgo || !siw || !s_slm || !s_sfa) return;
    wchar_t dir[MAX_PATH];
    if (!GetModuleFileNameW(NULL, dir, MAX_PATH)) dir[0] = 0;
    if (wchar_t* sl = wcsrchr(dir, L'\\')) *sl = 0;
    port_dbghelp_lock();
    if (g_port_dbghelp_report) s_sym_ok = -1;
    else {
        /* SymSetOptions is process-wide: add ours, keep the crash reporter's (SYMOPT_LOAD_LINES) */
        sso(sgo() | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
        s_sym_ok = siw(s_symh, dir, FALSE) ? 1 : 0;
    }
    port_dbghelp_unlock();
}

/* by value: the cache is skipped while a crash / hang report runs */
hc_sym hc_symbol(uint64_t pc)
{
    auto it = s_symcache.find(pc);
    if (it != s_symcache.end()) return it->second;
    hc_sym r; r.start = pc; r.exe = pc >= s_exe_lo && pc < s_exe_hi;
    bool cache = true;
    char buf[64];
    HMODULE m = NULL;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)pc, &m) && m) {
        alignas(8) uint8_t sb[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO* si = (SYMBOL_INFO*)sb;
        memset(sb, 0, sizeof sb);
        si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 255;
        DWORD64 disp = 0;
        bool found = false;
        if (s_sym_ok > 0) {
            wchar_t path[MAX_PATH]; MODULEINFO mi = {};
            const bool need_mod = std::find(s_sym_mods.begin(), s_sym_mods.end(), (uintptr_t)m) == s_sym_mods.end();
            const bool have_info = need_mod && GetModuleFileNameW(m, path, MAX_PATH) &&
                                   K32GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof mi);
            /* every DbgHelp call under the process-wide lock; none while a crash / hang report runs */
            port_dbghelp_lock();
            if (g_port_dbghelp_report) cache = false;
            else {
                if (need_mod) {
                    s_sym_mods.push_back((uintptr_t)m);
                    if (have_info) s_slm(s_symh, NULL, path, NULL, (DWORD64)(uintptr_t)m, mi.SizeOfImage, NULL, 0);
                }
                found = s_sfa(s_symh, pc, &disp, si) && si->Name[0];
            }
            port_dbghelp_unlock();
        }
        if (found) {
            r.name.assign(si->Name, si->NameLen < 255 ? si->NameLen : 255);
            r.start = si->Address;
        } else {
            wchar_t path[MAX_PATH]; char mod[MAX_PATH] = "?";
            if (GetModuleFileNameW(m, path, MAX_PATH)) {
                const wchar_t* bn = wcsrchr(path, L'\\');
                WideCharToMultiByte(CP_UTF8, 0, bn ? bn + 1 : path, -1, mod, sizeof mod, NULL, NULL);
            }
            snprintf(buf, sizeof buf, "+0x%llX", (unsigned long long)(pc - (uintptr_t)m));
            r.name = std::string(mod) + buf;
        }
    } else {
        snprintf(buf, sizeof buf, "?%llX", (unsigned long long)pc);
        r.name = buf;
    }
    if (!cache) return r;   /* a report is running: the module+offset name, looked up again next time */
    return s_symcache.emplace(pc, std::move(r)).first->second;
}

/* "func_XXXXXXXX" for a PPU function; the PDB name (or spu_<ls>) for SPU code */
std::string hc_guest_name(const hc_gmap* g)
{
    char b[32];
    if (!g->spu) { snprintf(b, sizeof b, "func_%08X", g->g); return b; }
    const hc_sym& s = hc_symbol(g->h);
    if (s.start == g->h && !s.name.empty() && s.name[0] != '?' && s.name[0] != '+') return s.name;
    snprintf(b, sizeof b, "spu_%05X", g->g);
    return b;
}

/* ---- writer: unwinding --------------------------------------------------------- */

alignas(16) uint8_t s_work[HC_STK + HC_PAD];
alignas(16) CONTEXT s_wctx;

int hc_walk(CONTEXT* c, uintptr_t lo, uintptr_t hi, uint64_t* out, int max)
{
    int n = 0;
    __try {
        while (n < max) {
            const DWORD64 pc = c->Rip, sp = c->Rsp;
            if (!pc) break;
            out[n++] = pc;
            if (sp < lo || sp + 8 > hi) break;
            DWORD64 base = 0;
            PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(pc, &base, NULL);
            if (f) {
                PVOID hd = NULL; DWORD64 est = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, pc, f, c, &hd, &est, NULL);
            } else {
                if (n > 1) break;            /* only the leaf may be frameless */
                c->Rip = *(const DWORD64*)(uintptr_t)sp;
                c->Rsp = sp + 8;
            }
            if (c->Rsp < lo + 8 || c->Rsp > hi || (c->Rsp <= sp && c->Rip == pc)) break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

int hc_frames(const hc_ent* e, uint64_t* out)
{
    if (!e->len) { out[0] = e->rip; return 1; }
    memcpy(s_work, e->stk, e->len);
    memset(s_work + e->len, 0, HC_PAD);
    const uintptr_t olo = (uintptr_t)e->rsp, ohi = olo + e->len;   /* pointers into the copied part */
    const intptr_t delta = (intptr_t)(uintptr_t)s_work - (intptr_t)olo;
    uint64_t* w = (uint64_t*)s_work;
    for (size_t i = 0; i < e->len / 8; i++)
        if (w[i] >= olo && w[i] < ohi) w[i] = (uint64_t)((intptr_t)w[i] + delta);
    memset(&s_wctx, 0, sizeof s_wctx);
    s_wctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    s_wctx.Rip = e->rip;
    s_wctx.Rsp = (DWORD64)(uintptr_t)s_work;
    s_wctx.Rbp = (e->rbp >= olo && e->rbp < ohi) ? (DWORD64)((intptr_t)e->rbp + delta) : e->rbp;
    const int n = hc_walk(&s_wctx, (uintptr_t)s_work, (uintptr_t)s_work + e->len, out, HC_DEPTH);
    if (n <= 0) { out[0] = e->rip; return 1; }
    return n;
}

/* ---- writer: a dump ------------------------------------------------------------- */

struct hc_agg {
    unsigned n = 0;
    std::map<std::string, unsigned> leaf;
    std::string example;
};

FILE* s_file;
std::string s_fname;

void hc_out_file(const std::string& s)
{
    if (!s_file) return;
    fputs(s.c_str(), s_file);
    fflush(s_file);
}

void hc_out_log(const std::string& s)
{
    /* the frame thread may print to stderr too: do not hold its lock at low priority */
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    fputs(s.c_str(), stderr);
    fflush(stderr);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
}

std::string trunc(const std::string& s, size_t n) { return s.size() <= n ? s : s.substr(0, n - 2) + ".."; }

void hc_process(unsigned dump_no)
{
    const int64_t w0 = qpc();
    hc_build_gmap();
    hc_sym_init();
    const hc_req& r = s_dump.req;
    char b[512];
    snprintf(b, sizeof b, "[hitch] %.0f ms frame %ld:", r.ms, r.frame);
    std::string line = b;
    snprintf(b, sizeof b, "\n=== hitch %u: %.1f ms, frame %ld, ended t=%.3f s; window %.1f ms before the frame end\n",
             dump_no, r.ms, r.frame, qms(r.end - s_t0) / 1000.0, qms(r.end - r.start));
    std::string file = b;
    int nthr_out = 0;
    int order[HC_SLOTS];
    for (int ti = 0; ti < s_dump.nthr; ti++) order[ti] = ti;
    static const char* const ranks[] = { "main", "rsx", "render", "walker", "fifo" };
    auto rank = [](const char* role) {
        for (int i = 0; i < 5; i++) if (!strcmp(role, ranks[i])) return i;
        return 5;
    };
    std::stable_sort(order, order + s_dump.nthr, [&](int x, int y) { return rank(s_dump.thr[x].role) < rank(s_dump.thr[y].role); });
    for (int oi = 0; oi < s_dump.nthr; oi++) {
        const hc_dump_thr& d = s_dump.thr[order[oi]];
        std::map<std::string, hc_agg> agg;
        unsigned nwin = 0, nran = 0, nunk = 0;
        const hc_ent* real = NULL;
        std::vector<uint64_t> fr(HC_DEPTH);
        int nfr = 0;
        std::string key, leaf, stack;
        for (unsigned k = 0; k < d.n; k++) {
            const hc_ent* e = &s_dump.ents[d.first + k];
            if (e->st != 'w' && e->st != 'x' && e != real) {
                real = e;
                nfr = hc_frames(e, fr.data());
                /* key: the innermost lifted function, else the innermost frame in the exe, else the leaf */
                const hc_gmap* g = NULL;
                int gi = -1;
                for (int i = 0; i < nfr && !g; i++) if ((g = hc_guest(fr[i]))) gi = i;
                if (!g && e->len) {   /* unwind did not reach lifted code: nearest lifted return address on the stack */
                    const uint64_t* w = (const uint64_t*)e->stk;
                    for (unsigned i = 0; i < e->len / 8 && !g; i++) g = hc_guest(w[i]);
                }
                const hc_sym& ls = hc_symbol(fr[0]);
                const hc_gmap* lg = hc_guest(fr[0]);
                leaf = lg ? hc_guest_name(lg) : ls.name;
                if (g) key = hc_guest_name(g) + (gi < 0 ? "~" : "");
                else {
                    key.clear();
                    for (int i = 0; i < nfr && key.empty(); i++) { const hc_sym& s = hc_symbol(fr[i]); if (s.exe) key = s.name; }
                    if (key.empty()) key = leaf;
                }
                stack.clear();
                for (int i = 0; i < nfr && i < 8; i++) {
                    const hc_gmap* fg = hc_guest(fr[i]);
                    if (i) stack += " < ";
                    stack += trunc(fg ? hc_guest_name(fg) : hc_symbol(fr[i]).name, 60);
                }
            }
            if (e->t < r.start) continue;
            nwin++;
            if (!real || e->st == 'x') { nunk++; continue; }
            if (e->st == 'r' || e->st == 'q') nran++;
            hc_agg& a = agg[key];
            a.n++;
            if (leaf != key && leaf + "~" != key) a.leaf[leaf]++;
            if (a.example.empty()) a.example = stack;
        }
        if (!nwin) continue;
        std::vector<std::pair<std::string, const hc_agg*>> v;
        for (auto& kv : agg) v.push_back({ kv.first, &kv.second });
        std::sort(v.begin(), v.end(), [](const auto& x, const auto& y) { return x.second->n > y.second->n; });
        snprintf(b, sizeof b, "%s %s(ran %u%%): ", nthr_out ? ";" : "", d.role, nwin ? nran * 100 / nwin : 0);
        line += b;
        snprintf(b, sizeof b, "%s tid %lu: %u samples, ran %u%%%s\n", d.role, (unsigned long)d.tid, nwin,
                 nwin ? nran * 100 / nwin : 0, nunk ? " (some not sampled)" : "");
        file += b;
        for (size_t i = 0; i < v.size() && i < 8; i++) {
            const unsigned pct = v[i].second->n * 100 / nwin;
            std::string top_leaf;
            unsigned top_n = 0;
            for (auto& lk : v[i].second->leaf) if (lk.second > top_n) { top_n = lk.second; top_leaf = lk.first; }
            if (i < 3) {
                snprintf(b, sizeof b, "%s%s %u%%", i ? ", " : "", trunc(v[i].first, 48).c_str(), pct);
                line += b;
                if (top_n * 2 >= v[i].second->n && !top_leaf.empty()) line += " [" + trunc(top_leaf, 32) + "]";
            }
            snprintf(b, sizeof b, "  %3u%%  %s\n", pct, v[i].first.c_str());
            file += b;
            for (auto& lk : v[i].second->leaf) {
                snprintf(b, sizeof b, "          leaf %3u%% %s\n", lk.second * 100 / nwin, trunc(lk.first, 120).c_str());
                file += b;
            }
            file += "          e.g. " + v[i].second->example + "\n";
        }
        nthr_out++;
    }
    if (!nthr_out) line += " (no samples in the window)";
    line += "\n";
    const double tsc_s = (double)(__rdtsc() - s_tsc0);
    ULONG64 cyc = 0;
    QueryThreadCycleTime(s_sampler_h, &cyc);
    snprintf(b, sizeof b, "sampler: %llu passes, %llu suspends (avg %.1f us, max %.1f us), %llu idle skips, %.2f%% of one core; "
                          "writer %.1f ms; hitches %llu, dumps %llu, rate-limited %llu\n",
             s_st.passes, s_st.suspends, s_st.suspends ? s_st.susp_ns_total / 1000.0 / (double)s_st.suspends : 0.0,
             s_st.susp_ns_max / 1000.0, s_st.skipped_idle, tsc_s > 0 ? 100.0 * (double)cyc / tsc_s : 0.0,
             qms(qpc() - w0), s_st.hitches, s_st.dumps_written + 1, s_st.dumps_rate_limited);
    file += b;
    hc_out_file(file);
    hc_out_log(line);
}

DWORD WINAPI hc_writer_main(LPVOID)
{
    s_writer_tid = GetCurrentThreadId();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    SetThreadDescription(GetCurrentThread(), L"hitchcatch writer");
    /* append: an exitspawn child (or a second run) must not truncate the parent's report */
    s_file = _wfopen(s_file_path, L"a");
    if (s_file) {
        SYSTEMTIME lt; GetLocalTime(&lt);
        fprintf(s_file, "\n######## hitchcatch: pid %lu, started %04u-%02u-%02u %02u:%02u:%02u\n",
                (unsigned long)GetCurrentProcessId(), lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond);
        fprintf(s_file, "# hitchcatch: frames over %.0f ms; sampling every %u us; dumps at least %u ms apart, at most %u\n"
                        "# per thread: share of the hitch window's samples by innermost lifted function (func_XXXXXXXX = guest PC;\n"
                        "# a trailing ~ = found by stack scan, not unwind), else innermost exe frame; 'leaf' = where RIP was.\n",
                s_thr_ms, s_period_us, s_gap_ms, s_max_dumps);
        fflush(s_file);
    }
    /* load the exe's PDB now rather than at the first hitch: a cold load holds the
     * DbgHelp lock for seconds, and a crash report would then have to wait for it */
    hc_sym_init();
    if (s_sym_ok > 0) hc_symbol((uint64_t)(uintptr_t)&hc_writer_main);
    unsigned n = 0;
    ULONGLONG last_scan = 0;
    for (;;) {
        WaitForSingleObject(s_writer_ev, 1000);
        const ULONGLONG now = GetTickCount64();
        /* the name scan reads every thread's description: every 5 s */
        const bool scan = now - last_scan >= 5000;
        if (scan) last_scan = now;
        hc_housekeep(scan);
        if (s_dump_state == 1) {
            hc_process(++n);
            s_st.dumps_written++;
            InterlockedExchange(&s_dump_state, 0);
        }
    }
}

void hc_start()
{
    s_dump.ents = (hc_ent*)VirtualAlloc(NULL, sizeof(hc_ent) * HC_SLOTS * HC_RING, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    s_writer_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!s_dump.ents || !s_writer_ev) { g_port_hc_state = -1; return; }
#ifndef PORT_HC_NO_DISCOVERY
    s_pause_flag = (volatile int*)&g_ps3_host_paused;
#endif
    s_tsc0 = __rdtsc();
    HANDLE w = CreateThread(NULL, 1024 * 1024, hc_writer_main, NULL, 0, NULL);
    s_sampler_h = CreateThread(NULL, 64 * 1024, hc_sampler_main, NULL, 0, NULL);
    if (w) CloseHandle(w);
    if (!w || !s_sampler_h) g_port_hc_state = -1;
}

} // namespace

extern "C" void port_hitchcatch_frame_slow(long frame_no, int discard)
{
    if (g_port_hc_state == 0) {
        const char* e = getenv("PORT_HITCHCATCH");
        const double ms = e ? atof(e) : 0.0;
        if (!(ms > 0.0)) { g_port_hc_state = -1; return; }
        s_thr_ms = ms < 10.0 ? 40.0 : ms;
        s_period_us = env_u("PORT_HITCHCATCH_PERIOD_US", 2000, 250, 20000);
        s_gap_ms    = env_u("PORT_HITCHCATCH_GAP_MS", 1000, 0, 3600000);
        s_max_dumps = env_u("PORT_HITCHCATCH_MAX", 100, 1, 100000);
        s_max_ms    = (double)env_u("PORT_HITCHCATCH_MAXMS", 2000, 50, 600000);
        { const char* sp = getenv("PORT_HITCHCATCH_SPURS"); s_spurs = !(sp && *sp == '0'); }
        if (!GetEnvironmentVariableW(L"PORT_HITCHCATCH_FILE", s_file_path, MAX_PATH)) {
            wchar_t dir[MAX_PATH];
            if (!GetModuleFileNameW(NULL, dir, MAX_PATH)) dir[0] = 0;
            if (wchar_t* sl = wcsrchr(dir, L'\\')) *sl = 0;
            _snwprintf_s(s_file_path, MAX_PATH, _TRUNCATE, L"%s\\..\\logs", dir);
            CreateDirectoryW(s_file_path, NULL);
            wcscat_s(s_file_path, MAX_PATH, L"\\hitchcatch.txt");
        }
        QueryPerformanceFrequency(&s_qpf);
        s_t0 = s_last_frame = qpc();
        s_main_tid = GetCurrentThreadId();
        g_port_hc_state = 1;
        hc_start();
        if (g_port_hc_state == 1)
            fprintf(stderr, "[hitch] hitchcatch on: frames over %.0f ms -> stacks of the main / RSX / SPU threads "
                            "(sampling every %u us; %ls)\n", s_thr_ms, s_period_us, s_file_path);
        return;
    }
    if (g_port_hc_state != 1) return;
    const int64_t now = qpc(), start = s_last_frame;
    s_last_frame = now;
    s_frames++;
    if (s_pause_qpc >= start) discard = 1;
    const double ms = qms(now - start);
    if (ms < s_thr_ms) return;
    if (discard || ms > s_max_ms) { s_st.discarded++; return; }
    s_st.hitches++;
    if (s_dumps_posted >= s_max_dumps || (s_last_dump && qms(now - s_last_dump) < (double)s_gap_ms)) {
        s_st.dumps_rate_limited++;
        return;
    }
    if (s_req_w - s_req_r >= HC_REQS) { s_st.dumps_busy++; return; }
    hc_req& r = s_req[s_req_w % HC_REQS];
    r.start = start; r.end = now; r.frame = frame_no >= 0 ? frame_no : s_frames; r.ms = ms;
    _ReadWriteBarrier();
    InterlockedIncrement(&s_req_w);
    s_last_dump = now;
    s_dumps_posted++;
}

extern "C" void port_hitchcatch_register(unsigned long tid, const char* role)
{
    if (g_port_hc_state < 0 || !tid) return;
    for (auto& r : s_regq) {
        if (InterlockedCompareExchange(&r.st, 1, 0) != 0) continue;
        r.tid = (DWORD)tid;
        copy_role(r.role, role ? role : "thread");
        InterlockedExchange(&r.st, 2);
        if (s_writer_ev) SetEvent(s_writer_ev);
        return;
    }
}

extern "C" int port_hitchcatch_stats(port_hc_stats* out)
{
    if (g_port_hc_state != 1) return 0;
    *out = s_st;
    ULONG64 c = 0;
    if (s_sampler_h && QueryThreadCycleTime(s_sampler_h, &c)) out->sampler_cycles = c;
    int live = 0;
    for (auto& s : s_slot) live += s.state == S_LIVE;
    out->threads_live = live;
    return 1;
}

#endif
