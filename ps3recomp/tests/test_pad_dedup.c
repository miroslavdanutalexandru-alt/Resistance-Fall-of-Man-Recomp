/* Duplicate-pad detection pure logic (libs/input/pad_logic.h, used by pad_sdl3.c padx_dedup): a
 * wrapper's virtual copy (DSX / DS4Windows / Steam Input) is told apart from a real second controller.
 * Same cases as Resistance 3's test_pad_coop.c.
 *   python ps3recomp\tools\test_pad_dedup.py
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "pad_logic.h"
/* Duplicate detection (pad_sdl3.c): a wrapper's copy matches many different
 * inputs; two real pads that are just held the same way are not a copy. */
static void test_dedup_needs_distinct_inputs(void)
{
    PadSigHist real; PadPair pp;
    memset(&real, 0, sizeof real); memset(&pp, 0, sizeof pp);
    const unsigned fwd = padd_sig(0, 0, -30000, 0, -30000, 0, 0);   /* both sticks forward */
    padd_hist_push(&real, fwd);
    for (int i = 0; i < 100; i++) assert(padd_update(&pp, fwd, &real, PADD_MIN_SEEN_ANY, PADD_MIN_MATCH) == 0);
    /* The wrapper's Xbox-id pad only needs the one input to match. */
    memset(&pp, 0, sizeof pp);
    int v = 0;
    for (int i = 0; i < 20 && v == 0; i++) v = padd_update(&pp, fwd, &real, 1, PADD_MIN_MATCH);
    assert(v == 1);
    /* Any pad copying several different inputs is a duplicate. */
    memset(&pp, 0, sizeof pp); memset(&real, 0, sizeof real);
    const unsigned in[4] = { fwd, padd_sig(0x1000, 0, 0, 0, 0, 0, 0),
                             padd_sig(0x2000, 0, 0, 0, 0, 0, 0), padd_sig(0, 0, 0, 0, 0, 255, 0) };
    v = 0;
    for (int i = 0; i < 40 && v == 0; i++) {
        padd_hist_push(&real, in[i % 4]);
        v = padd_update(&pp, in[i % 4], &real, PADD_MIN_SEEN_ANY, PADD_MIN_MATCH);
    }
    assert(v == 1);
    /* Compared afresh: its own input (a button) shows a different controller. */
    memset(&pp, 0, sizeof pp);
    const unsigned own = padd_sig(0x4000, 30000, 0, 0, 0, 0, 0);
    v = 0;
    for (int i = 0; i < 20 && v == 0; i++) v = padd_update(&pp, own, &real, PADD_MIN_SEEN_ANY, PADD_MIN_MATCH);
    assert(v == -1);
}

/* An inexact copy (a wrapper's anti-deadzone puts its stick over the threshold
 * while the real stick is just under it) is neither freed nor taken for a
 * different controller; a real second pad pressing its own buttons still is. */
static void test_dedup_inexact_mirror(void)
{
    PadSigHist real; PadPair pp; PadFree fr;
    memset(&real, 0, sizeof real); memset(&pp, 0, sizeof pp); memset(&fr, 0, sizeof fr);
    const unsigned under = padd_sig(0, 15000, 0, 0, 0, 0, 0);     /* real: just under, sig 0 */
    const unsigned over  = padd_sig(0, 17000, 0, 0, 0, 0, 0);     /* copy: just over */
    assert(under == 0 && over != 0);
    for (int i = 0; i < 200; i++) {
        padd_hist_push(&real, under);
        assert(padd_update(&pp, over, &real, PADD_MIN_SEEN_ANY, PADD_MIN_MATCH) == 0);
        assert(!padd_free_update(&fr, over, padd_hist_neutral(&real), padd_hist_close(&real, over)));
    }
    /* Mixed into real mirroring, the offset samples don't hide the copy. */
    const unsigned in[4] = { padd_sig(0x1000, 0, 0, 0, 0, 0, 0), padd_sig(0x2000, 0, 0, 0, 0, 0, 0),
                             padd_sig(0x4000, 0, 0, 0, 0, 0, 0), padd_sig(0x8000, 0, 0, 0, 0, 0, 0) };
    int v = 0;
    for (int i = 0; i < 80 && v == 0; i++) {
        padd_hist_push(&real, (i & 1) ? in[i / 2 % 4] : under);
        v = padd_update(&pp, (i & 1) ? in[i / 2 % 4] : over, &real, PADD_MIN_SEEN_ANY, PADD_MIN_MATCH);
    }
    assert(v == 1);
    /* An ignored copy: one remapped button (touchpad -> a button) while the real
     * pad is neutral, held for a long time, does not free it ... */
    memset(&real, 0, sizeof real); memset(&fr, 0, sizeof fr);
    for (int i = 0; i < PADD_HIST; i++) padd_hist_push(&real, 0);
    const unsigned remap = padd_sig(0x1000, 0, 0, 0, 0, 0, 0);
    for (int i = 0; i < 1000; i++) assert(!padd_free_update(&fr, remap, padd_hist_neutral(&real), 0));
    /* ... nor do buttons the real pad pressed just before (a mirror's lag) ... */
    memset(&fr, 0, sizeof fr);
    for (int i = 0; i < 300; i++) {
        PadSigHist h; memset(&h, 0, sizeof h);
        padd_hist_push(&h, in[i % 4]);
        assert(!padd_free_update(&fr, in[i % 4], padd_hist_neutral(&h), padd_hist_close(&h, in[i % 4])));
    }
    /* ... and evidence decays when it stops. */
    memset(&fr, 0, sizeof fr);
    for (int i = 0; i < 40; i++) padd_free_update(&fr, in[i % 3], 1, 0);
    padd_free_decay(&fr); padd_free_decay(&fr);
    assert(!padd_free_update(&fr, in[0], 1, 0));
    /* A real pad pressing several of its own buttons while player 1 is idle is freed. */
    memset(&fr, 0, sizeof fr);
    int freed = 0;
    for (int i = 0; i < 90 && !freed; i++) freed = padd_free_update(&fr, in[i / 30], padd_hist_neutral(&real), 0);
    assert(freed);
}

int main(void)
{
    test_dedup_needs_distinct_inputs();
    test_dedup_inexact_mirror();
    printf("test_pad_dedup: all passed\n");
    return 0;
}