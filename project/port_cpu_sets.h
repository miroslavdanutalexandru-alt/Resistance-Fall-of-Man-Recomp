/* port_cpu_sets.h -- hybrid-CPU thread placement (Intel 12th gen+ / Core Ultra P + E cores), the same rules in all
 * three Resistance ports:
 *
 *   - the threads the frame rate hangs on (main PPU thread, raw SPUs, SPURS lanes, RSX FIFO / render / engine
 *     threads, the audio mixer) get thread CPU sets = the usable P-cores (SetThreadSelectedCpuSets) and per-thread
 *     power throttling (EcoQoS) off unless PORT_ECOQOS=1. CPU sets are not a soft hint: combined with an affinity
 *     mask the thread runs only on the intersection (Windows falls back to the affinity when they do not
 *     overlap), and they reserve nothing -- other threads still run there. So the hot threads are budgeted by the
 *     usable P-cores (port_pcore_role_ok, about 2 hot threads per P-core): "usable" = physical P-cores of the main
 *     thread's group inside the mask the hot threads may run in -- the process affinity, or the core keeper's
 *     "rest" mask while it hard-pins main / FIFO / raw SPU 0 (port_pcore_query's mask). Below 2 usable P-cores
 *     nothing is placed. Threads the keeper hard-pins get no CPU sets (PORT_PCORE_PINNED: EcoQoS / priority only);
 *   - everything else (audio/video decode, file IO, shader compilers, monitors) is left unrestricted;
 *   - a CPU whose logical processors all share one EfficiencyClass (every non-hybrid CPU, AMD multi-CCD included)
 *     is left untouched: no-op.
 *
 * Switches (port_native_parse_bool rules):
 *   PORT_CPU_PCORES      default on; off = no P-core preference. PORT_PCORE_HOT is the older name (read when
 *                        PORT_CPU_PCORES is unset or not a boolean word). PORT_THREAD_PLACEMENT=off also turns it off.
 *   PORT_THREAD_PRIORITY default off; on = main and RSX threads at THREAD_PRIORITY_ABOVE_NORMAL (only raised, never
 *                        lowered; any CPU, also with PORT_CPU_PCORES=off;
 *                        PORT_THREAD_PLACEMENT=off turns it off). Opt-in: a raised spinner can starve
 *                        normal threads on 4-core CPUs.
 *
 * The CPU-set selection (port_pcore_plan_build) is plain code over a GetSystemCpuSetInformation buffer, so
 * project/tests/port_cpu_sets_test.c checks it with mocked hybrid, homogeneous, multi-group (> 64 CPU), small hybrid
 * (2P + 4E; 2P + 8E with the keeper's rest mask) and affinity-restricted layouts.
 * Shared file: keep the copies in ResistancePort, Resistance2Port and Resistance3Port identical. */
#pragma once
#include "port_native_summary.h"

#define PORT_PCORE_MAX_IDS 256

typedef struct port_pcore_plan {
    int hybrid;             /* EfficiencyClass differs between logical processors */
    int pcores, ecores;     /* physical cores of the whole machine, every group (ecores 0 when not hybrid): log only */
    int usable;             /* physical P-cores with a selected set (group, mask, not reserved): the gates use this */
    int llc;                /* last-level-cache domains (AMD CCX/CCD); informational */
    int groups;             /* processor groups seen */
    unsigned n;             /* selected CPU set ids: P-cores of the main thread's group inside the mask */
    unsigned long ids[PORT_PCORE_MAX_IDS];
    const char* why_off;    /* NULL = placement applies; else the reason for the [native] line */
} port_pcore_plan;

/* PORT_CPU_PCORES (older name PORT_PCORE_HOT), default on. */
static inline int port_cpu_pcores_env(void)
{
    const int v = port_native_parse_bool(getenv("PORT_CPU_PCORES"), -1);
    return v >= 0 ? v : port_native_parse_bool(getenv("PORT_PCORE_HOT"), 1);
}
static inline int port_thread_placement_env(void) { return port_native_parse_bool(getenv("PORT_THREAD_PLACEMENT"), 1); }
static inline int port_thread_priority_env(void) { return port_native_parse_bool(getenv("PORT_THREAD_PRIORITY"), 0); }

/* Hot-thread roles and the usable P-cores each needs (about 2 hot threads per P-core at most):
 *   main guest thread; RSX FIFO thread / rsx::thread / FIFO walker        >= 2
 *   RSX render / engine thread, raw SPU 0, SPURS lanes 0-2, audio mixer  >= 4
 *   raw SPU 1-3                                                          >= 6
 *   SPURS lanes 3+ (overflow lanes past the 3 SPU identities; 13600K: 6)  >= 8
 * Below 2 the plan is off (why_off) and nothing gets CPU sets. */
enum { PORT_PCORE_MAIN, PORT_PCORE_FIFO, PORT_PCORE_RENDER, PORT_PCORE_RAW, PORT_PCORE_LANE, PORT_PCORE_AUDIO };
static inline int port_pcore_role_ok(const port_pcore_plan* p, int role, int idx)
{
    const int u = p->usable;
    if (role == PORT_PCORE_MAIN || role == PORT_PCORE_FIFO) return u >= 2;
    if (role == PORT_PCORE_RAW) return u >= (idx == 0 ? 4 : 6);
    if (role == PORT_PCORE_LANE) return u >= (idx < 3 ? 4 : 8);
    return u >= 4;
}
/* port_pcore_place_tid flags. PINNED and NOSETS drop CPU sets the thread got earlier (hybrid CPUs only). */
#define PORT_PCORE_PRIO   1     /* PORT_THREAD_PRIORITY=on raises it */
#define PORT_PCORE_PINNED 2     /* the core keeper hard-pins it: no CPU sets, EcoQoS / priority only */
#define PORT_PCORE_NOSETS 4     /* over the usable P-core budget: no CPU sets, EcoQoS / priority only */

/* The [native] entry's switch label: "PORT_CPU_PCORES" when the switch says what is in effect, else
 * "PORT_CPU_PCORES,overridden: <why>" (as the ports' other effective-state entries). */
static inline const char* port_pcore_label(char* buf, size_t cap, int on, const char* why)
{
    if (port_cpu_pcores_env() == on || !why) snprintf(buf, cap, "PORT_CPU_PCORES");
    else snprintf(buf, cap, "PORT_CPU_PCORES,overridden: %s", why);
    return buf;
}

/* The [cpu] start-up line's text. */
static inline const char* port_pcore_describe(const port_pcore_plan* p, char* buf, size_t cap)
{
    if (p->why_off)
        snprintf(buf, cap, "P-core placement off (%s; %d core%s, %d L3 domain%s, %d group%s)", p->why_off,
                 p->pcores + p->ecores, p->pcores + p->ecores == 1 ? "" : "s", p->llc, p->llc == 1 ? "" : "s",
                 p->groups, p->groups == 1 ? "" : "s");
    else
        snprintf(buf, cap, "hybrid CPU %d P + %d E, %d usable: main / SPU / RSX / audio mixer threads on the "
                           "P-cores (%u CPU sets, as many hot threads as the usable P-cores allow), other threads "
                           "unrestricted (PORT_CPU_PCORES=off disables)",
                 p->pcores, p->ecores, p->usable, p->n);
    return buf;
}

#ifdef _WIN32
#include <windows.h>

/* buf/len: a GetSystemCpuSetInformation result. group/affinity: the processor group and the mask the hot threads
 * may run in (process affinity, or the core keeper's rest mask). P-cores = the highest EfficiencyClass. CPU sets
 * reserved for another process (Allocated without AllocatedToTargetProcess) are never selected. */
static inline void port_pcore_plan_build(const void* buf, unsigned long len, unsigned short group,
                                         unsigned long long affinity, port_pcore_plan* p)
{
    unsigned keys[PORT_PCORE_MAX_IDS], ukeys[PORT_PCORE_MAX_IDS], llcs[64], grps[64];
    int nk = 0, nu = 0, nl = 0, ng = 0, any = 0;
    unsigned char maxe = 0, mine = 255;
    const unsigned char* b = (const unsigned char*)buf;
    memset(p, 0, sizeof *p);
    for (unsigned long o = 0; b && o + 8 <= len;) {
        const SYSTEM_CPU_SET_INFORMATION* c = (const SYSTEM_CPU_SET_INFORMATION*)(b + o);
        if (c->Size < 8 || o + c->Size > len) { any = 0; break; }   /* malformed: do not act on part of it */
        if (c->Type == CpuSetInformation) {
            any = 1;
            if (c->CpuSet.EfficiencyClass > maxe) maxe = c->CpuSet.EfficiencyClass;
            if (c->CpuSet.EfficiencyClass < mine) mine = c->CpuSet.EfficiencyClass;
        }
        o += c->Size;
    }
    if (!any) { p->why_off = "CPU sets unavailable"; return; }
    p->hybrid = maxe != mine;
    for (unsigned long o = 0; o + 8 <= len;) {
        const SYSTEM_CPU_SET_INFORMATION* c = (const SYSTEM_CPU_SET_INFORMATION*)(b + o);
        if (c->Size < 8 || o + c->Size > len) break;
        o += c->Size;
        if (c->Type != CpuSetInformation) continue;
        const int perf = c->CpuSet.EfficiencyClass == maxe;
        /* physical cores: one per (Group, CoreIndex) -- SMT siblings share it */
        const unsigned key = ((unsigned)c->CpuSet.Group << 8) | c->CpuSet.CoreIndex;
        const unsigned lkey = ((unsigned)c->CpuSet.Group << 8) | c->CpuSet.LastLevelCacheIndex;
        int k = 0;
        while (k < nk && keys[k] != key) k++;
        if (k == nk && nk < PORT_PCORE_MAX_IDS) { keys[nk++] = key; if (perf || !p->hybrid) p->pcores++; else p->ecores++; }
        k = 0; while (k < nl && llcs[k] != lkey) k++;
        if (k == nl && nl < 64) llcs[nl++] = lkey;
        k = 0; while (k < ng && grps[k] != c->CpuSet.Group) k++;
        if (k == ng && ng < 64) grps[ng++] = c->CpuSet.Group;
        if (!p->hybrid || !perf || c->CpuSet.Group != group || c->CpuSet.LogicalProcessorIndex >= 64) continue;
        if (!(affinity >> c->CpuSet.LogicalProcessorIndex & 1)) continue;
        if (c->CpuSet.Allocated && !c->CpuSet.AllocatedToTargetProcess) continue;
        if (p->n >= PORT_PCORE_MAX_IDS) continue;
        p->ids[p->n++] = c->CpuSet.Id;
        k = 0; while (k < nu && ukeys[k] != key) k++;
        if (k == nu) ukeys[nu++] = key;
    }
    p->usable = nu;
    p->llc = nl;
    p->groups = ng;
    if (!p->hybrid) p->why_off = "no hybrid CPU";
    else if (!p->n) p->why_off = "no P-core in the process affinity";
    else if (nu < 2) p->why_off = "fewer than 2 usable P-cores";
}

/* Fills p from this machine and the switches; p->why_off = NULL means placement applies. Group: the calling
 * thread's. Mask: the process affinity (the calling thread may already be pinned), narrowed by `mask` when non-zero
 * -- the core keeper's rest mask while it hard-pins. */
static inline void port_pcore_query(port_pcore_plan* p, unsigned long long mask)
{
    typedef BOOL (WINAPI *gscsi_t)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    gscsi_t g = k32 ? (gscsi_t)GetProcAddress(k32, "GetSystemCpuSetInformation") : NULL;
    GROUP_AFFINITY ga;
    unsigned short grp = 0;
    unsigned long long aff = ~0ull;
    ULONG need = 0;
    BYTE* b = NULL;
    memset(&ga, 0, sizeof ga);
    DWORD_PTR pa = 0, sa = 0;
    if (GetThreadGroupAffinity(GetCurrentThread(), &ga) && ga.Mask) { grp = ga.Group; aff = (unsigned long long)ga.Mask; }
    if (GetProcessAffinityMask(GetCurrentProcess(), &pa, &sa) && pa) aff = (unsigned long long)pa;   /* 0: multi-group */
    if (mask) aff &= mask;
    if (g) {
        g(NULL, 0, &need, GetCurrentProcess(), 0);
        b = need ? (BYTE*)malloc(need) : NULL;
        if (b && !g((PSYSTEM_CPU_SET_INFORMATION)b, need, &need, GetCurrentProcess(), 0)) need = 0;
    }
    port_pcore_plan_build(b, b ? need : 0, grp, aff, p);
    free(b);
    if (!p->why_off && !port_thread_placement_env()) p->why_off = "PORT_THREAD_PLACEMENT=off";
    if (!p->why_off && !port_cpu_pcores_env()) p->why_off = "PORT_CPU_PCORES=off";
}

/* Thread h on the plan's P-cores (not with PORT_PCORE_PINNED / NOSETS: those drop earlier CPU sets, so a later hard
 * pin or a budget change never leaves a thread squeezed into a stale set); per-thread EcoQoS off unless
 * PORT_ECOQOS=1. Returns bit 0: CPU sets applied, bit 1: power throttling off. */
static inline int port_pcore_apply(HANDLE h, const port_pcore_plan* p, int flags)
{
    typedef BOOL (WINAPI *stscs_t)(HANDLE, const ULONG*, ULONG);
    typedef BOOL (WINAPI *sti_t)(HANDLE, THREAD_INFORMATION_CLASS, LPVOID, DWORD);
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    stscs_t s = k32 ? (stscs_t)GetProcAddress(k32, "SetThreadSelectedCpuSets") : NULL;
    sti_t ti = k32 ? (sti_t)GetProcAddress(k32, "SetThreadInformation") : NULL;
    int r = 0;
    if (!h) return 0;
    if ((flags & (PORT_PCORE_PINNED | PORT_PCORE_NOSETS)) && p->hybrid && s) s(h, NULL, 0);
    if (p->why_off || !p->n) return 0;
    if (!(flags & (PORT_PCORE_PINNED | PORT_PCORE_NOSETS)) && s && s(h, (const ULONG*)p->ids, (ULONG)p->n)) r |= 1;
    if (ti && !port_native_parse_bool(getenv("PORT_ECOQOS"), 0)) {
        THREAD_POWER_THROTTLING_STATE pt;
        memset(&pt, 0, sizeof pt);
        pt.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
        pt.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
        pt.StateMask = 0;   /* controlled + off = never EcoQoS */
        if (ti(h, ThreadPowerThrottling, &pt, sizeof pt)) r |= 2;
    }
    return r;
}

/* PORT_THREAD_PRIORITY=on: thread h at ABOVE_NORMAL if it is below that (never lowers). 1 = raised. */
static inline int port_prio_raise(HANDLE h)
{
    if (!h || !port_thread_priority_env()) return 0;
    const int cur = GetThreadPriority(h);
    if (cur == THREAD_PRIORITY_ERROR_RETURN || cur >= THREAD_PRIORITY_ABOVE_NORMAL) return 0;
    return SetThreadPriority(h, THREAD_PRIORITY_ABOVE_NORMAL) ? 1 : 0;
}

/* Hot thread tid: port_pcore_apply and, with PORT_PCORE_PRIO, port_prio_raise. Logs the first 48 as
 * "[cpu] <what> (thread N): ...". Returns port_pcore_apply's bits | 4 when the priority was raised. */
static inline int port_pcore_place_tid(const port_pcore_plan* p, unsigned long tid, const char* what, int flags)
{
    static volatile long logged = 0;
    HANDLE h = tid ? OpenThread(THREAD_SET_INFORMATION | THREAD_SET_LIMITED_INFORMATION | THREAD_QUERY_INFORMATION |
                                THREAD_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)tid) : NULL;
    int r;
    if (!h) return 0;
    r = port_pcore_apply(h, p, flags);
    if ((flags & PORT_PCORE_PRIO) && port_prio_raise(h)) r |= 4;
    CloseHandle(h);
    if (r && InterlockedIncrement(&logged) <= 48)
        fprintf(stderr, "[cpu] %s (thread %lu):%s%s%s\n", what, tid,
                p->why_off ? "" : (flags & PORT_PCORE_PINNED) ? " hard-pinned, no CPU sets" :
                (flags & PORT_PCORE_NOSETS) ? " over the P-core budget, no CPU sets" :
                (r & 1) ? " P-cores" : " P-cores FAILED",
                (r & 2) ? ", power throttling off" : "", (r & 4) ? ", priority above normal" : "");
    return r;
}

/* port_pcore_place_tid for hot role `role` (index idx: raw SPU / lane number): CPU sets only when the plan is on
 * and the usable P-cores admit the role (port_pcore_role_ok). */
static inline int port_pcore_place_role(const port_pcore_plan* p, unsigned long tid, const char* what, int role,
                                        int idx, int flags)
{
    if (p->why_off || !port_pcore_role_ok(p, role, idx)) flags |= PORT_PCORE_NOSETS;
    return port_pcore_place_tid(p, tid, what, flags);
}

#endif /* _WIN32 */
