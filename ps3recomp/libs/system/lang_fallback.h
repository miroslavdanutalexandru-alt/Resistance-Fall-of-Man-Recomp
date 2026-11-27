/*
 * ps3recomp - spoken-language fallback for system languages a disc has no
 * voice-over for (header only: sys_fs.c, ppu_fs.cpp and the tests use it).
 *
 * A PAL disc usually ships one English voice track. A host set to English (UK)
 * reports PS3 language 18 (CELL_SYSUTIL_LANG_ENGLISH_GB); the game then shows
 * its UK text but asks for UK speech that is not on the disc:
 *   - Bink movies (Resistance 2 / 3) carry the voice-over as audio tracks
 *     (lang + 1) * 10 + k. There is no 190 group, so cutscenes play without
 *     dialogue. lf_bink_plan() renames the English group (20 + k) to the
 *     requested one in the header the guest reads, so the player finds it.
 *   - Loose speech files "<name>.<xx>.<ext>": a read-only open that fails is
 *     retried with the English suffix (".us." / ".en.", or ".e." for the
 *     one-letter scheme) when that file exists (lf_voice_candidate()).
 * The text language is left alone. cellSysutil publishes the language it
 * reports as PS3_LANG_RESOLVED.
 *
 * Which languages get the English voice (lf_voice_policy()): English variants
 * (UK) always, since English speech is their voice-over. Any other language only
 * when the disc has no dub for it at all (lf_disc_voice_lang() over the speech
 * movies): a dubbed campaign keeps a movie that lacks its group as it was rather
 * than switch to English mid-game (R2 c08_timer_fmv has only 0/20/100/110).
 * PS3_LANG_VOICE_FALLBACK: 0 = off, 1 = the above (default), 2 = also any single
 * movie / file missing the language (opt-in).
 */
#ifndef PS3RECOMP_LANG_FALLBACK_H
#define PS3RECOMP_LANG_FALLBACK_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#else
#include <dirent.h>
#endif
#include <stdio.h>

#define LF_LANG_ENGLISH_US 1
#define LF_LANG_ENGLISH_GB 18
#define LF_BINK_MAX_REMAP  16

/* PS3_LANG_VOICE_FALLBACK: 0 off, 1 default, 2 per movie / file (opt-in). */
static inline int lf_mode(void)
{
    const char* e = getenv("PS3_LANG_VOICE_FALLBACK");
    if (!e || !*e) return 1;
    if (e[0] == '0') return 0;
    return e[0] == '2' ? 2 : 1;
}
static inline int lf_enabled(void) { return lf_mode() != 0; }

static inline int lf_english_variant(int lang) { return lang == LF_LANG_ENGLISH_GB; }

/* Whether speech missing in lang may be replaced by English: 1 yes, 0 no,
 * 2 only if the disc has no dub for lang anywhere (ask lf_disc_voice_lang()). */
static inline int lf_voice_policy(int lang)
{
    const int m = lf_mode();
    if (!m || lang < 0 || lang > 19 || lang == LF_LANG_ENGLISH_US) return 0;
    if (m == 2 || lf_english_variant(lang)) return 1;
    return 2;
}

/* The PS3 system language the title was given, or -1 when not known yet. */
static inline int lf_system_language(void)
{
    const char* e = getenv("PS3_LANG_RESOLVED");
    if (!e || !*e) return -1;
    char* end = NULL;
    const long v = strtol(e, &end, 10);
    return (end && *end == '\0' && v >= 0 && v <= 19) ? (int)v : -1;
}

static inline char lf_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

static inline int lf_has_ci(const char* s, size_t n, const char* needle)
{
    const size_t m = strlen(needle);
    for (size_t i = 0; i + m <= n; i++) {
        size_t k = 0;
        while (k < m && lf_lower(s[i + k]) == needle[k]) k++;
        if (k == m) return 1;
    }
    return 0;
}

/* A failed open of this guest path may be retried in English. Not on the HDD:
 * a miss in the game's HDD cache is the normal "not cached yet" path (the disc
 * copy is opened next), so English must not be substituted there. */
static inline int lf_guest_path_ok(const char* gpath)
{
    return gpath && !(lf_has_ci(gpath, strlen(gpath) < 8 ? strlen(gpath) : 8, "/dev_hdd"));
}

/* For a speech file path "<dir>/<stem>.<xx>.<ext>" whose language suffix is
 * not English, the English path to try instead (index 0, 1, ...). Returns 1 and
 * fills out, or 0 when there is no (further) candidate. Only stems that name
 * speech or sound data qualify. */
static inline int lf_voice_candidate(const char* path, int index, char* out, size_t n)
{
    if (!path || !out || n == 0 || index < 0) return 0;
    const size_t len = strlen(path);
    if (len + 1 > n) return 0;
    size_t base = len;
    while (base > 0 && path[base - 1] != '/' && path[base - 1] != '\\') base--;
    size_t ext = len;
    while (ext > base && path[ext - 1] != '.') ext--;
    if (ext <= base + 1) return 0;
    ext--;                                        /* the '.' before the extension */
    size_t suf = ext;
    while (suf > base && path[suf - 1] != '.') suf--;
    if (suf <= base + 1) return 0;                /* no "<stem>." before the suffix */
    const size_t sl = ext - suf;                  /* suffix length */
    if (sl < 1 || sl > 2) return 0;
    for (size_t i = suf; i < ext; i++) {
        const char c = lf_lower(path[i]);
        if (c < 'a' || c > 'z') return 0;
    }
    static const char* const kinds[] = { "dialog", "voice", "speech", "lipsync", "sound", "vo_" };
    int speech = 0;
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0] && !speech; i++)
        speech = lf_has_ci(path + base, suf - base, kinds[i]);
    if (!speech) return 0;
    const char* const two[] = { "us", "en" };
    const char* const one[] = { "e" };
    const char* const* cand = sl == 2 ? two : one;
    const int ncand = sl == 2 ? 2 : 1;
    for (int i = 0; i < ncand; i++)
        if (lf_lower(path[suf]) == cand[i][0] && (sl == 1 || lf_lower(path[suf + 1]) == cand[i][1]))
            return 0;                             /* already English */
    if (index >= ncand) return 0;
    const char* c = cand[index];
    memcpy(out, path, len + 1);
    for (size_t j = 0; j < sl; j++)
        out[suf + j] = (path[suf + j] >= 'A' && path[suf + j] <= 'Z') ? (char)(c[j] - 'a' + 'A') : c[j];
    return 1;
}

/* ---- Bink audio-track remap ------------------------------------------- */

typedef struct LfBinkPlan {
    int n;
    uint32_t off[LF_BINK_MAX_REMAP];   /* file offset of a little-endian track id */
    uint32_t val[LF_BINK_MAX_REMAP];   /* the id the guest sees there */
} LfBinkPlan;

static inline uint32_t lf_rd32le(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* hdr = the first len bytes of a .bik file. Fills plan with the English voice
 * tracks (20..29) renamed to lang's group ((lang + 1) * 10 ..) when the file
 * has English speech but none in lang. Returns the number of ids renamed. */
/* Offset of the audio-track id table in a .bik header (*ntr = track count), 0 if none. */
static inline size_t lf_bink_ids(const uint8_t* hdr, size_t len, uint32_t* ntr_out)
{
    if (!hdr || len < 0x30) return 0;
    const int bik = hdr[0] == 'B' && hdr[1] == 'I' && hdr[2] == 'K';
    const int kb2 = hdr[0] == 'K' && hdr[1] == 'B' && hdr[2] == '2';
    if (!bik && !kb2) return 0;
    const uint32_t ntr = lf_rd32le(hdr + 0x28);
    if (ntr == 0 || ntr > 256) return 0;
    size_t off = 0x2C;
    if (kb2 && hdr[3] >= 'i') off += 4;
    const size_t ids = off + (size_t)ntr * 8;     /* after the per-track buffer sizes and rate/flags */
    if (ids + (size_t)ntr * 4 > len) return 0;
    *ntr_out = ntr;
    return ids;
}

static inline int lf_bink_plan(const uint8_t* hdr, size_t len, int lang, LfBinkPlan* plan)
{
    if (!plan) return 0;
    plan->n = 0;
    if (lang < 0 || lang > 19 || lang == LF_LANG_ENGLISH_US) return 0;
    uint32_t ntr = 0;
    const size_t ids = lf_bink_ids(hdr, len, &ntr);
    if (!ids) return 0;
    const uint32_t want = (uint32_t)(lang + 1) * 10u;
    int english = 0;
    for (uint32_t i = 0; i < ntr; i++) {
        const uint32_t id = lf_rd32le(hdr + ids + i * 4);
        if (id >= want && id < want + 10) return 0;   /* the disc has this language */
        if (id >= 20 && id < 30) english++;
    }
    if (!english) return 0;
    for (uint32_t i = 0; i < ntr && plan->n < LF_BINK_MAX_REMAP; i++) {
        const uint32_t id = lf_rd32le(hdr + ids + i * 4);
        if (id < 20 || id >= 30) continue;
        plan->off[plan->n] = (uint32_t)(ids + i * 4);
        plan->val[plan->n] = want + (id - 20);
        plan->n++;
    }
    return plan->n;
}

/* Rewrites the planned ids inside buf, which holds file bytes [pos, pos + n). */
static inline void lf_bink_apply(const LfBinkPlan* plan, uint64_t pos, uint8_t* buf, size_t n)
{
    if (!plan || !buf) return;
    for (int i = 0; i < plan->n; i++)
        for (uint32_t b = 0; b < 4; b++) {
            const uint64_t o = (uint64_t)plan->off[i] + b;
            if (o >= pos && o < pos + n) buf[o - pos] = (uint8_t)(plan->val[i] >> (8 * b));
        }
}

static inline int lf_is_bink(const char* path)
{
    const size_t l = path ? strlen(path) : 0;
    return l > 4 && path[l - 4] == '.' && lf_lower(path[l - 3]) == 'b' &&
           lf_lower(path[l - 2]) == 'i' && lf_lower(path[l - 1]) == 'k';
}

/* One .bik header: -1 = not a speech movie (no English group 20..29), else
 * whether it has lang's group. */
static inline int lf_bink_file_has_lang(const char* path, int lang)
{
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t hdr[2048];
    const size_t got = fread(hdr, 1, sizeof hdr, f);
    fclose(f);
    uint32_t ntr = 0;
    const size_t ids = lf_bink_ids(hdr, got, &ntr);
    if (!ids) return -1;
    const uint32_t want = (uint32_t)(lang + 1) * 10u;
    int english = 0, has = 0;
    for (uint32_t i = 0; i < ntr; i++) {
        const uint32_t id = lf_rd32le(hdr + ids + i * 4);
        if (id >= 20 && id < 30) english = 1;
        if (id >= want && id < want + 10) has = 1;
    }
    return english ? has : -1;
}

/* Does the disc dub lang at all? Looks at the speech movies around movie_path:
 * "<movies>/<name>/<name>.bik" (R2 / R3) or "<movies>/<name>.bik". Returns 1 if
 * any has lang's voice group, 0 if speech movies were found and none has it,
 * -1 if none were found (unknown). Reads at most 512 headers of 2 KB. */
static inline int lf_disc_voice_lang(const char* movie_path, int lang)
{
    if (!movie_path || lang < 0 || lang > 19) return -1;
    char dir[1024];
    const size_t l = strlen(movie_path);
    if (l >= sizeof dir) return -1;
    memcpy(dir, movie_path, l + 1);
    int roots = 0;                                /* the movie's own dir, then its parent */
    char root[2][1024];
    for (int up = 0; up < 2; up++) {
        size_t k = strlen(dir);
        while (k > 0 && dir[k - 1] != '/' && dir[k - 1] != '\\') k--;
        if (k == 0) break;
        dir[k - 1] = '\0';
        snprintf(root[roots++], sizeof root[0], "%s", dir);
    }
    int speech = 0, scanned = 0;
    for (int r = roots - 1; r >= 0; r--) {         /* the parent first: it sees every movie */
        char p[1200];
#ifdef _WIN32
        struct _finddata_t fd;
        snprintf(p, sizeof p, "%s/*", root[r]);
        intptr_t h = _findfirst(p, &fd);
        if (h == -1) continue;
        do {
            const char* nm = fd.name;
            const int isdir = (fd.attrib & _A_SUBDIR) != 0;
#else
        DIR* d = opendir(root[r]);
        if (!d) continue;
        struct dirent* de;
        while ((de = readdir(d)) != NULL) {
            const char* nm = de->d_name;
            int isdir = 0;
            { char q[1200]; snprintf(q, sizeof q, "%s/%s", root[r], nm);
              DIR* sd = opendir(q); if (sd) { isdir = 1; closedir(sd); } }
#endif
            if (nm[0] != '.' && scanned < 512) {
                if (isdir) snprintf(p, sizeof p, "%s/%s/%s.bik", root[r], nm, nm);
                else snprintf(p, sizeof p, "%s/%s", root[r], nm);
                if (lf_is_bink(p)) {
                    scanned++;
                    const int v = lf_bink_file_has_lang(p, lang);
                    if (v == 1) {
#ifdef _WIN32
                        _findclose(h);
#else
                        closedir(d);
#endif
                        return 1;
                    }
                    if (v == 0) speech++;
                }
            }
#ifdef _WIN32
        } while (_findnext(h, &fd) == 0);
        _findclose(h);
#else
        }
        closedir(d);
#endif
        if (speech) break;                        /* the parent had speech movies: done */
    }
    return speech ? 0 : -1;
}

#endif /* PS3RECOMP_LANG_FALLBACK_H */
