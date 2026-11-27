// port_frametime.h statistics: average, 1% / 0.1% lows from the histogram,
// stalls kept apart, worst-spike list, histogram rows, marker window, report
// text, and which old frametimes_*.txt files are pruned.
#include "port_frametime.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int g_fail = 0, g_n = 0;
static void check(bool ok, const char* what)
{
    g_n++;
    if (!ok) g_fail++;
    printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
}
static bool near(double a, double b, double eps) { return fabs(a - b) <= eps; }

int main()
{
    using namespace port_ft;
    static Stats s;
    memset(&s, 0, sizeof s);
    // 990 frames at 16.6 ms, 9 at 33.3, one at 100: 1% low = average of the slowest 10
    for (int i = 0; i < 990; i++) stats_add(&s, 16.6, i * 0.0166);
    for (int i = 0; i < 9; i++) stats_add(&s, 33.3, 20.0 + i);
    stats_add(&s, 100.0, 42.5);
    stats_add(&s, 1500.0, 50.0);   // a stall: not a frame
    stats_add(&s, -1.0, 51.0);     // ignored
    check(s.n == 1000, "frames counted, stall and negative left out");
    check(s.stalls == 1 && s.nstall == 1 && near(s.stall_ms, 1500.0, 1e-9), "stall counted apart");
    check(near(s.sum / s.n, (990 * 16.6 + 9 * 33.3 + 100.0) / 1000.0, 1e-9), "average");
    double l1, p99, l01, p999;
    stats_low(&s, 0.01, &l1, &p99);
    stats_low(&s, 0.001, &l01, &p999);
    check(near(l1, (9 * 33.3 + 100.0) / 10.0, 1e-6), "1% low = mean of the slowest 10");
    check(near(l01, 100.0, 1e-6), "0.1% low = the slowest frame");
    check(p99 > 33.2 && p99 <= 33.5, "p99 at the 33.3 ms bucket");
    check(near(p999, 100.0, 0.11), "p99.9 at the worst frame");
    check(s.ntop == TOP && near(s.max, 100.0, 1e-9), "top list full, max");
    {
        Spike t[TOP]; memcpy(t, s.top, sizeof t);
        sort_spikes(t, TOP, 0);
        check(near(t[0].ms, 100.0, 1e-4) && near(t[0].t, 42.5, 1e-4) && near(t[1].ms, 33.3, 1e-4) &&
              near(t[9].ms, 33.3, 1e-4) && near(t[10].ms, 16.6, 1e-4), "worst spikes in order with their time");
    }
    {
        uint64_t rows[HIST_ROWS]; hist_rows(&s, rows);
        uint64_t tot = 0; for (int r = 0; r < HIST_ROWS; r++) tot += rows[r];
        check(rows[2] == 990 && rows[5] == 9 && rows[8] == 1 && tot == 1000, "histogram rows (16.6 / 33.3 / 100 ms)");
    }
    {
        static Stats e; memset(&e, 0, sizeof e);
        stats_add(&e, 300.0, 1.0);   // beyond the fine buckets, still a frame
        stats_low(&e, 0.01, &l1, &p99);
        check(e.n == 1 && near(l1, 300.0, 1e-9) && near(p99, 300.0, 1e-9), "frame over 250 ms: last bucket, exact");
        stats_low(&s, 0.5, &l1, &p99);
        check(l1 > 16.6 && l1 < 20.0, "50% low mixes partial buckets");
    }
    {
        static char buf[64 * 1024];
        Info in{}; in.title = "Resistance 2"; in.exe = "R2.exe"; in.session = "20260101_120000";
        in.reasons = "exit"; in.marker_key = "Scroll Lock"; in.start = 0; in.duration_s = 60.0;
        const size_t n = format_report(&s, &in, "  MARKER #1 ...\n", buf, sizeof buf);
        check(n == strlen(buf) && strstr(buf, "1% low") && strstr(buf, "0.1% low") && strstr(buf, "worst 20 frames") &&
              strstr(buf, "longest stalls") && strstr(buf, "MARKER #1") && strstr(buf, "histogram"), "report text sections");
        char tiny[64];
        const size_t m = format_report(&s, &in, "", tiny, sizeof tiny);
        check(m < sizeof tiny && strlen(tiny) == m, "report truncates safely");
    }
    {
        static float ring[RING];
        for (int i = 0; i < 300; i++) ring[i] = 10.0f;   // 3 s of 10 ms frames
        ring[299] = 40.0f;
        char line[MARK_LEN];
        format_marker(3, 12.5, 0, ring, 300, line, sizeof line);
        check(strstr(line, "MARKER #3") && strstr(line, "197 frames") && strstr(line, "worst 40.00 ms") &&
              strstr(line, "1 over 25 ms"), "marker: last 2 s of frames");
        format_marker(1, 0.0, 0, ring, 0, line, sizeof line);
        check(strstr(line, "0 frames") != NULL, "marker: no frames yet");
    }
    {
        const char* names[] = { "frametimes_20260103_000000.txt", "frametimes_20260101_000000_ResistanceFoMGame.txt",
                                "frametimes_20260105_000000.txt", "frametimes_20260102_000000.txt", "frametimes_20260104_000000.txt" };
        int del[8];
        const int nd = prune_plan(names, 5, 3, del, 8);
        check(nd == 2 && ((del[0] == 1 && del[1] == 3) || (del[0] == 3 && del[1] == 1)), "prune: the two oldest of five");
        check(prune_plan(names, 5, 10, del, 8) == 0, "prune: under the limit");
    }
    printf("%d/%d passed\n", g_n - g_fail, g_n);
    return g_fail ? 1 : 0;
}
