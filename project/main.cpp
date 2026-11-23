/*
 * ps3recomp game project -- entry point template.
 *
 * This is the runner. It boots a lifted title through the toolkit's PPU
 * scaffold: allocate the flat guest VM, load the PPU ELF named on the command
 * line, register the lifted function table and the HLE NID handlers, start a
 * frame clock, and dispatch the entry OPD.
 *
 * The scaffold does the work; nothing below reimplements it. ppu_load_elf,
 * ppu_recomp_register, ppu_hle_init, ppu_sysprx_register, ppu_fs_register,
 * lv2_init_syscalls and ppu_run all come from the toolkit, compiled into this
 * project from PS3RECOMP_DIR (see CMakeLists.txt). What is left here is the
 * part a port owns: which backend to present through, how the frame clock is
 * paced, and whatever diagnostics the title turns out to need.
 *
 * runtime/ppu/tests/boot_main.cpp in the toolkit is the same boot in its
 * fully-instrumented form -- crash filter, hang watchdog, guest-PC sampling
 * profiler. Read it when a boot goes wrong; copy from it what the title
 * needs. It is deliberately not what a fresh project starts with.
 *
 * The one part that IS duplicated from it is the frame clock below, because a
 * port is expected to change its pacing and a shared one would be the wrong
 * shape for that. It was copied faithfully, comments and all. If a pacing bug
 * is fixed in one of the two, look at the other.
 *
 * This builds on Windows, macOS and Linux. CI builds and runs it against the
 * boot smoke title on the last two, and compile-checks it against clang-cl on
 * the first (tools/check_ppu_scaffold.py).
 */

/* The lifter's generated header, which declares ppu_context and the function
 * table. It comes first: everything below is written against that struct. */
#include "ppu_recomp.h"

/* The Win32 names -- Sleep, GetTickCount64, CreateThread, InterlockedIncrement,
 * VirtualAlloc, the scalar typedefs -- from one place. On Windows this is a
 * passthrough to <windows.h>; off Windows the toolkit's shim supplies the same
 * names over pthreads and mmap, so the call sites below are written once. */
#include "win32_compat.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/mman.h>   /* mmap: the guest arena, see alloc_guest_vm */
#endif

#ifdef _WIN32
/* timeBeginPeriod: <windows.h> arrives with WIN32_LEAN_AND_MEAN set, which
 * excludes the multimedia timer API, so it is asked for by name -- and after
 * windows.h, since timeapi.h uses UINT. No shim exists off Windows because
 * there is nothing to shim: the 15.6 ms default timer granularity this works
 * around is a Windows problem. */
#include <timeapi.h>
#include <dwmapi.h>     /* RFOM_MOVIE_VBLANK_LOCK: DwmGetCompositionTimingInfo */
#pragma comment(lib, "dwmapi.lib")
#endif

/* ---------------------------------------------------------------------------
 * What the scaffold gives this file
 * -----------------------------------------------------------------------*/
extern "C" {
uint32_t ppu_load_elf(const char* path);      /* ELF -> guest RAM, returns entry OPD */
void     ppu_recomp_register(void);           /* generated: lifted table -> address map */
void     ppu_hle_init(void);                  /* firmware import NID -> HLE handler */
void     ppu_sysprx_register(void);           /* boot-critical CRT (sys_initialize_tls, ...) */
void     ppu_fs_register(void);               /* cellFs over the real game directory */
void     lv2_init_syscalls(void);             /* the lv2 syscall table */
int      ppu_run(uint32_t entry_opd, uint32_t stack_top);

/* This project's own: load whatever real system PRX modules the title needs
 * into guest RAM and register their exports. stubs.cpp has the empty version
 * a game with no lifted PRX wants. */
void     ps3_load_prx_modules(void);

extern const char* ppu_vfs_root;              /* host dir the PS3 mount points map into */

/* The status the guest handed to sys_process_exit. A title that exits that way
 * never returns through ppu_run, so this is only read on the path where the
 * entry function unwound instead. */
extern int     g_sys_process_exit_called;
extern int32_t g_sys_process_exit_code;

/* Host-provided symbols the runtime and the HLE libraries link against. */
uint8_t* vm_base = nullptr;
extern uint32_t ppu_vm_size;                  /* ppu_loader.cpp: the OOB guard */
extern uint32_t ppu_hle_inject_base;          /* cellGcmSys.c (v163 boot check) */

/* Guest-callback dispatch. g_ps3_guest_caller is the hook the HLE runtime
 * calls back into recompiled code through -- cellSysutil events and the GCM
 * vblank/flip handlers. ppu_guest_call does the OPD -> dispatch. */
typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
extern ps3_guest_caller_fn g_ps3_guest_caller;
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t, uint64_t);

/* The GCM half of the frame clock. */
void     cellGcmTickVBlank(void);
void     cellGcmTickFlip(void);
int      cellGcm_take_flip_pending(void);
void     cellGcm_rsx_process_fifo(void);      /* drain get -> put */
unsigned cellGcm_flip_request_count(void);
}

#include "port_cpucheck.h"  /* AVX2 (x86-64-v3) build: clear error on older CPUs */
#include "rfom_crash.inc"   /* crash minidumps + hang watchdog -> logs\crash */
#include "rfom_pcport.inc"  /* save folder, RFOM_PRESET */
#include "port_native_summary.h"
#include "port_mem.h"       /* PORT_LARGE_PAGES: guest memory in large pages (alloc_guest_vm) */
#include "port_cpu_sets.h"  /* hybrid CPUs: P-core placement (debugtools.inc), the [native] pcore-pin entry */
#include "../ps3recomp/libs/video/rsx_mt_record.h"  /* rsx_mtr_thread_count / rsx_mtr_replay_env: the [native] mt-record / mt-replay entries */

/* ---------------------------------------------------------------------------
 * RSX present backend
 * -----------------------------------------------------------------------*/
/* D3D12 on Windows, Metal on Apple, and the null backend's headless software
 * path anywhere else -- the same selection runtime/host/host_posix.c and the
 * toolkit's boot harness make. All three expose the same three entry points,
 * so the frame clock below is backend-agnostic. */
extern "C" int  spu_sched_effective(void);   /* spu_channels.c: PORT_SPU_SCHED, [native] line */
#if defined(_WIN32)
extern "C" int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_d3d12_dpi_init(void);
extern "C" void rsx_d3d12_backend_present(void);
extern "C" int  rsx_d3d12_backend_pump_messages(void);
#  define rsx_backend_init    rsx_d3d12_backend_init
#  define rsx_backend_present rsx_d3d12_backend_present
#  define rsx_backend_pump    rsx_d3d12_backend_pump_messages
#  define RSX_BACKEND_NAME    "D3D12"
#elif defined(__APPLE__)
extern "C" int  rsx_metal_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_metal_backend_present(void);
extern "C" int  rsx_metal_backend_pump_messages(void);
#  define rsx_backend_init    rsx_metal_backend_init
#  define rsx_backend_present rsx_metal_backend_present
#  define rsx_backend_pump    rsx_metal_backend_pump_messages
#  define RSX_BACKEND_NAME    "Metal"
#else
extern "C" int  rsx_null_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_null_backend_present(void);
extern "C" int  rsx_null_backend_pump_messages(void);
#  define rsx_backend_init    rsx_null_backend_init
#  define rsx_backend_present rsx_null_backend_present
#  define rsx_backend_pump    rsx_null_backend_pump_messages
#  define RSX_BACKEND_NAME    "null (headless software)"
#endif

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/
/* The flat VM must span every region the PS3 memory map uses, not just the
 * game image: sys_ppu_thread_create puts thread stacks at 0xD0000000, so a
 * smaller arena makes every spawned thread's stack access out of bounds. */
#define VM_SIZE       0x100010000ull   /* the full 32-bit guest space + a 64K guard */
#define STACK_TOP     0x0FF00000u      /* main-thread stack, below the 0x10000000 segment */
#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 720

/* ---------------------------------------------------------------------------
 * Frame clock
 * -----------------------------------------------------------------------*/
/* On real hardware the RSX raises a vblank interrupt about 60 times a second
 * and that is what drives the game's frame loop. With no RSX it is synthesized
 * here: a host thread calls cellGcmTickVBlank/TickFlip, which invoke the
 * handlers the guest registered, and drains the GCM FIFO. Without this the
 * title initialises, registers its handlers and then waits forever.
 *
 * The ticks are driven off real elapsed time rather than off how long present()
 * took. A hidden or occluded window makes present block hard, and pacing the
 * ticks behind it paces the whole game behind it. */
static volatile LONG g_frames_presented = 0;

/* Frames handed to the backend at a guest FLIP boundary -- one per frame the
 * guest actually finished. The presents made before the guest's first flip, so
 * a fresh window is not left blank through a long boot, carry no guest frame
 * and are deliberately not counted. */
extern "C" unsigned ppu_boot_frames_presented(void)
{
    return (unsigned)g_frames_presented;
}

static void present_guest_frame(void)
{
    rsx_backend_present();
    /* This thread increments and guest threads read, so interlocked rather
     * than a volatile ++, which on arm64 is neither atomic nor a fence. */
    InterlockedIncrement(&g_frames_presented);
}

extern "C" uint32_t cellGcm_rsx_put_snapshot(void);
extern "C" void* cellGcm_fifo_kick_event(void);   /* RSX_SPINWAIT=4 */
extern "C" int ps3_host_core_scarce(void);       /* sys_ppu_thread.c, PORT_CORE_SCARCE */
extern "C" uint32_t cellGcm_get_flip_mode(void);
extern "C" int cellVdec_movie_active(unsigned window_ms);

/* Resistance port (v86): the vblank clock in microseconds from QPC.
 * GetTickCount64 only moves in ~15.6 ms steps (timeBeginPeriod does not
 * change it), so the 60 Hz beat that completes flips fired on a jittery
 * 0/15.6/31.2 ms grid and a finished frame often waited a whole extra beat
 * for its flip. RSX_VBLANK_US overrides the period (default 16683 = 59.94 Hz). */
static long long fc_us(void)
{
    static LARGE_INTEGER f = { 0 };
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (long long)((double)c.QuadPart * 1e6 / (double)f.QuadPart);
}

static DWORD WINAPI frame_clock(LPVOID)
{
    const char* title = getenv("PS3_TITLE");
    if (!title || !*title) title = "ps3recomp";

    int rsx_ok = (rsx_backend_init(WINDOW_WIDTH, WINDOW_HEIGHT, title) == 0);
    fprintf(stderr, "[rsx] %s backend init %s\n", RSX_BACKEND_NAME,
            rsx_ok ? "OK -- window open" : "FAILED");

    unsigned  last_flip = 0;
    long long next_tick = fc_us();
    static long long vb_us = -1;
    if (vb_us < 0) { const char* e = getenv("RSX_VBLANK_US"); vb_us = e ? atoll(e) : 16683; if (vb_us < 1000) vb_us = 16683; }

    for (;;) {
        /* v200: window in the background (RSX_FOCUS_PAUSE) or the quit prompt
         * is up: stop ticking vblank and draining the FIFO. The guest waits on
         * the RSX like it would on a stalled GPU; keep pumping messages so the
         * window still reacts. Game time is clamped per frame on resume. */
        if (rsx_ok && rsx_focus_paused) {
            Sleep(20);
            if (rsx_backend_pump() != 0) rsx_ok = 0;
            next_tick = fc_us();
            continue;
        }
        /* Resistance port: wake as soon as the guest publishes new FIFO work
         * (its `put` moves) instead of a blind Sleep(4). The guest fences many
         * passes per frame on labels this drain writes, so each blind sleep was
         * paid several times a frame. Spin-yield up to ~2 ms, then sleep 1 ms.
         * RSX_SPINWAIT=0 restores Sleep(4). */
        {
            static int spin = -1;
            if (spin < 0) {
                /* Unset: the busy spin (1) as before, or the event-woken wait (4) on a CPU with
                 * few physical cores (ps3_host_core_scarce, PORT_CORE_SCARCE), where the spin
                 * holds a core the guest's main thread needs. */
                const char* e = getenv("RSX_SPINWAIT");
                spin = e && *e ? atoi(e) : (ps3_host_core_scarce() ? 4 : 1);
                fprintf(stderr, "[rsx] frame clock idle: RSX_SPINWAIT=%d%s\n", spin,
                        e && *e ? "" : spin == 4 ? " (default: kick-event wait, few cores)" : " (default: spin)");
            }
            if (!spin) Sleep(4);
            else {
                static uint32_t last_put = 0;
                uint32_t p0 = cellGcm_rsx_put_snapshot();
                if (p0 == last_put) {
                    /* Resistance port: keep spinning until the next vblank
                     * tick is due instead of falling into Sleep(1) after 2 ms.
                     * The guest ping-pongs with this thread many times a frame
                     * (kick a segment, wait for its label); every Sleep(1) was
                     * 1-2 ms of dead latency per round trip. Costs one core.
                     * RSX_SPINWAIT=2 restores the 2 ms + Sleep(1) behaviour. */
                    ULONGLONG t0 = GetTickCount64();
                    if (spin == 2) {
                        while (cellGcm_rsx_put_snapshot() == p0 && GetTickCount64() - t0 < 2)
                            SwitchToThread();
                        if (cellGcm_rsx_put_snapshot() == p0) Sleep(1);
                    } else if (spin == 4) {
                        /* RSX_SPINWAIT=4 (frame-clock kick wait, after the inFAMOUS 2 PC
                         * port's 0006-frame-clock-kick-wait, MIT): spin RSX_SPIN_US (150),
                         * then sleep until the FIFO kick event (a guest fence poll that finds
                         * the queue dry, cellGcm_ref_on_poll; cellGcm_wait_fifo_caught_up) or
                         * a high-resolution timer of RSX_IDLE_US (100) -- the guest's `put`
                         * is a plain store that cannot signal, so it is still polled at that
                         * period. GCM_FIFO_KICK=0 leaves the timer only. */
                        static long long spin_us = -1, idle_us = -1;
                        static HANDLE tmr = NULL, kev = NULL;
                        if (spin_us < 0) {
                            const char* e = getenv("RSX_SPIN_US");  spin_us = e ? atoll(e) : 150;
                            e = getenv("RSX_IDLE_US"); idle_us = e ? atoll(e) : 100;
                            if (idle_us < 10) idle_us = 10;
                            tmr = CreateWaitableTimerExW(NULL, NULL,
                                      0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                                      TIMER_ALL_ACCESS);
                            if (!tmr) { timeBeginPeriod(1); tmr = CreateWaitableTimerW(NULL, TRUE, NULL); }
                            e = getenv("GCM_FIFO_KICK");
                            if (!(e && *e == '0')) kev = (HANDLE)cellGcm_fifo_kick_event();
                        }
                        const long long ts = fc_us();
                        unsigned k = 0;
                        for (;;) {
                            if (cellGcm_rsx_put_snapshot() != p0) break;
                            const long long now_s = fc_us();
                            if (now_s - next_tick >= 0) break;
                            if (now_s - ts < spin_us) {
                                if (++k & 63u) YieldProcessor(); else SwitchToThread();
                                continue;
                            }
                            long long w = next_tick - now_s;
                            if (w > idle_us) w = idle_us;
                            HANDLE hs[2]; DWORD n = 0;
                            LARGE_INTEGER due; due.QuadPart = -(w * 10);
                            if (tmr && SetWaitableTimer(tmr, &due, 0, NULL, NULL, FALSE)) hs[n++] = tmr;
                            if (kev) hs[n++] = kev;
                            if (!n) { Sleep(0); continue; }
                            const DWORD r = WaitForMultipleObjects(n, hs, FALSE, 2);
                            if (kev && r == WAIT_OBJECT_0 + n - 1) break;   /* kicked: walk now */
                        }
                    } else {
                        unsigned k = 0;
                        while (cellGcm_rsx_put_snapshot() == p0 &&
                               fc_us() - next_tick < 0) {
                            if (++k & 63u) YieldProcessor(); else SwitchToThread();
                        }
                    }
                }
                last_put = cellGcm_rsx_put_snapshot();
            }
        }
        long long now = fc_us();

#ifdef _WIN32
        /* RFOM_MOVIE_VBLANK_LOCK (default 1; 0 = old free-running clock; off by
         * default with RFOM_FPS=vrr): while a movie plays (flip mode 2 and the
         * video decoder fed in the last 500 ms) phase/period-lock the synthetic
         * vblank to the display's real one (DWM timing), so each 30 fps movie
         * frame (2 ticks) spans an exact integer number of refreshes on 60/120/
         * 240 Hz panels. Other refresh rates, and gameplay, keep the 59.94 clock. */
        {
            static int lock = -1; static long long base_vb = 0; static ULONGLONG last_q = 0;
            if (lock < 0) { char b[8]; DWORD n = GetEnvironmentVariableA("RFOM_MOVIE_VBLANK_LOCK", b, sizeof b);
                            char f[8]; DWORD fn = GetEnvironmentVariableA("RFOM_FPS", f, sizeof f);
                            const int vrr = fn > 0 && fn < sizeof f && !_stricmp(f, "vrr");
                            lock = (n > 0 && n < sizeof b) ? atoi(b) : (vrr ? 0 : 1); base_vb = vb_us;
                            fprintf(stderr, "[rsx] movie vblank lock %s (RFOM_MOVIE_VBLANK_LOCK)\n", lock ? "on" : "off"); }
            if (lock) {
                if (cellGcm_get_flip_mode() == 2 && cellVdec_movie_active(500)) {
                    const ULONGLONG t = GetTickCount64();
                    if (t - last_q >= 500) {
                        last_q = t;
                        DWM_TIMING_INFO ti = {}; ti.cbSize = sizeof ti;
                        LARGE_INTEGER qf; QueryPerformanceFrequency(&qf);
                        if (SUCCEEDED(DwmGetCompositionTimingInfo(NULL, &ti)) && ti.qpcRefreshPeriod && qf.QuadPart) {
                            const double per_us = (double)ti.qpcRefreshPeriod * 1e6 / (double)qf.QuadPart;
                            const int k = (int)(16683.0 / per_us + 0.5);   /* refreshes per 60 Hz tick */
                            const double d = k * per_us - 16683.0;
                            if (k >= 1 && k <= 4 && d > -400.0 && d < 400.0) {
                                vb_us = (long long)(k * per_us + 0.5);
                                /* next tick 1.5 ms after a real vblank */
                                LARGE_INTEGER qn; QueryPerformanceCounter(&qn);
                                const long long since = (long long)((double)(qn.QuadPart - (LONGLONG)ti.qpcVBlank) * 1e6 / (double)qf.QuadPart);
                                long long p = (long long)per_us; if (p <= 0) p = 1;
                                const long long ph = ((since % p) + p) % p;
                                /* phase error of next_tick against "vblank + 1.5 ms", in
                                 * (-p/2, p/2]: shift by it, never by whole periods (a due
                                 * tick must not be pushed back a refresh) */
                                const long long tgt = now - ph + 1500;
                                long long off = ((next_tick - tgt) % p + p) % p;
                                if (off > p / 2) off -= p;
                                if (off > 1000 || off < -1000) next_tick -= off;
                            }
                        }
                    }
                } else if (vb_us != base_vb) vb_us = base_vb;
            }
        }
#endif

        int fired = 0;
        while (now - next_tick >= 0 && fired < 240) {
            cellGcmTickVBlank();
            cellGcmTickFlip();
            /* Present a pending flip BEFORE draining any further. The flip
             * fires at a get==put frame boundary on the guest thread, so the
             * batch held right now is exactly the completed frame; presenting
             * after the drain races the guest's next-frame writes and shows a
             * mixed one. */
            if (rsx_ok && cellGcm_take_flip_pending()) {
                present_guest_frame();
                last_flip = cellGcm_flip_request_count();
            }
            /* Drain the FIFO every tick. This is what writes the RSX sync-fence
             * labels the game's per-frame logic blocks on, so it has to keep
             * advancing at 60 Hz even while present() throttles. */
            if (rsx_ok) cellGcm_rsx_process_fifo();
            next_tick += vb_us;          /* ~60 Hz */
            fired++;
        }
        if (fired >= 240) next_tick = now;   /* fell too far behind -- resync */

        /* Drain at the outer cadence too, not only on the 16 ms tick: titles
         * fence every render pass on a label the drain writes, and one wave of
         * those at 16 ms apiece paces the guest into single-figure frame rates.
         * The real RSX writes them in microseconds. */
        if (rsx_ok) {
            if (cellGcm_take_flip_pending()) {
                present_guest_frame();
                last_flip = cellGcm_flip_request_count();
            }
            cellGcm_rsx_process_fifo();

            if (rsx_backend_pump() != 0) {
                rsx_ok = 0;              /* window closed */
                continue;
            }
            /* Present on a guest flip. A present on a fixed clock can catch the
             * drain mid-frame and flash a partial one. Before the first flip
             * present freely, so the window is not blank during boot. */
            unsigned fc = cellGcm_flip_request_count();
            if (fc != last_flip) {
                present_guest_frame();
                last_flip = fc;
            } else if (fc == 0) {
                rsx_backend_present();
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * VFS root
 * -----------------------------------------------------------------------*/
/* Derive the directory holding PS3_GAME from the EBOOT path
 * <root>/PS3_GAME/USRDIR/EBOOT.elf -> <root>. $PS3_VFS_ROOT overrides. */
static char s_vfs_root[1024];

static void derive_vfs_root(const char* eboot)
{
    const char* env = getenv("PS3_VFS_ROOT");
    if (env && *env) { ppu_vfs_root = env; return; }

    strncpy(s_vfs_root, eboot, sizeof s_vfs_root - 1);
    for (char* p = s_vfs_root; *p; p++) if (*p == '\\') *p = '/';
    /* strip EBOOT.elf, USRDIR and PS3_GAME */
    for (int i = 0; i < 3; i++) { char* s = strrchr(s_vfs_root, '/'); if (s) *s = 0; }
    if (!s_vfs_root[0]) strcpy(s_vfs_root, ".");
    ppu_vfs_root = s_vfs_root;
}

/* ---------------------------------------------------------------------------
 * Guest VM
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
/* Demand-paging for the flat VM: reserve the whole 4 GB guest space, which
 * costs no commit, and commit each 64 KB page on first access. Every 32-bit
 * guest offset is then valid, so a garbage guest pointer reads zero instead of
 * killing the process. Faults outside the arena fall through untouched. */
static LONG WINAPI vm_commit_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR fault = ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t base  = (uintptr_t)vm_base;
        if (vm_base && fault >= base && fault < base + VM_SIZE) {
            void* page = (void*)(fault & ~(uintptr_t)0xFFFF);
            if (VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

extern "C" void vm_inline_refresh(void);
static port_large_pages s_large_pages;
static bool alloc_guest_vm(void)
{
#ifdef _WIN32
    AddVectoredExceptionHandler(1, vm_commit_veh);
    /* PORT_LARGE_PAGES: guest [0, 0x20000000) -- code, data and the sys_memory heap, the game's hot range --
     * committed up front in large pages. Nothing in R1 changes the protection of that range (vm_protect is used
     * for the stack guards at 0xD0000000 only; the D3D12 renderer has no write watches), and nothing decommits
     * guest memory; vm_commit and the commit-on-fault handler skip committed runs. */
    s_large_pages.requested = port_large_pages_env();
    vm_base = s_large_pages.requested
                  ? (uint8_t*)port_mem_reserve_large_window(VM_SIZE, 0, 0x20000000u, &s_large_pages) : nullptr;
    if (!vm_base) vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE, PAGE_READWRITE);
    ppu_vm_size = 0;              /* the whole space is backed; no OOB guard needed */
#else
    /* No vectored exception handlers off Windows, so the arena is mapped up
     * front and lazily backed by the OS: only pages the title touches cost
     * anything. It stops below the 0xE0000000 mark, so ppu_vm_size arms the
     * loader's bounds check for what is left.
     *
     * mmap, not calloc: vm_commit() hands vm_base + addr to mprotect, which
     * takes page-aligned addresses only. glibc serves an allocation this big
     * from its own mmap chunk and returns a pointer 16 bytes past the page
     * start, so every commit failed with EINVAL and sys_memory_allocate
     * answered ENOMEM to every call. mmap's base is page-aligned and zeroed. */
    void* p = mmap(NULL, 0xE0000000u, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    vm_base = (p == MAP_FAILED) ? nullptr : (uint8_t*)p;
    ppu_vm_size = 0xE0000000u;
#endif
    vm_inline_refresh();   /* ppu_recomp.h inline bounds (ppu_loader.cpp) */
    /* v163: the lifted game code is built with these bounds as constants
     * (PPU_FIXED_BOUNDS): VM end 0xE0000000, gcm ref-poll word 0x20002008. */
    { if (ppu_hle_inject_base != 0x20000000u || (ppu_vm_size && ppu_vm_size != 0xE0000000u))
          fprintf(stderr, "[vm] WARNING: inject base 0x%08X / vm size 0x%08X differ from the PPU_FIXED_BOUNDS constants\n",
                  ppu_hle_inject_base, ppu_vm_size); }
    return vm_base != nullptr;
}

/* ---------------------------------------------------------------------------
 * Main
 * -----------------------------------------------------------------------*/
static void harness_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4,
                                 uint64_t a5, uint64_t a6, uint64_t a7)
{
    ppu_guest_call(opd, a0, a1, a2, a3, a4, a5, a6, a7);
}

extern "C" void cellGame_init_from_paramsfo(const char* sfo_path);

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: %s <PPU ELF>\n", argv[0]);
        return 2;
    }

#ifdef _WIN32
#pragma comment(lib, "winmm.lib")
    /* Real pixels under Windows display scaling (issue #25), before any window
     * or DPI-dependent call; RSX_DPI_AWARE=0 skips it. */
    rsx_d3d12_dpi_init();
    /* 1 ms timer resolution. The default granularity is about 15.6 ms, which
     * inflates every shorter wait the title makes and throttles the whole
     * thing. POSIX timers are already fine-grained. */
    timeBeginPeriod(1);
    /* Windows 11 ignores timer-resolution requests of a process whose window
     * is minimised / occluded (power throttling): opt out so the 1 ms
     * resolution and the frame limiter hold in borderless play behind
     * overlays. PORT_TIMER_NOTHROTTLE=0 keeps the default Windows behaviour. */
    {
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif
        char nt[4] = "", pn[4] = "";
        if (GetEnvironmentVariableA("PORT_TIMER_NOTHROTTLE", nt, sizeof nt) >= sizeof nt) nt[0] = 0;
        /* PORT_ECOQOS=1 keeps the Windows default (R2/R3 naming); otherwise opt the whole
         * process out of EcoQoS (execution-speed power throttling). Windows 11
         * applies it heuristically to apps whose window is not in the
         * foreground, and on hybrid CPUs that also steers their threads to
         * E-cores -- the game then stutters until it is alt-tabbed to. */
        if (GetEnvironmentVariableA("PORT_ECOQOS", pn, sizeof pn) >= sizeof pn) pn[0] = 0;
        /* One call for both: each SetProcessInformation(ProcessPowerThrottling) replaces
         * the whole ControlMask, so a second call would undo the first opt-out. */
        const ULONG want_timer = nt[0] != '0' ? (ULONG)PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION : 0u;
        const ULONG want_eco = atoi(pn) == 0 ? (ULONG)PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0u;
        if (want_timer | want_eco) {
            PROCESS_POWER_THROTTLING_STATE pt = {};
            pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
            pt.ControlMask = want_timer | want_eco;
            pt.StateMask = 0;   /* opt out of each bit in ControlMask */
            BOOL ok = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);
            BOOL timer_ok = ok && want_timer, eco_ok = ok && want_eco;
            if (!ok && want_timer && want_eco) {   /* pre-Windows 11 rejects the timer bit: keep the EcoQoS opt-out */
                pt.ControlMask = want_eco;
                eco_ok = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);
            }
            if (want_timer)
                fprintf(stderr, "[timer] 1 ms timer resolution; ignore-timer-resolution throttling opt-out %s\n",
                        timer_ok ? "on" : "unavailable (pre-Windows 11)");
            if (want_eco)
                fprintf(stderr, "[timer] EcoQoS (background power throttling) opt-out %s (PORT_ECOQOS=1 keeps the Windows default)\n",
                        eco_ok ? "on" : "unavailable");
        }
    }    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: do not lose prints on a kill */
#endif

    printf("=== ps3recomp game runner ===\n");
    {   /* defaults: rsx_texture_layout.c, rsx_native_hot.c, rsx_d3d12_backend.c; pcore-pin: port_cpu_sets.h
         * (debugtools.inc places the threads; the same query here gives the effective state) */
        char pcore_sw[96];
        port_pcore_plan pcore;
#ifdef _WIN32
        port_pcore_query(&pcore, 0);
#else
        memset(&pcore, 0, sizeof pcore);
        pcore.why_off = "not Windows";
#endif
        const int pcore_on = !pcore.why_off;
        port_pcore_label(pcore_sw, sizeof pcore_sw, pcore_on, pcore.why_off);
        const port_native_feature k_native[] = {
            { "texdec", "RSX_NATIVE_TEXDEC", 1 }, { "wccopy", "RSX_NATIVE_WCCOPY", 1 }, { "srv-cache", "RSX_SRV_CACHE", 0 },
                        { "ssao", "RSX_SSAO", 0 },   /* rsx_ssao.inc (D3D12 post pass) */
                        { "hd-ui", "RSX_HD_UI", 0 },
                        { "pcore-pin", pcore_sw, pcore_on },
                        { "mt-record", "RSX_MT_RECORD", 1 },   /* rsx_mt_record.c (render-thread offload) */
                        { "mt-replay", "RSX_MT_REPLAY", RSX_MTR_REPLAY_DEFAULT },   /* rsx_mt_replay.c (draw runs on the pool) */
                        { "spu-sched", "PORT_SPU_SCHED", spu_sched_effective() },   /* spu_channels.c */
        };
        enum { k_n = (int)(sizeof k_native / sizeof k_native[0]), k_hdui = 4, k_mtrec = 6, k_mtrep = 7 };
        int st[k_n];
        for (int i = 0; i < k_n; i++) st[i] = port_native_on(&k_native[i]);
#ifdef _WIN32
        {   /* mt-record: off when it gets no worker (rsx_mtr_thread_count, the pool's own rule) */
            SYSTEM_INFO si; GetSystemInfo(&si);
            if (rsx_mtr_thread_count((unsigned)si.dwNumberOfProcessors) <= 0) st[k_mtrec] = 0;
        }
#else
        st[k_mtrec] = 0;
#endif
        st[k_mtrep] = st[k_mtrec] && rsx_mtr_replay_env();   /* rides on the pool; RT_REBIND keeps it off */
        {   /* RSX_HD_UI also takes a folder (r1_hd_ui.h r1hd_env_dir): on unless unset, empty, 0 or off */
            const char* e = getenv("RSX_HD_UI");
            st[k_hdui] = (e && *e && strcmp(e, "0") &&
                          !((e[0] | 32) == 'o' && (e[1] | 32) == 'f' && (e[2] | 32) == 'f' && !e[3])) ? 1 : 0;
        }
        port_native_summary_state(k_native, st, k_n);
    }
    rfom_crash_init();
    rfom_save_location_init();
    rfom_preset_init();

    if (!alloc_guest_vm()) {
        fprintf(stderr, "ERROR: could not allocate the guest address space\n");
        return 1;
    }
    port_large_pages_summary(&s_large_pages);

    uint32_t entry = ppu_load_elf(argv[1]);
    if (!entry) {
        fprintf(stderr, "ERROR: could not load %s\n", argv[1]);
        return 1;
    }

    derive_vfs_root(argv[1]);
    printf("[boot] VFS root: %s\n", ppu_vfs_root);

    /* Resistance port: real title id (BCUS98107) from PARAM.SFO, not BLES00000 */
    {
        char sfo[1100];
        snprintf(sfo, sizeof sfo, "%s/PS3_GAME/PARAM.SFO", ppu_vfs_root);
        cellGame_init_from_paramsfo(sfo);
    }

    ppu_recomp_register();   /* lifted function table -> address map */
    ps3_load_prx_modules();  /* this game's lifted system PRX, if it has any */
    ppu_hle_init();          /* firmware import NID -> HLE handlers */
    ppu_sysprx_register();   /* boot-critical CRT */
    ppu_fs_register();       /* cellFs over the game directory */
    lv2_init_syscalls();     /* the lv2 syscall table */

    /* Install the guest-callback hook, then start the frame clock. It no-ops
     * until the title registers its vblank and flip handlers during init. */
    g_ps3_guest_caller = harness_guest_caller;
    CreateThread(NULL, 4u * 1024 * 1024, frame_clock, NULL, 0, NULL);

    printf("\n[boot] dispatching entry OPD 0x%08X (stack top 0x%08X)\n\n",
           entry, STACK_TOP);

    int rc = ppu_run(entry, STACK_TOP);
    printf("\n[boot] ppu_run returned %d (entry function unwound)\n", rc);

    /* A title that called sys_process_exit never reaches this line: that path
     * ends in the host exit(). Getting here means the entry function returned
     * instead, so hand back the status the guest published if it published
     * one. Returning a hardcoded 0 would report success for a run that never
     * got anywhere. */
    return g_sys_process_exit_called ? (int)g_sys_process_exit_code : 0;
}
