/* ps3_log.h -- gate for high-volume diagnostic logging.
 *
 * Chatty per-wait / per-round logs are off unless PS3_VERBOSE=1 (the
 * Resistance port always redirects stderr to logs\run.log, so the old
 * "verbose when redirected" default logged them in every normal run). On a
 * live console they also hurt: conhost flushes stall the emitting threads, and the ~20k
 * [WAIT] lines/minute of an LBP intro run showed up as harsh ~1 Hz hitches
 * in user-visible runs while file-redirected runs played smoothly.
 */
#ifndef PS3_LOG_H
#define PS3_LOG_H

#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <io.h>
#define PS3_ISATTY_STDERR() _isatty(_fileno(stderr))
#else
#include <unistd.h>
#define PS3_ISATTY_STDERR() isatty(fileno(stderr))
#endif

#ifdef __cplusplus
static inline int ps3_diag_enabled(void)
#else
static __inline int ps3_diag_enabled(void)
#endif
{
    /* Resistance port: PORT_DIAG=1 turns on the debug diagnostics (periodic
     * statistics lines, per-event traces). Off by default so release runs stay
     * quiet and the statistics' timers and string formatting are skipped. */
    static int d = -1;
    if (d < 0) {
        const char* e = getenv("PORT_DIAG");
        d = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return d;
}

#ifdef __cplusplus
static inline int ps3_log_verbose(void)
#else
static __inline int ps3_log_verbose(void)
#endif
{
    /* PS3_VERBOSE, when set, decides. Without it: quiet (the old default was
     * verbose whenever stderr was redirected).
     *
     * Why verbose is opt-in: the per-event lines are
     * emitted from every guest thread through one FILE lock, and Windows locks
     * are not fair, so at the ~6k lines/s a SPURS audio loop produces a thread
     * can starve on the lock for seconds. Tokyo Jungle presents four frames in
     * 40 s with the log discarded and one with it redirected to a file -- the
     * logging changes what the title does. */
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("PS3_VERBOSE");
        if (e) v = (e[0] && e[0] != '0') ? 1 : 0;
        else   v = ps3_diag_enabled();
    }
    return v;
}

#endif /* PS3_LOG_H */
