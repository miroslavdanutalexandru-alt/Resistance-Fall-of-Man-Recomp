/* Split-screen co-op detection (GitHub #13, #25). The game runs its look code
 * once per local player object every frame; two or more distinct player objects
 * seen within window_ms means split screen. Pure so it can be unit-tested. */
#pragma once
#include <stdint.h>

typedef struct { uint32_t obj; unsigned long long seen; } rfom_split_slot;

/* Note that `player` ran at now_ms; returns how many distinct players ran within
 * window_ms (this one included). Four slots, the stalest is reused. */
static inline int rfom_split_note(rfom_split_slot s[4], uint32_t player, unsigned long long now_ms, unsigned window_ms)
{
    if (player) {
        int me = -1, old = 0;
        for (int i = 0; i < 4; i++) {
            if (s[i].obj == player) me = i;
            if (s[i].seen < s[old].seen) old = i;
        }
        if (me < 0) { me = old; s[me].obj = player; }
        s[me].seen = now_ms;
    }
    int n = 0;
    for (int i = 0; i < 4; i++)
        if (s[i].obj && now_ms >= s[i].seen && now_ms - s[i].seen < window_ms) n++;
    return n;
}

/* Split screen still on: once two players have been seen together it stays on until
 * exactly one player has run alone for solo_ms of play. Menus and loads (no player
 * runs the look code at all) keep the current state and don't count as time alone --
 * the game still draws both views behind the pause menu. Returns the state. */
typedef struct { rfom_split_slot s[4]; unsigned long long last, solo_play; int on; } rfom_split_state;
static inline int rfom_split_update(rfom_split_state* st, uint32_t player, unsigned long long now_ms,
                                    unsigned window_ms, unsigned solo_ms)
{
    const int n = rfom_split_note(st->s, player, now_ms, window_ms);
    const unsigned long long gap = (st->last && now_ms >= st->last) ? now_ms - st->last : 0;
    st->last = now_ms;
    if (n > 1) { st->on = 1; st->solo_play = 0; }
    else if (st->on && n == 1) {
        if (gap <= window_ms) st->solo_play += gap;   /* a longer gap was a menu / load */
        if (st->solo_play >= solo_ms) { st->on = 0; st->solo_play = 0; }
    }
    return st->on;
}
