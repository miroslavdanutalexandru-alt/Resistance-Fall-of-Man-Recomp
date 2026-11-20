/*
 * ps3recomp - PAD_MERGE: every connected controller drives player 1.
 *
 * Adapted from GoW-Ascension-Recomp (bc160201983, MIT, commit 1be0537,
 * ps3kit/libs/input/cellPad.c pad_merge_port0). Notice in THIRD_PARTY.md.
 *
 * Include after the PadHostState typedef. Buttons are OR'd; each stick comes
 * from whichever source deflects it most (L1 distance from 128); triggers and
 * pressure take the maximum. A neutral merged pad has its sticks at exactly 128.
 * Returns the number of connected sources (0: *out is a disconnected neutral pad).
 */
#ifndef PAD_MERGE_H
#define PAD_MERGE_H

#include <stdlib.h>
#include <string.h>

static inline int pad_merge_enabled(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("PAD_MERGE"); on = (e && *e && *e != '0') ? 1 : 0; }
    return on;
}

static inline int pad_merge_states(const PadHostState* src, int n, PadHostState* out)
{
    PadHostState m;
    int bl = 0, br = 0, cnt = 0;
    memset(&m, 0, sizeof m);
    m.analog_lx = m.analog_ly = m.analog_rx = m.analog_ry = 128;
    for (int i = 0; i < n; i++) {
        const PadHostState* s = &src[i];
        if (!s->connected) continue;
        cnt++;
        m.buttons = (u16)(m.buttons | s->buttons);
        const int dl = abs((int)s->analog_lx - 128) + abs((int)s->analog_ly - 128);
        const int dr = abs((int)s->analog_rx - 128) + abs((int)s->analog_ry - 128);
        if (dl > bl) { bl = dl; m.analog_lx = s->analog_lx; m.analog_ly = s->analog_ly; }
        if (dr > br) { br = dr; m.analog_rx = s->analog_rx; m.analog_ry = s->analog_ry; }
#define PAD_MERGE_MAXU8(f) if (s->f > m.f) m.f = s->f
        PAD_MERGE_MAXU8(trigger_l2); PAD_MERGE_MAXU8(trigger_r2);
        PAD_MERGE_MAXU8(press_right); PAD_MERGE_MAXU8(press_left); PAD_MERGE_MAXU8(press_up); PAD_MERGE_MAXU8(press_down);
        PAD_MERGE_MAXU8(press_triangle); PAD_MERGE_MAXU8(press_circle); PAD_MERGE_MAXU8(press_cross); PAD_MERGE_MAXU8(press_square);
        PAD_MERGE_MAXU8(press_l1); PAD_MERGE_MAXU8(press_r1);
#undef PAD_MERGE_MAXU8
    }
    m.connected = cnt > 0;
    *out = m;
    return cnt;
}

#endif /* PAD_MERGE_H */
