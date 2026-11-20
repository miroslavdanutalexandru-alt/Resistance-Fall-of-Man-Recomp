/*
 * Native hot paths vs the code they replace (R1 port, RSX_NATIVE_*):
 *
 *   - rsx_texture_decode_native against rsx_texture_decode_reference for
 *     every format class, swizzled and linear, square / wide / tall images,
 *     pitched destinations (nothing written outside the image), both
 *     A8R8G8B8 orders: byte for byte.
 *   - rsx_nh_copy_wc against memcpy over lengths and misalignments.
 *
 * `test_native_hot --bench` also times both sides (cached and, on Windows,
 * write-combined destinations, which is what the D3D12 upload heap is).
 */
#include "rsx_texture_layout.h"
#include "rsx_native_hot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static uint32_t rng = 0x12345678u;
static uint32_t xr(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static void fill(uint8_t* p, size_t n) { for (size_t i = 0; i < n; i++) p[i] = (uint8_t)xr(); }

static const uint32_t k_formats[] = {
    0x85, 0x9E, 0x8B, 0x8C, 0x8D, 0x82, 0x83, 0x84, 0x97, 0x9D,
    0x90, 0x92, 0x94, 0x95, 0x9A, 0x9B, 0x9C, 0x9F, 0x81, 0x86,
};

static int fails = 0, cases = 0, native_cases = 0;

static void check(uint32_t fmt, uint32_t w, uint32_t h, uint32_t pitch, int argb)
{
    rsx_tex_layout tl;
    rsx_texture_layout_pitched(fmt, w, h, pitch, &tl);
    if (tl.compressed) return;
    const size_t srcsz = (size_t)tl.face_bytes + 64;
    const uint32_t dpitch = w * tl.dst_bytes_per_texel + 32;   /* pitched dst */
    const size_t dsz = (size_t)dpitch * h + 64;
    uint8_t* src = (uint8_t*)malloc(srcsz);
    uint8_t* a = (uint8_t*)malloc(dsz);
    uint8_t* b = (uint8_t*)malloc(dsz);
    fill(src, srcsz);
    memset(a, 0xCD, dsz); memset(b, 0xCD, dsz);
    rsx_texture_decode_reference(a, dpitch, src, w, h, &tl, argb);
    const int handled = rsx_texture_decode_native(b, dpitch, src, w, h, &tl, argb);
    cases++;
    if (handled) {
        native_cases++;
        if (memcmp(a, b, dsz) != 0) {
            size_t i = 0; while (a[i] == b[i]) i++;
            if (fails < 20)
                printf("FAIL fmt=0x%02X %ux%u pitch=%u argb=%d swz=%d: first diff at byte %zu (row %zu)\n",
                       fmt, w, h, pitch, argb, tl.swizzled, i, i / dpitch);
            fails++;
        }
    } else {
        /* not handled: must not have written anything */
        uint8_t* z = (uint8_t*)malloc(dsz); memset(z, 0xCD, dsz);
        if (memcmp(b, z, dsz) != 0) { printf("FAIL fmt=0x%02X %ux%u: unhandled but wrote\n", fmt, w, h); fails++; }
        free(z);
    }
    /* the public entry point (which verifies and may take either path) agrees too */
    memset(b, 0xCD, dsz);
    rsx_texture_decode(b, dpitch, src, w, h, &tl, argb);
    if (memcmp(a, b, dsz) != 0) { printf("FAIL rsx_texture_decode fmt=0x%02X %ux%u\n", fmt, w, h); fails++; }
    free(src); free(a); free(b);
}

static void test_texdec(void)
{
    static const uint32_t dims[] = { 1, 2, 4, 8, 16, 64, 256, 1024, 3, 5, 12, 100 };
    const size_t nd = sizeof dims / sizeof dims[0];
    for (size_t f = 0; f < sizeof k_formats / sizeof k_formats[0]; f++)
        for (int ln = 0; ln < 2; ln++) {
            const uint32_t fmt = k_formats[f] | (ln ? 0x20u : 0u);
            for (size_t i = 0; i < nd; i++)
                for (size_t j = 0; j < nd; j++) {
                    if ((size_t)dims[i] * dims[j] > 256u * 1024u) continue;
                    for (int argb = 0; argb < 2; argb++) {
                        check(fmt, dims[i], dims[j], 0, argb);
                        if (ln) check(fmt, dims[i], dims[j], dims[i] * 16u + 48u, argb);
                    }
                }
        }
    /* the largest sizes the game binds */
    check(0x85, 2048, 2048, 0, 0);
    check(0x84, 1024, 2048, 0, 0);
    check(0x9A, 512, 512, 0, 0);
    check(0x9B, 256, 128, 0, 0);
    printf("texdec: %d cases, %d through the native path, %d failures\n", cases, native_cases, fails);
}

static void test_wccopy(void)
{
    enum { N = 70000 };
    uint8_t* s = (uint8_t*)malloc(N); uint8_t* a = (uint8_t*)malloc(N + 64); uint8_t* b = (uint8_t*)malloc(N + 64);
    fill(s, N);
    int f0 = fails, n = 0;
    for (size_t len = 0; len < 600; len += (len < 140 ? 1 : 37))
        for (int da = 0; da < 16; da++)
            for (int sa = 0; sa < 16; sa += 3) {
                memset(a, 0x5A, N + 64); memset(b, 0x5A, N + 64);
                memcpy(a + da, s + sa, len);
                rsx_nh_copy_wc(b + da, s + sa, len); rsx_nh_wc_fence();
                n++;
                if (memcmp(a, b, N + 64) != 0) { if (fails - f0 < 10) printf("FAIL wccopy len=%zu da=%d sa=%d\n", len, da, sa); fails++; }
            }
    const size_t big[] = { 4096, 7488, 9000, 65536, 69999 };
    for (size_t k = 0; k < 5; k++) {
        memset(a, 0x5A, N + 64); memset(b, 0x5A, N + 64);
        memcpy(a, s, big[k]); rsx_nh_copy_wc(b, s, big[k]); rsx_nh_wc_fence(); n++;
        if (memcmp(a, b, N + 64) != 0) { printf("FAIL wccopy len=%zu\n", big[k]); fails++; }
    }
    printf("wccopy: %d cases, %d failures, engaged=%d\n", n, fails - f0, rsx_nh_wccopy_on());
    free(s); free(a); free(b);
}

/* ---- RSX_SRV_CACHE bookkeeping: references, sweep, invalidate ----------- */
typedef struct { int ref; } MockRes;
static long mock_addrefs, mock_releases;
static void mock_addref(void* r)  { ((MockRes*)r)->ref++; mock_addrefs++; }
static void mock_release(void* r) { ((MockRes*)r)->ref--; mock_releases++; }
#define SRVC_EXPECT(c, msg) do { srvc_n++; if (!(c)) { printf("FAIL srvcache: %s (line %d)\n", msg, __LINE__); fails++; } } while (0)

static void test_srvcache(void)
{
    enum { N = 4096, AGE = 600, R = 5000 };
    int f0 = fails, srvc_n = 0;
    MockRes* res = (MockRes*)calloc(R, sizeof *res);
    for (int i = 0; i < R; i++) res[i].ref = 1;   /* the owner's reference */
    rsx_nh_srvc c = {0};
    c.e = (rsx_nh_srvc_ent*)calloc(N, sizeof *c.e); c.n = N;
    c.addref = mock_addref; c.release = mock_release;
    int hit = 0;

    /* insert, hit, a second key on the same resource */
    int s0 = rsx_nh_srvc_lookup(&c, &res[0], 28, 0x1688, 0, 10, 9, AGE, &hit);
    SRVC_EXPECT(s0 >= 0 && !hit && res[0].ref == 2 && c.live == 1, "insert holds one reference");
    int s1 = rsx_nh_srvc_lookup(&c, &res[0], 28, 0x1688, 0, 11, 10, AGE, &hit);
    SRVC_EXPECT(s1 == s0 && hit && res[0].ref == 2, "hit takes no extra reference");
    int s2 = rsx_nh_srvc_lookup(&c, &res[0], 87, 0x1688, 1, 11, 10, AGE, &hit);
    SRVC_EXPECT(s2 >= 0 && s2 != s0 && !hit && res[0].ref == 3 && c.live == 2, "second view of one resource");

    /* grave: every entry of the buried resource goes, and a new resource at
     * the same address (simulated by reusing the pointer) misses */
    SRVC_EXPECT(rsx_nh_srvc_invalidate(&c, &res[0]) == 2 && res[0].ref == 1 && c.live == 0, "invalidate drops all entries");
    SRVC_EXPECT(rsx_nh_srvc_invalidate(&c, &res[0]) == 0 && res[0].ref == 1, "invalidate twice is a no-op");
    rsx_nh_srvc_lookup(&c, &res[0], 28, 0x1688, 0, 12, 11, AGE, &hit);
    SRVC_EXPECT(!hit && res[0].ref == 2, "reused address is re-created, not a stale hit");
    SRVC_EXPECT(rsx_nh_srvc_invalidate(&c, &res[1]) == 0 && res[1].ref == 1, "invalidate of an uncached resource");

    /* sweep: age and GPU completion are both required */
    rsx_nh_srvc_clear(&c);
    SRVC_EXPECT(res[0].ref == 1 && c.live == 0, "clear releases");
    rsx_nh_srvc_lookup(&c, &res[2], 28, 0, 0, 100, 99, AGE, &hit);
    SRVC_EXPECT(rsx_nh_srvc_sweep(&c, 100 + AGE, 200, AGE) == 0 && res[2].ref == 2, "not older than AGE: kept");
    SRVC_EXPECT(rsx_nh_srvc_sweep(&c, 101 + AGE, 100, AGE) == 0 && res[2].ref == 2, "last use not complete on the GPU: kept");
    SRVC_EXPECT(rsx_nh_srvc_sweep(&c, 101 + AGE, 101, AGE) == 1 && res[2].ref == 1 && c.live == 0, "aged and complete: released");
    /* the batch counter wraps */
    rsx_nh_srvc_lookup(&c, &res[2], 28, 0, 0, 0xFFFFFF00u, 0xFFFFFEFFu, AGE, &hit);
    SRVC_EXPECT(rsx_nh_srvc_sweep(&c, 0x10u, 0x0Fu, AGE) == 0 && res[2].ref == 2, "wrap: young entry kept");
    SRVC_EXPECT(rsx_nh_srvc_sweep(&c, 0xFFFFFF00u + AGE + 1u, 0x150u, AGE) == 1 && res[2].ref == 1, "wrap: aged entry released");

    /* the review case: many textures used once and then freed outside the
     * grave must not stay pinned -- one sweep later they are all released */
    int inserted = 0;
    for (int i = 0; i < R; i++)
        if (rsx_nh_srvc_lookup(&c, &res[i], 28, 0x1688, 0, 1000 + (unsigned)i / 64u, 999 + (unsigned)i / 64u, AGE, &hit) >= 0) inserted++;
    SRVC_EXPECT(inserted > N / 2 && (int)c.live == inserted, "bulk insert");
    long held = 0; for (int i = 0; i < R; i++) held += res[i].ref - 1;
    SRVC_EXPECT(held == (long)c.live, "one reference per live entry");
    unsigned last = 1000 + (R - 1) / 64u;
    rsx_nh_srvc_sweep(&c, last + AGE + 1u, last + AGE, AGE);
    held = 0; for (int i = 0; i < R; i++) held += res[i].ref - 1;
    SRVC_EXPECT(c.live == 0 && held == 0, "sweep releases every unused entry");

    /* a full probe window: no slot until something ages out, then the insert
     * reuses one stale slot and releases exactly its reference */
    rsx_nh_srvc s = {0};
    rsx_nh_srvc_ent small[16]; memset(small, 0, sizeof small);
    s.e = small; s.n = 16; s.addref = mock_addref; s.release = mock_release;
    for (int i = 0; i < 16; i++) rsx_nh_srvc_lookup(&s, &res[100 + i], 28, 0, 0, 50, 49, AGE, &hit);
    SRVC_EXPECT(s.live == 16, "small table full");
    SRVC_EXPECT(rsx_nh_srvc_lookup(&s, &res[200], 28, 0, 0, 60, 59, AGE, &hit) == -1 && res[200].ref == 1, "full window: no slot, no reference");
    long r0 = mock_releases;
    int si = rsx_nh_srvc_lookup(&s, &res[200], 28, 0, 0, 50 + AGE + 1u, 51, AGE, &hit);
    SRVC_EXPECT(si >= 0 && !hit && res[200].ref == 2 && mock_releases == r0 + 1 && s.live == 16, "stale slot reused, its reference released");
    rsx_nh_srvc_clear(&s);
    held = 0; for (int i = 0; i < R; i++) held += res[i].ref - 1;
    SRVC_EXPECT(held == 0 && mock_addrefs == mock_releases, "references balance");

    printf("srvcache: %d checks, %d failures\n", srvc_n, fails - f0);
    free(c.e); free(res);
}

/* ---- microbenchmarks ---------------------------------------------------- */
#ifdef _WIN32
static double now_us(void)
{
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e6 / (double)f.QuadPart;
}
static uint8_t* alloc_dst(size_t n, int wc)
{
    return (uint8_t*)VirtualAlloc(NULL, n, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE | (wc ? PAGE_WRITECOMBINE : 0));
}
static void free_dst(uint8_t* p) { VirtualFree(p, 0, MEM_RELEASE); }

static void bench_tex(uint32_t fmt, uint32_t w, uint32_t h, int wc)
{
    rsx_tex_layout tl; rsx_texture_layout_pitched(fmt, w, h, 0, &tl);
    const size_t ss = tl.face_bytes, ds = (size_t)w * h * tl.dst_bytes_per_texel;
    uint8_t* src = (uint8_t*)malloc(ss); fill(src, ss);
    uint8_t* d = alloc_dst(ds, wc);
    const int reps = (int)(64u * 1024u * 1024u / ds) + 2;
    double best_r = 1e30, best_n = 1e30;
    for (int it = 0; it < 5; it++) {
        double t0 = now_us();
        for (int r = 0; r < reps; r++) rsx_texture_decode_reference(d, w * tl.dst_bytes_per_texel, src, w, h, &tl, 0);
        double t1 = now_us();
        for (int r = 0; r < reps; r++) rsx_texture_decode_native(d, w * tl.dst_bytes_per_texel, src, w, h, &tl, 0);
        double t2 = now_us();
        if ((t1 - t0) / reps < best_r) best_r = (t1 - t0) / reps;
        if ((t2 - t1) / reps < best_n) best_n = (t2 - t1) / reps;
    }
    printf("  texdec fmt=0x%02X %4ux%-4u %s %s: reference %8.1f us, native %8.1f us  (x%.1f)\n",
           fmt, w, h, tl.swizzled ? "swz" : "lin", wc ? "WC " : "WB ", best_r, best_n, best_r / best_n);
    free(src); free_dst(d);
}

static void bench_copy(size_t len, int wc)
{
    const size_t span = 64u << 20;      /* walk a big ring like the upload heap */
    uint8_t* src = (uint8_t*)malloc(len + 64); fill(src, len + 64);
    uint8_t* d = alloc_dst(span, wc);
    const size_t n = span / ((len + 31) & ~(size_t)15);
    double best_m = 1e30, best_s = 1e30, best_f = 1e30;
    for (int it = 0; it < 7; it++) {
        size_t o = 0;
        double t0 = now_us();
        for (size_t i = 0; i < n; i++) { memcpy(d + o, src, len); o += (len + 31) & ~(size_t)15; }
        double t1 = now_us();
        o = 0;
        for (size_t i = 0; i < n; i++) { rsx_nh_copy_wc(d + o, src, len); o += (len + 31) & ~(size_t)15; }
        rsx_nh_wc_fence();
        double t2 = now_us();
        o = 0;
        for (size_t i = 0; i < n; i++) { rsx_nh_copy_wc(d + o, src, len); rsx_nh_wc_fence(); o += (len + 31) & ~(size_t)15; }
        double t3 = now_us();
        if ((t1 - t0) < best_m) best_m = t1 - t0;
        if ((t2 - t1) < best_s) best_s = t2 - t1;
        if ((t3 - t2) < best_f) best_f = t3 - t2;
    }
    const double gb = (double)n * len / 1e3;   /* bytes per us = MB/s; /1e3 -> GB/s */
    printf("  copy %6zu B %s: memcpy %6.2f GB/s (%.3f us/copy), stream %6.2f GB/s (%.3f us/copy)  (x%.2f), stream+sfence each %.3f us/copy\n",
           len, wc ? "WC " : "WB ", gb / best_m, best_m / n, gb / best_s, best_s / n, best_m / best_s, best_f / n);
    free(src); free_dst(d);
}

static void bench(void)
{
    printf("bench (best of 5-7):\n");
    for (int wc = 0; wc < 2; wc++) {
        bench_tex(0x85, 512, 512, wc);
        bench_tex(0x85, 1024, 1024, wc);
        bench_tex(0x85, 128, 128, wc);
        bench_tex(0xA5, 512, 512, wc);
        bench_tex(0x84, 512, 512, wc);
        bench_tex(0x83, 256, 256, wc);
        bench_tex(0x9A, 256, 256, wc);
        bench_tex(0x8B, 512, 512, wc);
    }
    for (int wc = 0; wc < 2; wc++) {
        bench_copy(1024, wc); bench_copy(4096, wc); bench_copy(7488, wc); bench_copy(65536, wc);
    }
}
#else
static void bench(void) { printf("bench: Windows only\n"); }
#endif

int main(int argc, char** argv)
{
    test_texdec();
    test_wccopy();
    test_srvcache();
    if (argc > 1 && strcmp(argv[1], "--bench") == 0) bench();
    printf(fails ? "native_hot: FAILED (%d)\n" : "native_hot: all byte-exact\n", fails);
    return fails ? 1 : 0;
}
