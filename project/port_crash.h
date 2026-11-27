/* Resistance PC ports: crash and hang capture for testers (shared by the R1, R2
 * and R3 ports -- keep the three copies identical).
 *
 * The port's <game>_crash.inc fills a PortCrashConfig and calls
 * port_crash::init() first thing in main(). From then on:
 *
 *   crash  An exception nobody handles (the unhandled-exception filter), a
 *          stack overflow or heap corruption (seen first-chance by a vectored
 *          handler that runs AFTER the runtime's guest-fault handlers, because
 *          the filter chain may never get to run for those), abort() /
 *          std::terminate, a pure virtual call or a CRT invalid parameter.
 *          Writes, in logs\crash:
 *            <TAG>_<yyyymmdd_hhmmss>_crash.dmp  minidump: thread stacks, the
 *                memory they point at, thread info -- not the 4 GB guest space
 *            <TAG>_<yyyymmdd_hhmmss>_crash.txt  readable report: exception code
 *                and address, faulting module + offset, the lifted guest
 *                function (func_XXXXXXXX) when the address is in lifted code,
 *                registers, a native stack trace of every thread, the last
 *                lines of run.log, the env / config summary and the exe identity
 *            crashes.txt                        one line per crash / hang
 *          then hands the exception on: to the filter that was installed before
 *          (chained), and finally to Windows, so the process dies as before.
 *   hang   A watchdog thread: when the game loop (and, with PORT_HANG_DUMP=1,
 *          the renderer thread) makes no progress for a while, writes
 *          <TAG>_<time>_hang.dmp / .txt once per stall; the game keeps running.
 *
 * The report is written by a reporter thread that is started at init, never on
 * the faulting thread: a stack overflow leaves no stack to work with, and a
 * thread that died holding a lock (heap, CRT stream) cannot deadlock the
 * report. Other threads are only suspended long enough to read their registers
 * and copy the top of their stack, then walked from that copy (DbgHelp
 * StackWalk64, unwind data from RtlLookupFunctionEntry, no PDB needed).
 *
 * When a PDB sits next to the exe, frames and the fault address also get
 * {symbol+0xNN file:line} (DbgHelp SymFromAddr, best-effort, not after heap
 * corruption). With PortCrashConfig::guest_state the faulting thread's guest
 * PPU state is captured too (thread id, lr, sp, syscall in flight / last
 * syscall, last syscall callsite). A crash also writes the exception section
 * and the faulting thread's stack to stderr (run.log), not only crashes.txt.
 * With PortCrashConfig::guest_threads every report (crash and hang) also
 * gets a "guest threads" section: per guest thread the syscall it is blocked
 * in, its first argument (the wait object / address), for how long, and the
 * last syscall callsite -- the per-thread heartbeat of a hang.
 *
 * Switches (environment; settings.bat / run.bat):
 *   PORT_CRASH_DUMPS=0 (or <prefix>_CRASH_DUMPS=0)  everything off
 *   PORT_CRASH_DUMPS=full (or <prefix>_CRASH_DUMPS=full, CRASH_DUMP_FULL=1)
 *                                full-memory dumps (several GB)
 *   PORT_HANG_DUMP=1             strict hang watchdog: 20 s without game-loop
 *                                progress, or without a renderer flip while the
 *                                game loop runs (not while paused)
 *   PORT_HANG_DUMP=0             no hang watchdog
 *   (unset)                      the original watchdog: game loop only,
 *                                <prefix>_HANG_S seconds (default 90)
 *   PORT_HANG_S=<s>              the strict watchdog's limit (default 20, >= 5)
 *   PORT_CRASH_KEEP=<n>          dumps kept in logs\crash (default 12)
 *   PORT_CRASH_SYMBOLS=0         no PDB symbol lookup in reports
 *   PORT_CRASH_EXTRA=0           report as before the guest-state / guest-threads / symbols / run.log summary additions
 */
#pragma once
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#include "port_dbghelp_lock.h"   /* DbgHelp is single-threaded: shared with port_hitchcatch.cpp */
#include <tlhelp32.h>
#include <psapi.h>
#include <intrin.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct PortGuestState {                  /* filled on the faulting thread (loads only) */
    int valid;                           /* 0: not a guest thread */
    uint64_t thread_id;                  /* guest PPU thread id */
    uint32_t lr, sp;                     /* guest link register, r1 */
    uint32_t syscall_inflight;           /* lv2 syscall the thread is inside (0 = none) */
    uint32_t syscall_last;               /* last lv2 syscall it made */
    uint32_t callsite;                   /* guest function of the last syscall / HLE call */
};

struct PortCrashConfig {
    const char* tag;                     /* file-name prefix: "R3" -> R3_<time>_crash.dmp */
    const char* title;                   /* "Resistance 3" (report heading) */
    const char* env_prefix;              /* legacy switches: <prefix>_CRASH_DUMPS, <prefix>_HANG_S */
    const char* exe_suffix;              /* optional, appended to file names (R1: the exe name) */
    uint64_t (*main_progress)(void);     /* game-loop counter (frames / flips) */
    uint64_t main_start_min;             /* below this the loop has not started yet (boot, movies) */
    uint64_t (*rsx_progress)(void);      /* renderer-thread progress, 0 = unknown; NULL = none */
    int (*paused)(void);                 /* nonzero: paused on purpose, no progress expected */
    uint8_t* const* vm_base;             /* guest address space: AV addresses shown as guest */
    uint64_t vm_size;
    uint32_t (*guest_resolve)(void* host_pc);   /* host pc -> nearest lifted guest function (0 = none) */
    void (*before_report)(void);         /* crash only, on the reporter thread: e.g. give the display mode back */
    /* optional (NULL = none), on the faulting thread before the report; must
     * not allocate or use much stack (a stack overflow lands here too) */
    void (*guest_state)(PortGuestState* out);
    /* optional (NULL = none), on the reporter / watchdog thread: write a text
     * table of all guest threads into buf (NUL-terminated, at most cap bytes);
     * returns the length. Must only read (the game may still be running). */
    size_t (*guest_threads)(char* buf, size_t cap);
    /* optional (NULL = none), on the reporter thread after each report (kind:
     * 0 crash, 1 hang): e.g. write the frame-time summary (port_frametime.h) */
    void (*after_report)(int kind);
};

namespace port_crash {

enum { KIND_CRASH = 0, KIND_HANG = 1 };
enum { MAX_MODS = 512, MAX_THREADS = 256, SNAP_BYTES = 96 * 1024, LOG_TAIL_LINES = 80 };

typedef BOOL (WINAPI* mdw_t)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                             PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
typedef BOOL (WINAPI* sw64_t)(DWORD, HANDLE, HANDLE, LPSTACKFRAME64, PVOID, PREAD_PROCESS_MEMORY_ROUTINE64,
                              PFUNCTION_TABLE_ACCESS_ROUTINE64, PGET_MODULE_BASE_ROUTINE64, PTRANSLATE_ADDRESS_ROUTINE64);
typedef HRESULT (WINAPI* gtd_t)(HANDLE, PWSTR*);

struct Request {
    int kind;
    EXCEPTION_POINTERS* ep;      /* crash: the faulting thread's exception */
    DWORD tid;                   /* crash: the faulting thread */
    const char* how;             /* "unhandled exception", "abort()", ... */
    char detail[200];            /* hang: what stalled */
    char out_txt[MAX_PATH];      /* result: the .txt written ("" = none) */
    char out_dmp[MAX_PATH];      /* result: the .dmp written ("" = none) */
    PortGuestState guest;        /* crash: the faulting thread's guest state */
};

struct Mod { DWORD64 base, end; char name[64]; };

static PortCrashConfig s_cfg;
static char s_dir[MAX_PATH];             /* <port>\logs\crash */
static char s_root[MAX_PATH];            /* <port> */
static char s_exe[MAX_PATH];
static int s_on, s_full, s_hang_mode, s_hang_s;
static ULONGLONG s_start_ms;
static mdw_t s_mdw;
static sw64_t s_sw64;
static gtd_t s_gtd;
static LPTOP_LEVEL_EXCEPTION_FILTER s_prev_filter;
static void (__cdecl* s_prev_abrt)(int);
static _purecall_handler s_prev_purecall;
static _invalid_parameter_handler s_prev_invparam;
static HANDLE s_req_ev, s_done_ev, s_thread;
static DWORD s_thread_id, s_watch_id;
static SRWLOCK s_lock = SRWLOCK_INIT;
static volatile LONG s_crashed;

/* Best-effort PDB symbols (DbgHelp SymFromAddr), only on the reporter thread,
 * never after heap corruption. PORT_CRASH_SYMBOLS=0 turns them off. */
typedef DWORD (WINAPI* sso_t)(DWORD);
typedef BOOL (WINAPI* si_t)(HANDLE, PCSTR, BOOL);
typedef BOOL (WINAPI* sfa_t)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
typedef BOOL (WINAPI* sgl_t)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64);
static sso_t s_sso;
static si_t s_si;
static sfa_t s_sfa;
static sgl_t s_sgl;
static int s_sym_state;          /* 0 = not tried, 1 = on, -1 = off */
static int s_sym_allowed;        /* per report */
static int s_nosym;              /* PORT_CRASH_SYMBOLS=0 (or PORT_CRASH_EXTRA=0) */
static int s_nodbg;              /* per report: another thread holds the DbgHelp lock -> no DbgHelp call at all */
static DWORD s_dbg_wait_ms;      /* PORT_CRASH_DBGHELP_WAIT_MS; 0 = crash 20 s, hang 30 s */
static int s_noextra;            /* PORT_CRASH_EXTRA=0: no guest state / symbols / run.log summary */

/* The crash summary also written to stderr (run.log): exception + faulting stack. */
static char s_tee[12288];
static size_t s_teelen;
static int s_tee_on;
static Request s_req;
static Mod s_mods[MAX_MODS];
static int s_nmods;
static DWORD64 s_exe_base, s_exe_end;

/* ---- output: a buffered WriteFile writer, no CRT streams, no heap ---- */
static HANDLE s_out = INVALID_HANDLE_VALUE;
static char s_obuf[32768];
static size_t s_olen;
static void flush_out()
{
    if (s_out != INVALID_HANDLE_VALUE && s_olen) { DWORD w; WriteFile(s_out, s_obuf, (DWORD)s_olen, &w, NULL); }
    s_olen = 0;
}
static void put(const char* s, size_t n)
{
    if (s_tee_on && s_teelen < sizeof s_tee) {
        const size_t k = n < sizeof s_tee - s_teelen ? n : sizeof s_tee - s_teelen;
        memcpy(s_tee + s_teelen, s, k); s_teelen += k;
    }
    while (n) {
        if (s_olen == sizeof s_obuf) flush_out();
        size_t k = sizeof s_obuf - s_olen; if (k > n) k = n;
        memcpy(s_obuf + s_olen, s, k); s_olen += k; s += k; n -= k;
    }
}
static void out(const char* fmt, ...)
{
    char b[2048];
    va_list a; va_start(a, fmt);
    int n = vsnprintf(b, sizeof b, fmt, a);
    va_end(a);
    if (n < 0) return;
    if (n >= (int)sizeof b) n = (int)sizeof b - 1;
    put(b, (size_t)n);
}

static void stamp(char* b, size_t n, int file_form)
{
    SYSTEMTIME t; GetLocalTime(&t);
    snprintf(b, n, file_form ? "%04u%02u%02u_%02u%02u%02u" : "%04u-%02u-%02u %02u:%02u:%02u",
             t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

static const char* code_name(DWORD c)
{
    switch (c) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_INT_OVERFLOW: return "integer overflow";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error (file / disk read failed)";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "datatype misalignment";
    case EXCEPTION_BREAKPOINT: return "breakpoint";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "non-continuable exception";
    case 0xC0000374: return "heap corruption";
    case 0xC0000409: return "stack buffer overrun / fast fail";
    case 0xC0000017: return "out of memory (no memory)";
    case 0x40000015: return "fatal app exit (abort)";
    case 0xC0000417: return "invalid CRT parameter";
    case 0xE06D7363: return "C++ exception";
    default: return (c & 0xF0000000u) == 0xC0000000u ? "error" : "exception";
    }
}

/* ---- modules (read once per report) ---- */
static void load_mods()
{
    s_nmods = 0;
    HMODULE hm[MAX_MODS]; DWORD need = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), hm, sizeof hm, &need)) return;
    const int n = (int)(need / sizeof(HMODULE)) < MAX_MODS ? (int)(need / sizeof(HMODULE)) : MAX_MODS;
    for (int i = 0; i < n; i++) {
        MODULEINFO mi;
        if (!K32GetModuleInformation(GetCurrentProcess(), hm[i], &mi, sizeof mi)) continue;
        Mod& m = s_mods[s_nmods++];
        m.base = (DWORD64)mi.lpBaseOfDll; m.end = m.base + mi.SizeOfImage;
        if (!K32GetModuleBaseNameA(GetCurrentProcess(), hm[i], m.name, sizeof m.name)) { m.name[0] = '?'; m.name[1] = 0; }
    }
}
static const Mod* find_mod(DWORD64 a)
{
    for (int i = 0; i < s_nmods; i++) if (a >= s_mods[i].base && a < s_mods[i].end) return &s_mods[i];
    return NULL;
}

/* Lifted guest function for a host pc in the exe. `exact` = the pc's host
 * function starts exactly at a registered lifted function; otherwise the
 * result is only the nearest registered one below (or 0 off the exe). */
static uint32_t guest_of(DWORD64 pc, int* exact, DWORD64* host_start)
{
    *exact = 0; if (host_start) *host_start = 0;
    if (!s_cfg.guest_resolve || pc < s_exe_base || pc >= s_exe_end) return 0;
    DWORD64 base = 0;
    if (PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(pc, &base, NULL)) {
        const DWORD64 start = base + f->BeginAddress;
        if (host_start) *host_start = start;
        const uint32_t g = s_cfg.guest_resolve((void*)start);
        if (g && g != s_cfg.guest_resolve((void*)(start - 1))) { *exact = 1; return g; }
    }
    return s_cfg.guest_resolve((void*)pc);
}

static int sym_ready()
{
    if (!s_sym_allowed) return 0;
    if (s_sym_state) return s_sym_state > 0;
    s_sym_state = -1;
    if (s_nosym || !s_sso || !s_si || !s_sfa) return 0;
    s_sso(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
    char dir[MAX_PATH]; snprintf(dir, sizeof dir, "%s", s_exe);
    if (char* s = strrchr(dir, '\\')) *s = 0;
    /* FALSE when something else already initialised DbgHelp for this process:
     * its session works for lookups too, so carry on either way. */
    s_si(GetCurrentProcess(), dir, TRUE);
    s_sym_state = 1;
    return 1;
}

/* " {name+0xNN file.c:123}" for a pc, "" when there is no symbol. */
static void sym_of(char* b, size_t n, DWORD64 pc)
{
    b[0] = 0;
    if (!sym_ready()) return;
    static union { SYMBOL_INFO si; char raw[sizeof(SYMBOL_INFO) + 256]; } u;
    memset(&u.si, 0, sizeof u.si);
    u.si.SizeOfStruct = sizeof(SYMBOL_INFO); u.si.MaxNameLen = 255;
    DWORD64 off = 0;
    if (!s_sfa(GetCurrentProcess(), pc, &off, &u.si)) return;
    int len = snprintf(b, n, " {%s+0x%llX", u.si.Name, (unsigned long long)off);
    if (len < 0 || (size_t)len >= n) { b[0] = 0; return; }
    IMAGEHLP_LINE64 ln; memset(&ln, 0, sizeof ln); ln.SizeOfStruct = sizeof ln;
    DWORD lo = 0;
    if (s_sgl && s_sgl(GetCurrentProcess(), pc, &lo, &ln) && ln.FileName) {
        const char* f = strrchr(ln.FileName, '\\'); f = f ? f + 1 : ln.FileName;
        const int k = snprintf(b + len, n - len, " %s:%lu", f, (unsigned long)ln.LineNumber);
        if (k > 0 && (size_t)(len + k) < n) len += k;
    }
    if ((size_t)len + 2 <= n) { b[len] = '}'; b[len + 1] = 0; }
}

static void describe(char* b, size_t n, DWORD64 pc, int is_return)
{
    const Mod* m = find_mod(pc);
    int len = m ? snprintf(b, n, "%s+0x%llX", m->name, (unsigned long long)(pc - m->base))
                : snprintf(b, n, "0x%016llX", (unsigned long long)pc);
    if (len < 0 || (size_t)len >= n) return;
    int exact = 0; DWORD64 hs = 0;
    const uint32_t g = guest_of(is_return ? pc - 1 : pc, &exact, &hs);
    if (g && exact) {
        const int k = snprintf(b + len, n - len, "  [func_%08X+0x%llX]", g, (unsigned long long)(pc - hs));
        if (k < 0 || (size_t)(len + k) >= n) return;
        len += k;
    }
    char s[384]; sym_of(s, sizeof s, is_return ? pc - 1 : pc);
    if (s[0]) snprintf(b + len, n - len, "%s", s);
}

/* ---- stack walking from a copy of the stack ---- */
static struct { DWORD64 lo, hi; unsigned char buf[SNAP_BYTES]; } s_snap;
static BOOL CALLBACK sw_read(HANDLE proc, DWORD64 addr, PVOID buf, DWORD n, LPDWORD got)
{
    if (addr >= s_snap.lo && addr + n <= s_snap.hi) {
        memcpy(buf, s_snap.buf + (addr - s_snap.lo), n); if (got) *got = n; return TRUE;
    }
    SIZE_T g = 0;
    const BOOL ok = ReadProcessMemory(proc, (LPCVOID)addr, buf, n, &g);
    if (got) *got = (DWORD)g;
    return ok;
}
static PVOID CALLBACK sw_fta(HANDLE, DWORD64 addr) { DWORD64 b; return RtlLookupFunctionEntry(addr, &b, NULL); }
static DWORD64 CALLBACK sw_base(HANDLE, DWORD64 addr) { const Mod* m = find_mod(addr); return m ? m->base : 0; }

static void snap_stack(DWORD64 rsp)
{
    s_snap.lo = s_snap.hi = 0;
    MEMORY_BASIC_INFORMATION mbi;
    if (!rsp || !VirtualQuery((LPCVOID)rsp, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return;
    DWORD64 end = (DWORD64)mbi.BaseAddress + mbi.RegionSize;
    if (end - rsp > SNAP_BYTES) end = rsp + SNAP_BYTES;
    SIZE_T got = 0;
    if (ReadProcessMemory(GetCurrentProcess(), (LPCVOID)rsp, s_snap.buf, (SIZE_T)(end - rsp), &got) && got) {
        s_snap.lo = rsp; s_snap.hi = rsp + got;
    }
}

/* DbgHelp-free walk, for a report while another thread is inside DbgHelp:
 * RtlLookupFunctionEntry / RtlVirtualUnwind (ntdll) on a copy of the stack
 * (s_snap) whose pointers into itself are rebased; every read SEH-guarded. */
alignas(16) static unsigned char s_rw[SNAP_BYTES + 512];
static int rtl_steps(CONTEXT* c, DWORD64 lo, DWORD64 hi, DWORD64* pcs, int max)
{
    int n = 0;
    __try {
        while (n < max) {
            const DWORD64 pc = c->Rip, sp = c->Rsp;
            if (!pc) break;
            pcs[n++] = pc;
            if (sp < lo || sp + 8 > hi) break;
            DWORD64 base = 0;
            if (PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(pc, &base, NULL)) {
                PVOID hd = NULL; DWORD64 est = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, pc, f, c, &hd, &est, NULL);
            } else {
                if (n > 1) break;   /* only the leaf may be frameless */
                c->Rip = *(const DWORD64*)sp; c->Rsp = sp + 8;
            }
            if (c->Rsp < lo + 8 || c->Rsp > hi || (c->Rsp <= sp && c->Rip == pc)) break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}
static void walk_rtl(const CONTEXT& c0, int max_frames)
{
    static DWORD64 pcs[64];
    if (max_frames > 64) max_frames = 64;
    int n = 0;
    const DWORD64 lo = s_snap.lo, len = s_snap.hi - s_snap.lo;
    if (len) {
        memcpy(s_rw, s_snap.buf, (size_t)len);
        memset(s_rw + len, 0, sizeof s_rw - (size_t)len);
        const DWORD64 delta = (DWORD64)(uintptr_t)s_rw - lo;
        DWORD64* w = (DWORD64*)s_rw;
        for (DWORD64 i = 0; i < len / 8; i++) if (w[i] >= lo && w[i] < lo + len) w[i] += delta;
        CONTEXT c = c0;
        DWORD64* regs[] = { &c.Rsp, &c.Rbp, &c.Rbx, &c.Rsi, &c.Rdi, &c.R12, &c.R13, &c.R14, &c.R15 };
        for (DWORD64* x : regs) if (*x >= lo && *x < lo + len) *x += delta;
        n = rtl_steps(&c, (DWORD64)(uintptr_t)s_rw, (DWORD64)(uintptr_t)s_rw + len, pcs, max_frames);
    }
    if (n <= 0) { pcs[0] = c0.Rip; n = 1; }
    for (int i = 0; i < n; i++) {
        char d[640]; describe(d, sizeof d, pcs[i], i > 0);
        out("    #%-2d %s\n", i, d);
    }
}

static void walk(HANDLE th, const CONTEXT& c0, int max_frames)
{
    if (s_nodbg) { walk_rtl(c0, max_frames); return; }
    if (!s_sw64) { out("    (no DbgHelp StackWalk64)\n"); return; }
    CONTEXT c = c0;
    STACKFRAME64 sf; memset(&sf, 0, sizeof sf);
    sf.AddrPC.Offset = c.Rip; sf.AddrPC.Mode = AddrModeFlat;
    sf.AddrFrame.Offset = c.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = c.Rsp; sf.AddrStack.Mode = AddrModeFlat;
    DWORD64 last_sp = 0;
    for (int i = 0; i < max_frames; i++) {
        if (!s_sw64(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), th, &sf, &c, sw_read, sw_fta, sw_base, NULL)) break;
        const DWORD64 pc = sf.AddrPC.Offset;
        if (!pc) break;
        char d[640]; describe(d, sizeof d, pc, i > 0);
        out("    #%-2d %s\n", i, d);
        if (i > 0 && sf.AddrStack.Offset <= last_sp) break;   /* not moving: a bad unwind */
        last_sp = sf.AddrStack.Offset;
    }
}

static void thread_name(HANDLE h, char* b, size_t n)
{
    b[0] = 0;
    if (!s_gtd || !h) return;
    PWSTR w = NULL;
    if (SUCCEEDED(s_gtd(h, &w)) && w) {
        WideCharToMultiByte(CP_UTF8, 0, w, -1, b, (int)n, NULL, NULL);
        LocalFree(w);
    }
}

static void all_threads(const Request& r)
{
    const DWORD pid = GetCurrentProcessId(), me = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) { out("(thread list unavailable)\n"); return; }
    static DWORD tids[MAX_THREADS];
    int nt = 0;
    THREADENTRY32 te; te.dwSize = sizeof te;
    if (Thread32First(snap, &te)) do {
        if (te.th32OwnerProcessID == pid && nt < MAX_THREADS) tids[nt++] = te.th32ThreadID;
        te.dwSize = sizeof te;
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);
    /* the faulting thread first */
    for (int i = 0; i < nt; i++)
        if (tids[i] == r.tid && i) { const DWORD t = tids[0]; tids[0] = tids[i]; tids[i] = t; break; }
    out("\n==== threads (%d) ====\n", nt);
    for (int i = 0; i < nt; i++) {
        const DWORD tid = tids[i];
        const int faulting = r.kind == KIND_CRASH && tid == r.tid && r.ep && r.ep->ContextRecord;
        if (tid == me && !faulting) continue;
        HANDLE h = tid == me ? NULL : OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
        char name[96] = "";
        /* GetThreadDescription allocates: not after heap corruption */
        if (!(r.ep && r.ep->ExceptionRecord && r.ep->ExceptionRecord->ExceptionCode == 0xC0000374)) thread_name(h, name, sizeof name);
        const char* role = faulting ? "  <== CRASHED HERE" : tid == s_watch_id ? "  (hang watchdog)" : "";
        out("\n--- thread %lu%s%s%s%s\n", tid, name[0] ? " \"" : "", name, name[0] ? "\"" : "", role);
        CONTEXT c; memset(&c, 0, sizeof c);
        int have = 0;
        if (faulting) { c = *r.ep->ContextRecord; have = 1; }
        else if (h) {
            if (SuspendThread(h) != (DWORD)-1) {
                c.ContextFlags = CONTEXT_FULL;
                have = GetThreadContext(h, &c) != 0;
                if (have) snap_stack(c.Rsp);
                ResumeThread(h);
            }
        }
        if (!have) { out("    (no context)\n"); if (h) CloseHandle(h); continue; }
        if (faulting) snap_stack(c.Rsp);
        if (faulting && s_teelen < sizeof s_tee) {
            const int k = snprintf(s_tee + s_teelen, sizeof s_tee - s_teelen, "faulting thread %lu%s%s%s stack:\n",
                                   (unsigned long)tid, name[0] ? " \"" : "", name, name[0] ? "\"" : "");
            if (k > 0) s_teelen = s_teelen + k < sizeof s_tee ? s_teelen + k : sizeof s_tee;
        }
        s_tee_on = faulting;
        walk(h ? h : GetCurrentThread(), c, faulting ? 64 : 24);
        s_tee_on = 0;
        s_snap.lo = s_snap.hi = 0;
        if (h) CloseHandle(h);
    }
}

/* ---- pieces of the text report ---- */
static void exe_identity()
{
    out("exe:        %s\n", s_exe);
    const BYTE* b = (const BYTE*)s_exe_base;
    if (!b) return;
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)b;
    const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)(b + dos->e_lfanew);
    out("exe build:  PE timestamp 0x%08lX, image size 0x%lX\n", (unsigned long)nt->FileHeader.TimeDateStamp,
        (unsigned long)nt->OptionalHeader.SizeOfImage);
    const IMAGE_DATA_DIRECTORY& dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    const IMAGE_DEBUG_DIRECTORY* d = (const IMAGE_DEBUG_DIRECTORY*)(b + dd.VirtualAddress);
    for (DWORD i = 0; dd.VirtualAddress && i < dd.Size / sizeof *d; i++) {
        if (d[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW || !d[i].AddressOfRawData) continue;
        const BYTE* cv = b + d[i].AddressOfRawData;
        if (memcmp(cv, "RSDS", 4)) continue;
        const GUID* g = (const GUID*)(cv + 4);
        const DWORD age = *(const DWORD*)(cv + 20);
        out("exe pdb:    %s {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X} age %lu\n", (const char*)(cv + 24),
            (unsigned long)g->Data1, g->Data2, g->Data3, g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
            g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7], (unsigned long)age);
        break;
    }
    char vp[MAX_PATH]; snprintf(vp, sizeof vp, "%s\\VERSION", s_root);
    HANDLE f = CreateFileA(vp, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        char v[64] = {0}; DWORD got = 0;
        ReadFile(f, v, sizeof v - 1, &got, NULL); CloseHandle(f);
        for (char* p = v; *p; p++) if (*p == '\r' || *p == '\n') { *p = 0; break; }
        out("version:    %s\n", v);
    }
}

static void sys_summary()
{
    int r[4]; char brand[49] = {0};
    __cpuid(r, 0x80000000);
    if ((unsigned)r[0] >= 0x80000004u) for (int i = 0; i < 3; i++) { __cpuid(r, 0x80000002 + i); memcpy(brand + 16 * i, r, 16); }
    const char* bp = brand; while (*bp == ' ') bp++;
    typedef LONG (WINAPI* rgv_t)(OSVERSIONINFOW*);
    OSVERSIONINFOW ov; memset(&ov, 0, sizeof ov); ov.dwOSVersionInfoSize = sizeof ov;
    if (rgv_t f = (rgv_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion")) f(&ov);
    MEMORYSTATUSEX ms; ms.dwLength = sizeof ms; GlobalMemoryStatusEx(&ms);
    PROCESS_MEMORY_COUNTERS_EX pm; memset(&pm, 0, sizeof pm);
    K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm);
    SYSTEM_INFO si; GetNativeSystemInfo(&si);
    out("system:     Windows %lu.%lu build %lu | %s | %lu logical CPUs\n", (unsigned long)ov.dwMajorVersion,
        (unsigned long)ov.dwMinorVersion, (unsigned long)ov.dwBuildNumber, *bp ? bp : "?", (unsigned long)si.dwNumberOfProcessors);
    out("memory:     RAM %.1f GB (%.1f GB free) | process: %.0f MB private, %.0f MB working set\n",
        ms.ullTotalPhys / 1073741824.0, ms.ullAvailPhys / 1073741824.0, pm.PrivateUsage / 1048576.0,
        pm.WorkingSetSize / 1048576.0);
}

static int env_wanted(const wchar_t* e)
{
    static const wchar_t* pre[] = { L"PORT_", L"R1_", L"R2_", L"R3_", L"RFOM_", L"RSX_", L"PS3", L"PPU_", L"SPU_",
                                    L"GCM_", L"CRASH_", L"FREEZE_", L"CHILD_", L"YDKJ_", L"VK_", L"DXVK" };
    for (const wchar_t* p : pre) if (!_wcsnicmp(e, p, wcslen(p))) return 1;
    return 0;
}
static void env_summary()
{
    out("\n==== settings (environment) ====\n");
    wchar_t* env = GetEnvironmentStringsW();
    if (!env) return;
    int n = 0;
    for (const wchar_t* e = env; *e; e += wcslen(e) + 1) {
        if (*e == L'=' || !env_wanted(e)) continue;
        char b[1024];
        WideCharToMultiByte(CP_UTF8, 0, e, -1, b, sizeof b, NULL, NULL);
        b[sizeof b - 1] = 0;
        out("%s\n", b);
        n++;
    }
    FreeEnvironmentStringsW(env);
    if (!n) out("(no port switches set)\n");
}

/* The game's stdout is redirected to logs\run.log by run.bat: find that file
 * from the handle (any redirect works), else <port>\logs\run.log. */
static void log_tail()
{
    char path[MAX_PATH] = "";
    HANDLE so = GetStdHandle(STD_OUTPUT_HANDLE);
    if (so && so != INVALID_HANDLE_VALUE && GetFileType(so) == FILE_TYPE_DISK) {
        char p[MAX_PATH + 8];
        const DWORD n = GetFinalPathNameByHandleA(so, p, sizeof p, 0);
        if (n && n < sizeof p) snprintf(path, sizeof path, "%s", strncmp(p, "\\\\?\\", 4) ? p : p + 4);
    }
    if (!path[0]) snprintf(path, sizeof path, "%s\\logs\\run.log", s_root);
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    out("\n==== last %d lines of %s ====\n", LOG_TAIL_LINES, path);
    if (f == INVALID_HANDLE_VALUE) { out("(not found)\n"); return; }
    static char buf[64 * 1024];
    LARGE_INTEGER sz; GetFileSizeEx(f, &sz);
    LARGE_INTEGER from; from.QuadPart = sz.QuadPart > (LONGLONG)sizeof buf ? sz.QuadPart - (LONGLONG)sizeof buf : 0;
    SetFilePointerEx(f, from, NULL, FILE_BEGIN);
    DWORD got = 0; ReadFile(f, buf, sizeof buf, &got, NULL); CloseHandle(f);
    int lines = 0; DWORD s = got;
    while (s > 0) {
        if (buf[s - 1] == '\n' && s != got && ++lines > LOG_TAIL_LINES) break;
        s--;
    }
    put(buf + s, got - s);
    if (got && buf[got - 1] != '\n') out("\n");
}

static void hex_line(const char* names[], const DWORD64* v, int n)
{
    out("   ");
    for (int i = 0; i < n; i++) out(" %s=%016llX", names[i], (unsigned long long)v[i]);
    out("\n");
}

/* CRT invalid parameter (0xC0000417): the handler runs several frames below
 * whoever passed the bad argument (caller -> fread -> _invalid_parameter ->
 * on_invparam), so RtlCaptureContext there only ever names on_invparam.
 * on_invparam records its stack on its own stack frame; crash() copies it
 * into s_ip_* only on the thread that wins the report, so a second
 * concurrent invalid parameter can't lend its stack to the first one's
 * report. The report then names the first frame outside the handler and the
 * CRT as the call site, makes it the exception address, and lists the stack.
 * The dump keeps the real handler context (walkable) plus a comment stream
 * with the same text. */
struct IpCapture { void* fr[32]; int n; const wchar_t* func; const wchar_t* file; unsigned line; };
static thread_local const IpCapture* t_ip;   /* set by on_invparam for its crash_here() */
static void* s_ip_frames[32];
static int s_ip_n;
static char s_ip_func[128], s_ip_file[160];
static unsigned s_ip_line;
static int s_ip_site = -1;              /* index into s_ip_frames, -1 = none */
static char s_ip_text[1024];            /* dump comment stream */

static int crt_mod(const Mod* m)
{
    if (!m) return 0;
    return !_strnicmp(m->name, "ucrtbase", 8) || !_strnicmp(m->name, "vcruntime", 9) ||
           !_strnicmp(m->name, "msvcr", 5) || !_strnicmp(m->name, "msvcp", 5) || !_strnicmp(m->name, "api-ms-", 7) ||
           !_stricmp(m->name, "ntdll.dll") || !_stricmp(m->name, "KERNELBASE.dll");
}
static void frame_where(char* b, size_t n, DWORD64 pc)
{
    const Mod* m = find_mod(pc);
    if (m) snprintf(b, n, "%s+0x%llX", m->name, (unsigned long long)(pc - m->base));
    else snprintf(b, n, "0x%016llX", (unsigned long long)pc);
}
/* Picks the call site for an invalid-parameter report (after load_mods). */
static void invparam_site(const Request& r)
{
    s_ip_site = -1; s_ip_text[0] = 0;
    if (r.kind != KIND_CRASH || !r.ep || !r.ep->ExceptionRecord || r.ep->ExceptionRecord->ExceptionCode != 0xC0000417 ||
        s_ip_n < 2)
        return;
    /* frame 0 is in on_invparam: skip it and anything else in that function */
    DWORD64 b0 = 0, b = 0;
    PRUNTIME_FUNCTION f0 = RtlLookupFunctionEntry((DWORD64)s_ip_frames[0], &b0, NULL);
    int crt_in_exe = 0;
    for (int i = 1; i < s_ip_n; i++) {
        const DWORD64 pc = (DWORD64)s_ip_frames[i];
        PRUNTIME_FUNCTION fi = RtlLookupFunctionEntry(pc, &b, NULL);
        if (f0 && fi && b == b0 && fi->BeginAddress == f0->BeginAddress) continue;
        if (crt_mod(find_mod(pc))) continue;
        /* a statically linked CRT: its invalid-parameter machinery is in the
         * exe (needs symbols to see), and the frame above it is the CRT
         * function that got the bad argument (strcpy_s, fread) */
        char s[384]; sym_of(s, sizeof s, pc - 1);
        if (strstr(s, "{_invalid_parameter") || strstr(s, "{_invoke_watson") || strstr(s, "{port_crash::")) { crt_in_exe = 1; continue; }
        if (crt_in_exe == 1) { crt_in_exe = 2; continue; }
        s_ip_site = i;
        break;
    }
    if (s_ip_site < 0) return;
    /* a return address points after the call: pc - 1 is inside the call instruction */
    r.ep->ExceptionRecord->ExceptionAddress = (PVOID)((DWORD64)s_ip_frames[s_ip_site] - 1);
    size_t k = 0;
    char w[160];
    frame_where(w, sizeof w, (DWORD64)s_ip_frames[s_ip_site] - 1);
    k += snprintf(s_ip_text + k, sizeof s_ip_text - k, "CRT invalid parameter, call site %s", w);
    if (s_ip_func[0] && k < sizeof s_ip_text)
        k += snprintf(s_ip_text + k, sizeof s_ip_text - k, " (%s, %s:%u)", s_ip_func, s_ip_file, s_ip_line);
    if (k < sizeof s_ip_text) k += snprintf(s_ip_text + k, sizeof s_ip_text - k, "; stack:");
    for (int i = 1; i < s_ip_n && i < 17 && k < sizeof s_ip_text; i++) {
        frame_where(w, sizeof w, (DWORD64)s_ip_frames[i]);
        k += snprintf(s_ip_text + k, sizeof s_ip_text - k, " %s", w);
    }
}
static void invparam_section()
{
    if (s_ip_site < 0) return;
    if (s_ip_func[0]) out("crt check:  %s (%s:%u)\n", s_ip_func, s_ip_file, s_ip_line);
    out("call stack: (from the invalid-parameter handler; the address above is the call site)\n");
    for (int i = 1; i < s_ip_n && i < 13; i++) {
        const DWORD64 pc = (DWORD64)s_ip_frames[i];
        char w[512]; describe(w, sizeof w, pc, 1);
        out("  #%-2d %s%s\n", i - 1, w, i == s_ip_site ? "  <== call site" : "");
    }
}

static void exception_section(const Request& r, char* line, size_t linen)
{
    const EXCEPTION_RECORD* er = r.ep->ExceptionRecord;
    const DWORD64 pc = (DWORD64)er->ExceptionAddress;
    const Mod* m = find_mod(pc);
    char where[160];
    if (m) snprintf(where, sizeof where, "%s+0x%llX", m->name, (unsigned long long)(pc - m->base));
    else snprintf(where, sizeof where, "0x%016llX", (unsigned long long)pc);
    char fault[128] = "";
    if ((er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || er->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        er->NumberParameters >= 2) {
        const uintptr_t fa = er->ExceptionInformation[1];
        const char* op = er->ExceptionInformation[0] == 1 ? "writing" : er->ExceptionInformation[0] == 8 ? "executing" : "reading";
        const uintptr_t vb = s_cfg.vm_base && *s_cfg.vm_base ? (uintptr_t)*s_cfg.vm_base : 0;
        if (vb && fa >= vb && fa < vb + s_cfg.vm_size)
            snprintf(fault, sizeof fault, ", %s guest 0x%08X", op, (uint32_t)(fa - vb));
        else
            snprintf(fault, sizeof fault, ", %s 0x%llX", op, (unsigned long long)fa);
    }
    out("\n==== exception ====\n");
    s_tee_on = 1;
    out("what:       %s\n", r.how);
    out("code:       0x%08lX (%s)%s\n", (unsigned long)er->ExceptionCode, code_name(er->ExceptionCode),
        (er->ExceptionFlags & EXCEPTION_NONCONTINUABLE) ? ", non-continuable" : "");
    char sym[384]; sym_of(sym, sizeof sym, pc);
    out("address:    0x%016llX = %s%s%s\n", (unsigned long long)pc, where, fault, sym);
    int exact = 0; DWORD64 hs = 0;
    const uint32_t g = guest_of(pc, &exact, &hs);
    if (g && exact)
        out("guest fn:   func_%08X (lifted code; host function +0x%llX)\n", g, (unsigned long long)(pc - hs));
    else if (g)
        out("guest fn:   none exact -- nearest lifted function below is func_%08X (the address is in runtime code or a split part)\n", g);
    else
        out("guest fn:   none (not in the game exe's lifted code)\n");
    invparam_section();
    out("thread:     %lu\n", (unsigned long)r.tid);
    if (r.guest.valid) {
        out("guest:      PPU thread %llu, lr=0x%08X sp=0x%08X\n", (unsigned long long)r.guest.thread_id,
            r.guest.lr, r.guest.sp);
        if (r.guest.syscall_inflight) out("syscall:    inside %u, last %u", r.guest.syscall_inflight, r.guest.syscall_last);
        else out("syscall:    none in flight, last %u", r.guest.syscall_last);
        if (r.guest.callsite) out(" (last syscall/HLE call from func_%08X)", r.guest.callsite);
        out("\n");
    } else if (s_cfg.guest_state) {
        out("guest:      (not a guest PPU thread)\n");
    }
    if (const CONTEXT* c = r.ep->ContextRecord) {
        out("registers:\n");
        const char* n1[] = { "rax", "rbx", "rcx", "rdx" };  const DWORD64 v1[] = { c->Rax, c->Rbx, c->Rcx, c->Rdx };
        const char* n2[] = { "rsi", "rdi", "rbp", "rsp" };  const DWORD64 v2[] = { c->Rsi, c->Rdi, c->Rbp, c->Rsp };
        const char* n3[] = { " r8", " r9", "r10", "r11" };  const DWORD64 v3[] = { c->R8, c->R9, c->R10, c->R11 };
        const char* n4[] = { "r12", "r13", "r14", "r15" };  const DWORD64 v4[] = { c->R12, c->R13, c->R14, c->R15 };
        hex_line(n1, v1, 4); hex_line(n2, v2, 4); hex_line(n3, v3, 4); hex_line(n4, v4, 4);
        out("    rip=%016llX eflags=%08lX\n", (unsigned long long)c->Rip, (unsigned long)c->EFlags);
    }
    s_tee_on = 0;
    char gf[32] = "";
    if (g && exact) snprintf(gf, sizeof gf, " in func_%08X", g);
    snprintf(line, linen, "code 0x%08lX (%s) at %s%s%s (thread %lu)", (unsigned long)er->ExceptionCode,
             code_name(er->ExceptionCode), where, fault, gf, (unsigned long)r.tid);
}

static void note_line(const char* line)
{
    char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\crashes.txt", s_dir);
    char ts[32]; stamp(ts, sizeof ts, 0);
    char b[1400];
    const int n = snprintf(b, sizeof b, "%s %s%s%s\n", ts, s_cfg.exe_suffix ? s_cfg.exe_suffix : "", s_cfg.exe_suffix ? " " : "", line);
    HANDLE f = CreateFileA(p, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(f, b, (DWORD)n, &w, NULL); CloseHandle(f); }
    HANDLE e = GetStdHandle(STD_ERROR_HANDLE);
    if (e && e != INVALID_HANDLE_VALUE) { DWORD w; const char* s = strstr(b, line); WriteFile(e, s, (DWORD)(n - (s - b)), &w, NULL); }
}

static int write_dump(const char* path, const Request& r)
{
    if (!s_mdw) return 0;
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    MINIDUMP_EXCEPTION_INFORMATION mei;
    mei.ThreadId = r.tid; mei.ExceptionPointers = r.ep; mei.ClientPointers = FALSE;
    const MINIDUMP_TYPE type = s_full
        ? (MINIDUMP_TYPE)(MiniDumpWithFullMemory | MiniDumpWithThreadInfo | MiniDumpWithHandleData | MiniDumpWithUnloadedModules)
        : (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo |
                          MiniDumpScanMemory | MiniDumpWithUnloadedModules);
    MINIDUMP_USER_STREAM us;
    MINIDUMP_USER_STREAM_INFORMATION usi;
    us.Type = CommentStreamA; us.Buffer = s_ip_text; us.BufferSize = (ULONG)strlen(s_ip_text) + 1;
    usi.UserStreamCount = 1; usi.UserStreamArray = &us;
    const BOOL ok = s_mdw(GetCurrentProcess(), GetCurrentProcessId(), f, type,
                          r.kind == KIND_CRASH && r.ep ? &mei : NULL, s_ip_text[0] ? &usi : NULL, NULL);
    CloseHandle(f);
    if (!ok) DeleteFileA(path);
    return ok != FALSE;
}

static void do_report(Request& r)
{
    if (r.kind == KIND_CRASH && s_cfg.before_report) s_cfg.before_report();
    load_mods();
    s_teelen = 0; s_tee_on = 0;
    /* DbgHelp allocates and loads the PDB: crashes only (a hang report must not
     * grow a still-running game), never after heap corruption */
    s_sym_allowed = r.kind == KIND_CRASH && !(r.ep && r.ep->ExceptionRecord && r.ep->ExceptionRecord->ExceptionCode == 0xC0000374);
    /* DbgHelp (dump, StackWalk64, symbols) is single-threaded and PORT_HITCHCATCH
     * uses it too: hold the shared lock for the whole report, taken before any
     * thread is suspended. If another thread still holds it after 20 s (hang:
     * 30 s), that thread may be inside DbgHelp: this report then makes no DbgHelp
     * call (RtlVirtualUnwind stacks, module+offset names) and writes the minidump
     * at the end if the lock frees within the same wait. A crash on the thread
     * that holds the lock is reported as before, without symbols. Uncontended
     * (PORT_HITCHCATCH off) this is the old path. */
    InterlockedExchange(&g_port_dbghelp_report, 1);
    const DWORD dbg_wait = s_dbg_wait_ms ? s_dbg_wait_ms : r.kind == KIND_CRASH ? 20000 : 30000;
    const DWORD dbg_owner = (DWORD)g_port_dbghelp_owner;
    const int self_owned = r.kind == KIND_CRASH && dbg_owner && dbg_owner == r.tid;
    int dbg_locked = !self_owned && port_dbghelp_trylock(dbg_wait);
    const DWORD dbg_holder = dbg_locked ? 0 : self_owned ? dbg_owner : (DWORD)g_port_dbghelp_owner;
    s_nodbg = !dbg_locked && !self_owned;
    if (!dbg_locked) s_sym_allowed = 0;
    invparam_site(r);
    char ts[32]; stamp(ts, sizeof ts, 1);
    char stem[MAX_PATH];
    const char* kind = r.kind == KIND_CRASH ? "crash" : "hang";
    if (s_cfg.exe_suffix && *s_cfg.exe_suffix)
        snprintf(stem, sizeof stem, "%s\\%s_%s_%s_%s", s_dir, s_cfg.tag, ts, s_cfg.exe_suffix, kind);
    else
        snprintf(stem, sizeof stem, "%s\\%s_%s_%s", s_dir, s_cfg.tag, ts, kind);
    char dmp[MAX_PATH], txt[MAX_PATH];
    snprintf(dmp, sizeof dmp, "%s.dmp", stem);
    snprintf(txt, sizeof txt, "%s.txt", stem);
    int dump_ok = s_nodbg ? 0 : write_dump(dmp, r);
    snprintf(r.out_dmp, sizeof r.out_dmp, "%s", dump_ok ? dmp : "");

    char line[1024] = "";
    s_out = CreateFileA(txt, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    s_olen = 0;
    char now[32]; stamp(now, sizeof now, 0);
    out("%s PC port -- %s report\n", s_cfg.title ? s_cfg.title : "Resistance", kind);
    out("time:       %s (%.0f s after start)\n", now, (GetTickCount64() - s_start_ms) / 1000.0);
    exe_identity();
    sys_summary();
    out("game loop:  %llu", s_cfg.main_progress ? (unsigned long long)s_cfg.main_progress() : 0ull);
    if (s_cfg.rsx_progress && r.kind == KIND_HANG) out(" | renderer progress %llu", (unsigned long long)s_cfg.rsx_progress());
    if (s_cfg.paused) out(" | paused %d", s_cfg.paused());
    out("\n");
    out("command:    %s\n", GetCommandLineA());
    out("dump:       %s\n", dump_ok ? dmp : s_nodbg ? "(deferred: DbgHelp busy, see the end of this report)" : "(failed)");
    if (s_nodbg)
        out("note:       DbgHelp lock held by thread %lu for %lu ms: stacks unwound without DbgHelp, no symbols, minidump deferred\n",
            (unsigned long)dbg_holder, (unsigned long)dbg_wait);
    else if (!dbg_locked) out("note:       DbgHelp lock held by the crashing thread %lu; no symbols\n", (unsigned long)dbg_holder);
    if (r.kind == KIND_CRASH && r.ep && r.ep->ExceptionRecord) {
        exception_section(r, line, sizeof line);
    } else {
        out("\n==== hang ====\n%s\n", r.detail);
        snprintf(line, sizeof line, "%s", r.detail);
    }
    if (s_cfg.guest_threads) {
        static char gb[16384];
        gb[0] = 0;
        size_t gn = s_cfg.guest_threads(gb, sizeof gb);
        if (gn >= sizeof gb) gn = sizeof gb - 1;
        out("\n==== guest threads ====\n");
        if (gn) put(gb, gn); else out("(none)\n");
    }
    all_threads(r);
    log_tail();
    env_summary();
    out("\n==== modules ====\n");
    for (int i = 0; i < s_nmods; i++)
        out("%016llX %8lluK %s\n", (unsigned long long)s_mods[i].base,
            (unsigned long long)((s_mods[i].end - s_mods[i].base) >> 10), s_mods[i].name);
    if (s_nodbg) {
        flush_out();   /* the report so far is on disk before the second wait */
        if (port_dbghelp_trylock(dbg_wait)) {
            dbg_locked = 1;
            dump_ok = write_dump(dmp, r);
            out("\nnote:       DbgHelp lock freed after the report; minidump (taken now, after the report): %s\n", dump_ok ? dmp : "(failed)");
        } else {
            out("\nnote:       DbgHelp lock still held by thread %lu after another %lu ms: no minidump\n",
                (unsigned long)g_port_dbghelp_owner, (unsigned long)dbg_wait);
        }
        snprintf(r.out_dmp, sizeof r.out_dmp, "%s", dump_ok ? dmp : "");
    }
    flush_out();
    const int txt_ok = s_out != INVALID_HANDLE_VALUE;
    if (txt_ok) CloseHandle(s_out);
    s_out = INVALID_HANDLE_VALUE;
    snprintf(r.out_txt, sizeof r.out_txt, "%s", txt_ok ? txt : "");

    if (r.kind == KIND_CRASH && s_teelen && !s_noextra) {
        HANDLE e = GetStdHandle(STD_ERROR_HANDLE);
        if (e && e != INVALID_HANDLE_VALUE) {
            static const char hd[] = "\n[crash] ======== crash report summary (full report in logs\\crash) ========\n";
            static const char ft[] = "[crash] ==================================================================\n";
            DWORD w;
            WriteFile(e, hd, (DWORD)(sizeof hd - 1), &w, NULL);
            WriteFile(e, s_tee, (DWORD)s_teelen, &w, NULL);
            WriteFile(e, ft, (DWORD)(sizeof ft - 1), &w, NULL);
        }
    }
    s_teelen = 0;
    char nl[1400];
    snprintf(nl, sizeof nl, "[%s] %s%s%s -> %s", kind, r.kind == KIND_CRASH ? r.how : "", r.kind == KIND_CRASH ? ": " : "",
             line, dump_ok ? dmp : txt_ok ? txt : "report FAILED");
    note_line(nl);
    if (dbg_locked) port_dbghelp_unlock();
    if (r.kind != KIND_CRASH) InterlockedExchange(&g_port_dbghelp_report, 0);
    if (s_cfg.after_report) s_cfg.after_report(r.kind);
}

static DWORD WINAPI reporter(LPVOID)
{
    for (;;) {
        WaitForSingleObject(s_req_ev, INFINITE);
        do_report(s_req);
        SetEvent(s_done_ev);
    }
}

/* Runs one report on the reporter thread (inline when there is none, or on
 * it). The caller holds s_lock. */
static void run(Request& r)
{
    s_req = r;
    if (s_thread && GetCurrentThreadId() != s_thread_id) {
        ResetEvent(s_done_ev);
        SetEvent(s_req_ev);
        WaitForSingleObject(s_done_ev, r.kind == KIND_CRASH ? 180000 : INFINITE);
    } else {
        do_report(s_req);
    }
    r = s_req;
}

static void crash(EXCEPTION_POINTERS* ep, const char* how)
{
    if (!s_on || GetCurrentThreadId() == s_thread_id) return;   /* the reporter itself failed: let it die */
    /* guest state from this thread's TLS, before handing over to the reporter */
    PortGuestState gs; memset(&gs, 0, sizeof gs);
    if (s_cfg.guest_state) s_cfg.guest_state(&gs);
    /* one report per process: a thread that faults while another reports
     * waits here until that report is written, then goes on dying */
    AcquireSRWLockExclusive(&s_lock);
    if (InterlockedCompareExchange(&s_crashed, 1, 0) != 0) { ReleaseSRWLockExclusive(&s_lock); return; }
    if (const IpCapture* ip = t_ip) {
        memcpy(s_ip_frames, ip->fr, sizeof(void*) * (size_t)ip->n);
        s_ip_n = ip->n;
        /* only the debug CRT passes these */
        if (ip->func) WideCharToMultiByte(CP_UTF8, 0, ip->func, -1, s_ip_func, (int)sizeof s_ip_func - 1, NULL, NULL);
        if (ip->file) {
            const wchar_t* b = wcsrchr(ip->file, L'\\');
            WideCharToMultiByte(CP_UTF8, 0, b ? b + 1 : ip->file, -1, s_ip_file, (int)sizeof s_ip_file - 1, NULL, NULL);
        }
        s_ip_line = ip->line;
    }
    static Request r;
    memset(&r, 0, sizeof r);
    r.kind = KIND_CRASH; r.ep = ep; r.tid = GetCurrentThreadId(); r.how = how; r.guest = gs;
    run(r);
    ReleaseSRWLockExclusive(&s_lock);
}

/* A crash that is not an exception (abort, purecall, invalid parameter):
 * report it with the caller's registers. */
static void crash_here(const char* how, DWORD code)
{
    /* on this thread's stack: it stays here (blocked in crash()) while its
     * report is written, and a concurrent loser can't overwrite it */
    CONTEXT c;
    EXCEPTION_RECORD er;
    EXCEPTION_POINTERS ep;
    RtlCaptureContext(&c);
    memset(&er, 0, sizeof er);
    er.ExceptionCode = code; er.ExceptionFlags = EXCEPTION_NONCONTINUABLE; er.ExceptionAddress = (PVOID)c.Rip;
    ep.ExceptionRecord = &er; ep.ContextRecord = &c;
    crash(&ep, how);
}

static LONG WINAPI filter(EXCEPTION_POINTERS* ep)
{
    crash(ep, "unhandled exception");
    return s_prev_filter ? s_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

/* Registered last (AddVectoredExceptionHandler(0, ...)), so the runtime's
 * guest-fault handlers (page commit, write watches, guards) see every fault
 * first; and only codes nothing recovers from, whose filter chain may never
 * run (no stack left after an overflow). Everything else is left to filter(). */
static LONG WINAPI fatal_veh(EXCEPTION_POINTERS* ep)
{
    const DWORD c = ep->ExceptionRecord->ExceptionCode;
    if (c == EXCEPTION_STACK_OVERFLOW || c == 0xC0000374 /* heap corruption */)
        crash(ep, "fatal exception");
    return EXCEPTION_CONTINUE_SEARCH;
}

static void __cdecl on_abort(int sig)
{
    crash_here("abort() / std::terminate", 0x40000015);
    if (s_prev_abrt && s_prev_abrt != SIG_DFL && s_prev_abrt != SIG_IGN && s_prev_abrt != on_abort) s_prev_abrt(sig);
}
static void __cdecl on_purecall()
{
    crash_here("pure virtual function call", 0xC0000409);
    if (s_prev_purecall) s_prev_purecall();
}
static void __cdecl on_invparam(const wchar_t* e, const wchar_t* f, const wchar_t* file, unsigned line, uintptr_t res)
{
    IpCapture ip;
    ip.n = (int)RtlCaptureStackBackTrace(0, 32, ip.fr, NULL);
    ip.func = f; ip.file = file; ip.line = line;
    t_ip = &ip;
    crash_here("CRT invalid parameter", 0xC0000417);
    t_ip = NULL;
    if (s_prev_invparam) { s_prev_invparam(e, f, file, line, res); return; }
    __fastfail(FAST_FAIL_INVALID_ARG);   /* what the default handler does */
}

/* ---- hang watchdog ---- */
static void hang(const char* what)
{
    if (!s_on || s_crashed) return;
    AcquireSRWLockExclusive(&s_lock);
    static Request r;
    memset(&r, 0, sizeof r);
    r.kind = KIND_HANG; r.tid = GetCurrentThreadId(); r.how = "hang";
    snprintf(r.detail, sizeof r.detail, "%s", what);
    run(r);
    ReleaseSRWLockExclusive(&s_lock);
}

static DWORD WINAPI watchdog(LPVOID)
{
    const ULONGLONG limit = (ULONGLONG)s_hang_s * 1000ull;
    uint64_t lm = s_cfg.main_progress(), lr = s_cfg.rsx_progress ? s_cfg.rsx_progress() : 0, mr = lm;
    ULONGLONG tm = GetTickCount64(), tr = tm;
    int rep_m = 0, rep_r = 0;
    for (;;) {
        Sleep(1000);
        const ULONGLONG now = GetTickCount64();
        const int paused = s_cfg.paused && s_cfg.paused();
        const uint64_t m = s_cfg.main_progress();
        const int started = m && m >= s_cfg.main_start_min;
        if (m != lm || paused || !started) { lm = m; tm = now; rep_m = 0; }
        else if (!rep_m && now - tm >= limit) {
            rep_m = 1;
            char d[200];
            snprintf(d, sizeof d, "no game-loop progress for %d s (counter stuck at %llu)", s_hang_s, (unsigned long long)m);
            hang(d);
        }
        if (s_hang_mode != 2 || !s_cfg.rsx_progress) continue;
        const uint64_t r = s_cfg.rsx_progress();
        if (r != lr || !r || paused || !started) { lr = r; tr = now; mr = m; rep_r = 0; }
        else if (!rep_r && !rep_m && m != mr && now - tr >= limit) {
            rep_r = 1;
            char d[200];
            snprintf(d, sizeof d, "no renderer flip for %d s while the game loop runs (game loop %llu -> %llu)",
                     s_hang_s, (unsigned long long)mr, (unsigned long long)m);
            hang(d);
        }
    }
}

/* Keep the newest `keep` dumps (and their .txt) in logs\crash. */
static void prune(int keep)
{
    struct F { char name[MAX_PATH]; ULONGLONG t; };
    static F fs[512];
    int n = 0;
    char pat[MAX_PATH]; snprintf(pat, sizeof pat, "%s\\*.dmp", s_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (n < 512) { snprintf(fs[n].name, MAX_PATH, "%s", fd.cFileName);
                       fs[n].t = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime; n++; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (n <= keep) return;
    qsort(fs, n, sizeof fs[0], [](const void* a, const void* b) {
        const ULONGLONG x = ((const F*)a)->t, y = ((const F*)b)->t; return x > y ? -1 : x < y ? 1 : 0; });
    for (int i = keep; i < n; i++) {
        char p[MAX_PATH * 2];
        snprintf(p, sizeof p, "%s\\%s", s_dir, fs[i].name); DeleteFileA(p);
        if (char* dot = strrchr(p, '.')) { memcpy(dot, ".txt", 5); DeleteFileA(p); }
    }
}

static const char* env2(const char* prefix, const char* name, char* buf, size_t n)
{
    snprintf(buf, n, "%s_%s", prefix, name);
    return getenv(buf);
}

static void init(const PortCrashConfig& cfg)
{
    static int done = 0;
    if (done) return;
    done = 1;
    s_cfg = cfg;
    s_start_ms = GetTickCount64();
    {
        char x[4] = "", sy[4] = "";
        if (GetEnvironmentVariableA("PORT_CRASH_EXTRA", x, sizeof x) >= sizeof x) x[0] = 0;
        if (GetEnvironmentVariableA("PORT_CRASH_SYMBOLS", sy, sizeof sy) >= sizeof sy) sy[0] = 0;
        s_noextra = x[0] == '0';
        s_nosym = s_noextra || sy[0] == '0';
        char dw[16] = "";
        if (GetEnvironmentVariableA("PORT_CRASH_DBGHELP_WAIT_MS", dw, sizeof dw) < sizeof dw && dw[0]) s_dbg_wait_ms = (DWORD)strtoul(dw, NULL, 10);
        if (s_noextra) { s_cfg.guest_state = nullptr; s_cfg.guest_threads = nullptr; }
    }
    char nb[64];
    const char* pe = getenv("PORT_CRASH_DUMPS");
    const char* le = cfg.env_prefix ? env2(cfg.env_prefix, "CRASH_DUMPS", nb, sizeof nb) : NULL;
    if ((pe && *pe == '0') || (le && *le == '0')) return;
    const char* full = getenv("CRASH_DUMP_FULL");
    s_full = (pe && !_stricmp(pe, "full")) || (le && !_stricmp(le, "full")) || (full && *full == '1');

    GetModuleFileNameA(NULL, s_exe, sizeof s_exe);
    s_exe_base = (DWORD64)GetModuleHandleA(NULL);
    {
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)s_exe_base;
        s_exe_end = s_exe_base + ((const IMAGE_NT_HEADERS64*)((const BYTE*)dos + dos->e_lfanew))->OptionalHeader.SizeOfImage;
    }
    /* <port>\build\X.exe (or build_*\) -> <port>\logs\crash */
    char d[MAX_PATH]; snprintf(d, sizeof d, "%s", s_exe);
    if (char* s = strrchr(d, '\\')) *s = 0;
    char up[MAX_PATH]; snprintf(up, sizeof up, "%s\\..", d);
    if (!GetFullPathNameA(up, sizeof s_root, s_root, NULL)) snprintf(s_root, sizeof s_root, "%s", up);
    snprintf(s_dir, sizeof s_dir, "%s\\logs", s_root);
    CreateDirectoryA(s_dir, NULL);
    strncat(s_dir, "\\crash", sizeof s_dir - strlen(s_dir) - 1);
    CreateDirectoryA(s_dir, NULL);

    if (HMODULE dh = LoadLibraryA("dbghelp.dll")) {
        s_mdw = (mdw_t)GetProcAddress(dh, "MiniDumpWriteDump");
        s_sw64 = (sw64_t)GetProcAddress(dh, "StackWalk64");
        s_sso = (sso_t)GetProcAddress(dh, "SymSetOptions");
        s_si = (si_t)GetProcAddress(dh, "SymInitialize");
        s_sfa = (sfa_t)GetProcAddress(dh, "SymFromAddr");
        s_sgl = (sgl_t)GetProcAddress(dh, "SymGetLineFromAddr64");
    }
    s_gtd = (gtd_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");

    s_req_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    s_done_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (s_req_ev && s_done_ev) s_thread = CreateThread(NULL, 1024 * 1024, reporter, NULL, 0, &s_thread_id);
    s_on = 1;

    s_prev_filter = SetUnhandledExceptionFilter(filter);
    AddVectoredExceptionHandler(0, fatal_veh);
    s_prev_abrt = signal(SIGABRT, on_abort);
    s_prev_purecall = _set_purecall_handler(on_purecall);
    s_prev_invparam = _set_invalid_parameter_handler(on_invparam);

    int keep = 12;
    if (const char* k = getenv("PORT_CRASH_KEEP")) { const int v = atoi(k); if (v >= 1) keep = v; }
    prune(keep);

    const char* hd = getenv("PORT_HANG_DUMP");
    if (hd && *hd == '0') s_hang_mode = 0;
    else if (hd && *hd == '1') {
        s_hang_mode = 2; s_hang_s = 20;
        if (const char* e = getenv("PORT_HANG_S")) { const int v = atoi(e); if (v >= 5) s_hang_s = v; }
    } else {
        s_hang_mode = 1; s_hang_s = 90;
        if (const char* e = cfg.env_prefix ? env2(cfg.env_prefix, "HANG_S", nb, sizeof nb) : NULL) {
            const int v = atoi(e); if (v >= 10) s_hang_s = v;
        }
    }
    if (s_hang_mode && cfg.main_progress) {
        HANDLE w = CreateThread(NULL, 64 * 1024, watchdog, NULL, 0, &s_watch_id);
        if (w) CloseHandle(w);
    }
    fprintf(stderr, "[crash] reporter on: reports in %s%s; hang watchdog %s\n", s_dir, s_full ? " (full memory)" : "",
            !s_hang_mode ? "off" : s_hang_mode == 2 ? "strict (PORT_HANG_DUMP=1)" : "on");
    if (s_hang_mode) fprintf(stderr, "[crash] hang limit %d s\n", s_hang_s);
}

} // namespace port_crash
#endif /* _WIN32 */
