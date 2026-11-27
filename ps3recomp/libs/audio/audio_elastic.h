/*
 * ps3recomp - "elastic" consumption of the game's audio blocks (cellAudio.c, PORT_AUDIO_ELASTIC).
 *
 * Adapted from the GoW Ascension recomp's AUDIO_ELASTIC (ps3kit/libs/audio/cellAudio.c, commit 1be0537,
 * https://github.com/bc160201983/GoW-Ascension-Recomp), MIT licence:
 *
 *   Copyright (c) 2026 BilalReloaded
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
 *   associated documentation files (the "Software"), to deal in the Software without restriction, including
 *   without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the
 *   following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all copies or substantial
 *   portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT
 *   LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO
 *   EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 *   IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 *   USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The scheme (RPCS3's "untouched buffer" idea): consumed port blocks are refilled with -0.0f (the sentinel),
 * so "not written yet" differs from silence. A block still unwritten at the cursor of a port that is feeding
 * sound is neither mixed nor consumed: the mixer waits while the host buffer holds the floor, and below it
 * writes one block of concealment WITHOUT consuming the game's block (the late block still plays when it
 * lands; nothing the game wrote is dropped). Each concealed block leaves the read index one block behind real
 * time (a debt, up to debt_max); it is repaid by consuming, without playing, blocks the game wrote as
 * digital silence, at most one per block mixed. Past debt_max, and all of it when a port gives up (no block
 * for cap_ms: the game stopped feeding it), the offset is forgiven.
 *
 * This header holds the decisions only (no locks, no I/O) so test_audio_out.cpp can drive them.
 */
#ifndef PS3RECOMP_AUDIO_ELASTIC_H
#define PS3RECOMP_AUDIO_ELASTIC_H

#include <stdint.h>

#define AUDIO_EL_SENTINEL_BE 0x00000080u   /* -0.0f in guest (big-endian) byte order, read as a host u32 */

typedef struct {
    double             last_written_ms;  /* a written block was last seen at the cursor (0 = never) */
    int                quiet;            /* gave up on this port: no waits until it writes again */
    double             wait_t0;          /* start of the wait for the block at the cursor (0 = none) */
    unsigned long long wait_ridx;        /* the read index that wait is for */
} AudioElPort;

typedef struct {
    double   cap_ms;      /* PORT_AUDIO_ELASTIC_CAP_MS: longest wait / concealment for one block */
    unsigned debt_max;    /* PORT_AUDIO_ELASTIC_DEBT */
    unsigned debt;        /* concealed blocks not yet repaid */
    int      repay_ok;    /* a block was mixed since the last repayment */
    unsigned run;         /* concealment blocks in a row (for the crossfade back into the mix) */
    /* counters (reset by the caller's log window) */
    unsigned waits, saved, conceal, skip, giveup, repaid, forgiven;
    double   wait_ms, wait_max_ms;
} AudioElastic;

static inline void audio_el_init(AudioElastic* el, double cap_ms, unsigned debt_max)
{
    AudioElastic z = { 0 };
    *el = z;
    el->cap_ms = cap_ms > 0.0 ? cap_ms : 150.0;
    el->debt_max = debt_max;
}

/* 0 = every word still the sentinel (not written since consumed), 1 = written (its last 16 frames hold data),
 * 2 = written at the head only (the producer is writing it right now). w: the raw big-endian block. */
static inline int audio_el_block_state(const uint32_t* w, unsigned nwords, unsigned nch)
{
    const unsigned tail = nwords > 16u * nch ? nwords - 16u * nch : 0u;
    for (unsigned i = tail; i < nwords; i++) if (w[i] != AUDIO_EL_SENTINEL_BE) return 1;
    for (unsigned i = 0; i < tail; i++) if (w[i] != AUDIO_EL_SENTINEL_BE) return 2;
    return 0;
}
static inline int audio_el_block_written(const uint32_t* w, unsigned nwords)
{
    for (unsigned i = 0; i < nwords; i++) if (w[i] != AUDIO_EL_SENTINEL_BE) return 1;
    return 0;
}
/* any sample other than +-0.0 (BE sign bit = bit 7 of the host word) */
static inline int audio_el_block_nonzero(const uint32_t* w, unsigned nwords)
{
    for (unsigned i = 0; i < nwords; i++) if (w[i] & 0xFFFFFF7Fu) return 1;
    return 0;
}

static inline int audio_el_active(const AudioElPort* e, double now_ms)
{
    return !e->quiet && e->last_written_ms > 0.0 && now_ms - e->last_written_ms <= 1000.0;
}

/* One running port's block at the cursor. st: audio_el_block_state; later_written: a later ring block is
 * written while this one is not (the producer skipped it). Returns 1 = late (the mixer waits or conceals),
 * 0 = mix. A give-up forgives the whole debt. */
static inline int audio_el_port_gate(AudioElastic* el, AudioElPort* e, int st, int later_written,
                                     unsigned long long ridx, double now_ms)
{
    if (st == 1) {
        if (e->wait_t0 > 0.0) {   /* it came while the mixer waited: saved */
            const double w = now_ms - e->wait_t0;
            el->saved++; el->wait_ms += w;
            if (w > el->wait_max_ms) el->wait_max_ms = w;
            e->wait_t0 = 0.0;
        }
        e->last_written_ms = now_ms; e->quiet = 0;
        return 0;
    }
    if (st == 2) { e->last_written_ms = now_ms; e->quiet = 0; }   /* being written now */
    if (!audio_el_active(e, now_ms)) { e->wait_t0 = 0.0; return 0; }
    if (st == 0 && later_written) { el->skip++; e->wait_t0 = 0.0; return 0; }
    if (e->wait_t0 <= 0.0 || e->wait_ridx != ridx) { e->wait_t0 = now_ms; e->wait_ridx = ridx; el->waits++; return 1; }
    if (now_ms - e->wait_t0 >= el->cap_ms) {   /* the game stopped feeding it */
        el->giveup++; e->quiet = 1; e->wait_t0 = 0.0;
        el->forgiven += el->debt; el->debt = 0;
        return 0;
    }
    return 1;
}

/* May this port's block at the cursor be dropped to repay debt? Only digital silence the game wrote, or the
 * unwritten block of a port that is not feeding sound. */
static inline int audio_el_port_repayable(const AudioElPort* e, int st, int nonzero, double now_ms)
{
    if (st == 2) return 0;
    if (st == 1) return !nonzero;
    return !audio_el_active(e, now_ms);
}

/* One block of concealment written: owe it, or forgive it past debt_max */
static inline void audio_el_on_conceal(AudioElastic* el)
{
    el->conceal++; el->run++;
    if (el->debt < el->debt_max) el->debt++; else el->forgiven++;
}
static inline void audio_el_on_repaid(AudioElastic* el) { if (el->debt) el->debt--; el->repaid++; el->repay_ok = 0; }
static inline void audio_el_on_mixed(AudioElastic* el) { el->repay_ok = 1; }

#endif
