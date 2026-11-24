/*
 * net.c - networking for offline play and for games on the local network.
 *
 * WININET (HTTP to the master server, news, ladders) reports "cannot connect".
 * Winsock works, but by default only on the loopback interface: the engine runs
 * even a solo game through its own client/server pair on 127.0.0.1. Nothing can
 * leave the device.
 *
 * TM_NET_LAN=1 (set by the start screen for a LAN game, see lan.c) opens the local
 * network as well: private IPv4 ranges (10/8, 172.16/12, 192.168/16, 169.254/16) and
 * broadcasts, so devices on the same Wi-Fi find each other and play together. The
 * internet stays unreachable either way.
 */
#define _GNU_SOURCE
#include "tmrt.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define WININET_HANDLE 0x1A000000u
#define ERROR_INTERNET_CANNOT_CONNECT 12029u

/* ------------------------------------------------------------ WININET */
static void shim_InternetOpenA(void) { RET(WININET_HANDLE, 5); }
static void shim_InternetConnectA(void) { RET(WININET_HANDLE + 1, 8); }
static void shim_HttpOpenRequestA(void) { RET(WININET_HANDLE + 2, 8); }
static void shim_HttpSendRequestA(void) { tm_set_last_error(ERROR_INTERNET_CANNOT_CONNECT); RET(0, 5); }
static void shim_HttpAddRequestHeadersA(void) { RET(1, 4); }
static void shim_HttpQueryInfoA(void) { tm_set_last_error(ERROR_INTERNET_CANNOT_CONNECT); RET(0, 5); }
static void shim_InternetReadFile(void) { if (ARG(3)) wr32(ARG(3), 0); RET(1, 4); }   /* 0 bytes = EOF */
static void shim_InternetCloseHandle(void) { RET(1, 1); }
static void shim_InternetQueryOptionA(void) {
    /* return the queried option length as 0 / a zeroed value */
    uint32_t buf = ARG(2), len = ARG(3);
    if (len && rd32(len) >= 4 && buf) wr32(buf, 0);
    RET(1, 4);
}
static void shim_InternetSetOptionA(void) { RET(1, 4); }
static void shim_InternetAttemptConnect(void) { RET(ERROR_INTERNET_CANNOT_CONNECT, 1); }
static void shim_InternetCheckConnectionA(void) { tm_set_last_error(ERROR_INTERNET_CANNOT_CONNECT); RET(0, 3); }
static void shim_InternetConnectW(void) { RET(WININET_HANDLE + 1, 8); }
static void shim_InternetSetStatusCallback(void) { RET(0, 2); }

/* ------------------------------------------------------------ WS2_32 */
/*
 * Winsock over host sockets, restricted to the loopback interface.
 *
 * The engine plays even a solo game through its own client/server pair
 * connected over 127.0.0.1, so sockets have to work. Offline play is kept by
 * policy instead of by failing every call: a socket may only talk to 127.0.0.0/8
 * (connect/sendto elsewhere fail with WSAENETUNREACH), binds are placed on the
 * loopback interface, and name resolution only knows the local host.
 *
 * Guest SOCKETs are small handles into a table of host descriptors. A call that
 * can wait (blocking accept/connect/recv/send, select with a timeout) releases
 * the machine lock so the peer - usually another guest thread - can run.
 */
#define WSAEINTR 10004
#define WSAEBADF 10009
#define WSAEINVAL 10022
#define WSAEMFILE 10024
#define WSAEWOULDBLOCK 10035
#define WSAEINPROGRESS 10036
#define WSAEALREADY 10037
#define WSAENOTSOCK 10038
#define WSAEMSGSIZE 10040
#define WSAEOPNOTSUPP 10045
#define WSAEAFNOSUPPORT 10047
#define WSAEADDRINUSE 10048
#define WSAEADDRNOTAVAIL 10049
#define WSAENETUNREACH 10051
#define WSAECONNABORTED 10053
#define WSAECONNRESET 10054
#define WSAENOBUFS 10055
#define WSAEISCONN 10056
#define WSAENOTCONN 10057
#define WSAETIMEDOUT 10060
#define WSAECONNREFUSED 10061
#define WSAHOST_NOT_FOUND 11001
#define SOCK_ERR 0xFFFFFFFFu

#define MAX_SOCK 256
#define SOCK_BASE 0x00000100u
static struct { int fd; int nonblock; } g_sock[MAX_SOCK];
static pthread_mutex_t g_sock_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_sock_init;
static __thread int t_wsa_err;

static uint32_t wsa_fail(int code) { t_wsa_err = code; tm_set_last_error((uint32_t)code); return SOCK_ERR; }
static int wsa_from_errno(int e) {
    switch (e) {
    case EINTR: return WSAEINTR;            case EBADF: return WSAENOTSOCK;       case EINVAL: return WSAEINVAL;
    case EMFILE: case ENFILE: return WSAEMFILE;
    case EAGAIN: return WSAEWOULDBLOCK;     case EINPROGRESS: return WSAEWOULDBLOCK; case EALREADY: return WSAEALREADY;
    case ENOTSOCK: return WSAENOTSOCK;      case EMSGSIZE: return WSAEMSGSIZE;    case EOPNOTSUPP: return WSAEOPNOTSUPP;
    case EAFNOSUPPORT: return WSAEAFNOSUPPORT; case EADDRINUSE: return WSAEADDRINUSE; case EADDRNOTAVAIL: return WSAEADDRNOTAVAIL;
    case ENETUNREACH: case EHOSTUNREACH: case EACCES: case EPERM: return WSAENETUNREACH;
    case ECONNABORTED: return WSAECONNABORTED; case ECONNRESET: case EPIPE: return WSAECONNRESET;
    case ENOBUFS: case ENOMEM: return WSAENOBUFS; case EISCONN: return WSAEISCONN; case ENOTCONN: return WSAENOTCONN;
    case ETIMEDOUT: return WSAETIMEDOUT;    case ECONNREFUSED: return WSAECONNREFUSED;
    default: return WSAEINVAL;
    }
}
static uint32_t wsa_errno(void) { return wsa_fail(wsa_from_errno(errno)); }

static uint32_t sock_put(int fd) {
    uint32_t h = 0;
    pthread_mutex_lock(&g_sock_mu);
    if (!g_sock_init) { for (int i = 0; i < MAX_SOCK; i++) g_sock[i].fd = -1; g_sock_init = 1; }
    for (int i = 0; i < MAX_SOCK; i++) if (g_sock[i].fd < 0) { g_sock[i].fd = fd; g_sock[i].nonblock = 0; h = SOCK_BASE + (uint32_t)i * 4u; break; }
    pthread_mutex_unlock(&g_sock_mu);
    return h;
}
static int sock_idx(uint32_t h) {
    uint32_t i = (h - SOCK_BASE) / 4u;
    return g_sock_init && h >= SOCK_BASE && i < MAX_SOCK && !(h & 3u) && g_sock[i].fd >= 0 ? (int)i : -1;
}
static int sock_fd(uint32_t h) { int i = sock_idx(h); return i < 0 ? -1 : g_sock[i].fd; }
static int sock_blocking(uint32_t h) { int i = sock_idx(h); return i >= 0 && !g_sock[i].nonblock; }

/* Policy: 127.0.0.0/8 is always reachable; with TM_NET_LAN=1 also the local network. */
static int g_lan = -1;
int tm_net_lan(void) {
    if (g_lan < 0) { const char *e = getenv("TM_NET_LAN"); g_lan = e && *e == '1'; if (g_lan) TM_INFO("net: local network open (TM_NET_LAN=1)"); }
    return g_lan;
}
static int ip_is_lan(uint32_t a /* host order */) {
    return (a >> 24) == 10 || (a >> 20) == 0xAC1u || (a >> 16) == 0xC0A8u || (a >> 16) == 0xA9FEu
        || a == 0xFFFFFFFFu;                                                 /* limited broadcast */
}
static int ip_allowed(uint32_t a_be) {
    uint32_t a = ntohl(a_be);
    return (a >> 24) == 127 || (tm_net_lan() && ip_is_lan(a));
}
static int addr_allowed(const uint8_t *sa, uint32_t len) {
    if (!sa || len < 8 || sa[0] != 2 || sa[1] != 0) return 0;               /* AF_INET */
    uint32_t a; memcpy(&a, sa + 4, 4);
    return ip_allowed(a);
}
/* The device's address on the local network (Wi-Fi first), network byte order; 0 if none. */
int tm_net_local_ip(uint32_t *addr_be, uint32_t *mask_be, char *ifname, size_t ifname_len) {
    struct ifaddrs *ifs = NULL, *best = NULL;
    if (getifaddrs(&ifs) != 0) return 0;
    for (struct ifaddrs *i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
        uint32_t a = ntohl(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr);
        if (!ip_is_lan(a)) continue;                                         /* mobile data etc. */
        if (!best || (!strncmp(i->ifa_name, "wlan", 4) && strncmp(best->ifa_name, "wlan", 4))) best = i;
    }
    int ok = best != NULL;
    if (ok) {
        *addr_be = ((struct sockaddr_in *)best->ifa_addr)->sin_addr.s_addr;
        if (mask_be) *mask_be = best->ifa_netmask ? ((struct sockaddr_in *)best->ifa_netmask)->sin_addr.s_addr : htonl(0xFFFFFF00u);
        if (ifname) snprintf(ifname, ifname_len, "%s", best->ifa_name);
    }
    freeifaddrs(ifs);
    return ok;
}
/* TM_NET_TRACE=1 (with TM_NET_LAN): every socket call on the local network side. */
static int g_trace = -1;
static int net_trace(void) { if (g_trace < 0) { const char *e = getenv("TM_NET_TRACE"); g_trace = e && (*e == '1' || *e == '2'); } return g_trace; }
#define NTRACE(...) do { if (net_trace()) TM_INFO(__VA_ARGS__); } while (0)
/* TM_NET_TRACE: the bytes of a TCP send/recv, 400 per line (TM_NET_TRACE=2: all of them) */
static void trace_bytes(const char *dir, const uint8_t *b, long n) {
    const char *t = getenv("TM_NET_TRACE");
    long max = t && *t == '2' ? n : (n < 400 ? n : 400);
    for (long o = 0; o < max; o += 400) {
        char hex[820]; int k = 0;
        for (long i = o; i < max && i < o + 400; i++) k += snprintf(hex + k, sizeof hex - k, "%02x", b[i]);
        TM_INFO("net: %s%5ld %s", dir, o, hex);
    }
}

/* LAN games: socket errors and closed connections (the engine throws CNetSystemError on them). */
static void lan_err(const char *what, uint32_t h, long r, int e) {
    static int n;
    if (!tm_net_lan() || n > 300) return;
    if (r < 0 && (e == EAGAIN || e == EWOULDBLOCK || e == EINPROGRESS)) return;
    n++;
    TM_INFO("net: %s(%X) -> %ld%s%s", what, h, r, r < 0 ? " " : "", r < 0 ? strerror(e) : "");
}
static void lan_note(const char *what, const uint8_t *sa) {
    static int n;
    if (!tm_net_lan() || n > 200) return;
    uint32_t a; uint16_t port; memcpy(&a, sa + 4, 4); memcpy(&port, sa + 2, 2); a = ntohl(a);
    if ((a >> 24) == 127 && strncmp(what, "connect", 7)) return;
    n++;
    TM_INFO("net: %s %u.%u.%u.%u:%u", what, a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255, ntohs(port));
}

static void shim_WSAStartup(void) {
    if (ARG(1)) { memset(G2H(ARG(1)), 0, 400); wr16(ARG(1), 0x0202); wr16(ARG(1) + 2, 0x0202); }
    RET(0, 2);
}
static void shim_WSACleanup(void) { RET(0, 0); }
static void shim_WSAGetLastError(void) { RET((uint32_t)t_wsa_err, 0); }
static void shim_WSASetLastError(void) { t_wsa_err = (int)ARG(0); RET(0, 1); }

static void shim_socket(void) {
    int af = (int)ARG(0), type = (int)ARG(1), proto = (int)ARG(2);
    if (af != 2) RET(wsa_fail(WSAEAFNOSUPPORT), 3);                       /* IPv4 only */
    int fd = socket(AF_INET, type == 2 ? SOCK_DGRAM : SOCK_STREAM, proto == 17 ? IPPROTO_UDP : proto == 6 ? IPPROTO_TCP : 0);
    if (fd < 0) RET(wsa_errno(), 3);
    uint32_t h = sock_put(fd);
    if (!h) { close(fd); RET(wsa_fail(WSAEMFILE), 3); }
    TM_INFO("socket(%s) -> %X", type == 2 ? "udp" : "tcp", h);
    RET(h, 3);
}
static void shim_closesocket(void) {
    int i = sock_idx(ARG(0));
    NTRACE("net: closesocket(%X)", ARG(0));
    if (i < 0) RET(wsa_fail(WSAENOTSOCK), 1);
    close(g_sock[i].fd);
    pthread_mutex_lock(&g_sock_mu); g_sock[i].fd = -1; pthread_mutex_unlock(&g_sock_mu);
    RET(0, 1);
}
static void shim_bind(void) {
    int fd = sock_fd(ARG(0));
    if (fd < 0) RET(wsa_fail(WSAENOTSOCK), 3);
    if (!ARG(1) || ARG(2) < 16) RET(wsa_fail(WSAEINVAL), 3);
    struct sockaddr_in sa; memcpy(&sa, G2H(ARG(1)), sizeof sa);
    sa.sin_family = AF_INET;
    /* offline: never listen on an external interface, everything binds to loopback;
       LAN game: the requested address (usually any) when it is a local one */
    if (!(tm_net_lan() && (sa.sin_addr.s_addr == htonl(INADDR_ANY) || ip_allowed(sa.sin_addr.s_addr))))
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (tm_net_lan()) { int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one); }
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        int e = errno;
        TM_INFO("bind(%X, %s:%u) failed: %s", ARG(0), inet_ntoa(sa.sin_addr), ntohs(sa.sin_port), strerror(e));
        errno = e; RET(wsa_errno(), 3);
    }
    TM_INFO("bind(%X, %s:%u)", ARG(0), inet_ntoa(sa.sin_addr), ntohs(sa.sin_port));
    RET(0, 3);
}
static void shim_listen(void) {
    int fd = sock_fd(ARG(0));
    if (fd < 0) RET(wsa_fail(WSAENOTSOCK), 2);
    if (listen(fd, (int)ARG(1) > 0 && (int)ARG(1) < 128 ? (int)ARG(1) : 128) != 0) RET(wsa_errno(), 2);
    RET(0, 2);
}
static void shim_connect(void) {
    uint32_t h = ARG(0); int fd = sock_fd(h);
    if (fd < 0) RET(wsa_fail(WSAENOTSOCK), 3);
    if (!ARG(1) || ARG(2) < 16) RET(wsa_fail(WSAEINVAL), 3);
    if (!addr_allowed(G2H(ARG(1)), ARG(2))) { lan_note("connect BLOCKED", G2H(ARG(1))); RET(wsa_fail(WSAENETUNREACH), 3); }   /* offline */
    lan_note("connect", G2H(ARG(1)));
    struct sockaddr_in sa; memcpy(&sa, G2H(ARG(1)), sizeof sa); sa.sin_family = AF_INET;
    int blocking = sock_blocking(h), r, e;
    if (blocking) TM_BLOCKING_BEGIN();
    r = connect(fd, (struct sockaddr *)&sa, sizeof sa); e = errno;
    if (blocking) TM_BLOCKING_END();
    if (r < 0) lan_err("connect", h, (long)r, e);
    if (r != 0) { errno = e; RET(wsa_errno(), 3); }
    RET(0, 3);
}
static void shim_accept(void) {
    uint32_t h = ARG(0); int fd = sock_fd(h);
    if (fd < 0) RET(wsa_fail(WSAENOTSOCK), 3);
    struct sockaddr_in sa; socklen_t sl = sizeof sa;
    int blocking = sock_blocking(h), r, e;
    if (blocking) TM_BLOCKING_BEGIN();
    r = accept(fd, (struct sockaddr *)&sa, &sl); e = errno;
    if (blocking) TM_BLOCKING_END();
    if (r >= 0 && tm_net_lan()) TM_INFO("net: accept(%X) from %s:%u", h, inet_ntoa(sa.sin_addr), ntohs(sa.sin_port));
    if (r < 0) { errno = e; RET(wsa_errno(), 3); }
    uint32_t n = sock_put(r);
    if (!n) { close(r); RET(wsa_fail(WSAEMFILE), 3); }
    if (ARG(1) && ARG(2) && rd32(ARG(2)) >= 16) { memcpy(G2H(ARG(1)), &sa, 16); wr16(ARG(1), 2); wr32(ARG(2), 16); }
    RET(n, 3);
}
static void shim_send(void) {
    uint32_t h = ARG(0); int fd = sock_fd(h);
    if (fd < 0) RET(wsa_fail(WSAENOTSOCK), 4);
    int blocking = sock_blocking(h), e; ssize_t r;
    if (blocking) TM_BLOCKING_BEGIN();
    r = send(fd, G2H(ARG(1)), ARG(2), MSG_NOSIGNAL | (ARG(3) & 1 ? MSG_OOB : 0)); e = errno;
    if (blocking) TM_BLOCKING_END();
    if (r < 0) lan_err("send", h, (long)r, e);
    NTRACE("net: send(%X, %u) -> %ld%s", h, ARG(2), (long)r, r < 0 ? strerror(e) : "");
    if (net_trace() && r > 0) trace_bytes(">", G2H(ARG(1)), r);
    if (r < 0) { errno = e; RET(wsa_errno(), 4); }
    RET((uint32_t)r, 4);
}
static void shim_sendto(void) {
    uint32_t h = ARG(0); int fd = sock_fd(h);
    if (fd < 0) RET(wsa_fail(WSAENOTSOCK), 6);
    struct sockaddr_in sa; struct sockaddr *to = NULL; socklen_t tl = 0;
    if (ARG(4)) {
        if (!addr_allowed(G2H(ARG(4)), ARG(5))) { lan_note("sendto BLOCKED", G2H(ARG(4))); RET(wsa_fail(WSAENETUNREACH), 6); }   /* offline: no LAN/WAN datagrams */
        lan_note("sendto", G2H(ARG(4)));
        memcpy(&sa, G2H(ARG(4)), sizeof sa); sa.sin_family = AF_INET; to = (struct sockaddr *)&sa; tl = sizeof sa;
    }
    ssize_t r = sendto(fd, G2H(ARG(1)), ARG(2), MSG_NOSIGNAL, to, tl);
    if (r < 0) RET(wsa_errno(), 6);
    if (to) NTRACE("net: sendto(%X, %u) -> %ld", h, ARG(2), (long)r);
    RET((uint32_t)r, 6);
}
static void shim_recv(void) {
    uint32_t h = ARG(0); int fd = sock_fd(h);
    if (fd < 0) RET(wsa_fail(WSAENOTSOCK), 4);
    int blocking = sock_blocking(h), e; ssize_t r;
    if (blocking) TM_BLOCKING_BEGIN();
    r = recv(fd, G2H(ARG(1)), ARG(2), (ARG(3) & 2 ? MSG_PEEK : 0) | (ARG(3) & 1 ? MSG_OOB : 0)); e = errno;
    if (blocking) TM_BLOCKING_END();
    if (r <= 0) lan_err("recv", h, (long)r, e);
    if (r >= 0 || (e != EAGAIN && e != EWOULDBLOCK)) NTRACE("net: recv(%X, %u) -> %ld%s", h, ARG(2), (long)r, r < 0 ? strerror(e) : "");
    if (net_trace() && r > 0 && !(ARG(3) & 2)) trace_bytes("<", G2H(ARG(1)), r);
    if (r < 0) { errno = e; RET(wsa_errno(), 4); }
    RET((uint32_t)r, 4);
}
static void shim_recvfrom(void) {
    uint32_t h = ARG(0); int fd = sock_fd(h);
    if (fd < 0) RET(wsa_fail(WSAENOTSOCK), 6);
    struct sockaddr_in sa; socklen_t sl = sizeof sa; memset(&sa, 0, sizeof sa);
    int blocking = sock_blocking(h), e; ssize_t r;
    if (blocking) TM_BLOCKING_BEGIN();
    r = recvfrom(fd, G2H(ARG(1)), ARG(2), ARG(3) & 2 ? MSG_PEEK : 0, (struct sockaddr *)&sa, &sl); e = errno;
    if (blocking) TM_BLOCKING_END();
    if (r < 0) { errno = e; RET(wsa_errno(), 6); }
    if (net_trace()) {                                   /* datagrams: the first 40, then every 500th */
        static unsigned n;
        if (n < 40 || n % 500 == 0) {
            char ip[INET_ADDRSTRLEN] = "?"; inet_ntop(AF_INET, &sa.sin_addr, ip, sizeof ip);
            TM_INFO("net: recvfrom(%X) #%u %ld bytes from %s:%u", h, n, (long)r, ip, ntohs(sa.sin_port));
        }
        n++;
    }
    if (ARG(4) && ARG(5) && rd32(ARG(5)) >= 16) { memcpy(G2H(ARG(4)), &sa, 16); wr16(ARG(4), 2); wr32(ARG(5), 16); }
    RET((uint32_t)r, 6);
}

/* Winsock fd_set = { u_int fd_count; SOCKET fd_array[64]; } */
static void shim_select(void) {
    uint32_t sets[3] = { ARG(1), ARG(2), ARG(3) }, tv = ARG(4);
    static const short want[3] = { POLLIN, POLLOUT, POLLPRI };
    struct pollfd pf[192]; uint32_t ph[192]; int pset[192]; int n = 0;
    for (int k = 0; k < 3; k++) {
        if (!sets[k]) continue;
        uint32_t c = rd32(sets[k]);
        for (uint32_t i = 0; i < c && i < 64 && n < 192; i++) {
            uint32_t h = rd32(sets[k] + 4 + 4 * i); int fd = sock_fd(h);
            if (fd < 0) RET(wsa_fail(WSAENOTSOCK), 5);
            pf[n].fd = fd; pf[n].events = want[k]; pf[n].revents = 0; ph[n] = h; pset[n] = k; n++;
        }
    }
    int ms = tv ? (int)((int32_t)rd32(tv) * 1000 + (int32_t)rd32(tv + 4) / 1000) : -1;
    int r, e;
    if (ms != 0) TM_BLOCKING_BEGIN();
    r = poll(pf, (nfds_t)n, ms); e = errno;
    if (ms != 0) TM_BLOCKING_END();
    if (net_trace() && r > 0)
        for (int i = 0; i < n; i++) if (pf[i].revents) NTRACE("net: select %X set %d revents %x", ph[i], pset[i], pf[i].revents);
    if (r < 0) { errno = e; RET(wsa_errno(), 5); }
    uint32_t cnt[3] = { 0, 0, 0 }, total = 0;
    for (int i = 0; i < n; i++) {
        int k = pset[i];
        short hit = k == 0 ? (pf[i].revents & (POLLIN | POLLHUP | POLLERR))
                  : k == 1 ? ((pf[i].revents & (POLLERR | POLLHUP)) ? 0 : (pf[i].revents & POLLOUT))
                           : (pf[i].revents & (POLLPRI | POLLERR));      /* failed connect is reported in exceptfds */
        if (hit) { wr32(sets[k] + 4 + 4 * cnt[k], ph[i]); cnt[k]++; total++; }
    }
    for (int k = 0; k < 3; k++) if (sets[k]) wr32(sets[k], cnt[k]);
    RET(total, 5);
}
static void shim___WSAFDIsSet(void) {
    uint32_t h = ARG(0), set = ARG(1);
    if (set) { uint32_t c = rd32(set); for (uint32_t i = 0; i < c && i < 64; i++) if (rd32(set + 4 + 4 * i) == h) RET(1, 2); }
    RET(0, 2);
}
static void shim_ioctlsocket(void) {
    int i = sock_idx(ARG(0)); uint32_t cmd = ARG(1), argp = ARG(2);
    NTRACE("net: ioctlsocket(%X, %X)", ARG(0), cmd);
    if (i < 0) RET(wsa_fail(WSAENOTSOCK), 3);
    if (cmd == 0x8004667Eu) {                                   /* FIONBIO */
        int on = argp && rd32(argp);
        int fl = fcntl(g_sock[i].fd, F_GETFL, 0);
        fcntl(g_sock[i].fd, F_SETFL, on ? fl | O_NONBLOCK : fl & ~O_NONBLOCK);
        g_sock[i].nonblock = on;
        RET(0, 3);
    }
    if (cmd == 0x4004667Fu) {                                   /* FIONREAD */
        int avail = 0; ioctl(g_sock[i].fd, FIONREAD, &avail);
        if (argp) wr32(argp, (uint32_t)avail);
        RET(0, 3);
    }
    RET(wsa_fail(WSAEOPNOTSUPP), 3);
}
/* Winsock option -> host option. Returns 0 if the option is unknown here. */
static int map_sockopt(uint32_t level, uint32_t opt, int *hl, int *ho) {
    if (level == 0xFFFF) {
        *hl = SOL_SOCKET;
        switch (opt) {
        case 0x0004: *ho = SO_REUSEADDR; return 1;   case 0x0008: *ho = SO_KEEPALIVE; return 1;
        case 0x0020: *ho = SO_BROADCAST; return 1;   case 0x1001: *ho = SO_SNDBUF; return 1;
        case 0x1002: *ho = SO_RCVBUF; return 1;      case 0x1007: *ho = SO_ERROR; return 1;
        case 0x1008: *ho = SO_TYPE; return 1;
        }
    } else if (level == 6 && opt == 1) { *hl = IPPROTO_TCP; *ho = TCP_NODELAY; return 1; }
    return 0;
}
static void shim_setsockopt(void) {
    int fd = sock_fd(ARG(0)); int hl, ho;
    if (fd < 0) RET(wsa_fail(WSAENOTSOCK), 5);
    if (ARG(3) && ARG(4) >= 4 && map_sockopt(ARG(1), ARG(2), &hl, &ho)) {
        int v = (int)rd32(ARG(3));
        setsockopt(fd, hl, ho, &v, sizeof v);
    }
    RET(0, 5);                                                   /* other options (linger, timeouts, QoS) are accepted and ignored */
}
static void shim_getsockopt(void) {
    int fd = sock_fd(ARG(0)); int hl, ho;
    NTRACE("net: getsockopt(%X, %X, %X)", ARG(0), ARG(1), ARG(2));
    if (fd < 0) RET(wsa_fail(WSAENOTSOCK), 5);
    int v = 0; socklen_t sl = sizeof v;
    if (map_sockopt(ARG(1), ARG(2), &hl, &ho)) {
        getsockopt(fd, hl, ho, &v, &sl);
        if (ho == SO_ERROR && hl == SOL_SOCKET && v) v = wsa_from_errno(v);
        if (ho == SO_TYPE && hl == SOL_SOCKET) v = v == SOCK_DGRAM ? 2 : 1;
    }
    if (ARG(3) && ARG(4) && rd32(ARG(4)) >= 4) { wr32(ARG(3), (uint32_t)v); wr32(ARG(4), 4); }
    RET(0, 5);
}
static void name_common(int peer) {
    int fd = sock_fd(ARG(0));
    if (fd < 0) RET(wsa_fail(WSAENOTSOCK), 3);
    struct sockaddr_in sa; socklen_t sl = sizeof sa; memset(&sa, 0, sizeof sa);
    if ((peer ? getpeername(fd, (struct sockaddr *)&sa, &sl) : getsockname(fd, (struct sockaddr *)&sa, &sl)) != 0) RET(wsa_errno(), 3);
    if (!ARG(1) || !ARG(2) || rd32(ARG(2)) < 16) RET(wsa_fail(WSAEINVAL), 3);
    memcpy(G2H(ARG(1)), &sa, 16); wr16(ARG(1), 2); wr32(ARG(2), 16);
    RET(0, 3);
}
static void shim_getsockname(void) { name_common(0); }
static void shim_getpeername(void) { name_common(1); }
static void shim_shutdown(void) {
    int fd = sock_fd(ARG(0));
    NTRACE("net: shutdown(%X, %u)", ARG(0), ARG(1));
    if (fd < 0) RET(wsa_fail(WSAENOTSOCK), 2);
    shutdown(fd, (int)ARG(1) == 0 ? SHUT_RD : (int)ARG(1) == 1 ? SHUT_WR : SHUT_RDWR);
    RET(0, 2);
}
static void shim_htons(void) { uint16_t v = (uint16_t)ARG(0); RET(((v & 0xFF) << 8) | (v >> 8), 1); }
static void shim_ntohs(void) { uint16_t v = (uint16_t)ARG(0); RET(((v & 0xFF) << 8) | (v >> 8), 1); }
static void shim_htonl(void) { uint32_t v = ARG(0); RET(__builtin_bswap32(v), 1); }
static void shim_inet_addr(void) {
    struct in_addr a;
    if (!ARG(0) || !inet_aton((const char *)G2H(ARG(0)), &a)) RET(0xFFFFFFFFu, 1);   /* INADDR_NONE */
    RET(a.s_addr, 1);
}
static void shim_inet_ntoa(void) {
    static __thread uint32_t buf;                              /* per-thread result buffer, like Winsock */
    if (!buf) buf = tm_galloc(16);
    uint32_t a = ARG(0);
    snprintf((char *)G2H(buf), 16, "%u.%u.%u.%u", a & 0xFF, (a >> 8) & 0xFF, (a >> 16) & 0xFF, a >> 24);
    RET(buf, 1);
}
#define LOCAL_HOSTNAME "localhost"
/* On the LAN every device needs its own name: the engine uses it as the player's login,
   and a server refuses a second player with the same login. From the player name
   (TM_PLAYER_NAME), else from the device's address. */
static const char *lan_hostname(void) {
    static char name[40];
    if (!name[0]) {
        const char *p = getenv("TM_PLAYER_NAME");
        size_t k = 0;
        for (; p && *p && k < 24; p++)
            if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_') name[k++] = *p;
        name[k] = 0;
        uint32_t a = 0;
        if (tm_net_local_ip(&a, NULL, NULL, 0)) { a = ntohl(a); snprintf(name + k, sizeof name - k, "%s%u-%u", k ? "-" : "tmturbo-", (a >> 8) & 255, a & 255); }
        else if (!k) snprintf(name, sizeof name, "tmturbo");
    }
    return name;
}
#define LAN_HOSTNAME lan_hostname()
static void shim_gethostname(void) {
    const char *n = tm_net_lan() ? LAN_HOSTNAME : LOCAL_HOSTNAME;
    if (!ARG(0) || ARG(1) < strlen(n) + 1) RET(wsa_fail(WSAEINVAL), 2);
    memcpy(G2H(ARG(0)), n, strlen(n) + 1);
    RET(0, 2);
}
/* The only names that resolve: the local host (on the LAN: its Wi-Fi address) and IPv4
   literals of reachable addresses. */
static int resolve_local(const char *name, uint32_t *addr_be) {
    struct in_addr a;
    if (tm_net_lan()) TM_INFO("net: resolve '%s'", name ? name : "(null)");
    if (!name || !*name || !strcasecmp(name, LOCAL_HOSTNAME)) { *addr_be = htonl(INADDR_LOOPBACK); return 1; }
    if (tm_net_lan() && !strcasecmp(name, LAN_HOSTNAME)) {
        if (!tm_net_local_ip(addr_be, NULL, NULL, 0)) *addr_be = htonl(INADDR_LOOPBACK);
        return 1;
    }
    if (inet_aton(name, &a) && ip_allowed(a.s_addr)) { *addr_be = a.s_addr; return 1; }
    if (tm_net_lan() && inet_aton(name, &a)) TM_INFO("net: address %s is outside the local network", name);
    if (tm_net_lan()) TM_INFO("net: name '%s' does not resolve (local network only)", name);
    return 0;
}
static void shim_gethostbyname(void) {
    /* hostent { char *h_name; char **h_aliases; short h_addrtype, h_length; char **h_addr_list; } */
    static __thread uint32_t he;
    uint32_t addr;
    if (!resolve_local(ARG(0) ? (const char *)G2H(ARG(0)) : NULL, &addr)) { wsa_fail(WSAHOST_NOT_FOUND); RET(0, 1); }
    if (!he) he = tm_galloc(64);
    snprintf((char *)G2H(he + 32), 32, "%s", ARG(0) && *(const char *)G2H(ARG(0)) ? (const char *)G2H(ARG(0)) : LOCAL_HOSTNAME);   /* name */
    wr32(he + 16, 0);                                               /* aliases = {0}   */
    wr32(he + 20, he + 28); wr32(he + 24, 0);                       /* addr_list       */
    wr32(he + 28, addr);                                            /* the address     */
    wr32(he + 0, he + 32); wr32(he + 4, he + 16); wr16(he + 8, 2); wr16(he + 10, 4); wr32(he + 12, he + 20);
    RET(he, 1);
}
static void shim_getaddrinfo(void) {
    /* addrinfo { int flags, family, socktype, protocol; size_t addrlen; char *canonname; sockaddr *addr; addrinfo *next; } */
    uint32_t node = ARG(0), service = ARG(1), hints = ARG(2), res = ARG(3), addr;
    int passive = hints && (rd32(hints) & 1);
    if (res) wr32(res, 0);
    if (!node && passive) addr = 0;                                 /* wildcard; bind() places it on loopback */
    else if (!resolve_local(node ? (const char *)G2H(node) : NULL, &addr)) { wsa_fail(WSAHOST_NOT_FOUND); RET(WSAHOST_NOT_FOUND, 4); }
    uint32_t ai = tm_galloc(32 + 16);
    wr32(ai + 0, hints ? rd32(hints) : 0); wr32(ai + 4, 2);
    wr32(ai + 8, hints && rd32(hints + 8) ? rd32(hints + 8) : 1); wr32(ai + 12, hints ? rd32(hints + 12) : 0);
    wr32(ai + 16, 16); wr32(ai + 20, 0); wr32(ai + 24, ai + 32); wr32(ai + 28, 0);
    wr16(ai + 32, 2); wr16(ai + 34, htons((uint16_t)(service ? atoi((const char *)G2H(service)) : 0))); wr32(ai + 36, addr);
    if (res) wr32(res, ai);
    RET(0, 4);
}
static void shim_freeaddrinfo(void) { if (ARG(0)) tm_gfree(ARG(0)); RET(0, 1); }
static void shim_WSAIoctl(void) { RET(wsa_fail(WSAEOPNOTSUPP), 9); }
static void shim_WSACancelBlockingCall(void) { RET(0, 0); }

static const tm_shim_def net_shims[] = {
#define WI(n) TM_SHIM("wininet.dll", n)
    WI(InternetOpenA), WI(InternetConnectA), WI(InternetConnectW), WI(HttpOpenRequestA), WI(HttpSendRequestA),
    WI(HttpAddRequestHeadersA), WI(HttpQueryInfoA), WI(InternetReadFile), WI(InternetCloseHandle),
    WI(InternetQueryOptionA), WI(InternetSetOptionA), WI(InternetAttemptConnect), WI(InternetCheckConnectionA),
    WI(InternetSetStatusCallback),
#undef WI
#define WS(n) TM_SHIM("ws2_32.dll", n)
    WS(WSAStartup), WS(WSACleanup), WS(WSAGetLastError), WS(WSASetLastError), WS(socket), WS(closesocket),
    WS(connect), WS(bind), WS(listen), WS(accept), WS(send), WS(sendto), WS(recv), WS(recvfrom), WS(select),
    WS(ioctlsocket), WS(setsockopt), WS(getsockopt), WS(getsockname), WS(getpeername), WS(shutdown), WS(htons),
    WS(ntohs), WS(htonl), WS(inet_addr), WS(inet_ntoa), WS(gethostname), WS(gethostbyname), WS(getaddrinfo),
    WS(freeaddrinfo), WS(WSAIoctl), WS(WSACancelBlockingCall), WS(__WSAFDIsSet),
#undef WS
    /* the game imports Winsock by ordinal; register those names too */
    { "ws2_32.dll", "#1", shim_accept },
    { "ws2_32.dll", "#2", shim_bind },
    { "ws2_32.dll", "#3", shim_closesocket },
    { "ws2_32.dll", "#4", shim_connect },
    { "ws2_32.dll", "#5", shim_getpeername },
    { "ws2_32.dll", "#6", shim_getsockname },
    { "ws2_32.dll", "#7", shim_getsockopt },
    { "ws2_32.dll", "#8", shim_htonl },
    { "ws2_32.dll", "#9", shim_htons },
    { "ws2_32.dll", "#10", shim_ioctlsocket },
    { "ws2_32.dll", "#11", shim_inet_addr },
    { "ws2_32.dll", "#12", shim_inet_ntoa },
    { "ws2_32.dll", "#13", shim_listen },
    { "ws2_32.dll", "#15", shim_ntohs },
    { "ws2_32.dll", "#16", shim_recv },
    { "ws2_32.dll", "#17", shim_recvfrom },
    { "ws2_32.dll", "#18", shim_select },
    { "ws2_32.dll", "#19", shim_send },
    { "ws2_32.dll", "#20", shim_sendto },
    { "ws2_32.dll", "#21", shim_setsockopt },
    { "ws2_32.dll", "#22", shim_shutdown },
    { "ws2_32.dll", "#23", shim_socket },
    { "ws2_32.dll", "#52", shim_gethostbyname },
    { "ws2_32.dll", "#57", shim_gethostname },
    { "ws2_32.dll", "#111", shim_WSAGetLastError },
    { "ws2_32.dll", "#112", shim_WSASetLastError },
    { "ws2_32.dll", "#113", shim_WSACancelBlockingCall },
    { "ws2_32.dll", "#115", shim_WSAStartup },
    { "ws2_32.dll", "#116", shim_WSACleanup },
    { "ws2_32.dll", "#151", shim___WSAFDIsSet },
};
void tm_net_register(void) { tm_register_shims(net_shims, sizeof net_shims / sizeof net_shims[0]); }
