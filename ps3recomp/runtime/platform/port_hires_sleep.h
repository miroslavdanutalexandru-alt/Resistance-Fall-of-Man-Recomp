/* port_hires_sleep.h -- 1 ms naps on a high-resolution waitable timer (perf pass 9).
 *
 * Sleep(1) on Windows returns at the next scheduler tick: 1-2 ms with the 1 ms
 * timer period the port requests, up to 15.6 ms without it. The polling loops
 * that use it (interrupt-thread wait, SPU mailbox-full wait, timed sys_mutex
 * lock, SPURS job-guard / queue-push waits) only mean "about a millisecond",
 * so their wake latency was 0-1 ms worse than intended on every lap.
 * port_sleep_1ms() waits on a per-thread CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
 * timer (Windows 10 1803+), which wakes within ~0.1 ms of the due time.
 *
 * PORT_HIRES_SLEEP=0 restores plain Sleep(1). Read once per translation unit
 * (the launcher sets it before start). Falls back to Sleep(1) when the timer
 * cannot be created (older Windows). */
#ifndef PORT_HIRES_SLEEP_H
#define PORT_HIRES_SLEEP_H

#ifdef _WIN32
#include <windows.h>
#include <stdlib.h>

#if defined(__cplusplus)
#define PORT_HIRES_TLS thread_local
#else
#define PORT_HIRES_TLS _Thread_local
#endif

/* 0 = off, 1 = on; the -1 sentinel is replaced on first use. */
static inline int port_hires_sleep_on(void)
{
    static volatile int s_on = -1;
    int v = s_on;
    if (v < 0) {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
        const char* e = getenv("PORT_HIRES_SLEEP");
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
        s_on = v = (e && *e == '0') ? 0 : 1;
    }
    return v;
}

/* Sleep about `us` microseconds (intended for < 2 ms naps). */
static inline void port_hires_sleep_us(unsigned us)
{
    if (port_hires_sleep_on()) {
        static PORT_HIRES_TLS HANDLE t_tmr;
        static PORT_HIRES_TLS int t_tried;
        if (!t_tried) {
            t_tried = 1;
            t_tmr = CreateWaitableTimerExW(NULL, NULL, 0x00000002u /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                                           0x1F0003u /* TIMER_ALL_ACCESS */);
        }
        if (t_tmr) {
            LARGE_INTEGER d;
            d.QuadPart = -(LONGLONG)us * 10;   /* 100 ns units, relative */
            if (SetWaitableTimer(t_tmr, &d, 0, NULL, NULL, FALSE)) {
                WaitForSingleObject(t_tmr, INFINITE);
                return;
            }
        }
    }
    Sleep((us + 999u) / 1000u);
}

static inline void port_sleep_1ms(void) { port_hires_sleep_us(1000u); }

#else /* !_WIN32 */
#include <time.h>
static inline void port_hires_sleep_us(unsigned us)
{
    struct timespec ts = { (time_t)(us / 1000000u), (long)(us % 1000000u) * 1000L };
    nanosleep(&ts, 0);
}
static inline void port_sleep_1ms(void) { port_hires_sleep_us(1000u); }
#endif

#endif /* PORT_HIRES_SLEEP_H */
