/*
 * rsx_mt_replay.c (RSX_MT_REPLAY): the draw replay split across command lists
 * recorded on the rsx_mt_record pool. See rsx_mt_replay.h.
 */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsx_mt_replay.h"

#define RP_MAXL   48        /* command lists per frame (chunks + continuations) */
#define RP_MAXS   8         /* allocator ring slots */
#define RP_MAXP   8         /* root parameters tracked */
#define RP_OPS    65536     /* ops per frame */
#define RP_RUNS   1024      /* runs per frame */
#define RP_CHUNK  64        /* fewest draws per chunk */

/* local copies: this file is linked without initguid / dxguid */
static const GUID k_iid_alloc = {0x6102dee4, 0xaf59, 0x4b09, {0xb9, 0x99, 0xb4, 0x4d, 0x73, 0xf0, 0x9b, 0x24}};
static const GUID k_iid_list  = {0x5b160d0f, 0xac1b, 0x4185, {0x8b, 0xa8, 0xb3, 0xae, 0x42, 0xa5, 0xa4, 0x55}};
static const GUID k_iid_list1 = {0x553103fb, 0x1fe7, 0x4557, {0xbb, 0x38, 0x94, 0x6d, 0x7d, 0x0e, 0x7c, 0xa7}};   /* ID3D12GraphicsCommandList1 */

enum { OP_PSO, OP_VPSC, OP_TABLE, OP_CBV, OP_SREF, OP_DRAW, OP_DRAWIDX, OP_BFACT, OP_DBND };
/* blend factor / depth bounds a list carries like the stencil ref (unset: never emitted) */
typedef struct { int has_bf, has_db; float bf[4]; float db[2]; } rp_om;
typedef struct {
    unsigned k, p;
    union {
        ID3D12PipelineState* pso;
        UINT64 a;
        UINT ref;
        float f[4];
        struct { UINT n, s; INT b; } d;
        struct { D3D12_VIEWPORT vp; D3D12_RECT sc; } v;
    } u;
} rp_op;

typedef struct {
    ID3D12PipelineState* pso;
    int has_vp; D3D12_VIEWPORT vp; D3D12_RECT sc;
    unsigned char rk[RP_MAXP];   /* 0 unset, 1 table, 2 CBV */
    UINT64 rv[RP_MAXP];
    UINT sref;
    rp_om x;
} rp_shadow;

typedef struct { struct rsx_mtr_rp* r; unsigned run, list, rs, ob, oe; UINT sref0; rp_om x0; } rp_job;

struct rsx_mtr_rp {
    ID3D12Device* dev;
    rp_op* ops; unsigned nops;
    rsx_mtr_rp_base* runs; unsigned nruns;
    unsigned* dend; unsigned ndraw;   /* op index just past each draw this frame */
    unsigned run_d0;
    rp_shadow run_sh;               /* the run's state so far */
    ID3D12GraphicsCommandList* lists[RP_MAXL];
    ID3D12CommandAllocator* alloc[RP_MAXS][RP_MAXL];
    unsigned used[RP_MAXS];
    ID3D12CommandList* seq[RP_MAXL + 1]; unsigned nseq;
    int seqk[RP_MAXL + 1];          /* pool list behind seq[i]; -1 = main */
    rp_job jobs[RP_MAXL];           /* chunk lists: what to re-record when a worker's list failed */
    unsigned char is_chunk[RP_MAXL];
    volatile long lok[RP_MAXL];     /* list recorded and closed */
    int cur_k;                      /* pool list behind cur; -1 = main */
    int failed;                     /* a list could not be recorded: one list from now on */
    int seq_bad;                    /* main or a continuation failed to close: the batch is not submitted */
    void (*fatal)(void* ctx, const char* why); void* fatal_ctx;
    volatile long fcnt[RSX_MTR_RP_NFAULT]; long fat[RSX_MTR_RP_NFAULT], fnum[RSX_MTR_RP_NFAULT];
    ID3D12GraphicsCommandList *main, *cur;
    unsigned slot, nl;              /* nl: pool lists taken this frame */
    int deferring, cur_own_open, q_open;
    unsigned run, run_start, run_draws;
    UINT run_sref, sref;
    rp_om run_x, x;
    rsx_mtr_queue q;
    rsx_mtr_rp_stats st;
};

static void set_dbounds(ID3D12GraphicsCommandList* cl, float mn, float mx)
{
    ID3D12GraphicsCommandList1* c1 = NULL;
    if (SUCCEEDED(cl->lpVtbl->QueryInterface(cl, &k_iid_list1, (void**)&c1)) && c1) {
        c1->lpVtbl->OMSetDepthBounds(c1, mn, mx);
        c1->lpVtbl->Release(c1);
    }
}
static void om_apply(rp_om* x, const rp_op* o)
{
    if (o->k == OP_BFACT) { x->has_bf = 1; memcpy(x->bf, o->u.f, sizeof x->bf); }
    else if (o->k == OP_DBND) { x->has_db = 1; x->db[0] = o->u.f[0]; x->db[1] = o->u.f[1]; }
}
static void emit(ID3D12GraphicsCommandList* cl, const rp_op* o)
{
    switch (o->k) {
    case OP_PSO:   cl->lpVtbl->SetPipelineState(cl, o->u.pso); break;
    case OP_VPSC:  cl->lpVtbl->RSSetViewports(cl, 1, &o->u.v.vp); cl->lpVtbl->RSSetScissorRects(cl, 1, &o->u.v.sc); break;
    case OP_TABLE: { D3D12_GPU_DESCRIPTOR_HANDLE h; h.ptr = o->u.a; cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, o->p, h); } break;
    case OP_CBV:   cl->lpVtbl->SetGraphicsRootConstantBufferView(cl, o->p, o->u.a); break;
    case OP_SREF:  cl->lpVtbl->OMSetStencilRef(cl, o->u.ref); break;
    case OP_BFACT: cl->lpVtbl->OMSetBlendFactor(cl, o->u.f); break;
    case OP_DBND:  set_dbounds(cl, o->u.f[0], o->u.f[1]); break;
    case OP_DRAW:  cl->lpVtbl->DrawInstanced(cl, o->u.d.n, 1, o->u.d.s, 0); break;
    case OP_DRAWIDX: cl->lpVtbl->DrawIndexedInstanced(cl, o->u.d.n, 1, o->u.d.s, o->u.d.b, 0); break;
    }
}
static void shadow_apply(rp_shadow* s, const rp_op* o)
{
    switch (o->k) {
    case OP_PSO:   s->pso = o->u.pso; break;
    case OP_VPSC:  s->has_vp = 1; s->vp = o->u.v.vp; s->sc = o->u.v.sc; break;
    case OP_TABLE: s->rk[o->p] = 1; s->rv[o->p] = o->u.a; break;
    case OP_CBV:   s->rk[o->p] = 2; s->rv[o->p] = o->u.a; break;
    case OP_SREF:  s->sref = o->u.ref; break;
    case OP_BFACT: case OP_DBND: om_apply(&s->x, o); break;
    default: break;
    }
}
/* everything a fresh list needs to continue the run where s says it is */
static void emit_state(ID3D12GraphicsCommandList* cl, const rsx_mtr_rp_base* b, const rp_shadow* s)
{
    cl->lpVtbl->SetGraphicsRootSignature(cl, b->rs);
    if (b->heap) {
        ID3D12DescriptorHeap* hs[2] = { b->heap, b->heap2 };
        cl->lpVtbl->SetDescriptorHeaps(cl, b->heap2 ? 2u : 1u, hs);
    }
    cl->lpVtbl->IASetPrimitiveTopology(cl, b->topo);
    if (b->has_vb) cl->lpVtbl->IASetVertexBuffers(cl, 0, 1, &b->vbv);
    if (b->has_ib) cl->lpVtbl->IASetIndexBuffer(cl, &b->ibv);
    if (b->srv_param >= 0) cl->lpVtbl->SetGraphicsRootShaderResourceView(cl, (UINT)b->srv_param, b->srv);
    cl->lpVtbl->OMSetRenderTargets(cl, b->nrt, b->rtv, FALSE, b->has_dsv ? &b->dsv : NULL);
    cl->lpVtbl->OMSetStencilRef(cl, s->sref);
    if (s->x.has_bf) cl->lpVtbl->OMSetBlendFactor(cl, s->x.bf);
    if (s->x.has_db) set_dbounds(cl, s->x.db[0], s->x.db[1]);
    if (s->pso) cl->lpVtbl->SetPipelineState(cl, s->pso);
    if (s->has_vp) { cl->lpVtbl->RSSetViewports(cl, 1, &s->vp); cl->lpVtbl->RSSetScissorRects(cl, 1, &s->sc); }
    for (UINT p = 0; p < RP_MAXP; p++) {
        if (s->rk[p] == 1) { D3D12_GPU_DESCRIPTOR_HANDLE h; h.ptr = s->rv[p]; cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, p, h); }
        else if (s->rk[p] == 2) cl->lpVtbl->SetGraphicsRootConstantBufferView(cl, p, s->rv[p]);
    }
}

/* tests: the at-th .. (at+count-1)-th call of a kind fails (rsx_mtr_rp_inject_fault) */
static int fault(struct rsx_mtr_rp* r, int k)
{
    if (!r->fnum[k]) return 0;
    const long i = InterlockedIncrement(&r->fcnt[k]) - 1;
    return i >= r->fat[k] && i < r->fat[k] + r->fnum[k];
}
/* an injected failure leaves the list open -- not executable, so the D3D12 runtime rejects it if it is
 * ever submitted (what the fault test checks) */
static HRESULT list_reset(struct rsx_mtr_rp* r, unsigned k, int fk)
{
    const HRESULT hr = r->lists[k]->lpVtbl->Reset(r->lists[k], r->alloc[r->slot][k], NULL);
    return SUCCEEDED(hr) && fault(r, fk) ? E_FAIL : hr;
}
static HRESULT list_close(struct rsx_mtr_rp* r, unsigned k, int fk)
{
    if (fault(r, fk)) return E_FAIL;
    return r->lists[k]->lpVtbl->Close(r->lists[k]);
}
/* one chunk into its own list; 1 = recorded and closed */
static int record_chunk(struct rsx_mtr_rp* r, const rp_job* j, int fk_reset, int fk_close)
{
    ID3D12GraphicsCommandList* cl = r->lists[j->list];
    if (FAILED(list_reset(r, j->list, fk_reset))) return 0;
    /* the chunk starts from the state of everything before it in the run */
    rp_shadow s; memset(&s, 0, sizeof s); s.sref = j->sref0; s.x = j->x0;
    for (unsigned i = j->rs; i < j->ob; i++) shadow_apply(&s, &r->ops[i]);
    emit_state(cl, &r->runs[j->run], &s);
    for (unsigned i = j->ob; i < j->oe; i++) emit(cl, &r->ops[i]);
    return SUCCEEDED(list_close(r, j->list, fk_close));
}
static void chunk_job(void* ctx, void* item)
{
    (void)ctx;
    const rp_job* j = (const rp_job*)item;
    j->r->lok[j->list] = record_chunk(j->r, j, RSX_MTR_RP_F_CHUNK_RESET, RSX_MTR_RP_F_CHUNK_CLOSE);
}
static void give_up(struct rsx_mtr_rp* r, const char* what)
{
    if (!r->failed)
        fprintf(stderr, "[native-hot] mt-replay: %s -- one command list from now on%c", what, 10);
    r->failed = 1;
}
/* close the render thread's current list; a pool list that fails to close is not submitted */
static void close_cur(struct rsx_mtr_rp* r)
{
    if (r->cur_k < 0) { if (FAILED(r->cur->lpVtbl->Close(r->cur))) r->seq_bad = 1; return; }
    r->lok[r->cur_k] = SUCCEEDED(list_close(r, (unsigned)r->cur_k, RSX_MTR_RP_F_CONT_CLOSE));
    if (!r->lok[r->cur_k]) r->seq_bad = 1;
}

rsx_mtr_rp* rsx_mtr_rp_create(ID3D12Device* dev)
{
    struct rsx_mtr_rp* r = (struct rsx_mtr_rp*)calloc(1, sizeof *r);
    if (!r) return NULL;
    r->dev = dev;
    r->ops = (rp_op*)malloc(sizeof(rp_op) * RP_OPS);
    r->runs = (rsx_mtr_rp_base*)malloc(sizeof(rsx_mtr_rp_base) * RP_RUNS);
    r->dend = (unsigned*)malloc(sizeof(unsigned) * RP_OPS);
    if (!r->ops || !r->runs || !r->dend || !rsx_mtr_queue_init(&r->q, chunk_job, r, sizeof(rp_job), RP_MAXL)) {
        free(r->ops); free(r->runs); free(r->dend); free(r); return NULL;
    }
    dev->lpVtbl->AddRef(dev);
    return r;
}

static void end_frame_lists(struct rsx_mtr_rp* r)
{
    if (r->q_open) {
        rsx_mtr_finish(&r->q); r->q_open = 0;
        r->st.chunks_by_workers += r->q.by_workers; r->q.by_workers = 0;
    }
    if (r->cur_own_open) { close_cur(r); r->cur_own_open = 0; }
}

void rsx_mtr_rp_destroy(rsx_mtr_rp* r)
{
    if (!r) return;
    end_frame_lists(r);
    for (int i = 0; i < RP_MAXL; i++) if (r->lists[i]) r->lists[i]->lpVtbl->Release(r->lists[i]);
    for (int s = 0; s < RP_MAXS; s++)
        for (int i = 0; i < RP_MAXL; i++) if (r->alloc[s][i]) r->alloc[s][i]->lpVtbl->Release(r->alloc[s][i]);
    rsx_mtr_queue_free(&r->q);
    r->dev->lpVtbl->Release(r->dev);
    free(r->ops); free(r->runs); free(r->dend); free(r);
}

void rsx_mtr_rp_frame_begin(rsx_mtr_rp* r, unsigned slot, ID3D12GraphicsCommandList* main)
{
    end_frame_lists(r);   /* a frame that never reached submit */
    r->slot = slot % RP_MAXS;
    for (unsigned i = 0; i < r->used[r->slot]; i++) r->alloc[r->slot][i]->lpVtbl->Reset(r->alloc[r->slot][i]);
    r->used[r->slot] = 0;
    r->main = r->cur = main; r->cur_k = -1; r->seq_bad = 0;
    r->seq[0] = (ID3D12CommandList*)main; r->seqk[0] = -1; r->nseq = 1;
    r->nops = r->nruns = r->ndraw = r->nl = 0;
    r->deferring = 0; r->sref = 0; memset(&r->x, 0, sizeof r->x);
    r->st.frames++;
}

ID3D12GraphicsCommandList* rsx_mtr_rp_list(const rsx_mtr_rp* r) { return r->cur; }
int rsx_mtr_rp_deferring(const rsx_mtr_rp* r) { return r->deferring; }

void rsx_mtr_rp_defer(rsx_mtr_rp* r, const rsx_mtr_rp_base* base)
{
    if (r->deferring || r->failed) return;
    if (r->nruns >= RP_RUNS || RP_OPS - r->nops < 1024 || r->nl + 2 > RP_MAXL) { r->st.fallbacks++; return; }
    r->runs[r->nruns] = *base;
    r->run = r->nruns++;
    r->run_start = r->nops; r->run_draws = 0; r->run_sref = r->sref; r->run_d0 = r->ndraw;
    memset(&r->run_sh, 0, sizeof r->run_sh); r->run_sh.sref = r->sref;
    r->run_x = r->x; r->run_sh.x = r->x;
    r->deferring = 1;
    r->st.runs++;
}

/* pool list k, recording-ready objects created on first use */
static int take_list(struct rsx_mtr_rp* r, unsigned k)
{
    ID3D12CommandAllocator** a = &r->alloc[r->slot][k];
    if (!*a && FAILED(r->dev->lpVtbl->CreateCommandAllocator(r->dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                             &k_iid_alloc, (void**)a))) { *a = NULL; return 0; }
    if (!r->lists[k]) {
        if (FAILED(r->dev->lpVtbl->CreateCommandList(r->dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, *a, NULL,
                                                     &k_iid_list, (void**)&r->lists[k]))) {
            r->lists[k] = NULL; return 0;
        }
        r->lists[k]->lpVtbl->Close(r->lists[k]);
    }
    if (r->used[r->slot] < k + 1) r->used[r->slot] = k + 1;
    return 1;
}

ID3D12GraphicsCommandList* rsx_mtr_rp_flush(rsx_mtr_rp* r, unsigned min_split)
{
    if (!r->deferring) return r->cur;
    r->deferring = 0;
    const unsigned ob = r->run_start, oe = r->nops, nd = r->run_draws;
    if (oe == ob) return r->cur;
    r->st.draws_deferred += nd;
    unsigned n = 0;
    if (min_split && nd >= min_split && rsx_mtr_workers() > 0) {
        n = nd / RP_CHUNK;
        if (n > (unsigned)rsx_mtr_workers() + 1) n = (unsigned)rsx_mtr_workers() + 1;
        if (n < 1) n = 1;
        if (r->nl + n + 1 > RP_MAXL) n = 0;
        for (unsigned i = 0; i < n + 1 && n; i++) if (!take_list(r, r->nl + i)) n = 0;
    }
    if (!n) {   /* short run (or no room): straight into the current list */
        for (unsigned i = ob; i < oe; i++) emit(r->cur, &r->ops[i]);
        return r->cur;
    }
    /* the continuation (the render thread carries on from the run's end state) must open before anything
     * is handed out: if it cannot, the run goes straight into the current list */
    const unsigned ck = r->nl + n;
    if (FAILED(list_reset(r, ck, RSX_MTR_RP_F_CONT_RESET))) {
        r->st.list_errs++;
        r->lists[ck]->lpVtbl->Close(r->lists[ck]);   /* not left recording on an allocator that gets reset */
        give_up(r, "a command list failed to reset");
        for (unsigned i = ob; i < oe; i++) emit(r->cur, &r->ops[i]);
        return r->cur;
    }
    /* chunk c ends just past draw nd*(c+1)/n (the last one at the run's end) */
    close_cur(r);
    r->cur_own_open = 0;
    if (!r->q_open) { rsx_mtr_begin(&r->q); r->q_open = 1; }
    unsigned cb = ob;
    for (unsigned c = 0; c < n; c++) {
        const unsigned ce = c + 1 == n ? oe : r->dend[r->run_d0 + (unsigned)((unsigned long long)nd * (c + 1) / n) - 1];
        rp_job j; j.r = r; j.run = r->run; j.list = r->nl; j.rs = ob; j.ob = cb; j.oe = ce; j.sref0 = r->run_sref; j.x0 = r->run_x;
        r->jobs[r->nl] = j; r->is_chunk[r->nl] = 1; r->lok[r->nl] = 0;
        r->seqk[r->nseq] = (int)r->nl;
        r->seq[r->nseq++] = (ID3D12CommandList*)r->lists[r->nl];
        r->nl++;
        rsx_mtr_push(&r->q, &j);
        cb = ce;
    }
    const unsigned c = n;
    const rp_shadow s = r->run_sh;
    rsx_mtr_publish(&r->q);
    ID3D12GraphicsCommandList* cl = r->lists[ck];
    emit_state(cl, &r->runs[r->run], &s);
    r->is_chunk[ck] = 0; r->lok[ck] = 0;   /* set when it closes */
    r->seqk[r->nseq] = (int)ck;
    r->seq[r->nseq++] = (ID3D12CommandList*)cl;
    r->nl++;
    r->cur = cl; r->cur_k = (int)ck; r->cur_own_open = 1;
    r->st.runs_split++; r->st.draws_split += nd; r->st.chunks += c; r->st.lists += c + 1;
    return cl;
}

unsigned rsx_mtr_rp_submit(rsx_mtr_rp* r, ID3D12CommandQueue* q, unsigned min_split)
{
    rsx_mtr_rp_flush(r, min_split);
    LARGE_INTEGER f, t0, t1, t2;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    if (r->q_open) {
        rsx_mtr_finish(&r->q); r->q_open = 0;
        r->st.chunks_by_workers += r->q.by_workers; r->q.by_workers = 0;
    }
    close_cur(r);
    r->cur_own_open = 0;
    /* main and the continuations hold the render thread's own work (target switches, transitions, copies,
     * helper passes, the present barrier): if one failed to close, nothing of the batch is submitted */
    if (r->seq_bad) {
        r->st.list_errs++; r->st.batches_lost++;
        for (unsigned i = 1; i < r->nseq; i++)   /* none left recording on an allocator that gets reset */
            if (!r->lok[r->seqk[i]]) r->lists[r->seqk[i]]->lpVtbl->Close(r->lists[r->seqk[i]]);
        fprintf(stderr, "[native-hot] mt-replay: a command list failed to close, so the batch was not submitted%c", 10);
        r->failed = 1;
        r->cur = r->main; r->cur_k = -1; r->nseq = 1;
        /* the caller's trackers (resource states, uploaded textures) already count the batch as done: not
         * recoverable. The fatal handler does not return in the game (crash report + exit); tests return. */
        if (r->fatal) r->fatal(r->fatal_ctx, "a command list failed to close");
        return 0;
    }
    /* only lists that recorded and closed go to the GPU: a failed worker chunk is recorded again here (its
     * ops live until the next frame_begin); if that fails too it is left out -- it holds draws only, the
     * transitions around it are in main and the continuations -- and the replay stays on one list after */
    unsigned w = 0;
    for (unsigned i = 0; i < r->nseq; i++) {
        const int k = r->seqk[i];
        if (k >= 0 && !r->lok[k]) {   /* a chunk: seq_bad covers the others */
            r->st.list_errs++;
            r->lists[k]->lpVtbl->Close(r->lists[k]);   /* Reset needs it closed; fails if it is */
            if (record_chunk(r, &r->jobs[k], RSX_MTR_RP_F_RERECORD, RSX_MTR_RP_F_RERECORD)) {
                r->lok[k] = 1; r->st.rerecorded++;
                static int said = 0;
                if (!said) { said = 1; fprintf(stderr, "[native-hot] mt-replay: a worker command list failed -- recorded again on the render thread%c", 10); }
            } else {
                r->st.dropped++;
                r->lists[k]->lpVtbl->Close(r->lists[k]);
                give_up(r, "a command list failed to record");
                continue;
            }
        }
        r->seq[w++] = r->seq[i];
    }
    r->nseq = w;
    r->st.exec_lists += w;
    QueryPerformanceCounter(&t1);
    q->lpVtbl->ExecuteCommandLists(q, r->nseq, r->seq);
    QueryPerformanceCounter(&t2);
    r->st.wait_us += (double)(t1.QuadPart - t0.QuadPart) * 1e6 / (double)f.QuadPart;
    r->st.exec_us += (double)(t2.QuadPart - t1.QuadPart) * 1e6 / (double)f.QuadPart;
    const unsigned n = r->nseq;
    r->cur = r->main; r->cur_k = -1; r->nseq = 1;
    return n;
}

void rsx_mtr_rp_get_stats(const rsx_mtr_rp* r, rsx_mtr_rp_stats* out) { *out = r->st; }
int rsx_mtr_rp_failed(const rsx_mtr_rp* r) { return r->failed; }
void rsx_mtr_rp_set_fatal(rsx_mtr_rp* r, void (*fn)(void* ctx, const char* why), void* ctx) { r->fatal = fn; r->fatal_ctx = ctx; }
void rsx_mtr_rp_inject_fault(rsx_mtr_rp* r, int kind, long at, long count)
{
    if (kind < 0 || kind >= RSX_MTR_RP_NFAULT) return;
    r->fcnt[kind] = 0; r->fat[kind] = at; r->fnum[kind] = count;
}

/* ---- emitters ---- */
static void put(struct rsx_mtr_rp* r, const rp_op* o)
{
    if (o->k == OP_SREF) r->sref = o->u.ref;
    om_apply(&r->x, o);
    if (r->deferring) {
        if (r->nops < RP_OPS && o->p < RP_MAXP) {
            r->ops[r->nops++] = *o;
            if (o->k == OP_DRAW || o->k == OP_DRAWIDX) { r->dend[r->ndraw++] = r->nops; r->run_draws++; }
            else shadow_apply(&r->run_sh, o);
            return;
        }
        r->st.fallbacks++;
        rsx_mtr_rp_flush(r, 0);   /* out of room: what is queued goes straight in, then this */
    }
    emit(r->cur, o);
}
void rsx_mtr_rp_pso(rsx_mtr_rp* r, ID3D12PipelineState* pso)
{ rp_op o; o.k = OP_PSO; o.p = 0; o.u.pso = pso; put(r, &o); }
void rsx_mtr_rp_vpsc(rsx_mtr_rp* r, const D3D12_VIEWPORT* vp, const D3D12_RECT* sc)
{ rp_op o; o.k = OP_VPSC; o.p = 0; o.u.v.vp = *vp; o.u.v.sc = *sc; put(r, &o); }
void rsx_mtr_rp_table(rsx_mtr_rp* r, UINT param, D3D12_GPU_DESCRIPTOR_HANDLE h)
{ rp_op o; o.k = OP_TABLE; o.p = param; o.u.a = h.ptr; put(r, &o); }
void rsx_mtr_rp_cbv(rsx_mtr_rp* r, UINT param, D3D12_GPU_VIRTUAL_ADDRESS a)
{ rp_op o; o.k = OP_CBV; o.p = param; o.u.a = a; put(r, &o); }
void rsx_mtr_rp_sref(rsx_mtr_rp* r, UINT ref)
{ rp_op o; o.k = OP_SREF; o.p = 0; o.u.ref = ref; put(r, &o); }
void rsx_mtr_rp_bfactor(rsx_mtr_rp* r, const float f[4])
{ rp_op o; o.k = OP_BFACT; o.p = 0; memcpy(o.u.f, f, sizeof o.u.f); put(r, &o); }
void rsx_mtr_rp_dbounds(rsx_mtr_rp* r, float mn, float mx)
{ rp_op o; o.k = OP_DBND; o.p = 0; o.u.f[0] = mn; o.u.f[1] = mx; o.u.f[2] = o.u.f[3] = 0.0f; put(r, &o); }
void rsx_mtr_rp_draw(rsx_mtr_rp* r, UINT verts, UINT start)
{ rp_op o; o.k = OP_DRAW; o.p = 0; o.u.d.n = verts; o.u.d.s = start; o.u.d.b = 0; put(r, &o); }
void rsx_mtr_rp_draw_indexed(rsx_mtr_rp* r, UINT count, UINT first, INT base)
{ rp_op o; o.k = OP_DRAWIDX; o.p = 0; o.u.d.n = count; o.u.d.s = first; o.u.d.b = base; put(r, &o); }
#endif
