/* port_hires_sleep.h: the 1 ms nap used by the runtime's polling waits (perf pass 9).
 * It must actually sleep (not return at once: the callers bound their waits by lap
 * count, e.g. 250 laps = 250 ms), must come back, and must work on every thread
 * (the timer handle is per thread). Prints the mean nap so a regression to
 * Sleep(1) granularity is visible in the log. */
#include "../../runtime/platform/port_hires_sleep.h"
#include <stdio.h>
#include <time.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static double now_us(void)
{
#ifdef _WIN32
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1e6 / (double)f.QuadPart;
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
#endif
}

static double mean_nap_us(int n)
{
    double t0 = now_us();
    for (int i = 0; i < n; i++) port_sleep_1ms();
    return (now_us() - t0) / n;
}

#ifdef _WIN32
static double s_thread_mean;
static DWORD WINAPI worker(LPVOID p) { (void)p; s_thread_mean = mean_nap_us(20); return 0; }
#endif

int main(void)
{
#ifdef _WIN32
    CHECK(port_hires_sleep_on() == 1);        /* default on (PORT_HIRES_SLEEP unset) */
#endif
    const double m = mean_nap_us(50);
    printf("port_sleep_1ms: mean %.0f us over 50 naps\n", m);
    CHECK(m >= 900.0);                         /* really sleeps ~1 ms */
    CHECK(m < 100000.0);                       /* and comes back */
    const double t0 = now_us();
    port_hires_sleep_us(200);
    const double s = now_us() - t0;
    CHECK(s >= 150.0 && s < 100000.0);
#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    CHECK(h != NULL);
    if (h) { WaitForSingleObject(h, INFINITE); CloseHandle(h); }
    printf("port_sleep_1ms on a second thread: mean %.0f us\n", s_thread_mean);
    CHECK(s_thread_mean >= 900.0 && s_thread_mean < 100000.0);
#endif
    if (!g_fail) printf("test_hires_sleep: OK\n");
    return g_fail;
}
