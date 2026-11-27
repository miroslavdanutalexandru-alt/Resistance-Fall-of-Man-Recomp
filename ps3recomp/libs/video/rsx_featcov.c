/* rsx_featcov.c -- RSX feature coverage tracer; see rsx_featcov.h.
 * Identical in all three ports (ps3recomp/libs/video). */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "rsx_featcov.h"
#include "rsx_featcov_tables.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
static SRWLOCK s_lock = SRWLOCK_INIT;
#define FC_LOCK()   AcquireSRWLockExclusive(&s_lock)
#define FC_UNLOCK() ReleaseSRWLockExclusive(&s_lock)
static unsigned long long fc_now_ms(void) { return GetTickCount64(); }
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
#define FC_LOCK()   pthread_mutex_lock(&s_lock)
#define FC_UNLOCK() pthread_mutex_unlock(&s_lock)
static unsigned long long fc_now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
}
#endif

unsigned char g_rsx_featcov_flags[RSX_FC_NREGS];
unsigned int g_rsx_featcov_last[RSX_FC_NREGS];
unsigned int g_rsx_featcov_vp_seen[2];

/* Value kinds. Method-carried kinds first, then the shader kinds
 * (RSX_FC_FP_OP.. mapped to VK_SHADER + kind - 1). */
enum {
    VK_NONE, VK_PRIM, VK_TEX_FMT, VK_TEX_DIM, VK_TEX_REMAP, VK_TEX_MIN, VK_TEX_MAG, VK_TEX_CONV,
    VK_TEX_SIGNED, VK_TEX_ANISO, VK_TEX_ALPHAKILL, VK_TEX_WRAP, VK_TEX_ZFUNC, VK_TEX_GAMMA,
    VK_VTEX_FMT, VK_VTEX_DIM, VK_SURF_COLOR, VK_SURF_DEPTH, VK_SURF_TYPE, VK_SURF_AA,
    VK_BLEND_EQ, VK_BLEND_SF, VK_BLEND_DF, VK_BLEND_MRT, VK_ALPHA_FUNC, VK_FOG_MODE,
    VK_POINT_SPRITE, VK_AA_CONTROL, VK_CLIP_PLANE, VK_TWO_SIDED_STENCIL, VK_LOGIC_OP_EN,
    VK_LOGIC_OP, VK_POLY_FRONT, VK_POLY_BACK, VK_SHADE_MODE, VK_DEPTH_BOUNDS, VK_SHADER_CONTROL,
    VK_SHADER, /* = RSX_FC_FP_OP */
    VK_FP_DERIV = VK_SHADER + RSX_FC_FP_SHADOW,
    VK_FP_PROG,     /* SHADER_PROGRAM values already decoded (R1; not dumped) */
    VK_COUNT
};
static const char* const k_vk_names[VK_COUNT] = {
    "none", "prim", "tex_fmt", "tex_dim", "tex_remap", "tex_min", "tex_mag", "tex_conv",
    "tex_signed", "tex_aniso", "tex_alphakill", "tex_wrap", "tex_zfunc", "tex_gamma",
    "vtex_fmt", "vtex_dim", "surf_color", "surf_depth", "surf_type", "surf_aa",
    "blend_eq", "blend_sfactor", "blend_dfactor", "blend_mrt", "alpha_func", "fog_mode",
    "point_sprite", "aa_control", "clip_plane", "two_sided_stencil", "logic_op_enable",
    "logic_op", "poly_mode_front", "poly_mode_back", "shade_mode", "depth_bounds", "shader_control",
    "fp_op", "vp_vec", "vp_sca", "fp_texdim", "vp_texdim", "fp_shadow", "fp_deriv", "fp_prog",
};
#define VK_CAP 96      /* distinct values kept per kind */
#define VK_PROG_CAP 4096

/* Distinct values: open addressing, written under the lock, probed without it
 * (key is published last; 0 = empty). key = kind << 32 | value. */
#define FC_HASH 16384u
typedef struct {
    volatile unsigned long long key;
    unsigned short src;        /* carrying method offset >> 2 (0 for shader kinds) */
    unsigned char st;          /* 0 real, 1 noop, 2 ignored, 3 unhandled */
    unsigned char dumped;
} fc_val;
static fc_val s_vals[FC_HASH];
static unsigned s_kind_n[VK_COUNT];
static volatile unsigned char s_kind_full[VK_COUNT];

static unsigned char s_dumped[RSX_FC_NREGS];
static unsigned char s_st_r1[RSX_FC_NREGS], s_st_rp[RSX_FC_NREGS];
static volatile int s_off;   /* RSX_FEATCOV=0 seen: fast bail-out for the ucode hooks */
static int s_inited, s_enabled = 1, s_port, s_dump_no, s_thread_started;
static unsigned s_interval_s = 60;
static unsigned long long s_t0;
static FILE* s_out;

static const char* const k_st_names[4] = { "real", "noop", "ignored", "unhandled" };

static int fc_value_kinds(unsigned off, unsigned v, unsigned* k, unsigned* x)
{
    int n = 0;
#define PUT(kind, val) do { k[n] = (kind); x[n] = (val); n++; } while (0)
    if (off >= 0x1A00 && off < 0x1C00) {
        switch (off & 0x1F) {
        case 0x04: PUT(VK_TEX_FMT, (v >> 8) & 0xFF); PUT(VK_TEX_DIM, ((v >> 4) & 0xF) | ((v >> 2) & 1) << 4); break;
        case 0x08: PUT(VK_TEX_WRAP, v & 0xF); PUT(VK_TEX_WRAP, (v >> 8) & 0xF); PUT(VK_TEX_WRAP, (v >> 16) & 0xF);
                   PUT(VK_TEX_ZFUNC, v >> 28); if ((v >> 20) & 0xF) PUT(VK_TEX_GAMMA, (v >> 20) & 0xF); break;
        case 0x0C: if (v >> 31) { PUT(VK_TEX_ANISO, (v >> 4) & 7); PUT(VK_TEX_ALPHAKILL, (v >> 2) & 3); } break;
        case 0x10: PUT(VK_TEX_REMAP, v & 0xFFFF); break;
        case 0x14: PUT(VK_TEX_MIN, (v >> 16) & 0xFF); PUT(VK_TEX_MAG, (v >> 24) & 0xF);
                   PUT(VK_TEX_CONV, (v >> 13) & 7); if (v >> 28) PUT(VK_TEX_SIGNED, v >> 28); break;
        }
        return n;
    }
    if (off >= 0x0900 && off < 0x0980) {
        if ((off & 0x1F) == 0x04) { PUT(VK_VTEX_FMT, (v >> 8) & 0xFF); PUT(VK_VTEX_DIM, ((v >> 4) & 0xF) | ((v >> 2) & 1) << 4); }
        return n;
    }
    switch (off) {
    case 0x1808: if (v) PUT(VK_PRIM, v); break;
    case 0x0208: PUT(VK_SURF_COLOR, v & 0x1F); PUT(VK_SURF_DEPTH, (v >> 5) & 7);
                 PUT(VK_SURF_TYPE, (v >> 8) & 0xF); PUT(VK_SURF_AA, (v >> 12) & 0xF); break;
    case 0x0320: PUT(VK_BLEND_EQ, v & 0xFFFF); PUT(VK_BLEND_EQ, v >> 16); break;
    case 0x0314: PUT(VK_BLEND_SF, v & 0xFFFF); PUT(VK_BLEND_SF, v >> 16); break;
    case 0x0318: PUT(VK_BLEND_DF, v & 0xFFFF); PUT(VK_BLEND_DF, v >> 16); break;
    case 0x036C: PUT(VK_BLEND_MRT, v); break;
    case 0x0308: PUT(VK_ALPHA_FUNC, v); break;
    case 0x08CC: PUT(VK_FOG_MODE, v); break;
    case 0x1EE8: PUT(VK_POINT_SPRITE, v); break;
    case 0x1D7C: PUT(VK_AA_CONTROL, v); break;
    case 0x1478: PUT(VK_CLIP_PLANE, v); break;
    case 0x0348: PUT(VK_TWO_SIDED_STENCIL, v); break;
    case 0x0374: PUT(VK_LOGIC_OP_EN, v); break;
    case 0x0378: PUT(VK_LOGIC_OP, v); break;
    case 0x1828: PUT(VK_POLY_FRONT, v); break;
    case 0x182C: PUT(VK_POLY_BACK, v); break;
    case 0x0368: PUT(VK_SHADE_MODE, v); break;
    case 0x0380: PUT(VK_DEPTH_BOUNDS, v); break;
    case 0x1D60: PUT(VK_SHADER_CONTROL, v); break;
    }
#undef PUT
    return n;
}

static int fc_is_tracked(unsigned off)
{
    unsigned k[8], x[8];
    /* a value that yields keys for every tracked register (0x80000000: tex enable) */
    return fc_value_kinds(off, 0x80000001u, k, x) > 0 || off == 0x1808;
}

static fc_val* fc_find(unsigned long long key)
{
    unsigned h = (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 51) & (FC_HASH - 1u);
    for (unsigned n = 0; n < FC_HASH; n++, h = (h + 1u) & (FC_HASH - 1u)) {
        const unsigned long long k = s_vals[h].key;
        if (k == key || k == 0) return &s_vals[h];
    }
    return NULL;
}

static unsigned char fc_method_st(unsigned idx)
{
    return s_port == RSX_FC_PORT_RPCS3 ? s_st_rp[idx] : s_st_r1[idx];
}

/* under the lock */
static void fc_add_value(unsigned kind, unsigned val, unsigned src, unsigned char st)
{
    const unsigned long long key = (unsigned long long)kind << 32 | val;
    fc_val* e = fc_find(key);
    if (!e) return;
    if (e->key == key) {
        if (st > e->st) { e->st = st; e->dumped = 0; }   /* later report: decompiler lacks it */
        return;
    }
    if (s_kind_full[kind]) return;
    if (++s_kind_n[kind] >= (kind == VK_FP_PROG ? VK_PROG_CAP : VK_CAP)) s_kind_full[kind] = 1;
    e->src = (unsigned short)src; e->st = st; e->dumped = 0;
#if defined(_MSC_VER) && !defined(__clang__)
    _ReadWriteBarrier();
#else
    __asm__ __volatile__("" ::: "memory");
#endif
    e->key = key;
}

static int fc_have_value(unsigned kind, unsigned val)
{
    const unsigned long long key = (unsigned long long)kind << 32 | val;
    const fc_val* e = fc_find(key);
    return (e && e->key == key) || s_kind_full[kind];
}

static const rsx_featcov_meth* fc_meth(unsigned off)
{
    int lo = 0, hi = RSX_FEATCOV_NMETHS - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const unsigned o = k_rsx_featcov_meths[mid].off;
        if (o == off) return &k_rsx_featcov_meths[mid];
        if (o < off) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

static void fc_dump_locked(const char* why)
{
    if (!s_inited || !s_enabled) return;
    FILE* o = s_out ? s_out : stderr;
    unsigned nm = 0, nv = 0, tm = 0, tv = 0;
    for (unsigned i = 0; i < RSX_FC_NREGS; i++)
        if (g_rsx_featcov_flags[i]) { tm++; if (!s_dumped[i]) nm++; }
    for (unsigned i = 0; i < FC_HASH; i++)
        if (s_vals[i].key) { tv++; if (!s_vals[i].dumped) nv++; }
    s_dump_no++;
    fprintf(o, "[featcov] dump #%d %s t=%llus port=%s new_methods=%u/%u new_values=%u/%u\n",
            s_dump_no, why, (fc_now_ms() - s_t0) / 1000ull,
            s_port == RSX_FC_PORT_RPCS3 ? "rpcs3" : "r1", nm, tm, nv, tv);
    for (unsigned i = 0; i < RSX_FC_NREGS; i++) {
        if (!g_rsx_featcov_flags[i] || s_dumped[i]) continue;
        s_dumped[i] = 1;
        const rsx_featcov_meth* m = fc_meth(i << 2);
        fprintf(o, "[featcov] m 0x%04X %s %s\n", i << 2, m ? m->name : "?", k_st_names[fc_method_st(i)]);
    }
    for (unsigned kind = 1; kind < VK_FP_PROG; kind++) {
        for (unsigned i = 0; i < FC_HASH; i++) {
            fc_val* e = &s_vals[i];
            if (!e->key || e->dumped || (unsigned)(e->key >> 32) != kind) continue;
            e->dumped = 1;
            if (e->src)
                fprintf(o, "[featcov] v %s 0x%X %s via=0x%04X\n", k_vk_names[kind],
                        (unsigned)e->key, k_st_names[e->st & 3], (unsigned)e->src << 2);
            else
                fprintf(o, "[featcov] v %s 0x%X %s\n", k_vk_names[kind], (unsigned)e->key, k_st_names[e->st & 3]);
        }
        if (s_kind_full[kind] == 1) {
            s_kind_full[kind] = 2;
            fprintf(o, "[featcov] cap %s %u values (more not recorded)\n", k_vk_names[kind], VK_CAP);
        }
    }
    fflush(o);
}

static volatile long s_exit_dumped;

/* Exit paths (ExitProcess/_exit, atexit fallback, signal-ish contexts): dump once, never wait
 * more than ~5 ms for the lock (another thread may be mid-insert or killed while holding it). */
static void fc_dump_exit(void)
{
#ifdef _WIN32
    if (InterlockedExchange(&s_exit_dumped, 1)) return;
    LARGE_INTEGER f, t0, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (;;) {                                       /* Sleep(1) can be 15 ms: spin with yields */
        if (TryAcquireSRWLockExclusive(&s_lock)) { fc_dump_locked("exit"); FC_UNLOCK(); return; }
        QueryPerformanceCounter(&t);
        if ((t.QuadPart - t0.QuadPart) * 1000 > f.QuadPart * 5) break;
        SwitchToThread();
    }
    s_exit_dumped = 0;                               /* lock busy: let a later exit path retry */
#else
    if (__sync_lock_test_and_set(&s_exit_dumped, 1)) return;
    for (int i = 0; i < 50; i++) {
        if (pthread_mutex_trylock(&s_lock) == 0) { fc_dump_locked("exit"); FC_UNLOCK(); return; }
        usleep(100);
    }
    s_exit_dumped = 0;
#endif
}

void rsx_featcov_dump(const char* why)
{
    if (why && strcmp(why, "exit") == 0) { fc_dump_exit(); return; }
    FC_LOCK();
    fc_dump_locked(why ? why : "manual");
    FC_UNLOCK();
}

static void fc_atexit(void) { fc_dump_exit(); }

#ifdef _WIN32
static DWORD WINAPI fc_thread(LPVOID arg)
{
    (void)arg;
    for (;;) { Sleep(s_interval_s * 1000u); rsx_featcov_dump("periodic"); }
    return 0;
}
#else
static void* fc_thread(void* arg)
{
    (void)arg;
    for (;;) { sleep(s_interval_s); rsx_featcov_dump("periodic"); }
    return NULL;
}
#endif

/* under the lock */
static void fc_init(void)
{
    /* Default off (RSX thread perf): RSX_FEATCOV=1/on/true/yes enables */
    const char* e = getenv("RSX_FEATCOV");
    s_enabled = e && ((e[0] >= '1' && e[0] <= '9') || e[0] == 'y' || e[0] == 'Y' || e[0] == 't' || e[0] == 'T' ||
                      ((e[0] == 'o' || e[0] == 'O') && (e[1] == 'n' || e[1] == 'N')));
    const char* s = getenv("RSX_FEATCOV_SEC");
    s_interval_s = s ? (unsigned)atoi(s) : 60u;
    memset(s_st_r1, 3, sizeof s_st_r1);
    memset(s_st_rp, 3, sizeof s_st_rp);
    for (int i = 0; i < RSX_FEATCOV_NMETHS; i++) {
        s_st_r1[k_rsx_featcov_meths[i].off >> 2] = k_rsx_featcov_meths[i].r1;
        s_st_rp[k_rsx_featcov_meths[i].off >> 2] = k_rsx_featcov_meths[i].rpcs3;
    }
    s_t0 = fc_now_ms();
    s_inited = 1;
    if (!s_enabled) { s_off = 1; memset(g_rsx_featcov_vp_seen, 0xFF, sizeof g_rsx_featcov_vp_seen); return; }
    static int s_atexit;
    if (!s_atexit) { s_atexit = 1; atexit(fc_atexit); }
    if (s_interval_s && !s_thread_started) {
        s_thread_started = 1;
#ifdef _WIN32
        HANDLE h = CreateThread(NULL, 64 * 1024, fc_thread, NULL, 0, NULL);
        if (h) CloseHandle(h);
#else
        pthread_t t;
        if (pthread_create(&t, NULL, fc_thread, NULL) == 0) pthread_detach(t);
#endif
    }
    fprintf(s_out ? s_out : stderr, "[featcov] on (dump every %us and at exit; RSX_FEATCOV=1 enables, default off)\n", s_interval_s);
}

void rsx_featcov_slow(int port, unsigned idx, unsigned value)
{
    idx &= RSX_FC_NREGS - 1u;
    const unsigned off = idx << 2;
    unsigned k[8], x[8];
    const unsigned char f = g_rsx_featcov_flags[idx];
    int n = 0;
    if (f == RSX_FC_TRACKED) {
        /* value change on a tracked register: lock-free probe first */
        g_rsx_featcov_last[idx] = value;
        n = fc_value_kinds(off, value, k, x);
        int all = 1;
        for (int j = 0; j < n; j++) if (!fc_have_value(k[j], x[j])) { all = 0; break; }
        if (all) return;
    }
    FC_LOCK();
    if (!s_inited) fc_init();
    if (!s_port) s_port = port;
    if (!s_enabled) { g_rsx_featcov_flags[idx] = RSX_FC_SEEN; FC_UNLOCK(); return; }
    if (!g_rsx_featcov_flags[idx]) {
        g_rsx_featcov_last[idx] = value;
        n = fc_value_kinds(off, value, k, x);
        g_rsx_featcov_flags[idx] = fc_is_tracked(off) ? RSX_FC_TRACKED : RSX_FC_SEEN;
    }
    const unsigned char st = fc_method_st(idx);
    for (int j = 0; j < n; j++) fc_add_value(k[j], x[j], idx, st);
    FC_UNLOCK();
}

void rsx_featcov_shader(int port, int kind, unsigned value, int implemented)
{
    if (kind < RSX_FC_FP_OP || kind > RSX_FC_FP_SHADOW) return;
    const unsigned vk = VK_SHADER + (unsigned)kind - 1u;
    const unsigned char st = implemented ? 0 : 3;
    const int deriv = kind == RSX_FC_FP_OP && ((value & 63u) == 0x15u || (value & 63u) == 0x16u);
    if (implemented && fc_have_value(vk, value) && (!deriv || fc_have_value(VK_FP_DERIV, value & 63u)))
        return;
    FC_LOCK();
    if (!s_inited) fc_init();
    if (!s_port) s_port = port;
    if (s_enabled) {
        fc_add_value(vk, value, 0, st);
        if (deriv) fc_add_value(VK_FP_DERIV, value & 63u, 0, st);
    }
    FC_UNLOCK();
}

void rsx_featcov_vp_slow(int port, unsigned vec, unsigned sca)
{
    const int r1 = port == RSX_FC_PORT_R1;
    rsx_featcov_shader(port, RSX_FC_VP_VEC, vec, !r1 || ((RSX_FEATCOV_R1_VP_VEC_OK >> vec) & 1u));
    rsx_featcov_shader(port, RSX_FC_VP_SCA, sca, !r1 || ((RSX_FEATCOV_R1_VP_SCA_OK >> sca) & 1u));
    FC_LOCK();
    if (s_enabled) { g_rsx_featcov_vp_seen[0] |= 1u << (vec & 31u); g_rsx_featcov_vp_seen[1] |= 1u << (sca & 31u); }
    FC_UNLOCK();
}

int rsx_featcov_fp_program(unsigned key)
{
    if (s_off || s_kind_full[VK_FP_PROG] || fc_have_value(VK_FP_PROG, key)) return 0;
    int fresh = 0;
    FC_LOCK();
    if (!s_inited) fc_init();
    if (s_enabled && !fc_have_value(VK_FP_PROG, key)) { fc_add_value(VK_FP_PROG, key, 0, 0); fresh = 1; }
    FC_UNLOCK();
    return fresh;
}

static unsigned fc_fp_word(const unsigned char* p)
{
    const unsigned be = (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3];
    return be << 16 | be >> 16;    /* FP ucode: big-endian words, 16-bit halves swapped */
}

void rsx_featcov_fp_ucode(int port, const unsigned char* ucode, unsigned max_bytes)
{
    if (!ucode) return;
    const int r1 = port == RSX_FC_PORT_R1;
    for (unsigned off = 0, n = 0; off + 16 <= max_bytes && n < 1024; n++) {
        const unsigned w0 = fc_fp_word(ucode + off), w1 = fc_fp_word(ucode + off + 4);
        const unsigned w2 = fc_fp_word(ucode + off + 8), w3 = fc_fp_word(ucode + off + 12);
        off += 16;
        const unsigned op = (w0 >> 24) & 63u, br = w2 >> 31;
        const int ok = !r1 || (br ? (RSX_FEATCOV_R1_FP_BRANCH_OK >> op) & 1u : (unsigned)((RSX_FEATCOV_R1_FP_OK >> op) & 1u));
        rsx_featcov_shader(port, RSX_FC_FP_OP, op | br << 6, ok);
        if (!br && ((w1 & 3u) == 2u || (w2 & 3u) == 2u || (w3 & 3u) == 2u)) off += 16;   /* inline constant */
        if (w0 & 1u) break;                                                              /* END */
    }
}

int rsx_featcov_enabled(void)
{
    FC_LOCK();
    if (!s_inited) fc_init();
    const int en = s_enabled;
    FC_UNLOCK();
    return en;
}

void rsx_featcov_test_reset(void* file)
{
    FC_LOCK();
    memset(g_rsx_featcov_flags, 0, sizeof g_rsx_featcov_flags);
    memset(g_rsx_featcov_last, 0, sizeof g_rsx_featcov_last);
    memset(s_vals, 0, sizeof s_vals);
    memset(s_kind_n, 0, sizeof s_kind_n);
    memset((void*)s_kind_full, 0, sizeof s_kind_full);
    memset(s_dumped, 0, sizeof s_dumped);
    memset(g_rsx_featcov_vp_seen, 0, sizeof g_rsx_featcov_vp_seen);
    s_inited = 0; s_port = 0; s_dump_no = 0; s_off = 0; s_exit_dumped = 0;
    s_out = (FILE*)file;
    FC_UNLOCK();
}

void rsx_featcov_test_hold_lock(int hold)
{
    if (hold) FC_LOCK(); else FC_UNLOCK();
}
