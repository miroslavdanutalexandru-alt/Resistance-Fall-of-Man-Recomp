/*
 * port_dbghelp_lock.h -- one process-wide lock for DbgHelp, shared by the
 * crash / hang reporter (port_crash.h) and PORT_HITCHCATCH (port_hitchcatch.cpp).
 *
 * DbgHelp is single-threaded: every call into it (SymInitialize, SymFromAddr,
 * SymLoadModuleEx, StackWalk64, MiniDumpWriteDump, ...) must hold this lock.
 * Identical in the R1 / R2 / R3 ports.
 *
 * g_port_dbghelp_report: set while a crash / hang report runs (left set after
 * a crash). Optional users (hitchcatch) then skip symbolising; the reporter
 * takes the lock before it suspends any thread, so it never waits for a
 * thread it has stopped itself.
 */
#ifndef PORT_DBGHELP_LOCK_H
#define PORT_DBGHELP_LOCK_H
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* C++17 inline variables: one definition for the whole program */
inline SRWLOCK       g_port_dbghelp_lock = SRWLOCK_INIT;
inline volatile LONG g_port_dbghelp_owner;    /* thread id holding the lock, 0 = none */
inline volatile LONG g_port_dbghelp_report;   /* a crash / hang report is in progress */

inline void port_dbghelp_lock()
{
    AcquireSRWLockExclusive(&g_port_dbghelp_lock);
    InterlockedExchange(&g_port_dbghelp_owner, (LONG)GetCurrentThreadId());
}

/* Up to `ms` milliseconds; 0 if the lock is still held after that. */
inline int port_dbghelp_trylock(DWORD ms)
{
    const ULONGLONG t0 = GetTickCount64();
    for (;;) {
        if (TryAcquireSRWLockExclusive(&g_port_dbghelp_lock)) {
            InterlockedExchange(&g_port_dbghelp_owner, (LONG)GetCurrentThreadId());
            return 1;
        }
        if (GetTickCount64() - t0 >= ms) return 0;
        Sleep(1);
    }
}

inline void port_dbghelp_unlock()
{
    InterlockedExchange(&g_port_dbghelp_owner, 0);
    ReleaseSRWLockExclusive(&g_port_dbghelp_lock);
}

#endif
#endif
