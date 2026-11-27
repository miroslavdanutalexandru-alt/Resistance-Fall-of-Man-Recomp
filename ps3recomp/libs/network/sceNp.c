/*
 * ps3recomp - sceNp HLE implementation
 *
 * Provides fake PSN identity so games can proceed through NP checks.
 * The username defaults to "PS3Player" but is configurable.
 */

#include "sceNp.h"
#include "../../runtime/ppu/ppu_context.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write*: guest EA -> host, byte-swapped */
#include "np_psnr.h"
#include "../system/cellSysutil.h"
#include "../filesystem/edat.h"


/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int  s_np_initialized = 0;
static int  s_score_initialized = 0;
static char s_fake_username[SCE_NP_ONLINEID_MAX_LENGTH + 1] = "PS3Player";

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/

void sceNpSetFakeUsername(const char* username)
{
    if (username) {
        strncpy(s_fake_username, GUEST_PTR(username, const char*),
            SCE_NP_ONLINEID_MAX_LENGTH);
        s_fake_username[SCE_NP_ONLINEID_MAX_LENGTH] = '\0';
    }
}

const char* np_fake_username(void)
{
    return s_fake_username;
}

/* Build a fake NP ID from the current username (PS3_NP_ONLINE_ID wins, so
 * two instances on one machine can be two players). */
static void np_build_fake_id(SceNpId* npId)
{
    memset(npId, 0, sizeof(SceNpId));
    strncpy(npId->handle.data, np_psnr_online_id(), SCE_NP_ONLINEID_MAX_LENGTH);
    npId->handle.term = '\0';
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 sceNpInit(u32 poolSize, void* poolPtr)
{
    (void)poolSize;
    (void)poolPtr;

    printf("[sceNp] Init(poolSize=%u, username=\"%s\")\n",
           poolSize, np_psnr_online_id());

    if (s_np_initialized)
        return SCE_NP_ERROR_ALREADY_INITIALIZED;

    s_np_initialized = 1;
    return CELL_OK;
}

/* Whether sceNpInit has run; sceNpBasic follows it (see sceNpBasic.c). */
int np_is_initialized(void) { return s_np_initialized; }

s32 sceNpTerm(void)
{
    printf("[sceNp] Term()\n");

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    s_np_initialized = 0;
    return CELL_OK;
}

s32 sceNpGetNpId(s32 userId, SceNpId* npId)
{
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!npId)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    /* A guest address, like every sibling getter's: writing through it
     * untranslated hit a host address and crashed Simpsons Arcade the first
     * time it asked for its NP ID online. */
    npId = GUEST_PTR(npId, SceNpId*);
    np_build_fake_id(npId);
    printf("[sceNp] GetNpId(user=%d) -> \"%s\"\n", userId, np_psnr_online_id());
    return CELL_OK;
}

s32 sceNpGetOnlineId(s32 userId, SceNpOnlineId* onlineId)
{
    onlineId = GUEST_PTR(onlineId, SceNpOnlineId*);
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!onlineId)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    memset(onlineId, 0, sizeof(SceNpOnlineId));
    strncpy(onlineId->data, np_psnr_online_id(), SCE_NP_ONLINEID_MAX_LENGTH);
    onlineId->term = '\0';

    printf("[sceNp] GetOnlineId(user=%d) -> \"%s\"\n", userId, np_psnr_online_id());
    return CELL_OK;
}

s32 sceNpGetOnlineName(s32 userId, SceNpOnlineName* onlineName)
{
    onlineName = GUEST_PTR(onlineName, SceNpOnlineName*);
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!onlineName)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    memset(onlineName, 0, sizeof(SceNpOnlineName));
    strncpy(onlineName->data, np_psnr_online_id(),
            SCE_NP_ONLINENAME_MAX_LENGTH - 1);

    printf("[sceNp] GetOnlineName(user=%d) -> \"%s\"\n",
           userId, np_psnr_online_id());
    return CELL_OK;
}

s32 sceNpGetUserProfile(s32 userId, SceNpUserInfo* userInfo)
{
    userInfo = GUEST_PTR(userInfo, SceNpUserInfo*);
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!userInfo)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    memset(userInfo, 0, sizeof(SceNpUserInfo));
    np_build_fake_id(&userInfo->npId);
    strncpy(userInfo->onlineName.data, np_psnr_online_id(),
            SCE_NP_ONLINENAME_MAX_LENGTH - 1);
    /* Leave avatar URL empty */

    printf("[sceNp] GetUserProfile(user=%d) -> \"%s\"\n",
           userId, np_psnr_online_id());
    return CELL_OK;
}

s32 sceNpGetAccountRegion(s32 userId, u32* region)
{
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!region)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    /* Region: US (SCEA) = 0x5553 ('US') */
    vm_write32((u32)(uintptr_t)region, (u32)0x5553);

    printf("[sceNp] GetAccountRegion(user=%d) -> US\n", userId);
    return CELL_OK;
}

s32 sceNpGetAccountAge(s32 userId, s32* age)
{
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!age)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    vm_write32((u32)(uintptr_t)age, 25); /* default adult age */
    printf("[sceNp] GetAccountAge(user=%d) -> 25\n", userId);
    return CELL_OK;
}

s32 sceNpGetMyLanguages(SceNpMyLanguages* langs)
{
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!langs)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    u32 langs_ea = (u32)(uintptr_t)langs;
    vm_write32(langs_ea + 0, SCE_NP_LANG_ENGLISH);
    vm_write32(langs_ea + 4, 0);
    vm_write32(langs_ea + 8, 0);

    printf("[sceNp] GetMyLanguages() -> English\n");
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * NP Manager (sign-in state)
 *
 * The toolkit runs with a fake local profile but no live PSN connection. So the
 * manager reports OFFLINE — games gate their online flows on GetStatus and skip
 * them cleanly — while the identity getters still hand back the fake account,
 * matching how NP behaves on a real signed-in-but-disconnected console.
 * Imported by most online-capable titles.
 * -----------------------------------------------------------------------*/

static SceNpManagerCallback s_npmgr_cb = NULL;
static void*                s_npmgr_cb_arg = NULL;

s32 sceNpManagerGetStatus(s32* status)
{
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;
    if (!status)
        return SCE_NP_ERROR_INVALID_ARGUMENT;
    /* `status` is a GUEST address -- the HLE ABI adapter passes pointer
     * parameters straight through as guest values, so dereferencing one
     * writes to whatever host address shares that number. Same trap as
     * cellGcmSys had. Tokyo Jungle calls this during its online init. */
    /* Signed in whenever the run is online (PS3_NET_ONLINE), psnr server or
     * not: a title with its own backend (Medius) only goes online once the
     * NP manager says it is signed in, and never talks to psnr. */
    s32 st = getenv("PS3_NET_ONLINE") ? SCE_NP_MANAGER_STATUS_ONLINE
                                      : SCE_NP_MANAGER_STATUS_OFFLINE;
    vm_write32((uint32_t)(uintptr_t)status, (uint32_t)st);
    { static s32 last = -2;
      if (st != last) printf("[sceNp] ManagerGetStatus() -> %s\n", st < 0 ? "OFFLINE" : "ONLINE");
      last = st; }
    return CELL_OK;
}

s32 sceNpManagerRegisterCallback(SceNpManagerCallback callback, void* arg)
{
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;
    s_npmgr_cb     = callback;
    s_npmgr_cb_arg = arg;
    printf("[sceNp] ManagerRegisterCallback()\n");
    /* Online: tell the title it is signed in, as the console does once the
     * manager reaches ONLINE. Delivered on its next cellSysutilCheckCallback. */
    if (getenv("PS3_NET_ONLINE") && callback) {
        const u64 args[8] = { (u64)(u32)SCE_NP_MANAGER_STATUS_ONLINE, 0,
                              (u64)(u32)(uintptr_t)arg, 0, 0, 0, 0, 0 };
        cellSysutilQueueGuestCallbackArgs((u32)(uintptr_t)callback, args);
    }
    return CELL_OK;
}

s32 sceNpManagerUnregisterCallback(void)
{
    s_npmgr_cb     = NULL;
    s_npmgr_cb_arg = NULL;
    return CELL_OK;
}

/* Identity getters: reuse the fake-profile implementations (offline-with-account). */
s32 sceNpManagerGetNpId(SceNpId* npId)               { return sceNpGetNpId(0, npId); }
s32 sceNpManagerGetOnlineId(SceNpOnlineId* onlineId) { return sceNpGetOnlineId(0, onlineId); }
s32 sceNpManagerGetOnlineName(SceNpOnlineName* name) { return sceNpGetOnlineName(0, name); }
s32 sceNpManagerGetAccountAge(s32* age)              { return sceNpGetAccountAge(0, age); }

/* Parental controls: an unrestricted adult account. Unimplemented, these
 * returned OK without writing, and the title read whatever was in its stack. */
s32 sceNpManagerGetContentRatingFlag(s32* isRestricted, s32* age)
{
    if (!s_np_initialized) return SCE_NP_ERROR_NOT_INITIALIZED;
    if (isRestricted) vm_write32((u32)(uintptr_t)isRestricted, 0);
    if (age) vm_write32((u32)(uintptr_t)age, 25);
    return CELL_OK;
}

s32 sceNpManagerGetChatRestrictionFlag(s32* isRestricted)
{
    if (!s_np_initialized) return SCE_NP_ERROR_NOT_INITIALIZED;
    if (isRestricted) vm_write32((u32)(uintptr_t)isRestricted, 0);
    return CELL_OK;
}

/* Score setup is local even while the NP manager is offline. Games may
 * initialize it before starting the worker that decides whether to use PSN. */
s32 sceNpScoreInit(void)
{
    if (s_score_initialized)
        return SCE_NP_COMMUNITY_ERROR_ALREADY_INITIALIZED;
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;
    s_score_initialized = 1;
    return CELL_OK;
}

s32 sceNpScoreTerm(void)
{
    if (!s_score_initialized)
        return SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED;
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;
    s_score_initialized = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * NPDRM
 *
 * On hardware sceNpDrmIsAvailable[2](klicensee, path) checks the licence for an
 * NPDRM file and leaves the klicensee with the kernel, so later cellFsOpen calls
 * on EDATs return plaintext. Free (license type 3) and disc-bound game data use a
 * title-specific klicensee that only the game knows; record it for the EDAT
 * reader (libs/filesystem/edat.c), which verifies it against each file's
 * dev_hash before using it. The two calls differ only in licence-check details
 * that do not apply here.
 * -----------------------------------------------------------------------*/

static s32 np_drm_is_available(const u8* k_licensee, const char* drm_path)
{
    const u8* k = GUEST_PTR(k_licensee, const u8*);
    const char* path = GUEST_PTR(drm_path, const char*);
    /* A NULL klicensee is legal for content that needs none; nothing to record. */
    if (k) edat_add_klicensee(k);
    fprintf(stderr, "[sceNp] DRM availability check: %s\n", path ? path : "(null)");
    return CELL_OK;
}

s32 sceNpDrmIsAvailable(const u8* k_licensee, const char* drm_path)
{
    return np_drm_is_available(k_licensee, drm_path);
}

s32 sceNpDrmIsAvailable2(const u8* k_licensee, const char* drm_path)
{
    return np_drm_is_available(k_licensee, drm_path);
}

/* ---------------------------------------------------------------------------
 * NP ticket: sceNpManagerRequestTicket / GetTicket / GetTicketParam
 *
 * A title that authenticates against its own servers asks the NP manager for
 * a ticket, waits for the manager callback to report it, then copies the blob
 * out with GetTicket and reads fields from it with GetTicketParam. None of the
 * three had an entry, so such a title waited forever for a completion that
 * never came.
 *
 * On a console the ticket is an SCE-signed blob. There is no SCE here: the
 * blob only has to be accepted by whatever server the player connects to,
 * and middleware usually forwards it opaque. It carries a recognisable
 * header and the online ID, and nothing that identifies the machine.
 * -----------------------------------------------------------------------*/

#define SCE_NP_MANAGER_EVENT_GOT_TICKET 255
#define NP_TICKET_SIZE                  0x100
#define NP_TICKET_LIFETIME_MS           (24ull * 3600ull * 1000ull)

enum {
    SCE_NP_TICKET_PARAM_SERIAL_ID = 0,
    SCE_NP_TICKET_PARAM_ISSUER_ID,
    SCE_NP_TICKET_PARAM_ISSUED_DATE,
    SCE_NP_TICKET_PARAM_EXPIRE_DATE,
    SCE_NP_TICKET_PARAM_SUBJECT_ACCOUNT_ID,
    SCE_NP_TICKET_PARAM_SUBJECT_ONLINE_ID,
    SCE_NP_TICKET_PARAM_SUBJECT_REGION,
    SCE_NP_TICKET_PARAM_SUBJECT_DOMAIN,
    SCE_NP_TICKET_PARAM_SERVICE_ID,
    SCE_NP_TICKET_PARAM_SUBJECT_STATUS,
    SCE_NP_TICKET_PARAM_STATUS_DURATION,
    SCE_NP_TICKET_PARAM_SUBJECT_DOB,
};

static u8   s_np_ticket[NP_TICKET_SIZE];
static int  s_np_ticket_ready = 0;
static char s_np_ticket_service[25];   /* the service ID it was requested for */
static u64  s_np_ticket_issued_ms;

/* PORT_ONLINE (port_online.c): a real ticket from an RPCN server, for
 * community servers that verify it. Without it (or before the first
 * RequestTicket it takes) the made-up ticket below stands. */
const np_ticket_source* g_np_ticket_source = NULL;

/* A stable 64-bit account ID (FNV-1a) from the name and its saved salt
 * (np_psnr_identity_key), so one player keeps one account across runs and two
 * players never share one, even with the same name. */
static u64 np_ticket_account_id(void)
{
    const char* key = np_psnr_identity_key();
    if (!key) key = np_psnr_online_id();
    u64 h = 0xCBF29CE484222325ull;
    for (const char* p = key; *p; p++) { h ^= (u8)*p; h *= 0x100000001B3ull; }
    return h;
}

static void np_ticket_build(u32 service_ea)
{
    const char* id = np_psnr_online_id();
    memset(s_np_ticket, 0, sizeof s_np_ticket);
    memcpy(s_np_ticket, "PS3RECOMP-NP-TICKET", 19);   /* serial: obviously not SCE's */
    memcpy(s_np_ticket + 24, id, strnlen(id, SCE_NP_ONLINEID_MAX_LENGTH));
    for (u32 i = 48; i < NP_TICKET_SIZE; i++) s_np_ticket[i] = (u8)(i * 7 + 0x5A);

    memset(s_np_ticket_service, 0, sizeof s_np_ticket_service);
    for (u32 i = 0; service_ea && i < sizeof s_np_ticket_service - 1; i++) {
        u8 c = vm_read8(service_ea + i);
        if (!c) break;
        s_np_ticket_service[i] = (char)c;
    }
    s_np_ticket_issued_ms = (u64)time(NULL) * 1000ull;
    s_np_ticket_ready = 1;
}

/* RequestTicket(npId, serviceId, cookie, cookieSize, entitlementId,
 * consumedCount). There is no per-call callback: completion is reported as
 * SCE_NP_MANAGER_EVENT_GOT_TICKET through the callback the title registered
 * with sceNpManagerRegisterCallback, on its next cellSysutilCheckCallback. */
static s32 np_ticket_request(u32 np_id_ea, u32 service_ea, u32 cookie_ea, u32 cookie_size)
{
    if (!s_np_initialized) return SCE_NP_ERROR_NOT_INITIALIZED;
    if (!service_ea) return SCE_NP_ERROR_INVALID_ARGUMENT;
    np_ticket_build(service_ea);
    if (g_np_ticket_source && g_np_ticket_source->request) {
        static u8 cookie[NP_TICKET_COOKIE_MAX];
        if (cookie_size > NP_TICKET_COOKIE_MAX) return SCE_NP_ERROR_INVALID_ARGUMENT;
        for (u32 i = 0; cookie_ea && i < cookie_size; i++) cookie[i] = vm_read8(cookie_ea + i);
        s32 r = g_np_ticket_source->request(s_np_ticket_service, cookie, cookie_ea ? cookie_size : 0);
        if (r != 0) {
            printf("[sceNp] ManagerRequestTicket(service=\"%s\", cookie %u bytes) -> %s\n",
                   s_np_ticket_service, cookie_ea ? cookie_size : 0,
                   r > 0 ? "fetching from the RPCN server" : "refused");
            return r > 0 ? CELL_OK : r;
        }
    }
    printf("[sceNp] ManagerRequestTicket(npId=0x%08X, service=\"%s\", cookie=0x%08X/%u) -> %u bytes\n",
           np_id_ea, s_np_ticket_service, cookie_ea, cookie_size, (u32)NP_TICKET_SIZE);
    if (s_npmgr_cb) {
        const u64 args[8] = { (u64)(u32)SCE_NP_MANAGER_EVENT_GOT_TICKET, 0,
                              (u64)(u32)(uintptr_t)s_npmgr_cb_arg, 0, 0, 0, 0, 0 };
        cellSysutilQueueGuestCallbackArgs((u32)(uintptr_t)s_npmgr_cb, args);
    } else {
        printf("[sceNp] ManagerRequestTicket: no manager callback registered to report it\n");
    }
    return CELL_OK;
}

/* Completion of a request g_np_ticket_source took: GOT_TICKET with the
 * ticket's size, or a negative error, as the console reports it. Safe from
 * any thread (the event is queued for the title's next CheckCallback). */
void np_ticket_deliver(s32 result)
{
    printf("[sceNp] GOT_TICKET: %s %d\n", result < 0 ? "error" : "ticket of", result);
    if (!s_npmgr_cb) {
        printf("[sceNp] GOT_TICKET: no manager callback registered to report it\n");
        return;
    }
    const u64 args[8] = { (u64)(u32)SCE_NP_MANAGER_EVENT_GOT_TICKET, (u64)(u32)result,
                          (u64)(u32)(uintptr_t)s_npmgr_cb_arg, 0, 0, 0, 0, 0 };
    cellSysutilQueueGuestCallbackArgs((u32)(uintptr_t)s_npmgr_cb, args);
}

/* The real ticket, when g_np_ticket_source has one: 1 and *t, *len set; 0 when
 * none was requested yet (the made-up one stands); a negative error when the
 * last request failed. */
static s32 np_ticket_real(const u8** t, u32* len)
{
    if (!g_np_ticket_source || !g_np_ticket_source->get) return 0;
    return g_np_ticket_source->get(t, len);
}

/* Titles read the ticket before ever requesting one: Resistance: Fall of Man
 * calls GetTicket and GetTicketParam right after sceNpManagerRegisterCallback,
 * and only goes on to RequestTicket if that worked. A console signed in at
 * boot has one by then, so there always is one here: made on first use, with
 * no service ID until a RequestTicket names one. */
static void np_ticket_ensure(void)
{
    if (!s_np_ticket_ready) np_ticket_build(0);
}

/* GetTicket(buffer, bufferSize*): *bufferSize is output only -- it is the
 * ticket's size, not the buffer's capacity. Resistance: Fall of Man passes a
 * buffer with *bufferSize = 0 and reads the ticket from it, so refusing a
 * "too small" buffer breaks it. Ignoring the value is on purpose and matches
 * RPCS3, whose sceNpManagerGetTicket overwrites *bufferSize with the ticket's
 * size before its size check. A NULL buffer asks for the size alone. */
static s32 np_ticket_get(u32 buf_ea, u32 size_ea)
{
    if (!s_np_initialized) return SCE_NP_ERROR_NOT_INITIALIZED;
    if (!size_ea) return SCE_NP_ERROR_INVALID_ARGUMENT;
    const u8* real = NULL;
    u32 real_len = 0;
    s32 r = np_ticket_real(&real, &real_len);
    if (r < 0) {
        printf("[sceNp] ManagerGetTicket: the last request failed (0x%08X)\n", (u32)r);
        return r;
    }
    if (r > 0) {
        /* Same output-only *bufferSize. Resistance: Fall of Man's buffer is
         * 64 KiB (func_0003542C), far beyond any ticket RPCN issues. */
        if (buf_ea)
            for (u32 i = 0; i < real_len; i++) vm_write8(buf_ea + i, real[i]);
        vm_write32(size_ea, real_len);
        printf("[sceNp] ManagerGetTicket(buf=0x%08X) -> %u bytes (RPCN)\n", buf_ea, real_len);
        return CELL_OK;
    }
    np_ticket_ensure();
    if (buf_ea)
        for (u32 i = 0; i < NP_TICKET_SIZE; i++)
            vm_write8(buf_ea + i, s_np_ticket[i]);   /* raw bytes, not BE fields */
    vm_write32(size_ea, NP_TICKET_SIZE);
    printf("[sceNp] ManagerGetTicket(buf=0x%08X) -> %u bytes\n", buf_ea, (u32)NP_TICKET_SIZE);
    return CELL_OK;
}

/* GetTicketParam(paramId, SceNpTicketParam* param): one field of the ticket
 * into a 256-byte union. Dates are SceNpTime, s64 ms since the Unix epoch;
 * a title typically turns expire - issued into the ticket's lifetime. */
static s32 np_ticket_get_param(s32 id, u32 out_ea)
{
    if (!s_np_initialized) return SCE_NP_ERROR_NOT_INITIALIZED;
    if (!out_ea) return SCE_NP_ERROR_INVALID_ARGUMENT;
    if (id < SCE_NP_TICKET_PARAM_SERIAL_ID || id > SCE_NP_TICKET_PARAM_SUBJECT_DOB)
        return SCE_NP_ERROR_INVALID_ARGUMENT;
    {
        const u8* real = NULL;
        u32 real_len = 0;
        s32 r = np_ticket_real(&real, &real_len);
        if (r < 0) return SCE_NP_ERROR_INVALID_STATE;
        if (r > 0) {
            u8 out[256];
            if (g_np_ticket_source->param && g_np_ticket_source->param(real, real_len, id, out) == 0) {
                for (u32 i = 0; i < 256; i++) vm_write8(out_ea + i, out[i]);
                printf("[sceNp] ManagerGetTicketParam(id=%d) (RPCN)\n", id);
                return CELL_OK;
            }
            printf("[sceNp] ManagerGetTicketParam(id=%d): not in the RPCN ticket, made-up value\n", id);
        }
    }
    np_ticket_ensure();
    printf("[sceNp] ManagerGetTicketParam(id=%d)\n", id);

    for (u32 i = 0; i < 256; i++) vm_write8(out_ea + i, 0);
    const char* online_id = np_psnr_online_id();
    switch (id) {
    case SCE_NP_TICKET_PARAM_SERIAL_ID:
        for (u32 i = 0; i < 20; i++) vm_write8(out_ea + i, s_np_ticket[i]);
        break;
    case SCE_NP_TICKET_PARAM_ISSUER_ID:   vm_write32(out_ea, 0x100u); break;
    case SCE_NP_TICKET_PARAM_ISSUED_DATE: vm_write64(out_ea, s_np_ticket_issued_ms); break;
    case SCE_NP_TICKET_PARAM_EXPIRE_DATE:
        vm_write64(out_ea, s_np_ticket_issued_ms + NP_TICKET_LIFETIME_MS);
        break;
    case SCE_NP_TICKET_PARAM_SUBJECT_ACCOUNT_ID:
        vm_write64(out_ea, np_ticket_account_id());
        break;
    case SCE_NP_TICKET_PARAM_SUBJECT_ONLINE_ID:
        for (u32 i = 0; i < SCE_NP_ONLINEID_MAX_LENGTH && online_id[i]; i++)
            vm_write8(out_ea + i, (u8)online_id[i]);
        break;
    case SCE_NP_TICKET_PARAM_SUBJECT_REGION:   /* country code, then language */
        vm_write8(out_ea + 0, 'u'); vm_write8(out_ea + 1, 's');
        vm_write8(out_ea + 2, 'e'); vm_write8(out_ea + 3, 'n');
        break;
    case SCE_NP_TICKET_PARAM_SUBJECT_DOMAIN:
        vm_write8(out_ea, 'u'); vm_write8(out_ea + 1, 'n');
        break;
    case SCE_NP_TICKET_PARAM_SERVICE_ID:
        for (u32 i = 0; s_np_ticket_service[i]; i++) vm_write8(out_ea + i, (u8)s_np_ticket_service[i]);
        break;
    case SCE_NP_TICKET_PARAM_SUBJECT_STATUS:   /* age 30 in the top byte, no restrictions */
        vm_write32(out_ea, 30u << 24);
        break;
    default:   /* status duration, date of birth: zero */
        break;
    }
    return CELL_OK;
}

static void h_np_request_ticket(ppu_context* ctx)
{
    ctx->gpr[3] = (u64)(int64_t)np_ticket_request((u32)ctx->gpr[3], (u32)ctx->gpr[4],
                                                  (u32)ctx->gpr[5], (u32)ctx->gpr[6]);
}
static void h_np_get_ticket(ppu_context* ctx)
{
    ctx->gpr[3] = (u64)(int64_t)np_ticket_get((u32)ctx->gpr[3], (u32)ctx->gpr[4]);
}
static void h_np_get_ticket_param(ppu_context* ctx)
{
    ctx->gpr[3] = (u64)(int64_t)np_ticket_get_param((s32)ctx->gpr[3], (u32)ctx->gpr[4]);
}

extern void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));

/* Called from ppu_sysprx_register(): the generated NID table has no entry for
 * these, and they must be in place before the title's first call. Online
 * (PS3_NET_ONLINE) only: offline, the NIDs stay unregistered and the calls
 * behave exactly as before (CELL_OK, nothing written). */
void np_register_ctx(void)
{
    if (!getenv("PS3_NET_ONLINE")) return;
    ps3_hle_register_ctx(0x7E2FEF28u, "sceNpManagerRequestTicket",  h_np_request_ticket);
    ps3_hle_register_ctx(0x0968AA36u, "sceNpManagerGetTicket",      h_np_get_ticket);
    ps3_hle_register_ctx(0x58FA4FCDu, "sceNpManagerGetTicketParam", h_np_get_ticket_param);
}
