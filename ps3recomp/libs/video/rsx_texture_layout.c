/*
 * ps3recomp - RSX texture layout (see rsx_texture_layout.h)
 */
#include "rsx_texture_layout.h"

#include <stdio.h>    /* fprintf      */
#include <stdlib.h>   /* getenv, atoi */
#include <string.h>   /* memcpy       */
#include "../../../project/port_native_summary.h"   /* port_native_parse_bool: the shared switch rules */

u32 rsx_log2_ceil(u32 v)
{
    u32 l = 0;
    while ((1u << l) < v) l++;
    return l;
}

u32 rsx_swizzle_offset(u32 x, u32 y, u32 log2w, u32 log2h)
{
    u32 off = 0, shift = 0;
    while (log2w && log2h) {
        off |= (x & 1u) << shift; x >>= 1; shift++;
        off |= (y & 1u) << shift; y >>= 1; shift++;
        log2w--; log2h--;
    }
    off |= (x | y) << shift;     /* only one of x/y still has bits */
    return off;
}

u32 rsx_swizzle_offset_3d(u32 x, u32 y, u32 z, u32 log2w, u32 log2h, u32 log2d)
{
    u32 off = 0, shift = 0;
    while (log2w || log2h || log2d) {
        if (log2w) { off |= (x & 1u) << shift; x >>= 1; shift++; log2w--; }
        if (log2h) { off |= (y & 1u) << shift; y >>= 1; shift++; log2h--; }
        if (log2d) { off |= (z & 1u) << shift; z >>= 1; shift++; log2d--; }
    }
    return off;
}

void rsx_texture_layout(u32 rsx_fmt, u32 w, u32 h, rsx_tex_layout* out)
{
    rsx_texture_layout_pitched(rsx_fmt, w, h, 0, out);
}

void rsx_texture_layout_pitched(u32 rsx_fmt, u32 w, u32 h, u32 pitch,
                                rsx_tex_layout* out)
{
    if (!out) return;

    /* Format classes (base = fmt & 0x9F, masking off the LN/UN flag bits).
     * The LBP loading screen exercises the first group: 0x85 A8R8G8B8
     * (swizzled UI art), 0x8B G8B8 (the 1024x2048 linear font atlas -- without
     * it no text renders at all), 0x86/87/88 DXT1/23/45 (512x512 detail and
     * LUT layers bound on every draw). The rest are what the live draw engine
     * reached for once a real title's frame was being drawn. */
    u32 basef = rsx_fmt & 0x9Fu;
    u32 src_bpp = 1, dst_bpp = 1;

    out->base_format    = basef;
    out->compressed     = 0;
    out->block_bytes    = 0;
    out->fmt            = RSX_TEXFMT_R8;

    switch (basef) {
    /* Four bytes, A,R,G,B. D8R8G8B8 is the same image with no alpha channel:
     * the byte is there but means nothing, so the decode forces it to one
     * rather than letting whatever the guest left behind modulate a draw. */
    case 0x85: case 0x9E:
        out->fmt = RSX_TEXFMT_R8G8B8A8; src_bpp = dst_bpp = 4; break;
    /* Two 8-bit channels. HILO is a normal-map pair; HILO_S8's channels are
     * signed, which is not modelled -- neither reference backend samples it,
     * so there is nothing to mirror and unorm is the conservative reading. */
    case 0x8B: case 0x8C: case 0x8D:
        out->fmt = RSX_TEXFMT_R8G8;     src_bpp = dst_bpp = 2; break;
    /* Packed 16-bit colour: two source bytes unpacked to four host ones. */
    case 0x82: case 0x83: case 0x84: case 0x97: case 0x9D:
        out->fmt = RSX_TEXFMT_R8G8B8A8; src_bpp = 2; dst_bpp = 4; break;
    case 0x86: out->fmt = RSX_TEXFMT_BC1; out->compressed = 1; out->block_bytes = 8;  break;
    case 0x87: out->fmt = RSX_TEXFMT_BC2; out->compressed = 1; out->block_bytes = 16; break;
    case 0x88: out->fmt = RSX_TEXFMT_BC3; out->compressed = 1; out->block_bytes = 16; break;
    /* Depth read as colour. Both sample as a single red channel: a shader
     * reading one of these wants the depth value, and spreading it across RGB
     * would only make the crossbar's job ambiguous. DEPTH24_D8's 24-bit
     * integer has no host format, so it is normalised into a float, which is
     * exact -- 24 bits fit a float32 mantissa. */
    case 0x90: out->fmt = RSX_TEXFMT_R32F;  src_bpp = dst_bpp = 4; break;
    case 0x92: case 0x94:
        out->fmt = RSX_TEXFMT_R16;      src_bpp = dst_bpp = 2; break;
    case 0x95: out->fmt = RSX_TEXFMT_R16G16; src_bpp = dst_bpp = 4; break;
    /* Float render targets sampled back as textures. Host formats take these
     * bit for bit; only the guest's big-endian component order changes. */
    case 0x9A: out->fmt = RSX_TEXFMT_R16G16B16A16F; src_bpp = dst_bpp = 8;  break;
    case 0x9B: out->fmt = RSX_TEXFMT_R32G32B32A32F; src_bpp = dst_bpp = 16; break;
    case 0x9C: out->fmt = RSX_TEXFMT_R32F;   src_bpp = dst_bpp = 4; break;
    case 0x9F: out->fmt = RSX_TEXFMT_R16G16F; src_bpp = dst_bpp = 4; break;
    default:   break;   /* R8, one byte per texel -- the pre-existing fallback */
    }

    if (out->compressed) {
        /* Compressed data is stored as linear rows of 4x4 blocks, and is never
         * Morton-swizzled on RSX. */
        out->bytes_per_texel = 0;
        out->swizzled = 0;
        out->row_bytes = ((w + 3u) / 4u) * out->block_bytes;
        out->rows      = (h + 3u) / 4u;
        out->dst_bytes_per_texel = 0;
        out->dst_row_bytes       = out->row_bytes;
        /* Resistance port: a LINEAR compressed image keeps the pitch register
         * as its source stride between block rows; the host copy stays
         * packed (dst_row_bytes). */
        if ((rsx_fmt & 0x20u) && pitch > out->row_bytes)
            out->row_bytes = pitch;
    } else {
        /* Swizzled unless the LN bit is set. The hardware requires power-of-two
         * dimensions to swizzle, so a NPOT image is linear regardless. */
        out->swizzled  = !(rsx_fmt & 0x20u) &&
                         w && h &&
                         (w & (w - 1u)) == 0u && (h & (h - 1u)) == 0u;
        out->bytes_per_texel = src_bpp;
        out->row_bytes = w * src_bpp;
        /* A swizzled image has no rows to space out, so the pitch register
         * only applies to a linear one. */
        if (pitch > out->row_bytes && (rsx_fmt & 0x20u))
            out->row_bytes = pitch;
        out->rows      = h;
        out->dst_bytes_per_texel = dst_bpp;
        out->dst_row_bytes       = w * dst_bpp;
    }

    out->face_bytes = out->row_bytes * out->rows;
}

u32 rsx_texture_mip_chain(u32 rsx_fmt, u32 w, u32 h, u32 levels, u32 pitch,
                          rsx_tex_level* out)
{
    if (!out || !w || !h) return 0;
    if (levels == 0) levels = 1;
    if (levels > RSX_MAX_TEXTURE_LEVELS) levels = RSX_MAX_TEXTURE_LEVELS;

    u32 mw = w, mh = h, off = 0, n = 0;
    while (n < levels) {
        rsx_tex_level* lv = &out[n];
        lv->w = mw; lv->h = mh; lv->offset = off;
        /* Only level 0 has a pitch register; the rest are packed at their own
         * width, as the live draw engine's texture_source_span walks them. */
        rsx_texture_layout_pitched(rsx_fmt, mw, mh, n == 0 ? pitch : 0, &lv->tl);
        off += lv->tl.face_bytes;
        n++;
        if (mw == 1 && mh == 1) break;   /* no level below 1x1 */
        if (mw > 1) mw >>= 1;
        if (mh > 1) mh >>= 1;
    }
    return n;
}

u32 rsx_texture_cube_face_stride(u32 rsx_fmt, u32 w, u32 h, u32 levels,
                                 u32 pitch)
{
    rsx_tex_level lv[RSX_MAX_TEXTURE_LEVELS];
    const u32 n = rsx_texture_mip_chain(rsx_fmt, w, h, levels, pitch, lv);
    if (!n) return 0;
    const u32 total = lv[n - 1].offset + lv[n - 1].tl.face_bytes;
    return (total + 127u) & ~127u;
}

int rsx_texture_argb_is_rgba(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char* e = getenv("TEX_RGBA");
        cached = e ? atoi(e) : 0;
    }
    return cached;
}

/* A 5- or 6-bit field widened to 8 bits, and a 4-bit one, exactly as the live
 * draw engine's decode_texel widens them. */
static u8 expand5(u32 v) { return (u8)(v * 255u / 31u); }
static u8 expand6(u32 v) { return (u8)(v * 255u / 63u); }
static u8 expand4(u32 v) { return (u8)(v * 17u); }

/* Guest half-words and words are big-endian. */
static u32 be16(const u8* p) { return ((u32)p[0] << 8) | p[1]; }
static u32 be32(const u8* p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* `n` components of `width` bytes, big-endian in guest memory and
 * little-endian on every host this runs on. */
static void swap_components(u8* d, const u8* s, u32 n, u32 width)
{
    for (u32 c = 0; c < n; c++)
        for (u32 b = 0; b < width; b++)
            d[c * width + b] = s[c * width + (width - 1u - b)];
}

/* One source texel at `s` to one host texel at `d`, which receives
 * tl->dst_bytes_per_texel bytes. */
static void decode_texel(const rsx_tex_layout* tl, const u8* s, u8* d,
                         int argb_as_rgba)
{
    switch (tl->base_format) {
    case 0x85:            /* A8R8G8B8 */
    case 0x9E:            /* D8R8G8B8: the alpha byte is not a channel */
        if (argb_as_rgba) {
            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
        } else {
            /* guest A,R,G,B -> host R,G,B,A */
            d[0] = s[1]; d[1] = s[2]; d[2] = s[3]; d[3] = s[0];
        }
        if (tl->base_format == 0x9E) d[3] = 255;
        return;

    /* Packed 16-bit colour. RSX numbers these fields from the high bit of a
     * big-endian half-word, so the half-word is read that way and each field
     * widened to a byte. */
    case 0x82: case 0x9D: {   /* A1R5G5B5, and D1R5G5B5 with no alpha */
        const u32 v = be16(s);
        d[0] = expand5((v >> 10) & 0x1Fu);
        d[1] = expand5((v >>  5) & 0x1Fu);
        d[2] = expand5( v        & 0x1Fu);
        d[3] = (tl->base_format == 0x9D) ? 255u
             : (u8)((v & 0x8000u) ? 255u : 0u);
        return;
    }
    case 0x83: {              /* A4R4G4B4 */
        const u32 v = be16(s);
        d[0] = expand4((v >> 8) & 0xFu);
        d[1] = expand4((v >> 4) & 0xFu);
        d[2] = expand4( v       & 0xFu);
        d[3] = expand4((v >> 12) & 0xFu);
        return;
    }
    case 0x84: {              /* R5G6B5 */
        const u32 v = be16(s);
        d[0] = expand5((v >> 11) & 0x1Fu);
        d[1] = expand6((v >>  5) & 0x3Fu);
        d[2] = expand5( v        & 0x1Fu);
        d[3] = 255;
        return;
    }
    case 0x97: {              /* R5G5B5A1: alpha is the LOW bit, not the high */
        const u32 v = be16(s);
        d[0] = expand5((v >> 11) & 0x1Fu);
        d[1] = expand5((v >>  6) & 0x1Fu);
        d[2] = expand5((v >>  1) & 0x1Fu);
        d[3] = (u8)((v & 1u) ? 255u : 0u);
        return;
    }

    case 0x90: {              /* DEPTH24_D8: depth in the top 24 bits */
        const float f = (float)(be32(s) >> 8) / 16777215.0f;
        memcpy(d, &f, sizeof f);
        return;
    }

    default:
        break;
    }

    /* Everything left keeps its bits. One byte per component needs nothing
     * done to it; wider components are swapped out of big-endian. */
    switch (tl->fmt) {
    case RSX_TEXFMT_R16:             swap_components(d, s, 1, 2); return;
    case RSX_TEXFMT_R16G16:
    case RSX_TEXFMT_R16G16F:         swap_components(d, s, 2, 2); return;
    case RSX_TEXFMT_R16G16B16A16F:   swap_components(d, s, 4, 2); return;
    case RSX_TEXFMT_R32F:            swap_components(d, s, 1, 4); return;
    case RSX_TEXFMT_R32G32B32A32F:   swap_components(d, s, 4, 4); return;
    default:
        for (u32 b = 0; b < tl->dst_bytes_per_texel; b++) d[b] = s[b];
        return;
    }
}

static void texdec_ref(void* dst, u32 dst_pitch,
                       const u8* src, u32 w, u32 h,
                       const rsx_tex_layout* tl, int argb_as_rgba)
{
    u8* d = (u8*)dst;

    if (tl->compressed) {
        /* BC1/2/3 are bit-identical to DXT1/23/45, so the payload is copied
         * rather than converted -- only the row stride changes. */
        for (u32 y = 0; y < tl->rows; y++)
            memcpy(d + (size_t)y * dst_pitch,
                   src + (size_t)y * tl->row_bytes, tl->dst_row_bytes);
        return;
    }

    const u32 l2w = rsx_log2_ceil(w), l2h = rsx_log2_ceil(h);
    const u32 sbpp = tl->bytes_per_texel, dbpp = tl->dst_bytes_per_texel;

    /* R8 and R8G8 reach the host byte for byte. A linear one is then a
     * straight row copy, which is worth keeping as a separate path: it is the
     * common case (Bink video planes, the G8B8 font atlas) and by far the
     * faster one. G8B8's channel placement is the sampler's component remap,
     * not this. */
    if ((tl->fmt == RSX_TEXFMT_R8 || tl->fmt == RSX_TEXFMT_R8G8) && !tl->swizzled) {
        for (u32 y = 0; y < h; y++)
            memcpy(d + (size_t)y * dst_pitch,
                   src + (size_t)y * tl->row_bytes, (size_t)w * sbpp);
        return;
    }

    /* Resistance port (v127): the per-texel loop below spent most of its time
     * re-deriving the Morton offset bit by bit (rsx_swizzle_offset) and
     * switching on the format for every texel -- decode_texel and this
     * function were the render thread's top entries while York streamed in
     * new textures. The x and y bits of a Morton offset land in disjoint
     * positions, so offset(x, y) = offset(x, 0) | offset(0, y): two per-axis
     * tables make it one OR. A8R8G8B8 (most of the game's art) gets its own
     * loop, a 32-bit rotate per texel. Output is identical; RSX_TEXDEC_FAST=0
     * restores the old loop. */
    {
        static int fast = -1;
        if (fast < 0) { const char* e = getenv("RSX_TEXDEC_FAST"); fast = e ? atoi(e) : 1; }
        static const u32 one = 1u;
        const int little = *(const u8*)&one == 1u;
        static _Thread_local u32* s_tab = NULL;      /* tx[4096] then ty[4096] */
        if (fast && w <= 4096u && h <= 4096u && sbpp &&
            (s_tab || (s_tab = (u32*)malloc(2u * 4096u * sizeof(u32))) != NULL)) {
            u32* tx = s_tab; u32* ty = s_tab + 4096;
            const int swz = tl->swizzled;
            if (swz) {
                for (u32 x = 0; x < w; x++) tx[x] = rsx_swizzle_offset(x, 0, l2w, l2h);
                for (u32 y = 0; y < h; y++) ty[y] = rsx_swizzle_offset(0, y, l2w, l2h);
            }
            const u32 bf = tl->base_format;
            if ((bf == 0x85u || bf == 0x9Eu) && sbpp == 4u && dbpp == 4u && little) {
                const u32 amask = (bf == 0x9Eu) ? 0xFF000000u : 0u;
                for (u32 y = 0; y < h; y++) {
                    u8* drow = d + (size_t)y * dst_pitch;
                    const u8* srow = src + (size_t)y * tl->row_bytes;
                    const u32 yo = swz ? ty[y] : 0u;
                    for (u32 x = 0; x < w; x++) {
                        const u8* sp = swz ? src + (size_t)(tx[x] | yo) * 4u : srow + (size_t)x * 4u;
                        u32 v; memcpy(&v, sp, 4);
                        if (!argb_as_rgba) v = (v >> 8) | (v << 24);   /* A,R,G,B -> R,G,B,A */
                        v |= amask;
                        memcpy(drow + (size_t)x * 4u, &v, 4);
                    }
                }
                return;
            }
            for (u32 y = 0; y < h; y++) {
                u8* drow = d + (size_t)y * dst_pitch;
                const u8* srow = src + (size_t)y * tl->row_bytes;
                const u32 yo = swz ? ty[y] : 0u;
                for (u32 x = 0; x < w; x++) {
                    const u8* sp = swz ? src + (size_t)(tx[x] | yo) * sbpp : srow + (size_t)x * sbpp;
                    decode_texel(tl, sp, drow + (size_t)x * dbpp, argb_as_rgba);
                }
            }
            return;
        }
    }

    /* Morton order interleaves the low bits of x and y, so a swizzled image
     * has to be walked texel by texel; so does anything the host cannot take
     * as stored. */
    for (u32 y = 0; y < h; y++) {
        u8* drow = d + (size_t)y * dst_pitch;
        for (u32 x = 0; x < w; x++) {
            const u8* s = tl->swizzled
                ? src + (size_t)rsx_swizzle_offset(x, y, l2w, l2h) * sbpp
                : src + (size_t)y * tl->row_bytes + (size_t)x * sbpp;
            decode_texel(tl, s, drow + (size_t)x * dbpp, argb_as_rgba);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Resistance port (native hot path, RSX_NATIVE_TEXDEC, default 1): the decode
 * above, rewritten around 4x4 texel blocks. A swizzled image with both sides
 * >= 4 stores each 4x4 block as 16 consecutive texels (the low two bits of x
 * and y are the low four bits of the Morton offset), so a block is one
 * contiguous load, a fixed shuffle into rows and one SIMD conversion, instead
 * of 16 table lookups and 16 per-texel format switches. The packed 16-bit
 * colour formats go through a 64K-entry table built FROM decode_texel, so
 * they are exact by construction; the others are bit moves (rotate, byte
 * swaps, copies). The wrapper below checks this against texdec_ref on the
 * first 64 decodes and on 1 in 512 after, and turns itself off for the rest
 * of the run on any difference. DEPTH24_D8 (a float conversion) stays on the
 * reference path.
 * -------------------------------------------------------------------------*/
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#define TEXNH_SSE2 1
#else
#define TEXNH_SSE2 0
#endif

enum { NHC_NONE = 0, NHC_COPY, NHC_ARGB, NHC_SWAP16, NHC_SWAP32, NHC_LUT16 };

static int nh_class(const rsx_tex_layout* tl)
{
    const u32 sb = tl->bytes_per_texel, db = tl->dst_bytes_per_texel;
    if (tl->compressed || !sb || !db) return NHC_NONE;
    switch (tl->base_format) {
    case 0x85: case 0x9E:
        return (sb == 4 && db == 4) ? NHC_ARGB : NHC_NONE;
    case 0x82: case 0x83: case 0x84: case 0x97: case 0x9D:
        return (sb == 2 && db == 4) ? NHC_LUT16 : NHC_NONE;
    case 0x90:
        return NHC_NONE;
    default: break;
    }
    if (sb != db) return NHC_NONE;
    switch (tl->fmt) {
    case RSX_TEXFMT_R16: case RSX_TEXFMT_R16G16: case RSX_TEXFMT_R16G16F:
    case RSX_TEXFMT_R16G16B16A16F:
        return NHC_SWAP16;
    case RSX_TEXFMT_R32F: case RSX_TEXFMT_R32G32B32A32F:
        return NHC_SWAP32;
    default:
        return NHC_COPY;
    }
}

#if TEXNH_SSE2
/* The packed 16-bit formats: host texel for every guest half-word. */
static u32* volatile s_nh_lut[5];
static int nh_lut_index(u32 bf)
{
    switch (bf) { case 0x82: return 0; case 0x83: return 1; case 0x84: return 2;
                  case 0x97: return 3; case 0x9D: return 4; default: return -1; }
}
static const u32* nh_lut(const rsx_tex_layout* tl)
{
    const int li = nh_lut_index(tl->base_format);
    if (li < 0) return NULL;
    u32* t = s_nh_lut[li];
    if (t) return t;
    t = (u32*)malloc(65536u * sizeof(u32));
    if (!t) return NULL;
    for (u32 v = 0; v < 65536u; v++) {
        const u8 s[2] = { (u8)(v >> 8), (u8)v };
        u8 o[4];
        decode_texel(tl, s, o, 0);
        memcpy(&t[v], o, 4);
    }
#if defined(_MSC_VER) && !defined(__clang__)
    {   void* prev = _InterlockedCompareExchangePointer((void* volatile*)&s_nh_lut[li], t, NULL);
        if (prev) { free(t); t = (u32*)prev; } }
#else
    {   u32* expect = NULL;
        if (!__atomic_compare_exchange_n(&s_nh_lut[li], &expect, t, 0,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { free(t); t = expect; } }
#endif
    return t;
}

/* n contiguous source texels -> n contiguous host texels. */
static void nh_conv(int cls, const u8* s, u8* d, u32 n, u32 sbpp,
                    __m128i amask, int rot, const u32* lut)
{
    u32 i = 0;
    switch (cls) {
    case NHC_COPY:
        memcpy(d, s, (size_t)n * sbpp);
        return;
    case NHC_ARGB:
        for (; i + 4 <= n; i += 4) {
            __m128i v = _mm_loadu_si128((const __m128i*)(s + (size_t)i * 4u));
            if (rot) v = _mm_or_si128(_mm_srli_epi32(v, 8), _mm_slli_epi32(v, 24));
            _mm_storeu_si128((__m128i*)(d + (size_t)i * 4u), _mm_or_si128(v, amask));
        }
        for (; i < n; i++) {
            u32 v; memcpy(&v, s + (size_t)i * 4u, 4);
            if (rot) v = (v >> 8) | (v << 24);
            v |= (u32)_mm_cvtsi128_si32(amask);
            memcpy(d + (size_t)i * 4u, &v, 4);
        }
        return;
    case NHC_SWAP16:
    case NHC_SWAP32: {
        const size_t nb = (size_t)n * sbpp;
        size_t b = 0;
        for (; b + 16 <= nb; b += 16) {
            __m128i v = _mm_loadu_si128((const __m128i*)(s + b));
            v = _mm_or_si128(_mm_slli_epi16(v, 8), _mm_srli_epi16(v, 8));
            if (cls == NHC_SWAP32)
                v = _mm_shufflehi_epi16(_mm_shufflelo_epi16(v, 0xB1), 0xB1);
            _mm_storeu_si128((__m128i*)(d + b), v);
        }
        if (cls == NHC_SWAP16)
            for (; b + 2 <= nb; b += 2) { d[b] = s[b + 1]; d[b + 1] = s[b]; }
        else
            for (; b + 4 <= nb; b += 4) { d[b] = s[b + 3]; d[b + 1] = s[b + 2];
                                          d[b + 2] = s[b + 1]; d[b + 3] = s[b]; }
        return;
    }
    case NHC_LUT16:
        for (; i < n; i++) {
            const u32 v = lut[((u32)s[(size_t)i * 2u] << 8) | s[(size_t)i * 2u + 1u]];
            memcpy(d + (size_t)i * 4u, &v, 4);
        }
        return;
    default:
        return;
    }
}
#endif /* TEXNH_SSE2 */

/* Returns 1 when it decoded the image, 0 (nothing written) when the layout is
 * one it leaves to the reference path. */
int rsx_texture_decode_native(void* dst, u32 dst_pitch,
                              const u8* src, u32 w, u32 h,
                              const rsx_tex_layout* tl, int argb_as_rgba)
{
#if TEXNH_SSE2
    if (!dst || !src || !tl || !w || !h) return 0;
    const int cls = nh_class(tl);
    if (cls == NHC_NONE) return 0;
    const u32 sbpp = tl->bytes_per_texel, dbpp = tl->dst_bytes_per_texel;
    const int swz = tl->swizzled;
    if (!swz && (cls == NHC_COPY || cls == NHC_ARGB))
        return 0;   /* the reference's row memcpy / vectorised rotate already */
    if (swz && (w < 4u || h < 4u || w > 4096u || h > 4096u)) return 0;
    const u32* lut = NULL;
    if (cls == NHC_LUT16 && !(lut = nh_lut(tl))) return 0;
    const __m128i amask = _mm_set1_epi32(tl->base_format == 0x9Eu ? (int)0xFF000000u : 0);
    const int rot = !argb_as_rgba;
    u8* d = (u8*)dst;

    if (!swz) {
        for (u32 y = 0; y < h; y++)
            nh_conv(cls, src + (size_t)y * tl->row_bytes, d + (size_t)y * dst_pitch,
                    w, sbpp, amask, rot, lut);
        return 1;
    }

    const u32 l2w = rsx_log2_ceil(w), l2h = rsx_log2_ceil(h);
    static _Thread_local u32 txb[1024], tyb[1024];
    const u32 bw = w >> 2, bh = h >> 2;
    for (u32 bx = 0; bx < bw; bx++) txb[bx] = rsx_swizzle_offset(bx << 2, 0, l2w, l2h);
    for (u32 by = 0; by < bh; by++) tyb[by] = rsx_swizzle_offset(0, by << 2, l2w, l2h);

    /* Each 4-row strip is built in a cached scratch buffer and then copied out
     * row by row. The destination is the D3D12 upload ring, which is
     * write-combined: storing 16 bytes into four rows at a time there split
     * every combining buffer and ran slower than the per-texel loop it
     * replaces (scratch\nativehot bench), while sequential rows run at full
     * write-combining speed. */
    const size_t srow = (size_t)w * (sbpp > dbpp ? sbpp : dbpp);
    static _Thread_local u8* s_strip = NULL;
    static _Thread_local size_t s_strip_cap = 0;
    if (s_strip_cap < 4u * srow) {
        u8* nb = (u8*)realloc(s_strip, 4u * srow);
        if (!nb) return 0;
        s_strip = nb; s_strip_cap = 4u * srow;
    }
    u8* const st = s_strip;

    if (cls == NHC_ARGB) {
        for (u32 by = 0; by < bh; by++) {
            u8* r0 = st;
            u8* r1 = r0 + srow; u8* r2 = r1 + srow; u8* r3 = r2 + srow;
            const u32 yo = tyb[by];
            for (u32 bx = 0; bx < bw; bx++) {
                const u8* b = src + (size_t)(txb[bx] | yo) * 4u;
                const __m128i q0 = _mm_loadu_si128((const __m128i*)(b));
                const __m128i q1 = _mm_loadu_si128((const __m128i*)(b + 16));
                const __m128i q2 = _mm_loadu_si128((const __m128i*)(b + 32));
                const __m128i q3 = _mm_loadu_si128((const __m128i*)(b + 48));
                __m128i v0 = _mm_unpacklo_epi64(q0, q1), v1 = _mm_unpackhi_epi64(q0, q1);
                __m128i v2 = _mm_unpacklo_epi64(q2, q3), v3 = _mm_unpackhi_epi64(q2, q3);
                if (rot) {
                    v0 = _mm_or_si128(_mm_srli_epi32(v0, 8), _mm_slli_epi32(v0, 24));
                    v1 = _mm_or_si128(_mm_srli_epi32(v1, 8), _mm_slli_epi32(v1, 24));
                    v2 = _mm_or_si128(_mm_srli_epi32(v2, 8), _mm_slli_epi32(v2, 24));
                    v3 = _mm_or_si128(_mm_srli_epi32(v3, 8), _mm_slli_epi32(v3, 24));
                }
                const size_t xo = (size_t)bx * 16u;
                _mm_storeu_si128((__m128i*)(r0 + xo), _mm_or_si128(v0, amask));
                _mm_storeu_si128((__m128i*)(r1 + xo), _mm_or_si128(v1, amask));
                _mm_storeu_si128((__m128i*)(r2 + xo), _mm_or_si128(v2, amask));
                _mm_storeu_si128((__m128i*)(r3 + xo), _mm_or_si128(v3, amask));
            }
            u8* out = d + (size_t)(by << 2) * dst_pitch;
            for (u32 r = 0; r < 4; r++)
                memcpy(out + (size_t)r * dst_pitch, st + (size_t)r * srow, (size_t)w * 4u);
        }
        return 1;
    }

    /* Any other format: gather each block's 16 texels into row-major order in
     * the strip (Morton pair k holds texels 2k, 2k+1; row r is pairs P and P+2
     * with P = (r & 1) + 4 * (r >> 1)), then convert the strip's four rows
     * straight into the destination. */
    const u32 pair = 2u * sbpp;
    for (u32 by = 0; by < bh; by++) {
        const u32 yo = tyb[by];
        for (u32 bx = 0; bx < bw; bx++) {
            const u8* b = src + (size_t)(txb[bx] | yo) * sbpp;
            u8* o = st + (size_t)bx * 2u * pair;
            for (u32 r = 0; r < 4; r++) {
                const u32 p = (r & 1u) + 4u * (r >> 1);
                memcpy(o + (size_t)r * srow,        b + (size_t)p * pair,        pair);
                memcpy(o + (size_t)r * srow + pair, b + (size_t)(p + 2u) * pair, pair);
            }
        }
        u8* out = d + (size_t)(by << 2) * dst_pitch;
        for (u32 r = 0; r < 4; r++)
            nh_conv(cls, st + (size_t)r * srow, out + (size_t)r * dst_pitch, w, sbpp, amask, rot, lut);
    }
    return 1;
#else
    (void)dst; (void)dst_pitch; (void)src; (void)w; (void)h; (void)tl; (void)argb_as_rgba;
    return 0;
#endif
}

void rsx_texture_decode_reference(void* dst, u32 dst_pitch,
                                  const u8* src, u32 w, u32 h,
                                  const rsx_tex_layout* tl, int argb_as_rgba)
{
    if (!dst || !src || !tl || !w || !h) return;
    texdec_ref(dst, dst_pitch, src, w, h, tl, argb_as_rgba);
}

static volatile int s_nh_texdec = -1;   /* RSX_NATIVE_TEXDEC; 0 after a mismatch */
static volatile unsigned s_nh_texdec_n;

void rsx_texture_decode(void* dst, u32 dst_pitch,
                        const u8* src, u32 w, u32 h,
                        const rsx_tex_layout* tl, int argb_as_rgba)
{
    if (!dst || !src || !tl || !w || !h) return;

    if (s_nh_texdec < 0) { const char* e = getenv("RSX_NATIVE_TEXDEC"); s_nh_texdec = port_native_parse_bool(e, 1); }
    if (s_nh_texdec > 0 && TEXNH_SSE2 && !tl->compressed && nh_class(tl) != NHC_NONE) {
        const unsigned n = s_nh_texdec_n++;
        const int verify = n < 64u || (n & 511u) == 0u;
        const size_t vsz = (size_t)w * h * tl->dst_bytes_per_texel;
        static volatile int logged = 0;
        if (verify && vsz > (16u << 20)) {
            /* too big to check by copy: this one goes the reference way */
        } else if (!verify) {
            if (rsx_texture_decode_native(dst, dst_pitch, src, w, h, tl, argb_as_rgba)) return;
        } else {
            const size_t row = (size_t)w * tl->dst_bytes_per_texel, sz = row * h;
            u8* a = (u8*)malloc(sz);
            u8* b = (u8*)malloc(sz);
            int done = 0;
            if (a && b && rsx_texture_decode_native(a, (u32)row, src, w, h, tl, argb_as_rgba)) {
                texdec_ref(b, (u32)row, src, w, h, tl, argb_as_rgba);
                const int same = memcmp(a, b, sz) == 0;
                if (!same) {
                    s_nh_texdec = 0;
                    fprintf(stderr, "[native-hot] texdec: MISMATCH against the reference decode"
                                    " (fmt 0x%02X %ux%u swz=%d) -- native decode off for this run%c",
                            tl->base_format, w, h, tl->swizzled, 10);
                } else if (!logged) {
                    logged = 1;
                    fprintf(stderr, "[native-hot] texdec: SSE2 4x4-block untile/convert engaged"
                                    " (verified on the first 64 decodes, 1 in 512 after;"
                                    " RSX_NATIVE_TEXDEC=0 = reference)%c", 10);
                }
                const u8* res = same ? a : b;
                for (u32 y = 0; y < h; y++)
                    memcpy((u8*)dst + (size_t)y * dst_pitch, res + (size_t)y * row, row);
                done = 1;
            }
            free(a); free(b);
            if (done) return;
        }
    }
    texdec_ref(dst, dst_pitch, src, w, h, tl, argb_as_rgba);
}

void rsx_texture_component_remap(u32 control1, u32 rsx_fmt, u8 out[4])
{
    if (!out) return;

    /* Source codes index the presented vector {A,R,G,B}. The uploaded resource
     * holds R,G,B,A at components 0..3, so A is component 3 and R,G,B are
     * 0,1,2 -- that is what lanes_argb says. G8B8 only has two real channels,
     * and the sampler presents them as {G,R,G,R}. B8 decodes to a one-channel
     * resource whose byte is presented as R, G and B with an opaque A, which
     * is what the D3D12 live engine's CPU decode builds (decode_texel); read
     * through lanes_argb its B source would be component 2, always zero.
     * X16 is one 16-bit channel decoded to R16 and is presented the same way:
     * God of War: Ascension samples its X16 font atlas through crossbar 0xAAEE
     * (every output from G or B), which read zeros -- all menu text invisible. */
    static const u8 lanes_argb[4] = {3, 0, 1, 2};
    static const u8 lanes_g8b8[4] = {1, 0, 1, 0};
    static const u8 lanes_b8[4]   = {RSX_REMAP_ONE, 0, 0, 0};
    /* The depth formats read as colour (DEPTH24_D8, DEPTH16 and their float
     * forms) and X32_FLOAT are one-channel resources whose value the sampler
     * presents in all four lanes (RPCS3 get_component_mapping: R,R,R,R).
     * Through lanes_argb an identity read came back (v, 0, 0, 1): a red tint
     * wherever a pass reads one of them through .rgb. RFOM_DEPTH_RRRR=0
     * restores the old mapping. */
    static const u8 lanes_rrrr[4] = {0, 0, 0, 0};
    static int rrrr = -1;
    if (rrrr < 0) { const char* e = getenv("RFOM_DEPTH_RRRR"); rrrr = e ? atoi(e) : 1; }
    const u32 base = rsx_fmt & 0x9Fu;
    const u8* src2res = (base == 0x8Bu) ? lanes_g8b8
                      : (base == 0x81u || base == 0x94u) ? lanes_b8
                      : (rrrr && ((base >= 0x90u && base <= 0x93u) || base == 0x9Cu)) ? lanes_rrrr : lanes_argb;

    if (!(control1 & 0xFFFFu)) control1 = 0xAAE4u;   /* unset -> identity */

    /* Y16_X16 (and HILO8): two 16-bit channels whose crossbar is encoded in
     * 16-bit blocks -- 0xE4 or 0x4E only -- and presented interleaved
     * R-G-R-G, or R-R-R-G when CONTROL1's high word (the remap override,
     * bit 16) is set (RPCS3 fragment_texture::decoded_remap, verified on
     * hardware with Virtua Fighter 5 / Mass Effect 3). Through the plain
     * ARGB table a .w read got the resource's empty alpha, 1.0, instead of
     * Y: inFAMOUS 2's water reads its 64x64 wave map through .yw that way. */
    if (base == 0x95u || base == 0x98u || base == 0x99u) {
        const int ovr = (control1 >> 16) & 0xFFFFu;
        u32 lo = control1 & 0xFFu;
        switch (lo) {
        case 0xE4: lo = ovr ? 0x56u : 0x66u; break;
        case 0x4E: lo = ovr ? 0xA9u : 0x99u; break;
        case 0xEE: lo = 0xAAu; break;
        case 0x44: lo = 0x55u; break;
        default: break;
        }
        control1 = (control1 & ~0xFFu) | lo;
    }

    /* i runs A, R, G, B -- the crossbar's field order, LSB first. */
    for (int i = 0; i < 4; i++) {
        u32 s  = (control1 >> (i * 2)) & 3u;
        u32 op = (control1 >> (8 + i * 2)) & 3u;
        out[i] = (op == 0) ? (u8)RSX_REMAP_ZERO
               : (op == 1) ? (u8)RSX_REMAP_ONE
                           : src2res[s];
    }
}

/* ADDRESS gamma flags name final RGBA lanes, after remapping. Signed input
 * interpretation wins; its FILTER ARGB flags follow the source crossbar. */
u8 rsx_texture_gamma_mask(u32 format, u32 control1, u32 filter, u32 address)
{
    switch (format & 0x9Fu) {
    case 0x81: case 0x82: case 0x83: case 0x84: case 0x85:
    case 0x86: case 0x87: case 0x88: case 0x8B: case 0x8D: case 0x8E: case 0x8F:
    case 0x97: case 0x9D: case 0x9E: break;
    default: return 0;
    }
    u8 gamma = (u8)((address >> 20) & 15u);
    if (!gamma) return 0;
    if (!(control1 & 0xFFFFu)) control1 = 0xAAE4u;
    u8 remap[4]; rsx_texture_component_remap(control1, format, remap);
    static const u8 rgba_lane[4] = {3, 0, 1, 2};
    for (u32 i = 0; i < 4; ++i) {
        const u32 source = (control1 >> (2u * i)) & 3u;
        if (remap[i] < RSX_REMAP_ZERO && ((filter >> (28u + source)) & 1u))
            gamma &= (u8)~(1u << rgba_lane[i]);
    }
    return gamma;
}

u8 rsx_texture_expand_mask(u32 format, u32 control1, u32 filter, u32 address)
{
    /* RPCS3 rsx::get_format_features, RSX_FORMAT_FEATURE_BIASED_NORMALIZATION:
     * the 8-bit colour formats, the depth formats, X16, Y16_X16 and HILO8.
     * Floats and the 32-bit depth-as-colour reads have nothing to expand. */
    switch (format & 0x9Fu) {
    case 0x81: case 0x82: case 0x83: case 0x84: case 0x85:
    case 0x86: case 0x87: case 0x88: case 0x8B: case 0x8D: case 0x8E: case 0x8F:
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x98:
    case 0x97: case 0x9D: case 0x9E: break;
    default: return 0;
    }
    if (!((address >> 12) & 1u)) return 0;          /* UNSIGNED_REMAP_NORMAL */
    if (!(control1 & 0xFFFFu)) control1 = 0xAAE4u;
    u8 remap[4]; rsx_texture_component_remap(control1, format, remap);
    static const u8 rgba_lane[4] = {3, 0, 1, 2};
    u8 mask = 0;
    for (u32 i = 0; i < 4; ++i) {
        const u32 source = (control1 >> (2u * i)) & 3u;
        if (remap[i] < RSX_REMAP_ZERO && !((filter >> (28u + source)) & 1u))
            mask |= (u8)(1u << rgba_lane[i]);
    }
    /* Bit 4: 16-bit channels (X16, Y16_X16) re-centre on 32768/32767. */
    if (mask && ((format & 0x9Fu) == 0x94u || (format & 0x9Fu) == 0x95u)) mask |= 0x10u;
    return mask;
}
