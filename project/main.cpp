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

/* Online: settles the player name and the psnr server before the toolkit
 * registers its HLE handlers (ps3recomp/libs/network/np_psnr.h). Online is on
 * by default (online_setup); PS3_NET_ONLINE=0 turns it off. */
#include "np_psnr.h"

#include <string>
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

#include "rfom_crash.inc"   /* crash minidumps + hang watchdog -> logs\crash */
#include "rfom_pcport.inc"  /* save folder, RFOM_PRESET */

/* ---------------------------------------------------------------------------
 * RSX present backend
 * -----------------------------------------------------------------------*/
/* D3D12 on Windows, Metal on Apple, and the null backend's headless software
 * path anywhere else -- the same selection runtime/host/host_posix.c and the
 * toolkit's boot harness make. All three expose the same three entry points,
 * so the frame clock below is backend-agnostic. */
#if defined(_WIN32)
extern "C" int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
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
            if (spin < 0) { const char* e = getenv("RSX_SPINWAIT"); spin = e ? atoi(e) : 1; }
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
 * Online
 * -----------------------------------------------------------------------*/
/* The effective server and name are settled before the title runs because the
 * runtime caches getenv() answers and ppu_sysprx_register() chooses real
 * sockets over the offline stubs from PS3_NET_ONLINE at registration time.
 * Launcher.exe / run.bat set the variables through settings.bat; these flags
 * override them for a manual launch, matching the toolkit's runtime:
 *   --psnr host[:port]   sets PS3_NET_ONLINE=1 and PSNR_SERVER
 *   --username NAME      the online ID (PS3_NP_ONLINE_ID if not given)
 * np_psnr_setup also exports the two variables, so the level process the front
 * end spawns inherits them (stubs.cpp passes its environment on). */
static const char* arg_value(int argc, char** argv, const char* flag)
{
    for (int i = 2; i + 1 < argc; i++)
        if (!strcmp(argv[i], flag)) return argv[i + 1];
    return nullptr;
}

static void env_default(const char* k, const char* v)
{
    if (getenv(k)) return;
#ifdef _WIN32
    _putenv_s(k, v);
#else
    setenv(k, v, 0);
#endif
}

/* The hostname map new installs start with: every Sony hostname the front end
 * looks up -> the community server. */
static const char kDefaultHosts[] =
    "# Resistance: Fall of Man -- hostname overrides (Multiplayer page).\n"
    "# One \"hostname address\" per line (an IP or a domain name); '#' starts a comment.\n"
    "# Every Sony hostname goes to the community server; for a server on this PC use 127.0.0.1.\n"
    "\n"
    "resistance.ps3.online.scea.com            r1.rfom.online\n"
    "resistance.online.scee.com                r1.rfom.online\n"
    "resistance-muis.scej-online.jp            r1.rfom.online\n"
    "resistance-prod-muis.pdonline.scek.co.kr  r1.rfom.online\n"
    "rfom.rt.au.playstation.com                r1.rfom.online\n"
    "blob128.scea.com                          r1.rfom.online\n";

/* Absolute path of a file in the game folder (the working directory run.bat sets). */
static void game_path(const char* rel, char* out, size_t n)
{
#ifdef _WIN32
    if (!_fullpath(out, rel, n)) snprintf(out, n, "%s", rel);
#else
    if (!realpath(rel, out)) snprintf(out, n, "%s", rel);
#endif
}

/* The player name: id= in online.txt (the launcher's Multiplayer page). With
 * none set, a random one ("ExcitingHippo") is picked once and written back, so
 * it stays the same from run to run. Never the Windows user name. */
static void online_player_name(void)
{
    const char* env = getenv("PS3_NP_ONLINE_ID");
    if (env && *env) return;
    char path[1024];
    game_path("online.txt", path, sizeof path);
    std::string text;
    if (FILE* f = fopen(path, "rb")) {
        char b[4096]; size_t n;
        while ((n = fread(b, 1, sizeof b, f)) > 0) text.append(b, n);
        fclose(f);
    }
    /* find the id= line */
    size_t pos = 0, line = std::string::npos;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        if (text.compare(pos, 3, "id=") == 0) { line = pos; break; }
        pos = end + 1;
    }
    std::string name;
    if (line != std::string::npos) {
        size_t end = text.find_first_of("\r\n", line);
        name = text.substr(line + 3, (end == std::string::npos ? text.size() : end) - line - 3);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
    }
    if (name.empty()) {
        char id[17];
        np_psnr_random_id(id);
        name = id;
        if (line != std::string::npos) {
            size_t end = text.find_first_of("\r\n", line);
            text.replace(line, (end == std::string::npos ? text.size() : end) - line, "id=" + name);
        } else {
            if (!text.empty() && text.back() != '\n') text += "\r\n";
            text += "id=" + name + "\r\n";
        }
        if (FILE* f = fopen(path, "wb")) { fwrite(text.data(), 1, text.size(), f); fclose(f); }
        printf("[online] player name: %s (chosen at random; change id= in online.txt)\n", name.c_str());
    }
    env_default("PS3_NP_ONLINE_ID", name.c_str());
}

static void online_setup(int argc, char** argv)
{
    /* Online is always on: the game talks to the community server
     * (r1.rfom.online) once the player opens the Online menu, and nothing goes
     * on the network before that. PS3_NET_ONLINE=0 keeps a run fully offline.
     * Each default below is only applied when the variable is unset, so
     * tools\mp\trace_run.bat (a local server) still overrides them. */
    const char* on = getenv("PS3_NET_ONLINE");
    if (on && !strcmp(on, "0")) {
#ifdef _WIN32
        _putenv_s("PS3_NET_ONLINE", "");
#else
        unsetenv("PS3_NET_ONLINE");
#endif
        np_psnr_setup(arg_value(argc, argv, "--username"), nullptr);
        printf("[online] off (PS3_NET_ONLINE=0)\n");
        return;
    }
    env_default("PS3_NET_ONLINE", "1");
    /* The NP layer only reports "signed in" with a psnr server set. Resistance
     * matches through Medius, not Matching2, so nothing has to listen here. */
    env_default("PSNR_SERVER", "127.0.0.1:36100");
    /* sceNpManagerRequestTicket completes with event 255 ("async op finished"). */
    env_default("PS3_NP_TICKET_EVENT", "255");
    /* The community server cannot present a Sony-signed SCERT certificate. */
    env_default("PS3_MP_SCERT_ACCEPT", "1");
    char path[1024];
    if (!getenv("PS3_NET_HOSTS")) {
        game_path("net_hosts.txt", path, sizeof path);
        FILE* f = fopen(path, "rb");
        if (f) fclose(f);
        else if ((f = fopen(path, "wb"))) { fputs(kDefaultHosts, f); fclose(f); }
        env_default("PS3_NET_HOSTS", path);
    }
    if (!arg_value(argc, argv, "--username")) online_player_name();
    np_psnr_setup(arg_value(argc, argv, "--username"), arg_value(argc, argv, "--psnr"));

    const char* server = getenv("PSNR_SERVER");
    printf("[online] real host sockets on; psnr server %s\n",
           server && *server ? server : "(none -- sockets only)");
    if (getenv("PS3_NET_TRACE")) printf("[online] PS3_NET_TRACE: logging every packet\n");
    if (getenv("PS3_MP_SCERT_ACCEPT"))
        printf("[online] PS3_MP_SCERT_ACCEPT: accepting the community server's SCERT certificate\n");
    if (getenv("PS3_NET_HOSTS")) printf("[online] PS3_NET_HOSTS: %s\n", getenv("PS3_NET_HOSTS"));
}

/* The ticket login (SVO, SSL 3.0) trusts only the SCERT root embedded in the
 * front end at 0x10038310 (958 bytes). The community server's stand-in root
 * (tools\mp\r1_root.der: same names and size, the server's own key) replaces
 * it after the ELF is loaded. Done here rather than with PS3_MEMPATCH, which
 * the level program would inherit: the bytes are only replaced where they
 * really are that root, so game.self is never touched. PS3_MEMPATCH (set by
 * tools\mp\trace_run.bat for a local server) takes precedence. */
static void online_patch_root(void)
{
    const uint32_t ea = 0x10038310u, len = 958u;
    if (!getenv("PS3_NET_ONLINE") || getenv("PS3_MEMPATCH") || !vm_base) return;
    uint8_t* p = vm_base + ea;
    static const uint8_t hdr[4] = { 0x30, 0x82, 0x03, 0xBA };   /* SEQUENCE, 958 - 4 bytes */
    static const char name[] = "SCERT Root Authority";
    if (memcmp(p, hdr, 4) != 0) return;                         /* not the front end */
    bool named = false;
    for (uint32_t i = 0; i + sizeof name - 1 <= len && !named; i++)
        named = memcmp(p + i, name, sizeof name - 1) == 0;
    if (!named) return;
    char path[1024];
    game_path("tools\\mp\\r1_root.der", path, sizeof path);
    uint8_t buf[958];
    FILE* f = fopen(path, "rb");
    size_t n = f ? fread(buf, 1, sizeof buf, f) : 0;
    if (f) fclose(f);
    if (n != len || memcmp(buf, hdr, 4) != 0) {
        printf("[online] %s is missing or not a 958-byte root: the ticket login will fail\n", path);
        return;
    }
    memcpy(p, buf, len);
    printf("[online] community server TLS root patched in (%s)\n", path);
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
static bool alloc_guest_vm(void)
{
#ifdef _WIN32
    AddVectoredExceptionHandler(1, vm_commit_veh);
    vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE, PAGE_READWRITE);
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
        printf("usage: %s <PPU ELF> [--psnr host[:port]] [--username NAME]\n", argv[0]);
        return 2;
    }

#ifdef _WIN32
#pragma comment(lib, "winmm.lib")
    /* 1 ms timer resolution. The default granularity is about 15.6 ms, which
     * inflates every shorter wait the title makes and throttles the whole
     * thing. POSIX timers are already fine-grained. */
    timeBeginPeriod(1);
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: do not lose prints on a kill */
#endif

    printf("=== ps3recomp game runner ===\n");
    rfom_crash_init();
    rfom_save_location_init();
    rfom_preset_init();

    if (!alloc_guest_vm()) {
        fprintf(stderr, "ERROR: could not allocate the guest address space\n");
        return 1;
    }

    /* Before ppu_load_elf (it reads PS3_MEMPATCH) and ppu_sysprx_register (it
     * reads PS3_NET_ONLINE to pick real sockets over the offline libnet model). */
    online_setup(argc, argv);

    uint32_t entry = ppu_load_elf(argv[1]);
    if (!entry) {
        fprintf(stderr, "ERROR: could not load %s\n", argv[1]);
        return 1;
    }
    online_patch_root();

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
