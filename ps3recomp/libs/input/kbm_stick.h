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

/* Weapon wheel on keyboard & mouse (PORT_KBM_WHEEL). The wheel picks the slot
 * the stick points at, and only once the stick is pushed far out (Resistance 3:
 * the raw left stick past 0.75 of full). The mouse only made brief, small right
 * stick deflections that fall back to centre, and the keys gave a square
 * (diagonal = 1.41 x 1.41, and nothing once menu mode took WASD).
 * While the wheel button is held, mouse counts add up to a direction (length
 * capped at radius, so turning the mouse the other way soon swings it round);
 * past 0.4 x radius the sticks are pushed all the way out along it, and stay
 * there when the mouse stops. Movement keys set it to a full-length 8-way
 * direction (diagonal 0.707 per axis). After the button is let go the direction
 * is kept for two more pad reports (at most 250 ms), so the game still sees it
 * in the report where the button comes up -- that is when it takes the pick. */
typedef struct {
    double x, y;                 /* direction, mouse counts (length <= radius) */
    int down, live, menu;        /* button held at the last step; sticks fed; pressed in a menu */
    unsigned long long t0, rel;  /* press time, last held step (ms) */
    unsigned deliv;              /* reports delivered at the last held step */
} kbm_wheel;

/* One stick byte for a unit-direction component in [-1, 1] (128 = centre). */
static inline unsigned char kbm_wheel_byte(double c)
{
    if (!(c == c)) return 128;
    if (c > 1.0) c = 1.0;
    if (c < -1.0) c = -1.0;
    return (unsigned char)(int)floor(128.0 + c * 127.0 + 0.5);
}

/* Stick pushed all the way out along (x, y); centred under min_len. */
static inline void kbm_wheel_dir(double x, double y, double min_len, unsigned char* sx, unsigned char* sy)
{
    const double l = sqrt(x * x + y * y);
    if (!(l >= min_len) || l <= 0.0) { *sx = *sy = 128; return; }
    *sx = kbm_wheel_byte(x / l);
    *sy = kbm_wheel_byte(y / l);
}

/* One pad poll. key: the wheel button is held; in_menu: menu mode now (looked
 * at when the button goes down: a press in a menu is ignored); mdx/mdy: mouse
 * counts since the last poll (screen direction, +y = down); kx/ky: movement keys
 * (-1/0/1, +y = down); deliv: pad reports the game has taken so far; hold_ms:
 * held this long before the sticks are taken (a tap stays a tap). Returns 1
 * when both sticks the wheel may read must show *sx / *sy. */
static inline int kbm_wheel_step(kbm_wheel* w, int key, int in_menu, double mdx, double mdy,
                                 int kx, int ky, unsigned long long now, unsigned deliv,
                                 double radius, unsigned hold_ms, unsigned char* sx, unsigned char* sy)
{
    if (!(radius > 1.0)) radius = 1.0;
    if (key && !w->down) { w->x = w->y = 0.0; w->t0 = now; w->live = 0; w->menu = in_menu; }
    w->down = key;
    if (key && !w->menu) {
        if (mdx == mdx && mdy == mdy) { w->x += mdx; w->y += mdy; }
        const double l = sqrt(w->x * w->x + w->y * w->y);
        if (l > radius) { w->x *= radius / l; w->y *= radius / l; }
        if (kx || ky) {
            const double k = radius / sqrt((double)(kx * kx + ky * ky));
            w->x = kx * k; w->y = ky * k;
        }
        if (now - w->t0 >= hold_ms) w->live = 1;
        w->rel = now; w->deliv = deliv;
    } else if (w->live && !(deliv - w->deliv < 2u && now - w->rel < 250u)) {
        w->live = 0; w->x = w->y = 0.0;
    }
    if (!w->live) return 0;
    kbm_wheel_dir(w->x, w->y, radius * 0.4, sx, sy);
    return 1;
}

#endif
