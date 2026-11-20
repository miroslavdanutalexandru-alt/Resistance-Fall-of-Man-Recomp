/* port_latency.h: the PORT_LATENCY "Latency reduction" choice (NVIDIA Reflex / AMD Anti-Lag 2). */
#include <stdio.h>
#include "port_latency.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    const unsigned NV = PORT_LAT_VENDOR_NVIDIA, AMD = PORT_LAT_VENDOR_AMD, INTEL = 0x8086u;

    CHECK(port_latency_parse(NULL) == PORT_LAT_LEGACY, "unset");
    CHECK(port_latency_parse("") == PORT_LAT_LEGACY, "empty");
    CHECK(port_latency_parse("bogus") == PORT_LAT_LEGACY, "unknown text keeps the old behaviour");
    CHECK(port_latency_parse("OFF") == PORT_LAT_OFF && port_latency_parse("0") == PORT_LAT_OFF, "off");
    CHECK(port_latency_parse("Reflex") == PORT_LAT_REFLEX && port_latency_parse("nvidia") == PORT_LAT_REFLEX, "reflex");
    CHECK(port_latency_parse("antilag") == PORT_LAT_ANTILAG && port_latency_parse("AntiLag2") == PORT_LAT_ANTILAG, "antilag");
    CHECK(port_latency_parse("auto") == PORT_LAT_AUTO, "auto");
    CHECK(port_latency_parse("reflexx") == PORT_LAT_LEGACY && port_latency_parse("ref") == PORT_LAT_LEGACY, "no prefix match");

    /* legacy: the <game>_REFLEX value alone, clamped */
    CHECK(port_latency_reflex(PORT_LAT_LEGACY, 0, NV) == 0, "legacy off");
    CHECK(port_latency_reflex(PORT_LAT_LEGACY, 1, AMD) == 1, "legacy on, any vendor");
    CHECK(port_latency_reflex(PORT_LAT_LEGACY, 5, 0) == 2, "legacy clamps to boost");
    CHECK(port_latency_reflex(PORT_LAT_LEGACY, -3, 0) == 0, "legacy clamps negatives");
    CHECK(!port_latency_antilag(PORT_LAT_LEGACY, AMD), "legacy never Anti-Lag");

    /* reflex: on, boost when <game>_REFLEX=2 */
    CHECK(port_latency_reflex(PORT_LAT_REFLEX, 0, AMD) == 1, "reflex forces on");
    CHECK(port_latency_reflex(PORT_LAT_REFLEX, 2, NV) == 2, "reflex keeps boost");
    CHECK(!port_latency_antilag(PORT_LAT_REFLEX, AMD), "reflex: no Anti-Lag");

    /* off / antilag: no Reflex even when <game>_REFLEX asks */
    CHECK(port_latency_reflex(PORT_LAT_OFF, 2, NV) == 0 && !port_latency_antilag(PORT_LAT_OFF, AMD), "off");
    CHECK(port_latency_reflex(PORT_LAT_ANTILAG, 2, NV) == 0, "antilag: no Reflex");
    CHECK(port_latency_antilag(PORT_LAT_ANTILAG, NV) && port_latency_antilag(PORT_LAT_ANTILAG, 0), "antilag asks on any vendor");

    /* auto: by vendor; unknown vendor (0) tries both and lets the driver decide */
    CHECK(port_latency_reflex(PORT_LAT_AUTO, 0, NV) == 1 && !port_latency_antilag(PORT_LAT_AUTO, NV), "auto NVIDIA");
    CHECK(port_latency_reflex(PORT_LAT_AUTO, 2, NV) == 2, "auto NVIDIA boost");
    CHECK(port_latency_reflex(PORT_LAT_AUTO, 0, AMD) == 0 && port_latency_antilag(PORT_LAT_AUTO, AMD), "auto AMD");
    CHECK(port_latency_reflex(PORT_LAT_AUTO, 0, INTEL) == 0 && !port_latency_antilag(PORT_LAT_AUTO, INTEL), "auto Intel");
    CHECK(port_latency_reflex(PORT_LAT_AUTO, 0, 0) == 1 && port_latency_antilag(PORT_LAT_AUTO, 0), "auto unknown vendor");

    /* render ids (R2/R3 VKR2PcExtras lat_render): never ahead of the game, never made up */
    CHECK(port_latency_render_id(1, 5, 6, 4) == 5, "queued frame");
    CHECK(port_latency_render_id(0, 0, 6, 5) == 6, "nothing queued: frame in progress");
    CHECK(port_latency_render_id(0, 0, 5, 5) == 0, "extra present without a game tick: no id");
    CHECK(port_latency_render_id(1, 5, 6, 5) == 0, "stale queued frame: no id");
    CHECK(port_latency_render_id(1, 9, 6, 4) == 6, "clamped to the started frame");
    CHECK(port_latency_render_id(0, 0, 0, 0) == 0, "before the first game frame");

    /* model: game ticks (sim end -> queue, sim start) and RSX presents, incl. double presents
     * (load screens); every present id must be started by the game and strictly increasing */
    {
        unsigned long long sim = 0, last = 0, ring[8];
        unsigned head = 0, tail = 0, presents = 0, marked = 0;
        const char* seq = "TPPPTPTPPTPTTPPPPTP"; /* T = game tick, P = RSX present */
        for (const char* c = seq; *c; c++) {
            if (*c == 'T') {
                if (sim && head - tail < 8) ring[head++ % 8] = sim;
                sim++;
            } else {
                const int q = tail != head;
                const unsigned long long id = port_latency_render_id(q, q ? ring[tail % 8] : 0, sim, last);
                presents++;
                if (id) {
                    marked++;
                    CHECK(id <= sim, "present id %llu ahead of game frame %llu", id, sim);
                    CHECK(id > last, "present id %llu not after %llu", id, last);
                    last = id;
                }
                while (tail != head && ring[tail % 8] <= last) tail++;
            }
        }
        CHECK(last <= sim, "render ids ended ahead of the game (%llu > %llu)", last, sim);
        CHECK(marked > 0 && marked < presents, "extra presents skipped (%u of %u marked)", marked, presents);
        /* steady state after a burst of extra presents: next tick + present is marked with the new frame */
        sim++; ring[head++ % 8] = sim - 1;
        {
            const int q = tail != head;
            const unsigned long long id = port_latency_render_id(q, q ? ring[tail % 8] : 0, sim, last);
            CHECK(id == sim - 1 || id == sim, "recovers after extra presents (id %llu, sim %llu)", id, sim);
        }
    }

    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("port_latency: all checks passed\n");
    return 0;
}
