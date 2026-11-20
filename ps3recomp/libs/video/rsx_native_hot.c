/*
 * ps3recomp - native host replacements for render-side hot loops (R1 port).
 * See rsx_native_hot.h.
 */
#include "rsx_native_hot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../../../project/port_native_summary.h"   /* port_native_parse_bool: the shared switch rules */

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#define NH_SSE2 1
#else
#define NH_SSE2 0
#endif

void rsx_nh_copy_wc(void* dst, const void* src, size_t len)
{
#if NH_SSE2
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (len < 128) { memcpy(d, s, len); return; }
    /* Head up to a 16-byte boundary with ordinary stores: the upload heap
     * callers pass aligned destinations, so this is normally empty. */
    const size_t head = (size_t)(-(intptr_t)d) & 15u;
    if (head) { memcpy(d, s, head); d += head; s += head; len -= head; }
    size_t i = 0;
    for (; i + 64 <= len; i += 64) {
        const __m128i v0 = _mm_loadu_si128((const __m128i*)(s + i));
        const __m128i v1 = _mm_loadu_si128((const __m128i*)(s + i + 16));
        const __m128i v2 = _mm_loadu_si128((const __m128i*)(s + i + 32));
        const __m128i v3 = _mm_loadu_si128((const __m128i*)(s + i + 48));
        _mm_stream_si128((__m128i*)(d + i), v0);
        _mm_stream_si128((__m128i*)(d + i + 16), v1);
        _mm_stream_si128((__m128i*)(d + i + 32), v2);
        _mm_stream_si128((__m128i*)(d + i + 48), v3);
    }
    for (; i + 16 <= len; i += 16)
        _mm_stream_si128((__m128i*)(d + i), _mm_loadu_si128((const __m128i*)(s + i)));
    if (i < len) memcpy(d + i, s + i, len - i);
#else
    memcpy(dst, src, len);
#endif
}

void rsx_nh_wc_fence(void)
{
#if NH_SSE2
    _mm_sfence();
#endif
}

/* One pass over lengths and misalignments that cover the head, the 64-byte
 * body, the 16-byte steps and the tail, in ordinary (cached) memory. */
static int nh_wccopy_selftest(void)
{
    enum { N = 4096 + 96 };
    uint8_t* src = (uint8_t*)malloc(N);
    uint8_t* a = (uint8_t*)malloc(N + 64);
    uint8_t* b = (uint8_t*)malloc(N + 64);
    int ok = src && a && b;
    if (ok) {
        uint32_t x = 0x9E3779B9u;
        for (int i = 0; i < N; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; src[i] = (uint8_t)x; }
        static const size_t lens[] = { 0, 1, 15, 16, 63, 64, 127, 128, 129, 200, 1000, 4096, 4096 + 33 };
        for (size_t li = 0; ok && li < sizeof lens / sizeof lens[0]; li++)
            for (int da = 0; ok && da < 16; da += 5)
                for (int sa = 0; ok && sa < 16; sa += 7) {
                    memset(a, 0xA5, N + 64); memset(b, 0xA5, N + 64);
                    memcpy(a + da, src + sa, lens[li]);
                    rsx_nh_copy_wc(b + da, src + sa, lens[li]);
                    rsx_nh_wc_fence();
                    if (memcmp(a, b, N + 64) != 0) ok = 0;
                }
    }
    free(src); free(a); free(b);
    return ok;
}

int rsx_nh_wccopy_on(void)
{
    static volatile int on = -1;
    int v = on;
    if (v >= 0) return v;
    const char* e = getenv("RSX_NATIVE_WCCOPY");
    v = port_native_parse_bool(e, 1);
    if (v && !NH_SSE2) v = 0;
    if (v) {
        if (nh_wccopy_selftest())
            fprintf(stderr, "[native-hot] wccopy: streaming-store copy into the upload heap engaged"
                            " (vertex ranges + vertex constants; RSX_NATIVE_WCCOPY=0 = memcpy)%c", 10);
        else {
            fprintf(stderr, "[native-hot] wccopy: self-test MISMATCH against memcpy -- using memcpy%c", 10);
            v = 0;
        }
    }
    on = v;
    return v;
}

/* ---- SRV descriptor cache bookkeeping (RSX_SRV_CACHE) ------------------ */

static unsigned srvc_hash(void* res, unsigned fmt, unsigned mapping, int cube)
{
    uint64_t k = (uint64_t)(uintptr_t)res * 0x9E3779B97F4A7C15ull
               ^ ((uint64_t)fmt << 40) ^ ((uint64_t)mapping << 8) ^ (uint64_t)(unsigned)cube;
    k ^= k >> 29;
    return (unsigned)k;
}
static void srvc_drop(rsx_nh_srvc* c, rsx_nh_srvc_ent* e)
{
    if (!e->res) return;
    c->release(e->res);
    memset(e, 0, sizeof *e);
    c->live--;
}
/* Unused for more than `age` batches and that last use is complete on the GPU. */
static int srvc_stale(const rsx_nh_srvc_ent* e, unsigned now, unsigned done, unsigned age)
{
    return now - e->last > age && (int)(done - e->last) > 0;   /* wrap-safe */
}

int rsx_nh_srvc_lookup(rsx_nh_srvc* c, void* res, unsigned fmt, unsigned mapping, int cube,
                       unsigned now, unsigned done, unsigned age, int* hit)
{
    *hit = 0;
    if (!c->e || !res) return -1;
    rsx_nh_srvc_ent* victim = NULL;
    unsigned vi = 0, m = c->n - 1u;
    for (unsigned n = 0, i = srvc_hash(res, fmt, mapping, cube) & m; n < 16u && n < c->n; n++, i = (i + 1u) & m) {
        rsx_nh_srvc_ent* e = &c->e[i];
        if (e->res == res && e->fmt == fmt && e->mapping == mapping && e->cube == cube) {
            e->last = now; *hit = 1;
            return (int)i;
        }
        if (!victim && (!e->res || srvc_stale(e, now, done, age))) { victim = e; vi = i; }
    }
    if (!victim) return -1;
    srvc_drop(c, victim);
    c->addref(res);
    victim->res = res; victim->fmt = fmt; victim->mapping = mapping; victim->cube = cube;
    victim->last = now;
    c->live++;
    return (int)vi;
}

unsigned rsx_nh_srvc_sweep(rsx_nh_srvc* c, unsigned now, unsigned done, unsigned age)
{
    unsigned k = 0;
    if (!c->e || !c->live) return 0;
    for (unsigned i = 0; i < c->n; i++)
        if (c->e[i].res && srvc_stale(&c->e[i], now, done, age)) { srvc_drop(c, &c->e[i]); k++; }
    return k;
}

unsigned rsx_nh_srvc_invalidate(rsx_nh_srvc* c, void* res)
{
    unsigned k = 0;
    if (!c->e || !c->live || !res) return 0;
    for (unsigned i = 0; i < c->n; i++)
        if (c->e[i].res == res) { srvc_drop(c, &c->e[i]); k++; }
    return k;
}

void rsx_nh_srvc_clear(rsx_nh_srvc* c)
{
    if (!c->e) return;
    for (unsigned i = 0; i < c->n; i++) srvc_drop(c, &c->e[i]);
    c->live = 0;
}
