/*
 * ps3recomp - render-thread offload for the D3D12 frame prepass (R1 port).
 *
 * RSX_MT_RECORD (default 1): a small worker pool (RSX_MT_RECORD_THREADS,
 * 0/auto = 1-2 by core count) that takes two independent pieces of per-draw
 * work off the render thread while it walks the frame's draws:
 *   - RSX_MT_RECORD_SRV (default 1): the CreateShaderResourceView calls that
 *     fill each draw's own descriptor window. The render thread decides every
 *     view exactly as before and only hands over (device, resource, view desc,
 *     CPU handle); the pool creates them and the frame waits for all of them
 *     before it records anything that reads the windows.
 *   - RSX_MT_RECORD_CSUM (default 1): the sparse "did this texture change"
 *     checksums. The frame's bound textures are listed up front and their
 *     sums computed by the pool; the render thread takes a finished sum from
 *     the memo (or computes it itself if it gets there first). A memo entry is
 *     keyed by the exact inputs (address, byte count, quick/full), so a sum is
 *     the one the render thread would compute.
 * Logs one line when it engages. With the pool off every queue runs inline.
 */
#ifndef RSX_MT_RECORD_H
#define RSX_MT_RECORD_H

#include <stddef.h>
#include "../../../project/port_native_summary.h"   /* port_native_parse_level */

#ifdef __cplusplus
extern "C" {
#endif

/* Worker count from RSX_MT_RECORD_THREADS (0 / unset / unparsable = auto) and the logical CPU count: the one
 * rule the pool and the [native] summary (main.cpp) both use. Auto = 2 with 8+ CPUs, 1 with 4-7, none below. */
static inline int rsx_mtr_thread_count(unsigned ncpu)
{
    const int t = port_native_parse_level(getenv("RSX_MT_RECORD_THREADS"), 0);
    if (t > 0) return t;
    return ncpu >= 8 ? 2 : ncpu >= 4 ? 1 : 0;
}

/* RSX_MT_REPLAY (rsx_mt_replay.h): the draw replay split across worker command lists, on top of the pool.
 * RT_REBIND (a debug target rebind before every draw) keeps it off. Shared with the [native] summary. */
#define RSX_MTR_REPLAY_DEFAULT 0   /* opt-in: the gain is unmeasured in game */
static inline int rsx_mtr_replay_env(void)
{
    const char* rb = getenv("RT_REBIND");
    if (rb && atoi(rb)) return 0;
    return port_native_parse_bool(getenv("RSX_MT_REPLAY"), RSX_MTR_REPLAY_DEFAULT);
}
/* RSX_MT_REPLAY_MIN_DRAWS (level, default 512): frames with fewer draws stay in one list */
static inline unsigned rsx_mtr_replay_min_draws(void)
{
    const int v = port_native_parse_level(getenv("RSX_MT_REPLAY_MIN_DRAWS"), 512);
    return v < 0 ? 0u : (unsigned)v;
}

/* Switches (read once). rsx_mtr_on() also starts the workers. */
int  rsx_mtr_on(void);
int  rsx_mtr_srv_on(void);
int  rsx_mtr_csum_on(void);
int  rsx_mtr_workers(void);
/* Tests: set the switches instead of reading the environment (call before the
 * first rsx_mtr_on, or after rsx_mtr_shutdown). threads 0 = auto. */
void rsx_mtr_configure(int on, int threads, int srv, int csum);
void rsx_mtr_shutdown(void);

/* A job queue the pool drains while its producer keeps pushing. Items are
 * copied in; fn(ctx, item) runs once per item on some thread, in no
 * particular order. Use from one producer thread at a time. */
typedef void (*rsx_mtr_fn)(void* ctx, void* item);
typedef struct rsx_mtr_queue {
    rsx_mtr_fn fn; void* ctx;
    unsigned char* items; unsigned item_size, cap;
    volatile long pub, next, done;
    long n; int slot;
    unsigned long long jobs, by_workers, inline_runs;
    volatile long ran_w;
} rsx_mtr_queue;
int  rsx_mtr_queue_init(rsx_mtr_queue* q, rsx_mtr_fn fn, void* ctx, unsigned item_size, unsigned cap);
void rsx_mtr_queue_free(rsx_mtr_queue* q);
void rsx_mtr_begin(rsx_mtr_queue* q);              /* open; runs inline when the pool is off/full */
void rsx_mtr_push(rsx_mtr_queue* q, const void* item);
void rsx_mtr_publish(rsx_mtr_queue* q);            /* let the workers see what was pushed */
void rsx_mtr_finish(rsx_mtr_queue* q);             /* every pushed item has run on return */

/* D3D12 view creation as queue jobs (Windows): the queue's items are
 * (device, resource, CPU handle, D3D12_SHADER_RESOURCE_VIEW_DESC). A queued
 * job holds a reference on res (may be NULL = null view), taken by push and
 * dropped by rsx_mtr_srv_finish on the producer (use it instead of
 * rsx_mtr_finish). Typed as void* so this header does not need d3d12.h. */
int  rsx_mtr_srv_queue_init(rsx_mtr_queue* q, unsigned cap);
void rsx_mtr_srv_push(rsx_mtr_queue* q, void* device, void* res, size_t cpu_handle, const void* srv_desc);
void rsx_mtr_srv_finish(rsx_mtr_queue* q);

/* Checksum memo: value = fn(base, nb, kind), computed ahead by the pool. */
typedef unsigned (*rsx_mtr_csum_fn)(const unsigned char* base, unsigned nb, int kind);
typedef struct {
    const unsigned char* base; unsigned nb; int kind;
    volatile long state;        /* 0 pending, 1 being computed, 2 done */
    unsigned val, gen;
} rsx_mtr_memo_ent;
typedef struct rsx_mtr_memo {
    rsx_mtr_memo_ent* e; unsigned cap;   /* power of two */
    unsigned gen, n; int active;
    rsx_mtr_csum_fn fn;
    rsx_mtr_queue q;
    unsigned long long hits, waits, self, misses;
} rsx_mtr_memo;
int  rsx_mtr_memo_init(rsx_mtr_memo* m, unsigned cap, rsx_mtr_csum_fn fn);
void rsx_mtr_memo_free(rsx_mtr_memo* m);
void rsx_mtr_memo_begin(rsx_mtr_memo* m);          /* new generation, starts accepting adds */
void rsx_mtr_memo_add(rsx_mtr_memo* m, const unsigned char* base, unsigned nb, int kind);
void rsx_mtr_memo_start(rsx_mtr_memo* m);          /* publish the adds to the pool */
/* 1 = *out holds fn(base, nb, kind) (from the memo, or computed now and
 * recorded); 0 = not listed this generation (or the memo is closed). */
int  rsx_mtr_memo_get(rsx_mtr_memo* m, const unsigned char* base, unsigned nb, int kind, unsigned* out);
void rsx_mtr_memo_end(rsx_mtr_memo* m);            /* waits for the pool; closes */

#ifdef __cplusplus
}
#endif
#endif
