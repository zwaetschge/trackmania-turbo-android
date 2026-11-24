/*
 * uplay.c - offline Ubisoft Connect (Uplay R1) layer for the owner's own copy.
 *
 * On Android there is no Ubisoft Connect client, and the game is run offline
 * without signing in. This reproduces exactly what Uplay's own offline mode
 * reports to a game whose files the user owns: the product is owned, the client
 * is in offline mode, and no account is connected to online services. Nothing
 * here contacts a server or asserts entitlement beyond the local copy.
 *
 * All Uplay R1 entry points are __cdecl (caller cleans the stack: RETC).
 * Return conventions were recovered from this exact build:
 *   - UPLAY_Start returns 0 on success (TrackmaniaTurbo.exe 0x00775510 takes
 *     the init path only for a 0 return).
 *   - The engine stores "restricted = !UPLAY_USER_IsOwned(0xCA7)" (0x007757A0 →
 *     neg/sbb/inc at 0x00775627), so IsOwned must return 1 for an owned copy.
 */
#define _GNU_SOURCE
#include "tmrt.h"
#include <stdlib.h>
#include <string.h>

#define OFFLINE_PLAYER   "Player"
#define OFFLINE_ACCOUNTID "00000000-0000-0000-0000-000000000000"

/* ---- lifecycle ---------------------------------------------------------- */
static void shim_UPLAY_Start(void) {
    /* gameId = ARG(0), startOptions = ARG(1). Offline success. */
    TM_INFO("UPLAY_Start(gameId %u): offline mode, owned copy", ARG(0));
    RETC(0);
}
static void shim_UPLAY_Quit(void) { RETC(1); }
static void shim_UPLAY_Update(void) { RETC(1); }   /* pump: keep running */
static void shim_UPLAY_GetLastError(void) { if (ARG(0)) wr32(ARG(0), 0); RETC(0); }
static void shim_UPLAY_GetNextEvent(void) { RETC(0); }   /* event queue empty */
static void shim_UPLAY_PeekNextEvent(void) { RETC(0); }

/* ---- user / ownership --------------------------------------------------- */
static void shim_UPLAY_USER_IsOwned(void) { RETC(1); }            /* owned */
static void shim_UPLAY_USER_IsInOfflineMode(void) { RETC(1); }    /* offline */
static void shim_UPLAY_USER_IsConnected(void) { RETC(0); }        /* no online session */

/* UTF-8 getters: const char* f(void) - the string pointer is the return value
 * (the engine does strlen(eax) on it). They take NO arguments: the engine's
 * wrappers (0x00775690 / 0x007756F0) tail-jump here without pushing anything,
 * so [esp+4] is unrelated caller stack. Writing an "out-param" through it
 * overwrote the vtable of a live engine object (0x004EEBFE dispatch crash). */
static void shim_UPLAY_USER_GetNameUtf8(void) {
    static uint32_t s;
    if (!s) {                       /* TM_PLAYER_NAME: the start screen's player name (LAN games need distinct names) */
        const char *n = getenv("TM_PLAYER_NAME");
        s = tm_gstrdup(n && *n ? n : OFFLINE_PLAYER);
    }
    RETC(s);
}
static void shim_UPLAY_USER_GetAccountIdUtf8(void) {
    static uint32_t s;
    if (!s) s = tm_gstrdup(OFFLINE_ACCOUNTID);
    RETC(s);
}
/* Offline: return an empty string (not NULL) so callers that strlen it are safe. */
static void shim_UPLAY_USER_GetTicketUtf8(void) {
    static uint32_t s;
    if (!s) s = tm_gstrdup("");
    RETC(s);
}
static void shim_UPLAY_USER_SetGameSession(void) { RETC(1); }
static void shim_UPLAY_USER_ClearGameSession(void) { RETC(1); }

/* ---- overlapped (async) ops: complete immediately, succeed --------------- */
static void shim_UPLAY_HasOverlappedOperationCompleted(void) { RETC(1); }
static void shim_UPLAY_GetOverlappedOperationResult(void) { if (ARG(1)) wr32(ARG(1), 0); RETC(1); }

/* ---- friends / overlay / achievements / avatars: offline no-ops --------- */
static void shim_UPLAY_FRIENDS_Init(void) { RETC(1); }
static void shim_UPLAY_FRIENDS_GetFriendList(void) { if (ARG(1)) wr32(ARG(1), 0); RETC(0); }   /* no friends */
static void shim_UPLAY_FRIENDS_ShowInviteFriendsToGameUI(void) { RETC(1); }
static void shim_UPLAY_ACH_EarnAchievement(void) { RETC(1); }   /* recorded locally only */
static void shim_UPLAY_OVERLAY_Show(void) { RETC(1); }
static void shim_UPLAY_OVERLAY_ShowShopUrl(void) { RETC(1); }
static void shim_UPLAY_AVATAR_Get(void) { RETC(0); }   /* no avatar; arg 1 is a size enum, not a pointer */
static void shim_UPLAY_AVATAR_Release(void) { RETC(1); }
static void shim_UPLAY_WIN_RefreshActions(void) { RETC(1); }

static const tm_shim_def uplay_shims[] = {
#define UP(n) TM_SHIM("uplay_r1_loader.dll", n)
    UP(UPLAY_Start), UP(UPLAY_Quit), UP(UPLAY_Update), UP(UPLAY_GetLastError), UP(UPLAY_GetNextEvent),
    UP(UPLAY_PeekNextEvent), UP(UPLAY_USER_IsOwned), UP(UPLAY_USER_IsInOfflineMode), UP(UPLAY_USER_IsConnected),
    UP(UPLAY_USER_GetNameUtf8), UP(UPLAY_USER_GetAccountIdUtf8), UP(UPLAY_USER_GetTicketUtf8),
    UP(UPLAY_USER_SetGameSession), UP(UPLAY_USER_ClearGameSession), UP(UPLAY_HasOverlappedOperationCompleted),
    UP(UPLAY_GetOverlappedOperationResult), UP(UPLAY_FRIENDS_Init), UP(UPLAY_FRIENDS_GetFriendList),
    UP(UPLAY_FRIENDS_ShowInviteFriendsToGameUI), UP(UPLAY_ACH_EarnAchievement), UP(UPLAY_OVERLAY_Show),
    UP(UPLAY_OVERLAY_ShowShopUrl), UP(UPLAY_AVATAR_Get), UP(UPLAY_AVATAR_Release), UP(UPLAY_WIN_RefreshActions),
#undef UP
};
void tm_uplay_register(void) { tm_register_shims(uplay_shims, sizeof uplay_shims / sizeof uplay_shims[0]); }
