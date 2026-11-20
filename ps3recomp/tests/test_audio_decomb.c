/* audio_decomb.h: the libmixer TYPE2A delayed side copy (R1 #10 "tin can"). */
#include <stdio.h>
#include <math.h>
#include "audio_decomb.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

#define BLK 256u
#define NB  300u
#define TOT (BLK * NB + 4096u)

static unsigned seed;
static float noise(void) { seed = seed * 1664525u + 1013904223u; return (float)((seed >> 8) & 0xFFFF) / 32768.0f - 1.0f; }

/* band-limited noise with a syllable-rate envelope: crude speech stand-in */
static void make_signal(float* x, unsigned n, unsigned s)
{
    float y = 0.0f;
    seed = s;
    for (unsigned i = 0; i < n; i++) {
        y += 0.3f * (noise() - y);
        x[i] = 0.3f * y * (0.6f + 0.4f * sinf((float)i * 0.0015f));
    }
}

static float xa[TOT], xb[TOT], xc[TOT];
static float blk[BLK * 8];

/* Feed blocks [b0,b1) where L=R=front*x(t), Ls=Rs=side*s(t). */
static int run(AudioDecomb* d, unsigned b0, unsigned b1, const float* fr, float fg,
               const float* sd, int sdelay, float sg)
{
    int changes = 0;
    for (unsigned b = b0; b < b1; b++) {
        for (unsigned i = 0; i < BLK; i++) {
            const unsigned t = 2048u + b * BLK + i;
            float* f = &blk[i * 8];
            for (unsigned c = 0; c < 8; c++) f[c] = 0.0f;
            f[0] = f[1] = fg * fr[t];
            f[4] = f[5] = sg * sd[t - sdelay];
        }
        const int ch = audio_decomb_block(d, blk, BLK, 8, 0.5f);
        if (ch) changes += ch;
    }
    return changes;
}

int main(void)
{
    AudioDecomb d;
    make_signal(xa, TOT, 1u);
    make_signal(xb, TOT, 77u);
    make_signal(xc, TOT, 991u);

    /* The R1 movie case: direct 0.5775, side = copy delayed 222 frames x 0.8775. */
    audio_decomb_init(&d, AUDIO_DECOMB_DEFAULT_LAG);
    CHECK(run(&d, 0, 40, xa, 0.5775f, xa, 222, 0.8775f) == 1, "delayed copy detected");
    CHECK(d.on && d.corr > 0.95, "corr %.3f", d.corr);
    CHECK(d.best_lag == 222, "best lag %u", d.best_lag);
    CHECK(d.w1 == 0.0f, "side pair fully faded out (w %.3f)", d.w1);
    /* expected compensation sqrt((0.5775^2 + 0.25*0.8775^2) / 0.5775^2) = 1.256 */
    CHECK(fabsf(d.g1 - 1.256f) < 0.03f, "loudness compensation %.3f", d.g1);

    /* Silence: decision held. */
    {
        static const float z[TOT];
        CHECK(run(&d, 40, 60, z, 1.0f, z, 0, 1.0f) == 0 && d.on, "silence keeps it on");
    }
    /* Sides become independent (a real 5.1 strip): folded again. */
    CHECK(run(&d, 60, 100, xa, 0.5775f, xb, 0, 0.8775f) == -1, "independent sides switch it off");
    CHECK(!d.on && d.w1 == 1.0f && fabsf(d.g1 - 1.0f) < 1e-3f, "w %.3f g %.3f", d.w1, d.g1);

    /* Genuine surrounds never trigger it. */
    audio_decomb_init(&d, AUDIO_DECOMB_DEFAULT_LAG);
    CHECK(run(&d, 0, NB, xa, 0.6f, xc, 0, 0.5f) == 0 && !d.on, "uncorrelated surrounds keep the old fold");
    CHECK(d.w0 == 1.0f && d.w1 == 1.0f && d.g1 == 1.0f, "unchanged gains");

    /* Same-time (zero-lag) copy, e.g. an upmixed mono ambience: not a comb from a delay. */
    audio_decomb_init(&d, AUDIO_DECOMB_DEFAULT_LAG);
    CHECK(run(&d, 0, 60, xa, 0.6f, xa, 0, 0.5f) == 0 && !d.on, "zero-lag copy is left alone");

    /* A different delay is outside the +/-4 search unless the lag is overridden. */
    audio_decomb_init(&d, AUDIO_DECOMB_DEFAULT_LAG);
    CHECK(run(&d, 0, 60, xa, 0.6f, xa, 300, 0.8f) == 0 && !d.on, "lag 300 not matched at 222");
    audio_decomb_init(&d, 300);
    CHECK(run(&d, 0, 60, xa, 0.6f, xa, 300, 0.8f) == 1 && d.best_lag == 300, "lag override");
    /* Within the search window. */
    audio_decomb_init(&d, AUDIO_DECOMB_DEFAULT_LAG);
    CHECK(run(&d, 0, 60, xa, 0.6f, xa, 225, 0.8f) == 1 && d.best_lag == 225, "lag 225 found (%u)", d.best_lag);

    /* ~1 s of silent sides (front-only audio after the movie): detection and
     * makeup gain are cleared, gradually; re-detection needs fresh evidence. */
    audio_decomb_init(&d, AUDIO_DECOMB_DEFAULT_LAG);
    {
        static const float z[TOT];
        CHECK(run(&d, 0, 40, xa, 0.5775f, xa, 222, 0.8775f) == 1 && d.g1 > 1.2f, "detected before the reset");
        CHECK(run(&d, 40, 40 + AUDIO_DECOMB_SILENT_RESET - 1, xb, 0.6f, z, 0, 0.0f) == 0 && d.on && d.g1 > 1.2f,
              "short silence still held");
        CHECK(run(&d, 40 + AUDIO_DECOMB_SILENT_RESET - 1, 40 + AUDIO_DECOMB_SILENT_RESET, xb, 0.6f, z, 0, 0.0f) == -1 && !d.on,
              "cleared after ~1 s of silent sides");
        CHECK(d.g1 > 1.0f && d.g1 - 1.0f < 0.3f, "gain ramps, no step (%.3f)", d.g1);
        run(&d, 40 + AUDIO_DECOMB_SILENT_RESET, 40 + AUDIO_DECOMB_SILENT_RESET + 12, xb, 0.6f, z, 0, 0.0f);
        CHECK(fabsf(d.g1 - 1.0f) < 1e-3f && d.w1 == 1.0f, "no leftover boost (g %.4f)", d.g1);
        const unsigned b = 40 + AUDIO_DECOMB_SILENT_RESET + 12;
        CHECK(run(&d, b, b + AUDIO_DECOMB_MIN_BLOCKS - 1, xa, 0.5775f, xa, 222, 0.8775f) == 0 && !d.on,
              "re-detection waits for fresh evidence");
        CHECK(run(&d, b + AUDIO_DECOMB_MIN_BLOCKS - 1, b + 50, xa, 0.5775f, xa, 222, 0.8775f) == 1 && d.on,
              "re-detected");
    }

    /* Fade is gradual (no click): at most AUDIO_DECOMB_RAMP per block. */
    audio_decomb_init(&d, AUDIO_DECOMB_DEFAULT_LAG);
    {
        float prev = 1.0f; int ok = 1;
        for (unsigned b = 0; b < 40; b++) {
            run(&d, b, b + 1, xa, 0.5775f, xa, 222, 0.8775f);
            if (d.w0 != prev || prev - d.w1 > AUDIO_DECOMB_RAMP + 1e-6f) ok = 0;
            prev = d.w1;
        }
        CHECK(ok && prev == 0.0f, "ramped fade");
    }

    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("audio_decomb: all passed\n");
    return 0;
}
