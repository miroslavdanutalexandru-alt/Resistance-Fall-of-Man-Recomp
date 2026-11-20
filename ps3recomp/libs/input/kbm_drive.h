/* kbm_drive.h -- keyboard driving (Fall of Man issue #14). Pure helpers, shared
 * by cellPad.c and its test.
 *
 * The LU-P Lynx (and the other vehicles) accelerate with R2, brake / reverse with
 * L2 and steer with the right stick. The keyboard had none of that on W/S/A/D:
 * on foot they are the left stick, and while the in-play look code is not running
 * (vehicles, turrets, menus) cellPad.c turns them into the d-pad for the in-game
 * menus. So the jeep steered with the mouse but only moved on the weapon-wheel
 * key (E = R2). While the player is driving, W / S now also press R2 / L2 and
 * A / D also push the right stick left / right.
 *
 * "Driving" is not just "the look code is off" -- that is also true during a
 * hitch, a load, a cutscene or with the weapon wheel (R2) open, and on foot R2
 * is the weapon wheel: W held in a hitch would open the wheel, which keeps the
 * look code off, which keeps W on R2 (stuck in the wheel). So it also needs the
 * game's vehicle-driver input code to have run for this player in the last
 * 200 ms (stubs.cpp rfom_drive_note, hooked into the vehicles' driver input
 * functions); turrets and gunner seats run the on-foot look code instead. */
#ifndef KBM_DRIVE_H
#define KBM_DRIVE_H

/* now / drive_ms: ms ticks; drive_ms = last time the driver input code ran for
 * the keyboard player (0 = never). look_off: cellPad.c's in-game menu mode. */
static inline int kbm_drive_active(int enabled, unsigned long long now, unsigned long long drive_ms,
                                   int look_off, int pause_ui, int space_start)
{
    return enabled && look_off && !pause_ui && !space_start && drive_ms && now >= drive_ms && now - drive_ms < 200;
}

/* driving: kbm_drive_active().
 * Returns the buttons to add (accel / brake bits), *steer = -1 left, 0, 1 right.
 * W and S together cancel, as do A and D. */
static inline unsigned kbm_drive_map(int driving, int w, int s, int a, int d,
                                     unsigned accel_bit, unsigned brake_bit, int* steer)
{
    *steer = 0;
    if (!driving) return 0u;
    *steer = (d ? 1 : 0) - (a ? 1 : 0);
    if (w && !s) return accel_bit;
    if (s && !w) return brake_bit;
    return 0u;
}

/* Right stick X with the steering keys applied (they win over the mouse). */
static inline unsigned char kbm_drive_rx(int steer, unsigned char rx)
{
    return steer < 0 ? (unsigned char)0 : steer > 0 ? (unsigned char)255 : rx;
}

#endif
