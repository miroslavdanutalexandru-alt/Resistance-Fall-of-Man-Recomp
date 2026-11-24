/*
 * test_np_ticket - sceNpManagerRequestTicket / GetTicket / GetTicketParam.
 *
 * A title that logs in to its own servers requests a ticket, waits for the
 * NP manager callback to report SCE_NP_MANAGER_EVENT_GOT_TICKET, then copies
 * the ticket out and reads its fields. This drives the three through the NIDs
 * they register, as a lifted title calls them.
 *
 *   cc -std=gnu17 -I include -o test_np_ticket libs/network/tests/test_np_ticket.c
 * Driven by tools/test_np_ticket.py.
 */
#include "../sceNp.c"
#include <assert.h>
#include <stdlib.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}
void edat_add_klicensee(const uint8_t klic[16]) { (void)klic; }

static const char* s_online_id = "BraveOtter";
int np_psnr_enabled(void) { return 1; }
const char* np_psnr_online_id(void) { return s_online_id; }
static const char* s_key;   /* NULL: no salt, the account derives from the name */
const char* np_psnr_identity_key(void) { return s_key ? s_key : s_online_id; }

/* The callbacks the title would be handed on its next cellSysutilCheckCallback. */
static struct { u32 opd; u64 args[8]; } s_queued[8];
static int s_nqueued;
s32 cellSysutilQueueGuestCallbackArgs(u32 opd, const u64 args[8])
{
    assert(s_nqueued < 8);
    s_queued[s_nqueued].opd = opd;
    memcpy(s_queued[s_nqueued].args, args, sizeof s_queued[0].args);
    s_nqueued++;
    return 0;
}

static struct { uint32_t nid; void (*fn)(ppu_context*); } s_reg[8];
static int s_nreg;
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*))
{
    (void)name;
    s_reg[s_nreg].nid = nid;
    s_reg[s_nreg].fn = fn;
    s_nreg++;
}

static s32 call(uint32_t nid, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7, u64 a8)
{
    ppu_context ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.gpr[3] = a3; ctx.gpr[4] = a4; ctx.gpr[5] = a5;
    ctx.gpr[6] = a6; ctx.gpr[7] = a7; ctx.gpr[8] = a8;
    for (int i = 0; i < s_nreg; i++)
        if (s_reg[i].nid == nid) { s_reg[i].fn(&ctx); return (s32)ctx.gpr[3]; }
    fprintf(stderr, "no handler for NID 0x%08X\n", nid);
    abort();
}

#define REQUEST_TICKET 0x7E2FEF28u
#define GET_TICKET     0x0968AA36u
#define GET_PARAM      0x58FA4FCDu

/* Guest addresses. */
enum { NPID = 0x1000, SERVICE = 0x1100, SIZE = 0x1200, TICKET = 0x2000, PARAM = 0x3000,
       CB = 0x8000, CB_ARG = 0x9000 };

static void set_online(int on)
{
#ifdef _WIN32
    _putenv_s("PS3_NET_ONLINE", on ? "1" : "");
#else
    if (on) setenv("PS3_NET_ONLINE", "1", 1); else unsetenv("PS3_NET_ONLINE");
#endif
}

static int fails;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); fails++; } } while (0)

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    vm_base = (uint8_t*)calloc(1, 1u << 20);
    ppu_vm_size = 1u << 20;
    /* Offline nothing is registered, so the calls behave as before. */
    np_register_ctx();
    CHECK(s_nreg == 0);
    set_online(1);
    np_register_ctx();
    CHECK(s_nreg == 3);

    strcpy((char*)vm_base + SERVICE, "UP0001-NPXX00000_00");

    /* Before sceNpInit: nothing works. */
    CHECK((u32)call(REQUEST_TICKET, NPID, SERVICE, 0, 0, 0, 0) == SCE_NP_ERROR_NOT_INITIALIZED);
    CHECK(sceNpInit(0x100000, NULL) == CELL_OK);

    /* Online is signed in with no psnr server (a Medius title); offline is
     * not. */
    CHECK(getenv("PSNR_SERVER") == NULL);
    CHECK(sceNpManagerGetStatus((s32*)(uintptr_t)PARAM) == CELL_OK);
    CHECK((s32)vm_read32(PARAM) == SCE_NP_MANAGER_STATUS_ONLINE);
    set_online(0);
    CHECK(sceNpManagerGetStatus((s32*)(uintptr_t)PARAM) == CELL_OK);
    CHECK((s32)vm_read32(PARAM) == SCE_NP_MANAGER_STATUS_OFFLINE);
    set_online(1);

    /* Before any RequestTicket a ticket can already be read, as on a console
     * signed in at boot: Resistance: Fall of Man calls GetTicket with
     * *bufferSize = 0, then GetTicketParam 6, 9, 2, 3, and only goes online
     * if those work. It has no service ID yet. */
    vm_write32(SIZE, 0);
    CHECK(call(GET_TICKET, TICKET, SIZE, 0, 0, 0, 0) == CELL_OK);
    CHECK(vm_read32(SIZE) == NP_TICKET_SIZE);
    CHECK(memcmp(vm_base + TICKET, "PS3RECOMP-NP-TICKET", 19) == 0);
    CHECK(call(GET_PARAM, SCE_NP_TICKET_PARAM_SUBJECT_REGION, PARAM, 0, 0, 0, 0) == CELL_OK);
    CHECK(call(GET_PARAM, SCE_NP_TICKET_PARAM_SUBJECT_STATUS, PARAM, 0, 0, 0, 0) == CELL_OK);
    CHECK(call(GET_PARAM, SCE_NP_TICKET_PARAM_ISSUED_DATE, PARAM, 0, 0, 0, 0) == CELL_OK);
    CHECK(vm_read64(PARAM) != 0);
    CHECK(call(GET_PARAM, SCE_NP_TICKET_PARAM_SERVICE_ID, PARAM, 0, 0, 0, 0) == CELL_OK);
    CHECK(vm_base[PARAM] == 0);

    /* A request with no service ID is refused. */
    CHECK((u32)call(REQUEST_TICKET, NPID, 0, 0, 0, 0, 0) == SCE_NP_ERROR_INVALID_ARGUMENT);

    /* RequestTicket reports completion through the manager callback only.
     * r7 is entitlementId (a string), never a callback: a non-zero value must
     * not be called. */
    CHECK(sceNpManagerRegisterCallback((SceNpManagerCallback)(uintptr_t)CB,
                                       (void*)(uintptr_t)CB_ARG) == CELL_OK);
    s_nqueued = 0;   /* registering may queue its own sign-in event */
    CHECK(call(REQUEST_TICKET, NPID, SERVICE, 0, 0, 0x4000 /* entitlementId */, 1) == CELL_OK);
    CHECK(s_nqueued == 1);
    CHECK(s_queued[0].opd == CB);
    CHECK(s_queued[0].args[0] == SCE_NP_MANAGER_EVENT_GOT_TICKET);
    CHECK(s_queued[0].args[1] == 0);
    CHECK(s_queued[0].args[2] == CB_ARG);

    /* GetTicket: a NULL buffer asks for the size; *bufferSize is output
     * only, so whatever it held, the ticket is copied and its size written. */
    vm_write32(SIZE, 0);
    CHECK(call(GET_TICKET, 0, SIZE, 0, 0, 0, 0) == CELL_OK);
    u32 size = vm_read32(SIZE);
    CHECK(size == NP_TICKET_SIZE);
    CHECK((u32)call(GET_TICKET, TICKET, 0, 0, 0, 0, 0) == SCE_NP_ERROR_INVALID_ARGUMENT);
    memset(vm_base + TICKET, 0xA5, 0x400);
    vm_write32(SIZE, 0);
    CHECK(call(GET_TICKET, TICKET, SIZE, 0, 0, 0, 0) == CELL_OK);
    CHECK(vm_read32(SIZE) == size);
    CHECK(memcmp(vm_base + TICKET, "PS3RECOMP-NP-TICKET", 19) == 0);
    CHECK(memcmp(vm_base + TICKET + 24, "BraveOtter", 10) == 0);
    CHECK(vm_base[TICKET + size] == 0xA5);   /* nothing past the ticket */

    /* GetTicketParam. */
    CHECK((u32)call(GET_PARAM, 2, 0, 0, 0, 0, 0) == SCE_NP_ERROR_INVALID_ARGUMENT);   /* NULL out */
    CHECK((u32)call(GET_PARAM, 12, PARAM, 0, 0, 0, 0) == SCE_NP_ERROR_INVALID_ARGUMENT);
    CHECK((u32)call(GET_PARAM, (u64)(s64)-1, PARAM, 0, 0, 0, 0) == SCE_NP_ERROR_INVALID_ARGUMENT);

    /* Issued now, expiring later: a title takes expire - issued as the
     * lifetime, so both must be real dates and in order. */
    u64 now_ms = (u64)time(NULL) * 1000ull;
    CHECK(call(GET_PARAM, SCE_NP_TICKET_PARAM_ISSUED_DATE, PARAM, 0, 0, 0, 0) == CELL_OK);
    u64 issued = vm_read64(PARAM);
    CHECK(call(GET_PARAM, SCE_NP_TICKET_PARAM_EXPIRE_DATE, PARAM, 0, 0, 0, 0) == CELL_OK);
    u64 expire = vm_read64(PARAM);
    CHECK(issued <= now_ms && now_ms - issued < 60000ull);
    CHECK(expire > issued);

    /* The online ID, from offset 0 of the union, NUL-terminated. */
    memset(vm_base + PARAM, 0xA5, 256);
    CHECK(call(GET_PARAM, SCE_NP_TICKET_PARAM_SUBJECT_ONLINE_ID, PARAM, 0, 0, 0, 0) == CELL_OK);
    CHECK(strcmp((const char*)vm_base + PARAM, "BraveOtter") == 0);

    /* The service ID it was requested for, even after the title reuses the
     * buffer it passed. */
    strcpy((char*)vm_base + SERVICE, "overwritten");
    CHECK(call(GET_PARAM, SCE_NP_TICKET_PARAM_SERVICE_ID, PARAM, 0, 0, 0, 0) == CELL_OK);
    CHECK(strcmp((const char*)vm_base + PARAM, "UP0001-NPXX00000_00") == 0);

    /* The account ID is per player and stable. */
    CHECK(call(GET_PARAM, SCE_NP_TICKET_PARAM_SUBJECT_ACCOUNT_ID, PARAM, 0, 0, 0, 0) == CELL_OK);
    u64 account = vm_read64(PARAM);
    CHECK(account != 0);
    CHECK(call(GET_PARAM, SCE_NP_TICKET_PARAM_SUBJECT_ACCOUNT_ID, PARAM, 0, 0, 0, 0) == CELL_OK);
    CHECK(vm_read64(PARAM) == account);
    s_online_id = "QuietLynx";
    CHECK(call(GET_PARAM, SCE_NP_TICKET_PARAM_SUBJECT_ACCOUNT_ID, PARAM, 0, 0, 0, 0) == CELL_OK);
    CHECK(vm_read64(PARAM) != account);
    s_online_id = "BraveOtter";
    /* Same name, another salt: another account. */
    s_key = "BraveOtter#0123456789abcdef";
    CHECK(call(GET_PARAM, SCE_NP_TICKET_PARAM_SUBJECT_ACCOUNT_ID, PARAM, 0, 0, 0, 0) == CELL_OK);
    CHECK(vm_read64(PARAM) != account);
    s_key = NULL;

    /* Every known parameter answers; the serial matches the ticket's header. */
    for (s32 id = SCE_NP_TICKET_PARAM_SERIAL_ID; id <= SCE_NP_TICKET_PARAM_SUBJECT_DOB; id++)
        CHECK(call(GET_PARAM, (u64)id, PARAM, 0, 0, 0, 0) == CELL_OK);
    CHECK(call(GET_PARAM, SCE_NP_TICKET_PARAM_SERIAL_ID, PARAM, 0, 0, 0, 0) == CELL_OK);
    CHECK(memcmp(vm_base + PARAM, vm_base + TICKET, 20) == 0);

    /* No registered callback: the request still succeeds, and nothing is
     * queued to address 0. */
    CHECK(sceNpManagerUnregisterCallback() == CELL_OK);
    s_nqueued = 0;
    CHECK(call(REQUEST_TICKET, NPID, SERVICE, 0, 0, 0, 0) == CELL_OK);
    CHECK(s_nqueued == 0);

    if (fails) { printf("test_np_ticket: %d failed\n", fails); return 1; }
    printf("test_np_ticket: all passed\n");
    return 0;
}
