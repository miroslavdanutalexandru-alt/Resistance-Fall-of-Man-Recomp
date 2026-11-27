/* Tests for ps3emu/port_pc_extras2.h (benchmark statistics, crosshair, Steam Deck, window position, level-switch focus). */
#include <stdio.h>
#include <string.h>
#include "ps3emu/port_pc_extras2.h"

static int s_fail = 0, s_n = 0;
#define CHECK(c) do { s_n++; if (!(c)) { s_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define NEAR(a, b, e) ((a) - (b) < (e) && (b) - (a) < (e))

int main(void)
{
    /* 1. benchmark: 1000 frames at 10 ms with 10 hitches of 50 ms */
    static float ft[1000], scratch[1000];
    for (int i = 0; i < 1000; i++) ft[i] = (i % 100 == 50) ? 50.0f : 10.0f;
    PcxBenchStats st;
    pcx_bench_stats(ft, 1000, 0, scratch, &st);
    CHECK(st.frames == 1000);
    CHECK(NEAR(st.secs, 10.4, 1e-6));
    CHECK(NEAR(st.avg_fps, 1000 / 10.4, 1e-6));
    CHECK(NEAR(st.median_ms, 10.0, 1e-6));
    CHECK(NEAR(st.max_ms, 50.0, 1e-6));
    CHECK(NEAR(st.low1_fps, 20.0, 1e-6));        /* slowest 10 frames = the hitches */
    CHECK(NEAR(st.low01_fps, 20.0, 1e-6));       /* slowest frame */
    CHECK(st.stutters == 10);
    CHECK(ft[50] == 50.0f);                      /* input untouched */
    /* the wall time given wins over the sum of the frame times */
    pcx_bench_stats(ft, 1000, 20.0, scratch, &st);
    CHECK(NEAR(st.avg_fps, 50.0, 1e-6));
    /* a steady slow scene is not stutter; a gradual ramp is not either */
    for (int i = 0; i < 1000; i++) ft[i] = i < 500 ? 10.0f : 30.0f;
    CHECK(pcx_bench_stutters(ft, 1000) == 1);    /* only the step itself */
    for (int i = 0; i < 1000; i++) ft[i] = 10.0f + i * 0.02f;
    CHECK(pcx_bench_stutters(ft, 1000) == 0);
    /* small absolute hitches at very high fps (2 ms -> 5 ms) do not count */
    for (int i = 0; i < 100; i++) ft[i] = (i == 60) ? 5.0f : 2.0f;
    CHECK(pcx_bench_stutters(ft, 100) == 0);
    /* empty / one frame */
    pcx_bench_stats(ft, 0, 0, scratch, &st);
    CHECK(st.frames == 0 && st.avg_fps == 0);
    ft[0] = 16.0f;
    pcx_bench_stats(ft, 1, 0, scratch, &st);
    CHECK(NEAR(st.low1_fps, 62.5, 1e-6) && NEAR(st.p99_ms, 16.0, 1e-6) && st.stutters == 0);
    /* the hotkey */
    CHECK(pcx_bench_key(NULL) == 0x7A && pcx_bench_key("") == 0x7A);
    CHECK(pcx_bench_key("off") == 0 && pcx_bench_key("0") == 0);
    CHECK(pcx_bench_key("F6") == 0x75 && pcx_bench_key("f12") == 0x7B && pcx_bench_key("F24") == 0x87);
    CHECK(pcx_bench_key("F25") == 0x7A && pcx_bench_key("junk") == 0x7A);

    /* 2. crosshair */
    CHECK(pcx_xhair_rgb(NULL) == 0xF0F0F0 && pcx_xhair_rgb("") == 0xF0F0F0 && pcx_xhair_rgb("nope") == 0xF0F0F0);
    CHECK(pcx_xhair_rgb("Green") == 0x00FF40 && pcx_xhair_rgb("yellow") == 0xFFFF00);
    {   /* never the colour key */
        const char* all[] = { "white", "green", "yellow", "cyan", "red", "pink", "orange" };
        for (int i = 0; i < 7; i++) CHECK(pcx_xhair_rgb(all[i]) != 0xFF00FF);
    }
    CHECK(pcx_xhair_scale(NULL) == 100 && pcx_xhair_scale("") == 100 && pcx_xhair_scale("small") == 100);
    CHECK(pcx_xhair_scale("medium") == 150 && pcx_xhair_scale("LARGE") == 200 && pcx_xhair_scale("huge") == 300);
    CHECK(pcx_xhair_scale("175") == 175 && pcx_xhair_scale("10") == 100 && pcx_xhair_scale("999") == 100);

    /* 3. Steam Deck */
    CHECK(pcx_is_deck_board("Jupiter") && pcx_is_deck_board("galileo"));
    CHECK(!pcx_is_deck_board(NULL) && !pcx_is_deck_board("") && !pcx_is_deck_board("ROG Ally"));

    /* 4. window position */
    PcxWinPos p, q;
    CHECK(pcx_winpos_parse("100 200 1296 759 1280x720", &p));
    CHECK(p.x == 100 && p.y == 200 && p.w == 1296 && p.h == 759 && !strcmp(p.base, "1280x720"));
    CHECK(pcx_winpos_same_base(&p, "1280x720") && !pcx_winpos_same_base(&p, "1920x1080") && !pcx_winpos_same_base(&p, NULL));
    CHECK(pcx_winpos_parse("-5 -5 800 600", &q) && !strcmp(q.base, "-") && pcx_winpos_same_base(&q, ""));
    CHECK(!pcx_winpos_parse("1 2 3", &q) && !pcx_winpos_parse(NULL, &q) && !pcx_winpos_parse("0 0 50 50 -", &q));
    {
        char line[96]; pcx_winpos_format(&p, line, sizeof line);
        CHECK(!strcmp(line, "100 200 1296 759 1280x720\n"));
        CHECK(pcx_winpos_parse(line, &q) && q.x == p.x && q.h == p.h && !strcmp(q.base, p.base));
    }
    {
        const int mons[2][4] = { { 0, 0, 1920, 1040 }, { 1920, 0, 4480, 1400 } };
        CHECK(pcx_winpos_visible(&p, mons, 2));
        p.x = 3000; p.y = 100; CHECK(pcx_winpos_visible(&p, mons, 2));         /* second monitor */
        CHECK(!pcx_winpos_visible(&p, mons, 1));                               /* ... unplugged */
        p.x = -1250; p.y = 10; CHECK(!pcx_winpos_visible(&p, mons, 2));        /* only 46 px of the title on screen */
        p.x = 100; p.y = 1020; CHECK(!pcx_winpos_visible(&p, mons, 1));        /* title bar below the work area */
        p.y = -20; CHECK(!pcx_winpos_visible(&p, mons, 1));                    /* title bar above the top */
        p.y = 0; CHECK(pcx_winpos_visible(&p, mons, 1));
    }

    {
        const int work[4] = { 0, 0, 1920, 1040 };
        PcxWinPos f = { 1700, 900, 1296, 759, "-" };
        pcx_winpos_fit(&f, work);                                              /* off the bottom-right */
        CHECK(f.x == 624 && f.y == 281 && f.w == 1296 && f.h == 759);
        f.x = 50; f.y = 20; f.w = 2576; f.h = 1479; pcx_winpos_fit(&f, work);  /* bigger than the screen */
        CHECK(f.x == 0 && f.y == 0 && f.w == 1920 && f.h == 1040);
        f.x = -300; f.y = -10; f.w = 800; f.h = 600; pcx_winpos_fit(&f, work);
        CHECK(f.x == 0 && f.y == 0 && f.w == 800 && f.h == 600);
        const int second[4] = { 1920, 0, 4480, 1400 };
        f.x = 4000; f.y = 100; pcx_winpos_fit(&f, second);
        CHECK(f.x == 3680 && f.y == 100);
        f.x = 2000; f.y = 200; pcx_winpos_fit(&f, second);                     /* already inside: unchanged */
        CHECK(f.x == 2000 && f.y == 200 && f.w == 800 && f.h == 600);
    }

    /* level switch: the black cover in the foreground is not "in the background"
     * (focus pause would stop flips, the cover waits for flips -> black screen) */
    CHECK(pcx_focus_background(1, 0, 0, 1) == 0);
    CHECK(pcx_focus_background(1, 0, 0, 0) == 1);     /* another app: pause */
    CHECK(pcx_focus_background(1, 1, 0, 0) == 0);
    CHECK(pcx_focus_background(1, 0, 1, 1) == 1);     /* minimised wins */
    CHECK(pcx_focus_background(0, 0, 1, 0) == 0);     /* no window yet */
    CHECK(pcx_cover_release_due(1, 0, 500, 90000) == 0);
    CHECK(pcx_cover_release_due(2, 0, 500, 90000) == 1);
    CHECK(pcx_cover_release_due(0, 1, 500, 90000) == 1);   /* paused: never flips, release now */
    CHECK(pcx_cover_release_due(0, 0, 90000, 90000) == 1);
    CHECK(pcx_cover_release_due(0u - 1u, 0, 0, 90000) == 1);  /* counter wrap reads as many flips */

    printf("%d / %d checks passed\n", s_n - s_fail, s_n);
    return s_fail ? 1 : 0;
}
