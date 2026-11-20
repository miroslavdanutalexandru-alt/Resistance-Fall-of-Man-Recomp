/*
 * ps3recomp - mix thread buffer control (cellAudio.c): clock-drift steering and the adaptive target.
 * Shared by the three ports (keep identical). Pure C, no OS calls; test_audio_out.cpp covers it.
 *
 * Drift (PORT_AUDIO_DRIFT, default 1): the mixer owns the guest audio clock (one 256-frame block per
 * 5.33 ms on QPC) while the device plays on its own crystal, so the two drift apart by up to a few
 * hundred ppm and the host buffer slowly fills or drains. Instead of resampling the output, the guest
 * clock period is steered (the game simply mixes slightly faster / slower; no sample is touched):
 * a proportional term on the smoothed buffer error plus a slow integral that converges on the actual
 * drift (reported in ppm), both clamped to AUDIO_CATCHUP_PCT. PORT_AUDIO_DRIFT=0: the old bang-bang
 * (catch-up % short below the target, as much long above 1.5x the target).
 *
 * Adaptive target (PORT_AUDIO_ADAPTIVE, default 1, SoundTouch stage only): a 10 s window with an
 * underrun, or stretched >= 2 % of the time, raises the target buffer by 25 ms (small enough that the
 * buffer stays above the stretch threshold while it refills) up to PORT_AUDIO_BUFFER_MAX_MS
 * (default 300). After 5 clean minutes it steps back 25 ms towards the configured buffer.
 */
#ifndef PS3RECOMP_AUDIO_ADAPT_H
#define PS3RECOMP_AUDIO_ADAPT_H

typedef struct AudioDrift {
    double fill;      /* smoothed queued / target (EMA, ~50 ms) */
    double integ;     /* integral term = the estimated clock drift (fraction; 1e-6 = 1 ppm) */
    int    init;
} AudioDrift;

#define AUDIO_DRIFT_KP     0.03      /* speed fraction at an empty (or doubled) buffer */
#define AUDIO_DRIFT_KI     2e-6      /* integral gain per guest block */
#define AUDIO_DRIFT_IMAX   0.005     /* integral clamp: 5000 ppm, far above real crystal drift */

/* Fraction to shorten the guest block period by (negative = lengthen) for this block.
 * fill = host buffer queued / target. lim = AUDIO_CATCHUP_PCT / 100. */
static inline double audio_drift_step(AudioDrift* d, double fill, double lim)
{
    if (!(fill >= 0.0)) fill = 0.0;
    if (fill > 3.0) fill = 3.0;
    if (!d->init) { d->fill = fill; d->integ = 0.0; d->init = 1; }
    d->fill += (fill - d->fill) * 0.1;
    double err = 1.0 - d->fill;               /* > 0: below the target, produce faster */
    if (err > 1.0) err = 1.0;
    if (err < -1.0) err = -1.0;
    const double imax = lim < AUDIO_DRIFT_IMAX ? lim : AUDIO_DRIFT_IMAX;
    d->integ += err * AUDIO_DRIFT_KI;
    if (d->integ > imax) d->integ = imax;
    if (d->integ < -imax) d->integ = -imax;
    double s = AUDIO_DRIFT_KP * err + d->integ;
    if (s > lim) s = lim;
    if (s < -lim) s = -lim;
    return s;
}
/* forget the history (re-prime, device re-open, pause): the buffer is rebuilt from scratch */
static inline void audio_drift_reset(AudioDrift* d) { d->init = 0; }
static inline double audio_drift_ppm(const AudioDrift* d) { return d->integ * 1e6; }

typedef struct AudioAdapt {
    unsigned base_ms, max_ms, cur_ms;
    unsigned clean;   /* consecutive clean windows */
    unsigned raises;  /* session total */
} AudioAdapt;

#define AUDIO_ADAPT_STEP_MS        25
#define AUDIO_ADAPT_STRETCH_PCT    2.0
#define AUDIO_ADAPT_DECAY_WINDOWS  30   /* 30 x 10 s = 5 min clean */

static inline void audio_adapt_init(AudioAdapt* a, unsigned base_ms, unsigned max_ms)
{
    a->base_ms = a->cur_ms = base_ms;
    a->max_ms = max_ms > base_ms ? max_ms : base_ms;
    a->clean = 0; a->raises = 0;
}
/* One 10 s window. Returns the new target in ms when it changed, else 0. */
static inline unsigned audio_adapt_window(AudioAdapt* a, unsigned underruns, double stretched_pct)
{
    if (underruns || stretched_pct >= AUDIO_ADAPT_STRETCH_PCT) {
        a->clean = 0;
        if (a->cur_ms >= a->max_ms) return 0;
        a->cur_ms = a->cur_ms + AUDIO_ADAPT_STEP_MS < a->max_ms ? a->cur_ms + AUDIO_ADAPT_STEP_MS : a->max_ms;
        a->raises++;
        return a->cur_ms;
    }
    if (a->cur_ms > a->base_ms && ++a->clean >= AUDIO_ADAPT_DECAY_WINDOWS) {
        a->clean = 0;
        a->cur_ms = a->cur_ms - a->base_ms > AUDIO_ADAPT_STEP_MS ? a->cur_ms - AUDIO_ADAPT_STEP_MS : a->base_ms;
        return a->cur_ms;
    }
    return 0;
}
#endif /* PS3RECOMP_AUDIO_ADAPT_H */
