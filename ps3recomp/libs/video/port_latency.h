/* port_latency.h: the "Latency reduction" choice shared by the three ports (header-only, C and C++).
 *
 * PORT_LATENCY   unset / ""  legacy: the <game>_REFLEX switch alone decides (0 off, 1 On, 2 On + Boost)
 *                off         no NVIDIA Reflex and no AMD Anti-Lag 2 (DLSS Frame Generation still forces Reflex on)
 *                reflex      NVIDIA Reflex (level = <game>_REFLEX if it is 2 = Boost, else 1)
 *                antilag     AMD Anti-Lag 2 (Radeon RX 5000 or newer, driver 24.x+); nothing on other GPUs
 *                auto        Reflex on an NVIDIA GPU, Anti-Lag 2 on an AMD GPU, nothing elsewhere
 * Neither is ever a frame limiter: the port's own frame limiter stays in charge. */
#ifndef PORT_LATENCY_H
#define PORT_LATENCY_H

#define PORT_LAT_LEGACY  0
#define PORT_LAT_OFF     1
#define PORT_LAT_REFLEX  2
#define PORT_LAT_ANTILAG 3
#define PORT_LAT_AUTO    4

#define PORT_LAT_VENDOR_NVIDIA 0x10DEu
#define PORT_LAT_VENDOR_AMD    0x1002u

static inline int port_latency_ieq(const char* a, const char* b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return *a == *b;
}

/* PORT_LATENCY text -> PORT_LAT_*; unknown text = legacy */
static inline int port_latency_parse(const char* s)
{
    if (!s || !*s) return PORT_LAT_LEGACY;
    if (port_latency_ieq(s, "off") || port_latency_ieq(s, "0") || port_latency_ieq(s, "none")) return PORT_LAT_OFF;
    if (port_latency_ieq(s, "reflex") || port_latency_ieq(s, "nvidia")) return PORT_LAT_REFLEX;
    if (port_latency_ieq(s, "antilag") || port_latency_ieq(s, "antilag2") || port_latency_ieq(s, "amd")) return PORT_LAT_ANTILAG;
    if (port_latency_ieq(s, "auto")) return PORT_LAT_AUTO;
    return PORT_LAT_LEGACY;
}

/* Reflex level to request (0 off, 1 On, 2 On + Boost). reflex_env = the <game>_REFLEX value,
 * vendor = PCI vendor id of the GPU (0 = not known yet: auto then asks for Reflex and lets the driver decide). */
static inline int port_latency_reflex(int mode, int reflex_env, unsigned vendor)
{
    const int lvl = reflex_env >= 2 ? 2 : 1;
    if (reflex_env < 0) reflex_env = 0;
    if (reflex_env > 2) reflex_env = 2;
    switch (mode) {
    case PORT_LAT_LEGACY:  return reflex_env;
    case PORT_LAT_REFLEX:  return lvl;
    case PORT_LAT_AUTO:    return (vendor == PORT_LAT_VENDOR_NVIDIA || vendor == 0) ? lvl : 0;
    default:               return 0;
    }
}

/* 1 = try AMD Anti-Lag 2 (the driver/extension check still decides) */
static inline int port_latency_antilag(int mode, unsigned vendor)
{
    if (mode == PORT_LAT_ANTILAG) return 1;
    if (mode == PORT_LAT_AUTO) return vendor == PORT_LAT_VENDOR_AMD || vendor == 0;
    return 0;
}

/* Frame id for the RSX present that is starting (Reflex markers / Anti-Lag 2 frameIndex).
 * queued = oldest finished game frame not presented yet (have_queued 0 = none), sim = game frame
 * started most recently (SIMULATION_START sent), last = id of the previous present (0 = none).
 * Never invents an id: a present the game has not started a new frame for (load/boot screens
 * present without a game tick) gets 0 = no markers, so PRESENT/RENDERSUBMIT never run ahead of
 * SIMULATION_START/INPUT for the same id. */
static inline unsigned long long port_latency_render_id(int have_queued, unsigned long long queued,
                                                         unsigned long long sim, unsigned long long last)
{
    unsigned long long id = have_queued ? queued : sim;
    if (id > sim) id = sim;
    return id > last ? id : 0;
}

#endif