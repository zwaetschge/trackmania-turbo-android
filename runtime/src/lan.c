/*
 * lan.c - games over the local network (several devices on one Wi-Fi).
 *
 * The engine still contains ManiaPlanet's LAN server and client; the Turbo menus just
 * never offer them. The start screen (MainActivity/Launcher, page "LAN-Spiel") starts
 * the game with
 *
 *   host:  TM_LAN=host  TM_NET_LAN=1  TM_CMDLINE="/server /lan"
 *          TM_LAN_MATCH=<MatchSettings file, relative to Documents\TrackmaniaTurbo\Maps>
 *   join:  TM_LAN=join  TM_NET_LAN=1  TM_LAN_JOIN=<ip>:<port>  TM_CMDLINE="/lan"
 *
 * The command line is read by the app at start-up (0x00890F10): "/server" sets the
 * network object's pending action to 2 = start a server (the same as the XML-RPC method
 * StartServerLan), "/lan" marks it as a LAN game (no master server). net.c opens the
 * local network for the sockets (TM_NET_LAN).
 *
 * Joining is NOT done with "/join=": the app handles that before it has loaded the Turbo
 * title, and the connection then fails with "Wrong title." (0x009C0AE0 compares the
 * server's title with the client's current one). Instead the runtime waits until the
 * client's title is loaded (CGameCtnApp::HeavyUpdate, 0x008A4440, virtual, every frame)
 * and then does what the XML-RPC method JoinServerLan (0x008B13A0) does: pending action
 * 1 with the address in the network object (+0x160, password +0x170, +0x178 = 1,
 * +0x180 = 1); the app picks it up like a join from a server browser.
 *
 * What a server needs and the command line cannot give is its map list: it starts with
 * the "current selection" of the server, which only the old native menus or the XML-RPC
 * method LoadMatchSettings fill, so a plain "/server" stops with "No maps in the playlist
 * match the server settings". Host mode therefore loads the MatchSettings file into the
 * network object right before the server checks its playlist for the first time, with
 * the engine's own LoadMatchSettings:
 *
 *   0x00E61090  CTrackManiaNetwork playlist check, vtable+0x278 of 0x0183B8A8,
 *               thiscall(CFastString &error) -> 0 = no map fits the server settings,
 *               ret 4. Called through the vtable by the server start (0x00A18FB6 in
 *               0x00A18EA0, and by the XML-RPC StartServerLan), so the runtime can wrap
 *               it (tm_add_override). (The server coroutine 0x00A17570 is resumed at
 *               inner addresses, so wrapping its entry catches nothing.)
 *   vtable+0x27C CTrackManiaNetwork::LoadMatchSettings(const CFastStringInt &file,
 *               CFastString &error) -> number of maps, < 0 on error (0x00EA7DF0, the
 *               XML-RPC handler 0x00EAC170 calls it through 0x00EA0550).
 *   0x0074F470  cdecl Utf8ToWide(const char *, CFastStringInt *).
 *   0x016A8FB4 / 0x016A8FB8  globals with the engine's empty narrow / wide string.
 *
 * The MatchSettings file is the format of the dedicated server (<playlist>, <gameinfos>
 * with <script_name> = one of the title's mode scripts, e.g.
 * TrackMania\TMC_OnlineTimeAttack.Script.txt, <filter> with <is_lan>1</is_lan>, and the
 * maps as <map><file>Campaigns\01_White\01_Canyon\001.Map.Gbx</file></map>).
 */
#include "tmrt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline uint32_t ntohl_u(uint32_t v) { return __builtin_bswap32(v); }
#define VA_PLAYLIST_CHECK    0x00E61090u
#define VA_NET_BEFORE_ACTIVATE 0x00E62570u   /* CTrackManiaNetwork vtable+0x1A4, thiscall(app), ret 4 */
#define NET_OPEN_SERVER_PORTS 0x218u         /* CNetSystem config (net+0x204) +0x14 */
#define VTBL_LOADMATCHSETTINGS 0x27Cu
#define VA_UTF8_TO_WSTR      0x0074F470u
#define VA_EMPTY_STR         0x016A8FB4u
#define VA_EMPTY_WSTR        0x016A8FB8u

static recomp_func_t g_orig_check, g_orig_before_activate, g_orig_heavy;
static const char *g_join;            /* "ip:port" */
static int g_join_state;              /* 0 waiting for the title, 1 asked, 2 done */
static uint64_t g_title_seen_ns, g_last_poll_ns, g_join_at_ns;
static int g_join_tries;              /* join attempts so far (a failed one is retried a few times) */
#define JOIN_TRIES 6
static char g_title[64];

#define VA_APP_HEAVY_UPDATE  0x008A4440u   /* CGameCtnApp::HeavyUpdate, thiscall, virtual */
#define APP_NETWORK          0x14Cu
#define NET_TITLE_SRC        0x26Cu        /* object whose vtable+0x160() gives the current title's id */
#define VA_MWID_GETNAME      0x005035A0u   /* thiscall CMwId::GetName(CFastString &out, int) */
#define VA_FASTSTRING_ASSIGN 0x00406A70u   /* thiscall CFastString::Assign(CFastString by value) */
#define VA_NET_CLEAR_184     0x009FEB90u
#define VA_NET_CLEAR_19C     0x009FEBD0u
#define VA_NET_CLEAR_160     0x009FEBF0u

/* The title the network object thinks is current ("" until the title is loaded). */
static const char *current_title(uint32_t net) {
    static uint32_t tmp;
    uint32_t o = rd32(net + NET_TITLE_SRC), vt = o ? rd32(o) : 0, fn = vt ? rd32(vt + 0x160) : 0;
    g_title[0] = 0;
    if (!fn) return g_title;
    if (!tmp) tmp = tm_galloc(8);
    wr32(tmp, rd32(VA_EMPTY_STR)); wr32(tmp + 4, 0);
    uint32_t saved = g_ecx;
    uint32_t id = tm_call_guest_this(fn, o, 0);                 /* -> the title's CMwId */
    if (id) tm_call_guest_this(VA_MWID_GETNAME, id, 2, tmp, 0);
    g_ecx = saved;
    uint32_t p = rd32(tmp), n = rd32(tmp + 4);
    if (p && n && n < sizeof g_title) { memcpy(g_title, G2H(p), n); g_title[n] = 0; }
    return g_title;
}

/* Class name of an engine object from its MSVC RTTI (vtable[-1] -> COL -> TypeDescriptor). */
static const char *rtti_name(uint32_t obj) {
    static __thread char buf[4][24]; static __thread int k;
    if (!obj || obj < 0x10000) return "-";
    uint32_t vt = rd32(obj);
    char *b = buf[k++ & 3]; snprintf(b, 24, "vt %08X", vt);
    if (vt < 0x016A3000u || vt >= 0x01C00000u) return b;
    uint32_t col = rd32(vt - 4);
    if (col < 0x016A3000u || col >= 0x01C00000u) return b;
    uint32_t td = rd32(col + 12);
    if (td < 0x016A3000u || td >= 0x01D00000u) return b;
    return (const char *)G2H(td + 8);
}

static void assign(uint32_t dst, const char *text) {
    uint32_t g = tm_gstrdup(text);
    tm_call_guest_this(VA_FASTSTRING_ASSIGN, dst, 2, g, (uint32_t)strlen(text));
}

/* CFastString {char *p; uint32 len} -> printable C string (for logs). */
static const char *fstr(uint32_t s, char *buf, size_t n) {
    uint32_t p = rd32(s), len = rd32(s + 4);
    size_t k = 0;
    for (; p && k < len && k + 1 < n; k++) { uint8_t c = rd8(p + k); buf[k] = c >= 32 && c < 127 ? (char)c : '?'; }
    buf[k] = 0;
    return buf;
}

#define NET_MASTER           0x264u        /* CNetMasterServer (derived, vtable 0x017F3E40) */
#define VA_MS_ADD_USER       0x00531DB0u   /* thiscall AddUser(const uint32_t *id, int makeCurrent) -> index */

/* The LAN login of this device: the player name (lower case, [a-z0-9_-]) + "_" + the
   last byte of the IP, so two devices of the same player still differ. */
static const char *lan_login(void) {
    static char login[32];
    if (login[0]) return login;
    size_t n = 0;
    const char *name = getenv("TM_PLAYER_NAME");
    for (const char *c = name ? name : ""; *c && n < 16; c++) {
        char ch = *c >= 'A' && *c <= 'Z' ? (char)(*c - 'A' + 'a') : *c;
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-') login[n++] = ch;
    }
    if (!n) { memcpy(login, "player", 6); n = 6; }
    uint32_t a = 0;
    tm_net_local_ip(&a, NULL, NULL, 0);
    snprintf(login + n, sizeof login - n, "_%u", ntohl_u(a) & 0xFF);
    return login;
}

/* The local player info (net+0xBB4, class vtable 0x017C15C0, ctor 0x009B9F90) keeps the
   nickname other players see in the CFastStringInt +0xC; offline nothing replaces its
   default "Unnamed". Use the player name of the start screen. */
static void set_player_name(uint32_t net) {
    uint32_t pi = net ? rd32(net + 0xbb4) : 0;
    const char *name = getenv("TM_PLAYER_NAME");
    if (!pi || !name || !*name) return;
    static const char def[] = "Unnamed";
    uint32_t p = rd32(pi + 0xC), n = rd32(pi + 0x10);
    if (!p || n != sizeof def - 1) return;
    for (uint32_t i = 0; i < n; i++) if (rd16(p + 2 * i) != (uint16_t)def[i]) return;
    tm_call_guest(VA_UTF8_TO_WSTR, 2, tm_gstrdup(name), pi + 0xC);
    TM_INFO("lan: nickname of the local player info %08X: '%s' (length %u)", pi, name, rd32(pi + 0x10));
}

/* Offline the master server user AddUser makes has no login, and two places need one:
   - the join request (0x00C55150, subtype 3) carries the login 0x00555DC0 reads: the
     CFastString +0x1C XOR the key CFastString +0x24, whose length (+0x28) is the
     login's; its writer stops at a login shorter than 1 character, so the host got a
     cut request it could not read;
   - the local player info (net+0xBB4, login CFastString +0x30, normally copied from the
     user's plain login +0x14 by 0x008884C0) goes to the host in the player info message;
     the host's checks (0x009BAC40) ban a player whose login there is empty ("internal
     checks (2) failed") or differs from the connection's ((0), "login may be forged"). */
static void set_user_login(uint32_t net, uint32_t u) {
    const char *login = lan_login();
    uint32_t n = (uint32_t)strlen(login);
    if (u && !rd32(u + 0x28)) {
        uint32_t data = tm_galloc(n + 1), key = tm_galloc(n + 1);
        for (uint32_t i = 0; i < n; i++) { wr8(key + i, 0x80); wr8(data + i, (uint8_t)(login[i] ^ 0x80)); }
        wr8(data + n, 0); wr8(key + n, 0);
        tm_call_guest_this(VA_FASTSTRING_ASSIGN, u + 0x1C, 2, data, n);
        tm_call_guest_this(VA_FASTSTRING_ASSIGN, u + 0x24, 2, key, n);
        if (!rd32(u + 0x18)) assign(u + 0x14, login);
        static int logged;
        if (logged++ < 2) TM_INFO("lan: login of the LAN user: '%s' (length %u)", login, rd32(u + 0x28));
    }
    set_player_name(net);
    uint32_t pi = net ? rd32(net + 0xbb4) : 0;
    if (pi && !rd32(pi + 0x34)) {
        assign(pi + 0x30, login);
        TM_INFO("lan: login of the local player info %08X: '%s'", pi, login);
    }
}

/* A join is sent in the name of the master server's current user (0x009C0AE0: 0x00532360
   gives [[ms+0x34]+0x10], 0x00531D10 the user object from the list at ms+0x28). Offline
   there is none, so every join failed with "Could not connect". Give the client one. */
static uint32_t g_user_idp, g_net;
static int g_user_logged;
static void ensure_master_user(uint32_t net) {
    { const char *nu = getenv("TM_LAN_NOUSER"); if (nu && *nu == '1') return; }   /* development: compare without the user */
    uint32_t ms = rd32(net + NET_MASTER);
    g_net = net;
    if (!ms || rd32(ms + 0x34)) return;
    uint32_t a = 0;
    if (!g_user_idp) {
        g_user_idp = tm_galloc(4);
        tm_net_local_ip(&a, NULL, NULL, 0);
        wr32(g_user_idp, 0x4C414E00u | (ntohl_u(a) & 0xFF));
    }
    uint32_t idp = g_user_idp;
    int32_t idx = (int32_t)tm_call_guest_this(VA_MS_ADD_USER, ms, 2, idp, 1);
    uint32_t u = rd32(ms + 0x34);
    set_user_login(net, u);
    if (g_user_logged++ < 3) TM_INFO("lan: master server user created: index %d, user %08X (%s), list %u", idx, u, rtti_name(u), rd32(ms + 0x2c));
    uint32_t um = rd32(0x01C1B5ACu);
    if (um && getenv("TM_LAN_DEBUG")) {
        uint32_t lp = rd32(um + 0x1c), ln = rd32(um + 0x20);
        TM_INFO("lan: user manager %08X (%s): +1C %08X +20 %u first %08X (%s) ms+58 %08X n %u", um, rtti_name(um), lp, ln,
                ln && lp ? rd32(lp) : 0, ln && lp ? rtti_name(rd32(lp)) : "-", rd32(ms + 0x58), rd32(rd32(ms + 0x58) + 0x20));
    }
    if (u && getenv("TM_LAN_DEBUG") && g_user_logged == 1) {
        char line[256];
        for (uint32_t o = 0; o < 0x110; o += 0x20) {
            int k = snprintf(line, sizeof line, "+%03X:", o);
            for (uint32_t j = 0; j < 0x20; j += 4) k += snprintf(line + k, sizeof line - k, " %08X", rd32(u + o + j));
            TM_INFO("lan: user%s", line);
        }
    }
}

/* 0x008B7DA0 (an app command, run through the scheduler, so it can be wrapped) syncs the
   master server's users with the account list, which is empty offline, and so removes
   the user ensure_master_user() made: put it back after every run while joining. */
#define VA_APP_USER_SYNC 0x008B7DA0u
static recomp_func_t g_orig_user_sync;
static void hook_user_sync(void) {
    g_orig_user_sync();
    if (g_join && g_join_state >= 1 && g_net) {
        uint32_t saved_eax = g_eax;
        ensure_master_user(g_net);
        g_eax = saved_eax;
    }
}

/* A join that failed before the client got in is tried again a few times: typically the
   host is still loading its map and answers too late. Two ways it ends: the join step
   sets the error flag net+0x1D4 with "Could not connect..." (seen in hook_heavy_update),
   or the server-info coroutine gives up without an answer (0x00A16700, "could not
   contact", hook site; no error flag). A lost connection later ("disconnected") is not
   retried. */
static void join_failed(const char *why) {
    if (!g_join || g_join_state != 1 || g_join_tries >= JOIN_TRIES) return;
    g_join_state = 0;
    g_join_at_ns = tm_now_ns() + 8000000000ull;
    TM_INFO("lan: join failed (%s); trying again in 8 s (attempt %d of %d)", why, g_join_tries + 1, JOIN_TRIES);
    tm_status("LAN: %s - trying again", why);
}
static void hook_join_no_contact(uint32_t va) {
    (void)va;
    /* 0x00A16700: cmp [ebp+0x10], 2 -> 2 = end quietly; anything else first shows "could not
       contact the server" and waits for the player to close it, which would hold up the
       next attempt. Quiet while attempts remain. */
    if (g_join && g_join_state == 1 && g_join_tries < JOIN_TRIES) wr32(g_ebp + 0x10, 2);
    join_failed("no answer from the server");
}

static void hook_heavy_update(void) {
    uint32_t app = g_ecx;
    uint64_t now = tm_now_ns();
    if ((g_join_state < 2 || getenv("TM_LAN_DEBUG")) && now - g_last_poll_ns > 100000000ull) {
        g_last_poll_ns = now;
        uint32_t net = rd32(app + APP_NETWORK);
        if (net) {
            char was[64]; strcpy(was, g_title);
            const char *t = current_title(net);
            if (strcmp(was, t)) TM_INFO("lan: current title '%s'", t);
            if (g_join && *t) {
                if (!g_title_seen_ns) { g_title_seen_ns = now; g_join_at_ns = now + 3000000000ull; }
                if (g_join_state == 0 && now >= g_join_at_ns && rd32(net + 0x15c) == 0) {
                    g_join_tries++;
                    ensure_master_user(net);
                    /* JoinServerLan (0x008B1424..): pending action 1 = join this address */
                    wr32(net + 0x15c, 1);
                    tm_call_guest_this(VA_NET_CLEAR_184, net + 0x184, 0);
                    tm_call_guest_this(VA_NET_CLEAR_19C, net + 0x19c, 0);
                    tm_call_guest_this(VA_NET_CLEAR_160, net + 0x160, 0);
                    wr32(net + 0x180, 1);
                    assign(net + 0x160, g_join);
                    assign(net + 0x170, "");
                    wr32(net + 0x178, 1);                       /* 1 = join as a player (2 spectate, 3 referee) */
                    const char *sil = getenv("TM_LAN_SILENT");
                    wr32(net + 0x17c, sil && *sil == '0' ? 0 : 1); /* silent: no confirmation dialog */
                    g_join_state = 1;
                    { char x[48], y[48]; uint32_t bb4 = rd32(net + 0xbb4);
                      TM_INFO("lan: joining %s, attempt %d (title '%s'); net+1FC '%s', [net+BB4]+58 '%s'", g_join, g_join_tries, t,
                              fstr(net + 0x1fc, x, sizeof x), bb4 ? fstr(bb4 + 0x58, y, sizeof y) : "-"); }
                    tm_status("LAN: joining %s (attempt %d)", g_join, g_join_tries);
                }
            }
            if (!g_join && *t) g_join_state = 2;              /* host: only log the title */
            if (getenv("TM_LAN_DEBUG")) {                     /* development: the client's server connection */
                static uint32_t last_c = 0xFFFFFFFFu, last_q = 0xFFFFFFFFu;
                uint32_t cl = rd32(net + 0x264), c = cl ? rd32(cl + 0x34) : 0, q = rd32(net + 0x1e0 + 4);
                if (c != last_c || q != last_q) TM_INFO("lan: client %08X (%s) server connection %08X (%s), queries %u, pending %u, state1d4 %u, net+250 %08X (%s), net+25c %s, net+260 %s",
                    cl, rtti_name(cl), c, rtti_name(c), q, rd32(net + 0x15c), rd32(net + 0x1d4), rd32(net + 0x250), rtti_name(rd32(net + 0x250)),
                    rtti_name(rd32(net + 0x25c)), rtti_name(rd32(net + 0x260)));
                last_c = c; last_q = q;
            }
            static uint32_t last_err;
            uint32_t ef = rd32(net + 0x1d4);
            if (ef == 1 && ef != last_err) {                         /* the connection failed: why (CFastString at net+0x1CC) */
                uint32_t p = rd32(net + 0x1cc), len = rd32(net + 0x1d0);
                char msg[128] = "";
                for (uint32_t i = 0; p && i < len && i + 1 < sizeof msg; i++) { uint16_t c = rd16(p + 2 * i); msg[i] = c < 128 ? (char)c : '?'; msg[i + 1] = 0; }
                TM_INFO("lan: connection failed (flag %u) '%s', pending action %u", ef, msg, rd32(net + 0x15c));
                /* A join that failed before the client got in ("Could not connect...", typically
                   the host is still loading its map and answers too late) is tried again a few
                   times; a lost connection later ("disconnected") is not. */
                if (!strncmp(msg, "Could not connect", 17)) join_failed(msg);
            }
            last_err = ef;
        }
        g_ecx = app;
    }
    g_orig_heavy();
}
static int g_loaded;
static const char *g_match;

static void hook_playlist_check(void) {
    if (!g_loaded) {
        g_loaded = 1;
        uint32_t self = g_ecx, vt = self ? rd32(self) : 0;
        uint32_t fn = vt ? rd32(vt + VTBL_LOADMATCHSETTINGS) : 0;
        if (fn) {
            uint32_t name = tm_gstrdup(g_match), w = tm_galloc(8), e = tm_galloc(8);
            wr32(w, rd32(VA_EMPTY_WSTR)); wr32(w + 4, 0);
            wr32(e, rd32(VA_EMPTY_STR)); wr32(e + 4, 0);
            tm_call_guest(VA_UTF8_TO_WSTR, 2, name, w);
            int32_t n = (int32_t)tm_call_guest_this(fn, self, 2, w, e);
            const char *err = rd32(e) && rd32(e + 4) ? (const char *)G2H(rd32(e)) : "";
            TM_INFO("lan: LoadMatchSettings(%s) on network object %08X (vtable %08X) -> %d %s", g_match, self, vt, n, err);
            TM_INFO("lan: server title '%s'", current_title(self));
            set_player_name(self);
            tm_status("LAN server: %d maps from %s", n, g_match);
        }
        if (getenv("TM_LAN_DEBUG")) {                       /* development: the network object's fields */
            char line[256];
            for (uint32_t o = 0x150; o < 0x2c0; o += 0x20) {
                int k = snprintf(line, sizeof line, "+%03X:", o);
                for (uint32_t j = 0; j < 0x20; j += 4) k += snprintf(line + k, sizeof line - k, " %08X", rd32(self + o + j));
                TM_INFO("lan: net%s", line);
            }
        }
        const char *rs = getenv("TM_LAN_RESTART");               /* development: reopen the network during the server start */
        if (rs && *rs == '1') {
            TM_INFO("lan: restart network (development), active %08X", rd32(self + 0x25c));
            tm_call_guest_this(0x009C2A40u, self, 1, 0);
            wr32(self + NET_OPEN_SERVER_PORTS, 1);
            tm_call_guest_this(0x009C2A40u, self, 1, 1);
        }
        g_ecx = self;
    }
    g_orig_check();
}

/* Right before the app switches the network on (0x008B66C4, then 0x009C2A40(1)): ask
   for the server ports. Without them a server runs locally only (as for solo play): the
   network system opens one UDP client socket and nothing listens. */
static void hook_before_activate(void) {
    uint32_t self = g_ecx;
    g_orig_before_activate();
    wr32(self + NET_OPEN_SERVER_PORTS, 1);
    TM_INFO("lan: network object %08X: server ports on", self);
}

/* 0x009C0BE8 in 0x009C0AE0 (hook site): the join is about to fetch the master server's
   current user; make sure there is one (the engine's user sync drops it again and again). */
static void hook_join_user(uint32_t va) {
    (void)va;
    if (!g_join) return;
    uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx;
    ensure_master_user(g_edi);                               /* edi = the network object here */
    uint32_t ms = rd32(g_edi + NET_MASTER), u = ms ? rd32(ms + 0x34) : 0;
    set_user_login(g_edi, u);
    /* 0x009C0AE0(info, mode, &password, &net+0x1FC, [net+0xBB4]+0x58): mode 1 play,
       2 spectate, 3 (both); the request's writer rejects anything else */
    uint32_t mode = rd32(g_esp + 0x58);
    if (mode < 1 || mode > 3) { TM_INFO("lan: join mode %u -> 1 (play)", mode); wr32(g_esp + 0x58, 1); }
    g_eax = eax; g_ecx = ecx; g_edx = edx;
}

/* 0x009C0E78 in 0x009C0AE0 (hook site): the join request (ebx, class vtable 0x01806C98)
   is complete and about to be sent; its writer 0x00C55150 checks these bounds. */
static void hook_join_request(uint32_t va) {
    (void)va;
    uint32_t r = g_ebx;
    char a[48], b[48], c[48], d[48];
    TM_INFO("lan: join request: login '%s' (max %u), mode %u, +74 '%s' (1..10), +7C '%s' (10..30), +84 '%s', +94 len %u, +9C len %u, +A4 %u",
            fstr(r + 0x4C, a, sizeof a), rd32(0x01BF33E4u), rd32(r + 0x70), fstr(r + 0x74, b, sizeof b), fstr(r + 0x7C, c, sizeof c),
            fstr(r + 0x84, d, sizeof d), rd32(r + 0x98), rd32(r + 0xA0), rd8(r + 0xA4));
}

/* 0x009CBD45 (hook site, LAN host): CTrackManiaNetwork's check of a joining player's
   login (0x009CBC00) verifies a signature made with the master server's keys (0x00531CC0,
   0x00418BF0) and answers "Corrupted login." otherwise. Offline nobody has such a
   signature, so on our LAN server it passes. */
static void hook_login_check(uint32_t va) {
    static int n;
    (void)va;
    if (g_esi == 0) { if (n++ < 20) TM_INFO("lan: accepting a LAN player without a master server signature"); g_esi = 1; }
}

static int g_join_env(void) { const char *j = getenv("TM_LAN_JOIN"); return j && *j; }

/* TM_LAN_DEBUG: which check of the message parser 0x0051E2F0 sends it to its failure label */
static const uint32_t g_parse_fail_sites[] = { 0x0051E39Bu, 0x0051E3BCu, 0x0051E3C8u, 0x0051E5F0u, 0x0051E60Eu, 0x0051E654u, 0x0051E66Cu, 0x0051E6A7u, 0x0051E6C7u, 0x0051E727u, 0x0051E7BAu, 0x0051E7C2u, 0x0051E7FFu, 0x0051E8E4u, 0x0051E959u, 0x0051E989u, 0x0051E9B8u, 0x0051E9D7u, 0x0051EA83u, 0x0051EA91u, 0x0051EB13u };
static __thread uint32_t t_last_parse_site;
static void parse_site_hook(uint32_t va) { t_last_parse_site = va; }

/* TM_LAN_DEBUG: trace the join's get-info coroutine (hook sites in config/hooks.txt) */
static void trace_hook(uint32_t va) {
    static int n;
    if (n++ > 3000) return;
    switch (va) {
    case 0x00A15A85: { uint32_t st = rd32(g_esi);
        if (st && rd32(st + 4) == 0x1237 && rd32(st + 0x48) < 60000) wr32(st + 0x48, 60000);   /* slow test client: wait longer */
        TM_INFO("lan-trace A159E0 state %X (obj %08X) info %08X", st ? rd32(st + 4) : 0, st, g_edi); break; }
    case 0x0051E604: TM_INFO("lan-trace parse: first byte %02X", rd8(g_esp + 0x23)); break;
    case 0x0051E641: TM_INFO("lan-trace parse: type %02X", g_eax & 0xFF); break;
    case 0x0051E662: TM_INFO("lan-trace parse: entry %08X +8 %X +C %X +14 %X", g_ebp, rd32(g_ebp + 8), rd32(g_ebp + 0xc), rd32(g_ebp + 0x14)); break;
    case 0x0051E6DA: TM_INFO("lan-trace parse: conn id %04X, connection's %04X", rd32(g_esp + 0x30) & 0xFFFF, g_edi ? rd32(g_edi + 0xe4) : 0xFFFF); break;
    case 0x0051E4A7: TM_INFO("lan-trace parse: FAILED after the check at %08X", t_last_parse_site); break;
    case 0x0052194D: TM_INFO("lan-trace TcpReceive: parsed %08X", g_eax); break;
    case 0x00A15B11: TM_INFO("lan-trace A159E0 connection state %u", g_eax); break;
    case 0x00A15B43: { uint32_t st = rd32(g_esi); TM_INFO("lan-trace A159E0 finalize: pings %u total %u timeout-base %u", rd32(st + 0x40), rd32(st + 0x44), rd32(st + 0x30)); break; }
    case 0x00A16700: TM_INFO("lan-trace A159E0 could not contact: info %08X +38 %u +3C %d global %u", g_edi, g_edi ? rd32(g_edi + 0x38) : 0, g_edi ? (int)rd32(g_edi + 0x3c) : 0, rd32(0x01C0B47Cu)); break;
    case 0x009C196D: TM_INFO("lan-trace 9C1820 info reply (sub %u)", rd32(g_ebx + 0x18)); break;
    case 0x009C0B73: TM_INFO("lan-trace 9C0AE0 title: client len %u, server len %u ('%.40s')", g_eax, rd32(g_ebp + 0x2c), rd32(g_ebp + 0x28) ? (const char *)G2H(rd32(g_ebp + 0x28)) : ""); break;
    case 0x009C0BD1: TM_INFO("lan-trace 9C0AE0 network system %08X", rd32(g_edi + 0x25c)); break;
    case 0x009C0C07: TM_INFO("lan-trace 9C0AE0 user object %08X", g_eax); break;
    case 0x009C0E78: TM_INFO("lan-trace 9C0AE0 sending the join request to %08X", rd32(g_ebp + 0x30)); break;
    case 0x009C0E9D: TM_INFO("lan-trace 9C0AE0 wrong title"); break;
    case 0x009C18F8: TM_INFO("lan-trace 9C1820 message class %08X sub %u conn %08X", g_eax, rd32(g_ebx + 0x18), g_ebp); break;
    }
}

/* TM_LAN_SYNC_ALWAYS=1 (development, host): the server answers a client's clock-sync
   message (class 0x03069000) in 0x009CC250 only while its own frame time is under 300 ms
   ([0x01BF1CF8] < 300 and now - [0x01BF1CF0] < 300, clock 0x01BF1CE8). The x86 test host
   is slower than that, so its clients never leave synchronisation ("WAITING"). Around the
   server's message handler (0x00C54440, virtual, vtable 0x01806B20) the frame looks fresh. */
#define VA_NET_SERVER_ON_MESSAGE 0x00C54440u
#define VA_CLOCK_NOW_MS          0x00514560u
static recomp_func_t g_orig_server_msg;
static int g_sync_always;
static void hook_server_msg(void) {
    uint32_t saved_ecx = g_ecx, cf0 = rd32(0x01BF1CF0u), cf8 = rd32(0x01BF1CF8u);
    uint32_t now = tm_call_guest_this(VA_CLOCK_NOW_MS, 0x01BF1CE8u, 0);
    g_ecx = saved_ecx;
    if (getenv("TM_LAN_CLOCKLOG")) {                    /* development: the frame-time check of 0x009CC250 */
        uint32_t msg = rd32(g_esp + 4), vt = msg ? rd32(msg) : 0;
        uint32_t cls = vt ? tm_call_guest_this(rd32(vt + 0xc), msg, 0) : 0;
        g_ecx = saved_ecx;
        static int n;
        if (cls == 0x03069000u && n++ < 40)
            TM_INFO("lan: clock-sync message: frame dt [CF8] %u ms, now - [CF0] %d ms -> %s", cf8, (int)(now - cf0),
                    cf8 < 300 && now - cf0 < 300 ? "answered" : "ignored (server too slow)");
    }
    if (!g_sync_always) { g_orig_server_msg(); return; }
    wr32(0x01BF1CF0u, now); wr32(0x01BF1CF8u, 16);
    g_orig_server_msg();
    uint32_t eax = g_eax;
    wr32(0x01BF1CF0u, cf0); wr32(0x01BF1CF8u, cf8);
    g_eax = eax;
}

/* Called once the image is mapped. */
void tm_lan_register(void) {
    const char *mode = getenv("TM_LAN");
    if (!mode || !*mode) return;
    if (g_join_env()) {
        tm_hook_register(0x009C0BE8u, hook_join_user);
        tm_hook_register(0x009C0E78u, hook_join_request);
        tm_hook_register(0x00A16700u, hook_join_no_contact);
    }
    if (!strcmp(mode, "host")) tm_hook_register(0x009CBD45u, hook_login_check);
    { const char *sa = getenv("TM_LAN_SYNC_ALWAYS");
      g_sync_always = sa && *sa == '1';
      if ((g_sync_always || getenv("TM_LAN_CLOCKLOG")) && !strcmp(mode, "host") && (g_orig_server_msg = recomp_lookup(VA_NET_SERVER_ON_MESSAGE)))
          tm_add_override(VA_NET_SERVER_ON_MESSAGE, hook_server_msg); }
    if (getenv("TM_LAN_DEBUG")) {
        static const uint32_t sites[] = { 0x00A15A85, 0x00A15B11, 0x00A15B43, 0x00A16700, 0x009C196D, 0x009C18F8,
                                          0x009C0B73, 0x009C0BD1, 0x009C0C07, 0x009C0E78, 0x009C0E9D,
                                          0x0051E604, 0x0051E641, 0x0051E662, 0x0051E6DA, 0x0051E4A7, 0x0052194D };
        for (unsigned i = 0; i < sizeof sites / sizeof sites[0]; i++) tm_hook_register(sites[i], trace_hook);
        for (unsigned i = 0; i < sizeof g_parse_fail_sites / sizeof g_parse_fail_sites[0]; i++) tm_hook_register(g_parse_fail_sites[i], parse_site_hook);
    }
    const char *cl = getenv("TM_CMDLINE");
    TM_INFO("lan: %s game, command line \"%s\"", mode, cl ? cl : "");
    const char *ports = getenv("TM_LAN_PORTS");                 /* development: server ports on a client too */
    if (!strcmp(mode, "host") || (ports && *ports == '1')) {
        g_orig_before_activate = recomp_lookup(VA_NET_BEFORE_ACTIVATE);
        if (g_orig_before_activate) tm_add_override(VA_NET_BEFORE_ACTIVATE, hook_before_activate);
        else TM_WARN("lan: network start not found, no server ports");
    }
    g_join = getenv("TM_LAN_JOIN");
    if (g_join && !*g_join) g_join = NULL;
    if (g_join) {
        g_orig_user_sync = recomp_lookup(VA_APP_USER_SYNC);
        if (g_orig_user_sync) tm_add_override(VA_APP_USER_SYNC, hook_user_sync);
    }
    g_orig_heavy = recomp_lookup(VA_APP_HEAVY_UPDATE);
    if (g_orig_heavy) tm_add_override(VA_APP_HEAVY_UPDATE, hook_heavy_update);
    else TM_WARN("lan: app update not found");
    if (strcmp(mode, "host")) return;
    g_match = getenv("TM_LAN_MATCH");
    if (!g_match || !*g_match) g_match = "MatchSettings\\LanHost.txt";
    g_orig_check = recomp_lookup(VA_PLAYLIST_CHECK);
    if (!g_orig_check) { TM_WARN("lan: playlist check not found, the server will have no maps"); return; }
    tm_add_override(VA_PLAYLIST_CHECK, hook_playlist_check);
}
