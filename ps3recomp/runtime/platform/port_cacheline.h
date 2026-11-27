/* port_cacheline.h -- cache-line alignment for data written by several threads.
 *
 * Perf final pass: the lv2 object tables (g_sys_mutexes, g_sys_conds, ...) are
 * arrays of 80-104 byte structs, so neighbouring objects shared cache lines and
 * two threads working on two different mutexes / semaphores bounced one line
 * between their cores. PS3_CACHELINE_ALIGN on the struct starts every element
 * on a line of its own. Layout only; no behaviour change. */
#ifndef PORT_CACHELINE_H
#define PORT_CACHELINE_H

#define PS3_CACHELINE 64
#ifndef PS3_CACHELINE_ALIGN
#  if defined(_MSC_VER)
#    define PS3_CACHELINE_ALIGN __declspec(align(64))
#  else
#    define PS3_CACHELINE_ALIGN __attribute__((aligned(64)))
#  endif
#endif

#endif /* PORT_CACHELINE_H */
