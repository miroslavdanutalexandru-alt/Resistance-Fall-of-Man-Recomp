/* Resistance: Fall of Man PC port: optional single-player cheats (God mode,
 * Infinite ammo, Infinite grenades). All off by default; they only touch the
 * local players (the heroes the pad / mouse-look code runs for), never enemies.
 *
 *   God mode           the damage function func_0029EEC0 (object r3, damage f1)
 *                      takes health at obj+0x2D10 (float, max obj+0x2D14) down by
 *                      the damage; the hook right after the write (0x0029EF10,
 *                      tools/hooks/game.json) puts a local hero's old health back.
 *                      Every frame the hero's health is also topped up to the max
 *                      (rfom_mouselook_pre, stubs.cpp), unless it is already 0.
 *   Infinite ammo      the fire functions func_00339A40 (primary, weapon r31, clip
 *                      int16 at weapon+0x3BC) and func_00339B84 (alt fire, weapon r3,
 *                      weapon+0x3C0) subtract the shot's cost; the hooks keep the old
 *                      count when the weapon's owner (weapon+0x24) is a local hero.
 *   Infinite grenades  the inventory add func_002A6EB8 (player r3, item type r4,
 *                      delta r5; count byte at player+0x679+type): a -1 for a local
 *                      hero (a thrown grenade) becomes 0.
 *
 * The hook points are the ones from GitHub R1 #24 / #6 (the Artemis codes credited
 * to bungholio / hyper2k: health 0x0029EF08, clip 0x00339B00, alt 0x00339BF0,
 * grenades 0x002A6F10), checked against the lifted code. BCUS98107 1.00.
 *
 * Turned on by the launcher / F9 menu rows (PORT_CHEAT_GODMODE, PORT_CHEAT_AMMO,
 * PORT_CHEAT_GRENADES = 1) or, with PORT_CHEAT_KEYS=1, Ctrl+F1 (God mode) and
 * Ctrl+F2 (ammo and grenades) (rfom_pcextra.cpp). Once any has been on, trophy
 * unlocks are not recorded until the game is restarted (g_trophy_unlocks_blocked,
 * sceNpTrophy.c). A benchmark run (RFOM_BENCH) never cheats. Every change is logged. */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include "ppu_recomp.h"
#include <windows.h>

extern "C" volatile int g_trophy_unlocks_blocked;   /* sceNpTrophy.c */

extern "C" volatile int g_rfom_cheat_god = 0;
extern "C" volatile int g_rfom_cheat_ammo = 0;
extern "C" volatile int g_rfom_cheat_gren = 0;

static int env_on(const char* n) { const char* e = getenv(n); return e && *e && strcmp(e, "0") != 0; }
/* 1 = benchmark run (RFOM_BENCH): no cheats at all */
static int cheats_blocked(void)
{
    static int why = -1;
    if (why < 0) why = env_on("RFOM_BENCH") ? 1 : 0;
    return why;
}
static void block_trophies(void)
{
    if (g_trophy_unlocks_blocked) return;
    g_trophy_unlocks_blocked = 1;
    fprintf(stderr, "[cheat] trophies are off until the game is restarted\n");
}
static void cheats_init(void);
static const char* const s_name[3] = { "god mode", "infinite ammo", "infinite grenades" };
static volatile int& cheat_flag(int which) { return which == 0 ? g_rfom_cheat_god : which == 1 ? g_rfom_cheat_ammo : g_rfom_cheat_gren; }

/* 0 = God mode, 1 = Infinite ammo, 2 = Infinite grenades; on: 1 / 0, -1 = toggle.
 * Returns the new state, or -1 in a benchmark run. Any thread. */
extern "C" int rfom_cheat_set(int which, int on)
{
    if (which < 0 || which > 2) return 0;
    cheats_init();
    if (cheats_blocked()) {
        static int told = 0;
        if (on && !told++) fprintf(stderr, "[cheat] benchmark run: cheats are off\n");
        return -1;
    }
    volatile int& v = cheat_flag(which);
    if (on < 0) on = !v;
    on = on ? 1 : 0;
    if (on) block_trophies();
    if (v != on) fprintf(stderr, "[cheat] %s %s\n", s_name[which], on ? "on" : "off");
    v = on;
    return on;
}
static void cheats_init(void)
{
    static int done = 0;
    if (done) return;
    done = 1;
    if (env_on("PORT_CHEAT_GODMODE")) rfom_cheat_set(0, 1);
    if (env_on("PORT_CHEAT_AMMO")) rfom_cheat_set(1, 1);
    if (env_on("PORT_CHEAT_GRENADES")) rfom_cheat_set(2, 1);
}

/* The local heroes: rfom_mouselook_pre runs once a frame for each (split screen:
 * both). An object counts as local while it was seen in the last 5 s. Main thread. */
static struct { uint32_t ea; ULONGLONG at; } s_local[4];
static int is_local(uint32_t obj)
{
    if (!obj) return 0;
    const ULONGLONG now = GetTickCount64();
    for (auto& l : s_local) if (l.ea == obj && now - l.at < 5000) return 1;
    return 0;
}
static float rdf(uint32_t ea) { const uint32_t u = vm_read32(ea); float f; memcpy(&f, &u, 4); return f; }
static void wrf(uint32_t ea, float f) { uint32_t u; memcpy(&u, &f, 4); vm_write32(ea, u); }

/* stubs.cpp rfom_mouselook_pre: every frame, per local hero */
extern "C" void rfom_cheats_player(uint32_t player)
{
    if (!player) return;
    cheats_init();
    const ULONGLONG now = GetTickCount64();
    int k = -1, old = 0;
    for (int i = 0; i < 4; i++) {
        if (s_local[i].ea == player) { k = i; break; }
        if (s_local[i].at < s_local[old].at) old = i;
    }
    if (k < 0) {
        k = old; s_local[k].ea = player;
        if (g_rfom_cheat_god || g_rfom_cheat_ammo || g_rfom_cheat_gren)
            fprintf(stderr, "[cheat] local hero 0x%08X\n", player);
    }
    s_local[k].at = now;
    if (g_rfom_cheat_god) {
        const float h = rdf(player + 0x2D10), mx = rdf(player + 0x2D14);
        if (h > 0.0f && mx > 0.0f && mx < 100000.0f && h < mx) wrf(player + 0x2D10, mx);
    }
}
/* hook in func_0029EEC0 at 0x0029EF10: the damage has been written; old = the health before */
extern "C" void rfom_cheat_hurt(uint32_t obj, double old)
{
    if (!g_rfom_cheat_god || !is_local(obj)) return;
    const float cur = rdf(obj + 0x2D10);
    if (cur < (float)old) {
        wrf(obj + 0x2D10, (float)old);
        static int n = 0;
        if (n++ < 5) fprintf(stderr, "[cheat] god mode: hero 0x%08X kept %.1f (hit to %.1f)\n", obj, old, (double)cur);
    }
}
/* hooks in func_00339A40 (0x00339B00) / func_00339B84 (0x00339BF0): 1 = keep the clip */
extern "C" int rfom_cheat_ammo(uint32_t weapon)
{
    if (!g_rfom_cheat_ammo || !weapon) return 0;
    if (!is_local(vm_read32(weapon + 0x24))) return 0;
    static int n = 0;
    if (!n++) fprintf(stderr, "[cheat] infinite ammo: weapon 0x%08X (hero 0x%08X)\n", weapon, vm_read32(weapon + 0x24));
    return 1;
}
/* hook in func_002A6EB8 before the add (0x002A6EF8): 1 = drop this -1 */
extern "C" int rfom_cheat_item(uint32_t player, uint32_t type, int32_t delta)
{
    if (!g_rfom_cheat_gren || delta != -1 || !is_local(player)) return 0;
    static int n = 0;
    if (n++ < 5) fprintf(stderr, "[cheat] infinite grenades: hero 0x%08X item %u kept\n", player, type);
    return 1;
}
