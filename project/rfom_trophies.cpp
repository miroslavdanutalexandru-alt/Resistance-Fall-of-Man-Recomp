// Resistance: Fall of Man PC port: trophy notifications, the same as the Resistance 2 / 3 ports.
//
// The retail game has no trophy support (it predates PS3 trophies: the ELFs don't import
// sceNpTrophy and the disc has no TROPDIR), so on an unmodified copy this never fires. It is
// here so a game or mod that does unlock trophies through the runtime's sceNpTrophy HLE gets
// the same behaviour as R2 / R3. The HLE records unlocks (gamedata\trophies\<comm id>.json);
// this hooks it (g_trophy_unlock_hook) to:
//   - name the trophy from TROPHY.TRP (gamefiles\PS3_GAME\TROPDIR\<comm id>_00\TROPHY.TRP),
//   - mirror the unlock list to %USERPROFILE%\Saved Games\Resistance Fall of Man\trophies.json,
//   - log "[trophy] unlocked #N <name>" and show a PS3-style pop-up (port_trophy_popup.h; drawn
//     inside the swap chain, rsx_osd.inc). PORT_TROPHY_STYLE=old (or RFOM_TROPHY_STYLE=old): the
//     older toast with the grade badge. RFOM_TROPHY_TOAST=0 turns the notification off,
//   - never announce a trophy that the Saved Games record already has,
//   - award the platinum once every trophy that counts towards it is unlocked.
//
// The port's fan-made achievements (rfom_achievements.h, the launcher's Achievements page) pop up in
// game too: a watcher reads PROFILE.SAV whenever the game writes it and announces what is earned but
// not yet in Saved Games\Resistance Fall of Man\achievements_announced.txt (one name per line), so an
// unlock saved just before the front end / game hand-over is announced by the next process. Without
// that file (the first run with this) whatever the first readable save has is taken as announced.
// PORT_TROPHY_TEST=1: a few seconds after start-up, one sample pop-up per grade.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>
#include "rfom_trophy_data.h"
#include "rfom_achievements.h"
#include "port_trophy_popup.h"
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

extern "C" int (*volatile g_trophy_unlock_hook)(const char* comm_id, int trophy_id, const unsigned char* unlocked, unsigned count);
extern "C" const char* ppu_vfs_root;
extern "C" void rfpf_toast(const char* title, const char* text, char badge);
extern "C" void rfpf_trophy(const char* line1, const char* name, char grade);
extern "C" unsigned long long rfpf_trophy_ticket(const char* line1, const char* name, char grade);
extern "C" unsigned long long rfpf_toast_ticket(const char* title, const char* text, char badge);
extern "C" int rfpf_toast_shown(unsigned long long ticket);

namespace {

std::mutex s_mtx;
std::string s_loaded_comm;
std::vector<rft::Trophy> s_list;
std::string s_title;

std::string saved_games_dir()
{
    PWSTR p = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SavedGames, 0, nullptr, &p)) && p) {
        char b[MAX_PATH * 3];
        if (WideCharToMultiByte(CP_UTF8, 0, p, -1, b, sizeof b, nullptr, nullptr) > 0) out = b;
    }
    if (p) CoTaskMemFree(p);
    if (out.empty()) { const char* u = getenv("USERPROFILE"); out = std::string(u ? u : ".") + "\\Saved Games"; }
    out += "\\Resistance Fall of Man";
    return out;
}

void load(const char* comm)
{
    if (s_loaded_comm == comm) return;
    s_loaded_comm = comm;
    const std::string root = ppu_vfs_root && *ppu_vfs_root ? ppu_vfs_root : "gamefiles";
    std::string trp = root + "/PS3_GAME/TROPDIR/" + comm + "_00/TROPHY.TRP";
    if (GetFileAttributesA(trp.c_str()) == INVALID_FILE_ATTRIBUTES)
        trp = std::string("gamefiles/PS3_GAME/TROPDIR/") + comm + "_00/TROPHY.TRP";
    if (rft::load_trp(trp.c_str(), s_list, &s_title))
        fprintf(stderr, "[trophy] %zu trophies (%s) from %s\n", s_list.size(), s_title.c_str(), trp.c_str());
    else
        fprintf(stderr, "[trophy] %s not readable: unlocks are recorded without names\n", trp.c_str());
    const std::string sg = saved_games_dir() + "\\trophies.json";
    rft::merge_json_times(sg.c_str(), s_list);
    // only the Saved Games record decides "already earned": the runtime's own record has just
    // been written with this unlock
}

int hook(const char* comm_id, int id, const unsigned char* unlocked, unsigned count)
{
    std::lock_guard<std::mutex> lk(s_mtx);
    load(comm_id ? comm_id : "");
    std::vector<int> toast;
    const int plat = rft::on_unlock(s_list, id, unlocked, count, (long long)time(nullptr), toast);

    const std::string dir = saved_games_dir();
    SHCreateDirectoryExA(nullptr, dir.c_str(), nullptr);
    rft::write_json((dir + "\\trophies.json").c_str(), s_title.empty() ? "Resistance: Fall of Man" : s_title.c_str(),
                    comm_id ? comm_id : "", s_list);

    const char* e = getenv("RFOM_TROPHY_TOAST");
    const bool show = !(e && *e == '0');
    if (toast.empty()) fprintf(stderr, "[trophy] #%d already earned (Saved Games record), not announced again\n", id);
    for (int tid : toast) {
        const rft::Trophy* tr = nullptr;
        for (auto& x : s_list) if (x.id == tid) tr = &x;
        char head[96];
        snprintf(head, sizeof head, "%s trophy unlocked", tr ? rft::grade_name(tr->grade) : "Trophy");
        std::string body = tr ? tr->name : "Trophy #" + std::to_string(tid);
        if (tr && !tr->detail.empty()) body += " \xE2\x80\x94 " + tr->detail;
        fprintf(stderr, "[trophy] unlocked #%d %s (%s)\n", tid, tr ? tr->name.c_str() : "",
                tr ? rft::grade_name(tr->grade) : "?");
        if (!show) continue;
        if (ptp::style_old("RFOM")) rfpf_toast(head, body.c_str(), tr ? tr->grade : 'B');
        else rfpf_trophy("You have earned a trophy.", tr ? tr->name.c_str() : body.c_str(), tr ? tr->grade : 'B');
    }
    return plat;
}

// ---- the fan-made achievements, in game ----
// The pop-up's ticket (rfpf_toast_shown), 0 = not queued (queue full).
unsigned long long announce_achievement(const rfach::Achievement& a, bool test)
{
    fprintf(stderr, "[achievement] %s%s (%s)\n", test ? "test: " : "earned ", a.name, rft::grade_name(a.grade));
    if (ptp::style_old("RFOM")) {
        char head[96];
        snprintf(head, sizeof head, "%s achievement unlocked", rft::grade_name(a.grade));
        return rfpf_toast_ticket(head, (std::string(a.name) + " \xE2\x80\x94 " + a.detail).c_str(), a.grade);
    }
    return rfpf_trophy_ticket("You have earned an achievement.", a.name, a.grade);
}

std::string profile_path()
{
    const char* gd = getenv("PS3_GAMEDATA_ROOT");   // set by the port at start-up
    return std::string(gd && *gd ? gd : "gamedata") + "\\dev_hdd0\\home\\00000001\\savedata\\BCUS98107_PROFILE\\PROFILE.SAV";
}

DWORD WINAPI achievement_thread(void*)
{
    Sleep(4000);
    if (ptp::test_on()) {   // PORT_TROPHY_TEST=1: one of each grade
        rfach::Profile none;
        for (char g : { 'B', 'S', 'G', 'P' })
            for (const auto& a : rfach::evaluate(none))
                if (a.grade == g) { announce_achievement(a, true); break; }
    }
    const char* e = getenv("RFOM_TROPHY_TOAST");
    if (e && *e == '0') return 0;
    const std::string rec = saved_games_dir() + "\\achievements_announced.txt";
    std::vector<std::string> seen;
    bool have_rec = false;
    if (FILE* f = fopen(rec.c_str(), "rb")) {
        have_rec = true;
        char line[256];
        while (fgets(line, sizeof line, f)) {
            std::string s = line;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            if (!s.empty()) seen.push_back(s);
        }
        fclose(f);
    }
    auto save_rec = [&]() {
        SHCreateDirectoryExA(nullptr, saved_games_dir().c_str(), nullptr);
        const std::string tmp = rec + ".tmp";
        FILE* f = fopen(tmp.c_str(), "wb");
        if (!f) return;
        for (const auto& s : seen) fprintf(f, "%s\n", s.c_str());
        fclose(f);
        MoveFileExA(tmp.c_str(), rec.c_str(), MOVEFILE_REPLACE_EXISTING);
    };
    auto is_seen = [&](const char* n) { for (const auto& s : seen) if (s == n) return true; return false; };
    // A name goes into the record only once its own pop-up is on screen (dequeued): one still queued
    // when this process ends (the front end handing over to the game while its window is not in the
    // foreground) is announced again by the next process. Pop-ups are checked every 250 ms, PROFILE.SAV every 3 s.
    struct Pending { std::string name; unsigned long long ticket; };
    std::vector<Pending> unsaved;
    ULONGLONG stamp = ~0ULL;       // last write time + size seen
    for (unsigned tick = 0;; Sleep(250), tick++) {
        bool flushed = false;
        for (size_t i = 0; i < unsaved.size();)
            if (rfpf_toast_shown(unsaved[i].ticket)) { seen.push_back(unsaved[i].name); unsaved.erase(unsaved.begin() + i); flushed = true; }
            else i++;
        if (flushed) save_rec();
        if (tick % 12) continue;
        const std::string path = profile_path();
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fa)) continue;
        const ULONGLONG st = ((ULONGLONG)fa.ftLastWriteTime.dwHighDateTime << 32 | fa.ftLastWriteTime.dwLowDateTime) ^ fa.nFileSizeLow;
        if (st == stamp) continue;
        Sleep(500);   // let the game finish writing
        rfach::Profile p;
        if (!rfach::load(path.c_str(), p)) continue;   // mid-write or not a profile: try again next time
        stamp = st;
        if (!have_rec) {   // first run with this: what the first readable save has is not news
            for (const auto& a : rfach::evaluate(p)) if (a.earned) seen.push_back(a.name);
            save_rec();
            have_rec = true;
            continue;
        }
        for (const auto& a : rfach::evaluate(p)) {
            if (!a.earned || is_seen(a.name)) continue;
            bool queued = false;
            for (const auto& u : unsaved) if (u.name == a.name) queued = true;
            if (queued) continue;
            if (const unsigned long long t = announce_achievement(a, false)) unsaved.push_back({ a.name, t });
            else stamp = ~0ULL;   // queue full: look again next time
        }
    }
}

struct TrophyInit {
    TrophyInit()
    {
        g_trophy_unlock_hook = hook;
        if (HANDLE h = CreateThread(nullptr, 0, achievement_thread, nullptr, 0, nullptr)) CloseHandle(h);
    }
} s_init;

} // namespace
