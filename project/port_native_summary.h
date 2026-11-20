/* port_native_summary.h -- one start-up line that lists the native (hand-written PC) replacements of
 * guest code and the native renderer fast paths, the same format in all three Resistance ports:
 *
 *   [native] on: hot-ppu(R2_HOT_NATIVE) spu-jobs(R2_NATIVE_JOBS) | off: edge-zlib(PORT_NATIVE_EDGE_ZLIB)
 *
 * Each entry names the switch that controls it (port_native_parse_bool rules: unrecognised = the default given
 * here, which must match the module's own default; modules that resolve aliases or config files print
 * their own line with port_native_summary_state once the effective state is known). The modules keep their own detailed [tag] lines.
 * Shared file: keep the copies in ResistancePort, Resistance2Port and Resistance3Port identical. */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct port_native_feature {
    const char* name;   /* short id shown in the line */
    const char* env;    /* the switch */
    int def;            /* the module's default when the switch is unset or unrecognised */
    int level;          /* 1 = level switch (port_native_parse_level), on when non-zero; 0 = boolean */
} port_native_feature;

/* Boolean switch values (also the R2/R3 RSX bridge's penv_bool): 1/on/true/yes and 0/off/false/no, any case,
 * surrounding blanks ignored. Anything else ("2", "0x1", "abc"), unset or empty = def. Modules whose switch
 * is listed here parse it with this function, so the [native] line and the module always agree. */
static inline int port_native_word(const char* e, char* w, size_t cap)
{
    size_t b = 0, n = 0, len;
    if (!e) return 0;
    while (e[b] == ' ' || e[b] == '\t' || e[b] == '\r' || e[b] == '\n') b++;
    len = strlen(e + b);
    while (len && (e[b + len - 1] == ' ' || e[b + len - 1] == '\t' || e[b + len - 1] == '\r' || e[b + len - 1] == '\n')) len--;
    if (!len || len >= cap) return 0;
    for (; n < len; n++) { const char c = e[b + n]; w[n] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c); }
    w[n] = 0;
    return 1;
}

static inline int port_native_parse_bool(const char* e, int def)
{
    static const char* const k_on[] = { "1", "on", "true", "yes" };
    static const char* const k_off[] = { "0", "off", "false", "no" };
    char w[8];
    if (!port_native_word(e, w, sizeof w)) return def;
    for (int i = 0; i < 4; i++) {
        if (!strcmp(w, k_on[i])) return 1;
        if (!strcmp(w, k_off[i])) return 0;
    }
    return def;
}

/* Level switches (0 = off, 1..N = how much runs native, e.g. R3_NATIVE_DOF_JOB): the boolean words, or a
 * plain decimal 0..9999; anything else = def. On in the [native] line when the level is non-zero. */
static inline int port_native_parse_level(const char* e, int def)
{
    char w[8];
    int v = 0;
    const int b = port_native_parse_bool(e, -1);
    if (b >= 0) return b;
    if (!port_native_word(e, w, sizeof w) || strlen(w) > 4) return def;
    for (const char* p = w; *p; p++) {
        if (*p < '0' || *p > '9') return def;
        v = v * 10 + (*p - '0');
    }
    return v;
}
static inline int port_native_on(const port_native_feature* f)
{
    return f->level ? port_native_parse_level(getenv(f->env), f->def) != 0 : port_native_parse_bool(getenv(f->env), f->def);
}

/* Writes the line into buf (also used by the tests); returns buf. state: NULL = read the switches, else the
 * effective on/off of each entry, for modules that resolve aliases or config files themselves. */
static inline const char* port_native_format_state(const port_native_feature* f, const int* state, int n,
                                                   char* buf, size_t cap)
{
    size_t o = 0;
    buf[0] = 0;
    for (int pass = 1; pass >= 0; pass--) {
        int any = 0;
        o += (size_t)snprintf(buf + o, o < cap ? cap - o : 0, pass ? "[native] on:" : " | off:");
        if (o >= cap) break;
        for (int i = 0; i < n; i++) {
            if ((state ? (state[i] != 0) : port_native_on(&f[i])) != pass) continue;
            o += (size_t)snprintf(buf + o, o < cap ? cap - o : 0, " %s(%s)", f[i].name, f[i].env);
            if (o >= cap) break;
            any = 1;
        }
        if (o >= cap) break;
        if (!any) o += (size_t)snprintf(buf + o, cap - o, " -");
        if (o >= cap) break;
    }
    if (o >= cap && cap) buf[cap - 1] = 0;
    return buf;
}

static inline const char* port_native_format(const port_native_feature* f, int n, char* buf, size_t cap)
{
    return port_native_format_state(f, NULL, n, buf, cap);
}

static inline void port_native_summary(const port_native_feature* f, int n)
{
    char buf[1024];
    fprintf(stderr, "%s\n", port_native_format(f, n, buf, sizeof buf));
}

static inline void port_native_summary_state(const port_native_feature* f, const int* state, int n)
{
    char buf[1024];
    fprintf(stderr, "%s\n", port_native_format_state(f, state, n, buf, sizeof buf));
}
