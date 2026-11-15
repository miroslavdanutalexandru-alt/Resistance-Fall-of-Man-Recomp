/*
 * ps3recomp - sceNp HLE implementation
 *
 * Provides fake PSN identity so games can proceed through NP checks.
 * The username defaults to "PS3Player" but is configurable.
 */

#include "sceNp.h"
#include "../../runtime/ppu/ppu_context.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write*: guest EA -> host, byte-swapped */
#include "np_psnr.h"
#include <stdlib.h>
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
    /* Signed in when there is a psnr server to be signed in to. */
    s32 st = np_psnr_enabled() ? SCE_NP_MANAGER_STATUS_ONLINE : SCE_NP_MANAGER_STATUS_OFFLINE;
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
    printf("[sceNp] ManagerRegisterCallback(cb=0x%08X, arg=0x%08X)\n",
           (u32)(uintptr_t)callback, (u32)(uintptr_t)arg);
    /* Online: tell the title it is signed in, as the console does once the
     * manager reaches ONLINE. Delivered on its next cellSysutilCheckCallback. */
    if (np_psnr_enabled() && callback) {
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
 * NP ticket: sceNpManagerRequestTicket / sceNpManagerGetTicket
 *
 * The online front end asks the NP manager for a ticket, then hands the blob
 * to Medius/SVO as its credential ("ConnectionStats::TicketLogin - Attempting
 * SVO ticket login"), which is what the server ultimately authenticates.
 * Neither call was registered, so R:FoM's "online game" screen sat on a solid
 * black frame forever: RequestTicket returned CELL_OK and no completion was
 * ever delivered. Esc still worked, so it is a wait, not a hang.
 *
 * The observed call (PS3_HLE_ARGS=7E2FEF28) is
 *     RequestTicket(npId=0x1019B268, serviceId=0x10002F70, 0, 0, 0, 0)
 * -- no per-call callback, so completion has to arrive through the NP manager
 * callback that sceNpManagerRegisterCallback installed. Which event value the
 * title's handler keys on is not known yet, so it is a runtime knob
 * (PS3_NP_TICKET_EVENT, default 3) instead of a rebuild per guess.
 *
 * The ticket content is ours to choose: on a console it is an SCE-signed
 * blob, here it only has to satisfy our own server, and Medius forwards it
 * opaque. Size is a knob too (PS3_NP_TICKET_SIZE, default 0x100).
 * -----------------------------------------------------------------------*/

#define NP_TICKET_MAX 0x2000

static u8  s_np_ticket[NP_TICKET_MAX];
static u32 s_np_ticket_size = 0;
static int s_np_ticket_ready = 0;
static u32 s_np_ticket_serv_ea = 0;

static void np_ticket_build(void)
{
    const char* sz = getenv("PS3_NP_TICKET_SIZE");
    u32 size = sz ? (u32)strtoul(sz, NULL, 0) : 0x100u;
    if (size < 16) size = 16;
    if (size > NP_TICKET_MAX) size = NP_TICKET_MAX;

    memset(s_np_ticket, 0, sizeof(s_np_ticket));
    /* Recognisable header so a dump of the server side is obviously ours. */
    memcpy(s_np_ticket, "PS3RECOMP-NP-TICKET\0", 21);
    memcpy(s_np_ticket + 24, np_psnr_online_id(), strnlen(np_psnr_online_id(), 16));
    for (u32 i = 48; i < size; i++) s_np_ticket[i] = (u8)(i * 7 + 0x5A);
    s_np_ticket_size = size;
    s_np_ticket_ready = 1;
}

static s32 np_ticket_event(void)
{
    const char* e = getenv("PS3_NP_TICKET_EVENT");
    return e ? (s32)strtol(e, NULL, 0) : 3;
}

/* RequestTicket: prepare the blob and deliver the completion. */
static void np_ticket_request(ppu_context* ctx)
{
    u32 np_id_ea = (u32)ctx->gpr[3];
    u32 serv_ea  = (u32)ctx->gpr[4];
    u32 cookie   = (u32)ctx->gpr[5];
    u32 cookie_sz= (u32)ctx->gpr[6];
    u32 cb_ea    = (u32)ctx->gpr[7];
    u32 cb_arg   = (u32)ctx->gpr[8];

    np_ticket_build();
    s_np_ticket_serv_ea = serv_ea;

    printf("[sceNp] ManagerRequestTicket(npId=0x%08X, serviceId=0x%08X, cookie=0x%08X/%u, "
           "cb=0x%08X, arg=0x%08X) -> ticket %u bytes, event %d\n",
           np_id_ea, serv_ea, cookie, cookie_sz, cb_ea, cb_arg,
           s_np_ticket_size, np_ticket_event());
    (void)serv_ea;

    /* A per-call callback wins; otherwise the title expects the completion on
     * the manager callback it registered. Either way it is delivered on the
     * title's next cellSysutilCheckCallback, never re-entrantly here. */
    u32 target = cb_ea ? cb_ea : (u32)(uintptr_t)s_npmgr_cb;
    if (!target) {
        printf("[sceNp] ManagerRequestTicket: no callback to deliver the completion to\n");
        ctx->gpr[3] = (u64)(int64_t)(int32_t)CELL_OK;
        return;
    }
    const u64 args[8] = { (u64)(u32)np_ticket_event(), 0,
                          (u64)(u32)(cb_ea ? cb_arg : (u32)(uintptr_t)s_npmgr_cb_arg),
                          0, 0, 0, 0, 0 };
    cellSysutilQueueGuestCallbackArgs(target, args);
    ctx->gpr[3] = (u64)(int64_t)(int32_t)CELL_OK;
}

/* GetTicket: the usual two-phase shape -- size query, then the copy. */
static void np_ticket_get(ppu_context* ctx)
{
    u32 buf_ea  = (u32)ctx->gpr[3];
    u32 size_ea = (u32)ctx->gpr[4];

    if (!s_np_ticket_ready) np_ticket_build();

    printf("[sceNp] ManagerGetTicket(buf=0x%08X, size=0x%08X) -> %u\n",
           buf_ea, size_ea, s_np_ticket_size);
    if (buf_ea) {
        for (u32 i = 0; i < s_np_ticket_size; i++)
            vm_write8(buf_ea + i, s_np_ticket[i]);   /* ticket is raw bytes, not BE fields */
    }
    if (size_ea) vm_write32(size_ea, s_np_ticket_size);
    ctx->gpr[3] = (u64)(int64_t)(int32_t)CELL_OK;
}

/* GetTicketParam(paramId, SceNpTicketParam* param): one field of the parsed
 * ticket into a 256-byte union. R:FoM (func_00034xxx) reads 6 (region), 9
 * (subject status), 2 (issued) and 3 (expire) right after GetTicket and turns
 * expire - issued into the ticket's lifetime; left unimplemented, both dates
 * were whatever was on the stack, so the ticket was stale from birth.
 * SceNpTime is s64 milliseconds since the Unix epoch. */
#include <time.h>
static void np_ticket_get_param(ppu_context* ctx)
{
    s32 id     = (s32)ctx->gpr[3];
    u32 out_ea = (u32)ctx->gpr[4];
    s32 rc = CELL_OK;
    if (!s_np_ticket_ready) np_ticket_build();
    if (out_ea) {
        for (u32 i = 0; i < 256; i++) vm_write8(out_ea + i, 0);
        u64 now_ms = (u64)time(NULL) * 1000ull;
        switch (id) {
        case 0: /* serial id */
            for (u32 i = 0; i < 20; i++) vm_write8(out_ea + i, s_np_ticket[i]);
            break;
        case 1: vm_write32(out_ea, 0x100u); break;               /* issuer id */
        case 2: vm_write64(out_ea, now_ms); break;               /* issued */
        case 3: vm_write64(out_ea, now_ms + 24ull*3600ull*1000ull); break; /* expire */
        case 4: vm_write64(out_ea, 0x0123456789ABCDEFull); break; /* account id */
        case 5: {                                                /* online id */
            const char* o = np_psnr_online_id();
            for (u32 i = 0; i < 16 && o[i]; i++) vm_write8(out_ea + 4 + i, (u8)o[i]);
            break; }
        case 6: /* region: country code + language */
            vm_write8(out_ea + 0, 'u'); vm_write8(out_ea + 1, 's');
            vm_write8(out_ea + 2, 'e'); vm_write8(out_ea + 3, 'n');
            break;
        case 7: vm_write8(out_ea, 'u'); vm_write8(out_ea + 1, 'n'); break; /* domain */
        case 8: /* service id: echo the one the title requested the ticket for */
            if (s_np_ticket_serv_ea)
                for (u32 i = 0; i < 24; i++) {
                    u8 c = vm_read8(s_np_ticket_serv_ea + i);
                    if (!c) break;
                    vm_write8(out_ea + i, c);
                }
            break;
        case 9: vm_write32(out_ea, 30u << 24); break;  /* status: age 30, no restrictions */
        case 10: vm_write32(out_ea, 0); break;
        case 11: vm_write32(out_ea, 0); break;           /* date of birth */
        default: rc = (s32)0x8002AA0Bu; break;           /* unknown param */
        }
    }
    printf("[sceNp] ManagerGetTicketParam(id=%d, out=0x%08X) -> 0x%08X\n", id, out_ea, (u32)rc);
    ctx->gpr[3] = (u64)(int64_t)rc;
}

extern void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));

/* Registered from ppu_sysprx_register(): the generated NID table has no entry
 * for these, and a name-based registration has to happen before the title
 * calls them (sceNpInit, which runs first, is enough). */
void np_register_ctx(void)
{
    ps3_hle_register_ctx(0x7E2FEF28u, "sceNpManagerRequestTicket", np_ticket_request);
    ps3_hle_register_ctx(0x00968AA36u, "sceNpManagerGetTicket",    np_ticket_get);
    ps3_hle_register_ctx(0x58FA4FCDu, "sceNpManagerGetTicketParam", np_ticket_get_param);
}
