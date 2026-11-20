/* r1_hd_ui.h -- HD UI pack core for the Resistance: Fall of Man port, shared by the D3D12 backend's texture upload
 * (rsx_texpack.inc, RSX_HD_UI), the offline pack builder (project\hd_ui_pack.cpp + project\hd_ui_game_r1.h, run by the
 * launcher's "Create HD UI pack (GPU)" button) and their tests. Plain C (also compiles as C++), no backend types, so
 * the offline tool computes the exact key the game's upload computes. The R2/R3 ports have the same core in
 * rpcs3/Emu/RSX/Common/r2_hd_ui.h; the formats, scale rules, DDS layout and error texts here are the same.
 *
 * Key: rsx_texpack.inc's texture-pack key (RSX_TEXDUMP / RSX_TEXPACK use it too): a 64-bit hash of level 0 as the
 * upload reads it (row_bytes * rows, so a linear texture's pitch padding is included) and of every lower level the
 * upload sends, tightly packed after level 0, seeded with width, height, the format byte (with LN/UN) and the number
 * of levels uploaded (r1hd_upload_mips). */
#ifndef R1_HD_UI_H
#define R1_HD_UI_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rsx_texture_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    R1HD_A8R8G8B8 = 0x85,
    R1HD_DXT1 = 0x86,
    R1HD_DXT23 = 0x87,
    R1HD_DXT45 = 0x88,
    R1HD_LN = 0x20,
    R1HD_UN = 0x40
};

#define R1HD_MAX_SCALE 8u
#define R1HD_MAX_SIDE 8192u
#define R1HD_MAX_LEVEL0 ((size_t)64 << 20)
#define R1HD_MIN_SIDE 8u          /* the runtime only keys textures at least 8x8 */
#define R1HD_MAX_LEVELS 16u
#define R1HD_FE_ATLAS_HASH 0xE82A7DC8u   /* the menu button atlas the port redraws (rsx_d3d12_backend.c keycaps) */

static inline uint32_t r1hd_base(uint32_t gcm) { return gcm & 0x9Fu; }
static inline int r1hd_is_dxt(uint32_t fmt) { fmt = r1hd_base(fmt); return fmt == R1HD_DXT1 || fmt == R1HD_DXT23 || fmt == R1HD_DXT45; }
static inline uint32_t r1hd_block_bytes(uint32_t fmt)
{
    fmt = r1hd_base(fmt);
    return fmt == R1HD_DXT1 ? 8u : (fmt == R1HD_DXT23 || fmt == R1HD_DXT45) ? 16u : 4u;
}
static inline const char* r1hd_fmt_name(uint32_t fmt)
{
    fmt = r1hd_base(fmt);
    return fmt == R1HD_DXT1 ? "DXT1" : fmt == R1HD_DXT23 ? "DXT3" : fmt == R1HD_DXT45 ? "DXT5" : "A8R8G8B8";
}
static inline int r1hd_handled_format(uint32_t fmt) { fmt = r1hd_base(fmt); return fmt == R1HD_A8R8G8B8 || r1hd_is_dxt(fmt); }

/* Why a texture can never get an HD replacement (NULL = it can). gcm = the SET_TEXTURE_FORMAT byte (LN/UN kept).
 * argb_is_rgba: the port's TEX_RGBA switch (rsx_texture_argb_is_rgba), which changes how A8R8G8B8 is decoded. */
static inline const char* r1hd_ineligible(uint32_t gcm, uint32_t w, uint32_t h, int cube, int argb_is_rgba)
{
    const uint32_t fmt = r1hd_base(gcm);
    if (!r1hd_handled_format(fmt)) return "format";
    if (cube) return "not a 2D texture";
    if (gcm & R1HD_UN) return "unnormalized coordinates";
    if (w < R1HD_MIN_SIDE || h < R1HD_MIN_SIDE) return "smaller than 8x8";
    if (w > 4096u || h > 4096u) return "larger than 4096";
    if (fmt == R1HD_A8R8G8B8 && argb_is_rgba) return "TEX_RGBA";
    return NULL;
}

/* The number of levels the upload sends (rsx_d3d12_backend.c vp_upload_tex_slot_impl): every level of a swizzled or
 * DXT 2D texture, DXT down to the last one that is still whole 4x4 blocks; RSX_TEX_MIPS=0 (mips_env 0) only level 0. */
static inline uint32_t r1hd_upload_mips(uint32_t gcm, uint32_t w, uint32_t h, uint32_t mips_reg, int cube, int mips_env)
{
    rsx_tex_layout tl;
    uint32_t nm = 1, lw = w, lh = h, L;
    rsx_texture_layout_pitched(gcm, w, h, 0, &tl);
    if (!mips_env || cube || mips_reg <= 1 || !(tl.compressed || tl.swizzled)) return 1;
    for (L = 1; L < mips_reg && L < 16; L++) {
        lw = lw > 1 ? lw >> 1 : 1; lh = lh > 1 ? lh >> 1 : 1;
        if (tl.compressed && (lw < 4 || lh < 4 || (lw & 3) || (lh & 3))) break;
        nm = L + 1;
    }
    return nm;
}

static inline void r1hd_mix(uint64_t* hsh, const uint8_t* p, uint32_t n)
{
    uint64_t x = *hsh, tail = 0;
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v; memcpy(&v, p + i, 8);
        x ^= v; x *= 0xFF51AFD7ED558CCDull; x ^= x >> 32;
    }
    if (n > i) memcpy(&tail, p + i, n - i);
    x ^= tail ^ ((uint64_t)n << 56); x *= 0xFF51AFD7ED558CCDull; x ^= x >> 32;
    *hsh = x;
}

/* rsx_texpack.inc's tp_hash: n0 = level 0's bytes as the upload reads them, fmt = the format byte, nm = levels
 * uploaded, bc = block compressed, bb = bytes per block (bc) or per texel. Lower levels follow level 0 tightly packed. */
static inline uint64_t r1hd_hash(const uint8_t* p, uint32_t n0, uint32_t w, uint32_t h, uint32_t fmt, uint32_t nm, int bc, uint32_t bb)
{
    uint64_t hsh = 0x9E3779B97F4A7C15ull ^ ((uint64_t)w << 48) ^ ((uint64_t)h << 32) ^ ((uint64_t)(nm & 0xFFu) << 8) ^ fmt;
    uint32_t o = bc ? ((w + 3) / 4) * ((h + 3) / 4) * bb : w * h * bb, lw = w, lh = h, l;
    r1hd_mix(&hsh, p, n0);
    for (l = 1; l < nm; l++) {
        uint32_t sz;
        lw = lw > 1 ? lw >> 1 : 1; lh = lh > 1 ? lh >> 1 : 1;
        sz = bc ? ((lw + 3) / 4) * ((lh + 3) / 4) * bb : lw * lh * bb;
        r1hd_mix(&hsh, p + o, sz);
        o += sz;
    }
    return hsh;
}

/* Bytes the key reads from the texture's start (level 0 as read, plus the packed lower levels) */
static inline uint64_t r1hd_key_bytes(uint32_t n0, uint32_t w, uint32_t h, uint32_t nm, int bc, uint32_t bb)
{
    uint64_t o = bc ? (uint64_t)((w + 3) / 4) * ((h + 3) / 4) * bb : (uint64_t)w * h * bb, end = n0;
    uint32_t lw = w, lh = h, l;
    for (l = 1; l < nm; l++) {
        lw = lw > 1 ? lw >> 1 : 1; lh = lh > 1 ? lh >> 1 : 1;
        o += bc ? (uint64_t)((lw + 3) / 4) * ((lh + 3) / 4) * bb : (uint64_t)lw * lh * bb;
        if (o > end) end = o;
    }
    return end;
}

/* What the upload keys a texture by: gcm = format byte, mips_reg = SET_TEXTURE_FORMAT's level count, tpitch =
 * SET_TEXTURE_CONTROL3's pitch. Returns 0 when it is not an A8R8G8B8 / DXT texture the key covers, or when its bytes
 * would run past `avail`. *nm_out = levels uploaded, *need_out = bytes read. */
static inline int r1hd_texture_key(const uint8_t* p, uint64_t avail, uint32_t gcm, uint32_t w, uint32_t h, uint32_t mips_reg,
                                   uint32_t tpitch, int cube, int mips_env, uint64_t* key_out, uint32_t* nm_out, uint64_t* need_out)
{
    rsx_tex_layout tl;
    uint32_t n0, bb, nm;
    int bc;
    uint64_t need;
    if (!w || !h || cube || !r1hd_handled_format(gcm)) return 0;
    rsx_texture_layout_pitched(gcm, w, h, (cube || mips_reg > 1) ? 0 : tpitch, &tl);
    if (tl.fmt != RSX_TEXFMT_R8G8B8A8 && tl.fmt != RSX_TEXFMT_BC1 && tl.fmt != RSX_TEXFMT_BC2 && tl.fmt != RSX_TEXFMT_BC3) return 0;
    n0 = tl.row_bytes * tl.rows;
    bc = tl.compressed;
    bb = bc ? (tl.fmt == RSX_TEXFMT_BC1 ? 8u : 16u) : (n0 / (w * h) ? n0 / (w * h) : 1u);
    nm = r1hd_upload_mips(gcm, w, h, mips_reg, cube, mips_env);
    need = r1hd_key_bytes(n0, w, h, nm, bc, bb);
    if (need_out) *need_out = need;
    if (nm_out) *nm_out = nm;
    if (need > avail || !n0) return 0;
    if (key_out) *key_out = r1hd_hash(p, n0, w, h, gcm & 0xFFu, nm, bc, bb);
    return 1;
}

/* The keycap atlas check of the upload (rsx_d3d12_backend.c, "[keycaps]"): FNV-1a of the decoded RGBA of a 512x256
 * DXT5 texture whose block rows are row_bytes apart. The port redraws that atlas's button pictures, so it is never
 * replaced. */
static inline uint32_t r1hd_fe_atlas_hash(const uint8_t* src, uint32_t row_bytes, uint32_t w, uint32_t h)
{
    uint32_t hsh = 2166136261u, by, bx, y, x;
    uint8_t* rgba = (uint8_t*)malloc((size_t)w * h * 4);
    if (!rgba) return 0;
    for (by = 0; by < h / 4; by++) for (bx = 0; bx < w / 4; bx++) {
        const uint8_t* b = src + (size_t)by * row_bytes + bx * 16u;
        const uint8_t* cb = b + 8;
        const uint32_t c0 = cb[0] | (cb[1] << 8), c1 = cb[2] | (cb[3] << 8);
        const uint32_t idx = cb[4] | (cb[5] << 8) | (cb[6] << 16) | ((uint32_t)cb[7] << 24);
        uint8_t pal[4][4], a[8], al[16];
        uint64_t ab = 0;
        int c, i;
        pal[0][0] = (uint8_t)(((c0 >> 11) & 31) * 255 / 31); pal[0][1] = (uint8_t)(((c0 >> 5) & 63) * 255 / 63); pal[0][2] = (uint8_t)((c0 & 31) * 255 / 31); pal[0][3] = 255;
        pal[1][0] = (uint8_t)(((c1 >> 11) & 31) * 255 / 31); pal[1][1] = (uint8_t)(((c1 >> 5) & 63) * 255 / 63); pal[1][2] = (uint8_t)((c1 & 31) * 255 / 31); pal[1][3] = 255;
        for (c = 0; c < 3; c++) { pal[2][c] = (uint8_t)((2 * pal[0][c] + pal[1][c]) / 3); pal[3][c] = (uint8_t)((pal[0][c] + 2 * pal[1][c]) / 3); }
        pal[2][3] = 255; pal[3][3] = 255;
        a[0] = b[0]; a[1] = b[1];
        if (a[0] > a[1]) for (i = 1; i < 7; i++) a[i + 1] = (uint8_t)(((7 - i) * a[0] + i * a[1]) / 7);
        else { for (i = 1; i < 5; i++) a[i + 1] = (uint8_t)(((5 - i) * a[0] + i * a[1]) / 5); a[6] = 0; a[7] = 255; }
        for (i = 0; i < 6; i++) ab |= (uint64_t)b[2 + i] << (8 * i);
        for (i = 0; i < 16; i++) al[i] = a[(ab >> (3 * i)) & 7];
        for (i = 0; i < 16; i++) {
            const uint8_t* q = pal[(idx >> (2 * i)) & 3];
            uint8_t* d = rgba + ((by * 4 + i / 4) * w + bx * 4 + (i & 3)) * 4;
            d[0] = q[0]; d[1] = q[1]; d[2] = q[2]; d[3] = al[i];
        }
    }
    for (y = 0; y < h; y++) for (x = 0; x < w * 4; x++) { hsh ^= rgba[(size_t)y * w * 4 + x]; hsh *= 16777619u; }
    free(rgba);
    return hsh;
}
static inline int r1hd_is_fe_atlas(const uint8_t* src, uint32_t row_bytes, uint32_t gcm, uint32_t w, uint32_t h)
{
    return w == 512 && h == 256 && r1hd_base(gcm) == R1HD_DXT45 && r1hd_fe_atlas_hash(src, row_bytes, w, h) == R1HD_FE_ATLAS_HASH;
}

/* Scale of a w2 x h2 replacement for a w x h texture: 1..8, the same on both axes; 0 = no match */
static inline uint32_t r1hd_scale_of(uint32_t w2, uint32_t h2, uint32_t w, uint32_t h)
{
    uint32_t s;
    if (!w || !h || !w2 || !h2 || (w2 % w) || (h2 % h)) return 0;
    s = w2 / w;
    if (s > R1HD_MAX_SCALE || h2 / h != s) return 0;
    if (s > 1 && (w2 > R1HD_MAX_SIDE || h2 > R1HD_MAX_SIDE)) return 0;
    return s;
}

/* Level l of an S x replacement: what the upload sends in place of the game's level l */
static inline uint32_t r1hd_level_dim(uint32_t base, uint32_t scale, uint32_t l) { const uint32_t v = (base * scale) >> l; return v ? v : 1; }
static inline size_t r1hd_level_size(uint32_t fmt, uint32_t lw, uint32_t lh)
{
    return r1hd_is_dxt(fmt) ? (size_t)((lw + 3) / 4) * ((lh + 3) / 4) * r1hd_block_bytes(fmt) : (size_t)lw * lh * 4;
}
static inline uint32_t r1hd_level_row(uint32_t fmt, uint32_t lw) { return r1hd_is_dxt(fmt) ? ((lw + 3) / 4) * r1hd_block_bytes(fmt) : lw * 4; }
static inline uint32_t r1hd_level_rows(uint32_t fmt, uint32_t lh) { return r1hd_is_dxt(fmt) ? (lh + 3) / 4 : lh; }

/* Upload-buffer layout of an S x replacement with nm levels, as the D3D12 backend stages it: rows 256-byte aligned
 * (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT), levels 512-byte aligned (placement alignment), level 0 at 0. Fills
 * lvl_off / lvl_pitch / lvl_rows[0..nm) and returns the bytes needed. */
static inline uint64_t r1hd_upload_layout(uint32_t fmt, uint32_t w, uint32_t h, uint32_t scale, uint32_t nm,
                                          uint32_t* lvl_off, uint32_t* lvl_pitch, uint32_t* lvl_rows)
{
    uint64_t o = 0;
    uint32_t L;
    for (L = 0; L < nm && L < R1HD_MAX_LEVELS; L++) {
        const uint32_t lw = r1hd_level_dim(w, scale, L), lh = r1hd_level_dim(h, scale, L);
        o = (o + 511u) & ~(uint64_t)511u;
        lvl_off[L] = (uint32_t)o;
        lvl_pitch[L] = (r1hd_level_row(fmt, lw) + 255u) & ~255u;
        lvl_rows[L] = r1hd_level_rows(fmt, lh);
        o += (uint64_t)lvl_pitch[L] * lvl_rows[L];
    }
    return o;
}

/* DDS header (128 bytes) for a DXT texture, or a 32-bit B8G8R8A8 one for A8R8G8B8 */
static inline void r1hd_dds_header(uint32_t hdr[32], uint32_t w, uint32_t h, uint32_t fmt, uint32_t levels)
{
    const int dxt = r1hd_is_dxt(fmt);
    memset(hdr, 0, 128);
    hdr[0] = 0x20534444u;
    hdr[1] = 124;
    hdr[2] = 0x1 | 0x2 | 0x4 | 0x1000 | (dxt ? 0x80000 : 0x8) | (levels > 1 ? 0x20000 : 0);
    hdr[3] = h;
    hdr[4] = w;
    hdr[5] = dxt ? (uint32_t)r1hd_level_size(fmt, w, h) : w * 4;
    hdr[7] = levels;
    hdr[19] = 32;
    if (dxt) {
        const uint32_t f = r1hd_base(fmt);
        hdr[20] = 0x4;
        hdr[21] = f == R1HD_DXT1 ? 0x31545844u : f == R1HD_DXT23 ? 0x33545844u : 0x35545844u;
    } else {
        hdr[20] = 0x40 | 0x1;
        hdr[22] = 32;
        hdr[23] = 0x00FF0000u;
        hdr[24] = 0x0000FF00u;
        hdr[25] = 0x000000FFu;
        hdr[26] = 0xFF000000u;
    }
    hdr[27] = 0x1000 | (levels > 1 ? 0x400008 : 0);
}

/* A checked DDS replacement: where each level the upload needs lies in the file */
typedef struct {
    uint32_t scale, levels, bgra;      /* bgra: A8R8G8B8 stored B,G,R,A (else R,G,B,A) */
    size_t off[R1HD_MAX_LEVELS], size[R1HD_MAX_LEVELS];
} r1hd_dds;

/* Validates a DDS replacement of `n` bytes for a w x h `fmt` texture whose upload sends `nm` levels. The file must be
 * 1x..8x the game texture's size on both axes (8192 at most) in its format (DXT1/3/5, or a 32-bit DDS with alpha for
 * A8R8G8B8) and hold all nm levels. why: the reason it is not used (the R2/R3 ports' wording). */
static inline int r1hd_dds_check(const uint8_t* file, size_t n, uint32_t w, uint32_t h, uint32_t fmt, uint32_t nm,
                                 r1hd_dds* out, char* why, size_t whylen)
{
    uint32_t hdr[32], fw, fh, s, have, l;
    size_t off = 128;
    int fourcc, rgb32;
    memset(out, 0, sizeof *out);
    if (!file || n < 128 || (memcpy(hdr, file, 128), hdr[0] != 0x20534444u)) { snprintf(why, whylen, "not a DDS file"); return 0; }
    fw = hdr[4]; fh = hdr[3];
    s = r1hd_scale_of(fw, fh, w, h);
    if (!s) { snprintf(why, whylen, "its size %ux%u is not 1x..8x the game texture's (8192 at most)", fw, fh); return 0; }
    fourcc = (hdr[20] & 0x4) != 0;
    rgb32 = (hdr[20] & 0x40) && hdr[22] == 32;
    if (r1hd_is_dxt(fmt)) {
        const uint32_t f = r1hd_base(fmt);
        const uint32_t want = f == R1HD_DXT1 ? 0x31545844u : f == R1HD_DXT23 ? 0x33545844u : 0x35545844u;
        if (!(fourcc && (hdr[21] == want || (f == R1HD_DXT23 && hdr[21] == 0x32545844u) || (f == R1HD_DXT45 && hdr[21] == 0x34545844u)))) {
            snprintf(why, whylen, "the game texture is %s and the DDS is not", r1hd_fmt_name(fmt));
            return 0;
        }
    } else {
        if (!rgb32 || hdr[26] != 0xFF000000u || hdr[24] != 0x0000FF00u ||
            !((hdr[23] == 0x00FF0000u && hdr[25] == 0x000000FFu) || (hdr[23] == 0x000000FFu && hdr[25] == 0x00FF0000u))) {
            snprintf(why, whylen, "the game texture is A8R8G8B8: use PNG or an uncompressed 32-bit DDS with alpha");
            return 0;
        }
        out->bgra = hdr[23] == 0x00FF0000u;
    }
    have = (hdr[2] & 0x20000) ? hdr[7] : 1;
    if (!have) have = 1;
    if (nm > R1HD_MAX_LEVELS || have < nm) {
        snprintf(why, whylen, "an HD replacement needs all %u mip levels, the DDS has %u", nm, have);
        return 0;
    }
    if (s > 1 && r1hd_is_dxt(fmt) && (((w * s) & 3) || ((h * s) & 3))) {
        snprintf(why, whylen, "its size %ux%u is not whole 4x4 blocks", fw, fh);
        return 0;
    }
    for (l = 0; l < nm; l++) {
        const uint32_t lw = r1hd_level_dim(w, s, l), lh = r1hd_level_dim(h, s, l);
        const size_t sz = r1hd_level_size(fmt, lw, lh);
        if (l == 0 && sz > R1HD_MAX_LEVEL0) { snprintf(why, whylen, "too large (64 MB per image at most)"); return 0; }
        if (off + sz > n) { snprintf(why, whylen, "the file is truncated"); return 0; }
        out->off[l] = off; out->size[l] = sz;
        off += sz;
    }
    out->scale = s;
    out->levels = nm;
    return 1;
}

/* Copies level l of a checked replacement into upload rows `dst_pitch` apart, in the upload's decoded layout (DXT
 * blocks as they are; A8R8G8B8 as R,G,B,A bytes, what rsx_texture_decode makes of it). */
static inline void r1hd_copy_level(uint8_t* dst, uint32_t dst_pitch, const uint8_t* file, const r1hd_dds* d, uint32_t l,
                                   uint32_t w, uint32_t h, uint32_t fmt)
{
    const uint32_t lw = r1hd_level_dim(w, d->scale, l), lh = r1hd_level_dim(h, d->scale, l);
    const uint32_t row = r1hd_level_row(fmt, lw), rows = r1hd_level_rows(fmt, lh);
    const uint8_t* src = file + d->off[l];
    uint32_t y, x;
    for (y = 0; y < rows; y++) {
        uint8_t* o = dst + (size_t)y * dst_pitch;
        const uint8_t* s = src + (size_t)y * row;
        if (r1hd_is_dxt(fmt) || !d->bgra) { memcpy(o, s, row); continue; }
        for (x = 0; x < row; x += 4) { o[x] = s[x + 2]; o[x + 1] = s[x + 1]; o[x + 2] = s[x]; o[x + 3] = s[x + 3]; }
    }
}

/* File name -> key: the first 16 characters must be hex digits */
static inline int r1hd_key_of_name(const char* name, uint64_t* key)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < 16; i++) {
        const char c = name[i];
        uint32_t dgt;
        if (c >= '0' && c <= '9') dgt = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') dgt = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') dgt = (uint32_t)(c - 'A' + 10);
        else return 0;
        v = (v << 4) | dgt;
    }
    *key = v;
    return 1;
}

/* RSX_HD_UI: 0 / off / unset = off; 1 / on = mods\hd_ui; anything else = that folder. Returns 1 when on. */
static inline int r1hd_env_dir(const char* e, char* out, size_t outlen)
{
    if (!e || !*e || !strcmp(e, "0") || ((e[0] == 'o' || e[0] == 'O') && (e[1] == 'f' || e[1] == 'F') && (e[2] == 'f' || e[2] == 'F') && !e[3]))
        return 0;
    snprintf(out, outlen, "%s", (!strcmp(e, "1") || ((e[0] == 'o' || e[0] == 'O') && (e[1] == 'n' || e[1] == 'N') && !e[2])) ? "mods\\hd_ui" : e);
    return 1;
}

/* RSX_HD_UI_CACHE_MB: MB of loaded replacements kept in memory (64..8192, default 512) */
static inline size_t r1hd_cache_budget(const char* e)
{
    int mb = (e && *e) ? atoi(e) : 512;
    if (mb < 64) mb = 64;
    if (mb > 8192) mb = 8192;
    return (size_t)mb << 20;
}

/* "Replaced so far" log lines: the first three, then 10, 25, 50 and every 100th (as R2/R3) */
static inline int r1hd_milestone(uint32_t n) { return n <= 3 || n == 10 || n == 25 || n == 50 || (n % 100) == 0; }

#ifdef __cplusplus
}
#endif

#endif
