/* Resistance: Fall of Man PC port: PC extras (the PC menu's "More PC Options"
 * page and everything behind it). Runs on the backend's PC-menu thread
 * (rsx_d3d12_backend.c pc_quit_thread -> g_pc_ui_tick, every ~30 ms) plus once
 * per game frame from stubs.cpp rfom_frame_wait (rfx_frame_tick).
 *
 *   Extras menu      F10 -> More PC Options (or F9 directly): colour-blind
 *                    filter, crosshair, HDR / low latency (next start),
 *                    movie skip, intro skip, Discord, key bindings,
 *                    controller layout. Changes go to settings.ini + settings.bat.
 *   Key bindings     controls.txt rows (KEY = ACTION # Label); Enter = press a new
 *                    key, Esc cancels; cellPad re-reads the file at once.
 *   Controller       PAD_MAP_<button>=<button>, PAD_SOUTHPAW=1 (cellPad XInput hook).
 *   Movie skip       RFOM_CUTSCENE_SKIP (default on): hold Space / Enter / A / Menu
 *                    for 1 s while a story movie (.avi) plays. PORT_SKIP_HINT (or
 *                    RFOM_SKIP_HINT): "Hold Space / A to skip" toast; unset = auto (shown
 *                    while Skip unwatched movies is on), 0 = never, 1 = always.
 *                    RFOM_CUTSCENE_SKIP_ALL (default on; 0 = off): the hold also skips
 *                    a movie not watched yet (hooks the game's watched check).
 *   Intro skip       RFOM_SKIP_INTRO (default on): the opening intro movie, if the
 *                    game plays one, is skipped like the above without holding.
 *   Photo mode       RFOM_PHOTO (default on; 0 = off), F7: pause, hide the port's
 *                    overlays, F12 = shot (the game runs ~0.15 s for the capture).
 *   Crosshair        RFOM_CROSSHAIR=0|dot|cross: a small marker at the centre while
 *                    on foot.
 *   Pause on pad loss RFOM_PAD_DISCONNECT_PAUSE (default on).
 *   Discord          RFOM_DISCORD=1 (+ RFOM_DISCORD_APPID or discord_appid.txt): Rich
 *                    Presence over the local IPC pipe; silent if Discord isn't running.
 *   Benchmark        RFOM_BENCH=1: the front end follows RFOM_BENCH_SCRIPT into a
 *                    level; the level program measures RFOM_BENCH_SECONDS after
 *                    RFOM_BENCH_SETTLE and writes logs\bench\RFOM_bench_<date>.txt. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <vector>
#include <string>
#include <algorithm>
#include <mutex>
#include "ps3emu/port_pc_extras.h"   /* pcx_ui_px (PORT_UI_SCALE) */
#include "ps3emu/port_pc_extras2.h"  /* benchmark stats, crosshair colour / size */
#include "port_ini_migrate.h"         /* settings.ini key renames */
#pragma comment(lib, "xinput.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

#define PCX_PREFIX "RFOM"
#define PCX_GAME_NAME "Resistance: Fall of Man"
#define PCX_TAG "rfx"

extern "C" {
extern volatile int g_rfom_input_block;                      /* cellPad.c: the game sees neutral pads */
extern volatile unsigned long long g_pad_inj_until;          /* cellPad.c: injected presses on port 0 */
extern volatile uint32_t g_pad_inj_btn, g_pad_inj_sticks, g_pad_inj_clear;
void cellpad_reload_controls(void);                          /* cellPad.c */
extern void (*volatile g_port_xi_hook)(int port, void* gamepad);
extern void (*volatile g_vfs_override_hook)(char* path, size_t cap);   /* sys_fs.c: every path the game opens */
extern volatile unsigned long long g_rfom_play_ms;           /* cellPad.c / stubs.cpp: on-foot look code last ran */
uint32_t cellGcm_get_flip_mode(void);                        /* cellGcmSys.c: 2 (VSYNC) while a movie plays */
void rfpf_toast(const char* title, const char* text, char badge);
int rfpf_overlay_get(void);
void rfpf_overlay_set(int on);
void rsx_prof_enable(int on);
int rsx_prof_sample(double* fps, double* draws_per_frame, double* us_per_draw);
/* rsx_d3d12_backend.c */
extern volatile int g_pc_cvd_mode;                           /* 0 off, 1 protan, 2 deutan, 3 tritan */
extern volatile int g_pc_photo_nohud;                        /* backend: photo mode presents the HUD-less frame */
extern volatile int g_pc_hdr_active;
extern volatile int g_pc_reflex_active;
extern void (*volatile g_pc_ui_tick)(void* hwnd, int focused, int other_prompt);
extern int (*volatile g_pc_want_pause)(void);
extern void (*volatile g_pc_more_menu)(void);
}

/* ---- small helpers (same file formats as r2_pcport.cpp / the launcher) ---- */
static std::string ltrim(const std::string& s) { size_t i = 0; while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++; return s.substr(i); }
static bool starts_ci(const std::string& s, const std::string& p) { return s.size() >= p.size() && !_strnicmp(s.c_str(), p.c_str(), p.size()); }
static bool read_lines(const char* path, std::vector<std::string>& out)
{
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char line[1024];
    while (fgets(line, sizeof line, f)) { line[strcspn(line, "\r\n")] = 0; out.push_back(line); }
    fclose(f);
    return true;
}
static bool write_lines(const char* path, const std::vector<std::string>& out)
{
    char tmp[MAX_PATH]; snprintf(tmp, sizeof tmp, "%s.pcx.tmp", path);
    FILE* f = fopen(tmp, "w");
    if (!f) return false;
    for (auto& l : out) fprintf(f, "%s\n", l.c_str());
    const bool ok = fclose(f) == 0;
    if (!ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) { DeleteFileA(tmp); return false; }
    return true;
}
static bool file_set(const char* path, const std::string& prefix, const std::string& line, const char* header)
{
    std::vector<std::string> in, out;
    const bool had = read_lines(path, in);
    if (!had && line.empty()) return true;
    bool done = false;
    for (auto& l : in) {
        if (starts_ci(ltrim(l), prefix)) { if (!done && !line.empty()) out.push_back(line); done = true; continue; }
        out.push_back(l);
    }
    if (!done && !line.empty()) { if (!had && header) out.push_back(header); out.push_back(line); }
    return write_lines(path, out);
}
static void ini_set(const char* key, const std::string& v) { file_set("settings.ini", std::string(key) + "=", std::string(key) + "=" + v, NULL); }
static void bat_set(const char* env, const std::string& v)
{
    file_set("settings.bat", std::string("set \"") + env + "=", v.empty() ? std::string() : std::string("set \"") + env + "=" + v + "\"", "@echo off");
}
static std::string envs(const char* name, const char* def)
{
    char full[96]; snprintf(full, sizeof full, "%s_%s", PCX_PREFIX, name);
    const char* e = getenv(full); return (e && *e) ? e : def;
}
static int envi(const char* name, int def) { std::string v = envs(name, ""); return v.empty() ? def : atoi(v.c_str()); }
/* the "Hold Space / A to skip" hint when a movie starts. PORT_SKIP_HINT (or <prefix>_SKIP_HINT; launcher
   "Show cutscene skip hint") unset/empty = auto: shown while Hold to skip movies and Skip unwatched movies
   are both on (checked live), since every story movie can then be skipped. 0/off = never, 1/on = always. */
static int skip_hint_auto(void);
static int skip_hint_on(void)
{
    static int mode = -2;  /* -1 auto, 0 never, 1 always; only the explicit env value is cached */
    if (mode == -2) {
        const char* e = getenv("PORT_SKIP_HINT");
        const std::string v = (e && *e) ? std::string(e) : envs("SKIP_HINT", "");
        const char* s = v.c_str();
        mode = (v == "1" || _stricmp(s, "on") == 0 || _stricmp(s, "true") == 0) ? 1
             : (v == "0" || _stricmp(s, "off") == 0 || _stricmp(s, "false") == 0) ? 0 : -1;
    }
    return mode < 0 ? skip_hint_auto() : mode;
}
static double now_s(void)
{
    static LARGE_INTEGER f = {}; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)f.QuadPart;
}
static int host_focused(HWND game)
{
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId() && !IsIconic(game);
}
#define LOG(...) do { fprintf(stderr, "[" PCX_TAG "] " __VA_ARGS__); fputc('\n', stderr); } while (0)

/* ======================================================================
 * Controller layout (XInput hook in cellPad.c pad_poll_xinput) */
enum { XB_A, XB_B, XB_X, XB_Y, XB_LB, XB_RB, XB_LT, XB_RT, XB_LS, XB_RS, XB_BACK, XB_START, XB_COUNT };
static const char* s_xb_key[XB_COUNT] = { "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "LS", "RS", "BACK", "START" };
static const wchar_t* s_xb_name[XB_COUNT] = { L"A", L"B", L"X", L"Y", L"LB", L"RB", L"LT", L"RT", L"Left stick click",
                                              L"Right stick click", L"View", L"Menu" };
static const WORD s_xb_bit[XB_COUNT] = { XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
    XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER, 0, 0, XINPUT_GAMEPAD_LEFT_THUMB,
    XINPUT_GAMEPAD_RIGHT_THUMB, XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_START };
static volatile int s_xmap[XB_COUNT];   /* physical -> acts as */
static volatile int s_southpaw = 0, s_xmap_ident = 1;
static void xmap_load(void)
{
    s_xmap_ident = 1;
    for (int i = 0; i < XB_COUNT; i++) {
        char k[32]; snprintf(k, sizeof k, "PAD_MAP_%s", s_xb_key[i]);
        const char* e = getenv(k); s_xmap[i] = i;
        if (e && *e) for (int j = 0; j < XB_COUNT; j++) if (!_stricmp(e, s_xb_key[j])) s_xmap[i] = j;
        if (s_xmap[i] != i) s_xmap_ident = 0;
    }
    const char* sp = getenv("PAD_SOUTHPAW"); s_southpaw = sp && *sp && *sp != '0';
}
static void xi_hook(int port, void* gp_)
{
    (void)port;
    XINPUT_GAMEPAD* gp = (XINPUT_GAMEPAD*)gp_;
    if (s_southpaw) {
        std::swap(gp->sThumbLX, gp->sThumbRX);
        std::swap(gp->sThumbLY, gp->sThumbRY);
    }
    if (s_xmap_ident) return;
    int down[XB_COUNT];
    for (int i = 0; i < XB_COUNT; i++) down[i] = s_xb_bit[i] ? (gp->wButtons & s_xb_bit[i]) != 0 : 0;
    down[XB_LT] = gp->bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    down[XB_RT] = gp->bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    const BYTE lt = gp->bLeftTrigger, rt = gp->bRightTrigger;
    WORD b = gp->wButtons;
    for (int i = 0; i < XB_COUNT; i++) b &= ~s_xb_bit[i];
    BYTE olt = 0, ort = 0;
    for (int i = 0; i < XB_COUNT; i++) {
        if (!down[i]) continue;
        const int t = s_xmap[i];
        const BYTE analog = i == XB_LT ? lt : i == XB_RT ? rt : 255;   /* trigger to trigger keeps its travel */
        if (t == XB_LT) olt = std::max(olt, analog);
        else if (t == XB_RT) ort = std::max(ort, analog);
        else b |= s_xb_bit[t];
    }
    gp->wButtons = b; gp->bLeftTrigger = olt; gp->bRightTrigger = ort;
}

/* ======================================================================
 * Movies (skip) and levels (Discord, benchmark) from the file-path hook.
 * A movie counts as playing from its .avi being opened until the flip mode
 * leaves 2 (VSYNC, set for movie playback). */
static volatile ULONGLONG s_movie_t0 = 0;
static volatile int s_movie_intro = 0, s_movie_on = 0, s_movie_seen2 = 0;
/* Skip unwatched movies (RFOM_CUTSCENE_SKIP_ALL): the game's own skip (func_000320F8 front end /
 * func_0002FDB8 levels, hooked in tools/hooks) only lets a button end a story movie whose bit is set in
 * the profile's watched-movie bitfield (profile + 0x2350, the same bits isMovieUnlocked reads). Once the
 * 1 s hold fires, s_skip_armed holds that movie's s_movie_t0 and rfom_movie_skip_unseen() reports the
 * movie as watched, so the hold's Start / Cross pulses end it through the game's normal skip path. */
static volatile ULONGLONG s_skip_armed = 0, s_skip_arm_ms = 0, s_skip_used = 0;
static std::mutex s_lvl_mu;
static std::string s_level;                /* "" = not known yet */
static volatile int s_level_gen = 0;
static void (*s_prev_vfs_hook)(char* path, size_t cap) = NULL;
static void vfs_hook(char* path, size_t cap)
{
    std::string p(path ? path : "");
    if (s_prev_vfs_hook) s_prev_vfs_hook(path, cap);   /* mods (rfom_mods.cpp) */
    for (auto& c : p) { if (c == '\\') c = '/'; c = (char)tolower((unsigned char)c); }
    if (p.size() > 4 && p.compare(p.size() - 4, 4, ".avi") == 0 && p.find("/movies/") != std::string::npos) {
        if (p.find("_attr") != std::string::npos) return;          /* the title screen's attract loop */
        s_movie_intro = p.find("_intro") != std::string::npos;
        s_movie_seen2 = 0; s_movie_t0 = GetTickCount64(); s_movie_on = 1;
        return;
    }
    const size_t k = p.find("/levels/level");
    if (k == std::string::npos) return;
    size_t e = k + 13; while (e < p.size() && isdigit((unsigned char)p[e])) e++;
    if (e == k + 13) return;
    std::string lv = p.substr(k + 8, e - (k + 8));
    std::lock_guard<std::mutex> g(s_lvl_mu);
    if (lv != s_level) { s_level = lv; s_level_gen++; LOG("level: %s", lv.c_str()); }
}
static void movie_tick(void)
{
    if (!s_movie_on) return;
    const uint32_t fm = cellGcm_get_flip_mode();
    /* a slow load (shader compile, first-run cache) can take more than 4 s to start the movie, which then played
     * with no skip (and no intro auto-skip). RFOM_MOVIE_START_MS=4000: the old window. */
    static ULONGLONG start_ms = 0;
    if (!start_ms) { const char* e = getenv("RFOM_MOVIE_START_MS"); const long long v = (e && *e) ? atoll(e) : 15000;
                     start_ms = v < 500 ? 500 : v > 120000 ? 120000 : (ULONGLONG)v; }
    if (fm == 2) s_movie_seen2 = 1;
    else if (s_movie_seen2 || GetTickCount64() - s_movie_t0 > start_ms) s_movie_on = 0;   /* ended (or never started) */
    const ULONGLONG t0 = s_movie_t0;
    if (!t0 || s_skip_armed != t0) return;
    const ULONGLONG after = GetTickCount64() - s_skip_arm_ms;
    if (!s_movie_on) {
        s_skip_armed = 0;
        LOG("movie skip: movie ended %llu ms after the hold (%s)", (unsigned long long)after,
            s_skip_used == t0 ? "unwatched movie: game skip check overridden, ended by the game's own skip path"
                              : "game's own skip path");
    } else if (after > 8000) {
        static ULONGLONG warned = 0;
        if (warned != t0) {
            warned = t0;
            LOG("movie skip: still playing 8 s after the hold (%s)",
                s_skip_used == t0 ? "unwatched override used" : "the game's skip check was not reached for this movie");
        }
    }
}
static int s_cut_skip = 1, s_cut_skip_all = 1;
/* hooked into the game's movie skip check: seen = the watched bit the game read for movie id */
/* PORT_SKIP_TEST_UNSEEN=1 (test only, default off): every movie answers "not watched" to the skip check,
 * so the override can be tested on a profile that has seen them all. Only the check's answer changes;
 * the game still sets the watched bits itself. */
static int skip_test_unseen(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("PORT_SKIP_TEST_UNSEEN"); v = (e && *e && atoi(e) != 0) ? 1 : 0; }
    return v;
}
extern "C" uint32_t rfom_movie_skip_unseen(uint32_t seen, uint32_t id)
{
    const ULONGLONG t0 = s_movie_t0;
    const int armed = t0 && s_movie_on && s_skip_armed == t0 && s_cut_skip && s_cut_skip_all;
    if (seen & 0xFF) {
        if (!skip_test_unseen() || armed) return seen;
        static ULONGLONG forced = 1;
        if (forced != t0) { forced = t0; LOG("movie: PORT_SKIP_TEST_UNSEEN: movie %u is watched, forced to unwatched for the skip check", id); }
        seen = 0;
    }
    static ULONGLONG noted = 1;
    if (noted != t0) {
        noted = t0;
        LOG("movie: game skip check: movie %u not watched yet%s", id,
            !s_cut_skip ? "" : s_cut_skip_all ? "; a 1 s hold skips it (Skip unwatched movies)" : "; not skippable (Skip unwatched movies off)");
    }
    if (!armed) return seen;
    if (s_skip_used != t0) { s_skip_used = t0; LOG("movie skip: unwatched movie %u: game skip check overridden after the hold", id); }
    return 1;
}
static int s_is_level_exe = 0;             /* ResistanceFoMGame.exe (the levels); else the front end */
static std::string level_pretty(void)
{
    std::string lv; { std::lock_guard<std::mutex> g(s_lvl_mu); lv = s_level; }
    if (!s_is_level_exe) return "In the menus";
    if (lv.empty()) return "Playing the campaign";
    return "Playing (" + lv + ")";
}

/* ======================================================================
 * Discord Rich Presence over the local IPC pipe */
static volatile int s_discord_quit = 0;
static bool pipe_send(HANDLE h, uint32_t op, const std::string& json)
{
    std::string buf(8 + json.size(), 0);
    const uint32_t len = (uint32_t)json.size();
    memcpy(&buf[0], &op, 4); memcpy(&buf[4], &len, 4); memcpy(&buf[8], json.data(), json.size());
    DWORD w = 0;
    return WriteFile(h, buf.data(), (DWORD)buf.size(), &w, NULL) && w == buf.size();
}
static void pipe_drain(HANDLE h)
{
    DWORD avail = 0; char tmp[4096];
    while (PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL) && avail) { DWORD r = 0; if (!ReadFile(h, tmp, std::min<DWORD>(avail, sizeof tmp), &r, NULL) || !r) break; }
}
static std::string json_esc(const std::string& s)
{
    std::string o; for (char c : s) { if (c == '"' || c == '\\') o += '\\'; if ((unsigned char)c >= 0x20) o += c; } return o;
}
static DWORD WINAPI discord_thread(LPVOID)
{
    std::string appid = envs("DISCORD_APPID", "");
    if (appid.empty()) {                         /* discord_appid.txt in the game folder (the launcher can't hold text) */
        std::vector<std::string> l;
        if (read_lines("discord_appid.txt", l))
            for (auto& s : l) { std::string t = ltrim(s); while (!t.empty() && (unsigned char)t.back() <= ' ') t.pop_back();
                                if (!t.empty() && isdigit((unsigned char)t[0])) { appid = t; break; } }
    }
    if (appid.empty()) { LOG("Discord: no application id (set " PCX_PREFIX "_DISCORD_APPID or discord_appid.txt); Rich Presence off"); return 0; }
    const long long start = (long long)time(NULL);
    HANDLE h = INVALID_HANDLE_VALUE; int sent_gen = -1; ULONGLONG retry = 0, next_send = 0; int nonce = 0;
    while (!s_discord_quit) {
        const ULONGLONG now = GetTickCount64();
        if (h == INVALID_HANDLE_VALUE) {
            if (now < retry) { Sleep(500); continue; }
            retry = now + 60000;
            for (int i = 0; i < 10 && h == INVALID_HANDLE_VALUE; i++) {
                char name[64]; snprintf(name, sizeof name, "\\\\.\\pipe\\discord-ipc-%d", i);
                h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
            }
            if (h == INVALID_HANDLE_VALUE) continue;   /* Discord not running: try again in a minute */
            if (!pipe_send(h, 0, "{\"v\":1,\"client_id\":\"" + json_esc(appid) + "\"}")) { CloseHandle(h); h = INVALID_HANDLE_VALUE; continue; }
            LOG("Discord: connected");
            sent_gen = -1;
        }
        pipe_drain(h);
        if (sent_gen != s_level_gen || now >= next_send) {
            sent_gen = s_level_gen; next_send = now + 15000 * 4;
            char js[1024];
            snprintf(js, sizeof js,
                "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu,\"activity\":{\"details\":\"%s\",\"state\":\"%s\","
                "\"timestamps\":{\"start\":%lld},\"assets\":{\"large_image\":\"logo\",\"large_text\":\"%s\"}}},\"nonce\":\"%d\"}",
                GetCurrentProcessId(), PCX_GAME_NAME, json_esc(level_pretty()).c_str(), start, PCX_GAME_NAME, ++nonce);
            if (!pipe_send(h, 1, js)) { CloseHandle(h); h = INVALID_HANDLE_VALUE; retry = now + 60000; continue; }
        }
        Sleep(1000);
    }
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    return 0;
}

/* ======================================================================
 * Benchmark. The front end (ResistanceFoM.exe) presses through the menus on a
 * timeline (RFOM_BENCH_SCRIPT); the level program (ResistanceFoMGame.exe) waits
 * RFOM_BENCH_SETTLE seconds after its first frame, then records frame times
 * for RFOM_BENCH_SECONDS, writes the report and closes. */
struct BenchStep { double at; uint32_t btn; };
static std::vector<BenchStep> s_bench_steps;
static int s_bench = 0, s_bench_step = 0, s_bench_state = 0;   /* 0 script / settle, 1 measuring, 2 done */
static double s_bench_t0 = 0, s_bench_m0 = 0, s_bench_secs = 60, s_bench_settle = 45;
static double s_last_frame = 0, s_first_frame = 0;
static double s_bench_dpf_sum = 0, s_bench_upd_sum = 0; static int s_bench_prof_n = 0;
static uint32_t btn_named(const std::string& n)
{
    static const struct { const char* n; uint32_t b; } t[] = { {"SELECT",1},{"L3",2},{"R3",4},{"START",8},{"UP",0x10},{"RIGHT",0x20},
        {"DOWN",0x40},{"LEFT",0x80},{"L2",0x100},{"R2",0x200},{"L1",0x400},{"R1",0x800},{"TRIANGLE",0x1000},{"CIRCLE",0x2000},
        {"CROSS",0x4000},{"SQUARE",0x8000} };
    for (auto& e : t) if (!_stricmp(n.c_str(), e.n)) return e.b;
    return 0;
}
/* "wait:BUTTON,wait:BUTTON,..." -- seconds after the previous step, timed from
 * the front end's start. Default: title screen, then Cross through the main
 * menu (continue the campaign). "none" = navigate yourself; the measurement
 * still starts in the level. */
static void bench_parse(void)
{
    std::string sc = envs("BENCH_SCRIPT", "120:START,8:CROSS,8:CROSS,8:CROSS,8:CROSS");
    if (!_stricmp(sc.c_str(), "none")) return;
    double t = 0; size_t i = 0;
    while (i < sc.size()) {
        size_t e = sc.find(',', i); if (e == std::string::npos) e = sc.size();
        std::string tok = sc.substr(i, e - i); i = e + 1;
        const size_t c = tok.find(':');
        t += atof(tok.c_str());
        s_bench_steps.push_back({ t, c == std::string::npos ? 0u : btn_named(tok.substr(c + 1)) });
    }
}
/* Frame-time capture, shared by the scripted benchmark (RFOM_BENCH=1) and the hotkey capture (F11,
 * RFOM_BENCH_KEY): the game thread (rfx_frame_tick) appends to a preallocated buffer, the UI thread
 * starts / stops it and writes the report. */
#define BENCH_CAP_MAX 400000u                    /* ~28 min at 240 fps */
static float* s_cap_buf = NULL;
static volatile LONG s_cap_n = 0, s_cap_on = 0;
static double s_cap_t0 = 0;
static int s_cap_kind = 0;                       /* 1 scripted benchmark, 2 hotkey capture */
static int s_cap_key = 0x7A;                     /* VK_F11; 0 = off */
static int cap_start(int kind)
{
    if (!s_cap_buf) s_cap_buf = (float*)malloc(BENCH_CAP_MAX * sizeof(float));
    if (!s_cap_buf) return 0;
    s_cap_kind = kind; s_cap_t0 = now_s();
    InterlockedExchange(&s_cap_n, 0);
    InterlockedExchange(&s_cap_on, 1);
    return 1;
}
/* logs\bench\<P>_bench_<date>.txt (summary, read by the launcher), .csv (every frame) and a row in bench_history.csv */
static std::string bench_report(const float* ft, unsigned n, double secs, int kind)
{
    const double dpf = kind == 1 && s_bench_prof_n ? s_bench_dpf_sum / s_bench_prof_n : 0;
    const double us_draw = kind == 1 && s_bench_prof_n ? s_bench_upd_sum / s_bench_prof_n : 0;
    std::vector<float> scratch(n ? n : 1);
    PcxBenchStats st; pcx_bench_stats(ft, n, secs, scratch.data(), &st);
    CreateDirectoryA("logs", NULL); CreateDirectoryA("logs\\bench", NULL);
    SYSTEMTIME t; GetLocalTime(&t);
    char base[MAX_PATH]; snprintf(base, sizeof base, "logs\\bench\\%s_bench_%04u%02u%02u_%02u%02u%02u", PCX_PREFIX,
                                  t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    const std::string txt = std::string(base) + ".txt", csv = std::string(base) + ".csv";
    if (FILE* f = fopen(txt.c_str(), "w")) {
        fprintf(f, "%s built-in benchmark\n", PCX_GAME_NAME);
        fprintf(f, "mode             %s\n", kind == 1 ? "scripted" : "hotkey capture");
        fprintf(f, "level            %s\n", level_pretty().c_str());
        fprintf(f, "seconds          %.1f\n", st.secs);
        fprintf(f, "frames           %u\n", st.frames);
        fprintf(f, "avg fps          %.2f\n", st.avg_fps);
        fprintf(f, "1%% low fps       %.2f\n", st.low1_fps);
        fprintf(f, "0.1%% low fps     %.2f\n", st.low01_fps);
        fprintf(f, "stutters         %u\n", st.stutters);
        fprintf(f, "lows             average of the slowest 1%% / 0.1%% frame times (CapFrameX style)\n");
        fprintf(f, "stutter rule     frame > 2x the average of the previous 30 and >= 4 ms over it\n");
        fprintf(f, "median ms        %.3f\n", st.median_ms);
        fprintf(f, "p99 ms           %.3f\n", st.p99_ms);
        fprintf(f, "max ms           %.3f\n", st.max_ms);
        if (kind == 1) {
            fprintf(f, "draws/frame      %.1f\n", dpf);
            fprintf(f, "us/draw          %.3f\n", us_draw);
        }
        fprintf(f, "frame cap        %s\n", getenv(PCX_PREFIX "_FPS") ? getenv(PCX_PREFIX "_FPS") : "default");
        fprintf(f, "frame times      %s\n", csv.c_str());
        fclose(f);
    }
    if (FILE* f = fopen(csv.c_str(), "w")) {
        fprintf(f, "frame,t_ms,frame_ms\n");
        double at = 0;
        for (unsigned i = 0; i < n; i++) { at += ft[i]; fprintf(f, "%u,%.3f,%.3f\n", i, at, ft[i]); }
        fclose(f);
    }
    const bool hdr = GetFileAttributesA("logs\\bench\\bench_history.csv") == INVALID_FILE_ATTRIBUTES;
    if (FILE* h = fopen("logs\\bench\\bench_history.csv", "a")) {
        if (hdr) fprintf(h, "date,game,level,seconds,frames,avg_fps,low1_fps,low01_fps,draws_per_frame,us_per_draw\n");
        fprintf(h, "%04u-%02u-%02u %02u:%02u:%02u,%s,%s,%.1f,%u,%.2f,%.2f,%.2f,%.1f,%.3f\n", t.wYear, t.wMonth, t.wDay, t.wHour,
                t.wMinute, t.wSecond, PCX_PREFIX, level_pretty().c_str(), st.secs, st.frames, st.avg_fps, st.low1_fps, st.low01_fps, dpf, us_draw);
        fclose(h);
    }
    LOG("benchmark (%s): avg %.2f fps, 1%% low %.2f, 0.1%% low %.2f, %u stutters, %.3f us/draw -> %s",
        kind == 1 ? "scripted" : "hotkey", st.avg_fps, st.low1_fps, st.low01_fps, st.stutters, us_draw, txt.c_str());
    char s[200]; snprintf(s, sizeof s, "%.1f fps avg, %.1f 1%% low, %.1f 0.1%% low, %u stutters (%u frames, %.0f s)",
                          st.avg_fps, st.low1_fps, st.low01_fps, st.stutters, st.frames, st.secs);
    return s;
}
static std::string cap_stop_report(void)
{
    InterlockedExchange(&s_cap_on, 0);
    const unsigned n = (unsigned)InterlockedCompareExchange(&s_cap_n, 0, 0);
    return bench_report(s_cap_buf, n, now_s() - s_cap_t0, s_cap_kind);
}
static void bench_tick(HWND game)
{
    if (!s_bench || s_bench_state == 2) return;
    if (!s_is_level_exe) {                   /* front end: the menu script only */
        const double t = now_s() - s_bench_t0;
        while (s_bench_step < (int)s_bench_steps.size() && t >= s_bench_steps[s_bench_step].at) {
            const BenchStep& st = s_bench_steps[s_bench_step++];
            if (st.btn) { g_pad_inj_sticks = 0x80808080u; g_pad_inj_clear = 0; g_pad_inj_btn = st.btn; g_pad_inj_until = GetTickCount64() + 150; }
            LOG("benchmark: step %d at %.0f s", s_bench_step, t);
        }
        return;
    }
    if (s_bench_state == 0) {
        if (s_first_frame <= 0 || now_s() - s_first_frame < s_bench_settle || s_movie_on) return;
        if (s_cap_on) cap_stop_report();     /* a hotkey capture still running is saved first */
        s_bench_state = 1; s_bench_m0 = now_s(); cap_start(1);
        s_bench_dpf_sum = s_bench_upd_sum = 0; s_bench_prof_n = 0;
        rsx_prof_enable(1);
        LOG("benchmark: measuring for %.0f s", s_bench_secs);
        return;
    }
    { static double next = 0; const double n = now_s();   /* the profiler publishes a 0.5 s window */
      if (n >= next) { next = n + 0.5; double fps = 0, dpf = 0, upd = 0;
                       if (n - s_bench_m0 > 1.0 && rsx_prof_sample(&fps, &dpf, &upd)) { s_bench_dpf_sum += dpf; s_bench_upd_sum += upd; s_bench_prof_n++; } } }
    if (now_s() - s_bench_m0 >= s_bench_secs) {
        s_bench_state = 2;
        cap_stop_report();
        if (!rfpf_overlay_get()) rsx_prof_enable(0);
        if (envi("BENCH_QUIT", 1) && game) PostMessageA(game, WM_CLOSE, 0, 0);
    }
}
/* the capture hotkey: press to start, press again to stop and save (also stops when the buffer is full) */
static void bench_key_tick(int focused, int other_prompt)
{
    static int was = 0;
    const int d = s_cap_key && focused && (GetAsyncKeyState(s_cap_key) & 0x8000) != 0;
    char key[8]; snprintf(key, sizeof key, "F%d", s_cap_key - 0x6F);
    if (d && !was && s_bench_state != 1 && !other_prompt) {
        if (!s_cap_on) {
            if (cap_start(2)) {
                char m[96]; snprintf(m, sizeof m, "Recording frame times. Press %s again to stop.", key);
                rfpf_toast("Benchmark", m, 0); LOG("benchmark: hotkey capture started");
            }
        } else rfpf_toast("Benchmark saved (logs\\bench)", cap_stop_report().c_str(), 0);
    } else if (s_cap_on && s_cap_kind == 2 && (unsigned)s_cap_n >= BENCH_CAP_MAX)
        rfpf_toast("Benchmark saved (buffer full)", cap_stop_report().c_str(), 0);
    was = d;
}
/* stubs.cpp rfom_frame_wait: once per game frame */
extern "C" void rfx_frame_tick(void)
{
    const double t = now_s();
    if (s_first_frame <= 0) s_first_frame = t;
    if (s_cap_on && s_last_frame > 0) {
        const LONG i = s_cap_n;
        if ((unsigned)i < BENCH_CAP_MAX) { s_cap_buf[i] = (float)((t - s_last_frame) * 1000.0); InterlockedExchange(&s_cap_n, i + 1); }
    }
    s_last_frame = t;
}

/* ======================================================================
 * The extras menu */
struct XChoice { std::wstring label; std::string value; };
struct XOpt {
    const wchar_t* title; const wchar_t* help; const char* ini; const char* env; int restart;
    std::vector<XChoice> ch; int sel, run;
    void (*live)(const std::string& v);
};
static std::vector<XOpt> s_opts;
static int s_photo_on = 0;
static int s_crosshair = 0;             /* 0 off, 1 dot, 2 cross */
static int s_disc_pause_on = 1, s_photo_allowed = 1, s_skip_intro = 1;
static void live_cvd(const std::string& v) { g_pc_cvd_mode = atoi(v.c_str()); }
extern "C" void pc_set_gamma(float g);   /* rsx_d3d12_backend.c: PS3_DISPLAY_GAMMA, live */
static void live_gamma(const std::string& v) { pc_set_gamma(v.empty() ? 0.0f : (float)atof(v.c_str())); }
static int crosshair_parse(const std::string& v) { return v == "dot" || v == "1" ? 1 : v == "cross" || v == "2" ? 2 : 0; }
static void live_xhair(const std::string& v) { s_crosshair = crosshair_parse(v); }
static unsigned s_xh_rgb = 0xF0F0F0; static int s_xh_scale = 100;
static void live_xhcolor(const std::string& v) { s_xh_rgb = pcx_xhair_rgb(v.c_str()); }
static void live_xhsize(const std::string& v) { s_xh_scale = pcx_xhair_scale(v.c_str()); }
static void live_benchkey(const std::string& v) { s_cap_key = pcx_bench_key(v.c_str()); }
static void live_dpause(const std::string& v) { s_disc_pause_on = v != "0"; }
static void live_cut(const std::string& v) { s_cut_skip = v != "0"; }
static void live_cut_all(const std::string& v) { s_cut_skip_all = v != "0"; }
static int skip_hint_auto(void) { return s_cut_skip && s_cut_skip_all; }
static void live_intro(const std::string& v) { s_skip_intro = v != "0"; }
extern "C" void cellpad_set_pad_opt(const char* key, const char* v);   /* cellPad.c (SDL3 backend) */
static void live_gyro(const std::string& v) { cellpad_set_pad_opt("gyro", v.c_str()); }
static void live_gyrosens(const std::string& v) { cellpad_set_pad_opt("gyrosens", v.c_str()); }
static void live_gyroinvy(const std::string& v) { cellpad_set_pad_opt("gyroinvy", v.c_str()); }
static void live_gyrospace(const std::string& v) { cellpad_set_pad_opt("gyrospace", v.c_str()); }
static void live_lightbar(const std::string& v) { cellpad_set_pad_opt("lightbar", v.c_str()); }
static void live_nswap(const std::string& v) { cellpad_set_pad_opt("nswap", v.c_str()); }
/* PC extras (ps3emu/port_pc_extras.h): cellPad.c takes the env names as keys */
static void live_rumble(const std::string& v) { cellpad_set_pad_opt("PAD_RUMBLE", v.c_str()); }
static void live_padbg(const std::string& v) { cellpad_set_pad_opt("PAD_BACKGROUND_INPUT", v.c_str()); }
static void live_outerdz(const std::string& v) { cellpad_set_pad_opt("PAD_OUTER_DEADZONE", v.c_str()); }
static void live_antidz(const std::string& v) { cellpad_set_pad_opt("PAD_ANTI_DEADZONE", v.c_str()); }
static void live_trigdz(const std::string& v) { cellpad_set_pad_opt("PAD_TRIGGER_DEADZONE", v.c_str()); }
static void live_turnboost(const std::string& v) { cellpad_set_pad_opt("PAD_TURN_BOOST", v.c_str()); }
static void live_crouch(const std::string& v) { cellpad_set_pad_opt("PAD_CROUCH_MODE", v.c_str()); }
extern "C" void rfom_aim_assist_set(const char* v);   /* stubs.cpp (issue #26) */
extern "C" void rfom_aim_assist_init(void);
static void live_aimassist(const std::string& v) { rfom_aim_assist_set(v.c_str()); }
extern "C" void cellaudio_set_pc_mix(int volume_pct, int mono);   /* cellAudio.c; -1 = unchanged */
static void live_volume(const std::string& v) { cellaudio_set_pc_mix(v.empty() ? 100 : atoi(v.c_str()), -1); }
static void live_mono(const std::string& v) { cellaudio_set_pc_mix(-1, v == "1" ? 1 : 0); }
extern "C" void pc_ui_scale_set(const char* v);   /* rsx_pc_menu.inc: the PC menus */
extern "C" int pc_ui_scale_menu(void);
static void live_uiscale(const std::string& v) { pc_ui_scale_set(v.c_str()); }
extern "C" void rfpf_overlay_graph_set(int on);   /* rfom_pcfeat.cpp */
static void live_ovgraph(const std::string& v) { rfpf_overlay_graph_set(v != "0"); }
extern "C" void cellpad_set_glyph_pref(const char* v);
static void opt_add(const wchar_t* title, const wchar_t* help, const char* ini, const char* env, int restart,
                    std::vector<XChoice> ch, const char* def, void (*live)(const std::string&))
{
    XOpt o{ title, help, ini, env, restart, std::move(ch), 0, 0, live };
    const char* e = getenv(env); std::string cur = (e && *e) ? e : def;
    o.run = 0;
    for (size_t k = 0; k < o.ch.size(); k++) if (!_stricmp(o.ch[k].value.c_str(), cur.c_str())) o.run = (int)k;
    o.sel = o.run;
    s_opts.push_back(std::move(o));
}
#define ENVN(x) PCX_PREFIX "_" x
static void opts_init(void)
{
    if (!s_opts.empty()) return;
    {   /* renamed settings.ini keys: copy old -> new once (the old line stays, see port_ini_migrate.h) */
        std::vector<std::string> l;
        if (read_lines("settings.ini", l) && port_ini_migrate_lines(l) > 0) write_lines("settings.ini", l);
    }
    /* values match launcher_extra.inc exactly ("" = default: no settings.bat line), or the launcher drops them */
    const std::vector<XChoice> onoff = { {L"On", ""}, {L"Off", "0"} };
    opt_add(L"Colour-blind filter", L"Daltonization filter on the final image.", "colorblind", ENVN("COLORBLIND"), 0,
            { {L"Off", ""}, {L"Protanopia (red)", "1"}, {L"Deuteranopia (green)", "2"}, {L"Tritanopia (blue)", "3"} }, "", live_cvd);
    opt_add(L"Brightness", L"Picture gamma: lower is brighter, higher darker (SDR; in HDR use HDR paper white).",
            "gamma", "PS3_DISPLAY_GAMMA", 0,
            { {L"1.0 (as the game)", ""}, {L"0.8 (brighter)", "0.8"}, {L"0.9 (brighter)", "0.9"}, {L"1.1 (darker)", "1.1"},
              {L"1.2 (darker)", "1.2"} }, "", live_gamma);
    opt_add(L"Crosshair", L"A small extra marker at the screen centre while on foot.", "crosshair", ENVN("CROSSHAIR"), 0,
            { {L"Off", ""}, {L"Dot", "dot"}, {L"Cross", "cross"} }, "", live_xhair);
    opt_add(L"Crosshair colour", L"Colour of the extra crosshair (high-contrast choices; a dark outline is always drawn).", "xhair_color", ENVN("CROSSHAIR_COLOR"), 0,
            { {L"White", ""}, {L"Green", "green"}, {L"Yellow", "yellow"}, {L"Cyan", "cyan"}, {L"Red", "red"}, {L"Pink", "pink"}, {L"Orange", "orange"} }, "", live_xhcolor);
    opt_add(L"Crosshair size", L"Size of the extra crosshair.", "xhair_size", ENVN("CROSSHAIR_SIZE"), 0,
            { {L"Small", ""}, {L"Medium", "medium"}, {L"Large", "large"}, {L"Huge", "huge"} }, "", live_xhsize);    opt_add(L"Hold to skip movies", L"Hold Space / Enter / A / Menu for 1 s to skip a story movie.", "cutscene_skip",
            ENVN("CUTSCENE_SKIP"), 0, onoff, "", live_cut);
    opt_add(L"Skip unwatched movies", L"Hold to skip movies also skips a story movie you have not watched yet (the game "
            L"normally refuses). Needs Hold to skip movies.", "cutscene_skip_all", ENVN("CUTSCENE_SKIP_ALL"), 0, onoff, "", live_cut_all);
    opt_add(L"Skip intro movies", L"Skips the opening intro movie, if the game plays it.", "skipintro", ENVN("SKIP_INTRO"), 0,
            onoff, "", live_intro);
    opt_add(L"Pause on controller loss", L"Pauses when the controller in use disconnects.", "pad_disconnect_pause",
            ENVN("PAD_DISCONNECT_PAUSE"), 0, onoff, "", live_dpause);
    opt_add(L"HDR output", L"Experimental. Needs Windows HDR on (SDR otherwise). Auto: HDR10 only when Windows HDR is on.",
            "hdr", ENVN("HDR"), 1,
            { {L"Off", ""}, {L"Auto", "auto"}, {L"On (HDR10)", "1"}, {L"On (scRGB)", "scrgb"} }, "", NULL);
    opt_add(L"HDR paper white", L"Brightness of SDR white / the HUD in nits. Only highlights go above it.", "hdr_paper",
            ENVN("HDR_PAPER"), 1,
            { {L"203 nits", ""}, {L"100 nits", "100"}, {L"160 nits", "160"}, {L"250 nits", "250"}, {L"300 nits", "300"},
              {L"400 nits", "400"} }, "", NULL);
    opt_add(L"HDR peak", L"Brightest highlights. Display: the peak Windows reports. Same as paper white = no expansion.",
            "hdr_peak", ENVN("HDR_PEAK"), 1,
            { {L"Display", ""}, {L"400 nits", "400"}, {L"600 nits", "600"}, {L"800 nits", "800"}, {L"1000 nits", "1000"},
              {L"1500 nits", "1500"}, {L"2000 nits", "2000"} }, "", NULL);
    opt_add(L"HDR saturation", L"Colour saturation in HDR (1 = as the game).", "hdr_sat", ENVN("HDR_SAT"), 1,
            { {L"1.0", ""}, {L"0.9", "0.9"}, {L"1.1", "1.1"}, {L"1.2", "1.2"} }, "", NULL);
    opt_add(L"Low latency", L"NVIDIA Reflex (NVIDIA GPUs); a one-frame-queue swap chain on other GPUs. Takes effect on restart.", "reflex",
            ENVN("REFLEX"), 1, { {L"Off", ""}, {L"On", "1"}, {L"On + Boost", "2"} }, "", NULL);
    opt_add(L"Latency reduction", L"NVIDIA Reflex (GeForce), AMD Anti-Lag 2 (Radeon RX 5000+) or Auto (Reflex, else Anti-Lag 2). "
            L"Default: the Low latency row alone decides. Not with frame generation. Takes effect on restart.", "latency", "PORT_LATENCY", 1,
            { {L"Default", ""}, {L"Off", "off"}, {L"NVIDIA Reflex", "reflex"}, {L"AMD Anti-Lag 2", "antilag"}, {L"Auto", "auto"} }, "", NULL);
    opt_add(L"Frame Generation", L"DLSS: needs Anti-aliasing = NVIDIA DLSS and an RTX 40/50 (3x / 4x: RTX 50). FSR 3 2x: any GPU. "
            L"Takes effect on restart.", "dlss_fg", ENVN("DLSS_FG"), 1,
            { {L"Off", ""}, {L"DLSS 2x", "2"}, {L"DLSS 3x", "3"}, {L"DLSS 4x", "4"}, {L"FSR 3 2x", "fsr3"} }, "", NULL);
    opt_add(L"Frame generation backend", L"Auto: DLSS Frame Generation, else FSR 3 2x. DLSS only: no fallback. Takes effect on restart.",
            "fg_backend", ENVN("FG_BACKEND"), 1,
            { {L"Auto", ""}, {L"DLSS Frame Generation only", "dlss"}, {L"AMD FSR 3", "fsr3"} }, "", NULL);
    opt_add(L"Game language", L"The language the game uses (text and speech it has). Takes effect on restart.",
            "language", "PS3_LANG", 1,
            { {L"Windows language", ""}, {L"English", "en"}, {L"English (UK)", "en-gb"}, {L"French", "fr"}, {L"German", "de"},
              {L"Italian", "it"}, {L"Spanish", "es"}, {L"Dutch", "nl"}, {L"Portuguese", "pt"}, {L"Russian", "ru"},
              {L"Polish", "pl"}, {L"Finnish", "fi"}, {L"Swedish", "sv"}, {L"Danish", "da"}, {L"Norwegian", "no"},
              {L"Japanese", "ja"}, {L"Korean", "ko"} }, "", NULL);
    opt_add(L"Discord Rich Presence", L"Shows the game in Discord (needs discord_appid.txt or " ENVN("DISCORD_APPID") L").",
            "discord", ENVN("DISCORD"), 1, { {L"Off", ""}, {L"On", "1"} }, "", NULL);
    opt_add(L"Benchmark capture key", L"Press to start recording frame times, again to stop: logs\\bench gets a summary (avg, 1% / 0.1% low, stutters) and a per-frame CSV.",
            "benchkey", ENVN("BENCH_KEY"), 0, { {L"F11", ""}, {L"F6", "F6"}, {L"F5", "F5"}, {L"Off", "off"} }, "", live_benchkey);
    opt_add(L"Remember window position", L"Windowed mode: reopen where the window was last left (window_pos.txt).", "winremember",
            "RSX_WINDOW_REMEMBER", 1, { {L"On", ""}, {L"Off", "0"} }, "", NULL);    /* controllers (SDL3 backend, cellPad.c); keys and values match the launcher's Controls page */
    opt_add(L"Controller support", L"SDL3: DualShock / DualSense / Switch / Xbox directly (prompts, rumble, light bar, gyro). "
            L"XInput: the old path.", "padbackend", "PAD_BACKEND", 1, { {L"SDL3", ""}, {L"XInput", "xinput"} }, "", NULL);
    opt_add(L"Gyro aiming", L"Turn the view by turning a DualShock 4 / DualSense / Switch controller. While zoomed: only "
            L"while zoomed in.", "gyro", "PAD_GYRO", 0, { {L"Off", ""}, {L"On", "1"}, {L"While zoomed", "aim"} }, "", live_gyro);
    opt_add(L"Gyro sensitivity", L"View degrees per controller degree.", "gyrosens", "PAD_GYRO_SENS", 0,
            { {L"Default (2x)", ""}, {L"0.5x", "0.5"}, {L"1x", "1"}, {L"1.5x", "1.5"}, {L"3x", "3"}, {L"4x", "4"}, {L"6x", "6"} },
            "", live_gyrosens);
    opt_add(L"Gyro vertical", L"Invert the up / down gyro.", "gyroinvy", "PAD_GYRO_INVERT_Y", 0,
            { {L"Normal", ""}, {L"Inverted", "1"} }, "", live_gyroinvy);
    opt_add(L"Gyro turning axis", L"Controller: the controller's own yaw axis. Player: follows gravity (flat or upright).",
            "gyrospace", "PAD_GYRO_SPACE", 0, { {L"Controller", ""}, {L"Player", "player"} }, "", live_gyrospace);
    opt_add(L"Light bar", L"DualShock 4 / DualSense light bar in the player's colour.", "lightbar", "PAD_LIGHTBAR", 0,
            { {L"Player colour", ""}, {L"Off", "0"} }, "", live_lightbar);
    opt_add(L"Nintendo button layout", L"Switch Pro / Joy-Con: Nintendo style confirms with A and shows Xbox-style letters.",
            "nswap", "PAD_NINTENDO_SWAP", 0, { {L"By position", ""}, {L"Nintendo style (A confirms)", "1"} }, "", live_nswap);
    /* PC extras; keys and values match the launcher (launcher_extra_r1.inc) */
    opt_add(L"Vibration", L"Controller rumble strength.", "rumble", "PAD_RUMBLE", 0,
            { {L"100%", ""}, {L"75%", "75"}, {L"50%", "50"}, {L"25%", "25"}, {L"Off", "0"} }, "", live_rumble);
    opt_add(L"Confirm button", L"Which face button the system treats as confirm (Cross as on Western consoles, or Circle "
            L"as on Japanese ones). Takes effect on restart; the game may ignore it.", "enterbtn", "PS3_ENTER_BUTTON", 1,
            { {L"Cross", ""}, {L"Circle", "circle"} }, "", NULL);
    opt_add(L"Controller in the background", L"Auto: the controller works while the game is in the background only "
            L"when it keeps running there (In the background = Keep running or Mute only).", "padbg", "PAD_BACKGROUND_INPUT", 0,
            { {L"Auto", ""}, {L"On", "1"}, {L"Off", "0"} }, "", live_padbg);
    opt_add(L"Stick outer deadzone", L"The last part of the stick's travel already counts as full.", "outerdz",
            "PAD_OUTER_DEADZONE", 0, { {L"Off", ""}, {L"5%", "5"}, {L"10%", "10"}, {L"15%", "15"} }, "", live_outerdz);
    opt_add(L"Stick anti-deadzone", L"Skips the game's own dead zone: the smallest stick movement already moves.", "antidz",
            "PAD_ANTI_DEADZONE", 0, { {L"Off", ""}, {L"10%", "10"}, {L"20%", "20"}, {L"30%", "30"} },
            "", live_antidz);
    opt_add(L"Trigger deadzone", L"How far L2 / R2 must be pressed before they count.", "trigdz", "PAD_TRIGGER_DEADZONE", 0,
            { {L"Off", ""}, {L"10%", "10"}, {L"20%", "20"}, {L"30%", "30"} }, "", live_trigdz);
    opt_add(L"Turn boost", L"Look acceleration: holding the right stick at the edge adds turn speed after a moment (not while aiming).",
            "turnboost", "PAD_TURN_BOOST", 0, { {L"Off", ""}, {L"90 deg/s", "90"}, {L"180 deg/s", "180"}, {L"270 deg/s", "270"} }, "", live_turnboost);
    opt_add(L"Crouch button (L2 / C)", L"Hold or toggle. Experimental: L2 is also sprint with some layouts.", "crouchmode",
            "PAD_CROUCH_MODE", 0, { {L"As the game", ""}, {L"Toggle", "toggle"}, {L"Hold", "hold"} }, "", live_crouch);
    rfom_aim_assist_init();   /* normalises PORT_AIM_ASSIST (RFOM_AIM_ASSIST alias, 1 / 0) for the row below */
    opt_add(L"Aim assist", L"The game's aim assist: your aim slows down over an enemy and is pulled toward it. Auto: off "
            L"with keyboard & mouse, on with a controller. On: always, as on PS3. Off: never.", "aimassist",
            "PORT_AIM_ASSIST", 0, { {L"Auto", ""}, {L"On", "on"}, {L"Off", "off"} }, "", live_aimassist);
    opt_add(L"Master volume", L"All game sound.", "volume", "PORT_VOLUME", 0,
            { {L"100%", ""}, {L"90%", "90"}, {L"80%", "80"}, {L"70%", "70"}, {L"60%", "60"}, {L"50%", "50"}, {L"40%", "40"},
              {L"30%", "30"}, {L"20%", "20"}, {L"10%", "10"}, {L"Muted", "0"} }, "", live_volume);
    opt_add(L"Mono audio", L"Both speakers play the same mix (accessibility, one ear / one speaker).", "audio_mono",
            "PORT_AUDIO_MONO", 0, { {L"Off", ""}, {L"On", "1"} }, "", live_mono);
    opt_add(L"UI scale", L"Size of the PC menus (here at once), the F3 overlay and notifications (next start). Auto: by "
            L"screen height.", "uiscale", "PORT_UI_SCALE", 0,
            { {L"Auto", ""}, {L"75%", "75"}, {L"100%", "100"}, {L"125%", "125"}, {L"150%", "150"}, {L"175%", "175"}, {L"200%", "200"} },
            "", live_uiscale);
    opt_add(L"Frame-time graph", L"A graph of the last 120 frame times under the F3 overlay's text.", "ovgraph",
            "PORT_OVERLAY_GRAPH", 0, { {L"On", ""}, {L"Off", "0"} }, "", live_ovgraph);
}
enum { PG_MAIN, PG_KEYS, PG_PAD };
static int s_open = 0, s_page = PG_MAIN, s_sel = 0, s_top = 0, s_capture = 0, s_release = 0;
static HWND s_ov = NULL, s_game = NULL;
static int s_prev_in = 0;

/* key binding rows: controls.txt lines that bind a key */
struct KeyRow { int line; std::string key, action, label; };
static std::vector<std::string> s_ctl_lines;
static std::vector<KeyRow> s_keys;
static void keys_load(void)
{
    s_ctl_lines.clear(); s_keys.clear();
    read_lines("controls.txt", s_ctl_lines);
    for (int i = 0; i < (int)s_ctl_lines.size(); i++) {
        std::string l = s_ctl_lines[i], com;
        const size_t h = l.find('#'); if (h != std::string::npos) { com = ltrim(l.substr(h + 1)); l = l.substr(0, h); }
        const size_t eq = l.find('='); if (eq == std::string::npos) continue;
        char k[64] = {}, v[64] = {};
        if (sscanf(l.substr(0, eq).c_str(), " %63s", k) != 1 || sscanf(l.substr(eq + 1).c_str(), " %63s", v) != 1) continue;
        if (starts_ci(k, "mouse_") || starts_ci(k, "menu_") || starts_ci(k, "pad_")) continue;
        s_keys.push_back({ i, k, v, com.empty() ? v : com });
    }
}
static const struct { const char* n; int vk; } s_vkn[] = {
    {"SPACE",VK_SPACE},{"LSHIFT",VK_LSHIFT},{"RSHIFT",VK_RSHIFT},{"LCTRL",VK_LCONTROL},{"RCTRL",VK_RCONTROL},{"ALT",VK_MENU},{"TAB",VK_TAB},
    {"ENTER",VK_RETURN},{"BACKSPACE",VK_BACK},{"UP",VK_UP},{"DOWN",VK_DOWN},{"LEFT",VK_LEFT},{"RIGHT",VK_RIGHT},
    {"MOUSE1",VK_LBUTTON},{"MOUSE2",VK_RBUTTON},{"MOUSE3",VK_MBUTTON},{"MOUSE4",VK_XBUTTON1},{"MOUSE5",VK_XBUTTON2},
    {"F1",VK_F1},{"F2",VK_F2},{"F4",VK_F4},{"F5",VK_F5},{"F6",VK_F6},{"F11",VK_F11},
    {"CAPSLOCK",VK_CAPITAL},{"INSERT",VK_INSERT},{"DELETE",VK_DELETE},{"HOME",VK_HOME},{"END",VK_END},{"PGUP",VK_PRIOR},{"PGDN",VK_NEXT},
    {"NUM0",VK_NUMPAD0},{"NUM1",VK_NUMPAD1},{"NUM2",VK_NUMPAD2},{"NUM3",VK_NUMPAD3},{"NUM4",VK_NUMPAD4},{"NUM5",VK_NUMPAD5},
    {"NUM6",VK_NUMPAD6},{"NUM7",VK_NUMPAD7},{"NUM8",VK_NUMPAD8},{"NUM9",VK_NUMPAD9},
    {"GRAVE",VK_OEM_3},{"MINUS",VK_OEM_MINUS},{"EQUALS",VK_OEM_PLUS},{"LBRACKET",VK_OEM_4},{"RBRACKET",VK_OEM_6},{"BACKSLASH",VK_OEM_5},
    {"SEMICOLON",VK_OEM_1},{"APOSTROPHE",VK_OEM_7},{"COMMA",VK_OEM_COMMA},{"PERIOD",VK_OEM_PERIOD},{"SLASH",VK_OEM_2},
};
/* a newly pressed key's controls.txt name ("" = none yet). F3/F7-F10/F12 and
 * Esc are the port's own keys and are not offered. */
static std::string capture_key(void)
{
    static bool held[256];
    std::string got;
    for (int vk = 1; vk < 256; vk++) {
        const bool d = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (d && !held[vk] && got.empty()) {
            if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) got = std::string(1, (char)vk);
            else for (auto& e : s_vkn) if (e.vk == vk) { got = e.n; break; }
        }
        held[vk] = d;
    }
    return got;
}
static void keys_set(int row, const std::string& key)
{
    KeyRow& r = s_keys[row];
    char line[256]; snprintf(line, sizeof line, "%-10s = %-10s # %s", key.c_str(), r.action.c_str(), r.label.c_str());
    s_ctl_lines[r.line] = line; r.key = key;
    if (write_lines("controls.txt", s_ctl_lines)) cellpad_reload_controls();
    LOG("key binding: %s -> %s (%s)", key.c_str(), r.action.c_str(), r.label.c_str());
}

static int rows(void)
{
    if (s_page == PG_MAIN) return (int)s_opts.size() + 3;    /* + Key bindings, Controller layout, Back */
    if (s_page == PG_KEYS) return (int)s_keys.size() + 2;    /* + Restore defaults, Back */
    return XB_COUNT + 2;                                     /* + Southpaw, Back */
}
static std::wstring wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
static void row_text(int i, std::wstring& l, std::wstring& v)
{
    l.clear(); v.clear();
    const int n = rows();
    if (i == n - 1) { l = L"Back"; return; }
    if (s_page == PG_MAIN) {
        const int no = (int)s_opts.size();
        if (i == no) { l = L"Key bindings (keyboard & mouse)"; v = L"\u25B8"; return; }
        if (i == no + 1) { l = L"Controller layout"; v = L"\u25B8"; return; }
        const XOpt& o = s_opts[i];
        l = o.title; v = L"\u25C2  " + o.ch[o.sel].label + (o.restart && o.sel != o.run ? L" *" : L"") + L"  \u25B8";
    } else if (s_page == PG_KEYS) {
        if (i == (int)s_keys.size()) { l = L"Restore default keys"; return; }
        l = wide(s_keys[i].label);
        v = (s_capture && i == s_sel) ? L"press a key...  (Esc cancels)" : wide(s_keys[i].key);
    } else {
        if (i == XB_COUNT) { l = L"Southpaw (swap sticks)"; v = s_southpaw ? L"\u25C2  On  \u25B8" : L"\u25C2  Off  \u25B8"; return; }
        l = s_xb_name[i]; v = std::wstring(L"\u25C2  acts as ") + s_xb_name[s_xmap[i]] + L"  \u25B8";
    }
}
static std::wstring help_text(void)
{
    if (s_page == PG_MAIN && s_sel < (int)s_opts.size()) {
        const XOpt& o = s_opts[s_sel]; std::wstring s = o.help;
        if (o.restart) s += L"  (Applies the next time the game starts.)";
        return s;
    }
    if (s_page == PG_KEYS) return L"Enter / A: bind a new key or mouse button.  Saved to controls.txt.";
    if (s_page == PG_PAD) return L"\u2190\u2192 choose what each controller button does. Saved for the launcher too.";
    return L"";
}
static void paint(HDC dc, const RECT& rc)
{
    const int n = rows(), vis = std::min(n, 14);
    const int H = rc.bottom - rc.top, W = rc.right - rc.left, rh = H / (vis + 4);
    if (s_sel < s_top) s_top = s_sel;
    if (s_sel >= s_top + vis) s_top = s_sel - vis + 1;
    const int top = rc.top + rh * 3 / 2;
    auto font = [&](int px, int wt) { return CreateFontW(-px, 0, 0, 0, wt, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                                          CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI"); };
    HFONT ft = font(rh * 62 / 100, FW_BOLD), fr = font(rh * 46 / 100, FW_SEMIBOLD), fs = font(rh * 34 / 100, FW_NORMAL);
    HGDIOBJ of = SelectObject(dc, ft);
    RECT r = { rc.left + rh / 2, rc.top + rh / 3, rc.right - rh / 2, top };
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, s_page == PG_MAIN ? L"MORE PC OPTIONS" : s_page == PG_KEYS ? L"KEY BINDINGS" : L"CONTROLLER LAYOUT", -1, &r,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, fr);
    const int vx = rc.left + W * 50 / 100;
    for (int k = 0; k < vis && s_top + k < n; k++) {
        const int i = s_top + k;
        RECT rr = { rc.left + 6, top + k * rh, rc.right - 6, top + (k + 1) * rh };
        if (i == s_sel) { HBRUSH b = CreateSolidBrush(RGB(150, 32, 24)); FillRect(dc, &rr, b); DeleteObject(b); }
        SetTextColor(dc, i == s_sel ? RGB(255, 255, 255) : RGB(205, 205, 210));
        std::wstring l, v; row_text(i, l, v);
        RECT rl = { rr.left + rh / 2, rr.top, vx, rr.bottom };
        DrawTextW(dc, l.c_str(), -1, &rl, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT rv = { vx, rr.top, rr.right - rh / 2, rr.bottom };
        DrawTextW(dc, v.c_str(), -1, &rv, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    SelectObject(dc, fs);
    SetTextColor(dc, RGB(185, 185, 190));
    RECT rh1 = { rc.left + rh / 2, top + vis * rh + rh / 6, rc.right - rh / 2, top + vis * rh + rh * 7 / 6 };
    DrawTextW(dc, help_text().c_str(), -1, &rh1, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
    SetTextColor(dc, RGB(140, 140, 150));
    RECT rh2 = { rc.left + rh / 2, top + vis * rh + rh * 7 / 6, rc.right - rh / 2, rc.bottom - rh / 6 };
    DrawTextW(dc, L"\u2191\u2193 select    \u2190\u2192 change    Enter / A  OK    Esc / B  back", -1, &rh2,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, of); DeleteObject(ft); DeleteObject(fr); DeleteObject(fs);
}
static std::wstring s_msg1, s_msg2;     /* a dialog instead of the menu (controller lost, photo mode) */
static LRESULT CALLBACK ov_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HDC mdc = CreateCompatibleDC(dc);
        HBITMAP bm = CreateCompatibleBitmap(dc, rc.right - rc.left, rc.bottom - rc.top);
        HGDIOBJ obm = SelectObject(mdc, bm);
        HBRUSH bg = CreateSolidBrush(RGB(16, 16, 20)); FillRect(mdc, &rc, bg); DeleteObject(bg);
        HBRUSH fr = CreateSolidBrush(RGB(190, 40, 30));
        for (int i = 0; i < 2; i++) { RECT r = { rc.left + i, rc.top + i, rc.right - i, rc.bottom - i }; FrameRect(mdc, &r, fr); }
        DeleteObject(fr);
        SetBkMode(mdc, TRANSPARENT);
        if (s_open) paint(mdc, rc);
        else {
            const int H = rc.bottom - rc.top;
            HFONT f1 = CreateFontW(-H * 26 / 100, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            HFONT f2 = CreateFontW(-H * 15 / 100, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            HGDIOBJ of = SelectObject(mdc, f1);
            RECT r1 = { rc.left + 8, rc.top + H * 10 / 100, rc.right - 8, rc.top + H * 55 / 100 };
            SetTextColor(mdc, RGB(255, 255, 255));
            DrawTextW(mdc, s_msg1.c_str(), -1, &r1, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(mdc, f2);
            RECT r2 = { rc.left + 8, rc.top + H * 55 / 100, rc.right - 8, rc.bottom - H * 10 / 100 };
            SetTextColor(mdc, RGB(200, 200, 205));
            DrawTextW(mdc, s_msg2.c_str(), -1, &r2, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(mdc, of); DeleteObject(f1); DeleteObject(f2);
        }
        BitBlt(dc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, obm); DeleteObject(bm); DeleteDC(mdc);
        EndPaint(h, &ps);
        return 0; }
    }
    return DefWindowProcW(h, m, w, l);
}
/* kind 0 = menu (centre, large), 1 = dialog (centre), 2 = small note at the top */
static void ov_show(int kind)
{
    if (!s_ov) {
        WNDCLASSEXW wc = { sizeof wc };
        wc.lpfnWndProc = ov_proc; wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.lpszClassName = L"rfx_overlay";
        RegisterClassExW(&wc);
        s_ov = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                               L"rfx_overlay", L"", WS_POPUP, 0, 0, 10, 10, s_game, NULL, wc.hInstance, NULL);
        if (s_ov) SetLayeredWindowAttributes(s_ov, 0, 235, LWA_ALPHA);
    }
    if (!s_ov) return;
    RECT rc; GetClientRect(s_game, &rc);
    POINT tl = { 0, 0 }; ClientToScreen(s_game, &tl);
    const int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    int w, h, x, y;
    const int us = pc_ui_scale_menu();   /* PORT_UI_SCALE */
    if (kind == 0) {
        const int rh = std::max(30, ch * 48 / 1000);
        w = std::max(640, cw * 50 / 100); h = rh * (std::min(rows(), 14) + 4);
        w = std::min(pcx_ui_px(w, us), cw); h = std::min(pcx_ui_px(h, us), ch);
        x = tl.x + (cw - w) / 2; y = tl.y + (ch - h) / 2;
    } else {
        w = std::max(kind == 2 ? 300 : 420, cw * (kind == 2 ? 28 : 46) / 100);
        h = std::max(kind == 2 ? 56 : 130, ch * (kind == 2 ? 8 : 20) / 100);
        w = std::min(pcx_ui_px(w, us), cw); h = std::min(pcx_ui_px(h, us), ch);
        x = tl.x + (cw - w) / 2; y = kind == 2 ? tl.y + ch * 4 / 100 : tl.y + (ch - h) / 2;
    }
    SetWindowPos(s_ov, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(s_ov, NULL, TRUE);
    UpdateWindow(s_ov);
}
static void ov_hide(void) { if (s_ov) ShowWindow(s_ov, SW_HIDE); }

enum { IN_YES = 1, IN_NO = 2, IN_UP = 8, IN_DOWN = 16, IN_LEFT = 32, IN_RIGHT = 64, IN_F10 = 128 };
/* All controllers' buttons (XInput bits), the left stick as the d-pad;
 * from cellPad.c, which reads SDL3 or XInput (PAD_BACKEND). */
extern "C" unsigned cellpad_menu_buttons(int* any_connected);
static WORD xi_raw(int* any_connected) { return (WORD)cellpad_menu_buttons(any_connected); }
static int read_input(int focused)
{
    int r = 0;
    if (focused) {
        auto kd = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        if (kd(VK_RETURN) || kd(VK_SPACE)) r |= IN_YES;
        if (kd(VK_ESCAPE) || kd(VK_BACK)) r |= IN_NO;
        if (kd(VK_F10) || kd(VK_F9)) r |= IN_F10;
        if (kd(VK_UP)) r |= IN_UP;
        if (kd(VK_DOWN)) r |= IN_DOWN;
        if (kd(VK_LEFT)) r |= IN_LEFT;
        if (kd(VK_RIGHT)) r |= IN_RIGHT;
    }
    const WORD b = xi_raw(NULL);
    if (b & XINPUT_GAMEPAD_A) r |= IN_YES;
    if (b & XINPUT_GAMEPAD_B) r |= IN_NO;
    if (b & XINPUT_GAMEPAD_DPAD_UP) r |= IN_UP;
    if (b & XINPUT_GAMEPAD_DPAD_DOWN) r |= IN_DOWN;
    if (b & XINPUT_GAMEPAD_DPAD_LEFT) r |= IN_LEFT;
    if (b & XINPUT_GAMEPAD_DPAD_RIGHT) r |= IN_RIGHT;
    return r;
}
static int edges(int in)
{
    static ULONGLONG rep_at = 0;
    const ULONGLONG now = GetTickCount64();
    int e = in & ~s_prev_in;
    const int dirs = in & (IN_UP | IN_DOWN | IN_LEFT | IN_RIGHT);
    if (e & dirs) rep_at = now + 400;
    else if (dirs && rep_at && now >= rep_at) { e |= dirs; rep_at = now + 110; }
    if (!dirs) rep_at = 0;
    s_prev_in = in;
    return e;
}
static void opt_change(int i, int d)
{
    XOpt& o = s_opts[i];
    const int n = (int)o.ch.size();
    o.sel = (o.sel + d + n) % n;
    const std::string& v = o.ch[o.sel].value;
    ini_set(o.ini, v);
    bat_set(o.env, v);
    if (o.live) o.live(v);
    LOG("%ls = %ls%s", o.title, o.ch[o.sel].label.c_str(), o.restart ? " (next start)" : "");
}
/* The same rows for the game's own Options screen (rfom_menurows.cpp). */
extern "C" int rfom_pcx_rows(void) { opts_init(); return (int)s_opts.size(); }
extern "C" void rfom_pcx_row(int i, char* title, int tcap, char* val, int vcap)
{
    opts_init();
    title[0] = val[0] = 0;
    if (i < 0 || i >= (int)s_opts.size()) return;
    const XOpt& o = s_opts[i];
    snprintf(title, (size_t)tcap, "%ls", o.title);
    snprintf(val, (size_t)vcap, "%ls%s", o.ch[o.sel].label.c_str(), o.restart && o.sel != o.run ? " (restart)" : "");
}
extern "C" void rfom_pcx_change(int i, int d) { opts_init(); if (i >= 0 && i < (int)s_opts.size() && d) opt_change(i, d < 0 ? -1 : 1); }
extern "C" const char* rfom_pcx_key(int i) { opts_init(); return i >= 0 && i < (int)s_opts.size() ? s_opts[i].ini : nullptr; }
static void pad_change(int i, int d)
{
    if (i == XB_COUNT) {
        s_southpaw = !s_southpaw;
        ini_set("pad_southpaw", s_southpaw ? "1" : ""); bat_set("PAD_SOUTHPAW", s_southpaw ? "1" : "");
        return;
    }
    s_xmap[i] = (s_xmap[i] + d + XB_COUNT) % XB_COUNT;
    int ident = 1; for (int k = 0; k < XB_COUNT; k++) if (s_xmap[k] != k) ident = 0;
    s_xmap_ident = ident;
    char ini[32], env[32]; snprintf(ini, sizeof ini, "padmap_%s", s_xb_key[i]); snprintf(env, sizeof env, "PAD_MAP_%s", s_xb_key[i]);
    for (char* c = ini; *c; c++) *c = (char)tolower((unsigned char)*c);
    const std::string v = s_xmap[i] == i ? "" : s_xb_key[s_xmap[i]];
    ini_set(ini, v); bat_set(env, v);
}
extern "C" int rfx_menu_open(void) { return s_open || s_release; }
extern "C" void rfx_open_menu(void)
{
    opts_init();
    s_open = 1; s_page = PG_MAIN; s_sel = 0; s_top = 0; s_capture = 0;
    s_prev_in = -1;                              /* the key that opened it is not a press */
    g_rfom_input_block = 1;
    ClipCursor(NULL);
    ov_show(0);
    LOG("extras menu");
}
static void menu_close(void) { s_open = 0; s_capture = 0; s_release = 1; ov_hide(); }
static void menu_tick(int focused)
{
    g_rfom_input_block = 1;
    if (s_capture) {
        const std::string k = capture_key();
        if (s_capture == 1) { if (k.empty() && !(GetAsyncKeyState(VK_RETURN) & 0x8000) && !(GetAsyncKeyState(VK_SPACE) & 0x8000)) s_capture = 2; return; }
        if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000)) { s_capture = 0; s_prev_in = read_input(focused); InvalidateRect(s_ov, NULL, FALSE); return; }
        if (!k.empty() && focused) { keys_set(s_sel, k); s_capture = 0; s_prev_in = read_input(focused) | IN_YES; InvalidateRect(s_ov, NULL, FALSE); }
        return;
    }
    const int in = read_input(focused);
    if (s_prev_in < 0) { s_prev_in = in; return; }
    const int e = edges(in), n = rows();
    if (e & IN_F10) { menu_close(); return; }
    if (e & IN_UP) s_sel = (s_sel + n - 1) % n;
    else if (e & IN_DOWN) s_sel = (s_sel + 1) % n;
    else if (e & IN_NO) {
        if (s_page == PG_MAIN) { menu_close(); return; }
        s_sel = (int)s_opts.size() + (s_page == PG_PAD); s_page = PG_MAIN; s_top = 0; ov_show(0); return;
    } else if (e & (IN_LEFT | IN_RIGHT | IN_YES)) {
        const int d = (e & IN_LEFT) ? -1 : 1;
        const bool ok = (e & IN_YES) != 0;
        if (s_sel == n - 1) {                    /* Back */
            if (!ok) return;
            if (s_page == PG_MAIN) { menu_close(); return; }
            s_sel = (int)s_opts.size() + (s_page == PG_PAD); s_page = PG_MAIN; s_top = 0; ov_show(0); return;
        }
        if (s_page == PG_MAIN) {
            const int no = (int)s_opts.size();
            if (s_sel < no) opt_change(s_sel, d);
            else if (ok) { s_page = s_sel == no ? PG_KEYS : PG_PAD; s_sel = 0; s_top = 0; if (s_page == PG_KEYS) keys_load(); ov_show(0); return; }
        } else if (s_page == PG_KEYS) {
            if (!ok) return;
            if (s_sel == (int)s_keys.size()) {   /* defaults: cellPad writes a fresh file */
                CopyFileA("controls.txt", "controls.txt.bak", FALSE);
                DeleteFileA("controls.txt"); cellpad_reload_controls();
                LOG("key bindings reset (old file: controls.txt.bak)");
                s_page = PG_MAIN; s_sel = (int)s_opts.size(); ov_show(0); return;
            }
            s_capture = 1;
        } else pad_change(s_sel, d);
    } else return;
    InvalidateRect(s_ov, NULL, FALSE);
}

/* ---- crosshair: a click-through colour-keyed window at the centre ---- */
static HWND s_xh = NULL; static int s_xh_kind = -1;
static unsigned s_xh_krgb = 0; static int s_xh_kscale = 0;    /* colour / size the marker was last drawn with */
static int xh_px(void) { return (24 * s_xh_scale / 100 + 1) & ~1; }   /* canvas side: 24 px at 100 % */
static void xh_draw(HDC dc, const RECT& rc)
{
    HBRUSH k = CreateSolidBrush(RGB(255, 0, 255)); FillRect(dc, &rc, k); DeleteObject(k);
    const int c = rc.right / 2;
    const double s = s_xh_scale / 100.0;
    auto px = [&](double v) { return (int)floor(v * s + 0.5); };
    const int ol = std::max(1, px(1));           /* dark outline width */
    HBRUSH o = CreateSolidBrush(RGB(0, 0, 0)),
           f = CreateSolidBrush(RGB((s_xh_rgb >> 16) & 255, (s_xh_rgb >> 8) & 255, s_xh_rgb & 255));
    if (s_xh_kind == 1) {
        const int h = std::max(2, px(3));
        RECT a = { c - h, c - h, c + h, c + h }; FillRect(dc, &a, o);
        RECT b = { c - h + ol, c - h + ol, c + h - ol, c + h - ol }; FillRect(dc, &b, f);
    } else {
        const int n = px(3), x = px(10), w = std::max(2, px(2));
        RECT a = { c - x, c - w, c - n, c + w }, b = { c + n, c - w, c + x, c + w }, d = { c - w, c - x, c + w, c - n }, e = { c - w, c + n, c + w, c + x };
        for (RECT* r : { &a, &b, &d, &e }) { FillRect(dc, r, o); RECT in = { r->left + ol, r->top + ol, r->right - ol, r->bottom - ol }; FillRect(dc, &in, f); }
    }
    DeleteObject(o); DeleteObject(f);
    s_xh_krgb = s_xh_rgb; s_xh_kscale = s_xh_scale;
}
static LRESULT CALLBACK xh_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc);
        xh_draw(dc, rc);
        EndPaint(h, &ps); return 0;
    }
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}
static void crosshair_tick(int show)
{
    if (!show) { if (s_xh && IsWindowVisible(s_xh)) ShowWindow(s_xh, SW_HIDE); return; }
    if (!s_xh) {
        WNDCLASSEXW wc = { sizeof wc }; wc.lpfnWndProc = xh_proc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"rfx_crosshair";
        RegisterClassExW(&wc);
        s_xh = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                               L"rfx_crosshair", L"", WS_POPUP, 0, 0, 24, 24, s_game, NULL, wc.hInstance, NULL);
        if (!s_xh) return;
        SetLayeredWindowAttributes(s_xh, RGB(255, 0, 255), 220, LWA_COLORKEY | LWA_ALPHA);
    }
    const int sz = xh_px();
    if (s_xh_kind != s_crosshair || s_xh_krgb != s_xh_rgb || s_xh_kscale != s_xh_scale) {
        s_xh_kind = s_crosshair; s_xh_krgb = s_xh_rgb; s_xh_kscale = s_xh_scale; InvalidateRect(s_xh, NULL, TRUE); }
    RECT rc; GetClientRect(s_game, &rc); POINT c = { (rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2 }; ClientToScreen(s_game, &c);
    SetWindowPos(s_xh, HWND_TOPMOST, c.x - sz / 2, c.y - sz / 2, sz, sz, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

/* ======================================================================
 * The per-tick driver (backend pc_quit_thread, every ~30 ms) */
static int s_init = 0, s_disc_paused = 0, s_photo_ov = 0;
static volatile int s_more_req = 0;
static void more_menu_req(void) { s_more_req = 1; }   /* the PC menu's "More PC Options" row */
static void init_once(void)
{
    if (s_init) return;
    s_init = 1;
    opts_init();
    for (auto& o : s_opts) if (o.live) o.live(o.ch[o.run].value);   /* apply the start-up values */
    s_photo_allowed = envi("PHOTO", 1) != 0;
    s_prev_vfs_hook = g_vfs_override_hook;    /* rfom_mods.cpp sets its hook at static init */
    g_vfs_override_hook = vfs_hook;
    if (envi("DISCORD", 0)) { HANDLE t = CreateThread(NULL, 0, discord_thread, NULL, 0, NULL); if (t) CloseHandle(t); }
    s_bench = envi("BENCH", 0) != 0;
    if (s_bench) {
        s_bench_secs = std::max(5.0, atof(envs("BENCH_SECONDS", "60").c_str()));
        s_bench_settle = std::max(0.0, atof(envs("BENCH_SETTLE", "45").c_str()));
        if (!s_is_level_exe) bench_parse();
        FILETIME c, e, k, u; GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
        FILETIME nowft; GetSystemTimeAsFileTime(&nowft);
        const double since = ((double)((((ULONGLONG)nowft.dwHighDateTime) << 32 | nowft.dwLowDateTime) -
                                       (((ULONGLONG)c.dwHighDateTime) << 32 | c.dwLowDateTime))) / 1e7;
        s_bench_t0 = now_s() - since;            /* the script is timed from process start */
        LOG("benchmark mode (%s): %zu menu steps; in the level %.0f s settle, then %.0f s",
            s_is_level_exe ? "level program" : "front end", s_bench_steps.size(), s_bench_settle, s_bench_secs);
    }
    LOG("extras: F9 / PC menu -> More PC Options; photo mode %s (F7); movie skip %s (unwatched %s); intro skip %s; crosshair %d; pause on pad loss %s",
        s_photo_allowed ? "on" : "off", s_cut_skip ? "on" : "off", s_cut_skip_all ? "on" : "off", s_skip_intro ? "on" : "off", s_crosshair, s_disc_pause_on ? "on" : "off");
    if (skip_test_unseen())
        LOG("extras: PORT_SKIP_TEST_UNSEEN=1 (test): every movie reads as unwatched to the skip check; a hold skips it only with %s_CUTSCENE_SKIP_ALL on (now %s)",
            PCX_PREFIX, s_cut_skip && s_cut_skip_all ? "on" : "off");
}
static int s_want_pause = 0;
static volatile ULONGLONG s_shot_run_until = 0;      /* photo mode: let the game present a frame for F12 */
static int want_pause(void) { return s_want_pause && GetTickCount64() >= s_shot_run_until; }
static void ui_tick(void* hwnd_, int focused, int other_prompt)
{
    HWND hwnd = (HWND)hwnd_;
    if (!hwnd) return;
    s_game = hwnd;
    init_once();
    { MSG m; while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); } }   /* our overlay windows */
    if (other_prompt) { if (s_open) menu_close(); return; }   /* the PC menu / quit prompt owns input */
    const ULONGLONG now = GetTickCount64();
    movie_tick();
    bench_tick(hwnd);
    bench_key_tick(focused, other_prompt);

    if (s_more_req) { s_more_req = 0; if (!s_open && !s_photo_on) rfx_open_menu(); }
    /* F9: the extras menu directly; F10 -> More PC Options opens it from the PC menu */
    { static int was = 0; const int d = focused && (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
      if (d && !was && !s_open && !s_photo_on) rfx_open_menu();
      was = d; }
    if (s_open) menu_tick(focused);
    else if (s_release) {                        /* hold the game's input until the closing key is let go */
        if (!(read_input(focused) & (IN_YES | IN_NO | IN_F10))) { s_release = 0; g_rfom_input_block = 0; }
        else g_rfom_input_block = 1;
    }

    /* photo mode */
    { static int was = 0; const int d = focused && (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
      if (d && !was && s_photo_allowed && !s_open) {
          s_photo_on = !s_photo_on;
          static int ov_was = 0;
          /* RFOM_PHOTO_HIDEHUD (default 1; 0 = HUD stays): the backend presents the HUD-less frame from the
           * HUD split, and the game runs ~120 ms more so one such frame reaches the screen before the pause */
          static const int hide_hud = envi("PHOTO_HIDEHUD", 1) != 0;
          g_pc_photo_nohud = s_photo_on && hide_hud;
          if (s_photo_on && hide_hud) s_shot_run_until = now + 120;
          if (s_photo_on) { ov_was = rfpf_overlay_get(); rfpf_overlay_set(0); s_msg1 = L"Photo mode"; s_msg2 = L"F12 screenshot    F7 back to the game";
                            ov_show(2); s_photo_ov = 1; LOG("photo mode on"); }
          else { rfpf_overlay_set(ov_was); ov_hide(); s_photo_ov = 0; s_release = 1; LOG("photo mode off"); }
      }
      was = d; }
    /* F12 / PrtScn in photo mode: the paused game presents nothing, so it runs (input
     * still blocked) for a moment while the backend takes the shot */
    { static int was = 0; const int d = focused && ((GetAsyncKeyState(VK_F12) & 0x8000) || (GetAsyncKeyState(VK_SNAPSHOT) & 0x8000));
      if (d && !was && s_photo_on) { if (s_photo_ov) { ov_hide(); s_photo_ov = 0; } s_shot_run_until = now + 150; }
      was = d; }
    { static ULONGLONG t_on = 0;
      if (s_photo_on && s_photo_ov) { if (!t_on) t_on = now; if (now - t_on > 2000) { ov_hide(); s_photo_ov = 0; } }
      if (!s_photo_on) t_on = 0; }

    /* pause when the controller in use goes away */
    { int conn = 0; xi_raw(&conn);
      static int was_conn = -1;
      if (was_conn == 1 && !conn && s_disc_pause_on && !s_open && !s_photo_on && !s_bench) {
          s_disc_paused = 1; s_msg1 = L"Controller disconnected"; s_msg2 = L"Reconnect it, or press Enter to continue";
          ov_show(1); LOG("controller lost: paused");
      }
      if (s_disc_paused && (conn || (focused && (GetAsyncKeyState(VK_RETURN) & 0x8000)))) {
          s_disc_paused = 0; s_release = 1; if (!s_open) ov_hide(); LOG("controller pause ended");
      }
      was_conn = conn; }

    /* movies: the intro is skipped on its own (RFOM_SKIP_INTRO), story movies on a 1 s hold.
     * PORT_SKIP_CLEAR (default 1; 0 = the old skip logic): held Cross / Start read as released between the
     * pulses (below), and the hold timer restarts whenever a guard skips the skip logic (no movie, our menu
     * up, the movie not on screen yet), so a stale hold never injects at once: it always takes a fresh 1 s. */
    static ULONGLONG held_since = 0;
    static int clear_on = -1;
    if (clear_on < 0) {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
        const char* e = getenv("PORT_SKIP_CLEAR");
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
        clear_on = (e && *e) ? atoi(e) != 0 : 1;
    }
    if (!(s_movie_on && !s_open)) { if (clear_on) held_since = 0; }
    else {
        static ULONGLONG told = 0;
        const WORD b = xi_raw(NULL);
        const int auto_skip = s_skip_intro && s_movie_intro && !s_is_level_exe;
        int held = (focused && ((GetAsyncKeyState(VK_SPACE) & 0x8000) || (GetAsyncKeyState(VK_RETURN) & 0x8000))) ||
                   (b & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START));
        if (!s_cut_skip) held = 0;
        if (auto_skip) { held = 1; if (!held_since) held_since = now - 1000; }
        else if (s_cut_skip && told != s_movie_t0 && now - s_movie_t0 > 500) { told = s_movie_t0; if (skip_hint_on()) rfpf_toast("Movie", "Hold Space / A to skip", 0); }
        if (!held) held_since = 0;
        else if (!held_since) held_since = now;
        else if (clear_on && cellGcm_get_flip_mode() != 2) held_since = 0;   /* movie not on screen: restart the hold */
        else if (now - held_since >= 1000 && cellGcm_get_flip_mode() == 2) {
            /* While skipping, a held Cross / Start (A, Space, Enter, Menu) reads as released between the pulses,
             * so every pulse is a fresh press edge -- holding A (= Cross) used to hide each CROSS pulse, so the
             * skip depended on the key held (GitHub issue #6). PORT_SKIP_CLEAR=0: the old pulses only. */
            const int pulse = (now - held_since) % 400 < 120;
            if (pulse || (clear_on && (!g_pad_inj_btn || now >= g_pad_inj_until))) {   /* never cut a pulse short */
                g_pad_inj_sticks = 0x80808080u;
                g_pad_inj_clear = clear_on ? 0x4008u /* CROSS|START */ : 0u;
                g_pad_inj_btn = !pulse ? 0u : ((now - held_since) / 400) & 1 ? 0x4000u /* CROSS */ : 0x0008u /* START */;
                g_pad_inj_until = now + (pulse ? 80 : 30);
            }
            static ULONGLONG logged = 0;
            if (logged != s_movie_t0) { logged = s_movie_t0; LOG("movie skip: held 1 s, pulsing Start/Cross%s%s", clear_on ? " (held buttons cleared)" : "",
                                                                  s_cut_skip && s_cut_skip_all ? "; unwatched movies allowed" : ""); }
            if (s_cut_skip && s_cut_skip_all && s_skip_armed != s_movie_t0) { s_skip_arm_ms = now; s_skip_armed = s_movie_t0; }
        }
    }

    const int on_foot = g_rfom_play_ms && now - g_rfom_play_ms < 250;
    crosshair_tick(s_crosshair && focused && on_foot && !s_open && !s_photo_on);

    s_want_pause = s_open || s_photo_on || s_disc_paused;
    /* the game gets no input while any of ours is up; released once, when they all end */
    { static int held = 0;
      if (s_open || s_release || s_photo_on || s_disc_paused) { g_rfom_input_block = 1; held = 1; }
      else if (held) { held = 0; g_rfom_input_block = 0; } }
}

/* registered before main(); the hooks run once the game does */
static struct PcExtraInit {
    PcExtraInit()
    {
        char exe[MAX_PATH] = ""; GetModuleFileNameA(NULL, exe, MAX_PATH);
        const char* b = strrchr(exe, '\\'); b = b ? b + 1 : exe;
        s_is_level_exe = _strnicmp(b, "ResistanceFoMGame", 17) == 0;
        if (envi("BENCH", 0)) { _putenv("RSX_FOCUS_PAUSE=0"); _putenv("RSX_FOCUS_MUTE=0"); }   /* the run must not stop in the background */
        xmap_load();
        g_port_xi_hook = xi_hook;
        g_pc_ui_tick = ui_tick;
        g_pc_want_pause = want_pause;
        g_pc_more_menu = more_menu_req;
    }
} s_pcextra_init;
