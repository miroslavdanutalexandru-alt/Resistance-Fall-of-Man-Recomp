/*
 * port_online - opt-in online play through a community server (PORT_ONLINE=1).
 * See port_online.h for the switches and docs/ONLINE.md ("Community servers")
 * for what works and what is left.
 *
 * Licence: this file is the port's own code, MIT like the rest of ps3recomp.
 * It talks to RPCN servers (the account/ticket service the RPCS3 project
 * runs) by implementing the protocol from its wire format; it contains no
 * RPCS3 source. The format, as the server speaks it:
 *   header, 15 bytes little-endian: u8 type, u16 command, u32 total size
 *   (header included), u64 packet id; TLS 1.2 on TCP 31313.
 *   On connect the server sends ServerInfo (type 3): u32 protocol version.
 *   Login (command 0): "user\0password\0token\0".
 *   RequestTicket (command 28): "service_id\0" + u32 cookie size + cookie.
 *   Replies (type 1) start with an error byte (0 = none).
 * The NP ticket layout (version 2.1) is the console's, as publicly documented
 * (psdevwiki "NP Tickets").
 */
#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    typedef SOCKET po_socket_t;
    #define PO_BAD_SOCKET INVALID_SOCKET
    #define po_close closesocket
#else
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <sys/time.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <fcntl.h>
    #include <unistd.h>
    #include <errno.h>
    #include <strings.h>
    typedef int po_socket_t;
    #define PO_BAD_SOCKET (-1)
    #define po_close close
#endif

#include "port_online.h"
#include "sceNp.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>

/* Hooks owned by the files they bend (pointers, so those files and their own
 * tests do not link against this one). */
extern int (*g_net_resolve_hook)(const char* name, uint32_t* ip_out);   /* sysNet.c */
extern const char* (*g_np_online_id_hook)(void);                         /* np_psnr.c */
extern uint32_t g_netctl_dns_override;                                    /* cellNetCtl.c */

/* ---------------------------------------------------------------------------
 * Config
 * -----------------------------------------------------------------------*/

static port_online_config s_cfg;
static int s_cfg_loaded;

static char* po_trim(char* s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    char* e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

static int po_parse_ipv4(const char* s, uint32_t* out)
{
    unsigned a, b, c, d; char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return 0;
    if (a > 255 || b > 255 || c > 255 || d > 255) return 0;
    const uint8_t bytes[4] = { (uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d };
    memcpy(out, bytes, 4);
    return 1;
}

static void po_copy(char* dst, size_t cap, const char* src)
{
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

int port_online_parse_config(const char* text, port_online_config* cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->rpcn_port = RPCN_DEFAULT_PORT;
    if (!text) return 0;
    int accepted = 0;
    const char* p = text;
    while (*p) {
        const char* nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        char line[512];
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, p, n); line[n] = '\0';
        p = nl ? nl + 1 : p + strlen(p);

        char* l = po_trim(line);
        if (!*l || *l == '#' || *l == ';' || *l == '[') continue;
        char* eq = strchr(l, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = po_trim(l);
        char* val = po_trim(eq + 1);

        if (!strcmp(key, "rpcn_host")) {
            char* colon = strrchr(val, ':');
            if (colon) {
                int port = atoi(colon + 1);
                if (port <= 0 || port > 65535) { printf("[online] config: bad rpcn_host port\n"); continue; }
                cfg->rpcn_port = (uint16_t)port;
                *colon = '\0';
            }
            po_copy(cfg->rpcn_host, sizeof(cfg->rpcn_host), val);
        } else if (!strcmp(key, "rpcn_user")) {
            po_copy(cfg->rpcn_user, sizeof(cfg->rpcn_user), val);
        } else if (!strcmp(key, "rpcn_password")) {
            po_copy(cfg->rpcn_password, sizeof(cfg->rpcn_password), val);
        } else if (!strcmp(key, "rpcn_token")) {
            po_copy(cfg->rpcn_token, sizeof(cfg->rpcn_token), val);
        } else if (!strcmp(key, "dns")) {
            if (!po_parse_ipv4(val, &cfg->dns_server)) { printf("[online] config: bad dns '%s'\n", val); continue; }
        } else if (!strncmp(key, "host.", 5) && key[5]) {
            uint32_t ip;
            if (!po_parse_ipv4(val, &ip)) { printf("[online] config: bad IP for %s\n", key); continue; }
            if (cfg->n_hosts >= PORT_ONLINE_MAX_HOSTS) { printf("[online] config: too many host.* entries\n"); continue; }
            po_copy(cfg->hosts[cfg->n_hosts].name, sizeof(cfg->hosts[0].name), key + 5);
            cfg->hosts[cfg->n_hosts].ip = ip;
            cfg->n_hosts++;
        } else {
            printf("[online] config: unknown key '%s' ignored\n", key);
            continue;
        }
        accepted++;
    }
    return accepted;
}

int port_online_enabled(void)
{
    const char* e = getenv("PORT_ONLINE");
    return e && *e && strcmp(e, "0") != 0;
}

static char* po_read_file(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 1 << 20) { fclose(f); return NULL; }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = '\0';
    fclose(f);
    return buf;
}

static void po_load_config(void)
{
    if (s_cfg_loaded) return;
    s_cfg_loaded = 1;
    char path[1024];
    const char* env = getenv("PORT_ONLINE_CONFIG");
    char* text = NULL;
    if (env && *env) {
        po_copy(path, sizeof(path), env);
        text = po_read_file(path);
    } else {
        po_copy(path, sizeof(path), "online.ini");
        text = po_read_file(path);
#ifdef _WIN32
        if (!text) {  /* next to the exe when started from elsewhere */
            DWORD n = GetModuleFileNameA(NULL, path, (DWORD)sizeof(path));
            char* slash = n ? strrchr(path, '\\') : NULL;
            if (slash && (size_t)(slash - path) + 12 < sizeof(path)) {
                strcpy(slash + 1, "online.ini");
                text = po_read_file(path);
            }
        }
#endif
    }
    if (!text) {
        port_online_parse_config(NULL, &s_cfg);
        printf("[online] no config (%s): offline identity, system DNS\n", path);
        return;
    }
    int n = port_online_parse_config(text, &s_cfg);
    free(text);
    /* Never log the password or token. */
    printf("[online] config %s: %d key(s), rpcn %s:%u user '%s' password %s token %s, dns %s, %d host override(s)\n",
           path, n, s_cfg.rpcn_host[0] ? s_cfg.rpcn_host : "(none)", (unsigned)s_cfg.rpcn_port,
           s_cfg.rpcn_user, s_cfg.rpcn_password[0] ? "set" : "unset",
           s_cfg.rpcn_token[0] ? "set" : "unset", s_cfg.dns_server ? "custom" : "system", s_cfg.n_hosts);
}

const port_online_config* port_online_config_get(void)
{
    po_load_config();
    return &s_cfg;
}

int port_online_signed_in(void)
{
    if (!port_online_enabled()) return 0;
    po_load_config();
    return s_cfg.rpcn_user[0] && s_cfg.rpcn_password[0];
}

static const char* po_online_id(void)
{
    return port_online_signed_in() ? s_cfg.rpcn_user : NULL;
}

/* ---------------------------------------------------------------------------
 * DNS (RFC 1035 A query over UDP)
 * -----------------------------------------------------------------------*/

int port_dns_build_query(uint16_t id, const char* name, uint8_t* out, size_t cap)
{
    size_t nlen = strlen(name);
    if (!nlen || nlen > 253 || cap < 12 + nlen + 2 + 4) return -1;
    memset(out, 0, 12);
    out[0] = (uint8_t)(id >> 8); out[1] = (uint8_t)id;
    out[2] = 0x01;                 /* RD */
    out[5] = 1;                    /* QDCOUNT */
    size_t o = 12;
    const char* s = name;
    while (*s) {
        const char* dot = strchr(s, '.');
        size_t lab = dot ? (size_t)(dot - s) : strlen(s);
        if (lab == 0 || lab > 63) return -1;
        out[o++] = (uint8_t)lab;
        memcpy(out + o, s, lab); o += lab;
        s += lab;
        if (*s == '.') s++;
    }
    out[o++] = 0;
    out[o++] = 0; out[o++] = 1;    /* QTYPE A */
    out[o++] = 0; out[o++] = 1;    /* QCLASS IN */
    return (int)o;
}

/* Skip a (possibly compressed) name; returns the offset after it or 0. */
static size_t po_dns_skip_name(const uint8_t* m, size_t len, size_t o)
{
    for (int guard = 0; guard < 128 && o < len; guard++) {
        uint8_t c = m[o];
        if (c == 0) return o + 1;
        if ((c & 0xC0) == 0xC0) return o + 2 <= len ? o + 2 : 0;
        if (c & 0xC0) return 0;
        o += 1 + (size_t)c;
    }
    return 0;
}

int port_dns_parse_a(const uint8_t* m, size_t len, uint16_t id, uint32_t* ip_out)
{
    if (len < 12) return 0;
    if (((m[0] << 8) | m[1]) != id) return 0;
    if (!(m[2] & 0x80) || (m[3] & 0x0F)) return 0;  /* not a response, or RCODE != 0 */
    unsigned qd = (unsigned)(m[4] << 8 | m[5]), an = (unsigned)(m[6] << 8 | m[7]);
    size_t o = 12;
    for (unsigned i = 0; i < qd; i++) {
        o = po_dns_skip_name(m, len, o);
        if (!o || o + 4 > len) return 0;
        o += 4;
    }
    for (unsigned i = 0; i < an; i++) {
        o = po_dns_skip_name(m, len, o);
        if (!o || o + 10 > len) return 0;
        unsigned type = (unsigned)(m[o] << 8 | m[o + 1]);
        unsigned cls  = (unsigned)(m[o + 2] << 8 | m[o + 3]);
        unsigned rdl  = (unsigned)(m[o + 8] << 8 | m[o + 9]);
        o += 10;
        if (o + rdl > len) return 0;
        if (type == 1 && cls == 1 && rdl == 4) { memcpy(ip_out, m + o, 4); return 1; }
        o += rdl;  /* CNAME etc.: the A record follows in the same answer */
    }
    return 0;
}

static void po_net_startup(void)
{
#ifdef _WIN32
    static int done;
    if (!done) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); done = 1; }
#endif
}

static int po_dns_query(uint32_t server, uint16_t port, const char* name, uint32_t* ip_out)
{
    uint8_t q[300], r[1500];
    uint16_t id = (uint16_t)(rand() ^ (unsigned)time(NULL));
    int qlen = port_dns_build_query(id, name, q, sizeof(q));
    if (qlen < 0) return 0;
    po_net_startup();
    po_socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == PO_BAD_SOCKET) return 0;
#ifdef _WIN32
    DWORD tmo = 1500;
#else
    struct timeval tmo = { 1, 500000 };
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = server;
    int ok = 0;
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
        if (sendto(s, (const char*)q, qlen, 0, (struct sockaddr*)&sa, sizeof(sa)) != qlen) break;
        for (;;) {
            int n = (int)recv(s, (char*)r, sizeof(r), 0);
            if (n <= 0) break;
            if (port_dns_parse_a(r, (size_t)n, id, ip_out)) { ok = 1; break; }
        }
    }
    po_close(s);
    return ok;
}

static uint16_t s_dns_port = 53;   /* tests point this at a loopback mock */

int port_online_resolve(const char* name, uint32_t* ip_out)
{
    if (!name || !*name) return 0;
    po_load_config();
    for (int i = 0; i < s_cfg.n_hosts; i++) {
#ifdef _WIN32
        if (!_stricmp(s_cfg.hosts[i].name, name)) {
#else
        if (!strcasecmp(s_cfg.hosts[i].name, name)) {
#endif
            *ip_out = s_cfg.hosts[i].ip;
            printf("[online] %s -> host override\n", name);
            return 1;
        }
    }
    if (s_cfg.dns_server) {
        if (po_dns_query(s_cfg.dns_server, s_dns_port, name, ip_out)) {
            printf("[online] %s -> via configured dns\n", name);
            return 1;
        }
        printf("[online] %s: configured dns gave no A record, using system resolver\n", name);
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * RPCN wire format
 * -----------------------------------------------------------------------*/

static void po_le16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void po_le32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void po_le64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t po_rd32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int rpcn_write_header(uint8_t* out, uint8_t type, uint16_t cmd, uint32_t total, uint64_t packet_id)
{
    out[0] = type;
    po_le16(out + 1, cmd);
    po_le32(out + 3, total);
    po_le64(out + 7, packet_id);
    return RPCN_HEADER_SIZE;
}

int rpcn_parse_header(const uint8_t* in, size_t len, uint8_t* type, uint16_t* cmd,
                      uint32_t* total, uint64_t* packet_id)
{
    if (len < RPCN_HEADER_SIZE) return -1;
    *type = in[0];
    *cmd = (uint16_t)(in[1] | in[2] << 8);
    *total = po_rd32(in + 3);
    *packet_id = (uint64_t)po_rd32(in + 7) | (uint64_t)po_rd32(in + 11) << 32;
    return *total < RPCN_HEADER_SIZE ? -1 : RPCN_HEADER_SIZE;
}

static int po_put_str(uint8_t* out, size_t cap, size_t* o, const char* s)
{
    size_t n = strlen(s ? s : "") + 1;
    if (*o + n > cap) return -1;
    memcpy(out + *o, s ? s : "", n);
    *o += n;
    return 0;
}

int rpcn_build_login(uint8_t* out, size_t cap, uint64_t packet_id,
                     const char* user, const char* password, const char* token)
{
    size_t o = RPCN_HEADER_SIZE;
    if (cap < o) return -1;
    if (po_put_str(out, cap, &o, user) || po_put_str(out, cap, &o, password) ||
        po_put_str(out, cap, &o, token)) return -1;
    rpcn_write_header(out, RPCN_PKT_REQUEST, RPCN_CMD_LOGIN, (uint32_t)o, packet_id);
    return (int)o;
}

int rpcn_build_ticket_request(uint8_t* out, size_t cap, uint64_t packet_id,
                              const char* service_id, const uint8_t* cookie, uint32_t cookie_size)
{
    size_t o = RPCN_HEADER_SIZE;
    if (cap < o) return -1;
    if (po_put_str(out, cap, &o, service_id)) return -1;
    if (o + 4 + cookie_size > cap) return -1;
    po_le32(out + o, cookie_size); o += 4;
    if (cookie_size) memcpy(out + o, cookie, cookie_size);
    o += cookie_size;
    rpcn_write_header(out, RPCN_PKT_REQUEST, RPCN_CMD_REQUEST_TICKET, (uint32_t)o, packet_id);
    return (int)o;
}

int64_t rpcn_parse_server_info(const uint8_t* pkt, size_t len)
{
    uint8_t type; uint16_t cmd; uint32_t total; uint64_t id;
    if (rpcn_parse_header(pkt, len, &type, &cmd, &total, &id) < 0) return -1;
    if (type != RPCN_PKT_SERVERINFO || total != RPCN_HEADER_SIZE + 4 || len < total) return -1;
    return (int64_t)po_rd32(pkt + RPCN_HEADER_SIZE);
}

/* ---------------------------------------------------------------------------
 * RPCN replies
 * -----------------------------------------------------------------------*/

int rpcn_parse_login_reply(const uint8_t* p, size_t len, char* online_name, size_t name_cap)
{
    if (len < 1) return -1;
    int err = p[0];
    if (online_name && name_cap) online_name[0] = '\0';
    if (err) return err;   /* the server sends the rest regardless; nothing in it matters on failure */
    const uint8_t* nul = (const uint8_t*)memchr(p + 1, 0, len - 1);
    if (!nul) return -1;
    if (online_name && name_cap) po_copy(online_name, name_cap, (const char*)p + 1);
    return 0;
}

int rpcn_parse_ticket_reply(const uint8_t* p, size_t len, const uint8_t** ticket, uint32_t* ticket_len)
{
    if (len < 1) return -1;
    if (p[0]) return p[0];
    if (len < 5) return -1;
    uint32_t n = po_rd32(p + 1);
    if (n > len - 5) return -1;
    *ticket = p + 5;
    *ticket_len = n;
    return 0;
}

/* ---------------------------------------------------------------------------
 * NP ticket 2.1
 *
 * be32 version (0x21010000), be32 size of what follows, then sections. Every
 * item is be16 type, be16 length, data. Section 0x3000 is the body, 0x3002
 * the signature (not checked here: the game server does that). The body's
 * items, in order, are GetTicketParam's ids 0..9; ids 10 and 11 (status
 * duration, date of birth) are not in it and read as zero. Integers and
 * dates are big-endian in the ticket, which is the guest's order already, so
 * every field is copied as it stands.
 * -----------------------------------------------------------------------*/

#define PO_TICKET_VERSION 0x21010000u

static uint32_t po_be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t po_be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

/* item type, exact length (0 = any up to 256) */
static const struct { uint16_t type, len; } k_ticket_body[10] = {
    { 8, 20 },   /* 0 serial id        binary   */
    { 1, 4 },    /* 1 issuer id        u32      */
    { 7, 8 },    /* 2 issued date      time ms  */
    { 7, 8 },    /* 3 expire date      time ms  */
    { 2, 8 },    /* 4 account id       u64      */
    { 4, 32 },   /* 5 online id        string   */
    { 8, 4 },    /* 6 region           binary   */
    { 4, 4 },    /* 7 domain           string   */
    { 8, 0 },    /* 8 service id       binary   */
    { 1, 4 },    /* 9 status           u32      */
};

/* Walks the body up to field `stop` (10 = all of it). Returns the offset of
 * that field's item, or of the end of the body for 10; 0 when malformed. */
static size_t po_ticket_walk(const uint8_t* t, size_t len, int stop, size_t* body_end)
{
    if (!t || len < 12 || po_be32(t) != PO_TICKET_VERSION || po_be32(t + 4) != len - 8) return 0;
    if (po_be16(t + 8) != 0x3000) return 0;
    size_t end = 12 + (size_t)po_be16(t + 10);
    if (end > len) return 0;
    size_t o = 12;
    for (int i = 0; i < 10; i++) {
        if (o + 4 > end) return 0;
        uint16_t type = po_be16(t + o), n = po_be16(t + o + 2);
        if (o + 4 + n > end || type != k_ticket_body[i].type || n > 256) return 0;
        if (k_ticket_body[i].len && n != k_ticket_body[i].len) return 0;
        if (i == stop) break;
        o += 4 + (size_t)n;
    }
    if (body_end) *body_end = end;
    return o;
}

int port_ticket_check(const uint8_t* t, size_t len)
{
    size_t end = 0;
    if (!po_ticket_walk(t, len, 10, &end)) return -1;
    if (end == len) return 0;   /* unsigned */
    if (end + 4 > len || po_be16(t + end) != 0x3002) return -1;
    return end + 4 + po_be16(t + end + 2) == len ? 0 : -1;
}

int port_ticket_param(const uint8_t* t, size_t len, int param_id, uint8_t out[256])
{
    memset(out, 0, 256);
    if (param_id < 0 || param_id > 11 || port_ticket_check(t, len) < 0) return -1;
    if (param_id >= 10) return 0;
    size_t o = po_ticket_walk(t, len, param_id, NULL);
    if (!o) return -1;
    memcpy(out, t + o + 4, po_be16(t + o + 2));
    return 0;
}

/* ---------------------------------------------------------------------------
 * RPCN password derivation
 *
 * RPCN accounts are keyed on a derived password, not the typed one:
 * PBKDF2-HMAC-SHA3-256 over the password with the salt below, 200000
 * iterations, 32 bytes, sent as 64 uppercase hex digits (what the RPCS3
 * client stores and sends). SHA3-256 is FIPS 202; this is a plain
 * implementation of the standard, as Windows' CNG only has SHA3 on recent
 * builds.
 * -----------------------------------------------------------------------*/

#define PO_SHA3_RATE 136   /* SHA3-256: 1600 - 2*256 bits */

static uint64_t po_rol64(uint64_t v, unsigned n) { return n ? (v << n) | (v >> (64 - n)) : v; }

static void po_keccak_f(uint64_t a[25])
{
    static const uint64_t rc[24] = {
        0x0000000000000001ull, 0x0000000000008082ull, 0x800000000000808Aull, 0x8000000080008000ull,
        0x000000000000808Bull, 0x0000000080000001ull, 0x8000000080008081ull, 0x8000000000008009ull,
        0x000000000000008Aull, 0x0000000000000088ull, 0x0000000080008009ull, 0x000000008000000Aull,
        0x000000008000808Bull, 0x800000000000008Bull, 0x8000000000008089ull, 0x8000000000008003ull,
        0x8000000000008002ull, 0x8000000000000080ull, 0x000000000000800Aull, 0x800000008000000Aull,
        0x8000000080008081ull, 0x8000000000008080ull, 0x0000000080000001ull, 0x8000000080008008ull,
    };
    static const unsigned rot[25] = { 0, 1, 62, 28, 27, 36, 44, 6, 55, 20, 3, 10, 43,
                                      25, 39, 41, 45, 15, 21, 8, 18, 2, 61, 56, 14 };
    for (int r = 0; r < 24; r++) {
        uint64_t c[5], b[25];
        for (int x = 0; x < 5; x++) c[x] = a[x] ^ a[x + 5] ^ a[x + 10] ^ a[x + 15] ^ a[x + 20];
        for (int x = 0; x < 5; x++) {
            uint64_t d = c[(x + 4) % 5] ^ po_rol64(c[(x + 1) % 5], 1);
            for (int y = 0; y < 25; y += 5) a[y + x] ^= d;
        }
        for (int x = 0; x < 5; x++)          /* rho + pi: (x,y) -> (y, 2x+3y) */
            for (int y = 0; y < 5; y++)
                b[y + 5 * ((2 * x + 3 * y) % 5)] = po_rol64(a[x + 5 * y], rot[x + 5 * y]);
        for (int y = 0; y < 25; y += 5)      /* chi */
            for (int x = 0; x < 5; x++)
                a[y + x] = b[y + x] ^ (~b[y + (x + 1) % 5] & b[y + (x + 2) % 5]);
        a[0] ^= rc[r];                       /* iota */
    }
}

typedef struct { uint64_t a[25]; size_t n; } po_sha3;

static void po_sha3_init(po_sha3* s) { memset(s, 0, sizeof(*s)); }

static void po_sha3_update(po_sha3* s, const uint8_t* d, size_t len)
{
    while (len--) {
        s->a[s->n / 8] ^= (uint64_t)*d++ << (8 * (s->n % 8));
        if (++s->n == PO_SHA3_RATE) { po_keccak_f(s->a); s->n = 0; }
    }
}

static void po_sha3_final(po_sha3* s, uint8_t out[32])
{
    s->a[s->n / 8] ^= (uint64_t)0x06 << (8 * (s->n % 8));
    s->a[(PO_SHA3_RATE - 1) / 8] ^= (uint64_t)0x80 << (8 * ((PO_SHA3_RATE - 1) % 8));
    po_keccak_f(s->a);
    for (int i = 0; i < 32; i++) out[i] = (uint8_t)(s->a[i / 8] >> (8 * (i % 8)));
}

void port_sha3_256(const uint8_t* d, size_t len, uint8_t out[32])
{
    po_sha3 s;
    po_sha3_init(&s);
    po_sha3_update(&s, d, len);
    po_sha3_final(&s, out);
}

/* HMAC keyed once: the inner and outer states after their pad blocks. */
typedef struct { po_sha3 in, out; } po_hmac;

static void po_hmac_key(po_hmac* h, const uint8_t* key, size_t klen)
{
    uint8_t k[PO_SHA3_RATE], pad[PO_SHA3_RATE];
    memset(k, 0, sizeof(k));
    if (klen > PO_SHA3_RATE) port_sha3_256(key, klen, k);
    else if (klen) memcpy(k, key, klen);
    for (int i = 0; i < PO_SHA3_RATE; i++) pad[i] = k[i] ^ 0x36;
    po_sha3_init(&h->in);  po_sha3_update(&h->in, pad, sizeof(pad));
    for (int i = 0; i < PO_SHA3_RATE; i++) pad[i] = k[i] ^ 0x5c;
    po_sha3_init(&h->out); po_sha3_update(&h->out, pad, sizeof(pad));
}

static void po_hmac_run(const po_hmac* h, const uint8_t* m, size_t mlen,
                        const uint8_t* m2, size_t m2len, uint8_t out[32])
{
    po_sha3 s = h->in;
    uint8_t inner[32];
    po_sha3_update(&s, m, mlen);
    if (m2len) po_sha3_update(&s, m2, m2len);
    po_sha3_final(&s, inner);
    s = h->out;
    po_sha3_update(&s, inner, sizeof(inner));
    po_sha3_final(&s, out);
}

void port_hmac_sha3_256(const uint8_t* key, size_t klen, const uint8_t* m, size_t mlen, uint8_t out[32])
{
    po_hmac h;
    po_hmac_key(&h, key, klen);
    po_hmac_run(&h, m, mlen, NULL, 0, out);
}

/* PBKDF2 with a 32-byte output: one block, T1 = U1 ^ ... ^ Uc. */
void port_pbkdf2_sha3_256(const uint8_t* pw, size_t pwlen, const uint8_t* salt, size_t slen,
                          uint32_t iterations, uint8_t out[32])
{
    po_hmac h;
    const uint8_t block1[4] = { 0, 0, 0, 1 };
    uint8_t u[32];
    po_hmac_key(&h, pw, pwlen);
    po_hmac_run(&h, salt, slen, block1, 4, u);
    memcpy(out, u, 32);
    for (uint32_t i = 1; i < iterations; i++) {
        po_hmac_run(&h, u, 32, NULL, 0, u);
        for (int j = 0; j < 32; j++) out[j] ^= u[j];
    }
}

void port_rpcn_password(const char* password, char out[65])
{
    static const char salt[] = "No matter where you go, everybody's connected.";
    static const char hex[] = "0123456789ABCDEF";
    size_t n = strlen(password);
    int already = n == 64;
    for (size_t i = 0; already && i < n; i++) already = isxdigit((unsigned char)password[i]) != 0;
    if (already) { memcpy(out, password, 65); return; }   /* stored derived, as RPCS3 keeps it */
    uint8_t d[32];
    port_pbkdf2_sha3_256((const uint8_t*)password, n, (const uint8_t*)salt, sizeof(salt) - 1, 200000, d);
    for (int i = 0; i < 32; i++) { out[2 * i] = hex[d[i] >> 4]; out[2 * i + 1] = hex[d[i] & 15]; }
    out[64] = '\0';
}

/* ---------------------------------------------------------------------------
 * RPCN client: TLS 1.2 over TCP. RPCS3 does not verify the server
 * certificate (SSL_VERIFY_NONE); Schannel's equivalent is manual validation
 * that we never perform. Windows only -- elsewhere the request fails cleanly.
 * -----------------------------------------------------------------------*/

#define PO_TIMEOUT_MS     10000
#define PO_MAX_PACKET     (1u << 20)

static po_socket_t po_tcp_connect(const char* host, uint16_t port, int timeout_ms)
{
    struct addrinfo hints, *res = NULL;
    char ps[8];
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(ps, sizeof(ps), "%u", (unsigned)port);
    po_net_startup();
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) {
        printf("[online] rpcn: cannot resolve %s\n", host);
        return PO_BAD_SOCKET;
    }
    po_socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int ok = 0;
    if (s != PO_BAD_SOCKET) {
#ifdef _WIN32
        u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
#else
        fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
#endif
        connect(s, res->ai_addr, (int)res->ai_addrlen);
        fd_set w, e; FD_ZERO(&w); FD_ZERO(&e); FD_SET(s, &w); FD_SET(s, &e);
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        if (select((int)s + 1, NULL, &w, &e, &tv) > 0 && FD_ISSET(s, &w) && !FD_ISSET(s, &e)) {
            int err = 0; socklen_t el = sizeof(err);
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &el);
            ok = err == 0;
        }
#ifdef _WIN32
        nb = 0; ioctlsocket(s, FIONBIO, &nb);
        DWORD tmo = (DWORD)timeout_ms;
#else
        fcntl(s, F_SETFL, fcntl(s, F_GETFL) & ~O_NONBLOCK);
        struct timeval tmo = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
#endif
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tmo, sizeof(tmo));
        if (!ok) { po_close(s); s = PO_BAD_SOCKET; }
    }
    freeaddrinfo(res);
    if (!ok) printf("[online] rpcn: TCP %s:%u not reachable\n", host, (unsigned)port);
    return s;
}

static int po_send_all(po_socket_t s, const uint8_t* d, size_t n)
{
    while (n) {
        int k = (int)send(s, (const char*)d, (int)n, 0);
        if (k <= 0) return -1;
        d += k; n -= (size_t)k;
    }
    return 0;
}

#ifdef _WIN32
#define SECURITY_WIN32
#include <security.h>
#include <schannel.h>
#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "ws2_32.lib")

#define PO_TLS_BUF (64 * 1024)

typedef struct {
    po_socket_t s;
    CredHandle  cred;
    CtxtHandle  ctx;
    int         have_cred, have_ctx;
    SecPkgContext_StreamSizes sizes;
    uint8_t*    in;    size_t in_len;          /* ciphertext not yet consumed */
    uint8_t*    plain; size_t plain_len, plain_off;
} po_tls;

static int po_tls_fill(po_tls* t)
{
    if (t->in_len >= PO_TLS_BUF) return -1;
    int k = (int)recv(t->s, (char*)t->in + t->in_len, (int)(PO_TLS_BUF - t->in_len), 0);
    if (k <= 0) return -1;
    t->in_len += (size_t)k;
    return 0;
}

static void po_tls_close(po_tls* t)
{
    if (t->have_ctx) DeleteSecurityContext(&t->ctx);
    if (t->have_cred) FreeCredentialsHandle(&t->cred);
    if (t->s != PO_BAD_SOCKET) po_close(t->s);
    free(t->in); free(t->plain);
    memset(t, 0, sizeof(*t));
    t->s = PO_BAD_SOCKET;
}

static int po_tls_connect(po_tls* t, const char* host, uint16_t port)
{
    memset(t, 0, sizeof(*t));
    t->s = po_tcp_connect(host, port, PO_TIMEOUT_MS);
    if (t->s == PO_BAD_SOCKET) return -1;
    t->in = (uint8_t*)malloc(PO_TLS_BUF);
    t->plain = (uint8_t*)malloc(PO_TLS_BUF);
    if (!t->in || !t->plain) return -1;

    SCHANNEL_CRED sc;
    memset(&sc, 0, sizeof(sc));
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    sc.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
    sc.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;
    SECURITY_STATUS ss = AcquireCredentialsHandleA(NULL, (SEC_CHAR*)UNISP_NAME_A, SECPKG_CRED_OUTBOUND,
                                                   NULL, &sc, NULL, NULL, &t->cred, NULL);
    if (ss != SEC_E_OK) { printf("[online] rpcn: AcquireCredentialsHandle 0x%08lX\n", (unsigned long)ss); return -1; }
    t->have_cred = 1;

    const DWORD req = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                      ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_MANUAL_CRED_VALIDATION;
    DWORD attrs = 0;
    int need_recv = 0;
    for (int round = 0; round < 64; round++) {
        SecBuffer ob = { 0, SECBUFFER_TOKEN, NULL };
        SecBufferDesc od = { SECBUFFER_VERSION, 1, &ob };
        SecBuffer ib[2] = { { (unsigned long)t->in_len, SECBUFFER_TOKEN, t->in }, { 0, SECBUFFER_EMPTY, NULL } };
        SecBufferDesc id = { SECBUFFER_VERSION, 2, ib };
        if (round == 0) {
            ss = InitializeSecurityContextA(&t->cred, NULL, (SEC_CHAR*)host, req, 0, 0, NULL, 0,
                                            &t->ctx, &od, &attrs, NULL);
            if (ss == SEC_I_CONTINUE_NEEDED) t->have_ctx = 1;
        } else {
            if (need_recv && po_tls_fill(t) < 0) { printf("[online] rpcn: connection closed during TLS handshake\n"); return -1; }
            ib[0].cbBuffer = (unsigned long)t->in_len;
            ss = InitializeSecurityContextA(&t->cred, &t->ctx, (SEC_CHAR*)host, req, 0, 0, &id, 0,
                                            NULL, &od, &attrs, NULL);
        }
        if (ss == SEC_E_INCOMPLETE_MESSAGE) { need_recv = 1; continue; }
        if (ob.cbBuffer && ob.pvBuffer) {
            int r = po_send_all(t->s, (const uint8_t*)ob.pvBuffer, ob.cbBuffer);
            FreeContextBuffer(ob.pvBuffer);
            if (r < 0) return -1;
        }
        if (ss != SEC_E_OK && ss != SEC_I_CONTINUE_NEEDED) {
            printf("[online] rpcn: TLS handshake failed 0x%08lX\n", (unsigned long)ss);
            return -1;
        }
        if (round > 0) {
            if (ib[1].BufferType == SECBUFFER_EXTRA && ib[1].cbBuffer) {
                memmove(t->in, t->in + t->in_len - ib[1].cbBuffer, ib[1].cbBuffer);
                t->in_len = ib[1].cbBuffer;
            } else {
                t->in_len = 0;
            }
        }
        if (ss == SEC_E_OK) {
            if (QueryContextAttributesA(&t->ctx, SECPKG_ATTR_STREAM_SIZES, &t->sizes) != SEC_E_OK) return -1;
            return 0;
        }
        need_recv = t->in_len == 0;
    }
    return -1;
}

static int po_tls_send(po_tls* t, const uint8_t* d, size_t n)
{
    const size_t hdr = t->sizes.cbHeader, trl = t->sizes.cbTrailer, max = t->sizes.cbMaximumMessage;
    uint8_t* buf = (uint8_t*)malloc(hdr + max + trl);
    if (!buf) return -1;
    int rc = 0;
    while (n && rc == 0) {
        size_t c = n < max ? n : max;
        memcpy(buf + hdr, d, c);
        SecBuffer b[4] = { { (unsigned long)hdr, SECBUFFER_STREAM_HEADER, buf },
                           { (unsigned long)c, SECBUFFER_DATA, buf + hdr },
                           { (unsigned long)trl, SECBUFFER_STREAM_TRAILER, buf + hdr + c },
                           { 0, SECBUFFER_EMPTY, NULL } };
        SecBufferDesc bd = { SECBUFFER_VERSION, 4, b };
        if (EncryptMessage(&t->ctx, 0, &bd, 0) != SEC_E_OK) rc = -1;
        else rc = po_send_all(t->s, buf, b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer);
        d += c; n -= c;
    }
    free(buf);
    return rc;
}

static int po_tls_recv(po_tls* t, uint8_t* out, size_t n)
{
    while (n) {
        if (t->plain_off < t->plain_len) {
            size_t c = t->plain_len - t->plain_off;
            if (c > n) c = n;
            memcpy(out, t->plain + t->plain_off, c);
            t->plain_off += c; out += c; n -= c;
            continue;
        }
        if (t->in_len == 0 && po_tls_fill(t) < 0) return -1;
        SecBuffer b[4] = { { (unsigned long)t->in_len, SECBUFFER_DATA, t->in },
                           { 0, SECBUFFER_EMPTY, NULL }, { 0, SECBUFFER_EMPTY, NULL }, { 0, SECBUFFER_EMPTY, NULL } };
        SecBufferDesc bd = { SECBUFFER_VERSION, 4, b };
        SECURITY_STATUS ss = DecryptMessage(&t->ctx, &bd, 0, NULL);
        if (ss == SEC_E_INCOMPLETE_MESSAGE) { if (po_tls_fill(t) < 0) return -1; continue; }
        if (ss != SEC_E_OK) {
            printf("[online] rpcn: TLS receive ended 0x%08lX\n", (unsigned long)ss);
            return -1;
        }
        t->plain_len = t->plain_off = 0;
        SecBuffer* extra = NULL;
        for (int i = 1; i < 4; i++) {
            if (b[i].BufferType == SECBUFFER_DATA && b[i].cbBuffer <= PO_TLS_BUF) {
                memcpy(t->plain, b[i].pvBuffer, b[i].cbBuffer);
                t->plain_len = b[i].cbBuffer;
            } else if (b[i].BufferType == SECBUFFER_EXTRA) {
                extra = &b[i];
            }
        }
        if (extra) {
            memmove(t->in, t->in + t->in_len - extra->cbBuffer, extra->cbBuffer);
            t->in_len = extra->cbBuffer;
        } else {
            t->in_len = 0;
        }
    }
    return 0;
}

/* Read one packet; the payload (after the header) is malloc'd into *payload. */
static int po_rpcn_read(po_tls* t, uint8_t* type, uint16_t* cmd, uint64_t* id, uint8_t** payload, uint32_t* plen)
{
    uint8_t h[RPCN_HEADER_SIZE];
    uint32_t total;
    if (po_tls_recv(t, h, sizeof(h)) < 0) return -1;
    if (rpcn_parse_header(h, sizeof(h), type, cmd, &total, id) < 0 || total > PO_MAX_PACKET) return -1;
    *plen = total - RPCN_HEADER_SIZE;
    *payload = (uint8_t*)malloc(*plen ? *plen : 1);
    if (!*payload) return -1;
    if (*plen && po_tls_recv(t, *payload, *plen) < 0) { free(*payload); *payload = NULL; return -1; }
    return 0;
}

/* Wait for the reply to (cmd, id), skipping notifications. */
static int po_rpcn_reply(po_tls* t, uint16_t want_cmd, uint64_t want_id, uint8_t** payload, uint32_t* plen)
{
    for (int i = 0; i < 256; i++) {
        uint8_t type; uint16_t cmd; uint64_t id;
        if (po_rpcn_read(t, &type, &cmd, &id, payload, plen) < 0) return -1;
        if (type == RPCN_PKT_REPLY && cmd == want_cmd && id == want_id) return 0;
        free(*payload); *payload = NULL;
    }
    return -1;
}

/* Connect, check the protocol version; with service_id also log in and fetch
 * a ticket into out/out_len (cap bytes). Returns 0 on success. */
static int po_rpcn_session(const char* service_id, const uint8_t* cookie, uint32_t cookie_size,
                           uint8_t* out, uint32_t cap, uint32_t* out_len)
{
    po_tls t;
    int rc = -1;
    uint8_t* p = NULL; uint32_t pl = 0;
    uint8_t type; uint16_t cmd; uint64_t id;
    if (po_tls_connect(&t, s_cfg.rpcn_host, s_cfg.rpcn_port) < 0) goto done;
    if (po_rpcn_read(&t, &type, &cmd, &id, &p, &pl) < 0 || type != RPCN_PKT_SERVERINFO || pl != 4) {
        printf("[online] rpcn: no ServerInfo from %s\n", s_cfg.rpcn_host);
        goto done;
    }
    uint32_t ver = po_rd32(p);
    free(p); p = NULL;
    if (ver != RPCN_PROTOCOL_VERSION) {
        printf("[online] rpcn: server protocol %u, client speaks %u\n", ver, RPCN_PROTOCOL_VERSION);
        goto done;
    }
    printf("[online] rpcn: %s:%u TLS ok, protocol %u\n", s_cfg.rpcn_host, (unsigned)s_cfg.rpcn_port, ver);
    if (!service_id) { rc = 0; goto done; }

    uint8_t buf[RPCN_HEADER_SIZE + 4 + 64 + NP_TICKET_COOKIE_MAX + 256];
    static char derived[65];   /* derived once per run: 200000 rounds */
    if (!derived[0]) port_rpcn_password(s_cfg.rpcn_password, derived);
    int n = rpcn_build_login(buf, sizeof(buf), 1, s_cfg.rpcn_user, derived, s_cfg.rpcn_token);
    if (n < 0 || po_tls_send(&t, buf, (size_t)n) < 0 || po_rpcn_reply(&t, RPCN_CMD_LOGIN, 1, &p, &pl) < 0) {
        printf("[online] rpcn: login exchange failed\n");
        goto done;
    }
    char name[64];
    int err = rpcn_parse_login_reply(p, pl, name, sizeof(name));
    free(p); p = NULL;
    if (err) {
        printf("[online] rpcn: login refused (error %d: check rpcn_user/password/token)\n", err);
        goto done;
    }
    printf("[online] rpcn: logged in as %s\n", name);

    n = rpcn_build_ticket_request(buf, sizeof(buf), 2, service_id, cookie, cookie_size);
    if (n < 0 || po_tls_send(&t, buf, (size_t)n) < 0 || po_rpcn_reply(&t, RPCN_CMD_REQUEST_TICKET, 2, &p, &pl) < 0) {
        printf("[online] rpcn: ticket exchange failed\n");
        goto done;
    }
    const uint8_t* tk; uint32_t tl;
    err = rpcn_parse_ticket_reply(p, pl, &tk, &tl);
    if (err || tl > cap) {
        printf("[online] rpcn: ticket refused (error %d, %u bytes)\n", err, err ? 0u : tl);
        goto done;
    }
    memcpy(out, tk, tl);
    *out_len = tl;
    printf("[online] rpcn: ticket for '%s', %u bytes\n", service_id, tl);
    rc = 0;
    /* Polite close, as RPCS3 does on shutdown; the reply is not awaited. */
    n = 0;
    rpcn_write_header(buf, RPCN_PKT_REQUEST, RPCN_CMD_TERMINATE, RPCN_HEADER_SIZE, 3);
    po_tls_send(&t, buf, RPCN_HEADER_SIZE);
done:
    free(p);
    po_tls_close(&t);
    return rc;
}
#else
static int po_rpcn_session(const char* service_id, const uint8_t* cookie, uint32_t cookie_size,
                           uint8_t* out, uint32_t cap, uint32_t* out_len)
{
    (void)service_id; (void)cookie; (void)cookie_size; (void)out; (void)cap; (void)out_len;
    printf("[online] rpcn: TLS transport is Windows-only (Schannel)\n");
    return -1;
}
#endif

/* PORT_ONLINE_PROBE: TLS connect + ServerInfo only. No login, no payload. */
static void po_probe(void)
{
    if (!s_cfg.rpcn_host[0]) { printf("[online] probe: no rpcn_host configured\n"); return; }
    int ok = po_rpcn_session(NULL, NULL, 0, NULL, 0, NULL) == 0;
    printf("[online] probe: %s:%u %s\n", s_cfg.rpcn_host, (unsigned)s_cfg.rpcn_port,
           ok ? "OK" : "FAILED");
}

/* ---------------------------------------------------------------------------
 * Ticket source for sceNp.c (g_np_ticket_source)
 *
 * Like the console, RequestTicket returns at once and the result arrives as
 * a GOT_TICKET manager event (np_ticket_deliver: the ticket's size, or a
 * negative error); the fetch runs on its own thread.
 * -----------------------------------------------------------------------*/

#define PO_TICKET_CAP 4096
static uint8_t  s_ticket[PO_TICKET_CAP];
static uint32_t s_ticket_len;
static volatile long s_ticket_state;   /* 1 = ticket held, 0 = none yet, <0 = last request's error */
static volatile long s_ticket_busy;

typedef struct {
    char     service[64];
    uint8_t  cookie[NP_TICKET_COOKIE_MAX];
    uint32_t cookie_size;
} po_ticket_job;

/* Test seams: the network session (tests never touch the network), and the
 * request running inline instead of on a thread. */
static int (*s_session_fn)(const char*, const uint8_t*, uint32_t, uint8_t*, uint32_t, uint32_t*) = po_rpcn_session;
static int s_ticket_sync;

static void po_store_state(long v)
{
#ifdef _WIN32
    InterlockedExchange(&s_ticket_state, v);
#else
    __atomic_store_n(&s_ticket_state, v, __ATOMIC_SEQ_CST);
#endif
}

static void po_ticket_worker(po_ticket_job* job)
{
    uint32_t len = 0;
    int32_t result;
    if (!s_cfg.rpcn_host[0]) {
        printf("[online] RequestTicket('%s'): no rpcn_host in online.ini\n", job->service);
        result = (int32_t)SCE_NP_ERROR_OFFLINE;
    } else if (s_session_fn(job->service, job->cookie, job->cookie_size, s_ticket, sizeof(s_ticket), &len) == 0
               && port_ticket_check(s_ticket, len) == 0) {
        s_ticket_len = len;
        result = (int32_t)len;
    } else {
        if (len) printf("[online] RequestTicket('%s'): the server's ticket is not a 2.1 ticket\n", job->service);
        result = (int32_t)SCE_NP_ERROR_OFFLINE;
    }
    free(job);
    po_store_state(result > 0 ? 1 : result);
    /* not busy before the event is queued: a GOT_TICKET callback that
     * re-requests must start a new fetch */
#ifdef _WIN32
    InterlockedExchange(&s_ticket_busy, 0);
#else
    __atomic_store_n(&s_ticket_busy, 0, __ATOMIC_SEQ_CST);
#endif
    np_ticket_deliver(result);
}

#ifdef _WIN32
static DWORD WINAPI po_ticket_thread(LPVOID p) { po_ticket_worker((po_ticket_job*)p); return 0; }
#else
#include <pthread.h>
static void* po_ticket_thread(void* p) { po_ticket_worker((po_ticket_job*)p); return NULL; }
#endif

static int32_t po_src_request(const char* service_id, const uint8_t* cookie, uint32_t cookie_size)
{
    if (!port_online_signed_in()) return 0;   /* no account: the made-up ticket answers */
    if (cookie_size > NP_TICKET_COOKIE_MAX) return (int32_t)SCE_NP_ERROR_INVALID_ARGUMENT;
#ifdef _WIN32
    if (InterlockedCompareExchange(&s_ticket_busy, 1, 0) != 0) return 1;   /* in flight: one event */
#else
    if (__sync_val_compare_and_swap(&s_ticket_busy, 0, 1) != 0) return 1;
#endif
    po_ticket_job* job = (po_ticket_job*)calloc(1, sizeof(*job));
    if (!job) { s_ticket_busy = 0; return (int32_t)SCE_NP_ERROR_OFFLINE; }
    po_copy(job->service, sizeof(job->service), service_id ? service_id : "");
    if (cookie && cookie_size) {
        memcpy(job->cookie, cookie, cookie_size);
        job->cookie_size = cookie_size;
    }
    po_store_state(0);
    printf("[online] RequestTicket(service '%s', cookie %u bytes)\n", job->service, job->cookie_size);
    if (s_ticket_sync) { po_ticket_worker(job); return 1; }
#ifdef _WIN32
    HANDLE th = CreateThread(NULL, 0, po_ticket_thread, job, 0, NULL);
    if (th) CloseHandle(th); else po_ticket_worker(job);
#else
    pthread_t th;
    if (pthread_create(&th, NULL, po_ticket_thread, job) == 0) pthread_detach(th); else po_ticket_worker(job);
#endif
    return 1;
}

static int32_t po_src_get(const uint8_t** t, uint32_t* len)
{
    long st = s_ticket_state;
    if (st <= 0) return (int32_t)st;
    *t = s_ticket;
    *len = s_ticket_len;
    return 1;
}

static int po_src_param(const uint8_t* t, uint32_t len, int32_t param_id, uint8_t out[256])
{
    return port_ticket_param(t, len, param_id, out);
}

static const np_ticket_source s_ticket_source = { po_src_request, po_src_get, po_src_param };

/* ---------------------------------------------------------------------------
 * Startup
 * -----------------------------------------------------------------------*/

void port_online_register(void)
{
    if (!port_online_enabled()) return;
    po_load_config();
#ifdef _WIN32
    if (!getenv("PS3_NET_ONLINE")) _putenv("PS3_NET_ONLINE=1");
#else
    if (!getenv("PS3_NET_ONLINE")) setenv("PS3_NET_ONLINE", "1", 0);
#endif
    g_net_resolve_hook = port_online_resolve;
    g_netctl_dns_override = s_cfg.dns_server;
    g_np_online_id_hook = po_online_id;
    g_np_ticket_source = &s_ticket_source;
    printf("[online] PORT_ONLINE: %s\n",
           port_online_signed_in() ? "signed in (RPCN credentials present)"
                                   : "online, not signed in (no rpcn_user/password: made-up NP ticket)");
    const char* probe = getenv("PORT_ONLINE_PROBE");
    if (probe && *probe && strcmp(probe, "0") != 0) po_probe();
}
