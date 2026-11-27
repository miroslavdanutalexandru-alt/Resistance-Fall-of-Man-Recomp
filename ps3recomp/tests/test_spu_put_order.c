/* GCM_PUT_ORDER (runtime/spu/spu_put_order.h): park detection and the
 * park-last copy of SPU PUTs into RSX-visible memory. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "spu/spu_put_order.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint16_t tab[4096];
static _Alignas(16) uint8_t dst[256], src[256];

static void put_be32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }

int main(void)
{
    memset(tab, 0xFF, sizeof(tab));
    tab[0x300] = 0x012;                  /* EA 0x30000000 -> IO 0x01200000 */
    const uint32_t ea = 0x30000140u, io = 0x01200140u;

    /* old-style JUMP to itself */
    put_be32(dst, 0x20000000u | io);
    CHECK(spu_put_head_is_park(dst, ea, tab));
    /* new-style JUMP (offset | 1) to itself */
    put_be32(dst, io | 1u);
    CHECK(spu_put_head_is_park(dst, ea, tab));
    /* a JUMP somewhere else, a method header, an unmapped page, no table */
    put_be32(dst, 0x20000000u | (io + 0x40u));
    CHECK(!spu_put_head_is_park(dst, ea, tab));
    put_be32(dst, 0x00041E00u);
    CHECK(!spu_put_head_is_park(dst, ea, tab));
    put_be32(dst, 0x20000000u | io);
    CHECK(!spu_put_head_is_park(dst, 0x40000140u, tab));
    CHECK(!spu_put_head_is_park(dst, ea, NULL));

    /* eligibility: whole 16-byte rows, aligned, more than one row */
    CHECK(spu_put_order_eligible(dst, 32));
    CHECK(!spu_put_order_eligible(dst, 16));
    CHECK(!spu_put_order_eligible(dst, 40));
    CHECK(!spu_put_order_eligible(dst + 4, 32));

    /* default mode reorders a park-head PUT; the bytes land the same */
    for (int i = 0; i < 256; i++) src[i] = (uint8_t)(i * 7 + 1);
    memset(dst, 0, sizeof(dst));
    put_be32(dst, 0x20000000u | io);
    g_gcm_ea_io_tab = tab;
    g_spu_put_order = -1;
    const unsigned long long n0 = g_spu_put_park_n;
    spu_put_copy(dst, src, 256, ea);
    CHECK(!memcmp(dst, src, 256));
    CHECK(g_spu_put_park_n == n0 + 1);
    CHECK(g_spu_put_order == 2);

    /* a PUT whose head is not a park is a plain copy */
    memset(dst, 0, sizeof(dst));
    spu_put_copy(dst, src, 256, ea);
    CHECK(!memcmp(dst, src, 256));
    CHECK(g_spu_put_park_n == n0 + 1);

    /* GCM_PUT_ORDER=0: never reordered */
    g_spu_put_order = 0;
    memset(dst, 0, sizeof(dst));
    put_be32(dst, 0x20000000u | io);
    spu_put_copy(dst, src, 256, ea);
    CHECK(!memcmp(dst, src, 256));
    CHECK(g_spu_put_park_n == n0 + 1);

    /* head-last copy of the smallest eligible PUT */
    memset(dst, 0, sizeof(dst));
    spu_put_copy_head_last(dst, src, 32);
    CHECK(!memcmp(dst, src, 32) && dst[32] == 0);

    if (fails) { fprintf(stderr, "%d failure(s)\n", fails); return 1; }
    printf("spu_put_order: all checks passed\n");
    return 0;
}
