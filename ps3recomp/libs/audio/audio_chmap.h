/*
 * ps3recomp - PS3 audio channels -> host speaker layout (5.1 / 7.1 output).
 *
 * A cellAudio port carries 1, 2 or 8 channels; the 8-channel order is the
 * PS3's L R C LFE Ls Rs Lb Rb (Ls/Rs = the surround pair of a 5.1 mix, Lb/Rb
 * = the extra back pair of 7.1). A Windows multichannel stream interleaves its
 * channels in ascending SPEAKER_* bit order: a 7.1 device (0x63F) is
 * FL FR FC LFE BL BR SL SR, so copying the PS3 block straight through would
 * put the 5.1 surrounds on the 7.1 back speakers. audio_chmap_build places
 * every PS3 channel on its own speaker of the host mask, folding it into the
 * nearest one the host layout lacks (5.1 back-surround 0x3F vs side-surround
 * 0x60F, quad, ...).
 *
 * Header-only, no OS calls: shared by cellAudio.c and the unit test.
 */
#ifndef PS3RECOMP_AUDIO_CHMAP_H
#define PS3RECOMP_AUDIO_CHMAP_H

#include <string.h>

/* Windows SPEAKER_* bits (ksmedia.h) */
#define AUDIO_SPK_FL  0x001u
#define AUDIO_SPK_FR  0x002u
#define AUDIO_SPK_FC  0x004u
#define AUDIO_SPK_LFE 0x008u
#define AUDIO_SPK_BL  0x010u
#define AUDIO_SPK_BR  0x020u
#define AUDIO_SPK_SL  0x200u
#define AUDIO_SPK_SR  0x400u

#define AUDIO_MASK_STEREO   0x003u
#define AUDIO_MASK_QUAD     0x033u
#define AUDIO_MASK_5POINT1  0x03Fu   /* KSAUDIO_SPEAKER_5POINT1 (back pair) */
#define AUDIO_MASK_5POINT1S 0x60Fu   /* KSAUDIO_SPEAKER_5POINT1_SURROUND (side pair) */
#define AUDIO_MASK_7POINT1  0x63Fu   /* KSAUDIO_SPEAKER_7POINT1_SURROUND */

#define AUDIO_CHMAP_MAX 8

typedef struct AudioChMap {
    unsigned out_ch;                               /* host channels (popcount of mask) */
    unsigned mask;                                 /* host SPEAKER_* mask */
    float m[AUDIO_CHMAP_MAX][AUDIO_CHMAP_MAX];     /* [PS3 8ch source][host channel] gain */
} AudioChMap;

static inline unsigned audio_chmap_popcount(unsigned v)
{
    unsigned n = 0;
    for (; v; v &= v - 1) n++;
    return n;
}

/* Host channel index of speaker bit `spk` in `mask` (ascending bit order), -1 if absent. */
static inline int audio_chmap_index(unsigned mask, unsigned spk)
{
    if (!(mask & spk)) return -1;
    return (int)audio_chmap_popcount(mask & (spk - 1));
}

/* The SPEAKER_* mask a stream of `ch` channels is opened with when the
 * device's own mask does not fit (or is unknown). */
static inline unsigned audio_chmap_default_mask(unsigned ch)
{
    switch (ch) {
    case 4: return AUDIO_MASK_QUAD;
    case 6: return AUDIO_MASK_5POINT1;
    case 8: return AUDIO_MASK_7POINT1;
    default: return AUDIO_MASK_STEREO;
    }
}

/* Host layout to open. dev_ch/dev_mask: the device's shared-mode mix format
 * (dev_mask 0 = a plain WAVEFORMATEX, no mask). force: 0 = follow the device,
 * 2 / 6 / 8 = that many channels whatever the device has (Windows' audio
 * engine converts). Anything the matrix cannot place (no front pair, more than
 * 8 channels, a speaker this header does not know) opens stereo, as before.
 * Returns the channel count; *out_mask gets the mask. */
static inline unsigned audio_chmap_pick(unsigned dev_ch, unsigned dev_mask, int force, unsigned* out_mask)
{
    const unsigned known = AUDIO_MASK_7POINT1;
    unsigned ch = dev_ch, mask = dev_mask;
    if (force == 2 || force == 6 || force == 8) {
        ch = (unsigned)force;
        if (dev_ch != ch || audio_chmap_popcount(dev_mask) != ch || (dev_mask & ~known)) mask = audio_chmap_default_mask(ch);
    } else {
        if (!mask || audio_chmap_popcount(mask) != ch) mask = audio_chmap_default_mask(ch);
        if (ch > 8 || ch < 3 || (mask & ~known) || (mask & AUDIO_MASK_STEREO) != AUDIO_MASK_STEREO) { ch = 2; mask = AUDIO_MASK_STEREO; }
    }
    if (out_mask) *out_mask = mask;
    return ch;
}

/* Build the PS3 8ch -> host matrix. swap_surround != 0 swaps which PS3 pair
 * (Ls/Rs or Lb/Rb) goes to the host side and back speakers. */
static inline void audio_chmap_build(AudioChMap* cm, unsigned mask, int swap_surround)
{
    memset(cm, 0, sizeof(*cm));
    cm->mask = mask;
    cm->out_ch = audio_chmap_popcount(mask);
    if (cm->out_ch > AUDIO_CHMAP_MAX) cm->out_ch = AUDIO_CHMAP_MAX;
    const int fl = audio_chmap_index(mask, AUDIO_SPK_FL), fr = audio_chmap_index(mask, AUDIO_SPK_FR);
    const int fc = audio_chmap_index(mask, AUDIO_SPK_FC), lfe = audio_chmap_index(mask, AUDIO_SPK_LFE);
    const int bl = audio_chmap_index(mask, AUDIO_SPK_BL), br = audio_chmap_index(mask, AUDIO_SPK_BR);
    const int sl = audio_chmap_index(mask, AUDIO_SPK_SL), sr = audio_chmap_index(mask, AUDIO_SPK_SR);
    if (fl < 0 || fr < 0) return;

    cm->m[0][fl] = 1.0f;
    cm->m[1][fr] = 1.0f;
    if (fc >= 0) cm->m[2][fc] = 1.0f;
    else { cm->m[2][fl] = 0.707f; cm->m[2][fr] = 0.707f; }
    if (lfe >= 0) cm->m[3][lfe] = 1.0f;
    else { cm->m[3][fl] = 0.5f; cm->m[3][fr] = 0.5f; }

    /* surround pair (PS3 4/5) prefers the side speakers, back pair (6/7) the
     * back ones; each falls back to the other pair, then into the fronts with
     * the stereo fold's gains */
    const int s_src = swap_surround ? 6 : 4, b_src = swap_surround ? 4 : 6;
    if (sl >= 0 && sr >= 0)      { cm->m[s_src][sl] = 1.0f; cm->m[s_src + 1][sr] = 1.0f; }
    else if (bl >= 0 && br >= 0) { cm->m[s_src][bl] = 1.0f; cm->m[s_src + 1][br] = 1.0f; }
    else                         { cm->m[s_src][fl] = 0.5f; cm->m[s_src + 1][fr] = 0.5f; }
    if (bl >= 0 && br >= 0)      { cm->m[b_src][bl] = 1.0f; cm->m[b_src + 1][br] = 1.0f; }
    else if (sl >= 0 && sr >= 0) { cm->m[b_src][sl] = 1.0f; cm->m[b_src + 1][sr] = 1.0f; }
    else                         { cm->m[b_src][fl] = 0.3f; cm->m[b_src + 1][fr] = 0.3f; }
}

/* Source rows for a port of `nch` channels: 1 = mono (both fronts), 2 = L/R,
 * 3..8 = the first nch rows of the 8ch matrix. */
static inline void audio_chmap_rows(const AudioChMap* cm, unsigned nch, float rows[AUDIO_CHMAP_MAX][AUDIO_CHMAP_MAX])
{
    memset(rows, 0, sizeof(float) * AUDIO_CHMAP_MAX * AUDIO_CHMAP_MAX);
    if (nch == 1) {
        for (unsigned o = 0; o < AUDIO_CHMAP_MAX; o++) rows[0][o] = cm->m[0][o] + cm->m[1][o];
        return;
    }
    if (nch > AUDIO_CHMAP_MAX) nch = AUDIO_CHMAP_MAX;
    memcpy(rows, cm->m, sizeof(float) * AUDIO_CHMAP_MAX * nch);
}

/* Master volume and mono for a multichannel block (pcx_audio_apply is the
 * stereo version). Mono puts the front + centre + surround content on both
 * fronts, silences the other main speakers and leaves the LFE alone. */
static inline void audio_chmap_apply_volume(float* buf, unsigned frames, const AudioChMap* cm, float gain, int mono)
{
    if (gain >= 0.999f && gain <= 1.001f && !mono) return;
    const unsigned n = cm->out_ch;
    const int fl = audio_chmap_index(cm->mask, AUDIO_SPK_FL), fr = audio_chmap_index(cm->mask, AUDIO_SPK_FR);
    const int lfe = audio_chmap_index(cm->mask, AUDIO_SPK_LFE);
    for (unsigned i = 0; i < frames; i++) {
        float* f = buf + (size_t)i * n;
        if (mono && fl >= 0 && fr >= 0) {
            float m = 0.5f * (f[fl] + f[fr]);
            for (unsigned c = 0; c < n; c++)
                if ((int)c != fl && (int)c != fr && (int)c != lfe) { m += 0.5f * f[c]; f[c] = 0.0f; }
            f[fl] = f[fr] = m;
        }
        for (unsigned c = 0; c < n; c++) f[c] *= gain;
    }
}

static inline const char* audio_chmap_name(unsigned mask)
{
    switch (mask) {
    case AUDIO_MASK_STEREO:   return "stereo";
    case AUDIO_MASK_QUAD:     return "quad";
    case AUDIO_MASK_5POINT1:  return "5.1";
    case AUDIO_MASK_5POINT1S: return "5.1 (side)";
    case AUDIO_MASK_7POINT1:  return "7.1";
    default:                  return "custom";
    }
}

#endif /* PS3RECOMP_AUDIO_CHMAP_H */
