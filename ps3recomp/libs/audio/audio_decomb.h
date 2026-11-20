/* Resistance port: "tin can" fix for the libmixer movie port (port 1).
 *
 * R1 plays its 2-channel movie audio through a libmixer TYPE2A channel strip
 * at position 0.3 (func_005FA2B0). That strip type is a surround spreader:
 * it writes the input to L/R (x 0.5775) AND a copy delayed by 222 frames
 * (111 two-frame vectors, func_006241F0; 4.6 ms) to Ls/Rs (x 0.8775). On a
 * real 5.1 system the delay is a Haas widening. Folded back into stereo it is
 * L = 0.58 x(t) + 0.44 x(t - 222): a comb filter, notches every 216 Hz, the
 * hollow/doubled "tin can" voice.
 *
 * This detects that case -- side pair == front pair delayed by ~222 frames --
 * and fades the side pair out of the stereo fold, compensating the lost
 * loudness. Genuine 5.1 surrounds (TYPE6A strips) do not correlate with the
 * delayed fronts and keep the normal fold. Pure, header-only, testable. */
#ifndef AUDIO_DECOMB_H
#define AUDIO_DECOMB_H

#include <math.h>
#include <string.h>

#define AUDIO_DECOMB_DEFAULT_LAG 222u
#define AUDIO_DECOMB_SEARCH      4u     /* +/- frames around the lag */
#define AUDIO_DECOMB_MAXLAG      2048u
#define AUDIO_DECOMB_ON_CORR     0.92
#define AUDIO_DECOMB_OFF_CORR    0.60
#define AUDIO_DECOMB_MIN_BLOCKS  8u     /* active blocks before a decision */
#define AUDIO_DECOMB_ALPHA       0.125  /* per-block smoothing */
#define AUDIO_DECOMB_RAMP        0.25f  /* fold-weight change per block */
#define AUDIO_DECOMB_SILENT_RESET 188u  /* ~1 s of 256-frame blocks at 48 kHz */

typedef struct {
    float  hist[(AUDIO_DECOMB_MAXLAG + AUDIO_DECOMB_SEARCH + 1) * 2]; /* front L/R ring */
    unsigned lag, ring, wpos;
    unsigned active;          /* blocks with side-pair signal seen */
    unsigned silent;          /* consecutive blocks with silent side pair */
    double cx[2 * AUDIO_DECOMB_SEARCH + 1];   /* smoothed side x delayed-front */
    double ed[2 * AUDIO_DECOMB_SEARCH + 1];   /* smoothed delayed-front energy */
    double es, ef, c0;        /* smoothed side / front energy, zero-lag cross */
    int on;                   /* delayed copy detected */
    unsigned best_lag;
    double corr;              /* last best normalised correlation */
    float mix;                /* side fold weight now: 1 = folded, 0 = dropped */
    float comp;               /* loudness compensation now */
    /* per-block output, linear ramps over the block */
    float w0, w1, g0, g1;
} AudioDecomb;

static inline void audio_decomb_init(AudioDecomb* d, unsigned lag)
{
    memset(d, 0, sizeof(*d));
    if (lag < AUDIO_DECOMB_SEARCH + 1) lag = AUDIO_DECOMB_SEARCH + 1;
    if (lag > AUDIO_DECOMB_MAXLAG) lag = AUDIO_DECOMB_MAXLAG;
    d->lag = lag;
    d->ring = lag + AUDIO_DECOMB_SEARCH + 1;
    d->best_lag = lag;
    d->mix = 1.0f; d->comp = 1.0f;
    d->w0 = d->w1 = 1.0f; d->g0 = d->g1 = 1.0f;
}

/* f: host-float interleaved frames, stride nch (>= 6; 0/1 = L/R, 4/5 = Ls/Rs).
 * side_gain: the fold gain the side pair would get (for the compensation).
 * Updates d->w0/w1 (side fold weight at block start/end) and d->g0/g1 (port
 * gain at block start/end). Returns +1 when detection turned on, -1 off, 0. */
static inline int audio_decomb_block(AudioDecomb* d, const float* f, unsigned frames,
                                     unsigned nch, float side_gain)
{
    enum { NL = 2 * AUDIO_DECOMB_SEARCH + 1 };
    double cx[NL], ed[NL], es = 0.0, ef = 0.0, c0 = 0.0;
    int changed = 0;
    memset(cx, 0, sizeof(cx)); memset(ed, 0, sizeof(ed));
    for (unsigned n = 0; n < frames; n++) {
        const float l = f[n * nch + 0], r = f[n * nch + 1];
        const float sl = f[n * nch + 4], sr = f[n * nch + 5];
        es += (double)sl * sl + (double)sr * sr;
        ef += (double)l * l + (double)r * r;
        c0 += (double)sl * l + (double)sr * r;
        for (unsigned k = 0; k < NL; k++) {
            const unsigned lg = d->lag - AUDIO_DECOMB_SEARCH + k;
            const unsigned i = (d->wpos + d->ring - lg) % d->ring;
            const float dl = d->hist[i * 2 + 0], dr = d->hist[i * 2 + 1];
            cx[k] += (double)sl * dl + (double)sr * dr;
            ed[k] += (double)dl * dl + (double)dr * dr;
        }
        d->hist[d->wpos * 2 + 0] = l;
        d->hist[d->wpos * 2 + 1] = r;
        d->wpos = (d->wpos + 1) % d->ring;
    }

    /* Silent side pair: hold the decision (pauses in speech must not flap),
     * but not forever -- after ~1 s of silent sides the movie is over; clear
     * the detection (and with it the makeup gain) so later front-only audio
     * on the port is not boosted. Re-detection needs the full evidence again. */
    if (es <= 1e-7 * frames) {
        if (d->silent < AUDIO_DECOMB_SILENT_RESET && ++d->silent == AUDIO_DECOMB_SILENT_RESET) {
            if (d->on) { d->on = 0; changed = -1; }
            d->active = 0;
        }
    } else {
        const double a = d->active ? AUDIO_DECOMB_ALPHA : 1.0;
        d->silent = 0;
        for (unsigned k = 0; k < NL; k++) {
            d->cx[k] += a * (cx[k] - d->cx[k]);
            d->ed[k] += a * (ed[k] - d->ed[k]);
        }
        d->es += a * (es - d->es);
        d->ef += a * (ef - d->ef);
        d->c0 += a * (c0 - d->c0);
        d->active++;

        double best = 0.0; unsigned bk = AUDIO_DECOMB_SEARCH;
        for (unsigned k = 0; k < NL; k++) {
            const double den = sqrt(d->es * d->ed[k]);
            const double c = den > 0.0 ? d->cx[k] / den : 0.0;
            if (c > best) { best = c; bk = k; }
        }
        const double den0 = sqrt(d->es * d->ef);
        const double z = den0 > 0.0 ? fabs(d->c0 / den0) : 0.0;
        d->corr = best;
        d->best_lag = d->lag - AUDIO_DECOMB_SEARCH + bk;
        if (d->active >= AUDIO_DECOMB_MIN_BLOCKS) {
            /* Delayed copy: strongly correlated at the delay, clearly more
             * than at zero lag (a steady tone correlates at every lag). */
            if (!d->on && best >= AUDIO_DECOMB_ON_CORR && best - z >= 0.2) { d->on = 1; changed = 1; }
            else if (d->on && best < AUDIO_DECOMB_OFF_CORR) { d->on = 0; changed = -1; }
        }
    }

    float target_comp = 1.0f;
    if (d->on && d->ef > 0.0) {
        const double g = sqrt((d->ef + (double)side_gain * side_gain * d->es) / d->ef);
        target_comp = (float)(g < 1.0 ? 1.0 : g > 1.6 ? 1.6 : g);
    }
    const float target_mix = d->on ? 0.0f : 1.0f;
    d->w0 = d->mix; d->g0 = d->comp;
    if (d->mix < target_mix) { d->mix += AUDIO_DECOMB_RAMP; if (d->mix > target_mix) d->mix = target_mix; }
    else if (d->mix > target_mix) { d->mix -= AUDIO_DECOMB_RAMP; if (d->mix < target_mix) d->mix = target_mix; }
    /* compensation follows the fold weight: full when the sides are out */
    const float want = 1.0f + (target_comp - 1.0f) * (1.0f - d->mix);
    d->comp += (want - d->comp) * 0.5f;
    if (fabsf(d->comp - want) < 1e-4f) d->comp = want;
    d->w1 = d->mix; d->g1 = d->comp;
    return changed;
}

#endif
