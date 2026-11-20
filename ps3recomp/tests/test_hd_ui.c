/*
 * HD UI pack, renderer side (R1 port, RSX_HD_UI): rsx_texpack.inc's HD path as the D3D12 backend's texture upload
 * runs it, on synthetic guest textures and pack folders.
 *
 *   - off (RSX_HD_UI unset / 0, or an empty pack): no lookup, no replacement
 *   - a valid 2x DXT5 replacement with mips and a 4x A8R8G8B8 one (B,G,R,A in the DDS): found by the key the upload
 *     computes, and hd_fill stages every level exactly as the file has it (A8R8G8B8 as R,G,B,A) in the backend's
 *     upload layout (r1hd_upload_layout)
 *   - fallbacks: not a DDS, a non-integer or too large scale, the wrong format, missing levels, a truncated file:
 *     the game's texture is used, the file is marked unusable and not read again
 *   - never replaced: unnormalized coordinates, cube maps, textures under 8x8, a texture whose bytes changed
 *   - folder scan: subfolders yes, '.' folders (tool scratch) and non-DDS files no
 *   - memory budget: a replacement over it is dropped after the upload and read again next time
 *   - D3D12 (WARP, skipped when there is none): a texture created at the replacement's size from the staged upload
 *     with the backend's footprints reads back as the file's levels
 *   - per-slot upload buffers (the batch's upload ring full): HD uploads do not keep their HD-sized buffer in the
 *     slot (many binds in a batch leave none behind), game textures keep theirs, a full grave keeps it
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "ps3emu/ps3types.h"
#include "rsx_texture_layout.h"

/* rsx_texpack.inc's texture-pack side links against stb (in the runtime); not exercised here */
unsigned char* stbi_load_from_memory(const unsigned char* buf, int len, int* x, int* y, int* comp, int req)
{ (void)buf; (void)len; (void)x; (void)y; (void)comp; (void)req; return NULL; }
void stbi_image_free(void* p) { free(p); }
int stbi_write_png(const char* fn, int w, int h, int comp, const void* data, int stride)
{ (void)fn; (void)w; (void)h; (void)comp; (void)data; (void)stride; return 0; }

#include "rsx_texpack.inc"

static int g_fail = 0, g_checks = 0;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng = 0x2468ACE1u;
static uint32_t xr(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static void fill(uint8_t* p, size_t n) { for (size_t i = 0; i < n; i++) p[i] = (uint8_t)xr(); }

static char g_dir[MAX_PATH];

static void hd_reset(const char* env)
{
    for (u32 i = 0; i < s_hd_n; i++) free(s_hd[i].file);
    free(s_hd); s_hd = NULL; s_hd_n = 0; s_hd_replaced = 0; s_hd_cache = 0;
    s_hd_on = -1;
    _putenv_s("RSX_HD_UI", env ? env : "");
}

/* A guest texture: bytes as the upload reads them, its key and level count */
typedef struct { uint8_t* p; size_t n; u32 gcm, w, h, mips, pitch; u64 key; u32 nm; u32 row; } GTex;

static GTex make_tex(u32 gcm, u32 w, u32 h, u32 mips, u32 pitch)
{
    GTex t; memset(&t, 0, sizeof t);
    t.gcm = gcm; t.w = w; t.h = h; t.mips = mips; t.pitch = pitch;
    u64 need = 0; u32 nm = 0;
    r1hd_texture_key(NULL, 0, gcm, w, h, mips, pitch, 0, 1, NULL, &nm, &need);
    t.n = (size_t)need + 64;
    t.p = (uint8_t*)malloc(t.n);
    fill(t.p, t.n);
    CHECK(r1hd_texture_key(t.p, t.n, gcm, w, h, mips, pitch, 0, 1, &t.key, &t.nm, NULL), "key for %ux%u 0x%02x", w, h, gcm);
    rsx_tex_layout tl; rsx_texture_layout_pitched(gcm, w, h, mips > 1 ? 0 : pitch, &tl);
    t.row = tl.row_bytes;
    return t;
}

/* A DDS for t at `scale` with `levels` levels; bgra for A8R8G8B8. Returns the file bytes (caller frees). */
static uint8_t* make_dds(const GTex* t, u32 scale, u32 levels, u32 fmt_override, size_t* n_out)
{
    const u32 fmt = fmt_override ? fmt_override : t->gcm;
    size_t n = 128;
    for (u32 l = 0; l < levels; l++) n += r1hd_level_size(fmt, r1hd_level_dim(t->w, scale, l), r1hd_level_dim(t->h, scale, l));
    uint8_t* f = (uint8_t*)malloc(n);
    uint32_t hdr[32];
    r1hd_dds_header(hdr, t->w * scale, t->h * scale, fmt, levels);
    memcpy(f, hdr, 128);
    fill(f + 128, n - 128);
    *n_out = n;
    return f;
}

static void write_file(const char* rel, const void* d, size_t n)
{
    char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\%s", g_dir, rel);
    for (char* s = p + strlen(g_dir) + 1; *s; s++)
        if (*s == '\\') { *s = 0; CreateDirectoryA(p, NULL); *s = '\\'; }
    FILE* f = fopen(p, "wb"); if (f) { fwrite(d, 1, n, f); fclose(f); }
}

static void dds_name(char* out, size_t n, const GTex* t, u32 scale, const char* sub)
{
    snprintf(out, n, "%s%s%016llx_%ux%u_%s_m%u.dds", sub ? sub : "", sub ? "\\" : "", (unsigned long long)t->key,
             t->w * scale, t->h * scale, r1hd_fmt_name(t->gcm), t->nm);
}

/* Stages t's replacement the way the backend does and checks every level against the file */
static int staged_matches(HdEntry* e, const GTex* t, const uint8_t* file, uint8_t** stage_out, u32* lo, u32* lp, u32* lr, u64* total)
{
    u32 off[16] = {0}, pit[16] = {0}, rows[16] = {0};
    const u32 s = e->dds.scale;
    const u64 up = r1hd_upload_layout(t->gcm, t->w, t->h, s, t->nm, off, pit, rows);
    uint8_t* st = (uint8_t*)calloc(1, (size_t)up);
    const u32 pitch0 = pit[0];
    u32 off_b[16], pit_b[16];
    memcpy(off_b, off, sizeof off); memcpy(pit_b, pit, sizeof pit);
    off_b[0] = pit_b[0] = 0;
    hd_fill(e, st, pitch0, off_b, pit_b, t->w, t->h, t->gcm, t->nm);
    int ok = 1;
    for (u32 l = 0; l < t->nm; l++) {
        const u32 lw = r1hd_level_dim(t->w, s, l), lh = r1hd_level_dim(t->h, s, l);
        const u32 row = r1hd_level_row(t->gcm, lw), nr = r1hd_level_rows(t->gcm, lh);
        const uint8_t* src = file + e->dds.off[l];
        for (u32 y = 0; y < nr && ok; y++) {
            const uint8_t* a = st + off[l] + (size_t)y * pit[l];
            const uint8_t* b = src + (size_t)y * row;
            if (r1hd_is_dxt(t->gcm) || !e->dds.bgra) ok = !memcmp(a, b, row);
            else for (u32 x = 0; x < row && ok; x += 4)
                ok = a[x] == b[x + 2] && a[x + 1] == b[x + 1] && a[x + 2] == b[x] && a[x + 3] == b[x + 3];
        }
    }
    if (stage_out) { *stage_out = st; memcpy(lo, off, sizeof off); memcpy(lp, pit, sizeof pit); memcpy(lr, rows, sizeof rows); *total = up; }
    else free(st);
    return ok;
}

/* ---- per-slot upload buffers (ring full): an HD upload buries its HD-sized buffer, a game texture keeps its own ---- */
static ID3D12Resource* g_buried[512];
static int g_buried_n, g_bury_cap;
static int mock_bury(ID3D12Resource* r) { if (g_buried_n >= g_bury_cap) return 0; g_buried[g_buried_n++] = r; return 1; }
static ID3D12Device* warp_device(void)
{
    IDXGIFactory4* fac = NULL; IDXGIAdapter* ad = NULL; ID3D12Device* dev = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory4, (void**)&fac))) return NULL;
    if (SUCCEEDED(IDXGIFactory4_EnumWarpAdapter(fac, &IID_IDXGIAdapter, (void**)&ad)))
        if (FAILED(D3D12CreateDevice((IUnknown*)ad, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void**)&dev))) dev = NULL;
    if (ad) IDXGIAdapter_Release(ad);
    IDXGIFactory4_Release(fac);
    return dev;
}
/* the backend's per-slot staging: created when the slot has none */
static int slot_up(ID3D12Device* dev, ID3D12Resource** up, u32* up_batch, u64 size)
{
    if (*up) return 1;
    D3D12_HEAP_PROPERTIES hu = {0}; hu.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {0};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = size; bd.Height = 1; bd.DepthOrArraySize = 1;
    bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(ID3D12Device_CreateCommittedResource(dev, &hu, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                                   &IID_ID3D12Resource, (void**)up))) { *up = NULL; return 0; }
    *up_batch = 7;
    return 1;
}
static int warp_stage_test(void)
{
    ID3D12Device* dev = warp_device();
    if (!dev) return -1;
    enum { SLOTS = 96 };
    const u64 hd_bytes = 4u << 20;               /* a 1024x1024 DXT5 replacement with its levels is ~1.3 MB; 4 MB = a big one */
    ID3D12Resource* up[SLOTS] = {0}; u32 ub[SLOTS] = {0};
    int made = 0, kept = 0;
    g_buried_n = 0; g_bury_cap = 512;
    for (int b = 0; b < 3; b++)                  /* three batches, every slot bound with a replacement each time */
        for (int i = 0; i < SLOTS; i++) {
            made += slot_up(dev, &up[i], &ub[i], hd_bytes);
            hd_stage_done(1, 0, &up[i], &ub[i], mock_bury);
        }
    for (int i = 0; i < SLOTS; i++) kept += up[i] != NULL || ub[i] != 0;
    CHECK(made == 3 * SLOTS && kept == 0 && g_buried_n == 3 * SLOTS,
          "HD binds with the ring full: %d buffers made, %d slots still hold one, %d buried", made, kept, g_buried_n);
    int last = 1;
    for (int i = 0; i < g_buried_n; i++) last = (int)ID3D12Resource_Release(g_buried[i]);
    CHECK(last == 0, "a buried buffer has no other reference (freed when the batch is done)");
    /* a game texture keeps its buffer (reused by the next upload); so does a ring upload and a full grave */
    g_buried_n = 0;
    slot_up(dev, &up[0], &ub[0], 64u << 10); hd_stage_done(0, 0, &up[0], &ub[0], mock_bury);
    CHECK(up[0] && ub[0] == 7 && g_buried_n == 0, "game texture keeps its upload buffer");
    hd_stage_done(1, 1, &up[0], &ub[0], mock_bury);
    CHECK(up[0] && g_buried_n == 0, "ring upload leaves the slot buffer alone");
    g_bury_cap = 0;
    hd_stage_done(1, 0, &up[0], &ub[0], mock_bury);
    CHECK(up[0] && ub[0] == 7, "grave full: the slot keeps the buffer rather than losing it");
    ID3D12Resource_Release(up[0]);
    ID3D12Device_Release(dev);
    return 1;
}

/* ---- D3D12 on WARP: create at the replacement's size, copy with the backend's footprints, read back ---- */
static int warp_check(const GTex* t, HdEntry* e, const uint8_t* file)
{
    IDXGIFactory4* fac = NULL; IDXGIAdapter* ad = NULL; ID3D12Device* dev = NULL;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory4, (void**)&fac))) return -1;
    if (FAILED(IDXGIFactory4_EnumWarpAdapter(fac, &IID_IDXGIAdapter, (void**)&ad)) ||
        FAILED(D3D12CreateDevice((IUnknown*)ad, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void**)&dev))) {
        if (ad) IDXGIAdapter_Release(ad);
        IDXGIFactory4_Release(fac);
        return -1;
    }
    uint8_t* st; u32 off[16], pit[16], rows[16]; u64 up;
    staged_matches(e, t, file, &st, off, pit, rows, &up);
    const u32 s = e->dds.scale, cw = t->w * s, ch = t->h * s;
    const DXGI_FORMAT df = r1hd_base(t->gcm) == R1HD_DXT1 ? DXGI_FORMAT_BC1_UNORM : r1hd_base(t->gcm) == R1HD_DXT23 ? DXGI_FORMAT_BC2_UNORM :
                           r1hd_base(t->gcm) == R1HD_DXT45 ? DXGI_FORMAT_BC3_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    ID3D12Resource *tex = NULL, *upb = NULL, *rb = NULL;
    ID3D12CommandQueue* q = NULL; ID3D12CommandAllocator* ca = NULL; ID3D12GraphicsCommandList* cl = NULL; ID3D12Fence* fe = NULL;
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {0};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = cw; td.Height = ch; td.DepthOrArraySize = 1;
    td.MipLevels = (UINT16)t->nm; td.Format = df; td.SampleDesc.Count = 1;
    int ok = SUCCEEDED(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&tex));
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[16]; UINT64 rbsize = 0;
    ID3D12Device_GetCopyableFootprints(dev, &td, 0, t->nm, 0, fp, NULL, NULL, &rbsize);
    D3D12_HEAP_PROPERTIES hu = {0}; hu.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES hr = {0}; hr.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {0};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Width = up;
    ok = ok && SUCCEEDED(ID3D12Device_CreateCommittedResource(dev, &hu, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&upb));
    bd.Width = rbsize;
    ok = ok && SUCCEEDED(ID3D12Device_CreateCommittedResource(dev, &hr, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&rb));
    D3D12_COMMAND_QUEUE_DESC qd = {0}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ok = ok && SUCCEEDED(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void**)&q));
    ok = ok && SUCCEEDED(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void**)&ca));
    ok = ok && SUCCEEDED(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, NULL, &IID_ID3D12GraphicsCommandList, (void**)&cl));
    ok = ok && SUCCEEDED(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void**)&fe));
    if (ok) {
        void* m = NULL; D3D12_RANGE nr = {0, 0};
        ID3D12Resource_Map(upb, 0, &nr, &m);
        memcpy(m, st, (size_t)up);
        ID3D12Resource_Unmap(upb, 0, NULL);
        /* the backend's copy: level 0 by (cw, ch, pitch), level L by (cw >> L, ch >> L, lvl_pitch[L]) at lvl_off[L] */
        D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
        dst.pResource = tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = upb; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint.Format = df; src.PlacedFootprint.Footprint.Depth = 1;
        for (u32 L = 0; L < t->nm; L++) {
            u32 lw = cw >> L, lh = ch >> L; if (!lw) lw = 1; if (!lh) lh = 1;
            dst.SubresourceIndex = L;
            src.PlacedFootprint.Offset = off[L];
            src.PlacedFootprint.Footprint.Width = lw; src.PlacedFootprint.Footprint.Height = lh;
            src.PlacedFootprint.Footprint.RowPitch = pit[L];
            ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
        }
        D3D12_RESOURCE_BARRIER b = {0};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = tex;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
        for (u32 L = 0; L < t->nm; L++) {
            D3D12_TEXTURE_COPY_LOCATION s2 = {0}, d2 = {0};
            s2.pResource = tex; s2.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; s2.SubresourceIndex = L;
            d2.pResource = rb; d2.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; d2.PlacedFootprint = fp[L];
            ID3D12GraphicsCommandList_CopyTextureRegion(cl, &d2, 0, 0, 0, &s2, NULL);
        }
        ID3D12GraphicsCommandList_Close(cl);
        ID3D12CommandQueue_ExecuteCommandLists(q, 1, (ID3D12CommandList* const*)&cl);
        ID3D12CommandQueue_Signal(q, fe, 1);
        HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
        ID3D12Fence_SetEventOnCompletion(fe, 1, ev);
        WaitForSingleObject(ev, 10000);
        CloseHandle(ev);
        ok = ID3D12Device_GetDeviceRemovedReason(dev) == S_OK;
        uint8_t* r = NULL; D3D12_RANGE rr = {0, (SIZE_T)rbsize};
        ok = ok && SUCCEEDED(ID3D12Resource_Map(rb, 0, &rr, (void**)&r));
        for (u32 L = 0; ok && L < t->nm; L++) {
            const u32 lw = r1hd_level_dim(t->w, s, L), lh = r1hd_level_dim(t->h, s, L);
            const u32 row = r1hd_level_row(t->gcm, lw), nr2 = r1hd_level_rows(t->gcm, lh);
            for (u32 y = 0; ok && y < nr2; y++)
                ok = !memcmp(r + fp[L].Offset + (size_t)y * fp[L].Footprint.RowPitch, st + off[L] + (size_t)y * pit[L], row);
        }
        if (r) ID3D12Resource_Unmap(rb, 0, NULL);
    }
    if (fe) ID3D12Fence_Release(fe);
    if (cl) ID3D12GraphicsCommandList_Release(cl);
    if (ca) ID3D12CommandAllocator_Release(ca);
    if (q) ID3D12CommandQueue_Release(q);
    if (rb) ID3D12Resource_Release(rb);
    if (upb) ID3D12Resource_Release(upb);
    if (tex) ID3D12Resource_Release(tex);
    ID3D12Device_Release(dev); IDXGIAdapter_Release(ad); IDXGIFactory4_Release(fac);
    free(st);
    return ok;
}

static HdEntry* look(const GTex* t)
{
    return hd_lookup(t->p, t->gcm, t->w, t->h, t->mips, t->pitch, 0, t->nm, t->row);
}

int main(void)
{
    char tmp[MAX_PATH];
    GetCurrentDirectoryA(sizeof tmp, tmp);
    snprintf(g_dir, sizeof g_dir, "%s\\hd_ui_test_pack", tmp);
    CreateDirectoryA(g_dir, NULL);

    /* swizzled DXT5 with 7 levels, linear pitched A8R8G8B8 (1 level), swizzled A8R8G8B8 with mips */
    GTex dxt = make_tex(0x88, 128, 64, 7, 0);
    GTex argb = make_tex(0x85 | 0x20, 40, 24, 1, 256);
    GTex argbm = make_tex(0x85, 32, 32, 6, 0);
    CHECK(dxt.nm == 5, "DXT5 128x64 uploads 5 whole-block levels, got %u", dxt.nm);
    CHECK(argb.nm == 1 && argbm.nm == 6, "levels %u %u", argb.nm, argbm.nm);

    size_t n1, n2, n3;
    uint8_t* f1 = make_dds(&dxt, 2, dxt.nm, 0, &n1);
    uint8_t* f2 = make_dds(&argb, 4, 1, 0, &n2);
    uint8_t* f3 = make_dds(&argbm, 3, argbm.nm, 0, &n3);
    char nm[160];
    dds_name(nm, sizeof nm, &dxt, 2, "menus"); write_file(nm, f1, n1);          /* subfolder */
    dds_name(nm, sizeof nm, &argb, 4, NULL); write_file(nm, f2, n2);
    dds_name(nm, sizeof nm, &argbm, 3, NULL); write_file(nm, f3, n3);

    /* off */
    hd_reset(NULL);
    CHECK(!look(&dxt) && s_hd_on == 0, "RSX_HD_UI unset: no replacement");
    hd_reset("0");
    CHECK(!look(&dxt) && s_hd_on == 0, "RSX_HD_UI=0: no replacement");

    /* on */
    hd_reset(g_dir);
    HdEntry* e = look(&dxt);
    CHECK(s_hd_n == 3, "3 replacements indexed, got %u", s_hd_n);
    CHECK(e && e->dds.scale == 2 && e->dds.levels == dxt.nm, "DXT5 2x found");
    if (e) CHECK(staged_matches(e, &dxt, f1, NULL, NULL, NULL, NULL, NULL), "DXT5 levels staged as in the file");
    e = look(&argb);
    CHECK(e && e->dds.scale == 4 && e->dds.bgra, "A8R8G8B8 4x found");
    if (e) CHECK(staged_matches(e, &argb, f2, NULL, NULL, NULL, NULL, NULL), "A8R8G8B8 staged as R,G,B,A");
    e = look(&argbm);
    CHECK(e && e->dds.scale == 3, "swizzled A8R8G8B8 3x with mips found");
    CHECK(s_hd_replaced == 2, "replacement counter %u", s_hd_replaced);

    /* never replaced */
    { GTex u = dxt; u.gcm |= 0x40; CHECK(!hd_lookup(u.p, u.gcm, u.w, u.h, u.mips, 0, 0, u.nm, u.row), "UN never replaced"); }
    CHECK(!hd_lookup(dxt.p, dxt.gcm, dxt.w, dxt.h, dxt.mips, 0, 1, dxt.nm, dxt.row), "cube never replaced");
    CHECK(!hd_lookup(dxt.p, dxt.gcm, dxt.w, dxt.h, dxt.mips, 0, 0, dxt.nm - 1, dxt.row), "level count differing from the key's: no");
    { GTex c = dxt; c.p = (uint8_t*)malloc(c.n); memcpy(c.p, dxt.p, c.n); c.p[100] ^= 1;
      CHECK(!look(&c), "changed guest bytes: other key, no replacement"); free(c.p); }
    { GTex s4 = make_tex(0x86, 4, 4, 1, 0); CHECK(!look(&s4), "4x4 never replaced"); free(s4.p); }

    /* fallbacks: each a separate texture whose file is bad */
    struct { const char* what; u32 scale_num, scale_den; u32 levels_minus; u32 fmt; size_t cut; int junk; } bad[] = {
        { "not a DDS", 2, 1, 0, 0, 0, 1 },
        { "1.5x", 3, 2, 0, 0, 0, 0 },
        { "16x", 16, 1, 0, 0, 0, 0 },
        { "wrong format (DXT1 for DXT5)", 2, 1, 0, 0x86, 0, 0 },
        { "missing levels", 2, 1, 2, 0, 0, 0 },
        { "truncated", 2, 1, 0, 0, 100, 0 },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        GTex b = make_tex(0x88, 64, 64, 5, 0);
        size_t n = 0;
        uint8_t* f;
        if (bad[i].scale_den == 2) {
            uint32_t hdr[32]; n = 128 + 96 * 96 * 2; f = (uint8_t*)calloc(1, n);
            r1hd_dds_header(hdr, 96, 96, 0x88, b.nm); memcpy(f, hdr, 128);
        } else {
            GTex g = b; g.nm = b.nm - bad[i].levels_minus;
            f = make_dds(&g, bad[i].scale_num, g.nm, bad[i].fmt, &n);
        }
        if (bad[i].junk) memcpy(f, "PNG\0", 4);
        n -= bad[i].cut;
        dds_name(nm, sizeof nm, &b, 2, "bad"); write_file(nm, f, n);
        hd_reset(g_dir);
        CHECK(!look(&b), "%s: the game's texture is used", bad[i].what);
        HdEntry* be = hd_find(b.key);
        CHECK(be && be->state == -1 && !be->file, "%s: marked unusable", bad[i].what);
        CHECK(!look(&b), "%s: still the game's texture", bad[i].what);
        char full[MAX_PATH]; snprintf(full, sizeof full, "%s\\%s", g_dir, nm); DeleteFileA(full);
        free(f); free(b.p);
    }

    /* folder scan: '.' folders (the tool's scratch) and PNG files are not replacements */
    {
        GTex h = make_tex(0x87, 64, 32, 1, 0);
        size_t n; uint8_t* f = make_dds(&h, 2, h.nm, 0, &n);
        dds_name(nm, sizeof nm, &h, 2, ".work"); write_file(nm, f, n);
        char png[160]; snprintf(png, sizeof png, "%016llx_x.png", (unsigned long long)h.key); write_file(png, f, n);
        hd_reset(g_dir);
        CHECK(!look(&h) && !hd_find(h.key), "scratch folder and PNG ignored");
        dds_name(nm, sizeof nm, &h, 2, "a\\b\\c"); write_file(nm, f, n);
        hd_reset(g_dir);
        CHECK(look(&h) != NULL, "nested folder scanned (DXT3 2x)");
        free(f); free(h.p);
    }

    /* memory budget: dropped after the upload, read again next time */
    hd_reset(g_dir);
    hd_init();
    s_hd_budget = 1;
    e = look(&dxt);
    CHECK(e && e->state == 1 && e->file, "loaded");
    hd_after_upload(e);
    CHECK(e && e->state == 0 && !e->file && s_hd_cache == 0, "over budget: dropped");
    e = look(&dxt);
    CHECK(e && e->state == 1 && e->dds.scale == 2, "read again");

    /* D3D12 on WARP */
    {
        const GTex* ts[3] = { &dxt, &argb, &argbm };
        const uint8_t* fs[3] = { f1, f2, f3 };
        hd_reset(g_dir);
        for (int i = 0; i < 3; i++) {
            HdEntry* w = look(ts[i]);
            if (!w) { CHECK(0, "replacement %d for WARP", i); continue; }
            const int r = warp_check(ts[i], w, fs[i]);
            if (r < 0) { printf("SKIP WARP: no D3D12 WARP device\n"); break; }
            CHECK(r == 1, "WARP: %ux%u %s at %ux reads back as the file", ts[i]->w, ts[i]->h, r1hd_fmt_name(ts[i]->gcm), w->dds.scale);
        }
    }
    if (warp_stage_test() < 0) printf("SKIP WARP staging: no D3D12 WARP device\n");

    hd_reset(NULL);
    free(f1); free(f2); free(f3); free(dxt.p); free(argb.p); free(argbm.p);
    {   /* remove the test pack */
        char cmd[MAX_PATH + 32]; snprintf(cmd, sizeof cmd, "rmdir /s /q \"%s\"", g_dir); system(cmd);
    }
    printf("%s: %d/%d checks\n", g_fail ? "FAIL" : "PASS", g_checks - g_fail, g_checks);
    return g_fail ? 1 : 0;
}
