/* spu_sched.h -- PORT_SPU_SCHED: how long a blocked SPU channel read sleeps
 * on ch_wait_cv before it re-checks its predicate (spu_ch_wait, spu_channels.c).
 *
 * A wait on a lost-reservation event (rdch SPU_RdEventStat with 0x400 in the
 * mask after a GETLLAR -- the SPURS/ig job queue and lock pattern) is woken by
 * spu_coh_notify_write, which every coherent writer calls: PPU stores to a
 * reserved line, SPU PUT/PUTLLC, cellSync/cellGcm HLE. A writer that bypasses
 * that path (a host memcpy into guest memory, a syscall writing guest memory)
 * is only noticed when the waiter polls the line (spu_resv_lost_poll), and the
 * waiter used to poll once per 10 ms CV timeout. PORT_SPU_SCHED (default on)
 * re-polls those waits every PORT_SPU_SCHED_MS (default 1, 1..10), so a missed
 * wake costs <= 1 ms instead of <= 10 ms. Purely a latency change: the
 * predicate is re-checked either way and a wait still ends only when it holds.
 * Other waits (mailbox, signal, other events) keep the 10 ms timeout -- their
 * producers all call spu_ch_wake.
 *
 * Shared runtime: the Resistance 1/2 copies of ps3recomp have the same wait. */
#pragma once
#include <stdint.h>
#include "../../../project/port_native_summary.h"   /* port_native_parse_bool: the shared switch rules */

enum { SPU_SCHED_SLOW_MS = 10, SPU_SCHED_LR_EVENT = 0x400u };

/* One switch parser in every module: port_native_parse_bool (1/on/true/yes,
 * 0/off/false/no; anything else = def). */
static inline int spu_sched_parse_bool(const char* e, int def) { return port_native_parse_bool(e, def); }

/* PORT_SPU_SCHED_MS: 1..10, anything else = 1. */
static inline int spu_sched_parse_ms(const char* e)
{
    int v = 0, n = 0;
    if (!e) return 1;
    for (; *e >= '0' && *e <= '9' && n < 3; e++, n++) v = v * 10 + (*e - '0');
    if (*e || !n || v < 1 || v > SPU_SCHED_SLOW_MS) return 1;
    return v;
}

/* CV timeout for one sleep of spu_ch_wait; is_event_read: the read is SPU_RdEventStat. */
static inline unsigned spu_sched_wait_ms(int on, int ms, int is_event_read,
                                         uint32_t event_mask, int resv_valid)
{
    if (on && is_event_read && (event_mask & SPU_SCHED_LR_EVENT) && resv_valid) return (unsigned)ms;
    return SPU_SCHED_SLOW_MS;
}
