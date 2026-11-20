/* test_rsx_featcov.c -- RSX feature coverage tracer (rsx_featcov.c).
 * New-only dumps, value sets, per-port status, shader decode, RSX_FEATCOV=0,
 * and the per-method-write cost. Build: rsx_featcov.c alongside (no other deps). */
#include "rsx_featcov.h"

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
static void env_set(const char* n, const char* v) { _putenv_s(n, v); }
static double now_s(void)
{
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
#else
static void env_set(const char* n, const char* v) { setenv(n, v, 1); }
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
#endif

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static const char* g_out_path = "test_rsx_featcov_out.txt";
static char g_buf[1 << 16];
static FILE* g_f;
static long g_pos;

static void start(const char* featcov_env)
{
    env_set("RSX_FEATCOV", featcov_env);
    env_set("RSX_FEATCOV_SEC", "0");               /* no periodic thread */
    if (g_f) fclose(g_f);
    g_f = fopen(g_out_path, "w+");
    g_pos = 0;
    rsx_featcov_test_reset(g_f);
}

/* the dump text written since the last call */
static const char* dump(void)
{
    static long pos;
    rsx_featcov_dump("test");
    fflush(g_f);
    fseek(g_f, 0, SEEK_END);
    const long end = ftell(g_f);
    if (end < pos) pos = 0;
    fseek(g_f, g_pos, SEEK_SET);
    const size_t n = fread(g_buf, 1, sizeof g_buf - 1, g_f);
    g_buf[n] = 0;
    g_pos = end;
    fseek(g_f, 0, SEEK_END);
    return g_buf;
}

static void fp_put(unsigned char* p, unsigned w)    /* inverse of the FP half-swap + BE read */
{
    const unsigned be = w << 16 | w >> 16;
    p[0] = (unsigned char)(be >> 24); p[1] = (unsigned char)(be >> 16); p[2] = (unsigned char)(be >> 8); p[3] = (unsigned char)be;
}

static void test_methods_r1(void)
{
    start("1");
    rsx_featcov_method(RSX_FC_PORT_R1, 0x1808, 5);      /* BEGIN_END TRIANGLES */
    rsx_featcov_method(RSX_FC_PORT_R1, 0x1808, 0);
    rsx_featcov_method(RSX_FC_PORT_R1, 0x1808, 5);
    rsx_featcov_method(RSX_FC_PORT_R1, 0x08CC, 0x2601); /* FOG_MODE: R1 has no handler */
    rsx_featcov_method(RSX_FC_PORT_R1, 0x1A04 + 0x20 * 3, 0x0000A9 << 8 | 0x2 << 4 | 1 << 2);
    rsx_featcov_method(RSX_FC_PORT_R1, 0x0320, 0x8006u << 16 | 0x800Au); /* blend eq rgb SUB, a ADD */
    const char* d = dump();
    CHECK(strstr(d, "[featcov] m 0x1808 NV4097_SET_BEGIN_END real"));
    CHECK(strstr(d, "[featcov] v prim 0x5 real via=0x1808"));
    CHECK(!strstr(d, "v prim 0x0"));
    CHECK(strstr(d, "port=r1"));
    CHECK(strstr(d, "[featcov] m 0x08CC NV4097_SET_FOG_MODE unhandled"));
    CHECK(strstr(d, "[featcov] v fog_mode 0x2601 unhandled"));
    CHECK(strstr(d, "NV4097_SET_TEXTURE_FORMAT[3]") || strstr(d, "m 0x1A64 "));
    CHECK(strstr(d, "[featcov] v tex_fmt 0xA9 real"));
    CHECK(strstr(d, "[featcov] v tex_dim 0x12 real"));         /* 2D | cube */
    CHECK(strstr(d, "[featcov] v blend_eq 0x800A real") && strstr(d, "[featcov] v blend_eq 0x8006 real"));
    /* second dump: only what is new */
    rsx_featcov_method(RSX_FC_PORT_R1, 0x1808, 4);
    rsx_featcov_method(RSX_FC_PORT_R1, 0x1808, 5);
    d = dump();
    CHECK(strstr(d, "new_methods=0/"));
    CHECK(strstr(d, "[featcov] v prim 0x4 real"));
    CHECK(!strstr(d, "prim 0x5"));
    CHECK(!strstr(d, "[featcov] m "));
    d = dump();
    CHECK(strstr(d, "new_methods=0/") && strstr(d, "new_values=0/"));
}

static void test_methods_rpcs3(void)
{
    start("1");
    rsx_featcov_method(RSX_FC_PORT_RPCS3, 0x08CC, 0x2601);
    rsx_featcov_method(RSX_FC_PORT_RPCS3, 0x1EE4, 1);           /* POINT_PARAMS_ENABLE: stored, unread */
    rsx_featcov_method(RSX_FC_PORT_RPCS3, 0xC300, 0xB);         /* NV3089 SET_COLOR_FORMAT */
    const char* d = dump();
    CHECK(strstr(d, "port=rpcs3"));
    CHECK(strstr(d, "[featcov] m 0x08CC NV4097_SET_FOG_MODE real"));
    CHECK(strstr(d, "[featcov] m 0x1EE4 NV4097_SET_POINT_PARAMS_ENABLE ignored"));
    CHECK(strstr(d, "[featcov] m 0xC300 NV3089_SET_COLOR_FORMAT"));
}

static void test_shaders(void)
{
    start("1");
    /* R1 FP: MOV, DDX (no R1 translation), TEX with an inline constant, END */
    unsigned char fp[16 * 5];
    memset(fp, 0, sizeof fp);
    fp_put(fp + 0, 0x01u << 24);
    fp_put(fp + 16, 0x15u << 24);
    fp_put(fp + 20, 2u);                 /* src0 = constant: next 16 bytes are data */
    fp_put(fp + 32, 0x3Fu << 24);        /* constant data that looks like opcode 0x3F: must be skipped */
    fp_put(fp + 48, 0x17u << 24 | 1u);   /* TEX + END */
    fp_put(fp + 64, 0x16u << 24);        /* after END: not decoded */
    CHECK(rsx_featcov_fp_program(0x1000));
    rsx_featcov_fp_ucode(RSX_FC_PORT_R1, fp, sizeof fp);
    CHECK(!rsx_featcov_fp_program(0x1000));
    rsx_featcov_vp_d1(RSX_FC_PORT_R1, 0x01u << 22 | 0x13u << 27);   /* MOV + PSH */
    rsx_featcov_vp_d1(RSX_FC_PORT_R1, 0x01u << 22 | 0x13u << 27);
    rsx_featcov_shader(RSX_FC_PORT_RPCS3, RSX_FC_FP_TEXDIM, 4, 1);
    const char* d = dump();
    CHECK(strstr(d, "[featcov] v fp_op 0x1 real"));
    CHECK(strstr(d, "[featcov] v fp_op 0x15 unhandled"));
    CHECK(strstr(d, "[featcov] v fp_deriv 0x15 unhandled"));
    CHECK(strstr(d, "[featcov] v fp_op 0x17 real"));
    CHECK(!strstr(d, "fp_op 0x3F"));
    CHECK(!strstr(d, "fp_op 0x16"));
    CHECK(!strstr(d, "fp_prog"));
    CHECK(strstr(d, "[featcov] v vp_vec 0x1 real"));
    CHECK(strstr(d, "[featcov] v vp_sca 0x13 unhandled"));
    CHECK(strstr(d, "[featcov] v fp_texdim 0x4 real"));
}

/* exit dump: once only, and never stuck behind a held lock */
static void test_exit(void)
{
    start("1");
    rsx_featcov_method(RSX_FC_PORT_R1, 0x1808, 5);
    rsx_featcov_test_hold_lock(1);
    const double t0 = now_s();
    rsx_featcov_dump("exit");                       /* lock busy: gives up */
    const double waited = now_s() - t0;
    rsx_featcov_test_hold_lock(0);
    CHECK(waited < 0.05);
    const char* d = dump();                          /* "test" dump still has everything */
    CHECK(strstr(d, "dump #1 test") && strstr(d, "m 0x1808"));
    start("1");
    rsx_featcov_method(RSX_FC_PORT_R1, 0x1808, 5);
    rsx_featcov_dump("exit");
    rsx_featcov_dump("exit");                       /* quit path + atexit fallback */
    fflush(g_f);
    fseek(g_f, 0, SEEK_SET);
    const size_t n = fread(g_buf, 1, sizeof g_buf - 1, g_f);
    g_buf[n] = 0;
    fseek(g_f, 0, SEEK_END);
    CHECK(strstr(g_buf, "dump #1 exit") && !strstr(g_buf, "dump #2"));
}

static void test_disabled(void)
{
    start("");                                          /* unset: off by default */
    CHECK(!rsx_featcov_enabled());
    start("off");
    CHECK(!rsx_featcov_enabled());
    start("on");
    CHECK(rsx_featcov_enabled());
    start("0");
    rsx_featcov_method(RSX_FC_PORT_R1, 0x1808, 5);
    CHECK(g_rsx_featcov_flags[0x1808 >> 2] == RSX_FC_SEEN);   /* fast path from now on */
    CHECK(!rsx_featcov_enabled());
    CHECK(!rsx_featcov_fp_program(0x2000));
    rsx_featcov_vp_d1(RSX_FC_PORT_R1, 0x13u << 27);
    rsx_featcov_shader(RSX_FC_PORT_R1, RSX_FC_FP_OP, 0x15, 0);
    const char* d = dump();
    CHECK(!strstr(d, "[featcov]"));
}

/* Cost per method write: a FIFO-like stream over 48 registers (draw state,
 * texture formats alternating between two values on 4 units, BEGIN_END on/off). */
static void bench(void)
{
    enum { N = 1 << 12 };
    static unsigned offs[N], vals[N];
    unsigned s = 12345;
    for (int i = 0; i < N; i++) {
        s = s * 1103515245u + 12345u;
        const unsigned r = (s >> 16) % 8;
        if (r < 2) { offs[i] = 0x1808; vals[i] = (i & 1) ? 0 : 5; }
        else if (r < 4) { offs[i] = 0x1A04 + 0x20 * ((s >> 8) & 3); vals[i] = (i & 2) ? 0xA900 | 0x20 : 0x8500 | 0x20; }
        else { offs[i] = 0x1C00 + 4 * ((s >> 4) & 63); vals[i] = s; }      /* vertex data: untracked */
    }
    volatile unsigned sink = 0;
    const long long iters = 50ll * 1000 * 1000;
    double best_on = 1e9, best_base = 1e9;
    for (int rep = 0; rep < 3; rep++) {
        start("1");
        for (int i = 0; i < N; i++) rsx_featcov_method(RSX_FC_PORT_R1, offs[i], vals[i]);   /* warm */
        double t0 = now_s();
        for (long long i = 0; i < iters; i++) {
            const int j = (int)(i & (N - 1));
            rsx_featcov_method(RSX_FC_PORT_R1, offs[j], vals[j]);
            sink += vals[j];
        }
        const double on = (now_s() - t0) * 1e9 / (double)iters;
        t0 = now_s();
        for (long long i = 0; i < iters; i++) {
            const int j = (int)(i & (N - 1));
            sink += vals[j] ^ offs[j];
        }
        const double base = (now_s() - t0) * 1e9 / (double)iters;
        if (on < best_on) best_on = on;
        if (base < best_base) best_base = base;
    }
    const double cost = best_on - best_base;
    printf("featcov bench: %.2f ns/write with tracer, %.2f ns/write loop only -> %.2f ns per method write\n",
           best_on, best_base, cost);
    CHECK(cost < 10.0);           /* a FIFO walk spends far more per method than this */
    /* default (unset): every register settles to a plain byte check */
    double best_off = 1e9;
    for (int rep = 0; rep < 3; rep++) {
        start("");
        for (int i = 0; i < N; i++) rsx_featcov_method(RSX_FC_PORT_R1, offs[i], vals[i]);
        const double t0 = now_s();
        for (long long i = 0; i < iters; i++) {
            const int j = (int)(i & (N - 1));
            rsx_featcov_method(RSX_FC_PORT_R1, offs[j], vals[j]);
            sink += vals[j];
        }
        const double off = (now_s() - t0) * 1e9 / (double)iters;
        if (off < best_off) best_off = off;
    }
    printf("featcov bench: default off %.2f ns/write -> %.2f ns per method write (saves %.2f ns vs on)\n",
           best_off, best_off - best_base, best_on - best_off);
    CHECK(best_off <= best_on);
    (void)sink;
}

int main(void)
{
    test_methods_r1();
    test_methods_rpcs3();
    test_shaders();
    test_exit();
    test_disabled();
    bench();
    rsx_featcov_test_reset(NULL);
    env_set("RSX_FEATCOV", "0");
    if (g_f) fclose(g_f);
    remove(g_out_path);
    printf(g_fail ? "test_rsx_featcov: %d FAILED\n" : "test_rsx_featcov: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
