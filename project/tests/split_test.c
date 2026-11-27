/* rfom_split.h: split screen is two or more local player objects running their look
 * code within the window; one player (or a player that stopped) is not. */
#include <stdio.h>
#include <string.h>
#include "rfom_split.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void test_single_player(void)
{
    rfom_split_slot s[4]; memset(s, 0, sizeof s);
    for (unsigned long long t = 1000; t < 20000; t += 16)
        CHECK(rfom_split_note(s, 0x10001000u, t, 1000u) == 1);
    CHECK(rfom_split_note(s, 0u, 20000, 1000u) == 1);   /* no player: counted, not added */
}

static void test_two_players(void)
{
    rfom_split_slot s[4]; memset(s, 0, sizeof s);
    unsigned long long t = 1000;
    CHECK(rfom_split_note(s, 0x10001000u, t, 1000u) == 1);
    CHECK(rfom_split_note(s, 0x10002000u, t, 1000u) == 2);
    for (int i = 0; i < 300; i++) {
        t += 16;
        CHECK(rfom_split_note(s, 0x10001000u, t, 1000u) == 2);
        CHECK(rfom_split_note(s, 0x10002000u, t, 1000u) == 2);
    }
    /* back to one player: the other drops out after the window */
    const unsigned long long t2 = t;
    while (t < t2 + 990) { t += 16; rfom_split_note(s, 0x10001000u, t, 1000u); }
    CHECK(rfom_split_note(s, 0x10001000u, t2 + 1000, 1000u) == 1);
}

static void test_slot_reuse(void)
{
    rfom_split_slot s[4]; memset(s, 0, sizeof s);
    for (unsigned p = 1; p <= 6; p++) rfom_split_note(s, 0x10000000u + p, 1000 + p * 2000ull, 1000u);
    /* the last one only: the stale ones were reused, none counted */
    CHECK(rfom_split_note(s, 0x10000006u, 13000, 1000u) == 1);
    CHECK(rfom_split_note(s, 0x10000007u, 13010, 1000u) == 2);
}

static void test_state_menus_keep_it(void)
{
    rfom_split_state st; memset(&st, 0, sizeof st);
    const unsigned W = 1000u, SOLO = 60000u;
    unsigned long long t = 1000;
    CHECK(!rfom_split_update(&st, 0x10001000u, t, W, SOLO));   /* single player */
    CHECK(rfom_split_update(&st, 0x10002000u, t, W, SOLO));    /* player 2 joins */
    for (int i = 0; i < 100; i++) { t += 16; rfom_split_update(&st, 0x10001000u, t, W, SOLO); CHECK(rfom_split_update(&st, 0x10002000u, t, W, SOLO)); }
    /* pause menu: nobody runs for 2 minutes, then both again in the same frame */
    t += 120000;
    CHECK(rfom_split_update(&st, 0x10001000u, t, W, SOLO));    /* alone for now, but not for a minute */
    CHECK(rfom_split_update(&st, 0x10002000u, t, W, SOLO));
    /* player 2 on a turret for 30 s (their look code stops): still split */
    const unsigned long long t0 = t;
    while (t < t0 + 30000) { t += 16; CHECK(rfom_split_update(&st, 0x10001000u, t, W, SOLO)); }
    CHECK(rfom_split_update(&st, 0x10002000u, t, W, SOLO));
    /* player 2 on a turret for 10 s, then a 2-minute pause, then player 1 alone again:
     * the pause isn't time alone, so still split */
    const unsigned long long t2 = t;
    while (t < t2 + 10000) { t += 16; rfom_split_update(&st, 0x10001000u, t, W, SOLO); }
    t += 120000;
    for (int i = 0; i < 60; i++) { t += 16; CHECK(rfom_split_update(&st, 0x10001000u, t, W, SOLO)); }
    CHECK(rfom_split_update(&st, 0x10002000u, t, W, SOLO));
    /* back to single player: off once player 1 has run alone for a minute */
    const unsigned long long t1 = t;
    while (t < t1 + 62000 && rfom_split_update(&st, 0x10001000u, t, W, SOLO)) t += 16;
    CHECK(t >= t1 + 60000 && t < t1 + 61500);
    CHECK(!rfom_split_update(&st, 0x10001000u, t + 16, W, SOLO));
}

int main(void)
{
    test_single_player();
    test_two_players();
    test_slot_reuse();
    test_state_menus_keep_it();
    if (fails) { fprintf(stderr, "split_test: %d failure(s)\n", fails); return 1; }
    printf("split_test: all passed\n");
    return 0;
}
