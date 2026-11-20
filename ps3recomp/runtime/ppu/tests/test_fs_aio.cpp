/*
 * ps3recomp - cellFsAioRead on a shared fd (ppu_fs.cpp).
 *
 * An AIO read names its own offset. It must not move the fd's position for
 * the plain cellFsRead that follows, and several guest threads (FIOS media
 * threads, audio streamers) issue them on one fd at once. The fixture writes
 * a file whose every 4-byte word holds its own offset, then checks both:
 * a cellFsRead after an AIO read continues from the fd's position, and
 * reads racing from four threads each get the words they asked for. Then a
 * stress run: eight threads opening, reading, seeking, fstat-ing, AIO-reading
 * and closing shared files for two seconds, once on their own fds and once
 * with threads closing random fds under them (ops answer OK or EBADF only).
 *
 *   tools/test_ppu_fs_aio.py
 *   test_fs_aio [DIR]     (DIR: where the file goes; default the temp dir)
 */
#include "ppu_fs.cpp"
#include <chrono>
#include <thread>
#include <vector>

/* ---- link stubs: nothing on the paths this test drives calls them ---- */
extern "C" {
uint8_t* vm_base = nullptr;
uint32_t ppu_vm_size = 0;
void vm_write32(uint64_t a, uint32_t v) { uint32_t be = __builtin_bswap32(v); memcpy(vm_base + a, &be, 4); }
void vm_write64(uint64_t a, uint64_t v) { uint64_t be = __builtin_bswap64(v); memcpy(vm_base + a, &be, 8); }
void ppu_guest_caller(char* out, size_t n) { if (n) out[0] = 0; }
void ps3_vfs_ps3game_fallback(char* path, size_t cap) { (void)path; (void)cap; }
void ydkj_host_bt(const char* tag) { (void)tag; }
void ps3_hle_register_ctx(uint32_t, const char*, void (*)(ppu_context*)) {}
}
ps3_guest_caller_fn g_ps3_guest_caller = nullptr;
const char* edat_resolve(const char* host_path, char* buf, size_t cap) { (void)buf; (void)cap; return host_path; }

static uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, vm_base + a, 4); return __builtin_bswap32(v); }
static uint64_t rd64(uint32_t a) { uint64_t v; memcpy(&v, vm_base + a, 8); return __builtin_bswap64(v); }
static void wr32(uint32_t a, uint32_t v) { vm_write32(a, v); }
static void wr64(uint32_t a, uint64_t v) { vm_write64(a, v); }

static int fails = 0;
#define CHECK(c, ...) do { printf((c) ? "[PASS] " : "[FAIL] "); printf(__VA_ARGS__); printf("\n"); \
                           if (!(c)) fails++; } while (0)

static const uint32_t WORDS = 1u << 16;   /* 256 KiB file */

/* One AIO read of `len` bytes at `off` into guest `buf`, through a CellFsAio
 * at guest `aio`: {fd u32, pad, offset u64, buf u32, pad, size u64}. */
static void aio_read(uint32_t aio, uint32_t fd, uint64_t off, uint32_t buf, uint64_t len)
{
    wr32(aio + 0x00, fd); wr64(aio + 0x08, off); wr32(aio + 0x10, buf); wr64(aio + 0x18, len);
    ppu_context ctx; memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = aio; ctx.gpr[4] = 0; ctx.gpr[5] = 0;
    cellFsAioRead(&ctx);
}

/* Every word at file offset o holds o (big-endian), so a buffer read from
 * offset `off` is right iff word i holds off + 4*i. */
static bool holds(uint32_t buf, uint64_t off, uint32_t len)
{
    for (uint32_t i = 0; i < len; i += 4) if (rd32(buf + i) != (uint32_t)(off + i)) return false;
    return true;
}

/* ---- 3. open / read / seek / fstat / AIO / close racing on shared fds ----
 *
 * The fd table was unlocked: two concurrent opens could get the same fd, and a
 * close could fclose a stream another thread was reading (UCRT invalid
 * parameter 0xC0000417 -- Resistance 3's FIOS media thread). Phase A: owner
 * threads open/read/seek/close their own fds on shared files; no fd may be
 * handed to two threads at once and every read returns its own file's bytes.
 * Phase B: the same plus threads closing random fds under the readers; every
 * call must answer CELL_OK or CELL_FS_EBADF and nothing may crash. */
static const uint32_t EBADF_RC = 0x8001002Au;
static const int SFILES = 4;
static const uint32_t SWORDS = 1u << 14;   /* 64 KiB per file; word at offset o = (id << 24) | o */

static bool holds_id(uint32_t buf, int id, uint32_t off, uint32_t len)
{
    for (uint32_t i = 0; i < len; i += 4) if (rd32(buf + i) != (((uint32_t)id << 24) | (off + i))) return false;
    return true;
}

static uint32_t s_call(void (*fn)(ppu_context*), uint64_t a, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0)
{
    ppu_context x; memset(&x, 0, sizeof x);
    x.gpr[3] = a; x.gpr[4] = b; x.gpr[5] = c; x.gpr[6] = d;
    fn(&x);
    return (uint32_t)x.gpr[3];
}

static void stress(const char* dir)
{
    char host[SFILES][1100];
    for (int id = 0; id < SFILES; id++) {
        snprintf(host[id], sizeof host[id], "%s/ps3recomp_fs_race_%d.bin", dir, id);
        FILE* f = fopen(host[id], "wb");
        if (!f) { CHECK(false, "could not create %s", host[id]); return; }
        for (uint32_t i = 0; i < SWORDS; i++) { uint32_t be = __builtin_bswap32(((uint32_t)id << 24) | (i * 4)); fwrite(&be, 4, 1, f); }
        fclose(f);
        snprintf((char*)vm_base + 0xA00 + 0x40 * id, 0x40, "/dev_hdd1/ps3recomp_fs_race_%d.bin", id);
    }
    /* thousands of "[fs] open" lines would drown the result */
    fflush(stderr);
#ifdef _WIN32
    freopen("NUL", "w", stderr);
#else
    freopen("/dev/null", "w", stderr);
#endif

    const int THREADS = 8;
    for (int phase = 0; phase < 2; phase++) {
        static std::atomic<int> owner[FS_MAX];
        for (auto& o : owner) o = 0;
        std::atomic<bool> stop{false};
        std::atomic<long> ops{0}, badrc{0}, dup{0}, wrong{0}, ebadf{0};
        std::vector<std::thread> ts;
        for (int t = 0; t < THREADS; t++)
            ts.emplace_back([&, t] {
                const uint32_t scratch = 0x800 + 0x40 * t, aio = 0xC00 + 0x40 * t, buf = 0x20000 + 0x8000 * t;
                uint32_t seed = 0x9E3779B9u * (t + 1) + phase;
                auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
                auto rc_ok = [&](uint32_t rc) {
                    ops++;
                    if (rc == EBADF_RC) ebadf++;
                    else if (rc != 0) badrc++;
                    return rc == 0;
                };
                const bool closer = phase == 1 && t >= THREADS - 2;
                while (!stop.load(std::memory_order_relaxed)) {
                    if (closer) {   /* close somebody else's fd, or a dead one */
                        rc_ok(s_call(cellFsClose, 3 + rnd() % 24));
                        std::this_thread::yield();
                        continue;
                    }
                    const int id = (int)(rnd() % SFILES);
                    if (!rc_ok(s_call(cellFsOpen, 0xA00 + 0x40 * id, 0, scratch))) continue;
                    const uint32_t fd = rd32(scratch);
                    if (phase == 0) {
                        int z = 0;
                        if (fd >= FS_MAX || !owner[fd].compare_exchange_strong(z, t + 1)) dup++;
                    }
                    for (int k = 0, n = 1 + (int)(rnd() % 6); k < n; k++) {
                        const uint32_t len = 4 * (1 + rnd() % 0x400);
                        const uint32_t off = (rnd() % (SWORDS * 4 - len)) & ~3u;
                        switch (rnd() % 4) {
                        case 0: case 1:
                            if (rc_ok(s_call(cellFsLseek, fd, off, 0, scratch + 8)) &&
                                rc_ok(s_call(cellFsRead, fd, buf, len, scratch + 16)) && phase == 0) {
                                const uint64_t got = rd64(scratch + 16);
                                for (uint32_t i = 0; i < got; i += 4)
                                    if (rd32(buf + i) != (((uint32_t)id << 24) | (off + i))) { wrong++; break; }
                                if (got != len) wrong++;
                            }
                            break;
                        case 2: {
                            const uint32_t sb = 0x7F000 + 0x40 * t;
                            if (rc_ok(s_call(cellFsFstat, fd, sb)) && phase == 0 && rd64(sb + 0x24) != SWORDS * 4) wrong++;
                            break;
                        }
                        default:
                            wr32(aio + 0x00, fd); wr64(aio + 0x08, off); wr32(aio + 0x10, buf); wr64(aio + 0x18, len);
                            rc_ok(s_call(cellFsAioRead, aio));
                            if (phase == 0 && !holds_id(buf, id, off, len)) wrong++;
                            break;
                        }
                    }
                    if (phase == 0) owner[fd < FS_MAX ? fd : 0] = 0;
                    rc_ok(s_call(cellFsClose, fd));
                }
            });
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        stop = true;
        for (auto& th : ts) th.join();
        const char* name = phase == 0 ? "A (own fds)" : "B (+ random closers)";
        CHECK(badrc == 0 && dup == 0 && wrong == 0 && ops > 1000,
              "stress %s: %ld calls over %d threads, %ld EBADF, %ld other errors, %ld fds handed out twice, %ld wrong reads",
              name, ops.load(), THREADS, ebadf.load(), badrc.load(), dup.load(), wrong.load());
        if (phase == 0) CHECK(ebadf == 0, "stress A: no EBADF on a thread's own fds (%ld)", ebadf.load());
    }
    /* everything closed: the table is empty again */
    int left = 0;
    for (int i = 0; i < FS_MAX; i++) if (g_fds[i]) { left++; s_call(cellFsClose, i); }
    CHECK(left == 0, "no fd leaked after the stress (%d left open)", left);
    for (int id = 0; id < SFILES; id++) remove(host[id]);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    static uint8_t mem[1 << 20];
    vm_base = mem; ppu_vm_size = sizeof mem;

    const char* dir = argc > 1 ? argv[1] : getenv("TMPDIR");
    if (!dir) dir = "/tmp";
    char host[1100];
    snprintf(host, sizeof host, "%s/ps3recomp_fs_aio.bin", dir);
    {
        FILE* f = fopen(host, "wb");
        if (!f) { printf("FAIL: could not create %s\n", host); return 1; }
        for (uint32_t i = 0; i < WORDS; i++) { uint32_t be = __builtin_bswap32(i * 4); fwrite(&be, 4, 1, f); }
        fclose(f);
    }

    ppu_vfs_root = dir;
    strcpy((char*)mem + 0x100, "/dev_hdd1/ps3recomp_fs_aio.bin");
    ppu_context ctx;
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = 0x100; ctx.gpr[4] = 0 /* O_RDONLY */; ctx.gpr[5] = 0x200;
    cellFsOpen(&ctx);
    const uint32_t fd = rd32(0x200);
    CHECK((uint32_t)ctx.gpr[3] == 0, "cellFsOpen returns CELL_OK (fd %u)", fd);
    if ((uint32_t)ctx.gpr[3] != 0) { remove(host); return 1; }

    /* 1. The fd's position survives an AIO read. */
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = fd; ctx.gpr[4] = 0x40; ctx.gpr[5] = 0 /* SET */; ctx.gpr[6] = 0x208;
    cellFsLseek(&ctx);
    aio_read(0x300, fd, 0x8000, 0x1000, 0x100);
    CHECK(holds(0x1000, 0x8000, 0x100), "the AIO read returns the words at its own offset");
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = fd; ctx.gpr[4] = 0x2000; ctx.gpr[5] = 16; ctx.gpr[6] = 0x210;
    cellFsRead(&ctx);
    CHECK(rd64(0x210) == 16 && holds(0x2000, 0x40, 16),
          "cellFsRead after it continues from the fd's position 0x40 (got word 0x%08X)", rd32(0x2000));

    /* 2. AIO reads racing on the same fd each get their own bytes. */
    const int THREADS = 4, ROUNDS = 2000;
    const uint32_t LEN = 0x400;
    std::vector<int> bad(THREADS, 0);
    std::vector<std::thread> ts;
    for (int t = 0; t < THREADS; t++)
        ts.emplace_back([&, t] {
            const uint32_t aio = 0x400 + 0x40 * t, buf = 0x10000 + 0x10000 * t;
            uint32_t seed = 0x9E3779B9u * (t + 1);
            for (int r = 0; r < ROUNDS; r++) {
                seed = seed * 1664525u + 1013904223u;
                const uint64_t off = (uint64_t)(seed % (WORDS * 4 - LEN)) & ~3ull;
                aio_read(aio, fd, off, buf, LEN);
                if (!holds(buf, off, LEN)) bad[t]++;
            }
        });
    for (auto& th : ts) th.join();
    int total = 0; for (int b : bad) total += b;
    CHECK(total == 0, "%d racing AIO reads over %d threads, %d returned the wrong bytes",
          THREADS * ROUNDS, THREADS, total);

    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = fd;
    cellFsClose(&ctx);
    CHECK((uint32_t)ctx.gpr[3] == 0, "cellFsClose returns CELL_OK");
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = fd;
    cellFsClose(&ctx);
    CHECK((uint32_t)ctx.gpr[3] == EBADF_RC, "closing it again is CELL_FS_EBADF (0x%08X)", (uint32_t)ctx.gpr[3]);
    memset(&ctx, 0, sizeof ctx); ctx.gpr[3] = fd; ctx.gpr[4] = 0x2000; ctx.gpr[5] = 16; ctx.gpr[6] = 0x210;
    cellFsRead(&ctx);
    CHECK((uint32_t)ctx.gpr[3] == EBADF_RC, "reading a closed fd is CELL_FS_EBADF (0x%08X)", (uint32_t)ctx.gpr[3]);
    remove(host);

    stress(dir);
    printf("%s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
