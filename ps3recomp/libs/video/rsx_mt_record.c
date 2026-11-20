/*
 * ps3recomp - render-thread offload for the D3D12 frame prepass (R1 port).
 * See rsx_mt_record.h.
 */
#include "rsx_mt_record.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../../project/port_native_summary.h"   /* port_native_parse_bool: the shared switch rules */

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <intrin.h>
#include <d3d12.h>       /* view-creation jobs: vtable calls only, no GUIDs */
#endif

#define MTR_MAXQ    4
#define MTR_MAXW    4
#define MTR_PUBLISH 16      /* items between publishes */
#define MTR_SPIN    4000    /* pause loops before a worker sleeps (~50-100 us) */

static int s_cfg = -1, s_srv = 1, s_csum = 1, s_threads = 0, s_nw = 0, s_started = 0;

#ifdef _WIN32
static rsx_mtr_queue* volatile s_q[MTR_MAXQ];
static volatile long s_qbusy[MTR_MAXQ];
static SRWLOCK s_lk = SRWLOCK_INIT;
static CONDITION_VARIABLE s_cv = CONDITION_VARIABLE_INIT;
static volatile long s_sleepers, s_stop;
static HANDLE s_th[MTR_MAXW];

static long ld(volatile long* p) { long v = *p; _ReadWriteBarrier(); return v; }

static long q_claim(rsx_mtr_queue* q)
{
    for (;;) {
        const long nx = ld(&q->next), pb = ld(&q->pub);
        if (nx >= pb) return -1;
        if (InterlockedCompareExchange(&q->next, nx + 1, nx) == nx) return nx;
    }
}
static int q_drain(rsx_mtr_queue* q)
{
    int ran = 0; long i;
    while ((i = q_claim(q)) >= 0) {
        q->fn(q->ctx, q->items + (size_t)i * q->item_size);
        InterlockedIncrement(&q->done);
        ran++;
    }
    return ran;
}
/* producer-side wait: a short pause spin, then give the CPU away -- a worker that holds a claimed job may be
 * preempted on a busy machine, and spinning on its core only delays it */
static void wait_backoff(unsigned* spins)
{
    const unsigned s = (*spins)++;
    if (s < 256) YieldProcessor();
    else if (s < 512) SwitchToThread();
    else Sleep(0);
}
static int work_visible(void)
{
    for (int s = 0; s < MTR_MAXQ; s++) {
        rsx_mtr_queue* q = s_q[s];
        if (q && ld(&q->next) < ld(&q->pub)) return 1;
    }
    return 0;
}
static DWORD WINAPI worker(LPVOID arg)
{
    (void)arg;
    int spins = 0;
    for (;;) {
        int did = 0;
        for (int s = 0; s < MTR_MAXQ; s++) {
            InterlockedIncrement(&s_qbusy[s]);
            rsx_mtr_queue* q = s_q[s];
            if (q) { const int r = q_drain(q); if (r) { InterlockedExchangeAdd(&q->ran_w, r); did = 1; } }
            InterlockedDecrement(&s_qbusy[s]);
        }
        if (did) { spins = 0; continue; }
        if (ld(&s_stop)) break;
        if (++spins < MTR_SPIN) { YieldProcessor(); continue; }
        AcquireSRWLockExclusive(&s_lk);
        InterlockedIncrement(&s_sleepers);
        if (!work_visible() && !ld(&s_stop)) SleepConditionVariableSRW(&s_cv, &s_lk, 100, 0);
        InterlockedDecrement(&s_sleepers);
        ReleaseSRWLockExclusive(&s_lk);
        spins = 0;
    }
    return 0;
}
static void wake(void)
{
    if (ld(&s_sleepers)) {
        AcquireSRWLockExclusive(&s_lk);
        WakeAllConditionVariable(&s_cv);
        ReleaseSRWLockExclusive(&s_lk);
    }
}
static int auto_threads(void)   /* RSX_MT_RECORD_THREADS or the CPU-count rule (rsx_mtr_thread_count) */
{
    SYSTEM_INFO si; GetSystemInfo(&si);
    return rsx_mtr_thread_count((unsigned)si.dwNumberOfProcessors);
}
static void start_pool(void)
{
    int n = s_threads > 0 ? s_threads : auto_threads();
    if (n > MTR_MAXW) n = MTR_MAXW;
    s_stop = 0; s_nw = 0;
    for (int i = 0; i < n; i++) {
        s_th[i] = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        if (!s_th[i]) break;
        SetThreadPriority(s_th[i], THREAD_PRIORITY_ABOVE_NORMAL);
        s_nw++;
    }
}
#endif

static void read_env(void)
{
    if (s_cfg >= 0) return;
    s_cfg = port_native_parse_bool(getenv("RSX_MT_RECORD"), 1);
    s_srv = port_native_parse_bool(getenv("RSX_MT_RECORD_SRV"), 1);
    s_csum = port_native_parse_bool(getenv("RSX_MT_RECORD_CSUM"), 1);
}

void rsx_mtr_configure(int on, int threads, int srv, int csum)
{
    s_cfg = on ? 1 : 0; s_threads = threads; s_srv = srv; s_csum = csum;
}

int rsx_mtr_on(void)
{
    read_env();
    if (!s_cfg) return 0;
#ifdef _WIN32
    if (!s_started) {
        s_started = 1;
        start_pool();
        if (s_nw > 0)
            fprintf(stderr, "[native-hot] mt-record: %d worker thread%s take%s %s%s%s off the render thread"
                            " (RSX_MT_RECORD=0 off)%c", s_nw, s_nw == 1 ? "" : "s", s_nw == 1 ? "s" : "",
                    s_srv ? "SRV creation" : "", s_srv && s_csum ? " and " : "",
                    s_csum ? "texture checksums" : "", 10);
        else
            fprintf(stderr, "[native-hot] mt-record: too few CPUs for a worker -- off%c", 10);
    }
    return s_nw > 0;
#else
    (void)s_started;
    return 0;
#endif
}
int rsx_mtr_srv_on(void)  { return rsx_mtr_on() && s_srv; }
int rsx_mtr_csum_on(void) { return rsx_mtr_on() && s_csum; }
int rsx_mtr_workers(void) { return s_nw; }

void rsx_mtr_shutdown(void)
{
#ifdef _WIN32
    if (!s_nw) { s_cfg = -1; s_started = 0; return; }
    InterlockedExchange(&s_stop, 1);
    AcquireSRWLockExclusive(&s_lk); WakeAllConditionVariable(&s_cv); ReleaseSRWLockExclusive(&s_lk);
    for (int i = 0; i < s_nw; i++) { WaitForSingleObject(s_th[i], INFINITE); CloseHandle(s_th[i]); s_th[i] = NULL; }
    s_nw = 0;
#endif
    s_cfg = -1; s_started = 0;
}

/* ---- queues ---- */
int rsx_mtr_queue_init(rsx_mtr_queue* q, rsx_mtr_fn fn, void* ctx, unsigned item_size, unsigned cap)
{
    memset(q, 0, sizeof *q);
    q->fn = fn; q->ctx = ctx; q->item_size = item_size; q->cap = cap; q->slot = -1;
    q->items = (unsigned char*)malloc((size_t)item_size * cap);
    return q->items != NULL;
}
void rsx_mtr_queue_free(rsx_mtr_queue* q)
{
    free(q->items); q->items = NULL; q->cap = 0;
}
void rsx_mtr_begin(rsx_mtr_queue* q)
{
    q->n = 0; q->pub = 0; q->next = 0; q->done = 0; q->ran_w = 0; q->slot = -1;
#ifdef _WIN32
    if (!q->items || !rsx_mtr_on()) return;
    for (int s = 0; s < MTR_MAXQ; s++)
        if (!s_q[s] && InterlockedCompareExchangePointer((PVOID volatile*)&s_q[s], q, NULL) == NULL) {
            q->slot = s; break;
        }
#endif
}
void rsx_mtr_publish(rsx_mtr_queue* q)
{
#ifdef _WIN32
    if (q->slot < 0 || q->pub == q->n) return;
    InterlockedExchange(&q->pub, q->n);
    wake();
#else
    (void)q;
#endif
}
void rsx_mtr_push(rsx_mtr_queue* q, const void* item)
{
    q->jobs++;
    if (q->slot < 0 || (unsigned)q->n >= q->cap) {   /* no pool / full: run it here */
        unsigned char tmp[256];
        void* it = q->item_size <= sizeof tmp ? (void*)tmp : malloc(q->item_size);
        if (!it) return;
        memcpy(it, item, q->item_size);
        q->fn(q->ctx, it);
        if (it != (void*)tmp) free(it);
        q->inline_runs++;
        return;
    }
    memcpy(q->items + (size_t)q->n * q->item_size, item, q->item_size);
    q->n++;
    if (q->n - q->pub >= MTR_PUBLISH) rsx_mtr_publish(q);
}
void rsx_mtr_finish(rsx_mtr_queue* q)
{
#ifdef _WIN32
    if (q->slot < 0) return;
    rsx_mtr_publish(q);
    const long own = q_drain(q);   /* help: run whatever no worker claimed yet */
    unsigned spins = 0;
    while (ld(&q->done) < q->n) wait_backoff(&spins);
    q->inline_runs += (unsigned long long)own;
    const int s = q->slot;
    InterlockedExchangePointer((PVOID volatile*)&s_q[s], NULL);
    spins = 0;
    while (ld(&s_qbusy[s])) wait_backoff(&spins);   /* no worker still holds the pointer */
    q->by_workers += (unsigned long long)q->ran_w;
    q->slot = -1;
#else
    (void)q;
#endif
}

/* ---- D3D12 view-creation jobs ---- */
#ifdef _WIN32
typedef struct { ID3D12Device* dev; ID3D12Resource* res; D3D12_CPU_DESCRIPTOR_HANDLE h;
                 D3D12_SHADER_RESOURCE_VIEW_DESC sv; } mtr_srv_job;
static void mtr_srv_run(void* ctx, void* item)
{
    (void)ctx;
    mtr_srv_job* j = (mtr_srv_job*)item;
    j->dev->lpVtbl->CreateShaderResourceView(j->dev, j->res, &j->sv, j->h);
}
int rsx_mtr_srv_queue_init(rsx_mtr_queue* q, unsigned cap)
{
    return rsx_mtr_queue_init(q, mtr_srv_run, NULL, sizeof(mtr_srv_job), cap);
}
void rsx_mtr_srv_push(rsx_mtr_queue* q, void* device, void* res, size_t cpu_handle, const void* srv_desc)
{
    mtr_srv_job j;
    j.dev = (ID3D12Device*)device; j.res = (ID3D12Resource*)res; j.h.ptr = cpu_handle;
    memcpy(&j.sv, srv_desc, sizeof j.sv);
    if (q->slot < 0 || (unsigned)q->n >= q->cap) {   /* runs here and now: no reference needed */
        mtr_srv_run(NULL, &j);
        q->jobs++; q->inline_runs++;
        return;
    }
    if (j.res) j.res->lpVtbl->AddRef(j.res);
    rsx_mtr_push(q, &j);
}
void rsx_mtr_srv_finish(rsx_mtr_queue* q)
{
    const long n = q->slot >= 0 ? q->n : 0;
    rsx_mtr_finish(q);
    /* the references are dropped here, on the producer, so the workers never
     * contend with it on a resource's reference count */
    for (long i = 0; i < n; i++) {
        mtr_srv_job* j = (mtr_srv_job*)(q->items + (size_t)i * q->item_size);
        if (j->res) j->res->lpVtbl->Release(j->res);
    }
}
#else
int rsx_mtr_srv_queue_init(rsx_mtr_queue* q, unsigned cap) { (void)q; (void)cap; return 0; }
void rsx_mtr_srv_push(rsx_mtr_queue* q, void* device, void* res, size_t cpu_handle, const void* srv_desc)
{ (void)q; (void)device; (void)res; (void)cpu_handle; (void)srv_desc; }
void rsx_mtr_srv_finish(rsx_mtr_queue* q) { (void)q; }
#endif

/* ---- checksum memo ---- */
static unsigned memo_hash(const unsigned char* b, unsigned nb, int kind)
{
    unsigned long long k = (unsigned long long)(size_t)b ^ ((unsigned long long)nb << 29) ^ (unsigned)kind;
    k ^= k >> 31; k *= 0x9E3779B97F4A7C15ull; k ^= k >> 29;
    return (unsigned)k;
}
static void memo_compute(rsx_mtr_memo* m, rsx_mtr_memo_ent* e)
{
    e->val = m->fn(e->base, e->nb, e->kind);
#ifdef _WIN32
    InterlockedExchange(&e->state, 2);
#else
    e->state = 2;
#endif
}
static void memo_job(void* ctx, void* item)
{
    rsx_mtr_memo* m = (rsx_mtr_memo*)ctx;
    rsx_mtr_memo_ent* e = &m->e[*(unsigned*)item];
#ifdef _WIN32
    if (InterlockedCompareExchange(&e->state, 1, 0) == 0) memo_compute(m, e);
#else
    if (e->state == 0) { e->state = 1; memo_compute(m, e); }
#endif
}
int rsx_mtr_memo_init(rsx_mtr_memo* m, unsigned cap, rsx_mtr_csum_fn fn)
{
    memset(m, 0, sizeof *m);
    unsigned c = 16; while (c < cap) c <<= 1;
    m->e = (rsx_mtr_memo_ent*)calloc(c, sizeof *m->e);
    if (!m->e) return 0;
    m->cap = c; m->fn = fn;
    if (!rsx_mtr_queue_init(&m->q, memo_job, m, sizeof(unsigned), c / 2)) { free(m->e); m->e = NULL; return 0; }
    return 1;
}
void rsx_mtr_memo_free(rsx_mtr_memo* m)
{
    free(m->e); m->e = NULL; rsx_mtr_queue_free(&m->q);
}
void rsx_mtr_memo_begin(rsx_mtr_memo* m)
{
    if (!m->e) return;
    if (++m->gen == 0) {                 /* wrapped: old stamps could match again */
        for (unsigned i = 0; i < m->cap; i++) m->e[i].gen = 0;
        m->gen = 1;
    }
    m->n = 0; m->active = 1;
    rsx_mtr_begin(&m->q);
}
static rsx_mtr_memo_ent* memo_find(rsx_mtr_memo* m, const unsigned char* b, unsigned nb, int kind, int insert)
{
    unsigned h = memo_hash(b, nb, kind);
    for (unsigned k = 0; k < m->cap; k++) {
        rsx_mtr_memo_ent* e = &m->e[(h + k) & (m->cap - 1)];
        if (e->gen != m->gen) {
            if (!insert) return NULL;
            e->base = b; e->nb = nb; e->kind = kind; e->state = 0; e->val = 0; e->gen = m->gen;
            m->n++;
            return e;
        }
        if (e->base == b && e->nb == nb && e->kind == kind) return e;
    }
    return NULL;
}
void rsx_mtr_memo_add(rsx_mtr_memo* m, const unsigned char* base, unsigned nb, int kind)
{
    if (!m->active || m->n >= m->cap / 2) return;
    const unsigned n0 = m->n;
    rsx_mtr_memo_ent* e = memo_find(m, base, nb, kind, 1);
    if (e && m->n != n0) {
        unsigned idx = (unsigned)(e - m->e);
        rsx_mtr_push(&m->q, &idx);
    }
}
void rsx_mtr_memo_start(rsx_mtr_memo* m)
{
    if (m->active) rsx_mtr_publish(&m->q);
}
int rsx_mtr_memo_get(rsx_mtr_memo* m, const unsigned char* base, unsigned nb, int kind, unsigned* out)
{
    if (!m->active) return 0;
    rsx_mtr_memo_ent* e = memo_find(m, base, nb, kind, 0);
    if (!e) { m->misses++; return 0; }
#ifdef _WIN32
    long st = ld(&e->state);
    if (st == 0 && InterlockedCompareExchange(&e->state, 1, 0) == 0) {
        memo_compute(m, e); m->self++;
    } else {
        if (st != 2) { m->waits++; while (ld(&e->state) != 2) YieldProcessor(); }
        else m->hits++;
    }
#else
    if (e->state != 2) { e->state = 1; memo_compute(m, e); m->self++; } else m->hits++;
#endif
    *out = e->val;
    return 1;
}
void rsx_mtr_memo_end(rsx_mtr_memo* m)
{
    if (!m->active) return;
#ifdef _WIN32
    if (m->q.slot >= 0) {   /* drop the sums nobody has started: the frame no longer needs them */
        rsx_mtr_publish(&m->q);
        const long pb = ld(&m->q.pub);
        long nx;
        do nx = ld(&m->q.next); while (nx < pb && InterlockedCompareExchange(&m->q.next, pb, nx) != nx);
        if (nx < pb) InterlockedExchangeAdd(&m->q.done, pb - nx);
    }
#endif
    rsx_mtr_finish(&m->q);
    m->active = 0;
}
