/* port_mem.h -- PORT_LARGE_PAGES: the guest's main memory in large pages (2 MB on x64), the same switch and the
 * same [native] entry in all three Resistance ports.
 *
 *   PORT_LARGE_PAGES=on   (default off, port_native_parse_bool rules)
 *
 * Large pages cut TLB misses for the game's scattered main-memory accesses. Windows hands them out only to an
 * account with the "Lock pages in memory" right (SeLockMemoryPrivilege, README), commits them up front (never
 * paged out) and cannot change the protection of part of one, so a port uses them only for guest ranges whose
 * page protection never changes; when they cannot be had the normal pages are used and the [native] line says
 * why ("large-pages(PORT_LARGE_PAGES,overridden: no privilege)").
 * Shared file: keep the copies in ResistancePort, Resistance2Port and Resistance3Port identical. */
#pragma once
#include "port_native_summary.h"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

typedef struct port_large_pages {
    int requested;        /* PORT_LARGE_PAGES */
    int active;           /* the range really is in large pages */
    size_t bytes;         /* committed in large pages */
    size_t page;          /* GetLargePageMinimum() */
    const char* reason;   /* why not active (NULL when active or not requested) */
} port_large_pages;

static inline int port_large_pages_env(void) { return port_native_parse_bool(getenv("PORT_LARGE_PAGES"), 0); }

/* The [native] entry: "large-pages(PORT_LARGE_PAGES)" in the on/off list as in effect, with the reason when the
 * switch asked for them and they could not be used. */
static inline void port_large_pages_summary(const port_large_pages* st)
{
    char sw[160];
    if (st->requested && !st->active && st->reason) snprintf(sw, sizeof sw, "PORT_LARGE_PAGES,overridden: %s", st->reason);
    else snprintf(sw, sizeof sw, "PORT_LARGE_PAGES");
    const port_native_feature f = { "large-pages", sw, 0, 0 };
    const int on = st->active;
    port_native_summary_state(&f, &on, 1);
}

#ifdef _WIN32
/* SeLockMemoryPrivilege for this process: NULL when held, else the reason. */
static inline const char* port_mem_lock_privilege(void)
{
    HANDLE tok = NULL;
    TOKEN_PRIVILEGES tp;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return "no privilege";
    memset(&tp, 0, sizeof tp);
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueA(NULL, "SeLockMemoryPrivilege", &tp.Privileges[0].Luid)) { CloseHandle(tok); return "no privilege"; }
    const BOOL ok = AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL);
    const DWORD err = GetLastError();   /* ERROR_NOT_ALL_ASSIGNED: the account does not have the right */
    CloseHandle(tok);
    return (ok && err == ERROR_SUCCESS) ? NULL : "no privilege";
}

/* Reserves `total` bytes in one place with [off, off + len) committed read/write in large pages and the rest
 * reserved as normal pages around it (up to three allocations: a commit that crosses a boundary must be split
 * per allocation, see the callers). Returns the base, or NULL with st->reason set when large pages cannot be had
 * (nothing is left allocated; the caller reserves normally). off and len must be multiples of the large-page size. */
static inline void* port_mem_reserve_large_window(size_t total, size_t off, size_t len, port_large_pages* st)
{
    st->active = 0; st->bytes = 0; st->reason = NULL;
    st->page = GetLargePageMinimum();
    if (!st->page) { st->reason = "not supported"; return NULL; }
    if (!len || off % st->page || len % st->page || off + len > total) { st->reason = "range not aligned"; return NULL; }
    if ((st->reason = port_mem_lock_privilege()) != NULL) return NULL;
    for (int attempt = 0; attempt < 4; attempt++) {
        /* find a free range of the whole size, then take it in pieces (another thread may take it between) */
        char* probe = (char*)VirtualAlloc(NULL, total + st->page, MEM_RESERVE, PAGE_NOACCESS);
        if (!probe) { st->reason = "no address space"; return NULL; }
        char* base = (char*)(((uintptr_t)probe + st->page - 1) & ~(uintptr_t)(st->page - 1));
        VirtualFree(probe, 0, MEM_RELEASE);
        char* lp = (char*)VirtualAlloc(base + off, len, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE);
        if (!lp) {
            const DWORD e = GetLastError();
            if (e == ERROR_INVALID_ADDRESS) continue;   /* raced: try another range */
            st->reason = (e == ERROR_NO_SYSTEM_RESOURCES || e == ERROR_NOT_ENOUGH_MEMORY || e == ERROR_COMMITMENT_LIMIT)
                             ? "not enough contiguous memory" : (e == ERROR_PRIVILEGE_NOT_HELD ? "no privilege" : "allocation failed");
            return NULL;
        }
        void* lo = off ? VirtualAlloc(base, off, MEM_RESERVE, PAGE_READWRITE) : base;
        void* hi = off + len < total ? VirtualAlloc(base + off + len, total - off - len, MEM_RESERVE, PAGE_READWRITE) : base;
        if (!lo || !hi) {
            if (lo && off) VirtualFree(lo, 0, MEM_RELEASE);
            if (hi && off + len < total) VirtualFree(hi, 0, MEM_RELEASE);
            VirtualFree(lp, 0, MEM_RELEASE);
            continue;
        }
        st->active = 1; st->bytes = len;
        return base;
    }
    st->reason = "no address space";
    return NULL;
}
#endif
