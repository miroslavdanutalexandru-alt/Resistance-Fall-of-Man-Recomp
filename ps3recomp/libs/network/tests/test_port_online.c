/*
 * test_port_online - PORT_ONLINE: online.ini parsing, hostname redirection
 * (static entries and a loopback mock DNS server), the RPCN wire format and
 * password derivation, NP ticket 2.1 fields, and the ticket source sceNp.c
 * draws on. No traffic leaves the machine.
 *
 *   clang-cl /I include /Fe:test_port_online.exe libs/network/tests/test_port_online.c ws2_32.lib
 *   cc -std=gnu17 -I include -o test_port_online libs/network/tests/test_port_online.c -lpthread
 */
#include "../port_online.c"
#include <assert.h>

/* What port_online.c reaches in sysNet.c, np_psnr.c, cellNetCtl.c, sceNp.c. */
int (*g_net_resolve_hook)(const char*, uint32_t*);
const char* (*g_np_online_id_hook)(void);
uint32_t g_netctl_dns_override;
const np_ticket_source* g_np_ticket_source;
static int s_delivered;
static s32 s_delivered_result;
void np_ticket_deliver(s32 result) { s_delivered++; s_delivered_result = result; }

static void set_env(const char* kv)
{
#ifdef _WIN32
    _putenv(kv);
#else
    putenv((char*)kv);
#endif
}

/* Mock DNS: answers "game.example" with a CNAME then an A record
 * (compressed names), and NXDOMAIN for anything else. */
static po_socket_t s_mock;
static void mock_dns_once(void)
{
    uint8_t q[512], r[512];
    struct sockaddr_in from; socklen_t fl = sizeof(from);
    int n = (int)recvfrom(s_mock, (char*)q, sizeof(q), 0, (struct sockaddr*)&from, &fl);
    if (n < 12) return;
    int known = n > 12 + 14 && !memcmp(q + 12, "\4game\7example\0", 14);
    memcpy(r, q, (size_t)n);
    r[2] = 0x81; r[3] = known ? 0x80 : 0x83;
    int o = n;
    if (known) {
        r[7] = 2;
        const uint8_t cname[] = { 0xC0, 12, 0, 5, 0, 1, 0, 0, 0, 60, 0, 2, 0xC0, 12 };
        memcpy(r + o, cname, sizeof(cname)); o += (int)sizeof(cname);
        const uint8_t a[] = { 0xC0, 12, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 203, 0, 113, 9 };
        memcpy(r + o, a, sizeof(a)); o += (int)sizeof(a);
    }
    sendto(s_mock, (const char*)r, o, 0, (struct sockaddr*)&from, fl);
}
#ifdef _WIN32
static DWORD WINAPI mock_thread(LPVOID u) { (void)u; mock_dns_once(); mock_dns_once(); return 0; }
static void start_mock(void) { CloseHandle(CreateThread(NULL, 0, mock_thread, NULL, 0, NULL)); }
#else
static void* mock_thread(void* u) { (void)u; mock_dns_once(); mock_dns_once(); return NULL; }
static void start_mock(void) { pthread_t t; pthread_create(&t, NULL, mock_thread, NULL); pthread_detach(t); }
#endif

static uint32_t ip4(int a, int b, int c, int d)
{
    const uint8_t x[4] = { (uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d };
    uint32_t v; memcpy(&v, x, 4); return v;
}

static void hex(const uint8_t* d, size_t n, char* out)
{
    for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", d[i]);
}

static char s_fake_service[64];
static uint32_t s_fake_cookie_size;
static uint8_t s_fake_cookie0;
static int s_fake_fail;
static const uint8_t* s_fake_ticket = (const uint8_t*)"TICKET";
static uint32_t s_fake_ticket_len = 6;
static int fake_session(const char* svc, const uint8_t* cookie, uint32_t cookie_size,
                        uint8_t* out, uint32_t cap, uint32_t* out_len)
{
    strcpy(s_fake_service, svc);
    s_fake_cookie_size = cookie_size;
    s_fake_cookie0 = cookie_size ? cookie[0] : 0;
    if (s_fake_fail) return -1;
    assert(cap >= s_fake_ticket_len);
    memcpy(out, s_fake_ticket, s_fake_ticket_len);
    *out_len = s_fake_ticket_len;
    return 0;
}

/* A 2.1 ticket as RPCN lays it out: body with the ten fields and two empty
 * items, then the signature section. */
static size_t tk_item(uint8_t* p, uint16_t type, const void* d, uint16_t n)
{
    p[0] = (uint8_t)(type >> 8); p[1] = (uint8_t)type; p[2] = (uint8_t)(n >> 8); p[3] = (uint8_t)n;
    if (n && d) memcpy(p + 4, d, n);
    return 4 + (size_t)n;
}
static size_t build_ticket(uint8_t* t)
{
    uint8_t body[512]; size_t b = 0;
    uint8_t serial[20]; memset(serial, 0x5A, 20);
    const uint8_t issuer[4] = { 0, 0, 0x01, 0x00 };
    const uint8_t issued[8] = { 0, 0, 0x01, 0x8F, 0, 0, 0, 1 };
    const uint8_t expire[8] = { 0, 0, 0x01, 0x8F, 0, 0, 0, 2 };
    const uint8_t account[8] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
    uint8_t online[32] = "Tester_1";
    const uint8_t status[4] = { 0, 0, 0, 7 };
    uint8_t service[24] = "UP9000-BCUS98107_00";
    b += tk_item(body + b, 8, serial, 20);
    b += tk_item(body + b, 1, issuer, 4);
    b += tk_item(body + b, 7, issued, 8);
    b += tk_item(body + b, 7, expire, 8);
    b += tk_item(body + b, 2, account, 8);
    b += tk_item(body + b, 4, online, 32);
    b += tk_item(body + b, 8, "us\0\0", 4);
    b += tk_item(body + b, 4, "un\0\0", 4);
    b += tk_item(body + b, 8, service, 24);
    b += tk_item(body + b, 1, status, 4);
    b += tk_item(body + b, 0, NULL, 0);
    b += tk_item(body + b, 0, NULL, 0);
    uint8_t sig[16]; size_t s = tk_item(sig, 8, "SIG!", 4);
    size_t o = 8;
    o += tk_item(t + o, 0x3000, body, (uint16_t)b);
    o += tk_item(t + o, 0x3002, sig, (uint16_t)s);
    const uint32_t ver = 0x21010000u, sz = (uint32_t)(o - 8);
    for (int i = 0; i < 4; i++) { t[i] = (uint8_t)(ver >> (24 - 8 * i)); t[4 + i] = (uint8_t)(sz >> (24 - 8 * i)); }
    return o;
}

int main(void)
{
    /* --- config ---------------------------------------------------------- */
    port_online_config c;
    const char* ini =
        "# comment\r\n"
        "[rpcn]\n"
        "rpcn_host = np.example.net:40000\r\n"
        "rpcn_user=Tester_1\n"
        "rpcn_password = p@ss=word\n"
        "rpcn_token=ABCD\n"
        "dns=127.0.0.1\n"
        "host.Resistance.Example.com = 10.1.2.3\n"
        "host.bad = 1.2.3\n"
        "bogus=1\n";
    assert(port_online_parse_config(ini, &c) == 6);
    assert(!strcmp(c.rpcn_host, "np.example.net") && c.rpcn_port == 40000);
    assert(!strcmp(c.rpcn_user, "Tester_1") && !strcmp(c.rpcn_password, "p@ss=word"));
    assert(!strcmp(c.rpcn_token, "ABCD"));
    assert(c.dns_server == ip4(127, 0, 0, 1));
    assert(c.n_hosts == 1 && c.hosts[0].ip == ip4(10, 1, 2, 3));
    assert(port_online_parse_config("rpcn_host=np.rpcs3.net\n", &c) == 1 && c.rpcn_port == RPCN_DEFAULT_PORT);
    assert(port_online_parse_config("rpcn_host=x:99999\n", &c) == 0);

    /* --- DNS wire format -------------------------------------------------- */
    uint8_t q[300];
    int ql = port_dns_build_query(0x1234, "a.bc", q, sizeof(q));
    const uint8_t want[] = { 0x12, 0x34, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 'a', 2, 'b', 'c', 0, 0, 1, 0, 1 };
    assert(ql == (int)sizeof(want) && !memcmp(q, want, sizeof(want)));
    assert(port_dns_build_query(1, "a..b", q, sizeof(q)) < 0);
    assert(port_dns_build_query(1, "", q, sizeof(q)) < 0);
    uint32_t ip = 0;
    assert(!port_dns_parse_a(q, (size_t)ql, 0x1234, &ip));      /* a query, not a response */

    /* --- SHA3-256, HMAC, PBKDF2 (FIPS 202 / NIST / RFC 8018 vectors) ------- */
    uint8_t d[32]; char hx[65];
    port_sha3_256(NULL, 0, d); hex(d, 32, hx);
    assert(!strcmp(hx, "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a"));
    port_sha3_256((const uint8_t*)"abc", 3, d); hex(d, 32, hx);
    assert(!strcmp(hx, "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532"));
    {
        uint8_t a200[200]; memset(a200, 'a', sizeof(a200));   /* crosses the 136-byte rate */
        port_sha3_256(a200, sizeof(a200), d); hex(d, 32, hx);
        assert(!strcmp(hx, "cce34485baf2bf2aca99b94833892a4f52896d3d153f7b840cc4f9fe695f1387"));
        uint8_t key[32]; for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
        const char* m = "Sample message for keylen<blocklen";
        port_hmac_sha3_256(key, 32, (const uint8_t*)m, strlen(m), d); hex(d, 32, hx);
        assert(!strcmp(hx, "4fe8e202c4f058e8dddc23d8c34e467343e23555e24fc2f025d598f558f67205"));
        port_pbkdf2_sha3_256((const uint8_t*)"pw", 2, (const uint8_t*)"salt", 4, 2, d); hex(d, 32, hx);
        assert(!strcmp(hx, "7707a0647b20a494f286cd484e35b19e86abd7b79c3dfb23592460ee3ea8512d"));
        uint8_t k200[200]; memset(k200, 'k', sizeof(k200));   /* key longer than a block */
        port_pbkdf2_sha3_256(k200, sizeof(k200), (const uint8_t*)"salt", 4, 3, d); hex(d, 32, hx);
        assert(!strcmp(hx, "a0c51840c29c7de3e2a0aacdf167b269c03f133d0c65769befd852edb3ddddb3"));
    }
    /* What RPCS3's account dialog stores for "secret"; stored ones pass through. */
    char pw[65];
    port_rpcn_password("secret", pw);
    assert(!strcmp(pw, "6BCEF5AF4C94598528E70636054F01DFA8B38DF18CDF2256773660C51C537B0A"));
    port_rpcn_password("6BCEF5AF4C94598528E70636054F01DFA8B38DF18CDF2256773660C51C537B0A", pw);
    assert(!strcmp(pw, "6BCEF5AF4C94598528E70636054F01DFA8B38DF18CDF2256773660C51C537B0A"));
    port_rpcn_password("6BCEF5AF4C94598528E70636054F01DFA8B38DF18CDF2256773660C51C537B0G", pw);
    assert(strcmp(pw, "6BCEF5AF4C94598528E70636054F01DFA8B38DF18CDF2256773660C51C537B0G") != 0 && strlen(pw) == 64);

    /* --- off unless PORT_ONLINE ------------------------------------------- */
    port_online_register();
    assert(!g_net_resolve_hook && !g_np_ticket_source && !port_online_signed_in());

    /* --- PORT_ONLINE with a config and a loopback DNS --------------------- */
    po_net_startup();
    s_mock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(s_mock, (struct sockaddr*)&sa, sizeof(sa)) == 0);
    socklen_t sl = sizeof(sa);
    getsockname(s_mock, (struct sockaddr*)&sa, &sl);
    s_dns_port = ntohs(sa.sin_port);

    FILE* f = fopen("test_port_online.ini", "wb");
    assert(f);
    fputs("rpcn_user=Tester_1\nrpcn_password=secret\ndns=127.0.0.1\nhost.svo.example=10.9.8.7\n", f);
    fclose(f);
    set_env("PORT_ONLINE_CONFIG=test_port_online.ini");
    set_env("PORT_ONLINE=1");
    port_online_register();
    assert(getenv("PS3_NET_ONLINE"));
    assert(g_net_resolve_hook == port_online_resolve && port_online_signed_in());
    assert(!strcmp(g_np_online_id_hook(), "Tester_1"));
    assert(g_netctl_dns_override == ip4(127, 0, 0, 1));   /* reported as cellNetCtl's primary DNS */
    assert(g_np_ticket_source == &s_ticket_source);

    assert(port_online_resolve("SVO.example", &ip) && ip == ip4(10, 9, 8, 7));   /* case-insensitive */
    start_mock();
    assert(port_online_resolve("game.example", &ip) && ip == ip4(203, 0, 113, 9));
    assert(!port_online_resolve("missing.example", &ip));                       /* NXDOMAIN -> system */
    po_close(s_mock);
    remove("test_port_online.ini");

    /* --- RPCN wire format ------------------------------------------------- */
    uint8_t pkt[256];
    int n = rpcn_build_login(pkt, sizeof(pkt), 0x0102030405060708ull, "Tester_1", "pw", "");
    const uint8_t login[] = { 0, 0, 0, 28, 0, 0, 0, 8, 7, 6, 5, 4, 3, 2, 1,
                              'T','e','s','t','e','r','_','1',0, 'p','w',0, 0 };
    assert(n == (int)sizeof(login) && !memcmp(pkt, login, sizeof(login)));
    uint8_t type; uint16_t cmd; uint32_t total; uint64_t pid;
    assert(rpcn_parse_header(pkt, (size_t)n, &type, &cmd, &total, &pid) == RPCN_HEADER_SIZE);
    assert(type == RPCN_PKT_REQUEST && cmd == RPCN_CMD_LOGIN && total == 28 && pid == 0x0102030405060708ull);
    assert(rpcn_build_login(pkt, 20, 1, "Tester_1", "pw", "") < 0);

    const uint8_t cookie[3] = { 0xAA, 0xBB, 0xCC };
    n = rpcn_build_ticket_request(pkt, sizeof(pkt), 9, "UP9000-BCUS98107_00", cookie, 3);
    assert(n == RPCN_HEADER_SIZE + 20 + 4 + 3);
    assert(pkt[1] == RPCN_CMD_REQUEST_TICKET && pkt[2] == 0 && pkt[3] == (uint8_t)n);
    assert(!memcmp(pkt + 15, "UP9000-BCUS98107_00", 20));
    assert(pkt[35] == 3 && pkt[36] == 0 && pkt[37] == 0 && pkt[38] == 0 && pkt[39] == 0xAA && pkt[41] == 0xCC);

    uint8_t si[19];
    rpcn_write_header(si, RPCN_PKT_SERVERINFO, 0, 19, 0);
    si[15] = 34; si[16] = si[17] = si[18] = 0;
    assert(rpcn_parse_server_info(si, sizeof(si)) == RPCN_PROTOCOL_VERSION);
    si[0] = RPCN_PKT_REPLY;
    assert(rpcn_parse_server_info(si, sizeof(si)) == -1);

    char name[32];
    const uint8_t lr[] = { 0, 'T','e','s','t','e','r','_','1',0, 'h','t','t','p',0, 1,0,0,0,0,0,0,0, 0,0,0,0 };
    assert(rpcn_parse_login_reply(lr, sizeof(lr), name, sizeof(name)) == 0 && !strcmp(name, "Tester_1"));
    const uint8_t lbad[] = { 7 };
    assert(rpcn_parse_login_reply(lbad, 1, name, sizeof(name)) == 7 && !name[0]);
    assert(rpcn_parse_login_reply(lr, 4, name, sizeof(name)) == -1);   /* unterminated name */
    const uint8_t tr[] = { 0, 3, 0, 0, 0, 'a', 'b', 'c' };
    const uint8_t* tk; uint32_t tl;
    assert(rpcn_parse_ticket_reply(tr, sizeof(tr), &tk, &tl) == 0 && tl == 3 && tk == tr + 5);
    assert(rpcn_parse_ticket_reply(tr, sizeof(tr) - 1, &tk, &tl) == -1);
    assert(rpcn_parse_ticket_reply(lbad, 1, &tk, &tl) == 7);

    /* --- ticket 2.1 fields, in guest byte order --------------------------- */
    static uint8_t tkb[1024];
    size_t tkl = build_ticket(tkb);
    uint8_t out[256];
    assert(port_ticket_check(tkb, tkl) == 0);
    assert(port_ticket_param(tkb, tkl, 1, out) == 0 && out[2] == 0x01 && out[3] == 0 && out[4] == 0);
    assert(port_ticket_param(tkb, tkl, 4, out) == 0 && out[0] == 0x11 && out[7] == 0x88);
    assert(port_ticket_param(tkb, tkl, 5, out) == 0 && !strcmp((char*)out, "Tester_1"));
    assert(port_ticket_param(tkb, tkl, 8, out) == 0 && !strcmp((char*)out, "UP9000-BCUS98107_00"));
    assert(port_ticket_param(tkb, tkl, 9, out) == 0 && out[3] == 7);
    out[0] = 1;
    assert(port_ticket_param(tkb, tkl, 11, out) == 0 && out[0] == 0);
    assert(port_ticket_param(tkb, tkl, 12, out) < 0);
    assert(port_ticket_check(tkb, tkl - 1) < 0);                      /* size field mismatch */
    tkb[12 + 1] = 9;                                                   /* serial's type 8 -> 9 */
    assert(port_ticket_param(tkb, tkl, 1, out) < 0);
    tkb[12 + 1] = 8;
    tkb[12 + 3] = 19;                                                  /* serial 20 -> 19 bytes */
    assert(port_ticket_check(tkb, tkl) < 0);
    tkb[12 + 3] = 20;

    /* --- the ticket source sceNp.c calls ---------------------------------- */
    const np_ticket_source* src = g_np_ticket_source;
    const uint8_t* got; uint32_t got_len;
    s_ticket_sync = 1;
    assert(src->get(&got, &got_len) == 0);                              /* none yet: made-up ticket */
    /* No rpcn_host configured: the request is taken and fails with an event. */
    assert(src->request("UP9000-BCUS98107_00", cookie, 3) == 1);
    assert(s_delivered == 1 && (uint32_t)s_delivered_result == SCE_NP_ERROR_OFFLINE);
    assert((uint32_t)src->get(&got, &got_len) == SCE_NP_ERROR_OFFLINE);
    assert((uint32_t)src->request("x", cookie, NP_TICKET_COOKIE_MAX + 1) == SCE_NP_ERROR_INVALID_ARGUMENT);

    strcpy(s_cfg.rpcn_host, "rpcn.invalid");
    s_session_fn = fake_session;
    /* "TICKET" is not a 2.1 ticket: refused, not handed to the game. */
    assert(src->request("UP9000-BCUS98107_00", cookie, 2) == 1);
    assert(!strcmp(s_fake_service, "UP9000-BCUS98107_00") && s_fake_cookie_size == 2 && s_fake_cookie0 == 0xAA);
    assert(s_delivered == 2 && (uint32_t)s_delivered_result == SCE_NP_ERROR_OFFLINE);

    s_fake_ticket = tkb; s_fake_ticket_len = (uint32_t)tkl;
    assert(src->request("UP9000-BCUS98107_00", NULL, 0) == 1);
    assert(s_delivered == 3 && s_delivered_result == (s32)tkl);
    assert(src->get(&got, &got_len) == 1 && got_len == tkl && !memcmp(got, tkb, tkl));
    assert(src->param(got, got_len, 5, out) == 0 && !strcmp((char*)out, "Tester_1"));

    s_fake_fail = 1;
    assert(src->request("UP9000-BCUS98107_00", NULL, 0) == 1);
    assert(s_delivered == 4 && (uint32_t)s_delivered_result == SCE_NP_ERROR_OFFLINE);
    assert((uint32_t)src->get(&got, &got_len) == SCE_NP_ERROR_OFFLINE);

    /* Without an account the made-up ticket answers (servers that do not check it). */
    s_cfg.rpcn_password[0] = '\0';
    assert(src->request("UP9000-BCUS98107_00", NULL, 0) == 0 && s_delivered == 4);

    printf("test_port_online: all passed\n");
    return 0;
}
