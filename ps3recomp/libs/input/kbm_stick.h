/* kbm_stick.h -- mouse -> virtual right stick (vehicles, turrets, anything the
 * direct mouse look does not drive). Pure helpers, shared by cellPad.c and its test.
 *
 * The game reads the stick as one byte per axis (128 = centre) and applies its
 * own deadzone and response curve. Mapping the mouse speed straight to a byte
 * truncated it to 127 steps whose lowest live step sat right at the deadzone
 * edge, so a slow mouse move toggled between "no turn" and "first step" from
 * poll to poll, and the poll interval came from the 15.6 ms system tick (a 0 ms
 * interval read as 1 ms: a 16x speed spike). Tank turrets showed it as jitter
 * (Fall of Man issue #9).
 *
 * kbm_stick_axis places the deflection past the deadzone continuously
 * (deadzone-aware: anything above zero lands in the live range) and carries the
 * fractional part of each poll's byte into the next one (error diffusion), so
 * the average over a few frames is the exact deflection and slow moves give a
 * slow, even turn instead of steps. */
#ifndef KBM_STICK_H
#define KBM_STICK_H

#include <math.h>

/* Below this deflection fraction the stick is centred (mouse stopped). */
#define KBM_STICK_IDLE 0.002

/* Per-axis carry, kept by the caller across polls ({0} initially). */
typedef struct kbm_stick_carry { double r; int dir; } kbm_stick_carry;

/* v: signed deflection fraction (1 = full). dz: the game's deadzone as a fraction
 * of the half range (mouse_deadzone). c: the axis' carry. Returns the stick byte
 * (1..255, 128 = centre). */
static inline unsigned char kbm_stick_axis(double v, double dz, kbm_stick_carry* c)
{
    double a = fabs(v);
    if (!(a >= KBM_STICK_IDLE)) { c->r = 0.0; c->dir = 0; return 128; }
    if (a > 1.0) a = 1.0;
    if (!(dz >= 0.0)) dz = 0.0;
    if (dz > 0.95) dz = 0.95;
    const double dzu = dz * 127.0;                  /* deadzone edge, stick units */
    const double want = dzu + (127.0 - dzu) * a;    /* continuous target past the edge */
    const int sgn = v < 0.0 ? -1 : 1;
    /* a carry from the other direction is meaningless: start fresh on a reversal */
    if (c->dir != sgn) { c->r = 0.0; c->dir = sgn; }
    const double acc = want + c->r;
    int out = (int)floor(acc + 0.5);
    const int lo = (int)floor(dzu);                 /* lowest step used: the last dead one */
    if (out < lo) out = lo;
    if (out > 127) out = 127;
    double r = acc - (double)out;
    if (r > 1.0) r = 1.0;                           /* bounded: clamping must not wind up */
    if (r < -1.0) r = -1.0;
    c->r = r;
    const int iv = 128 + sgn * out;
    return (unsigned char)(iv < 1 ? 1 : iv > 255 ? 255 : iv);
}

#endif
