/*
 * ps3recomp - sceNp HLE
 *
 * PlayStation Network core: NP IDs, online IDs, user profiles.
 */

#ifndef PS3RECOMP_SCE_NP_H
#define PS3RECOMP_SCE_NP_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes (values from the official SDK np/error.h -- games compare
 * against these exact codes; e.g. LBP special-cases NOT_INITIALIZED from
 * sceNpManagerGetStatus to mean "NP not up yet -> treat as offline")
 * -----------------------------------------------------------------------*/
#define SCE_NP_ERROR_NOT_INITIALIZED            0x8002aa01
#define SCE_NP_ERROR_ALREADY_INITIALIZED        0x8002aa02
#define SCE_NP_ERROR_INVALID_ARGUMENT           0x8002aa03
#define SCE_NP_ERROR_OUT_OF_MEMORY              0x8002aa04
#define SCE_NP_ERROR_ID_NOT_FOUND               0x8002aa06
#define SCE_NP_ERROR_SESSION_RUNNING            0x8002aa07
#define SCE_NP_ERROR_INVALID_STATE              0x8002aa0a
#define SCE_NP_ERROR_ABORTED                    0x8002aa0b
#define SCE_NP_ERROR_OFFLINE                    0x8002aa0c
#define SCE_NP_ERROR_INTERNAL                   0x8002aaff

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/
#define SCE_NP_ONLINEID_MAX_LENGTH              16
#define SCE_NP_ONLINENAME_MAX_LENGTH            48
#define SCE_NP_AVATAR_URL_MAX_LENGTH            127
#define SCE_NP_ABOUT_ME_MAX_LENGTH              63
#define SCE_NP_COMMUNICATION_ID_MAX_LENGTH      9
#define SCE_NP_COMMUNICATION_PASSPHRASE_SIZE    128
#define SCE_NP_COMMUNICATION_SIGNATURE_SIZE     160

/* Country codes */
#define SCE_NP_LANG_ENGLISH                     1

/* ---------------------------------------------------------------------------
 * Structures
 * -----------------------------------------------------------------------*/

typedef struct SceNpCommunicationId {
    char data[SCE_NP_COMMUNICATION_ID_MAX_LENGTH];
    char term;
    u8   num;
    u8   padding[3];
} SceNpCommunicationId;

typedef struct SceNpCommunicationPassphrase {
    u8 data[SCE_NP_COMMUNICATION_PASSPHRASE_SIZE];
} SceNpCommunicationPassphrase;

typedef struct SceNpCommunicationSignature {
    u8 data[SCE_NP_COMMUNICATION_SIGNATURE_SIZE];
} SceNpCommunicationSignature;

typedef struct SceNpOnlineId {
    char data[SCE_NP_ONLINEID_MAX_LENGTH];
    char term;
    char padding[3];
} SceNpOnlineId;

typedef struct SceNpId {
    SceNpOnlineId handle;
    u8            opt[8];
    u8            reserved[8];
} SceNpId;

typedef struct SceNpOnlineName {
    char data[SCE_NP_ONLINENAME_MAX_LENGTH];
} SceNpOnlineName;

typedef struct SceNpAvatarUrl {
    char data[SCE_NP_AVATAR_URL_MAX_LENGTH + 1];
} SceNpAvatarUrl;

typedef struct SceNpUserInfo {
    SceNpId         npId;
    SceNpOnlineName onlineName;
    SceNpAvatarUrl  avatarUrl;
} SceNpUserInfo;

typedef struct SceNpMyLanguages {
    u32 language1;
    u32 language2;
    u32 language3;
} SceNpMyLanguages;

/* ---------------------------------------------------------------------------
 * Functions
 * -----------------------------------------------------------------------*/

s32 sceNpInit(u32 poolSize, void* poolPtr);
s32 sceNpTerm(void);

#define SCE_NP_COMMUNITY_ERROR_ALREADY_INITIALIZED 0x8002a101
#define SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED     0x8002a102
s32 sceNpScoreInit(void);
s32 sceNpScoreTerm(void);

s32 sceNpGetNpId(s32 userId, SceNpId* npId);
s32 sceNpGetOnlineId(s32 userId, SceNpOnlineId* onlineId);
s32 sceNpGetOnlineName(s32 userId, SceNpOnlineName* onlineName);
s32 sceNpGetUserProfile(s32 userId, SceNpUserInfo* userInfo);
s32 sceNpGetAccountRegion(s32 userId, u32* region);
s32 sceNpGetAccountAge(s32 userId, s32* age);
s32 sceNpGetMyLanguages(SceNpMyLanguages* langs);

/* --- NP Manager (sign-in state). We run permanently offline: GetStatus reports
 * OFFLINE so games cleanly skip online flows instead of blocking on sign-in. --- */
#define SCE_NP_MANAGER_STATUS_OFFLINE          (-1)
#define SCE_NP_MANAGER_STATUS_GETTING_TICKET    0
#define SCE_NP_MANAGER_STATUS_GETTING_PROFILE   1
#define SCE_NP_MANAGER_STATUS_LOGGING_IN        2
#define SCE_NP_MANAGER_STATUS_ONLINE            3

typedef void (*SceNpManagerCallback)(s32 event, s32 result, void* arg);

s32 sceNpManagerGetStatus(s32* status);
s32 sceNpManagerRegisterCallback(SceNpManagerCallback callback, void* arg);
s32 sceNpManagerUnregisterCallback(void);
s32 sceNpManagerGetNpId(SceNpId* npId);
s32 sceNpManagerGetOnlineId(SceNpOnlineId* onlineId);
s32 sceNpManagerGetOnlineName(SceNpOnlineName* onlineName);
s32 sceNpManagerGetAccountAge(s32* age);

/* NPDRM availability checks. The guest passes its klicensee (16 bytes) and the
 * path of an EDAT/SELF; the klicensee is handed to the EDAT reader, which then
 * decrypts that title's files on open. Always CELL_OK (licence present). */
s32 sceNpDrmIsAvailable(const u8* k_licensee, const char* drm_path);
s32 sceNpDrmIsAvailable2(const u8* k_licensee, const char* drm_path);

/* Set the fake PSN username (call before sceNpInit if desired) */
void sceNpSetFakeUsername(const char* username);

/* A source of real NP tickets (port_online.c's RPCN client, PORT_ONLINE=1).
 *   request: 1 = taken; its GOT_TICKET event comes later through
 *            np_ticket_deliver. 0 = not taken (the made-up ticket answers).
 *            Negative = refused with that error.
 *   get:     1 = *t and *len hold the last ticket fetched; 0 = none requested
 *            yet; negative = the last request failed with that error.
 *   param:   GetTicketParam's 256-byte union for paramId, in guest byte
 *            order; 0 = filled, -1 = not in this ticket. */
#define NP_TICKET_COOKIE_MAX 1024
typedef struct {
    s32 (*request)(const char* service_id, const u8* cookie, u32 cookie_size);
    s32 (*get)(const u8** t, u32* len);
    int (*param)(const u8* t, u32 len, s32 param_id, u8 out[256]);
} np_ticket_source;
extern const np_ticket_source* g_np_ticket_source;
void np_ticket_deliver(s32 result);
int  np_is_initialized(void);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_SCE_NP_H */
