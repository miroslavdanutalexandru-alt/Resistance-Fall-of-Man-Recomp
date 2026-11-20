// port_ini_migrate.h: settings.ini keys renamed for the cross-port names (old -> new, old line kept).
#include "port_ini_migrate.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

// what a launcher's load_settings ends up with for `key` (the last matching line wins there)
static std::string loaded(std::vector<std::string> lines, const char* key)
{
    port_ini_migrate_lines(lines);
    std::string v = "<none>", k, x;
    for (auto& l : lines) if (port_ini_split(l, k, x) && k == key) v = x;
    return v;
}

int main()
{
    {   // old key only: copied right after the old line, the old line stays
        std::vector<std::string> l = { "fps=60", "lang=fr", "fov=90" };
        CHECK(port_ini_migrate_lines(l) == 1);
        CHECK(l.size() == 4 && l[1] == "lang=fr" && l[2] == "language=fr" && l[3] == "fov=90");
        CHECK(port_ini_migrate_lines(l) == 0);                       // second load: nothing more to write
        CHECK(l.size() == 4);
    }
    {   // every R1 rename, empty values included
        std::vector<std::string> l = { "pad_p1=2", "dlss_preset=K", "lod_bias=", "precompile_threads=8", "sensy=0.5",
                                       "ads=1.25", "focus=run", "saves=portable", "hud169=1" };
        CHECK(port_ini_migrate_lines(l) == 9);
        CHECK(loaded(l, "p1pad") == "2" && loaded(l, "dlsspreset") == "K" && loaded(l, "lodbias") == "");
        CHECK(loaded(l, "shaderthreads") == "8" && loaded(l, "msensy") == "0.5" && loaded(l, "adssens") == "1.25");
        CHECK(loaded(l, "background") == "run" && loaded(l, "savedir") == "portable" && loaded(l, "hudfix") == "1");
    }
    {   // new key only: untouched
        std::vector<std::string> l = { "language=de", "p1pad=1" };
        CHECK(port_ini_migrate_lines(l) == 0);
        CHECK(l.size() == 2 && l[0] == "language=de");
    }
    {   // both: the new key wins, nothing written
        std::vector<std::string> l = { "lang=fr", "language=de" };
        CHECK(port_ini_migrate_lines(l) == 0);
        CHECK(loaded(l, "language") == "de");
        std::vector<std::string> m = { "language=", "lang=fr" };     // new key with an empty value still counts
        CHECK(port_ini_migrate_lines(m) == 0 && loaded(m, "language") == "");
    }
    {   // malformed lines are left alone and never taken for a key
        std::vector<std::string> l = { "lang", "=fr", "", "  # comment", "langx=it", "xlang=it", "language2=it" };
        CHECK(port_ini_migrate_lines(l) == 0);
        CHECK(l.size() == 7);
        // blanks and a stray CR around an old key: the value is trimmed
        std::vector<std::string> m = { "  lang \t= es \r", "lang=it" };
        CHECK(port_ini_migrate_lines(m) == 1);
        CHECK(m.size() == 3 && m[1] == "language=es");               // the first old line is used
        // a value containing '=' is kept whole
        std::vector<std::string> n = { "saves=a=b" };
        CHECK(port_ini_migrate_lines(n) == 1 && n[1] == "savedir=a=b");
        std::vector<std::string> e;
        CHECK(port_ini_migrate_lines(e) == 0 && e.empty());
    }
    {   // the table: no key renamed twice, no new name that is also an old one
        for (const auto& a : k_port_ini_renames) {
            CHECK(port_ini_new_key(a.new_key) == nullptr);
            int n = 0;
            for (const auto& b : k_port_ini_renames) n += !strcmp(a.old_key, b.old_key) + !strcmp(a.new_key, b.new_key);
            CHECK(n == 2);
        }
        CHECK(port_ini_new_key("lang") && !strcmp(port_ini_new_key("lang"), "language"));
        CHECK(port_ini_new_key("fps") == nullptr);
    }
    std::printf(g_fail ? "ini_migrate: %d FAILED\n" : "ini_migrate: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
