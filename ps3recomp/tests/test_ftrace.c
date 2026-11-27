/* ps3_ftrace.h trigger-only mode (PS3_FTRACE=99999:...): the recorder stays
 * disarmed (PS3_FT_ON() false, no buffer) until ps3_ft_trigger, records the
 * window, writes it and disarms again. Also: triggers during a recording or a
 * write are refused, a re-armed window holds only its own spans, and one
 * thread hammering spans while another triggers and dumps keeps every file
 * consistent. */
#include "ps3emu/ps3_ftrace.h"
#include <process.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail++; } } while (0)

static int file_has(const char* path, const char* needle)
{
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char line[256]; int hit = 0;
    while (!hit && fgets(line, sizeof line, f)) hit = strstr(line, needle) != NULL;
    fclose(f);
    return hit;
}

/* Span lines (kind who a b t0 t1): count them, and those starting before the
 * header's start_us or a span of kind <bad_kind>. */
static int scan(const char* path, unsigned* spans, unsigned* early, unsigned bad_kind, unsigned* bad)
{
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char line[256]; double start_us = 0;
    *spans = *early = 0; if (bad) *bad = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned k, who, a, b; double t0, t1;
        if (sscanf(line, "# ps3_ftrace start_us=%lf", &start_us) == 1) continue;
        if (sscanf(line, "%u %u %x %x %lf %lf", &k, &who, &a, &b, &t0, &t1) == 6) {
            ++*spans;
            if (t0 + 0.001 < start_us) ++*early;
            if (bad && k == bad_kind) ++*bad;
        }
    }
    fclose(f);
    return 1;
}

static volatile long g_stop;
static volatile long g_spans;
static uint64_t g_stale;                       /* like t_ft_busy0: taken before any window, reused */
static unsigned __stdcall hammer(void* arg)
{
    (void)arg;
    while (!g_stop) {
        if (PS3_FT_ON()) {
            const uint64_t t = ps3_ft_now();
            ps3_ft_span(PS3_FT_SPU_WAIT, 7, 1, 2, t, t + 100);
            ps3_ft_span(PS3_FT_SPU_BUSY, 7, 3, 4, g_stale, t);   /* stale start: must never be written */
            _InterlockedIncrement(&g_spans);
        }
    }
    return 0;
}

int main(void)
{
    const char* out = "test_ftrace_req1.txt";
    const char* out2 = "test_ftrace_req2.txt";
    const char* out3 = "test_ftrace_req3.txt";
    remove(out); remove(out2); remove(out3);
    _putenv_s("PS3_FTRACE", "99999:6:test_ftrace_unused.txt");
    _putenv_s("PS3_FTRACE_CAP", "200000");

    /* disarmed until triggered */
    CHECK(PS3_FT_ON() == 0);
    CHECK(g_ps3_ft.cfg == 1);
    CHECK(g_ps3_ft.buf == NULL);
    CHECK(PS3_FT_ON() == 0);
    ps3_ft_name(1, "main");
    CHECK(strcmp(g_ps3_ft.names[1], "main") == 0);
    ps3_ft_dump();                       /* never triggered: writes nothing */
    CHECK(g_ps3_ft.dumped == 0);

    /* a window; a trigger while it records is refused and keeps its path */
    const uint64_t before = ps3_ft_now();
    Sleep(2);
    CHECK(ps3_ft_trigger(0.05, out) == 1);
    CHECK(PS3_FT_ON() == 1);
    CHECK(g_ps3_ft.buf != NULL);
    const uint64_t t0 = ps3_ft_now();
    ps3_ft_span(PS3_FT_SYSCALL, 1, 0x10, 0x20, t0, t0 + 1000000);
    ps3_ft_span(PS3_FT_SPU_BUSY, 1, 0x30, 0x40, before, t0);   /* began before the arm: stale */
    CHECK(g_ps3_ft.n == 1);
    CHECK(ps3_ft_trigger(1.0, out2) == 0);
    CHECK(strcmp(g_ps3_ft.path, out) == 0 && PS3_FT_ON() == 1);

    Sleep(80);
    const uint64_t t1 = ps3_ft_now();
    ps3_ft_span(PS3_FT_SYSCALL, 1, 0x11, 0x21, t1, t1 + 1000);   /* past the window: writes it */
    CHECK(PS3_FT_ON() == 0);
    CHECK(g_ps3_ft.dumped == 1);
    CHECK(file_has(out, "# ps3_ftrace start_us="));
    CHECK(file_has(out, "spans=1 dropped=0"));
    CHECK(file_has(out, "T 1 main"));
    CHECK(file_has(out, "1 1 10 20 "));
    CHECK(!file_has(out, "4 1 30 40 "));
    ps3_ft_dump();                       /* already written: no second write */
    CHECK(g_ps3_ft.on == 3);

    /* a trigger while the window is being written (on == 4) is refused */
    g_ps3_ft.on = 4;
    CHECK(ps3_ft_trigger(1.0, out2) == 0);
    CHECK(strcmp(g_ps3_ft.path, out) == 0 && g_ps3_ft.n == 1);
    CHECK(PS3_FT_ON() == 0);
    g_ps3_ft.on = 3;

    /* re-arm: a claimed-but-unfilled slot of the old window (kind left set)
     * must not reappear; only the new window's spans are written */
    g_ps3_ft.buf[1] = g_ps3_ft.buf[0];   /* stale record past n... */
    g_ps3_ft.n = 2;                      /* ...as if claimed by the old window */
    CHECK(ps3_ft_trigger(1.0, out2) == 1);
    CHECK(PS3_FT_ON() == 1 && g_ps3_ft.n == 0);
    CHECK(g_ps3_ft.buf[0].kind == 0 && g_ps3_ft.buf[1].kind == 0);
    __atomic_fetch_add(&g_ps3_ft.n, 1, __ATOMIC_RELAXED);   /* claimed, never filled */
    const uint64_t t2 = ps3_ft_now();
    ps3_ft_span(PS3_FT_SYSCALL, 2, 0x12, 0x22, t2, t2 + 1000000);
    ps3_ft_dump();
    CHECK(PS3_FT_ON() == 0);
    unsigned spans = 0, early = 0;
    CHECK(scan(out2, &spans, &early, 0, NULL));
    CHECK(spans == 1 && early == 0);
    CHECK(file_has(out2, "1 2 12 22 "));
    CHECK(!file_has(out2, "1 1 10 20 "));

    /* two threads: one hammers spans (and a stale-start span) while main
     * triggers windows and dumps them, sometimes before they end */
    g_stale = ps3_ft_now();
    Sleep(2);
    HANDLE th = (HANDLE)_beginthreadex(NULL, 0, hammer, NULL, 0, NULL);
    char path[64];
    int windows = 0;
    for (int w = 0; w < 6; w++) {
        snprintf(path, sizeof path, "test_ftrace_mt%d.txt", w);
        remove(path);
        if (!ps3_ft_trigger(0.02, path)) { Sleep(5); continue; }
        ++windows;
        Sleep(w & 1 ? 40 : 10);           /* odd: the hammer ends it; even: main dumps it early */
        ps3_ft_dump();
        for (int i = 0; i < 5000 && __atomic_load_n(&g_ps3_ft.on, __ATOMIC_ACQUIRE) != 3; i++) Sleep(1);   /* the hammer may still be writing (on == 4) */
        unsigned bad = 0;
        CHECK(scan(path, &spans, &early, PS3_FT_SPU_BUSY, &bad));
        CHECK(early == 0 && bad == 0);
        CHECK(g_ps3_ft.on == 3);
    }
    g_stop = 1;
    WaitForSingleObject(th, INFINITE); CloseHandle(th);
    CHECK(windows == 6);
    CHECK(g_spans > 0);

    remove(out); remove(out2); remove(out3);
    for (int w = 0; w < 6; w++) { snprintf(path, sizeof path, "test_ftrace_mt%d.txt", w); remove(path); }

    printf(g_fail ? "test_ftrace: %d failures\n" : "test_ftrace: OK\n", g_fail);
    return g_fail != 0;
}
