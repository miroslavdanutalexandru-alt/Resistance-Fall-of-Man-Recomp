/*
 * rsx_mt_replay.h (RSX_MT_REPLAY): the D3D12 draw replay split across command
 * lists recorded on the rsx_mt_record worker pool.
 *
 * The render thread still walks every draw and makes every decision. Draws
 * that only set pipeline / viewport / root tables / root CBVs / stencil ref and
 * draw ("clean" draws, the caller decides) go through the rsx_mtr_rp_* emitters
 * while a run is open (rsx_mtr_rp_defer): they become a compact op stream
 * instead of D3D12 calls. Anything else (barriers, target switches, clears,
 * copies, helper passes) must be preceded by rsx_mtr_rp_flush, which either
 * replays the run into the current list (short runs) or closes the current
 * list, hands the run to the workers in chunks -- each chunk in its own list
 * that first re-establishes the full state (root signature, heap, IA, root
 * SRV, render targets, stencil ref, then pipeline / viewport / tables / CBVs
 * as of the chunk's first op) -- and opens a continuation list with the state
 * as of the run's end. rsx_mtr_rp_submit executes every list in recording
 * order with one ExecuteCommandLists, so the GPU sees the same commands in
 * the same order as one list would hold, plus redundant state at list starts.
 *
 * Contract for the caller: a run's first draw sets all of its own state
 * (pipeline, viewport+scissor, tables, CBVs) -- the backend forgets its
 * redundant-state filter when it opens a run -- and the base given to
 * rsx_mtr_rp_defer is the state the list holds and the run does not change.
 * Command lists come from a per-slot pool: rsx_mtr_rp_frame_begin(slot) may
 * reset slot's allocators, so a slot must only come round again once the GPU
 * finished the batch that last used it (as for the caller's own allocator).
 * Needs d3d12.h included first. Windows only.
 */
#ifndef RSX_MT_REPLAY_H
#define RSX_MT_REPLAY_H
#ifdef _WIN32
#include "rsx_mt_record.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rsx_mtr_rp_base {
    ID3D12RootSignature* rs;
    ID3D12DescriptorHeap* heap;               /* NULL = none */
    ID3D12DescriptorHeap* heap2;              /* a second (sampler) heap set with heap; NULL = none */
    D3D12_PRIMITIVE_TOPOLOGY topo;
    int has_vb; D3D12_VERTEX_BUFFER_VIEW vbv;
    int has_ib; D3D12_INDEX_BUFFER_VIEW ibv;
    int srv_param; D3D12_GPU_VIRTUAL_ADDRESS srv;   /* root SRV; srv_param < 0 = none */
    UINT nrt; D3D12_CPU_DESCRIPTOR_HANDLE rtv[4];
    int has_dsv; D3D12_CPU_DESCRIPTOR_HANDLE dsv;
} rsx_mtr_rp_base;

typedef struct rsx_mtr_rp_stats {
    unsigned long long frames, runs, runs_split, draws_deferred, draws_split, chunks, chunks_by_workers, lists, fallbacks;
    unsigned long long list_errs, rerecorded, dropped;   /* lists that failed to reset/close; chunks recorded again / left out */
    unsigned long long batches_lost, exec_lists;         /* batches not submitted (main/continuation close failed); lists executed */
    double wait_us, exec_us;   /* submit: waiting for the chunks; inside ExecuteCommandLists */
} rsx_mtr_rp_stats;

typedef struct rsx_mtr_rp rsx_mtr_rp;
/* NULL when out of memory */
rsx_mtr_rp* rsx_mtr_rp_create(ID3D12Device* dev);
void rsx_mtr_rp_destroy(rsx_mtr_rp* r);
/* main: the caller's list, already Reset and recording; slot: its allocator ring index (< 8). */
void rsx_mtr_rp_frame_begin(rsx_mtr_rp* r, unsigned slot, ID3D12GraphicsCommandList* main);
ID3D12GraphicsCommandList* rsx_mtr_rp_list(const rsx_mtr_rp* r);   /* the list the render thread records into */
int  rsx_mtr_rp_deferring(const rsx_mtr_rp* r);
/* open a run (no-op while one is open or the frame ran out of room) */
void rsx_mtr_rp_defer(rsx_mtr_rp* r, const rsx_mtr_rp_base* base);
/* close the run: runs of >= min_split draws go to the workers; returns the current list */
ID3D12GraphicsCommandList* rsx_mtr_rp_flush(rsx_mtr_rp* r, unsigned min_split);
/* close, wait for the chunks, execute everything in order; the current list is main again. Returns the lists
 * executed: 0 = main or a continuation failed to close, nothing of the batch was submitted, and the fatal
 * handler (rsx_mtr_rp_set_fatal) was called first */
unsigned rsx_mtr_rp_submit(rsx_mtr_rp* r, ID3D12CommandQueue* q, unsigned min_split);
void rsx_mtr_rp_get_stats(const rsx_mtr_rp* r, rsx_mtr_rp_stats* out);
/* 1 once a list failed beyond repair: no more runs are opened (everything records into main) */
int  rsx_mtr_rp_failed(const rsx_mtr_rp* r);
/* Called when a batch cannot be submitted (main or a continuation failed to close): the caller's state
 * trackers already count it as executed, so the game ends there (the backend reports and aborts). */
void rsx_mtr_rp_set_fatal(rsx_mtr_rp* r, void (*fn)(void* ctx, const char* why), void* ctx);
/* Tests: calls at .. at+count-1 (0-based, per kind) of one kind report failure. A worker list that
 * fails is recorded again by the render thread at submit; if that fails it is not submitted. */
enum { RSX_MTR_RP_F_CHUNK_RESET, RSX_MTR_RP_F_CHUNK_CLOSE, RSX_MTR_RP_F_CONT_RESET, RSX_MTR_RP_F_CONT_CLOSE,
       RSX_MTR_RP_F_RERECORD, RSX_MTR_RP_NFAULT };
void rsx_mtr_rp_inject_fault(rsx_mtr_rp* r, int kind, long at, long count);

/* emitters: an op while a run is open, else the call on the current list */
void rsx_mtr_rp_pso(rsx_mtr_rp* r, ID3D12PipelineState* pso);
void rsx_mtr_rp_vpsc(rsx_mtr_rp* r, const D3D12_VIEWPORT* vp, const D3D12_RECT* sc);
void rsx_mtr_rp_table(rsx_mtr_rp* r, UINT param, D3D12_GPU_DESCRIPTOR_HANDLE h);
void rsx_mtr_rp_cbv(rsx_mtr_rp* r, UINT param, D3D12_GPU_VIRTUAL_ADDRESS a);
void rsx_mtr_rp_sref(rsx_mtr_rp* r, UINT ref);
void rsx_mtr_rp_bfactor(rsx_mtr_rp* r, const float f[4]);   /* OMSetBlendFactor */
void rsx_mtr_rp_dbounds(rsx_mtr_rp* r, float mn, float mx);  /* OMSetDepthBounds (command list 1) */
void rsx_mtr_rp_draw(rsx_mtr_rp* r, UINT verts, UINT start);
void rsx_mtr_rp_draw_indexed(rsx_mtr_rp* r, UINT count, UINT first, INT base);

#ifdef __cplusplus
}
#endif
#endif
#endif
