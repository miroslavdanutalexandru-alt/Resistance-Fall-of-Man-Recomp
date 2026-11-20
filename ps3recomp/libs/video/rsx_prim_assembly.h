/*
 * rsx_prim_assembly.h -- primitive assembly across DRAW batches.
 *
 * The RSX splits one primitive stream into DRAW_ARRAYS / DRAW_INDEX_ARRAY
 * words of at most 256 vertices, all inside one SET_BEGIN_END. When a batch
 * starts where the previous one ended, the hardware keeps assembling the SAME
 * strip / fan / quad list (RPCS3 merges such contiguous ranges into one draw:
 * NV47/FW draw_call.cpp, draw_clause::append). Assembling every batch on its
 * own drops the two triangles that straddle each strip seam (small triangular
 * holes in terrain drawn as long indexed strips), re-centres a fan on the
 * batch's first vertex, and puts quads out of phase.
 *
 * Pure helpers, no D3D: unit-tested in tests/test_prim_assembly.c.
 */
#ifndef RSX_PRIM_ASSEMBLY_H
#define RSX_PRIM_ASSEMBLY_H

#include <stdint.h>

/* Stream key: SET_BEGIN_END generation plus the in-BEGIN/END primitive-break
 * counter (rsx_state.prim_break). A vertex-data-base or transform-constant
 * write inside BEGIN/END closes the range on hardware (RPCS3 execution
 * barrier): the next batch starts a fresh primitive and must not join the
 * carried indices, which would be re-read under the new base / constants. */
static inline uint64_t rsx_prim_stream_key(uint32_t begin_epoch, uint32_t prim_break)
{
    return ((uint64_t)begin_epoch << 32) | prim_break;
}

typedef struct rsx_strip_carry {
    uint64_t epoch;              /* rsx_prim_stream_key of the last batch         */
    uint32_t prim, end;          /* the batch that ran last                       */
    int      open;
    uint32_t seg;                /* index position within the current strip/fan  */
    uint32_t a, b, c0;           /* last two indices, fan centre                  */
} rsx_strip_carry;

/* Call once per batch before rsx_strip_assemble. Returns 1 when the batch
 * continues the previous one (same BEGIN/END, same primitive, first == the
 * previous batch's end), keeping the strip state; otherwise starts afresh. */
static inline int rsx_strip_carry_begin(rsx_strip_carry* c, uint64_t epoch, uint32_t prim,
                                        uint32_t first, uint32_t count, int enable)
{
    const int cont = enable && c->open && c->epoch == epoch && c->prim == prim && c->end == first;
    if (!cont) { c->seg = 0; c->a = c->b = c->c0 = 0; }
    c->open = 1; c->epoch = epoch; c->prim = prim; c->end = first + count;
    return cont;
}

/* Expand `count` strip (fan = 0) or fan (fan = 1) indices into a triangle
 * list in tl[] (at most cap entries). src == NULL means the non-indexed
 * sequence first, first + 1, ... restart: 1 = an index equal to rv cuts the
 * strip. Returns the number of list indices written. Winding alternates per
 * strip triangle so every triangle keeps the strip's facing. */
static inline uint32_t rsx_strip_assemble(rsx_strip_carry* c, int fan, const uint32_t* src,
                                          uint32_t first, uint32_t count, int restart,
                                          uint32_t rv, uint32_t* tl, uint32_t cap)
{
    uint32_t n = 0, seg = c->seg, a = c->a, b = c->b, c0 = c->c0;
    for (uint32_t k = 0; k < count; k++) {
        const uint32_t ix = src ? src[k] : first + k;
        if (restart && ix == rv) { seg = 0; continue; }
        if (n + 3 <= cap) {
            if (fan) {
                if (seg >= 2) { tl[n++] = c0; tl[n++] = b; tl[n++] = ix; }
            } else if (seg >= 2) {
                if (seg & 1) { tl[n++] = b; tl[n++] = a; tl[n++] = ix; }
                else         { tl[n++] = a; tl[n++] = b; tl[n++] = ix; }
            }
        }
        if (fan && seg == 0) c0 = ix;
        a = b; b = ix;
        seg++;
    }
    c->seg = seg; c->a = a; c->b = b; c->c0 = c0;
    return n;
}

/* QUAD_STRIP (prim 9): vertices v0 v1 v2 v3 ... make quads (v2i, v2i+1,
 * v2i+3, v2i+2) -- the triangle-strip order, but a quad is only drawn once its
 * fourth vertex arrives (a trailing odd vertex draws nothing). Each quad emits
 * the same two triangles a strip would, so the winding matches TRIANGLE_STRIP.
 * Shares rsx_strip_carry with the strips; c0 holds the vertex two back. */
static inline uint32_t rsx_quadstrip_assemble(rsx_strip_carry* c, const uint32_t* src,
                                              uint32_t first, uint32_t count, int restart,
                                              uint32_t rv, uint32_t* tl, uint32_t cap)
{
    uint32_t n = 0, seg = c->seg, p = c->c0, a = c->a, b = c->b;
    for (uint32_t k = 0; k < count; k++) {
        const uint32_t ix = src ? src[k] : first + k;
        if (restart && ix == rv) { seg = 0; continue; }
        if (seg >= 3 && (seg & 1) && n + 6 <= cap) {
            tl[n++] = p; tl[n++] = a; tl[n++] = b;
            tl[n++] = b; tl[n++] = a; tl[n++] = ix;
        }
        p = a; a = b; b = ix;
        seg++;
    }
    c->seg = seg; c->c0 = p; c->a = a; c->b = b;
    return n;
}

/* Disjoint lists (quads): how many vertices of an unfinished primitive the
 * previous batch of this stream left behind. The batch is then re-read from
 * first - P so it starts on a primitive boundary (the previous batch dropped
 * that partial primitive). */
typedef struct rsx_list_carry {
    uint64_t epoch;
    uint32_t prim, end, total;
    int      open;
} rsx_list_carry;

static inline uint32_t rsx_list_phase(rsx_list_carry* c, uint64_t epoch, uint32_t prim,
                                      uint32_t first, uint32_t count, uint32_t unit, int enable)
{
    uint32_t P = 0;
    if (c->open && c->epoch == epoch && c->prim == prim && c->end == first) P = c->total % unit;
    else c->total = 0;
    c->open = 1; c->epoch = epoch; c->prim = prim; c->end = first + count; c->total += count;
    if (!enable || first < P) P = 0;
    return P;
}

#endif /* RSX_PRIM_ASSEMBLY_H */
