/* rsx_prim_assembly.h: a strip / fan / quad list split over several DRAW
 * batches inside one BEGIN/END must assemble exactly like the unsplit stream. */
#include "rsx_prim_assembly.h"
#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

/* Assemble src[0..total) once whole, once as batches of `step`; both lists must match. */
static void split_matches_whole(int fan, const uint32_t* src, uint32_t total, uint32_t step,
                                int restart, uint32_t rv)
{
    uint32_t whole[1024], split[1024], nw, ns = 0;
    rsx_strip_carry c; memset(&c, 0, sizeof c);
    rsx_strip_carry_begin(&c, 7, 6, 0, total, 1);
    nw = rsx_strip_assemble(&c, fan, src, 0, total, restart, rv, whole, 1024);
    memset(&c, 0, sizeof c);
    for (uint32_t f = 0; f < total; f += step) {
        const uint32_t n = (total - f < step) ? total - f : step;
        const int cont = rsx_strip_carry_begin(&c, 7, 6, f, n, 1);
        CHECK(cont == (f != 0));
        ns += rsx_strip_assemble(&c, fan, src ? src + f : NULL, f, n, restart, rv, split + ns, 1024 - ns);
    }
    CHECK(nw == ns);
    CHECK(memcmp(whole, split, nw * sizeof(uint32_t)) == 0);
}

int main(void)
{
    uint32_t seq[300];
    for (uint32_t i = 0; i < 300; i++) seq[i] = 1000 + i;

    /* Strip of 300 split 256 + 44: 298 triangles, none lost at the seam. */
    split_matches_whole(0, seq, 300, 256, 0, 0);
    { rsx_strip_carry c; memset(&c, 0, sizeof c); uint32_t tl[1024], n = 0;
      rsx_strip_carry_begin(&c, 1, 6, 0, 256, 1); n += rsx_strip_assemble(&c, 0, seq, 0, 256, 0, 0, tl, 1024);
      rsx_strip_carry_begin(&c, 1, 6, 256, 44, 1); n += rsx_strip_assemble(&c, 0, seq + 256, 256, 44, 0, 0, tl + n, 1024 - n);
      CHECK(n == 298 * 3);
      /* the seam triangles 254 (even) and 255 (odd, swapped) */
      CHECK(tl[254*3] == 1254 && tl[254*3+1] == 1255 && tl[254*3+2] == 1256);
      CHECK(tl[255*3] == 1256 && tl[255*3+1] == 1255 && tl[255*3+2] == 1257); }

    /* Odd-sized batches flip the parity mid-stream; winding must still match. */
    split_matches_whole(0, seq, 300, 7, 0, 0);
    /* Non-indexed (src NULL) strips and fans. */
    split_matches_whole(0, NULL, 300, 256, 0, 0);
    split_matches_whole(1, NULL, 300, 256, 0, 0);
    /* Fan: every triangle keeps the stream's first vertex as centre. */
    split_matches_whole(1, seq, 300, 5, 0, 0);

    /* Restart cuts landing on and around the batch seam. */
    { uint32_t r[300];
      for (uint32_t i = 0; i < 300; i++) r[i] = (i % 37 == 36 || i == 255 || i == 256) ? 0xFFFFu : i;
      split_matches_whole(0, r, 300, 256, 1, 0xFFFFu);
      split_matches_whole(0, r, 300, 3, 1, 0xFFFFu); }

    /* A batch that does not continue (new BEGIN/END, gap, other prim) restarts. */
    { rsx_strip_carry c; memset(&c, 0, sizeof c); uint32_t tl[64];
      rsx_strip_carry_begin(&c, 1, 6, 0, 4, 1); rsx_strip_assemble(&c, 0, NULL, 0, 4, 0, 0, tl, 64);
      CHECK(rsx_strip_carry_begin(&c, 2, 6, 4, 4, 1) == 0);
      CHECK(rsx_strip_assemble(&c, 0, NULL, 4, 4, 0, 0, tl, 64) == 6);
      CHECK(rsx_strip_carry_begin(&c, 2, 6, 9, 4, 1) == 0);   /* gap */
      CHECK(rsx_strip_carry_begin(&c, 2, 7, 13, 4, 1) == 0);  /* other primitive */
      CHECK(rsx_strip_carry_begin(&c, 2, 7, 17, 4, 0) == 0);  /* disabled */
    }

    /* Quads: 6 + 6 + 4 vertices = 4 quads; batches 2 and 3 re-read 2 back. */
    { rsx_list_carry q; memset(&q, 0, sizeof q);
      CHECK(rsx_list_phase(&q, 3, 8, 0, 6, 4, 1) == 0);
      CHECK(rsx_list_phase(&q, 3, 8, 6, 6, 4, 1) == 2);
      CHECK(rsx_list_phase(&q, 3, 8, 12, 4, 4, 1) == 0);
      CHECK(rsx_list_phase(&q, 4, 8, 16, 6, 4, 1) == 0);   /* new BEGIN/END */
      CHECK(rsx_list_phase(&q, 4, 8, 22, 4, 4, 0) == 0); } /* disabled */

    /* A mid-BEGIN/END barrier (vertex base / transform constant write bumps
     * rsx_state.prim_break) ends the stream: same epoch, contiguous range,
     * yet the next batch starts afresh -- no seam triangle re-reading the
     * carried indices under the new base or constants. */
    { rsx_strip_carry c; memset(&c, 0, sizeof c); uint32_t tl[64];
      CHECK(rsx_prim_stream_key(5, 0) != rsx_prim_stream_key(5, 1));
      CHECK(rsx_prim_stream_key(5, 1) != rsx_prim_stream_key(6, 0));
      rsx_strip_carry_begin(&c, rsx_prim_stream_key(5, 0), 6, 0, 4, 1);
      CHECK(rsx_strip_assemble(&c, 0, NULL, 0, 4, 0, 0, tl, 64) == 6);
      CHECK(rsx_strip_carry_begin(&c, rsx_prim_stream_key(5, 0), 6, 4, 2, 1) == 1);   /* no barrier */
      CHECK(rsx_strip_assemble(&c, 0, NULL, 4, 2, 0, 0, tl, 64) == 6);
      CHECK(rsx_strip_carry_begin(&c, rsx_prim_stream_key(5, 1), 6, 6, 2, 1) == 0);   /* barrier */
      CHECK(rsx_strip_assemble(&c, 0, NULL, 6, 2, 0, 0, tl, 64) == 0);
      rsx_list_carry q; memset(&q, 0, sizeof q);
      CHECK(rsx_list_phase(&q, rsx_prim_stream_key(5, 1), 8, 0, 6, 4, 1) == 0);
      CHECK(rsx_list_phase(&q, rsx_prim_stream_key(5, 2), 8, 6, 6, 4, 1) == 0); }

    /* QUAD_STRIP: 6 vertices = 2 quads in triangle-strip winding; a trailing
     * odd vertex draws nothing; batches split anywhere match the whole. */
    { rsx_strip_carry c; memset(&c, 0, sizeof c); uint32_t tl[64];
      rsx_strip_carry_begin(&c, 1, 9, 0, 7, 1);
      const uint32_t n = rsx_quadstrip_assemble(&c, seq, 0, 7, 0, 0, tl, 64);
      CHECK(n == 12);
      const uint32_t want[12] = {1000,1001,1002, 1002,1001,1003, 1002,1003,1004, 1004,1003,1005};
      CHECK(memcmp(tl, want, sizeof want) == 0);
      /* strip winding: same triangles a TRIANGLE_STRIP of 6 would draw */
      rsx_strip_carry s; memset(&s, 0, sizeof s); uint32_t st[64];
      rsx_strip_carry_begin(&s, 1, 6, 0, 6, 1);
      CHECK(rsx_strip_assemble(&s, 0, seq, 0, 6, 0, 0, st, 64) == 12);
      CHECK(memcmp(st, want, sizeof want) == 0); }
    { static const uint32_t steps[] = {256, 3, 1, 5};
      uint32_t whole[1024], split[1024];
      for (uint32_t si = 0; si < 4; si++) {
          rsx_strip_carry c; memset(&c, 0, sizeof c);
          rsx_strip_carry_begin(&c, 2, 9, 0, 300, 1);
          const uint32_t nw = rsx_quadstrip_assemble(&c, seq, 0, 300, 0, 0, whole, 1024);
          CHECK(nw == 149 * 6);
          memset(&c, 0, sizeof c); uint32_t ns = 0;
          for (uint32_t f = 0; f < 300; f += steps[si]) {
              const uint32_t k = (300 - f < steps[si]) ? 300 - f : steps[si];
              CHECK(rsx_strip_carry_begin(&c, 2, 9, f, k, 1) == (f != 0));
              ns += rsx_quadstrip_assemble(&c, seq + f, f, k, 0, 0, split + ns, 1024 - ns);
          }
          CHECK(ns == nw && memcmp(whole, split, nw * 4) == 0);
      } }
    /* Restart inside a quad strip drops the unfinished quad and re-pairs. */
    { rsx_strip_carry c; memset(&c, 0, sizeof c); uint32_t tl[64];
      const uint32_t r[9] = {0, 1, 2, 0xFFFFu, 3, 4, 5, 6, 7};
      rsx_strip_carry_begin(&c, 3, 9, 0, 9, 1);
      CHECK(rsx_quadstrip_assemble(&c, r, 0, 9, 1, 0xFFFFu, tl, 64) == 6);
      CHECK(tl[0] == 3 && tl[1] == 4 && tl[2] == 5 && tl[3] == 5 && tl[4] == 4 && tl[5] == 6); }

    if (g_fail) { printf("%d failure(s)\n", g_fail); return 1; }
    printf("prim_assembly: all passed\n");
    return 0;
}
