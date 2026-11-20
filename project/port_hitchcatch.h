/*
 * port_hitchcatch -- PORT_HITCHCATCH=<ms>: what the key threads were doing during a frame hitch.
 *
 * Off by default. With PORT_HITCHCATCH unset (or 0) the only cost is the one
 * branch in port_hitchcatch_frame() below, once per game frame; no thread is
 * started and nothing is allocated.
 *
 * On: a sampler thread wakes every PORT_HITCHCATCH_PERIOD_US (2000) and takes
 * RIP + RSP/RBP + the top 2 KB of the stack of a FEW threads only (the game's
 * frame-loop thread, the RSX threads and the SPU job threads; never the audio
 * mixer) into a small per-thread ring (~224 ms). When a frame is longer than
 * <ms>, the frame hook asks the sampler to copy that frame's window out of the
 * rings, and a low-priority writer thread unwinds, symbolizes (DbgHelp, lazily)
 * and maps host code to lifted guest functions, then logs
 *   [hitch] 72 ms frame 12345: main: func_006483A8 41%, ...; rsx: ...
 * and appends a detailed block to logs\hitchcatch.txt.
 *
 * Shared, identical in the R1 / R2 / R3 ports (project/port_hitchcatch.{h,cpp}).
 * Idea and the suspend/copy/unwind-after-resume discipline after the inFAMOUS 2
 * port's ps3_hitchcatch.c (SamiEnazi/infamous2-port; toolkit SamiEnazi/ps3recomp
 * branch infamous2, runtime/diag; MIT, Copyright (c) 2026 Ned Heller / sp00nznet).
 * See port_hitchcatch.cpp.
 */
#ifndef PORT_HITCHCATCH_H
#define PORT_HITCHCATCH_H

#ifdef __cplusplus
extern "C" {
#endif

/* 0: env not read yet, 1: on, -1: off. Only the frame hook writes it. */
extern volatile int g_port_hc_state;

void port_hitchcatch_frame_slow(long frame_no, int discard);

/* Once per game frame, on the game's frame-loop thread (it becomes "main").
 * discard = 1: this frame contained a pause / was not a hitch. */
static __inline void port_hitchcatch_frame(long frame_no, int discard)
{
    if (g_port_hc_state >= 0) port_hitchcatch_frame_slow(frame_no, discard);
}

/* Optional: sample one more host thread under `role` (<= 15 chars). Any
 * thread may call it, at any time; a no-op while hitchcatch is off. */
void port_hitchcatch_register(unsigned long tid, const char* role);

typedef struct port_hc_stats {
    unsigned long long passes, suspends, skipped_idle, failed;
    unsigned long long susp_ns_total, susp_ns_max, pass_ns_total, pass_ns_max;
    unsigned long long sampler_cycles;   /* the sampler thread's own CPU cycles */
    unsigned long long hitches, dumps_written, dumps_rate_limited, dumps_busy, discarded;
    int threads_live;
} port_hc_stats;
/* 0 while off. */
int port_hitchcatch_stats(port_hc_stats* out);

#ifdef __cplusplus
}
#endif
#endif
