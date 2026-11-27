// port_ini_migrate.h -- settings.ini key renames shared by the three ports (identical file in R1, R2 and R3).
//
// The same setting has the same settings.ini key in every port. Keys that were renamed for that are listed in
// k_port_ini_renames (old -> new). port_ini_migrate_lines() runs when a launcher or the game reads settings.ini:
// for every old key that is in the file while its new key is not, it adds "new=value" right after the old line.
// The old line is kept (and read as a fallback through the in-memory migration) for one release, so a launcher or
// game of the previous release still finds its setting. Callers write the lines back when the count is > 0.
//
// Values are kept as they are: each port reads its own value set (for example R1 background=pause, R2/R3 "" for
// Pause and mute). Settings whose values mean the opposite in the ports are not renamed (R1 aimhold vs R2/R3
// aimtoggle).
#pragma once
#include <string>
#include <vector>
#include <cstring>

struct PortIniRename { const char* old_key; const char* new_key; };

static const PortIniRename k_port_ini_renames[] = {
    { "lang",               "language" },      // R1 (R2/R3: language)
    { "pad_p1",             "p1pad" },         // R1 (R2/R3: p1pad, like p2pad)
    { "dlss_preset",        "dlsspreset" },    // R1
    { "lod_bias",           "lodbias" },       // R1
    { "precompile_threads", "shaderthreads" }, // R1
    { "sensy",              "msensy" },        // R1 Vertical sensitivity
    { "ads",                "adssens" },       // R1 Zoomed / aiming sensitivity
    { "focus",              "background" },    // R1 In the background
    { "saves",              "savedir" },       // R1 Save location
    { "hud169",             "hudfix" },        // R1 Ultrawide HUD (env RFOM_HUD_169 -> RSX_HUD_FIX, old name still read)
};

// "  key = value" -> key / value (blanks around both trimmed). false: no '=' or an empty key (left as it is).
inline bool port_ini_split(const std::string& line, std::string& key, std::string& value)
{
    const size_t eq = line.find('=');
    if (eq == std::string::npos) return false;
    size_t a = 0, b = eq;
    while (a < b && (line[a] == ' ' || line[a] == '\t')) a++;
    while (b > a && (line[b - 1] == ' ' || line[b - 1] == '\t')) b--;
    if (a == b) return false;
    key = line.substr(a, b - a);
    size_t c = eq + 1, d = line.size();
    while (d > c && (line[d - 1] == '\r' || line[d - 1] == '\n' || line[d - 1] == ' ' || line[d - 1] == '\t')) d--;
    while (c < d && (line[c] == ' ' || line[c] == '\t')) c++;
    value = line.substr(c, d - c);
    return true;
}

// The new name of a renamed key, or nullptr.
inline const char* port_ini_new_key(const char* old_key)
{
    for (const auto& r : k_port_ini_renames)
        if (!strcmp(r.old_key, old_key)) return r.new_key;
    return nullptr;
}

// Returns how many keys were copied to their new name (0: nothing to write back).
inline int port_ini_migrate_lines(std::vector<std::string>& lines)
{
    int copied = 0;
    for (const auto& r : k_port_ini_renames) {
        int old_at = -1;
        bool has_new = false;
        std::string k, v, old_v;
        for (size_t i = 0; i < lines.size(); i++) {
            if (!port_ini_split(lines[i], k, v)) continue;
            if (k == r.new_key) { has_new = true; break; }
            if (old_at < 0 && k == r.old_key) { old_at = (int)i; old_v = v; }
        }
        if (has_new || old_at < 0) continue;
        lines.insert(lines.begin() + old_at + 1, std::string(r.new_key) + "=" + old_v);
        copied++;
    }
    return copied;
}
