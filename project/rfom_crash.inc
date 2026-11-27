/* Resistance port: crash and hang reports for play-testing (shared engine:
 * port_crash.h, also used by the Resistance 2 / 3 ports).
 *
 * A crash (an exception nobody handles, a stack overflow, abort(); the
 * guest-VM demand-paging handler runs first and is unaffected) writes
 * logs\crash\RFOM_<time>_<exe>_crash.dmp (small minidump) and
 * RFOM_<time>_<exe>_crash.txt (exception, faulting module + offset, lifted guest
 * function, every thread's native stack, the end of run.log, the settings),
 * adds a line to logs\crash\crashes.txt, then lets the process die as before
 * (after the filter that was installed before it). A hang -- the flip counter
 * stops once the game flips steadily; focus-loss pauses do not count -- writes
 * RFOM_<time>_<exe>_hang.dmp/.txt once per stall and keeps running
 * (FREEZE_RELAUNCH, debugtools.inc, still decides about restarts).
 * The game quitting on its own (libc exit() / sys_process_exit: an internal
 * error, back to the XMB on PS3) writes RFOM_<time>_<exe>_exit.dmp/.txt, shows
 * a message box and ends the process instead of freezing on the last frame.
 *   PORT_GUEST_EXIT_QUIT=0                    no report / box: the old freeze
 *   RFOM_CRASH_DUMPS=0 / PORT_CRASH_DUMPS=0   both off
 *   RFOM_CRASH_DUMPS=full                     full-memory dumps (several GB)
 *   PORT_HANG_DUMP=1   strict watchdog: 20 s (PORT_HANG_S) without a flip
 *   PORT_HANG_DUMP=0   no watchdog; unset: RFOM_HANG_S (default 90) s
 * Both executables (ResistanceFoM.exe front end, ResistanceFoMGame.exe game)
 * include this, so the exe name is part of every file name and line. */
#ifdef _WIN32
#include "port_crash.h"
#define PORT_FRAMETIME_IMPL   /* frame-time recorder: logs\frametimes_*.txt, marker key */
#include "port_frametime.h"

extern "C" volatile long rsx_focus_paused;     /* rsx_d3d12_backend.c: paused on focus loss */
extern "C" volatile long g_ps3_host_pause_gen; /* sys_ppu_thread.c: bumped on every pause / resume */
extern "C" uint32_t ppu_prof_resolve_host(void* ra);   /* ppu_loader.cpp: host pc -> lifted function */

static uint64_t rfom_crash_flips(void) { return cellGcm_flip_request_count(); }
static int rfom_crash_paused(void) { return rsx_focus_paused != 0; }
extern "C" int ppu_crash_guest_state(uint64_t* tid, uint32_t* lr, uint32_t* sp, uint32_t* sc_in,
                                     uint32_t* sc_last, uint32_t* callsite);   /* ppu_loader.cpp */
extern "C" size_t ppu_crash_guest_threads(char* buf, size_t cap);   /* ppu_loader.cpp */
static void rfom_crash_guest(PortGuestState* g)
{
    g->valid = ppu_crash_guest_state(&g->thread_id, &g->lr, &g->sp, &g->syscall_inflight,
                                     &g->syscall_last, &g->callsite);
}

/* R3 RL160363 (same runtime here): the game quit on its own -- libc exit() after an internal error
 * -> sys_prx_exitspawn_with_level, which used to be a no-op, so it froze on the last frame until the
 * player closed it. sysPrxForUser.c calls this once (not for a player quit): write the report, tell the
 * player, and the caller exits. PORT_GUEST_EXIT_QUIT=0: the old behaviour. */
extern "C" int (*volatile g_port_guest_exit_hook)(const char* via, int32_t* code, int code_known);
extern "C" PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern "C" void ppu_dump_guest_stack(ppu_context*, const char*);
static int rfom_guest_exit(const char* via, int32_t* code, int code_known)
{
    const char* q = getenv("PORT_GUEST_EXIT_QUIT");
    if (q && *q == '0') return 0;
    uint64_t tid = 0; uint32_t lr = 0, sp = 0, sc_in = 0, sc_last = 0, site = 0;
    const int g = ppu_crash_guest_state(&tid, &lr, &sp, &sc_in, &sc_last, &site);
    char cs[32] = "unknown";
    if (code_known) snprintf(cs, sizeof cs, "%d", (int)*code);
    fprintf(stderr, "[exit] the game quit on its own: %s (guest exit code %s)\n", via, cs);
    if (g_active_ctx) ppu_dump_guest_stack(g_active_ctx, "exit");
    fflush(stdout); fflush(stderr);
    char d[400];
    if (g)
        snprintf(d, sizeof d, "the game called %s (guest exit code %s) on guest thread %llu, lr=0x%08X sp=0x%08X, "
                 "last syscall %u, callsite 0x%08X", via, cs, (unsigned long long)tid, lr, sp, sc_last, site);
    else
        snprintf(d, sizeof d, "the game called %s (guest exit code %s) off a guest thread", via, cs);
    const int saved = port_crash::guest_exit(d);
    ClipCursor(NULL);
    if (HWND w = GetForegroundWindow()) {
        DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
        if (pid == GetCurrentProcessId()) ShowWindowAsync(w, SW_MINIMIZE);
    }
    fprintf(stderr, "[exit] closing the game\n");
    fflush(stderr);
    MessageBoxA(NULL, saved ? "The game hit an internal error and closed. A report was saved in logs\\crash."
                            : "The game hit an internal error and closed.",
                "Resistance: Fall of Man", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
    if (!code_known) *code = 1;
    return 1;
}

static void rfom_crash_init(void)
{
    static char exe[64] = "game";
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, sizeof path);
    if (const char* b = strrchr(path, '\\')) {
        snprintf(exe, sizeof exe, "%s", b + 1);
        if (char* dot = strrchr(exe, '.')) *dot = 0;
    }
    PortCrashConfig c = {};
    c.tag = "RFOM";
    c.title = "Resistance: Fall of Man";
    c.env_prefix = "RFOM";
    c.exe_suffix = exe;
    c.main_progress = rfom_crash_flips;
    c.main_start_min = 200;          /* not before the game flips steadily (boot) */
    c.paused = rfom_crash_paused;
    c.vm_base = &vm_base;
    c.vm_size = 0x100010000ull;      /* VM_SIZE, defined further down */
    c.guest_resolve = ppu_prof_resolve_host;
    c.guest_state = rfom_crash_guest;
    c.guest_threads = ppu_crash_guest_threads;
    c.after_report = port_ft_on_report;
    port_crash::init(c);
    g_port_guest_exit_hook = rfom_guest_exit;
    PortFtConfig f = {};
    f.title = c.title;
    f.exe_suffix = exe;
    f.paused = rfom_crash_paused;
    f.pause_gen = [](void) -> long { return g_ps3_host_pause_gen; };
    port_ft_init(f);
}
#else
static void rfom_crash_init(void) {}
#endif