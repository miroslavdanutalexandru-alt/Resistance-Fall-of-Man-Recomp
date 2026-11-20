/* lang_fallback.h: English voice-over for a system language the disc has no
 * speech in (English (UK) -> PS3 language 18, no "190" Bink voice group). */
#include "../lang_fallback.h"

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(d) _mkdir(d)
#else
#include <sys/stat.h>
#define MKDIR(d) mkdir(d, 0755)
#endif

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* A Bink header with these audio-track ids; returns the offset of the id table. */
static size_t make_bink(uint8_t* h, size_t cap, const char* sig, const uint32_t* ids, uint32_t n)
{
    memset(h, 0, cap);
    memcpy(h, sig, 4);
    put32(h + 0x28, n);
    size_t off = 0x2C + ((sig[0] == 'K' && sig[3] >= 'i') ? 4 : 0);
    const size_t at = off + (size_t)n * 8;
    for (uint32_t i = 0; i < n; i++) put32(h + at + i * 4, ids[i]);
    return at;
}

static void test_bink(void)
{
    /* R2/R3 layout: effects 0..5, then voice groups per language; no 190. */
    uint32_t ids[48]; uint32_t n = 0;
    for (uint32_t i = 0; i < 6; i++) ids[n++] = i;
    const uint32_t groups[] = { 10, 20, 30, 40, 50, 60, 70, 80, 100, 110, 130, 140, 150, 160 };
    for (size_t g = 0; g < sizeof groups / sizeof groups[0] && n + 3 <= 48; g++)
        for (uint32_t k = 0; k < 3; k++) ids[n++] = groups[g] + k;
    uint8_t h[2048];
    const size_t at = make_bink(h, sizeof h, "KB2j", ids, n);
    const size_t en = at + 9 * 4;                 /* id 20 is the 10th track */

    LfBinkPlan p;
    CHECK(lf_bink_plan(h, sizeof h, 18, &p) == 3);
    CHECK(p.n == 3 && p.off[0] == en && p.val[0] == 190 && p.val[1] == 191 && p.val[2] == 192);

    /* The guest sees 190..192 where 20..22 were, nothing else changes. */
    uint8_t buf[2048];
    memcpy(buf, h, sizeof h);
    lf_bink_apply(&p, 0, buf, sizeof buf);
    CHECK(lf_rd32le(buf + en) == 190 && lf_rd32le(buf + en + 8) == 192);
    CHECK(lf_rd32le(buf + en - 4) == 12 && lf_rd32le(buf + en + 12) == 30);
    CHECK(memcmp(buf, h, en) == 0);

    /* Split reads, one cutting through an id. */
    memcpy(buf, h, sizeof h);
    const size_t cut = en + 2;
    lf_bink_apply(&p, 0, buf, cut);
    lf_bink_apply(&p, cut, buf + cut, sizeof h - cut);
    CHECK(lf_rd32le(buf + en) == 190 && lf_rd32le(buf + en + 4) == 191);
    /* A read past the header is untouched. */
    uint8_t far[16]; memset(far, 0xAB, sizeof far);
    lf_bink_apply(&p, 4096, far, sizeof far);
    CHECK(far[0] == 0xAB && far[15] == 0xAB);

    /* No remap: US English, unknown language, a language the file has. */
    CHECK(lf_bink_plan(h, sizeof h, 1, &p) == 0 && p.n == 0);
    CHECK(lf_bink_plan(h, sizeof h, -1, &p) == 0);
    CHECK(lf_bink_plan(h, sizeof h, 2, &p) == 0);   /* French: 30 present */
    CHECK(lf_bink_plan(h, sizeof h, 8, &p) == 3);   /* Russian: 90 missing */

    /* UK group present -> untouched. */
    ids[n - 1] = 190;
    make_bink(h, sizeof h, "KB2j", ids, n);
    CHECK(lf_bink_plan(h, sizeof h, 18, &p) == 0);

    /* No English group to fall back to. */
    const uint32_t noen[] = { 0, 1, 10, 11, 30, 31 };
    make_bink(h, sizeof h, "BIKi", noen, 6);
    CHECK(lf_bink_plan(h, sizeof h, 18, &p) == 0);

    /* Old BIK layout (no extra field), and a truncated / non-Bink header. */
    const uint32_t two[] = { 0, 20, 21 };
    const size_t at2 = make_bink(h, sizeof h, "BIKi", two, 3);
    CHECK(at2 == 0x2C + 24 && lf_bink_plan(h, sizeof h, 18, &p) == 2 && p.off[0] == at2 + 4);
    CHECK(lf_bink_plan(h, at2 + 8, 18, &p) == 0);
    h[0] = 'X';
    CHECK(lf_bink_plan(h, sizeof h, 18, &p) == 0);

    CHECK(lf_is_bink("C:/g/movies/c00_insom_fmv.BIK") && !lf_is_bink("a.bik.dat") && !lf_is_bink(".bik"));
}

static void test_voice_names(void)
{
    char o[256];
    CHECK(lf_voice_candidate("D:/g/PS3_GAME/USRDIR/streaming_sound.gb.dat", 0, o, sizeof o) &&
          strcmp(o, "D:/g/PS3_GAME/USRDIR/streaming_sound.us.dat") == 0);
    CHECK(lf_voice_candidate("D:/g/PS3_GAME/USRDIR/streaming_sound.gb.dat", 1, o, sizeof o) &&
          strcmp(o, "D:/g/PS3_GAME/USRDIR/streaming_sound.en.dat") == 0);
    CHECK(!lf_voice_candidate("D:/g/PS3_GAME/USRDIR/streaming_sound.gb.dat", 2, o, sizeof o));
    CHECK(lf_voice_candidate("x\\PS3DialogueStream.K.DAT", 0, o, sizeof o) && strcmp(o, "x\\PS3DialogueStream.E.DAT") == 0);
    /* Already English, not speech, no language suffix, too small a buffer. */
    CHECK(!lf_voice_candidate("a/streaming_dialogue.us.dat", 0, o, sizeof o));
    CHECK(!lf_voice_candidate("a/streaming_dialogue.en.dat", 0, o, sizeof o));
    CHECK(!lf_voice_candidate("a/dialogue.e.dat", 0, o, sizeof o));
    CHECK(!lf_voice_candidate("a/level_textures.gb.pkg", 0, o, sizeof o));
    CHECK(!lf_voice_candidate("a/streaming_dialogue.dat", 0, o, sizeof o));
    CHECK(!lf_voice_candidate("a/voice.d1.dat", 0, o, sizeof o));
    CHECK(!lf_voice_candidate("a.gb/voice", 0, o, sizeof o));
    CHECK(!lf_voice_candidate("a/streaming_dialogue.gb.dat", 0, o, 8));

    CHECK(lf_guest_path_ok("/dev_bdvd/PS3_GAME/USRDIR/streaming_dialogue.gb.dat"));
    CHECK(!lf_guest_path_ok("/dev_hdd0/game/BCES00226/USRDIR/streaming_dialogue.gb.dat"));
    CHECK(!lf_guest_path_ok("/DEV_HDD1/cache/x.gb.dat"));
    CHECK(!lf_guest_path_ok(NULL));
}

static void set(const char* k, const char* v)
{
#ifdef _WIN32
    _putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}

static void write_bink(const char* path, const uint32_t* ids, uint32_t n)
{
    uint8_t h[2048];
    make_bink(h, sizeof h, "KB2j", ids, n);
    FILE* f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f) { fwrite(h, 1, sizeof h, f); fclose(f); }
}

/* Disc-wide check: a dubbed language missing in one movie is not a reason to
 * play English there (R2 c08_timer_fmv has only 0/20/100/110). */
static void test_disc(void)
{
    MKDIR("movies");
    MKDIR("movies/logo");
    MKDIR("movies/c00_intro");
    MKDIR("movies/c08_timer_fmv");
    const uint32_t logo[] = { 0, 1 };                       /* no speech: ignored */
    const uint32_t full[] = { 0, 1, 10, 11, 20, 21, 30, 31, 40, 41 };
    const uint32_t timer[] = { 0, 20, 100, 110 };
    write_bink("movies/logo/logo.bik", logo, 2);
    write_bink("movies/c00_intro/c00_intro.bik", full, 10);
    write_bink("movies/c08_timer_fmv/c08_timer_fmv.bik", timer, 4);
    const char* timer_p = "movies/c08_timer_fmv/c08_timer_fmv.bik";

    uint8_t h[2048]; LfBinkPlan p;
    make_bink(h, sizeof h, "KB2j", timer, 4);
    CHECK(lf_bink_plan(h, sizeof h, 2, &p) == 1);           /* the movie alone lacks French ... */
    CHECK(lf_disc_voice_lang(timer_p, 2) == 1);             /* ... but the disc dubs it: no remap */
    CHECK(lf_disc_voice_lang(timer_p, 3) == 1);             /* Spanish (40) */
    CHECK(lf_disc_voice_lang(timer_p, 8) == 0);             /* Russian (90) nowhere: remap */
    CHECK(lf_disc_voice_lang(timer_p, 18) == 0);
    CHECK(lf_disc_voice_lang("movies/logo/logo.bik", 8) == 0);
    /* A movie by itself in a dir with no speech movies: unknown. */
    MKDIR("lone");
    MKDIR("lone/a");
    write_bink("lone/a/a.bik", logo, 2);
    CHECK(lf_disc_voice_lang("lone/a/a.bik", 8) == -1);
    CHECK(lf_disc_voice_lang("nowhere/x/x.bik", 8) == -1);
    /* Flat layout: <movies>/<name>.bik. */
    MKDIR("flat");
    write_bink("flat/c00.bik", full, 10);
    write_bink("flat/c08.bik", timer, 4);
    CHECK(lf_disc_voice_lang("flat/c08.bik", 2) == 1 && lf_disc_voice_lang("flat/c08.bik", 8) == 0);

    /* The policy: English variants always, others only disc-wide unless mode 2. */
    set("PS3_LANG_VOICE_FALLBACK", NULL);
    CHECK(lf_mode() == 1);
    CHECK(lf_voice_policy(18) == 1 && lf_voice_policy(2) == 2 && lf_voice_policy(1) == 0 && lf_voice_policy(-1) == 0);
    set("PS3_LANG_VOICE_FALLBACK", "2");
    CHECK(lf_mode() == 2 && lf_voice_policy(2) == 1 && lf_voice_policy(18) == 1 && lf_voice_policy(1) == 0);
    set("PS3_LANG_VOICE_FALLBACK", "0");
    CHECK(lf_mode() == 0 && lf_voice_policy(18) == 0);
    set("PS3_LANG_VOICE_FALLBACK", NULL);
}

static void test_env(void)
{
    set("PS3_LANG_RESOLVED", NULL);
    CHECK(lf_system_language() == -1);
    set("PS3_LANG_RESOLVED", "18");
    CHECK(lf_system_language() == 18);
    set("PS3_LANG_RESOLVED", "18x");
    CHECK(lf_system_language() == -1);
    set("PS3_LANG_RESOLVED", "20");
    CHECK(lf_system_language() == -1);
    set("PS3_LANG_VOICE_FALLBACK", NULL);
    CHECK(lf_enabled());
    set("PS3_LANG_VOICE_FALLBACK", "0");
    CHECK(!lf_enabled());
    set("PS3_LANG_VOICE_FALLBACK", "1");
    CHECK(lf_enabled());
    set("PS3_LANG_VOICE_FALLBACK", NULL);
}

int main(void)
{
    test_bink();
    test_voice_names();
    test_env();
    test_disc();
    if (fails) { printf("test_lang_fallback: %d failure(s)\n", fails); return 1; }
    printf("test_lang_fallback: OK\n");
    return 0;
}
