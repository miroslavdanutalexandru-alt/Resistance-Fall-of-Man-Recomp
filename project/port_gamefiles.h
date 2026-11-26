// port_gamefiles.h -- the launcher's "Game files folder" setting (identical file in R1, R2 and R3).
//
// The game files folder is the player's copy of the disc (PS3_GAME\PARAM.SFO, PS3_GAME\USRDIR\...): the game's
// /dev_bdvd, plus the PS3 hard-drive folders the game writes into it (cache\<title id>, game\<title id>). Setup copies
// it to <port>\gamefiles. The launcher can point the port at another folder instead (settings.ini gamefiles=<path>,
// settings.bat PORT_GAMEFILES); empty = <port>\gamefiles.
//
// Which folder is used (run.bat, setup.ps1, the launcher, Setup.exe agree on this):
//   1. PS3_VFS_ROOT, when it is set in the environment (or by hand near the top of run.bat), wins;
//   2. else the launcher's setting (PORT_GAMEFILES);
//   3. else <port>\gamefiles.
// The path goes through settings.bat and run.bat (cmd) and the ANSI file functions, so it must be plain ASCII
// without % " ! (see port_gamefiles_path_problem).
#pragma once
#include <windows.h>
#include <string>
#include <cctype>
#include <cstdio>
#include <cstring>

static inline bool port_gamefiles_exists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static inline bool port_gamefiles_is_dir(const std::string& p)
{
    const DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// trailing slashes off (a drive root keeps its "X:\"), '/' -> '\'
static inline std::string port_gamefiles_clean(std::string p)
{
    for (auto& c : p) if (c == '/') c = '\\';
    while (p.size() > 3 && p.back() == '\\') p.pop_back();
    if (p.size() == 2 && p[1] == ':') p += '\\';
    return p;
}

static inline bool port_gamefiles_same(const std::string& a, const std::string& b)
{
    char fa[MAX_PATH * 2] = "", fb[MAX_PATH * 2] = "";
    const std::string ca = port_gamefiles_clean(a), cb = port_gamefiles_clean(b);
    if (!GetFullPathNameA(ca.c_str(), sizeof fa, fa, NULL)) snprintf(fa, sizeof fa, "%s", ca.c_str());
    if (!GetFullPathNameA(cb.c_str(), sizeof fb, fb, NULL)) snprintf(fb, sizeof fb, "%s", cb.c_str());
    return !_stricmp(port_gamefiles_clean(fa).c_str(), port_gamefiles_clean(fb).c_str());
}

// "" when the path can be stored, else why not
static inline std::string port_gamefiles_path_problem(const std::string& p)
{
    if (p.empty()) return "";
    const bool abs = (p.size() >= 3 && isalpha((unsigned char)p[0]) && p[1] == ':' && p[2] == '\\') || !p.compare(0, 2, "\\\\");
    if (!abs) return "The game files folder must be a full path (like E:\\Games\\Resistance\\gamefiles).";
    if (p.size() > 200) return "The path of the game files folder is too long (200 characters at most). Pick a shorter one.";
    for (unsigned char c : p)
        if (c < 0x20 || c > 0x7E || c == '%' || c == '"' || c == '!')
            return "The path of the game files folder has characters the game's start script cannot pass on (letters "
                   "outside English, %, \" or !). Rename the folder or pick another one, like E:\\Games\\Resistance\\gamefiles.";
    return "";
}

// what the player picked -> the game files folder: the folder that holds PS3_GAME. Picking the folder above it (with a
// gamefiles folder inside, like another port folder), PS3_GAME itself or its USRDIR is taken as meaning that one.
static inline std::string port_gamefiles_normalize(const std::string& picked)
{
    std::string p = port_gamefiles_clean(picked);
    if (p.empty() || port_gamefiles_is_dir(p + (p.back() == '\\' ? "" : "\\") + "PS3_GAME")) return p;
    if (port_gamefiles_is_dir(p + "\\gamefiles\\PS3_GAME")) return p + "\\gamefiles";
    auto leaf = [](const std::string& s) { const size_t k = s.find_last_of('\\'); return k == std::string::npos ? s : s.substr(k + 1); };
    auto parent = [](const std::string& s) { const size_t k = s.find_last_of('\\'); return k == std::string::npos || k < 3 ? s.substr(0, 3) : s.substr(0, k); };
    if (!_stricmp(leaf(p).c_str(), "USRDIR") && !_stricmp(leaf(parent(p)).c_str(), "PS3_GAME")) return parent(parent(p));
    if (!_stricmp(leaf(p).c_str(), "PS3_GAME")) return parent(p);
    return p;
}

// "" when dir is a complete game files folder, else what is wrong. required: the files the port needs, relative to
// the folder ('/' or '\'), nullptr-terminated (tools\installer\game_profile.psd1 Required).
static inline std::string port_gamefiles_problem(const std::string& dir, const char* const* required)
{
    if (dir.empty() || !port_gamefiles_is_dir(dir)) return "The folder does not exist (or the drive is not connected):\n" + dir;
    if (port_gamefiles_exists(port_gamefiles_clean(dir) + ".copying"))
        return "Setup did not finish copying the game into this folder (" + port_gamefiles_clean(dir) + ".copying is there). "
               "Run Setup again or pick a complete copy.";
    for (const char* const* r = required; r && *r; r++) {
        std::string rel = *r;
        for (auto& c : rel) if (c == '/') c = '\\';
        if (!port_gamefiles_exists(port_gamefiles_clean(dir) + "\\" + rel))
            return "This is not a complete copy of the game: " + rel + " is missing in\n" + dir + "\n\nPick the folder that "
                   "has PS3_GAME in it (your game files, copied from the disc by Setup or dumped for RPCS3).";
    }
    return "";
}

// PARAM.SFO's TITLE_ID ("" when unreadable)
static inline std::string port_gamefiles_title_id(const std::string& dir)
{
    std::string out;
    FILE* f = fopen((port_gamefiles_clean(dir) + "\\PS3_GAME\\PARAM.SFO").c_str(), "rb");
    if (!f) return out;
    unsigned char b[8192];
    const size_t n = fread(b, 1, sizeof b, f);
    fclose(f);
    auto le32 = [&](size_t o) { return (unsigned)b[o] | (unsigned)b[o + 1] << 8 | (unsigned)b[o + 2] << 16 | (unsigned)b[o + 3] << 24; };
    if (n < 20 || memcmp(b, "\0PSF", 4)) return out;
    const unsigned kt = le32(8), dt = le32(12), cnt = le32(16);
    for (unsigned i = 0; i < cnt && 20 + (size_t)i * 16 + 16 <= n; i++) {
        const size_t e = 20 + (size_t)i * 16;
        const size_t ko = kt + (size_t)(b[e] | b[e + 1] << 8), len = le32(e + 4), dof = dt + (size_t)le32(e + 12);
        if (ko >= n || dof + len > n) continue;
        if (std::string((const char*)b + ko, strnlen((const char*)b + ko, n - ko)) != "TITLE_ID") continue;
        out.assign((const char*)b + dof, strnlen((const char*)b + dof, len));
        break;
    }
    return out;
}

enum { PORT_GF_DEFAULT = 0, PORT_GF_SETTING = 1, PORT_GF_ENV = 2 };
// the folder the game uses: PS3_VFS_ROOT (environment) > the launcher's setting > <port>\gamefiles
static inline std::string port_gamefiles_effective(const std::string& port_dir, const std::string& setting, int* source = nullptr)
{
    char env[MAX_PATH * 2] = "";
    const DWORD n = GetEnvironmentVariableA("PS3_VFS_ROOT", env, sizeof env);
    if (n > 0 && n < sizeof env) { if (source) *source = PORT_GF_ENV; return port_gamefiles_clean(env); }
    if (!setting.empty()) { if (source) *source = PORT_GF_SETTING; return port_gamefiles_clean(setting); }
    if (source) *source = PORT_GF_DEFAULT;
    return port_gamefiles_clean(port_dir) + "\\gamefiles";
}
