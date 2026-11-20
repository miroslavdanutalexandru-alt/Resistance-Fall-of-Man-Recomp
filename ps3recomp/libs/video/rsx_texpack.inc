/* Texture packs for the D3D12 backend: replace guest textures by a hash of
 * their guest bytes. Included by rsx_d3d12_backend.c.
 *
 *   RSX_TEXDUMP=1|<dir>  write every uploaded texture once to texdump\ (or <dir>)
 *                        as <key>_<w>x<h>_<fmt>_m<mips>.png (RGBA8) or .dds (DXT1/3/5),
 *                        listed in <dir>\manifest.tsv (key, size, format, mips, file)
 *   RSX_TEXPACK=1|<dir>  load replacements from mods\textures\ (or <dir>);
 *                        a file is matched by the leading 16 hex digits of its name
 *
 * Replacements must keep the guest texture's size; DDS files must also keep its
 * DXT format. Missing mips are generated (PNG) or kept from the guest (DDS).
 * Both off: one cached int test per upload.
 *
 *   RSX_HD_UI=1|<dir>    the HD UI pack: load 2x..8x replacements from mods\hd_ui\ (or <dir>), made on this PC by
 *                        the launcher's Create HD UI pack (GPU) button (HDUIPack.exe). The texture is created at the
 *                        replacement's size; sampling uses normalized coordinates, so nothing else changes. Missing,
 *                        damaged or mismatched files fall back to the game's own texture.
 *   RSX_HD_UI_CACHE_MB=n MB of loaded replacements kept in memory (64..8192, default 512). */

#include "r1_hd_ui.h"

/* stb_image / stb_image_write are compiled into the runtime (cellPngDec.c,
 * cellPngEnc.c). */
extern unsigned char* stbi_load_from_memory(const unsigned char* buf, int len, int* x, int* y, int* comp, int req);
extern void stbi_image_free(void* p);
extern int stbi_write_png(const char* fn, int w, int h, int comp, const void* data, int stride);

typedef struct TpEntry {
    u64 hash;
    char path[MAX_PATH];
    int state;              /* 0 = not loaded, 1 = loaded, -1 = unusable */
    u32 w, h, dxfmt, nlev;
    u8* lev[16];
    u32 lev_size[16];
} TpEntry;

static int s_tp_dump = -1, s_tp_load = 0;   /* -1: env not read yet */
static char s_tp_dump_dir[MAX_PATH], s_tp_pack_dir[MAX_PATH];
static TpEntry* s_tp = NULL;
static u32 s_tp_n = 0;
static u64* s_tp_dumped = NULL;            /* open-addressed set of dumped hashes */
static u32 s_tp_dumped_cap = 0, s_tp_dumped_n = 0;
static u32 s_tp_replaced = 0;
static FILE* s_tp_manifest = NULL;          /* <dump dir>\manifest.tsv */

static int tp_env_dir(const char* name, const char* def, char* out)
{
    const char* e = getenv(name);
    if (!e || !*e || !strcmp(e, "0") || !_stricmp(e, "off")) return 0;
    snprintf(out, MAX_PATH, "%s", (!strcmp(e, "1") || !_stricmp(e, "on")) ? def : e);
    return 1;
}

static u64 tp_hex(const char* s, int* ok)
{
    u64 v = 0; *ok = 0;
    for (int i = 0; i < 16; i++) {
        const char c = s[i]; u32 d;
        if (c >= '0' && c <= '9') d = (u32)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (u32)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (u32)(c - 'A' + 10);
        else return 0;
        v = (v << 4) | d;
    }
    *ok = 1;
    return v;
}

static void tp_scan(const char* dir, int depth)
{
    char pat[MAX_PATH]; snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        char full[MAX_PATH]; snprintf(full, sizeof full, "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth < 4) tp_scan(full, depth + 1); continue; }
        const char* ext = strrchr(fd.cFileName, '.');
        if (!ext || (_stricmp(ext, ".png") && _stricmp(ext, ".dds"))) continue;
        int ok; const u64 hv = tp_hex(fd.cFileName, &ok);
        if (!ok) continue;
        if ((s_tp_n & 255u) == 0) s_tp = (TpEntry*)realloc(s_tp, (s_tp_n + 256u) * sizeof(TpEntry));
        if (!s_tp) { s_tp_n = 0; break; }
        TpEntry* t = &s_tp[s_tp_n++];
        memset(t, 0, sizeof *t);
        t->hash = hv;
        snprintf(t->path, sizeof t->path, "%s", full);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static int tp_cmp(const void* a, const void* b)
{
    const u64 x = ((const TpEntry*)a)->hash, y = ((const TpEntry*)b)->hash;
    return x < y ? -1 : x > y;
}

static int tp_dumped_insert(u64 hv);

static void tp_init(void)
{
    s_tp_dump = tp_env_dir("RSX_TEXDUMP", "texdump", s_tp_dump_dir);
    s_tp_load = tp_env_dir("RSX_TEXPACK", "mods\\textures", s_tp_pack_dir);
    if (s_tp_dump) {
        CreateDirectoryA(s_tp_dump_dir, NULL);
        /* manifest.tsv: one line per dumped texture; keys listed by an earlier run are not dumped again */
        char mp[MAX_PATH]; snprintf(mp, sizeof mp, "%s\\manifest.tsv", s_tp_dump_dir);
        FILE* in = fopen(mp, "r");
        const int had = in != NULL;
        if (in) {
            char line[512];
            while (fgets(line, sizeof line, in)) {
                int ok; const u64 hv = tp_hex(line, &ok);
                if (ok && line[16] == 9) tp_dumped_insert(hv);
            }
            fclose(in);
        }
        s_tp_manifest = fopen(mp, "a");
        if (s_tp_manifest && !had)
            fprintf(s_tp_manifest, "# key\twidth\theight\tformat\tmips\tfile%c"
                    "# key = 64-bit content hash of all mip levels + width/height/format/mips; stable across runs%c", 10, 10);
        fprintf(stderr, "[texpack] dumping textures to %s%c", s_tp_dump_dir, 10);
    }
    if (s_tp_load) {
        tp_scan(s_tp_pack_dir, 0);
        if (s_tp_n) qsort(s_tp, s_tp_n, sizeof(TpEntry), tp_cmp);
        fprintf(stderr, "[texpack] %u replacement textures in %s%c", s_tp_n, s_tp_pack_dir, 10);
        if (!s_tp_n) s_tp_load = 0;
    }
}

/* Texture key, stable across runs (never the guest address): 64-bit hash of
 * the guest bytes of every mip level, seeded with width/height/format/mips.
 * r1_hd_ui.h has it, shared with the HD UI pack builder (HDUIPack.exe). */
static u64 tp_hash(const u8* p, u32 n0, u32 w, u32 h, u32 fmt, u32 nm, int bc, u32 bb)
{
    return r1hd_hash(p, n0, w, h, fmt, nm, bc, bb);
}

static int tp_dumped_insert(u64 hv)
{
    if (s_tp_dumped_n * 2 >= s_tp_dumped_cap) {
        const u32 nc = s_tp_dumped_cap ? s_tp_dumped_cap * 2 : 4096;
        u64* nt = (u64*)calloc(nc, sizeof(u64));
        if (!nt) return 0;
        for (u32 i = 0; i < s_tp_dumped_cap; i++) if (s_tp_dumped[i]) {
            u32 j = (u32)(s_tp_dumped[i] * 0x9E3779B97F4A7C15ull >> 40) & (nc - 1);
            while (nt[j]) j = (j + 1) & (nc - 1);
            nt[j] = s_tp_dumped[i];
        }
        free(s_tp_dumped); s_tp_dumped = nt; s_tp_dumped_cap = nc;
    }
    if (!hv) hv = 1;
    u32 j = (u32)(hv * 0x9E3779B97F4A7C15ull >> 40) & (s_tp_dumped_cap - 1);
    while (s_tp_dumped[j]) { if (s_tp_dumped[j] == hv) return 0; j = (j + 1) & (s_tp_dumped_cap - 1); }
    s_tp_dumped[j] = hv; s_tp_dumped_n++;
    return 1;
}

static u32 tp_fourcc(u32 dxfmt)
{
    return dxfmt == DXGI_FORMAT_BC1_UNORM ? 0x31545844u : dxfmt == DXGI_FORMAT_BC2_UNORM ? 0x33545844u : 0x35545844u;
}

static u32 tp_bc_bytes(u32 dxfmt) { return dxfmt == DXGI_FORMAT_BC1_UNORM ? 8u : 16u; }

/* Level l of the decoded upload: rows of `row` bytes, `rows` of them, at
 * `pitch` within the staging buffer. */
static void tp_level(u32 l, u32 w, u32 h, u32 dxfmt, u32 pitch0, const u32* lvl_off, const u32* lvl_pitch,
                     u32* off, u32* pitch, u32* row, u32* rows)
{
    const u32 lw = (w >> l) ? (w >> l) : 1, lh = (h >> l) ? (h >> l) : 1;
    const int bc = dxfmt != DXGI_FORMAT_R8G8B8A8_UNORM;
    *off = l ? lvl_off[l] : 0;
    *pitch = l ? lvl_pitch[l] : pitch0;
    *row = bc ? ((lw + 3) / 4) * tp_bc_bytes(dxfmt) : lw * 4;
    *rows = bc ? (lh + 3) / 4 : lh;
}

static void tp_dump(u64 hv, u32 w, u32 h, u32 fmt, u32 dxfmt, u32 nm, const u8* map, u32 pitch0,
                    const u32* lvl_off, const u32* lvl_pitch)
{
    if (!tp_dumped_insert(hv)) return;
    char fn[MAX_PATH];
    const int bc = dxfmt != DXGI_FORMAT_R8G8B8A8_UNORM;
    snprintf(fn, sizeof fn, "%s\\%016llx_%ux%u_%02x_m%u.%s", s_tp_dump_dir, (unsigned long long)hv, w, h, fmt & 0xFFu,
             nm, bc ? "dds" : "png");
    if (s_tp_manifest) {
        fprintf(s_tp_manifest, "%016llx\t%u\t%u\t%s\t%u\t%s%c", (unsigned long long)hv, w, h,
                !bc ? "RGBA8" : dxfmt == DXGI_FORMAT_BC1_UNORM ? "DXT1" : dxfmt == DXGI_FORMAT_BC2_UNORM ? "DXT3" : "DXT5",
                nm, fn + strlen(s_tp_dump_dir) + 1, 10);
        fflush(s_tp_manifest);
    }
    if (GetFileAttributesA(fn) != INVALID_FILE_ATTRIBUTES) return;
    if (!bc) {
        /* the staging buffer is write-combined: copy out before encoding */
        u8* px = (u8*)malloc((size_t)w * h * 4);
        if (!px) return;
        for (u32 y = 0; y < h; y++) memcpy(px + (size_t)y * w * 4, map + (size_t)y * pitch0, (size_t)w * 4);
        stbi_write_png(fn, (int)w, (int)h, 4, px, (int)w * 4);
        free(px);
    } else {
        FILE* f = fopen(fn, "wb");
        if (!f) return;
        u32 hdr[32] = {0};
        hdr[0] = 0x20534444u; hdr[1] = 124;
        hdr[2] = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000 | (nm > 1 ? 0x20000 : 0);
        hdr[3] = h; hdr[4] = w;
        hdr[5] = ((w + 3) / 4) * ((h + 3) / 4) * tp_bc_bytes(dxfmt);
        hdr[7] = nm;
        hdr[19] = 32; hdr[20] = 0x4; hdr[21] = tp_fourcc(dxfmt);
        hdr[27] = 0x1000 | (nm > 1 ? 0x400008 : 0);
        fwrite(hdr, 1, sizeof hdr, f);
        u8* tmp = (u8*)malloc(pitch0 ? pitch0 : 1);
        for (u32 l = 0; l < nm && tmp; l++) {
            u32 o, p, row, rows;
            tp_level(l, w, h, dxfmt, pitch0, lvl_off, lvl_pitch, &o, &p, &row, &rows);
            for (u32 y = 0; y < rows; y++) { memcpy(tmp, map + o + (size_t)y * p, row); fwrite(tmp, 1, row, f); }
        }
        free(tmp);
        fclose(f);
    }
    if (s_tp_dumped_n <= 4 || (s_tp_dumped_n % 500) == 0)
        fprintf(stderr, "[texpack] %u textures dumped to %s%c", s_tp_dumped_n, s_tp_dump_dir, 10);
}

static int tp_load(TpEntry* t, u32 w, u32 h, u32 dxfmt, u32 nm)
{
    FILE* f = fopen(t->path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    u8* file = (n > 0) ? (u8*)malloc((size_t)n) : NULL;
    if (!file || fread(file, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(file); return 0; }
    fclose(f);
    const char* ext = strrchr(t->path, '.');
    int ok = 0;
    t->w = w; t->h = h; t->dxfmt = dxfmt; t->nlev = 0;
    if (ext && !_stricmp(ext, ".dds")) {
        u32 hdr[32];
        if (dxfmt != DXGI_FORMAT_R8G8B8A8_UNORM && n >= 128) {
            memcpy(hdr, file, 128);
            const u32 want = tp_fourcc(dxfmt);
            const int fmt_ok = hdr[21] == want || (want == 0x33545844u && hdr[21] == 0x32545844u)
                            || (want == 0x35545844u && hdr[21] == 0x34545844u);
            if (hdr[0] == 0x20534444u && hdr[3] == h && hdr[4] == w && fmt_ok) {
                const u32 have = hdr[7] ? hdr[7] : 1;
                size_t o = 128;
                for (u32 l = 0; l < have && l < nm && l < 16; l++) {
                    const u32 lw = (w >> l) ? (w >> l) : 1, lh = (h >> l) ? (h >> l) : 1;
                    const size_t sz = (size_t)((lw + 3) / 4) * ((lh + 3) / 4) * tp_bc_bytes(dxfmt);
                    if (o + sz > (size_t)n) break;
                    t->lev[l] = (u8*)malloc(sz);
                    if (!t->lev[l]) break;
                    memcpy(t->lev[l], file + o, sz); t->lev_size[l] = (u32)sz;
                    o += sz; t->nlev = l + 1;
                }
                ok = t->nlev > 0;
            }
        }
    } else if (dxfmt == DXGI_FORMAT_R8G8B8A8_UNORM) {
        int iw = 0, ih = 0, ic = 0;
        u8* px = stbi_load_from_memory(file, (int)n, &iw, &ih, &ic, 4);
        if (px && (u32)iw == w && (u32)ih == h) {
            u32 lw = w, lh = h;
            u8* cur = px;
            for (u32 l = 0; l < nm && l < 16; l++) {
                const size_t sz = (size_t)lw * lh * 4;
                t->lev[l] = (u8*)malloc(sz);
                if (!t->lev[l]) break;
                memcpy(t->lev[l], cur, sz); t->lev_size[l] = (u32)sz; t->nlev = l + 1;
                if ((lw == 1 && lh == 1) || l + 1 >= nm) break;
                const u32 nw = lw > 1 ? lw / 2 : 1, nh = lh > 1 ? lh / 2 : 1;
                const u8* nx = cur;
                u8* dst = (u8*)malloc((size_t)nw * nh * 4);
                if (!dst) break;
                for (u32 y = 0; y < nh; y++) for (u32 x = 0; x < nw; x++) for (u32 c = 0; c < 4; c++) {
                    u32 s = 0;
                    for (u32 dy = 0; dy < 2; dy++) for (u32 dx = 0; dx < 2; dx++) {
                        const u32 sy = (y * 2 + dy < lh) ? y * 2 + dy : lh - 1, sx = (x * 2 + dx < lw) ? x * 2 + dx : lw - 1;
                        s += nx[((size_t)sy * lw + sx) * 4 + c];
                    }
                    dst[((size_t)y * nw + x) * 4 + c] = (u8)((s + 2) / 4);
                }
                if (cur != px) free(cur);
                cur = dst; lw = nw; lh = nh;
            }
            if (cur != px) free(cur);
            ok = t->nlev > 0;
        } else if (px && (u32)iw != w) {
            fprintf(stderr, "[texpack] %s: %dx%d, the game texture is %ux%u -- skipped%c", t->path, iw, ih, w, h, 10);
        }
        if (px) stbi_image_free(px);
    }
    free(file);
    return ok;
}

static TpEntry* tp_find(u64 hv)
{
    u32 lo = 0, hi = s_tp_n;
    while (lo < hi) {
        const u32 mid = (lo + hi) / 2;
        if (s_tp[mid].hash < hv) lo = mid + 1; else hi = mid;
    }
    return (lo < s_tp_n && s_tp[lo].hash == hv) ? &s_tp[lo] : NULL;
}

/* Called after the guest texture has been decoded into the staging buffer and
 * before it is copied to the GPU. 2D RGBA8 / BC1-3 textures only. */
static void tp_upload(const u8* src, u32 src_bytes, u32 w, u32 h, u32 fmt, u32 dxfmt, u32 nm,
                      u8* map, u32 pitch0, const u32* lvl_off, const u32* lvl_pitch)
{
    if (dxfmt != DXGI_FORMAT_R8G8B8A8_UNORM && dxfmt != DXGI_FORMAT_BC1_UNORM &&
        dxfmt != DXGI_FORMAT_BC2_UNORM && dxfmt != DXGI_FORMAT_BC3_UNORM) return;
    if (w < 4 || h < 4 || !src_bytes) return;
    const int bc = dxfmt != DXGI_FORMAT_R8G8B8A8_UNORM;
    const u64 hv = tp_hash(src, src_bytes, w, h, fmt & 0xFFu, nm, bc, bc ? tp_bc_bytes(dxfmt) : (src_bytes / (w * h) ? src_bytes / (w * h) : 1u));
    if (s_tp_load) {
        TpEntry* t = tp_find(hv);
        if (t && t->state == 0) {
            t->state = tp_load(t, w, h, dxfmt, nm) ? 1 : -1;
            if (t->state < 0) fprintf(stderr, "[texpack] %s does not match the game texture (%ux%u) -- skipped%c", t->path, w, h, 10);
        }
        if (t && t->state == 1 && t->w == w && t->h == h && t->dxfmt == dxfmt) {
            for (u32 l = 0; l < t->nlev && l < nm; l++) {
                u32 o, p, row, rows;
                tp_level(l, w, h, dxfmt, pitch0, lvl_off, lvl_pitch, &o, &p, &row, &rows);
                if ((size_t)row * rows != t->lev_size[l]) break;
                for (u32 y = 0; y < rows; y++) memcpy(map + o + (size_t)y * p, t->lev[l] + (size_t)y * row, row);
            }
            if (s_tp_replaced++ < 4 || (s_tp_replaced % 1000) == 0)
                fprintf(stderr, "[texpack] replaced %u textures (latest %s)%c", s_tp_replaced, t->path, 10);
            return;
        }
    }
    if (s_tp_dump) tp_dump(hv, w, h, fmt, dxfmt, nm, map, pitch0, lvl_off, lvl_pitch);
}

/* ---------------------------------------------------------------------------
 * HD UI pack (RSX_HD_UI): replacements 2x..8x the game texture's size, keyed
 * like the texture pack (r1_hd_ui.h). The upload creates the texture at the
 * replacement's size and copies the file's levels instead of decoding the
 * guest's; the SRV samples it with the same normalized coordinates. Texture
 * slots are only ever sampled (render targets live in the off_rt pool and
 * nothing copies out of a slot), and unnormalized-coordinate textures are
 * never replaced, so the larger size changes nothing else.
 * -------------------------------------------------------------------------*/
typedef struct HdEntry {
    u64 key;
    char path[MAX_PATH];
    int state;              /* 0 = not loaded, 1 = loaded, -1 = unusable */
    u8* file;
    size_t bytes;
    r1hd_dds dds;
} HdEntry;

static int s_hd_on = -1;                    /* -1: env not read yet */
static int s_hd_mips_env = 1;               /* RSX_TEX_MIPS, as the upload reads it */
static char s_hd_dir[MAX_PATH];
static HdEntry* s_hd = NULL;
static u32 s_hd_n = 0, s_hd_replaced = 0;
static size_t s_hd_cache = 0, s_hd_budget = 512u << 20;

static void hd_scan(const char* dir, int depth)
{
    char pat[MAX_PATH]; snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        /* '.' names: tool scratch (.work, .tmp) is never loaded as a replacement */
        if (fd.cFileName[0] == '.') continue;
        char full[MAX_PATH]; snprintf(full, sizeof full, "%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth < 8) hd_scan(full, depth + 1); continue; }
        const char* ext = strrchr(fd.cFileName, '.');
        if (!ext || _stricmp(ext, ".dds")) continue;   /* HDUIPack.exe writes DDS (every level, in the game's format) */
        u64 hv;
        if (strlen(fd.cFileName) < 16 || !r1hd_key_of_name(fd.cFileName, &hv)) continue;
        if ((s_hd_n & 255u) == 0) {
            HdEntry* n = (HdEntry*)realloc(s_hd, (s_hd_n + 256u) * sizeof(HdEntry));
            if (!n) break;
            s_hd = n;
        }
        HdEntry* t = &s_hd[s_hd_n++];
        memset(t, 0, sizeof *t);
        t->key = hv;
        snprintf(t->path, sizeof t->path, "%s", full);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static int hd_cmp(const void* a, const void* b)
{
    const u64 x = ((const HdEntry*)a)->key, y = ((const HdEntry*)b)->key;
    return x < y ? -1 : x > y;
}

static void hd_init(void)
{
    s_hd_on = r1hd_env_dir(getenv("RSX_HD_UI"), s_hd_dir, sizeof s_hd_dir);
    if (!s_hd_on) return;
    { const char* e = getenv("RSX_TEX_MIPS"); s_hd_mips_env = e ? atoi(e) : 1; }
    s_hd_budget = r1hd_cache_budget(getenv("RSX_HD_UI_CACHE_MB"));
    hd_scan(s_hd_dir, 0);
    if (s_hd_n) qsort(s_hd, s_hd_n, sizeof(HdEntry), hd_cmp);
    fprintf(stderr, "[texpack] HD UI pack: %u texture(s) in %s%s%c", s_hd_n, s_hd_dir,
            s_hd_n ? "" : " (none: make it with the launcher's Create HD UI pack button)", 10);
    if (!s_hd_n) s_hd_on = 0;
}

static HdEntry* hd_find(u64 hv)
{
    u32 lo = 0, hi = s_hd_n;
    while (lo < hi) {
        const u32 mid = (lo + hi) / 2;
        if (s_hd[mid].key < hv) lo = mid + 1; else hi = mid;
    }
    return (lo < s_hd_n && s_hd[lo].key == hv) ? &s_hd[lo] : NULL;
}

/* The replacement for the texture about to be uploaded, loaded and checked, or NULL (the game's own texture is
 * used). gcm = format byte, nm = the levels the upload sends, row_bytes = level 0's source row stride. */
static HdEntry* hd_lookup(const u8* src, u32 gcm, u32 w, u32 h, u32 mips, u32 tpitch, int cube, u32 nm, u32 row_bytes)
{
    if (s_hd_on < 0) hd_init();
    if (!s_hd_on || r1hd_ineligible(gcm, w, h, cube, rsx_texture_argb_is_rgba())) return NULL;
    u64 key; u32 knm = 0;
    if (!r1hd_texture_key(src, ~0ull, gcm, w, h, mips, tpitch, cube, s_hd_mips_env, &key, &knm, NULL) || knm != nm) return NULL;
    HdEntry* e = hd_find(key);
    if (!e || e->state < 0) return NULL;
    if (r1hd_is_fe_atlas(src, row_bytes, gcm, w, h)) return NULL;   /* the port redraws its button pictures */
    if (e->state == 0) {
        char why[160] = "cannot read the file";
        FILE* f = fopen(e->path, "rb");
        long n = -1;
        if (f) { fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET); }
        e->file = (n > 0) ? (u8*)malloc((size_t)n) : NULL;
        int ok = f && e->file && fread(e->file, 1, (size_t)n, f) == (size_t)n;
        if (f) fclose(f);
        if (ok) ok = r1hd_dds_check(e->file, (size_t)n, w, h, gcm, nm, &e->dds, why, sizeof why);
        if (!ok) {
            const char* b = strrchr(e->path, '\\');
            /* the game's texture is used: a bad file never breaks the picture */
            fprintf(stderr, "[texpack] HD UI pack: %s skipped: %s (game texture %ux%u %s, %u mips)%c",
                    b ? b + 1 : e->path, why, w, h, r1hd_fmt_name(gcm), nm, 10);
            free(e->file); e->file = NULL; e->state = -1;
            return NULL;
        }
        e->bytes = (size_t)n; e->state = 1;
        s_hd_cache += e->bytes;
    }
    return e;
}

/* Fills the upload rows (level 0 at 0 / pitch0, level l at lvl_off[l] / lvl_pitch[l]) from a replacement */
static void hd_fill(const HdEntry* e, u8* map, u32 pitch0, const u32* lvl_off, const u32* lvl_pitch, u32 w, u32 h, u32 gcm, u32 nm)
{
    for (u32 l = 0; l < nm; l++)
        r1hd_copy_level(map + (l ? lvl_off[l] : 0), l ? lvl_pitch[l] : pitch0, e->file, &e->dds, l, w, h, gcm);
    if (r1hd_milestone(++s_hd_replaced)) {
        const char* b = strrchr(e->path, '\\');
        fprintf(stderr, "[texpack] HD UI pack: %u texture upload(s) replaced so far (latest %s, %ux%u -> %ux%u)%c",
                s_hd_replaced, b ? b + 1 : e->path, w, h, w * e->dds.scale, h * e->dds.scale, 10);
    }
}

/* After the copy is recorded. A replacement staged in the slot's own upload buffer (the batch's upload ring was
 * full) does not keep that buffer: it is HD-sized, and a slot keeps its buffer for as long as it lives, so screens
 * full of replacements would pin hundreds of MB of upload heap that RSX_HD_UI_CACHE_MB does not count. `bury`
 * frees it once the batch that copies from it is done (0 = could not take it: then the slot keeps it, as before). */
static void hd_stage_done(int hd, int use_ring, ID3D12Resource** up, u32* up_batch, int (*bury)(ID3D12Resource*))
{
    if (!hd || use_ring || !*up || !bury(*up)) return;
    *up = NULL; *up_batch = 0;
}

/* After the copy is recorded: over the memory budget, this replacement is dropped (read again if re-uploaded) */
static void hd_after_upload(HdEntry* e)
{
    if (!e || s_hd_cache <= s_hd_budget) return;
    s_hd_cache -= e->bytes < s_hd_cache ? e->bytes : s_hd_cache;
    free(e->file); e->file = NULL; e->bytes = 0; e->state = 0;
}