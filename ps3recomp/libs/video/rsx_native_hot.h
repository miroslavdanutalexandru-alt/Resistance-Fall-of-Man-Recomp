/*
 * ps3recomp - native host replacements for render-side hot loops (R1 port).
 *
 * RSX_NATIVE_WCCOPY (default 1): a non-temporal (streaming-store) copy for the
 * vertex ranges (GPU fetch) and vertex constants written into the
 * write-combined D3D12 upload heap every draw. The bytes are the ones memcpy
 * would write; a self-test against memcpy runs on first use and keeps memcpy
 * if the two ever differ. Logs one line when it engages.
 *
 * The texture decode's native path (RSX_NATIVE_TEXDEC) lives with the
 * reference decode in rsx_texture_layout.c.
 */
#ifndef RSX_NATIVE_HOT_H
#define RSX_NATIVE_HOT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* memcpy for a write-combined destination: same bytes, non-temporal stores.
 * Does NOT fence; call rsx_nh_wc_fence() before the GPU may read the data. */
void rsx_nh_copy_wc(void* dst, const void* src, size_t len);
void rsx_nh_wc_fence(void);
/* RSX_NATIVE_WCCOPY, after its first-use self-test against memcpy. */
int  rsx_nh_wccopy_on(void);

/* Bookkeeping for the opt-in SRV descriptor cache (RSX_SRV_CACHE): a table of
 * (resource, format, mapping, cube) keys, one slot per cached descriptor. Each
 * entry holds a reference on its resource (addref on insert, release on drop)
 * so a resource released outside the deferred-release path cannot come back
 * at the same address and hit a stale descriptor. Entries are dropped when:
 *   - their resource goes to the deferred-release grave (invalidate),
 *   - they are unused for more than `age` batches and that last use has
 *     completed on the GPU (sweep, once per batch, or reuse by insert),
 *   - the device changes (clear).
 * Kept apart from the backend so the reference handling is unit-tested. */
typedef struct {
    void* res; unsigned fmt, mapping; int cube; unsigned last;
} rsx_nh_srvc_ent;
typedef struct {
    rsx_nh_srvc_ent* e;
    unsigned n;                  /* power of two */
    void (*addref)(void* res);
    void (*release)(void* res);
    unsigned live;
} rsx_nh_srvc;
/* Slot for the key, or -1 if no slot is free (create the view directly).
 * *hit = 1 when the slot already holds this key's descriptor; 0 when it was
 * just (re)assigned and the caller must write the descriptor into it.
 * now = current batch number, done = highest batch complete on the GPU. */
int      rsx_nh_srvc_lookup(rsx_nh_srvc* c, void* res, unsigned fmt, unsigned mapping, int cube,
                            unsigned now, unsigned done, unsigned age, int* hit);
unsigned rsx_nh_srvc_sweep(rsx_nh_srvc* c, unsigned now, unsigned done, unsigned age);
unsigned rsx_nh_srvc_invalidate(rsx_nh_srvc* c, void* res);
void     rsx_nh_srvc_clear(rsx_nh_srvc* c);

#ifdef __cplusplus
}
#endif
#endif
