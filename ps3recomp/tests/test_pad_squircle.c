/* PAD_SQUIRCLE / PAD_MERGE pure logic (libs/input/pad_squircle.h, pad_merge.h). */
#include <stdint.h>
#include <stdio.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef struct {
    int connected; u16 buttons;
    u8 analog_lx, analog_ly, analog_rx, analog_ry, trigger_l2, trigger_r2;
    u8 press_right, press_left, press_up, press_down, press_triangle, press_circle, press_cross, press_square, press_l1, press_r1;
} PadHostState;
#include "pad_squircle.h"
#include "pad_merge.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    u8 x, y;
    const double dz = 7849.0 / 32767.0;
    /* centre and inside the deadzone: exactly 128 (127 aliases SELECT+START) */
    pad_stick_squircle_u8(0, 0, dz, 4000, &x, &y);        CHECK(x == 128 && y == 128);
    pad_stick_squircle_u8(5000, -5000, dz, 4000, &x, &y); CHECK(x == 128 && y == 128);
    /* pure axes: full range, untouched axis stays 128; up (+y) is PS3 0 */
    pad_stick_squircle_u8(32767, 0, dz, 4000, &x, &y);    CHECK(x == 255 && y == 128);
    pad_stick_squircle_u8(-32767, 0, dz, 4000, &x, &y);   CHECK(x == 1 && y == 128);
    pad_stick_squircle_u8(0, 32767, dz, 4000, &x, &y);    CHECK(x == 128 && y == 1);
    pad_stick_squircle_u8(0, -32767, dz, 4000, &x, &y);   CHECK(x == 128 && y == 255);
    /* full diagonal on a round gate: ~70% without, ~88% per axis with factor 4000 */
    const int16_t d = (int16_t)(32767 * 0.70710678);
    pad_stick_squircle_u8(d, d, dz, 0, &x, &y);
    CHECK(x >= 215 && x <= 219 && y >= 37 && y <= 41);     /* 128 +/- 0.707*127 */
    pad_stick_squircle_u8(d, d, dz, 4000, &x, &y);
    CHECK(x >= 237 && x <= 241 && y >= 15 && y <= 19);     /* 128 +/- 0.884*127 */
    /* symmetric: reflected about 128 */
    u8 x2, y2;
    pad_stick_squircle_u8((int16_t)-d, (int16_t)-d, dz, 4000, &x2, &y2);
    CHECK(x + x2 == 256 && y + y2 == 256);
    /* small factor saturates per axis, never wraps */
    double sx = 0.7071, sy = 0.7071;
    pad_squircle(&sx, &sy, 500);                           CHECK(sx == 1.0 && sy == 1.0);
    /* factor 0: unchanged */
    sx = 0.5; sy = 0.5; pad_squircle(&sx, &sy, 0);         CHECK(sx == 0.5 && sy == 0.5);

    /* merge */
    PadHostState s[3] = { 0 }, m;
    for (int i = 0; i < 3; i++) s[i].analog_lx = s[i].analog_ly = s[i].analog_rx = s[i].analog_ry = 128;
    CHECK(pad_merge_states(s, 3, &m) == 0 && !m.connected && m.analog_lx == 128 && m.analog_ry == 128);
    s[0].connected = 1; s[0].buttons = 0x0001; s[0].analog_lx = 200; s[0].analog_rx = 100; s[0].trigger_l2 = 50;
    s[1].connected = 1; s[1].buttons = 0x0800; s[1].analog_lx = 10;  s[1].analog_rx = 120; s[1].trigger_l2 = 200;
    s[2].connected = 0; s[2].buttons = 0x4000; s[2].analog_lx = 0;   s[2].trigger_r2 = 255;
    CHECK(pad_merge_states(s, 3, &m) == 2);
    CHECK(m.connected && m.buttons == 0x0801);
    CHECK(m.analog_lx == 10 && m.analog_ly == 128);        /* most deflected left stick */
    CHECK(m.analog_rx == 100);                             /* most deflected right stick */
    CHECK(m.trigger_l2 == 200 && m.trigger_r2 == 0);       /* disconnected source ignored */

    if (!fails) printf("pad_squircle/pad_merge: all passed\n");
    return fails ? 1 : 0;
}
