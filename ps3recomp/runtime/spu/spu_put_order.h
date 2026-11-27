/*
 * ps3recomp - SPU PUT store order into RSX-visible memory (GCM_PUT_ORDER).
 *
 * Adapted from GoW-Ascension-Recomp (bc160201983, MIT, commit 1be0537,
 * ps3kit/runtime/spu/spu_channels.c spu_dma_copy_rsx / spu_dma_head_is_park).
 * Notice in THIRD_PARTY.md.
 *
 * SPU jobs that fill a hole in the RSX command stream PUT the whole hole at
 * once, with a "park" (a JUMP to its own IO address) at its head. The FIFO
 * reader (our walker in cellGcmSys.c, or RPCS3's FIFO_control in R2/R3) stays
 * on the park until that word changes and then reads on. A plain memcpy writes
 * the head first, so a copying host thread preempted half way (six SPU threads
 * on a 4-core PC) lets the reader walk into the half that is not written yet:
 * the previous lap's words decoded as commands -> desync / freeze. Writing the
 * rest of the PUT first and the head's 16 bytes last (park half last) keeps
 * the reader parked until the whole block is in memory. GoW measured 3 freezes
 * in 17 runs -> 0 in 41. Cost: one table lookup + compare per PUT.
 *
 * GCM_PUT_ORDER=2 (default): park-head PUTs written head-last.
 * GCM_PUT_ORDER=0: plain memcpy (the old behaviour).
 */
#ifndef SPU_PUT_ORDER_H
#define SPU_PUT_ORDER_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if defined(__x86_64__) || defined(_M_X64)
#include <emmintrin.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* EA 1 MB page -> IO 1 MB page (0xFFFF = unmapped). cellGcmSys.c points it at
 * its s_io_address_table at cellGcmInit; NULL = no ordering (no GCM yet). */
#if defined(_MSC_VER)
__declspec(selectany) const uint16_t* volatile g_gcm_ea_io_tab = 0;
__declspec(selectany) volatile int g_spu_put_order = -1;
__declspec(selectany) volatile unsigned long long g_spu_put_park_n = 0;
#else
__attribute__((weak)) const uint16_t* volatile g_gcm_ea_io_tab = 0;
__attribute__((weak)) volatile int g_spu_put_order = -1;
__attribute__((weak)) volatile unsigned long long g_spu_put_park_n = 0;
#endif

/* Pure: does the destination's current first word park the RSX on itself?
 * Both JUMP encodings: old (0x20000000 | offset) and new (offset | 1). */
static inline int spu_put_head_is_park(const uint8_t* dst, uint32_t ea, const uint16_t* tab)
{
    if (!tab) return 0;
    const uint32_t pg = tab[ea >> 20];
    if (pg == 0xFFFFu) return 0;
    const uint32_t io = (pg << 20) | (ea & 0xFFFFFu);
    const uint32_t w = (uint32_t)dst[0] << 24 | (uint32_t)dst[1] << 16 | (uint32_t)dst[2] << 8 | dst[3];
    return w == (0x20000000u | (io & 0x1FFFFFFCu)) || w == ((io & 0xFFFFFFFCu) | 1u);
}

/* Pure: the PUT shapes the park-last copy handles (whole 16-byte rows). */
static inline int spu_put_order_eligible(const void* dst, uint32_t n)
{
    return n > 16u && !(n & 15u) && !((uintptr_t)dst & 15u);
}

/* Everything after the first 16 bytes, then bytes 8..15, then 0..7 (the park). */
static inline void spu_put_copy_head_last(uint8_t* d, const uint8_t* s, uint32_t n)
{
    memcpy(d + 16, s + 16, n - 16u);
#if defined(__x86_64__) || defined(_M_X64)
    _mm_sfence();   /* a fast-string memcpy may use weakly ordered stores */
#endif
    uint64_t hi, lo;
    memcpy(&hi, s + 8, 8);
    memcpy(&lo, s, 8);
    __atomic_store_n((uint64_t*)(void*)(d + 8), hi, __ATOMIC_RELEASE);
    __atomic_store_n((uint64_t*)(void*)d, lo, __ATOMIC_RELEASE);
}

static inline int spu_put_order_mode(void)
{
    int m = g_spu_put_order;
    if (__builtin_expect(m < 0, 0)) {
        const char* e = getenv("GCM_PUT_ORDER");
        m = (e && e[0] == '0') ? 0 : 2;
        g_spu_put_order = m;
    }
    return m;
}

/* The SPU PUT copy: memcpy, except a park-head PUT under GCM_PUT_ORDER=2. */
static inline void spu_put_copy(uint8_t* d, const uint8_t* s, uint32_t n, uint32_t ea)
{
    const uint16_t* tab = g_gcm_ea_io_tab;
    if (tab && spu_put_order_mode() == 2 && spu_put_order_eligible(d, n) &&
        spu_put_head_is_park(d, ea, tab)) {
        spu_put_copy_head_last(d, s, n);
        __atomic_add_fetch(&g_spu_put_park_n, 1ull, __ATOMIC_RELAXED);
        return;
    }
    memcpy(d, s, n);
}

#ifdef __cplusplus
}
#endif

#endif /* SPU_PUT_ORDER_H */
