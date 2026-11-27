/* Tests for ps3emu/port_pc_extras.h (the PC-port extras' pure logic). */
#include <stdio.h>
#include <string.h>
#include "ps3emu/port_pc_extras.h"

static int s_fail = 0, s_n = 0;
#define CHECK(c) do { s_n++; if (!(c)) { s_fail++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define NEAR(a, b) ((a) - (b) < 0.001f && (b) - (a) < 0.001f)

static void put16(unsigned char* p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void put32(unsigned char* p, unsigned v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }

/* A game whose zoom toggles on a button press it reads once a frame, with the FOV easing between 82 and
 * 65 degrees. The player holds the zoom button for hold_ms from t=100; the game ignores the first
 * `ignore` presses it reads after the release (busy), and with rezoom_ms it zooms in again by itself
 * that long after the release (e.g. after a reload). Returns whether it is zoomed at the end. */
static int aim_sim(unsigned frame_ms, unsigned hold_ms, int ignore, unsigned rezoom_ms, int* clicks)
{
    PcxAimHold a; unsigned long long t; unsigned long frames = 0;
    int zoom = 0, last_read = 0, out = 0, released = 0;
    double fov = 82.0;
    memset(&a, 0, sizeof a);
    *clicks = 0;
    for (t = 0; t < 5000; t++) {
        const int held = t >= 100 && t < 100 + hold_ms;
        if (t >= 100 + hold_ms) released = 1;
        if (t % 4 == 0) {
            const int o = pcx_aim_hold(&a, held, fov < 81.5, fov, t, frames, 0);
            if (o && !out) (*clicks)++;
            out = o || held;
        }
        if (t % frame_ms == 0) {
            frames++;
            if (out && !last_read) { if (released && ignore > 0) ignore--; else zoom = !zoom; }
            last_read = out;
            if (rezoom_ms && t == 100 + hold_ms + rezoom_ms - (100 + hold_ms + rezoom_ms) % frame_ms) zoom = 1;
            fov += ((zoom ? 65.0 : 82.0) - fov) * 0.3;
            if (fov > 81.95) fov = 82.0;
        }
    }
    return zoom;
}

int main(void)
{
    /* 1 */
    CHECK(pcx_enter_button(NULL) == 1);
    CHECK(pcx_enter_button("cross") == 1);
    CHECK(pcx_enter_button("Circle") == 0);
    /* 2 */
    CHECK(pcx_parse_pct(NULL, 100, 0, 100) == 100);
    CHECK(pcx_parse_pct("off", 100, 0, 100) == 0);
    CHECK(pcx_parse_pct("150", 100, 0, 100) == 100);
    CHECK(pcx_parse_pct("50", 100, 0, 100) == 50);
    CHECK(pcx_parse_pct("junk", 70, 0, 100) == 70);
    CHECK(pcx_rumble_scale(65535, 100) == 65535);
    CHECK(pcx_rumble_scale(65535, 0) == 0);
    CHECK(pcx_rumble_scale(40000, 50) == 20000);
    /* 3 */
    CHECK(pcx_bg_mode("") == -1 && pcx_bg_mode("0") == 0 && pcx_bg_mode("1") == 1);
    CHECK(pcx_pad_input_allowed(-1, 1, 0) == 1);
    CHECK(pcx_pad_input_allowed(-1, 0, 0) == 0);
    CHECK(pcx_pad_input_allowed(-1, 0, 1) == 1);
    CHECK(pcx_pad_input_allowed(0, 0, 1) == 0);
    CHECK(pcx_pad_input_allowed(1, 0, 0) == 1);
    CHECK(pcx_bg_keeps_running("run") && pcx_bg_keeps_running("Mute"));
    CHECK(!pcx_bg_keeps_running("") && !pcx_bg_keeps_running(NULL) && !pcx_bg_keeps_running("pause"));
    /* 1: keyboard confirm / back with Circle as the confirm button (Cross 0x40, Circle 0x20, Start 0x08) */
    CHECK(pcx_swap_confirm(0x40 | 0x08, 0x40, 0x20, 1) == (0x40 | 0x08));
    CHECK(pcx_swap_confirm(0x40 | 0x08, 0x40, 0x20, 0) == (0x20 | 0x08));
    CHECK(pcx_swap_confirm(0x20, 0x40, 0x20, 0) == 0x40);
    CHECK(pcx_swap_confirm(0x60, 0x40, 0x20, 0) == 0x60);
    /* 10: duplicate import slot */
    CHECK(pcx_import_prefer(1, 5, 0, 9) && !pcx_import_prefer(0, 9, 1, 5));
    CHECK(pcx_import_prefer(0, 9, 0, 5) && !pcx_import_prefer(0, 5, 0, 9) && !pcx_import_prefer(1, 5, 1, 5));
    /* 4 */
    { float b[4] = { 1.0f, 0.0f, 0.5f, -0.5f };
      pcx_audio_apply(b, 2, 0.5f, 0);
      CHECK(NEAR(b[0], 0.5f) && NEAR(b[1], 0.0f) && NEAR(b[2], 0.25f));
      pcx_audio_apply(b, 2, 1.0f, 1);
      CHECK(NEAR(b[0], 0.25f) && NEAR(b[1], 0.25f) && NEAR(b[2], 0.0f) && NEAR(b[3], 0.0f)); }
    /* 5 */
    CHECK(pcx_stick_mag(0.05f, 0.1f, 0.0f, 0.0f, 1.0f) == 0.0f);
    CHECK(pcx_stick_mag(0.95f, 0.1f, 0.1f, 0.0f, 1.0f) == 1.0f);
    CHECK(NEAR(pcx_stick_mag(0.5f, 0.0f, 0.0f, 0.0f, 1.0f), 0.5f));
    CHECK(pcx_stick_mag(0.11f, 0.1f, 0.0f, 0.2f, 1.0f) >= 0.2f);   /* anti-deadzone: jumps past it */
    CHECK(pcx_stick_mag(0.5f, 0.0f, 0.0f, 0.0f, 2.0f) < 0.3f);     /* curve: finer near centre */
    CHECK(pcx_trigger_dz(20, 10) == 0);
    CHECK(pcx_trigger_dz(255, 10) == 255);
    CHECK(pcx_trigger_dz(100, 0) == 100);
    { PcxTurnBoost tb = { 0 };
      CHECK(pcx_turn_boost(&tb, 0.5f, 16, 1.0f, 400) == 0.0f);
      CHECK(pcx_turn_boost(&tb, 1.0f, 200, 1.0f, 400) > 0.4f);
      CHECK(pcx_turn_boost(&tb, -1.0f, 300, 1.0f, 400) == 1.0f);
      CHECK(pcx_turn_boost(&tb, 0.2f, 16, 1.0f, 400) == 0.0f && tb.held_ms == 0); }
    /* 6 */
    CHECK(pcx_btn_mode("toggle") == PCX_BTN_TOGGLE && pcx_btn_mode("hold") == PCX_BTN_HOLD && pcx_btn_mode("") == PCX_BTN_GAME);
    { PcxBtnConv c; memset(&c, 0, sizeof c);
      CHECK(pcx_btn_conv(&c, PCX_BTN_TOGGLE, 1, 0, 0) == 1);
      CHECK(pcx_btn_conv(&c, PCX_BTN_TOGGLE, 0, 10, 0) == 1);   /* released: stays latched */
      CHECK(pcx_btn_conv(&c, PCX_BTN_TOGGLE, 1, 20, 0) == 0);
      CHECK(pcx_btn_conv(&c, PCX_BTN_TOGGLE, 0, 30, 0) == 0);
      pcx_btn_conv(&c, PCX_BTN_TOGGLE, 1, 40, 0);
      CHECK(pcx_btn_conv(&c, PCX_BTN_TOGGLE, 1, 50, 1) == 1 && c.latched == 0); }   /* menu: raw, latch dropped */
    { PcxBtnConv c; unsigned long long t; int downs = 0, last = 0;
      memset(&c, 0, sizeof c);
      /* hold for 500 ms, then release: exactly two taps come out */
      for (t = 0; t < 1200; t += 10) {
          const int o = pcx_btn_conv(&c, PCX_BTN_HOLD, t < 500, t, 0);
          if (o && !last) downs++;
          last = o;
      }
      CHECK(downs == 2 && last == 0); }
    { /* hold: press, then a menu (reset) eats the release; out of it one corrective tap stands back up */
      PcxBtnConv c; unsigned long long t; int downs = 0, last = 0;
      memset(&c, 0, sizeof c);
      for (t = 0; t < 300; t += 10) { const int o = pcx_btn_conv(&c, PCX_BTN_HOLD, 1, t, 0); if (o && !last) downs++; last = o; }
      CHECK(downs == 1 && c.held == 1);
      for (; t < 600; t += 10) pcx_btn_conv(&c, PCX_BTN_HOLD, t < 400, t, 1);   /* released inside the menu */
      CHECK(c.held == 1);
      last = 0; downs = 0;
      for (; t < 1000; t += 10) { const int o = pcx_btn_conv(&c, PCX_BTN_HOLD, 0, t, 0); if (o && !last) downs++; last = o; }
      CHECK(downs == 1 && c.held == 0 && last == 0); }
    { /* hold: a tap cut short by a hitch / menu still counts; pressed again after: no extra tap */
      PcxBtnConv c; int downs = 0, last = 0; unsigned long long t;
      memset(&c, 0, sizeof c);
      CHECK(pcx_btn_conv(&c, PCX_BTN_HOLD, 1, 0, 0) == 1);
      pcx_btn_conv(&c, PCX_BTN_HOLD, 1, 10, 1);
      for (t = 20; t < 400; t += 10) { const int o = pcx_btn_conv(&c, PCX_BTN_HOLD, 1, t, 0); if (o && !last) downs++; last = o; }
      CHECK(downs == 0 && c.held == 1); }
    { /* hold-to-zoom on a toggle (issue #37): whatever the frame time, letting go ends zoomed out */
      static const unsigned fms[] = { 8, 16, 33, 45, 50, 70, 100 };
      int k, j, clicks;
      for (k = 0; k < (int)(sizeof fms / sizeof fms[0]); k++)
      for (j = 0; j < 8; j++) {   /* let go at different points of a frame */
          CHECK(aim_sim(fms[k], 600 + 11 * j, 0, 0, &clicks) == 0 && clicks == 1);
          CHECK(aim_sim(fms[k], 30 + 11 * j, 0, 0, &clicks) == 0 && clicks <= 1);    /* a tap */
          CHECK(aim_sim(fms[k], 600 + 11 * j, 1, 0, &clicks) == 0 && clicks == 2);   /* first click ignored */
      }
      CHECK(aim_sim(33, 600, 0, 1500, &clicks) == 0 && clicks == 2);        /* zoomed again by itself */
      CHECK(aim_sim(33, 600, 0, 3500, &clicks) == 1);                       /* not long after the release */
      CHECK(aim_sim(33, 600, 5, 0, &clicks) == 1 && clicks == PCX_AIM_TRIES); /* gives up */
    }
    { /* zoom hold: let go while the game zooms out on its own (sprint): no click; a menu sends none */
      PcxAimHold a; unsigned long long t; unsigned long f = 0; double fov = 65.0; int o = 0;
      memset(&a, 0, sizeof a);
      pcx_aim_hold(&a, 1, 1, fov, 0, f, 0);
      for (t = 4; t < 300; t += 4) { if (t % 16 == 0) { f++; fov += 1.0; } o |= pcx_aim_hold(&a, 0, fov < 81.5, fov, t, f, 0); }
      CHECK(o == 0);
      memset(&a, 0, sizeof a);
      pcx_aim_hold(&a, 1, 1, 65.0, 0, 0, 0);
      for (t = 4, o = 0; t < 1000; t += 4) o |= pcx_aim_hold(&a, 0, 1, 65.0, t, (unsigned long)(t / 16), 1);
      CHECK(o == 0 && a.armed == 1);
      for (o = 0; t < 1200; t += 4) o |= pcx_aim_hold(&a, 0, 1, 65.0, t, (unsigned long)(t / 16), 0);
      CHECK(o == 1); }   /* out of the menu: the click */
    { /* hold: quick press + release (bounce) ends with the game standing */
      PcxBtnConv c; unsigned long long t; int downs = 0, last = 0;
      memset(&c, 0, sizeof c);
      for (t = 0; t < 600; t += 10) { const int o = pcx_btn_conv(&c, PCX_BTN_HOLD, t < 20, t, 0); if (o && !last) downs++; last = o; }
      CHECK(downs == 2 && c.held == 0); }
    { PcxBtnConv c; memset(&c, 0, sizeof c);
      CHECK(pcx_btn_conv(&c, PCX_BTN_GAME, 1, 0, 0) == 1 && pcx_btn_conv(&c, PCX_BTN_GAME, 0, 1, 0) == 0); }
    /* 7 */
    CHECK(pcx_ui_scale_parse("auto") == 0 && pcx_ui_scale_parse("50") == 75 && pcx_ui_scale_parse("300") == 200);
    CHECK(pcx_ui_scale_pct(0, 1080) == 100 && pcx_ui_scale_pct(0, 1440) == 125 && pcx_ui_scale_pct(0, 2160) == 150);
    CHECK(pcx_ui_scale_pct(150, 720) == 150);
    CHECK(pcx_ui_px(20, 150) == 30);
    /* 8 */
    { PcxFtRing r; unsigned i; memset(&r, 0, sizeof r);
      for (i = 0; i < PCX_FT_N + 5; i++) pcx_ft_push(&r, (float)i);
      CHECK(r.count == PCX_FT_N);
      CHECK(NEAR(pcx_ft_at(&r, 0), 5.0f) && NEAR(pcx_ft_at(&r, PCX_FT_N - 1), (float)(PCX_FT_N + 4)));
      CHECK(pcx_ft_ceiling(&r) >= 124.0f || pcx_ft_ceiling(&r) >= 100.0f);
      memset(&r, 0, sizeof r); pcx_ft_push(&r, 16.0f);
      CHECK(NEAR(pcx_ft_ceiling(&r), 33.3f)); }
    /* 9 */
    CHECK(!strcmp(pcx_nx_label(2, 0), "B") && !strcmp(pcx_nx_label(0, 0), "A"));
    CHECK(!strcmp(pcx_nx_label(2, 1), "A") && !strcmp(pcx_nx_label(3, 1), "Y"));
    CHECK(!strcmp(pcx_nx_label(11, 0), "ZR") && !strcmp(pcx_nx_label(99, 0), ""));
    /* 10: a 2-entry PARAM.SFO */
    { unsigned char b[128]; char out[32];
      memset(b, 0, sizeof b);
      memcpy(b, "\0PSF", 4); put32(b + 4, 0x101);
      put32(b + 8, 20 + 32); put32(b + 12, 20 + 32 + 24); put32(b + 16, 2);
      put16(b + 20, 0); put16(b + 22, 0x0204); put32(b + 24, 10); put32(b + 28, 16); put32(b + 32, 0);
      put16(b + 36, 9); put16(b + 38, 0x0404); put32(b + 40, 4); put32(b + 44, 4); put32(b + 48, 16);
      memcpy(b + 52, "TITLE_ID\0ATTRIBUTE\0", 19);
      memcpy(b + 76, "BCUS98232", 9); put32(b + 92, 1234);
      CHECK(pcx_sfo_get(b, sizeof b, "TITLE_ID", out, sizeof out) && !strcmp(out, "BCUS98232"));
      CHECK(pcx_sfo_get(b, sizeof b, "ATTRIBUTE", out, sizeof out) && !strcmp(out, "1234"));
      CHECK(!pcx_sfo_get(b, sizeof b, "DETAIL", out, sizeof out));
      CHECK(!pcx_sfo_get(b, 10, "TITLE_ID", out, sizeof out));
      { size_t off = 0, len = 0;
        CHECK(pcx_sfo_find(b, sizeof b, "TITLE_ID", &off, &len) && off == 76 && len == 10);
        CHECK(!pcx_sfo_find(b, sizeof b, "DETAIL", &off, &len)); } }
    { const char* ids[] = { "BCUS98232", "BCES00905", NULL }; char nm[64];
      CHECK(pcx_title_match("BCUS98232-SAVE01", ids) && pcx_title_match("BCES00905", ids));
      CHECK(!pcx_title_match("BLUS30000", ids));
      CHECK(pcx_save_retarget("BCES00905_SAVE_0", ids, "BCUS98232", nm, sizeof nm) && !strcmp(nm, "BCUS98232_SAVE_0"));
      CHECK(!pcx_save_retarget("BCUS98232_SAVE_0", ids, "BCUS98232", nm, sizeof nm));
      CHECK(!pcx_save_retarget("BLUS30000_X", ids, "BCUS98232", nm, sizeof nm));
      CHECK(!pcx_save_retarget("BCES00905_SAVE_0", ids, "", nm, sizeof nm)); }
    /* 11 */
    { const char* o = "# order\r\nHD Textures\r\n-Old Mod\r\nfov fix\n";
      CHECK(pcx_mod_rank(o, "HD Textures") == 0);
      CHECK(pcx_mod_rank(o, "old mod") == -1);
      CHECK(pcx_mod_rank(o, "FOV FIX") == 1);
      CHECK(pcx_mod_rank(o, "other") == -2);
      CHECK(pcx_mod_rank(NULL, "x") == -2); }

    printf("%d/%d checks passed\n", s_n - s_fail, s_n);
    return s_fail ? 1 : 0;
}
