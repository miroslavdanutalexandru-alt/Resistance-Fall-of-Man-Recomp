/*
 * test_audio_chmap - PS3 8ch (L R C LFE Ls Rs Lb Rb) -> host speaker layouts.
 * Build: clang-cl /nologo test_audio_chmap.c  (exit 0 = ALL PASSED)
 */
#include "../audio_chmap.h"
#include <stdio.h>
#include <math.h>

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL: " __VA_ARGS__); printf("\n"); g_fail++; } } while (0)

/* the single host channel source `s` lands on at gain 1 (-1 = none or several) */
static int dest(const AudioChMap* cm, int s)
{
    int d = -1;
    for (unsigned o = 0; o < cm->out_ch; o++) {
        if (cm->m[s][o] == 0.0f) continue;
        if (d >= 0 || fabsf(cm->m[s][o] - 1.0f) > 1e-6f) return -1;
        d = (int)o;
    }
    return d;
}

int main(void)
{
    AudioChMap cm;

    /* 7.1 = FL FR FC LFE BL BR SL SR: PS3 surrounds -> sides, PS3 backs -> backs */
    audio_chmap_build(&cm, AUDIO_MASK_7POINT1, 0);
    CHECK(cm.out_ch == 8, "7.1: %u channels", cm.out_ch);
    { static const int want[8] = { 0, 1, 2, 3, 6, 7, 4, 5 };
      for (int s = 0; s < 8; s++) CHECK(dest(&cm, s) == want[s], "7.1: PS3 ch %d -> %d, want %d", s, dest(&cm, s), want[s]); }
    audio_chmap_build(&cm, AUDIO_MASK_7POINT1, 1);   /* PORT_AUDIO_SWAP_SURROUND=1 */
    { static const int want[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
      for (int s = 0; s < 8; s++) CHECK(dest(&cm, s) == want[s], "7.1 swapped: PS3 ch %d -> %d, want %d", s, dest(&cm, s), want[s]); }

    /* 5.1 (back) = FL FR FC LFE BL BR: both PS3 pairs on the BL/BR pair */
    audio_chmap_build(&cm, AUDIO_MASK_5POINT1, 0);
    CHECK(cm.out_ch == 6, "5.1: %u channels", cm.out_ch);
    { static const int want[8] = { 0, 1, 2, 3, 4, 5, 4, 5 };
      for (int s = 0; s < 8; s++) CHECK(dest(&cm, s) == want[s], "5.1: PS3 ch %d -> %d, want %d", s, dest(&cm, s), want[s]); }

    /* 5.1 (side) = FL FR FC LFE SL SR */
    audio_chmap_build(&cm, AUDIO_MASK_5POINT1S, 0);
    CHECK(cm.out_ch == 6, "5.1 side: %u channels", cm.out_ch);
    { static const int want[8] = { 0, 1, 2, 3, 4, 5, 4, 5 };
      for (int s = 0; s < 8; s++) CHECK(dest(&cm, s) == want[s], "5.1 side: PS3 ch %d -> %d, want %d", s, dest(&cm, s), want[s]); }

    /* quad = FL FR BL BR: centre and LFE folded into the fronts like the stereo fold */
    audio_chmap_build(&cm, AUDIO_MASK_QUAD, 0);
    CHECK(cm.out_ch == 4, "quad: %u channels", cm.out_ch);
    CHECK(fabsf(cm.m[2][0] - 0.707f) < 1e-6f && fabsf(cm.m[2][1] - 0.707f) < 1e-6f, "quad: centre not folded");
    CHECK(fabsf(cm.m[3][0] - 0.5f) < 1e-6f && fabsf(cm.m[3][1] - 0.5f) < 1e-6f, "quad: LFE not folded");
    CHECK(dest(&cm, 4) == 2 && dest(&cm, 7) == 3, "quad: surrounds not on the back pair");

    /* mono / stereo ports */
    { float rows[8][8];
      audio_chmap_build(&cm, AUDIO_MASK_7POINT1, 0);
      audio_chmap_rows(&cm, 1, rows);
      CHECK(rows[0][0] == 1.0f && rows[0][1] == 1.0f && rows[0][2] == 0.0f, "mono port: not on both fronts");
      audio_chmap_rows(&cm, 2, rows);
      CHECK(rows[0][0] == 1.0f && rows[1][1] == 1.0f && rows[2][2] == 0.0f && rows[4][6] == 0.0f, "stereo port rows"); }

    /* layout choice */
    { unsigned m = 0;
      CHECK(audio_chmap_pick(2, AUDIO_MASK_STEREO, 0, &m) == 2 && m == AUDIO_MASK_STEREO, "pick: stereo device");
      CHECK(audio_chmap_pick(8, AUDIO_MASK_7POINT1, 0, &m) == 8 && m == AUDIO_MASK_7POINT1, "pick: 7.1 device");
      CHECK(audio_chmap_pick(6, AUDIO_MASK_5POINT1S, 0, &m) == 6 && m == AUDIO_MASK_5POINT1S, "pick: 5.1 side device");
      CHECK(audio_chmap_pick(6, 0, 0, &m) == 6 && m == AUDIO_MASK_5POINT1, "pick: 6ch without mask");
      CHECK(audio_chmap_pick(1, 0x4, 0, &m) == 2 && m == AUDIO_MASK_STEREO, "pick: mono device -> stereo");
      CHECK(audio_chmap_pick(7, 0x13F, 0, &m) == 2, "pick: unknown speaker bit -> stereo");
      CHECK(audio_chmap_pick(2, AUDIO_MASK_STEREO, 8, &m) == 8 && m == AUDIO_MASK_7POINT1, "pick: forced 7.1 on stereo");
      CHECK(audio_chmap_pick(6, AUDIO_MASK_5POINT1S, 6, &m) == 6 && m == AUDIO_MASK_5POINT1S, "pick: forced 5.1 keeps the device mask");
      CHECK(audio_chmap_pick(8, AUDIO_MASK_7POINT1, 2, &m) == 2 && m == AUDIO_MASK_STEREO, "pick: forced stereo"); }

    /* volume / mono */
    { float f[8] = { 0.2f, 0.4f, 0.6f, 0.8f, 0.1f, 0.1f, 0.1f, 0.1f };
      audio_chmap_build(&cm, AUDIO_MASK_7POINT1, 0);
      audio_chmap_apply_volume(f, 1, &cm, 0.5f, 0);
      CHECK(fabsf(f[0] - 0.1f) < 1e-6f && fabsf(f[3] - 0.4f) < 1e-6f, "volume not applied");
      float g[8] = { 0.2f, 0.4f, 0.2f, 0.8f, 0.2f, 0.0f, 0.0f, 0.2f };
      audio_chmap_apply_volume(g, 1, &cm, 1.0f, 1);
      CHECK(fabsf(g[0] - g[1]) < 1e-6f && fabsf(g[0] - 0.6f) < 1e-6f, "mono: fronts %.3f %.3f", g[0], g[1]);
      CHECK(g[2] == 0.0f && g[4] == 0.0f && g[7] == 0.0f && g[3] == 0.8f, "mono: others not cleared / LFE touched"); }

    printf(g_fail ? "FAILED (%d)\n" : "ALL PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
