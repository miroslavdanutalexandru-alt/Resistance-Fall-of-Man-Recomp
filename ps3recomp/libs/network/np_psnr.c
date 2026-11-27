/*
 * ps3recomp - the connection to a psnr server. See np_psnr.h.
 */
#include "np_psnr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#ifndef _WIN32
#  include <unistd.h>   /* getpid */
#endif

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
static SRWLOCK s_lock = SRWLOCK_INIT;
#  define LOCK()   AcquireSRWLockExclusive(&s_lock)
#  define UNLOCK() ReleaseSRWLockExclusive(&s_lock)
#else
#  include <pthread.h>
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
#  define LOCK()   pthread_mutex_lock(&s_lock)
#  define UNLOCK() pthread_mutex_unlock(&s_lock)
#endif

/* The fake username from sceNp.c. */
extern const char* np_fake_username(void);

#define PENDING_MAX 64

static psnr_client* s_client;
static uint32_t     s_user_id;   /* ours, from HELLO: our own member entry needs no punch */
static int          s_tried;
static struct { uint32_t req; np_psnr_reply_fn fn; void* user; } s_pending[PENDING_MAX];
static void (*s_push_fn)(const psnr_msg*);

static void set_env(const char* k, const char* v)
{
#ifdef _WIN32
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}

/* PSN online IDs: 3-16 letters, digits, '-' and '_', starting with a letter.
 * Copies the allowed characters of `in`; 1 if the result is a valid ID. */
static int clean_online_id(const char* in, char out[17])
{
    int n = 0;
    for (; in && *in && n < 16; in++)
        if (isalnum((unsigned char)*in) || *in == '-' || *in == '_')
            out[n++] = *in;
    out[n] = 0;
    return n >= 3 && isalpha((unsigned char)out[0]);
}

static char s_id[17];
static char s_key[17 + 1 + 16];   /* what the identity derives from: see np_psnr_identity_key */
static int  s_id_state;     /* 0 unresolved, 1 a name applies, 2 none (offline) */

/* Adjective + animal, at most 8 + 8 letters, so every pair is a valid ID.
 * 128 x 128 names: two players picking the same one is rare, and when it happens
 * the saved salt still keeps them apart (np_psnr_identity_key). */
static const char* const k_adj[] = {
    "Amazing", "Brave", "Clever", "Daring", "Eager", "Fearless", "Fierce", "Gentle",
    "Happy", "Jolly", "Lucky", "Mighty", "Nimble", "Quick", "Quiet", "Rapid",
    "Sneaky", "Swift", "Witty", "Bold", "Calm", "Cosmic", "Crafty", "Dizzy",
    "Electric", "Exciting", "Fancy", "Fuzzy", "Grumpy", "Hasty", "Lively", "Mellow",
    "Plucky", "Proud", "Rowdy", "Sleepy", "Speedy", "Steady", "Sunny", "Zesty",
    "Agile", "Ancient", "Atomic", "Awesome", "Bouncy", "Breezy", "Bright", "Brisk",
    "Bubbly", "Cheeky", "Cheery", "Chilly", "Classic", "Cozy", "Curious", "Dapper",
    "Dashing", "Eternal", "Fabulous", "Feisty", "Fluffy", "Frosty", "Funky",
    "Giant", "Glossy", "Golden", "Groovy", "Hardy", "Heroic", "Humble", "Icy",
    "Jazzy", "Jumpy", "Keen", "Kind", "Lunar", "Loyal", "Magic", "Merry", "Mystic",
    "Noble", "Polar", "Peppy", "Quirky", "Radiant", "Royal", "Rustic", "Sassy",
    "Shiny", "Silent", "Silly", "Smooth", "Snappy", "Solar", "Spicy", "Stellar",
    "Stormy", "Sturdy", "Super", "Tiny", "Turbo", "Valiant", "Vivid", "Wacky",
    "Wild", "Wise", "Zany", "Zippy", "Nifty", "Brainy", "Chipper", "Comfy", "Crisp",
    "Dandy", "Dreamy", "Epic", "Fresh", "Gleeful", "Grand", "Jaunty", "Lofty",
    "Misty", "Perky", "Rugged", "Spunky", "Swanky", "Trusty", "Upbeat" };
static const char* const k_animal[] = {
    "Hippo", "Lizard", "Badger", "Beaver", "Bison", "Cobra", "Condor", "Coyote",
    "Falcon", "Ferret", "Gecko", "Heron", "Hyena", "Iguana", "Jackal", "Koala",
    "Lemur", "Lynx", "Marmot", "Moose", "Narwhal", "Ocelot", "Otter", "Panda",
    "Panther", "Parrot", "Pelican", "Penguin", "Puffin", "Raccoon", "Raven",
    "Rhino", "Salmon", "Shark", "Sloth", "Tapir", "Tiger", "Toucan", "Walrus",
    "Wombat", "Alpaca", "Anteater", "Axolotl", "Baboon", "Bear", "Beetle", "Bobcat",
    "Buffalo", "Camel", "Caribou", "Cheetah", "Chipmunk", "Cougar", "Crab", "Crane",
    "Cricket", "Dingo", "Dolphin", "Donkey", "Duck", "Eagle", "Elk", "Emu", "Finch",
    "Flamingo", "Fox", "Frog", "Gazelle", "Gerbil", "Gibbon", "Giraffe", "Goat",
    "Goose", "Gopher", "Gorilla", "Hamster", "Hare", "Hawk", "Hedgehog", "Ibex",
    "Impala", "Jaguar", "Kiwi", "Kitten", "Lark", "Lemming", "Leopard", "Lion",
    "Llama", "Lobster", "Macaw", "Magpie", "Mammoth", "Manatee", "Meerkat", "Mink",
    "Mole", "Mongoose", "Monkey", "Moth", "Mouse", "Newt", "Octopus", "Orca",
    "Osprey", "Ostrich", "Owl", "Oyster", "Pigeon", "Platypus", "Pony", "Possum",
    "Quail", "Rabbit", "Reindeer", "Robin", "Seal", "Shrimp", "Skunk", "Snail",
    "Sparrow", "Squid", "Stork", "Swan", "Turtle", "Viper", "Vulture", "Weasel" };

/* 64 random-enough bits: the clock, the process and a counter, mixed. Not for
 * secrets -- only to keep players' names and salts apart. */
static uint64_t random64(void)
{
    static uint64_t n;
    uint64_t x = (uint64_t)time(NULL) ^ ((uint64_t)clock() << 20) ^ (uint64_t)(uintptr_t)&n
               ^ (++n * 0x9E3779B97F4A7C15ull);
#ifdef _WIN32
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    x ^= ((uint64_t)GetCurrentProcessId() << 32) ^ GetTickCount64() ^ ((uint64_t)qpc.QuadPart << 7);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    x ^= ((uint64_t)getpid() << 32) ^ (uint64_t)ts.tv_nsec ^ ((uint64_t)ts.tv_sec << 30);
#endif
    if (!x) x = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 4; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; }   /* xorshift64 */
    return x;
}

void np_psnr_random_id(char out[17])
{
    uint64_t x = random64();
    snprintf(out, 17, "%s%s", k_adj[x % (sizeof k_adj / sizeof *k_adj)],
             k_animal[(x >> 32) % (sizeof k_animal / sizeof *k_animal)]);
}

/* The random name is saved, so a player keeps one name -- and the account ID
 * and console identity derived from it -- across launches. The file holds the
 * name on its first line and a random salt (16 hex digits) on the second. */
static const char* id_file(void)
{
    const char* f = getenv("PS3_NP_ID_FILE");
    return f && *f ? f : "online_id.txt";
}

/* Drop a UTF-8 BOM and surrounding whitespace, in place. */
static char* trim(char* s)
{
    if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
        s += 3;
    while (*s && isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    return s;
}

/* 1: a valid name, with *salt set if the file has one (*has_salt);
 * 0: no file or no name; -1: a name that is not a valid ID (reported). */
static int load_saved_id(char out[17], uint64_t* salt, int* has_salt)
{
    char l1[128] = {0}, l2[128] = {0};
    *has_salt = 0;
    FILE* f = fopen(id_file(), "r");
    if (!f) return 0;
    if (!fgets(l1, sizeof l1, f)) l1[0] = 0;
    if (!fgets(l2, sizeof l2, f)) l2[0] = 0;
    fclose(f);
    char* name = trim(l1);
    if (!*name) return 0;
    if (!clean_online_id(name, out) || strcmp(out, name) != 0) {
        printf("[psnr] %s: \"%s\" is not a valid online ID (3-16 letters, digits, - or _, "
               "starting with a letter); using a random name this run, the file is left as it is\n",
               id_file(), name);
        return -1;
    }
    char* s = trim(l2);
    int hex = strlen(s) == 16;
    for (int i = 0; hex && i < 16; i++) hex = isxdigit((unsigned char)s[i]) != 0;
    if (hex) { *salt = strtoull(s, NULL, 16); *has_salt = 1; }
    return 1;
}

static void save_id(const char* id, uint64_t salt)
{
    FILE* f = fopen(id_file(), "w");
    if (!f) { printf("[psnr] could not save the player name to %s\n", id_file()); return; }
    fprintf(f, "%s\n%016llx\n", id, (unsigned long long)salt);
    fclose(f);
}

void np_psnr_setup(const char* username, const char* server)
{
    /* Before anything reads them: the runtime's getenv cache keeps the first
     * answer it sees. */
    if (server && *server) {
        set_env("PS3_NET_ONLINE", "1");
        set_env("PSNR_SERVER", server);
    }
    /* The first valid one of: --username, PS3_NP_ONLINE_ID, and -- online
     * only -- the name saved by an earlier run, else a new random one
     * (np_psnr_random_id), saved for next time with a random salt. Never the OS login name: it
     * would show the player's account name to everyone online. Online means
     * PS3_NET_ONLINE, with or without a psnr server: a title with its own
     * backend (Medius) needs a name of its own too. */
    int online = getenv("PS3_NET_ONLINE") != NULL;
    const char* named[2] = { username, getenv("PS3_NP_ONLINE_ID") };
    int found = 0;
    for (int i = 0; i < 2 && !found; i++) {
        if (!named[i] || !*named[i]) continue;
        found = clean_online_id(named[i], s_id);
        if (!found)
            printf("[psnr] \"%s\" is not a valid online ID (3-16 letters, digits, - or _, "
                   "starting with a letter)\n", named[i]);
    }
    if (!found) {
        if (!online) { s_id_state = 2; return; }   /* offline: "PS3Player" */
        uint64_t salt = 0;
        int has_salt = 0;
        int r = load_saved_id(s_id, &salt, &has_salt);
        if (r == 1) {
            printf("[psnr] player name from %s\n", id_file());
            if (!has_salt) { salt = random64(); save_id(s_id, salt); }
        } else {
            np_psnr_random_id(s_id);
            salt = random64();
            if (r == 0) save_id(s_id, salt);   /* an invalid name is left for the player to fix */
        }
        snprintf(s_key, sizeof s_key, "%s#%016llx", s_id, (unsigned long long)salt);
    } else {
        snprintf(s_key, sizeof s_key, "%s", s_id);
    }
    s_id_state = 1;
    printf("[psnr] player \"%s\"%s\n", s_id, online ? "" : " (offline)");
}

const char* np_psnr_identity(void)
{
    if (!s_id_state) np_psnr_setup(NULL, NULL);
    return s_id_state == 1 ? s_id : NULL;
}

const char* np_psnr_identity_key(void)
{
    if (!s_id_state) np_psnr_setup(NULL, NULL);
    return s_id_state == 1 ? s_key : NULL;
}

int np_psnr_enabled(void)
{
    return getenv("PS3_NET_ONLINE") && getenv("PSNR_SERVER");
}

/* PORT_ONLINE (port_online.c): the RPCN account name from online.ini, which
 * the server's ticket carries, so the title shows the same name. NULL = none. */
const char* (*g_np_online_id_hook)(void) = NULL;

const char* np_psnr_online_id(void)
{
    const char* hooked = g_np_online_id_hook ? g_np_online_id_hook() : NULL;
    if (hooked && *hooked) return hooked;
    const char* id = np_psnr_identity();
    return id ? id : np_fake_username();
}

uint16_t np_psnr_p2p_port(void)
{
    const char* p = getenv("PS3_NET_P2P_PORT");
    return (uint16_t)(p ? atoi(p) : 3658);
}

int np_psnr_connected(void)
{
    return s_client != NULL;
}

int np_psnr_connect(const char* comm_id)
{
    char host[256];
    uint16_t port = 36100;
    uint32_t user_id = 0;
    uint8_t ip[4] = {0};

    if (!np_psnr_enabled()) return -1;
    LOCK();
    if (s_client || s_tried) {
        int r = s_client ? 0 : -1;
        UNLOCK();
        return r;
    }
    /* ponytail: one attempt per process; retry on the next context start if a
     * server that comes up late turns out to matter. */
    s_tried = 1;
    snprintf(host, sizeof(host), "%s", getenv("PSNR_SERVER"));
    char* colon = strrchr(host, ':');
    if (colon) { *colon = 0; port = (uint16_t)atoi(colon + 1); }

    s_client = psnr_connect(host, port, comm_id, np_psnr_online_id(),
                            np_psnr_p2p_port(), &user_id, ip);
    s_user_id = user_id;
    UNLOCK();
    if (s_client)
        printf("[psnr] connected to %s:%u as \"%s\" (%s), user %u, seen from %u.%u.%u.%u, p2p port %u\n",
               host, port, np_psnr_online_id(), comm_id, user_id, ip[0], ip[1], ip[2], ip[3],
               np_psnr_p2p_port());
    else if (psnr_connect_error() == PSNR_E_NAME_TAKEN)
        printf("[psnr] %s:%u: another player there is already \"%s\" -- pick another "
               "with --username. NP stays offline\n", host, port, np_psnr_online_id());
    else
        printf("[psnr] could not reach %s:%u -- NP stays offline\n", host, port);
    return s_client ? 0 : -1;
}

uint32_t np_psnr_request(uint8_t type, const void* body, uint32_t len,
                         np_psnr_reply_fn fn, void* user)
{
    uint32_t req = 0;
    LOCK();
    if (s_client) {
        int slot = -1;
        for (int i = 0; i < PENDING_MAX; i++)
            if (!s_pending[i].req) { slot = i; break; }
        if (slot >= 0 && (req = psnr_send(s_client, type, body, len)) != 0) {
            s_pending[slot].req = req;
            s_pending[slot].fn = fn;
            s_pending[slot].user = user;
        }
    }
    UNLOCK();
    return req;
}

void np_psnr_on_push(void (*fn)(const psnr_msg*))
{
    s_push_fn = fn;
}

static void (*s_tick_fn)(void);
void np_psnr_on_tick(void (*fn)(void))
{
    s_tick_fn = fn;
}

/* ---- NAT traversal and the relay, on the title's P2P socket ---- */

#define PUNCHES      5      /* per peer */
#define PUNCH_MS     200
#define PROBE_MS     500    /* until psnr answers */
#define REPROBE_MS   20000  /* after: routers forget idle UDP mappings */
#define RELAY_AFTER  3000   /* punched, nothing heard directly: use the relay */
#define MAX_PEERS    8      /* ponytail: a room has at most 4 */
#define STREAM_TIMEOUT_MS 12000

static np_psnr_p2p_send_fn s_p2p_send;
static np_psnr_stream_sink_fn s_stream_sink;
static int      s_probed;
static uint64_t s_next_probe;

/* Everyone Matching2 told us about. relay_stream comes from psnr's ROUTE
 * pushes; relay_udp is ours to decide: punched, never heard from directly. */
static struct peer {
    uint32_t user;
    uint8_t  ip[4];
    uint16_t port;
    int      punches_left;
    uint64_t next_punch, punched_at, heard_at;
    int      relay_stream, relay_udp;
} s_peers[MAX_PEERS];

static uint64_t nat_now_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

/* Callers hold LOCK. */
static struct peer* peer_by_user(uint32_t user)
{
    for (int i = 0; i < MAX_PEERS; i++)
        if (s_peers[i].user == user) return &s_peers[i];
    return NULL;
}

static struct peer* peer_at(const uint8_t ip[4], uint16_t port)
{
    for (int i = 0; i < MAX_PEERS; i++)
        if (s_peers[i].user && s_peers[i].port == port && !memcmp(s_peers[i].ip, ip, 4))
            return &s_peers[i];
    return NULL;
}

void np_psnr_set_p2p_sender(np_psnr_p2p_send_fn fn)
{
    s_p2p_send = fn;
    s_probed = 0;
    s_next_probe = 0;
}

void np_psnr_set_stream_sink(np_psnr_stream_sink_fn fn)
{
    s_stream_sink = fn;
}

int np_psnr_p2p_filter(void* buf, uint32_t* len, uint8_t from_ip[4], uint16_t* from_port)
{
    uint8_t ip[4];
    uint16_t port;
    uint32_t from;
    const uint8_t* payload;
    size_t plen;
    int drop = 1;

    if (!psnr_is_control(buf, *len)) {
        LOCK();
        struct peer* p = peer_at(from_ip, *from_port);
        if (p) p->heard_at = nat_now_ms();   /* the direct path works */
        UNLOCK();
        return 0;
    }
    if (psnr_relay_unwrap(buf, *len, &from, &payload, &plen)) {
        /* The peer's datagram, through the relay: the title gets it as if
         * it came from the peer's usual address. */
        LOCK();
        struct peer* p = peer_by_user(from);
        if (p) {
            memmove(buf, payload, plen);
            *len = (uint32_t)plen;
            memcpy(from_ip, p->ip, 4);
            *from_port = p->port;
            drop = 0;
        }
        UNLOCK();
        return drop;
    }
    if (psnr_probe_reply(buf, *len, ip, &port)) {
        if (!s_probed) {
            s_probed = 1;
            s_next_probe = nat_now_ms() + REPROBE_MS;
            printf("[psnr] P2P socket seen at %u.%u.%u.%u:%u\n", ip[0], ip[1], ip[2], ip[3], port);
        }
    } else {   /* a punch */
        LOCK();
        struct peer* p = peer_at(from_ip, *from_port);
        if (p) p->heard_at = nat_now_ms();
        UNLOCK();
    }
    return 1;
}

void np_psnr_punch(uint32_t user, const uint8_t ip[4], uint16_t port)
{
    if (user == s_user_id || !port) return;
    LOCK();
    struct peer* p = peer_by_user(user);
    if (!p) p = peer_by_user(0);
    if (p) {
        int known = p->user == user && p->port == port && !memcmp(p->ip, ip, 4);
        if (!known) {
            int relay_stream = p->user == user ? p->relay_stream : 0;
            memset(p, 0, sizeof(*p));
            p->user = user;
            memcpy(p->ip, ip, 4);
            p->port = port;
            p->relay_stream = relay_stream;
        }
        if (!p->punches_left) {
            p->punches_left = PUNCHES;
            p->next_punch = 0;
        }
    }
    UNLOCK();
}

int np_psnr_p2p_route(const void* buf, uint32_t len, const uint8_t ip[4], uint16_t port)
{
    uint8_t stack[1536], *pkt = stack, sip[4];
    uint16_t sport;
    uint32_t to = 0;
    LOCK();
    struct peer* p = peer_at(ip, port);
    if (p && p->relay_udp) to = p->user;
    UNLOCK();
    if (!to || !s_client || !s_p2p_send) return 0;
    if (len + 17 > sizeof(stack) && !(pkt = (uint8_t*)malloc(len + 17))) return 0;
    size_t n = psnr_relay_wrap(s_client, to, buf, len, pkt, len + 17);
    psnr_server_addr(s_client, sip, &sport);
    s_p2p_send(pkt, (uint32_t)n, sip, sport);
    if (pkt != stack) free(pkt);
    return 1;
}

int64_t np_psnr_stream_connect(const uint8_t ip[4], uint16_t port)
{
    uint32_t to = 0;
    LOCK();
    struct peer* p = peer_at(ip, port);
    if (p && p->relay_stream) to = p->user;
    UNLOCK();
    if (!to || !s_client) return -2;
    int64_t fd = psnr_stream_connect(s_client, to, port, STREAM_TIMEOUT_MS);
    printf("[psnr] stream to %u.%u.%u.%u:%u through the relay: %s\n",
           ip[0], ip[1], ip[2], ip[3], port, fd >= 0 ? "open" : "failed");
    return fd;
}

/* Pushes np_psnr handles itself: ROUTE and STREAM_OFFER. 1 if it took it. */
static int nat_push(const psnr_msg* m)
{
    if (m->type == PSNR_ROUTE && m->len >= 13) {
        uint32_t user = psnr_get32(m->data + 8);
        int relay = (m->data[12] & PSNR_ROUTE_STREAM_RELAY) != 0;
        LOCK();
        struct peer* p = peer_by_user(user);
        if (!p && (p = peer_by_user(0))) {
            memset(p, 0, sizeof(*p));
            p->user = user;
        }
        if (p) p->relay_stream = relay;
        UNLOCK();
        if (relay) printf("[psnr] streams to user %u go through the relay\n", user);
        return 1;
    }
    if (m->type == PSNR_STREAM_OFFER && m->len >= 10) {
        uint32_t id = psnr_get32(m->data), from = psnr_get32(m->data + 4);
        uint8_t ip[4] = {0};
        uint16_t port = 0;
        LOCK();
        struct peer* p = peer_by_user(from);
        if (p) { memcpy(ip, p->ip, 4); port = p->port; }
        UNLOCK();
        int64_t fd = s_client && s_stream_sink ? psnr_stream_accept(s_client, id, 5000) : -1;
        printf("[psnr] stream from user %u through the relay: %s\n", from, fd >= 0 ? "open" : "failed");
        if (fd >= 0) s_stream_sink(fd, ip, port);
        return 1;
    }
    return 0;
}

/* From np_psnr_pump: probe psnr until it answers (and again now and then),
 * send the punches that are due, and move peers that never answered a punch
 * to the relay. */
static void nat_tick(void)
{
    uint8_t pkt[PSNR_UDP_PROBE_LEN], ip[4];
    uint16_t port;
    uint64_t now = nat_now_ms();
    if (!s_client || !s_p2p_send) return;
    if (now >= s_next_probe) {
        psnr_probe_packet(s_client, pkt);
        psnr_server_addr(s_client, ip, &port);
        s_p2p_send(pkt, PSNR_UDP_PROBE_LEN, ip, port);
        s_next_probe = now + (s_probed ? REPROBE_MS : PROBE_MS);
    }
    LOCK();
    for (int i = 0; i < MAX_PEERS; i++) {
        struct peer* p = &s_peers[i];
        if (!p->user) continue;
        if (p->punches_left && now >= p->next_punch) {
            psnr_punch_packet(s_client, pkt);
            s_p2p_send(pkt, PSNR_UDP_PUNCH_LEN, p->ip, p->port);
            if (!p->punched_at) p->punched_at = now;
            p->punches_left--;
            p->next_punch = now + PUNCH_MS;
        }
        if (!p->relay_udp && !p->heard_at && p->punched_at && now - p->punched_at > RELAY_AFTER &&
            psnr_relay_available(s_client)) {
            p->relay_udp = 1;
            printf("[psnr] nothing direct from %u.%u.%u.%u:%u; its datagrams go through the relay\n",
                   p->ip[0], p->ip[1], p->ip[2], p->ip[3], p->port);
        }
    }
    UNLOCK();
}

void np_psnr_pump(void)
{
    psnr_msg m;
    if (s_tick_fn) s_tick_fn();
    nat_tick();
    if (!s_client) return;

    for (;;) {
        np_psnr_reply_fn fn = NULL;
        void* user = NULL;
        LOCK();
        if (!s_client) { UNLOCK(); return; }
        int r = psnr_poll(s_client, &m);
        if (r < 0) {
            /* Gone: fail every outstanding request so no title waits forever. */
            printf("[psnr] connection lost\n");
            psnr_close(s_client);
            s_client = NULL;
            psnr_msg dead;
            memset(&dead, 0, sizeof(dead));
            for (int i = 0; i < PENDING_MAX; i++)
                if (s_pending[i].req) {
                    np_psnr_reply_fn f = s_pending[i].fn;
                    void* u = s_pending[i].user;
                    s_pending[i].req = 0;
                    UNLOCK();
                    if (f) f(u, &dead);
                    LOCK();
                }
            UNLOCK();
            return;
        }
        if (r == 0) { UNLOCK(); return; }
        if (m.req) {
            for (int i = 0; i < PENDING_MAX; i++)
                if (s_pending[i].req == m.req) {
                    fn = s_pending[i].fn;
                    user = s_pending[i].user;
                    s_pending[i].req = 0;
                    break;
                }
        }
        UNLOCK();

        /* Handlers run unlocked: they send follow-up requests. */
        if (m.req && fn) fn(user, &m);
        else if (!m.req && !nat_push(&m) && s_push_fn) s_push_fn(&m);
        psnr_msg_free(&m);
    }
}
