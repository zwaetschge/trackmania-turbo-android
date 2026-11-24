/*
 * misc.c - advapi32, shell32, ole32/oleaut32, rpcrt4, version, userenv,
 * iphlpapi, setupapi, gdi32 basics, and platform services that do not exist
 * on Android (Uplay, Steam, OpenVR) answered truthfully as "not available".
 */
#define _GNU_SOURCE
#include "tmrt.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>

static void random_bytes(void *p, size_t n) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) { ssize_t r = read(fd, p, n); (void)r; close(fd); }
}

/* ------------------------------------------------------------ advapi32 */
/* No registry keys exist on this host: the game must use its defaults. */
static void shim_RegOpenKeyExA(void) { TM_INFO("RegOpenKeyExA(%08X, %s) -> not found", ARG(0), ARGS(1) ? ARGS(1) : ""); RET(2, 5); }
static void shim_RegQueryValueExA(void) { RET(2, 6); }
static void shim_RegEnumValueW(void) { RET(259, 8); }
static void shim_RegCloseKey(void) { RET(0, 1); }
static void shim_CryptAcquireContextW(void) { if (ARG(0)) wr32(ARG(0), 0x7800); RET(1, 5); }
static void shim_CryptReleaseContext(void) { RET(1, 2); }
static void shim_CryptGenRandom(void) { random_bytes(G2H(ARG(2)), ARG(1)); RET(1, 3); }
static void shim_GetUserNameW(void) {
    uint32_t buf = ARG(0), n = ARG(1);
    if (rd32(n) < 7) { wr32(n, 7); tm_set_last_error(122); RET(0, 2); }
    tm_u2w("Player", buf, (int)rd32(n)); wr32(n, 7); RET(1, 2);
}
static void shim_RegisterEventSourceA(void) { RET(0x7810, 2); }
static void shim_DeregisterEventSource(void) { RET(1, 1); }
static void shim_ReportEventA(void) { RET(1, 9); }
static void shim_OpenProcessToken(void) { wr32(ARG(2), 0x7820); RET(1, 3); }
static void shim_AllocateAndInitializeSid(void) { wr32(ARG(10), tm_galloc(32)); RET(1, 11); }
static void shim_FreeSid(void) { RET(0, 1); }
static void shim_SetEntriesInAclW(void) { if (ARG(3)) wr32(ARG(3), 0); RET(0, 4); }
static void shim_GetSecurityInfo(void) { RET(0, 8); }
static void shim_SetSecurityInfo(void) { RET(0, 7); }

/* ------------------------------------------------------------ shell32 */
/* SHGetFolderPathW(hwnd, csidl, token, flags, path) */
static void shim_SHGetFolderPathW(void) {
    uint32_t csidl = ARG(1) & 0xFF, out = ARG(4);
    const char *p;
    switch (csidl) {
    case 0x05: p = "C:\\Users\\Player\\Documents"; break;          /* CSIDL_PERSONAL */
    case 0x1A: p = "C:\\Users\\Player\\AppData\\Roaming"; break;   /* APPDATA */
    case 0x1C: p = "C:\\Users\\Player\\AppData\\Local"; break;     /* LOCAL_APPDATA */
    case 0x23: p = "C:\\ProgramData"; break;                       /* COMMON_APPDATA */
    case 0x2E: p = "C:\\Users\\Public\\Documents"; break;          /* COMMON_DOCUMENTS */
    case 0x28: p = "C:\\Users\\Player"; break;                     /* PROFILE */
    case 0x10: p = "C:\\Users\\Player\\Desktop"; break;
    case 0x27: p = "C:\\Users\\Player\\Pictures"; break;
    case 0x0E: p = "C:\\Users\\Player\\Videos"; break;
    case 0x24: p = "C:\\Windows"; break;
    case 0x25: p = "C:\\Windows\\System32"; break;
    case 0x26: p = "C:\\Program Files"; break;
    default: TM_WARN("SHGetFolderPathW csidl %X unknown", csidl); RET(0x80070002u, 5);
    }
    tm_u2w(p, out, 260);
    if (ARG(1) & 0x8000) { char host[2048]; tm_path_to_host(p, host, sizeof host); char cmd[2100]; snprintf(cmd, sizeof cmd, "%s", host); (void)cmd; }
    TM_INFO("SHGetFolderPathW(%X) = %s", csidl, p);
    RET(0, 5);
}
static void shim_ShellExecuteW(void) { TM_WARN("ShellExecuteW ignored"); RET(2, 6); }
static void shim_ShellExecuteExW(void) { RET(0, 1); }
static void shim_SHFileOperationW(void) { RET(1, 1); }
static void shim_SHParseDisplayName(void) { RET(0x80004001u, 5); }
static void shim_CommandLineToArgvW(void) {
    /* split the guest command line (no quotes inside arguments) */
    uint32_t cl = ARG(0), outn = ARG(1);
    char s[4096]; tm_w2u(cl, s, sizeof s);
    char *argv[64]; int argc = 0;
    char *p = s;
    while (*p && argc < 64) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (*p == '"') { argv[argc++] = ++p; while (*p && *p != '"') p++; }
        else { argv[argc++] = p; while (*p && *p != ' ') p++; }
        if (*p) *p++ = 0;
    }
    size_t total = 4 * (size_t)(argc + 1);
    for (int i = 0; i < argc; i++) total += (strlen(argv[i]) + 1) * 2;
    uint32_t blk = tm_galloc((uint32_t)total);
    uint32_t sp = blk + 4 * (uint32_t)(argc + 1);
    for (int i = 0; i < argc; i++) { wr32(blk + 4 * (uint32_t)i, sp); int n = tm_u2w(argv[i], sp, 4096); sp += (uint32_t)(n + 1) * 2; }
    wr32(blk + 4 * (uint32_t)argc, 0);
    if (outn) wr32(outn, (uint32_t)argc);
    RET(blk, 2);
}

/* ------------------------------------------------------------ ole32 / oleaut32 / rpcrt4 */
static void shim_CoInitialize(void) { RET(0, 1); }
static void shim_CoInitializeEx(void) { RET(0, 2); }
static void shim_CoUninitialize(void) { RET(0, 0); }
static void shim_CoSetProxyBlanket(void) { RET(0, 8); }
static void shim_CoCreateInstance(void) {
    uint32_t c = ARG(0);
    TM_INFO("CoCreateInstance({%08X-...}) -> class not registered", rd32(c));
    if (ARG(4)) wr32(ARG(4), 0);
    RET(0x80040154u, 5);
}
static void shim_CoCreateGuid(void) { random_bytes(G2H(ARG(0)), 16); RET(0, 1); }
static void shim_CoTaskMemFree(void) { tm_gfree(ARG(0)); RET(0, 1); }
static void shim_CLSIDFromString(void) { memset(G2H(ARG(1)), 0, 16); RET(0x800401F3u, 2); }
static void shim_SysAllocString(void) {
    uint32_t s = ARG(0);
    if (!s) RET(0, 1);
    uint32_t n = (uint32_t)tm_wcslen(s);
    uint32_t b = tm_galloc(4 + 2 * n + 2);
    wr32(b, 2 * n); memcpy(G2H(b + 4), G2H(s), 2 * n + 2);
    RET(b + 4, 1);
}
static void shim_SysFreeString(void) { if (ARG(0)) tm_gfree(ARG(0) - 4); RET(0, 1); }
static void shim_UuidCreate(void) { random_bytes(G2H(ARG(0)), 16); RET(0, 1); }
static void shim_UuidToStringA(void) {
    uint8_t *u = G2H(ARG(0));
    char s[40];
    snprintf(s, sizeof s, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             u[3], u[2], u[1], u[0], u[5], u[4], u[7], u[6], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
    wr32(ARG(1), tm_gstrdup(s));
    RET(0, 2);
}
static void shim_RpcStringFreeA(void) { tm_gfree(rd32(ARG(0))); wr32(ARG(0), 0); RET(0, 1); }

/* ------------------------------------------------------------ version / userenv / iphlpapi / setupapi */
/* Version resources: only the game EXE has one we can read (RT_VERSION #1 of
 * the mapped image). Other files report ERROR_RESOURCE_TYPE_NOT_FOUND. */
uint32_t tm_find_resource_entry(uint32_t type, uint32_t name);
static int is_game_exe(uint32_t wpath) {
    char p[1024]; tm_w2u(wpath, p, sizeof p);
    const char *b = p; for (const char *q = p; *q; q++) if (*q == '\\' || *q == '/') b = q + 1;
    return !strcasecmp(b, "TrackmaniaTurbo.exe");
}
static void shim_GetFileVersionInfoSizeW(void) {
    uint32_t e = is_game_exe(ARG(0)) ? tm_find_resource_entry(16, 1) : 0;
    if (ARG(1)) wr32(ARG(1), 0);
    if (!e) { tm_set_last_error(1813); RET(0, 2); }
    RET(rd32(e + 4), 2);
}
static void shim_GetFileVersionInfoW(void) {
    uint32_t e = is_game_exe(ARG(0)) ? tm_find_resource_entry(16, 1) : 0;
    if (!e) { tm_set_last_error(1813); RET(0, 4); }
    uint32_t n = rd32(e + 4); if (n > ARG(2)) n = ARG(2);
    memcpy(G2H(ARG(3)), G2H(g_image.image_base + rd32(e)), n);
    RET(1, 4);
}
/* Walk a VS_VERSIONINFO node tree: wLength, wValueLength, wType, szKey, value, children. */
static int ver_find(uint32_t node, const char *path, uint32_t *val, uint32_t *len) {
    uint16_t wlen = rd16(node), vlen = rd16(node + 2), type = rd16(node + 4);
    uint32_t k = node + 6; while (rd16(k)) k += 2; k += 2;
    uint32_t v = (k + 3) & ~3u;
    uint32_t vbytes = type == 1 ? vlen * 2u : vlen;
    while (*path == '\\') path++;
    if (!*path) { *val = v; *len = vlen; return 1; }
    char comp[128]; size_t n = 0;
    while (path[n] && path[n] != '\\' && n < sizeof comp - 1) { comp[n] = path[n]; n++; }
    comp[n] = 0;
    uint32_t c = (v + vbytes + 3) & ~3u, end = node + wlen;
    while (c + 6 < end) {
        uint16_t cl = rd16(c);
        if (!cl) break;
        char key[128]; tm_w2u(c + 6, key, sizeof key);
        if (!strcasecmp(key, comp)) return ver_find(c, path + n, val, len);
        c = (c + cl + 3) & ~3u;
    }
    return 0;
}
static void shim_VerQueryValueW(void) {
    char sub[256]; tm_w2u(ARG(1), sub, sizeof sub);
    uint32_t v = 0, l = 0;
    int ok = ver_find(ARG(0), sub, &v, &l);
    TM_INFO("VerQueryValueW(%s) -> %s", sub, ok ? "found" : "missing");
    if (ok) { if (ARG(2)) wr32(ARG(2), v); if (ARG(3)) wr32(ARG(3), l); }
    RET(ok, 4);
}
static void shim_GetUserProfileDirectoryW(void) {
    uint32_t buf = ARG(1), n = ARG(2);
    if (rd32(n) < 17) { wr32(n, 17); RET(0, 3); }
    tm_u2w("C:\\Users\\Player", buf, 17); wr32(n, 16); RET(1, 3);
}
/* Offline: no network adapter. LAN game (TM_NET_LAN=1): one adapter with the device's
   Wi-Fi address, so the engine announces and binds the right address. IP_ADAPTER_INFO
   (32-bit layout, 648 bytes): Next 0, ComboIndex 4, AdapterName[260] 8, Description[132]
   268, AddressLength 400, Address[8] 404, Index 412, Type 416, DhcpEnabled 420,
   CurrentIpAddress 424, IpAddressList 428 (IP_ADDR_STRING: Next, IpAddress[16],
   IpMask[16], Context = 40 bytes), GatewayList 468, DhcpServer 508, HaveWins 548, ... */
static void shim_GetAdaptersInfo(void) {
    uint32_t buf = ARG(0), lenp = ARG(1), a, m; char ifn[32];
    if (!tm_net_lan() || !tm_net_local_ip(&a, &m, ifn, sizeof ifn)) RET(232, 2);   /* ERROR_NO_DATA */
    if (!lenp) RET(87, 2);
    if (!buf || rd32(lenp) < 648) { wr32(lenp, 648); RET(111, 2); }               /* ERROR_BUFFER_OVERFLOW */
    memset(G2H(buf), 0, 648);
    snprintf((char *)G2H(buf + 8), 260, "{%s}", ifn);
    snprintf((char *)G2H(buf + 268), 132, "Wi-Fi (%s)", ifn);
    wr32(buf + 400, 6);
    uint8_t mac[6] = { 0x02, 0x54, 0x4D, (uint8_t)(ntohl(a) >> 16), (uint8_t)(ntohl(a) >> 8), (uint8_t)ntohl(a) };
    memcpy(G2H(buf + 404), mac, 6);
    wr32(buf + 412, 1); wr32(buf + 416, 71);                                       /* IF_TYPE_IEEE80211 */
    struct in_addr ia = { a }, im = { m };
    snprintf((char *)G2H(buf + 428 + 4), 16, "%s", inet_ntoa(ia));
    snprintf((char *)G2H(buf + 428 + 20), 16, "%s", inet_ntoa(im));
    snprintf((char *)G2H(buf + 468 + 4), 16, "0.0.0.0");
    TM_INFO("GetAdaptersInfo: %s %s/%s", ifn, (char *)G2H(buf + 432), (char *)G2H(buf + 448));
    RET(0, 2);
}
static void shim_GetAdaptersAddresses(void) { RET(232, 5); }
static void shim_GetBestInterface(void) { RET(1168, 2); }    /* ERROR_NOT_FOUND */
static void shim_GetIfEntry(void) { RET(1168, 1); }
static void shim_SetupDiGetClassDevsExW(void) { RET(0xFFFFFFFFu, 7); }
static void shim_SetupDiEnumDeviceInterfaces(void) { tm_set_last_error(259); RET(0, 5); }
static void shim_SetupDiGetDeviceInterfaceDetailW(void) { RET(0, 6); }
static void shim_SetupDiOpenDevRegKey(void) { RET(0xFFFFFFFFu, 6); }

/* ------------------------------------------------------------ gdi32 */
static void shim_CreateDCA(void) { RET(0x7900, 4); }
static void shim_DeleteDC(void) { RET(1, 1); }
static void shim_ExtEscape(void) { RET(0, 6); }
static void shim_GetDIBits(void) { RET(0, 7); }

/* ------------------------------------------------------------ comdlg32 */
static void shim_GetOpenFileNameW(void) { RET(0, 1); }
static void shim_GetSaveFileNameW(void) { RET(0, 1); }
static void shim_CommDlgExtendedError(void) { RET(0, 0); }

/* ------------------------------------------------------------ Steamworks (cdecl) */
static void shim_SteamAPI_Init(void) { TM_INFO("SteamAPI_Init: Steam not available"); RETC(0); }
static void shim_SteamAPI_IsSteamRunning(void) { RETC(0); }
static void shim_SteamAPI_Shutdown(void) { RETC(0); }
static void shim_SteamAPI_RunCallbacks(void) { RETC(0); }
static void shim_SteamAPI_RegisterCallback(void) { RETC(0); }
static void shim_SteamAPI_UnregisterCallback(void) { RETC(0); }
static void shim_SteamAPI_RegisterCallResult(void) { RETC(0); }
static void shim_SteamAPI_UnregisterCallResult(void) { RETC(0); }
static void shim_SteamAPI_GetHSteamUser(void) { RETC(0); }
static void shim_SteamAPI_GetHSteamPipe(void) { RETC(0); }
static void shim_SteamInternal_CreateInterface(void) { RETC(0); }

/* ------------------------------------------------------------ OpenVR (cdecl) */
static void shim_VR_IsHmdPresent(void) { RETC(0); }
static void shim_VR_IsRuntimeInstalled(void) { RETC(0); }
static void shim_VR_InitInternal(void) { if (ARG(0)) wr32(ARG(0), 108); RETC(0); }  /* VRInitError_Init_HmdNotFound */
static void shim_VR_ShutdownInternal(void) { RETC(0); }
static void shim_VR_GetInitToken(void) { RETC(0); }
static void shim_VR_IsInterfaceVersionValid(void) { RETC(0); }
static void shim_VR_GetGenericInterface(void) { if (ARG(1)) wr32(ARG(1), 108); RETC(0); }
static void shim_VR_GetVRInitErrorAsSymbol(void) { static uint32_t s; if (!s) s = tm_gstrdup("VRInitError_Init_HmdNotFound"); RETC(s); }

static const tm_shim_def misc_shims[] = {
#define A(n) TM_SHIM("advapi32.dll", n)
    A(RegOpenKeyExA), A(RegQueryValueExA), A(RegEnumValueW), A(RegCloseKey), A(CryptAcquireContextW),
    A(CryptReleaseContext), A(CryptGenRandom), A(GetUserNameW), A(RegisterEventSourceA), A(DeregisterEventSource),
    A(ReportEventA), A(OpenProcessToken), A(AllocateAndInitializeSid), A(FreeSid), A(SetEntriesInAclW),
    A(GetSecurityInfo), A(SetSecurityInfo),
#undef A
    TM_SHIM("shell32.dll", SHGetFolderPathW), TM_SHIM("shell32.dll", ShellExecuteW), TM_SHIM("shell32.dll", ShellExecuteExW),
    TM_SHIM("shell32.dll", SHFileOperationW), TM_SHIM("shell32.dll", SHParseDisplayName), TM_SHIM("shell32.dll", CommandLineToArgvW),
    TM_SHIM("ole32.dll", CoInitialize), TM_SHIM("ole32.dll", CoInitializeEx), TM_SHIM("ole32.dll", CoUninitialize),
    TM_SHIM("ole32.dll", CoSetProxyBlanket), TM_SHIM("ole32.dll", CoCreateInstance), TM_SHIM("ole32.dll", CoCreateGuid),
    TM_SHIM("ole32.dll", CoTaskMemFree), TM_SHIM("ole32.dll", CLSIDFromString),
    TM_SHIM("oleaut32.dll", SysAllocString), TM_SHIM("oleaut32.dll", SysFreeString),
    TM_SHIM("rpcrt4.dll", UuidCreate), TM_SHIM("rpcrt4.dll", UuidToStringA), TM_SHIM("rpcrt4.dll", RpcStringFreeA),
    TM_SHIM("version.dll", GetFileVersionInfoSizeW), TM_SHIM("version.dll", GetFileVersionInfoW), TM_SHIM("version.dll", VerQueryValueW),
    TM_SHIM("userenv.dll", GetUserProfileDirectoryW),
    TM_SHIM("iphlpapi.dll", GetAdaptersInfo), TM_SHIM("iphlpapi.dll", GetAdaptersAddresses), TM_SHIM("iphlpapi.dll", GetBestInterface),
    TM_SHIM("iphlpapi.dll", GetIfEntry),
    TM_SHIM("setupapi.dll", SetupDiGetClassDevsExW), TM_SHIM("setupapi.dll", SetupDiEnumDeviceInterfaces),
    TM_SHIM("setupapi.dll", SetupDiGetDeviceInterfaceDetailW), TM_SHIM("setupapi.dll", SetupDiOpenDevRegKey),
    TM_SHIM("gdi32.dll", CreateDCA), TM_SHIM("gdi32.dll", DeleteDC), TM_SHIM("gdi32.dll", ExtEscape), TM_SHIM("gdi32.dll", GetDIBits),
    TM_SHIM("comdlg32.dll", GetOpenFileNameW), TM_SHIM("comdlg32.dll", GetSaveFileNameW), TM_SHIM("comdlg32.dll", CommDlgExtendedError),
#define ST(n) TM_SHIM("steam_api.dll", n)
    ST(SteamAPI_Init), ST(SteamAPI_IsSteamRunning), ST(SteamAPI_Shutdown), ST(SteamAPI_RunCallbacks),
    ST(SteamAPI_RegisterCallback), ST(SteamAPI_UnregisterCallback), ST(SteamAPI_RegisterCallResult),
    ST(SteamAPI_UnregisterCallResult), ST(SteamAPI_GetHSteamUser), ST(SteamAPI_GetHSteamPipe), ST(SteamInternal_CreateInterface),
#undef ST
#define VR(n) TM_SHIM("openvr_api.dll", n)
    VR(VR_IsHmdPresent), VR(VR_IsRuntimeInstalled), VR(VR_InitInternal), VR(VR_ShutdownInternal), VR(VR_GetInitToken),
    VR(VR_IsInterfaceVersionValid), VR(VR_GetGenericInterface), VR(VR_GetVRInitErrorAsSymbol),
#undef VR
};

void tm_user32_register(void);
void tm_uplay_register(void);
void tm_misc_register(void) {
    tm_register_shims(misc_shims, sizeof misc_shims / sizeof misc_shims[0]);
    tm_user32_register();
    tm_uplay_register();
}
