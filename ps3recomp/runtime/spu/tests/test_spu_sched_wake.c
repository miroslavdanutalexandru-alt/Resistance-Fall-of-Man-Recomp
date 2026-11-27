/* test_spu_sched_wake.c -- PORT_SPU_SCHED offline harness (spu_sched.h).
 *
 * Reproduces the wait spu_ch_wait does for a job blocked on a lost-reservation
 * event (rdch SPU_RdEventStat, mask 0x400, after GETLLAR -- the R3 ig job
 * queue/lock pop in img 403/406, LS 0xF248): the consumer snapshots a 128-byte
 * line, then sleeps on an SRW condition variable with the spu_sched_wait_ms
 * timeout and memcmps the line after every wake or timeout. A producer writes
 * the line (a) and calls WakeAllConditionVariable, like spu_coh_notify_write,
 * or (b) does not -- a writer outside the coherence path. Prints the wake
 * latency for PORT_SPU_SCHED off (10 ms poll) and on (1 ms poll).
 *
 * Build (VS dev prompt):
 *   clang-cl /nologo /O2 test_spu_sched_wake.c
 */
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../spu_sched.h"
#pragma comment(lib, "winmm.lib")
static SRWLOCK g_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_cv = CONDITION_VARIABLE_INIT;
static __declspec(align(128)) volatile uint8_t g_line[128];
static volatile LONG g_notify;
static LARGE_INTEGER g_qf, g_write_t;
static int g_on;

static DWORD WINAPI producer(LPVOID p)
{
    (void)p;
    Sleep(3);   /* let the consumer reach the CV sleep */
    QueryPerformanceCounter(&g_write_t);
    g_line[0]++;
    MemoryBarrier();
    if (g_notify) { AcquireSRWLockExclusive(&g_lock); ReleaseSRWLockExclusive(&g_lock); WakeAllConditionVariable(&g_cv); }
    return 0;
}

/* One wait; returns wake latency in microseconds. */
static double one_wait(int notify)
{
    uint8_t snap[128];
    memcpy(snap, (const void*)g_line, 128);
    g_notify = notify;
    HANDLE h = CreateThread(NULL, 0, producer, NULL, 0, NULL);
    int resv_valid = 1;
    for (;;) {
        AcquireSRWLockExclusive(&g_lock);
        if (memcmp((const void*)g_line, snap, 128) != 0) resv_valid = 0;
        if (!resv_valid) { ReleaseSRWLockExclusive(&g_lock); break; }
        SleepConditionVariableSRW(&g_cv, &g_lock, spu_sched_wait_ms(g_on, 1, 1, SPU_SCHED_LR_EVENT, resv_valid), 0);
        ReleaseSRWLockExclusive(&g_lock);
    }
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    WaitForSingleObject(h, INFINITE); CloseHandle(h);
    return (double)(t.QuadPart - g_write_t.QuadPart) * 1e6 / (double)g_qf.QuadPart;
}

static int cmpd(const void* a, const void* b) { double x = *(const double*)a, y = *(const double*)b; return x < y ? -1 : x > y; }

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

int main(void)
{
    CHECK(spu_sched_parse_ms(NULL) == 1 && spu_sched_parse_ms("4") == 4 && spu_sched_parse_ms("10") == 10);
    CHECK(spu_sched_parse_ms("0") == 1 && spu_sched_parse_ms("11") == 1 && spu_sched_parse_ms("x") == 1);
    /* only armed lost-reservation event reads get the short poll */
    CHECK(spu_sched_wait_ms(1, 1, 1, 0x400u, 1) == 1);
    CHECK(spu_sched_wait_ms(1, 1, 1, 0x402u, 1) == 1);
    CHECK(spu_sched_wait_ms(0, 1, 1, 0x400u, 1) == SPU_SCHED_SLOW_MS);
    CHECK(spu_sched_wait_ms(1, 1, 0, 0x400u, 1) == SPU_SCHED_SLOW_MS);   /* mailbox/signal */
    CHECK(spu_sched_wait_ms(1, 1, 1, 0x002u, 1) == SPU_SCHED_SLOW_MS);   /* stall-notify only */
    CHECK(spu_sched_wait_ms(1, 1, 1, 0x400u, 0) == SPU_SCHED_SLOW_MS);   /* no reservation */

    timeBeginPeriod(1);   /* as project/main.cpp */
    QueryPerformanceFrequency(&g_qf);
    enum { N = 40 };
    double med[2][2];
    for (g_on = 0; g_on <= 1; g_on++)
        for (int notify = 1; notify >= 0; notify--) {
            double v[N];
            for (int i = 0; i < N; i++) v[i] = one_wait(notify);
            qsort(v, N, sizeof v[0], cmpd);
            med[g_on][notify] = v[N / 2];
            printf("PORT_SPU_SCHED=%d %-26s wake latency: median %7.0f us  p90 %7.0f us  max %7.0f us\n",
                   g_on, notify ? "notified writer:" : "writer bypassing notify:", v[N / 2], v[N * 9 / 10], v[N - 1]);
        }
    timeEndPeriod(1);
    CHECK(med[1][0] < 3000.0);              /* missed wake caught within ~1 ms poll */
    CHECK(med[1][0] < med[0][0]);
    CHECK(med[1][1] < 3000.0 && med[0][1] < 3000.0);   /* notified wakes unaffected */
    printf("%s (%d failures)\n", g_fail ? "FAILED" : "all spu_sched tests passed", g_fail);
    return g_fail != 0;
}
