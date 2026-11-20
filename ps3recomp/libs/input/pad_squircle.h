/*
 * ps3recomp - stick squircle mapping (PAD_SQUIRCLE).
 *
 * Adapted from GoW-Ascension-Recomp (bc160201983, MIT, commit 1be0537,
 * ps3kit/libs/input/cellPad.c pad_xinput_stick_pair), which follows the
 * behaviour of RPCS3's ConvertToSquirclePoint (behavioural reference only).
 * Notice in THIRD_PARTY.md.
 *
 * A modern stick's gate is round; a DualShock 3's is nearly square, and PS3
 * games are tuned for it. Through a round gate a full diagonal reaches only
 * ~70% per axis (less after a per-axis deadzone), so diagonal moves/turns are
 * slow and "flick" gestures may not register. The squircle stretches the
 * radius by 1 + sin^2(2a) / (factor / 1000): factor 4000 takes a full
 * diagonal to ~88% per axis; a pure axis is unchanged.
 *
 * PAD_SQUIRCLE=<factor> (default 4000; 0 = the old per-axis conversion and no
 * stretch on the PAD_DEADZONE radial path).
 */
#ifndef PAD_SQUIRCLE_H
#define PAD_SQUIRCLE_H

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

static inline int pad_squircle_factor(void)
{
    static int f = -1;
    if (f < 0) {
        const char* e = getenv("PAD_SQUIRCLE");
        int v = (e && *e) ? atoi(e) : 4000;
        f = v < 0 ? 0 : v;
    }
    return f;
}

/* Stretch a stick vector (each axis -1..1, magnitude <= 1) toward the square;
 * each axis clamped to -1..1. A zero vector and a pure axis stay as they are. */
static inline void pad_squircle(double* x, double* y, int factor)
{
    const double m = sqrt(*x * *x + *y * *y);
    if (factor <= 0 || m <= 1e-9) return;
    const double ux = *x / m, uy = *y / m, s2 = 2.0 * ux * uy;   /* sin(2a) */
    const double r = m * (1.0 + s2 * s2 / ((double)factor / 1000.0));
    double nx = ux * r, ny = uy * r;
    *x = nx < -1.0 ? -1.0 : nx > 1.0 ? 1.0 : nx;
    *y = ny < -1.0 ? -1.0 : ny > 1.0 ? 1.0 : ny;
}

/* Signed -1..1 -> PS3 0..255 byte, centre exactly 128 (127 = 0x7F aliases
 * SELECT+START in some titles' button reads). */
static inline uint8_t pad_axis_u8(double v)
{
    int i = (int)lround(v * 127.0) + 128;
    return (uint8_t)(i < 0 ? 0 : i > 255 ? 255 : i);
}

/* One stick from raw XInput-style axes (+y = up): round deadzone `dz`
 * (0..1 of full scale), rescaled to 0..1 outside it, squircled; PS3 Y is
 * down, so it is reflected about 128 (256 - y, never 255 - y). */
static inline void pad_stick_squircle_u8(int16_t rx, int16_t ry, double dz, int factor,
                                         uint8_t* ox, uint8_t* oy)
{
    const double x = rx / 32767.0, y = ry / 32767.0;
    const double m = sqrt(x * x + y * y);
    double sx = 0.0, sy = 0.0;
    if (m > dz && m > 1e-9) {
        double t = (m - dz) / (1.0 - dz);
        if (t > 1.0) t = 1.0;
        sx = x / m * t; sy = y / m * t;
        pad_squircle(&sx, &sy, factor);
    }
    *ox = pad_axis_u8(sx);
    *oy = (uint8_t)(256 - pad_axis_u8(sy));
}

#endif /* PAD_SQUIRCLE_H */
