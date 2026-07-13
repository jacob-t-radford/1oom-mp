/* 1oom-mp: portable TCP transport with length-prefixed framing. POSIX sockets + Winsock. */
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET sockfd_t;
  typedef int net_ssize_t;
  #define NET_BADSOCK     INVALID_SOCKET
  #define net_closesock   closesocket
  #define NET_EWOULDBLOCK WSAEWOULDBLOCK
  #define NET_EAGAIN      WSAEWOULDBLOCK   /* Winsock has no EAGAIN; EWOULDBLOCK covers it */
  #define NET_EINTR       WSAEINTR
  #define NET_LASTERR()   WSAGetLastError()
#else
  #include <errno.h>
  #include <signal.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/select.h>
  #include <sys/time.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  typedef int sockfd_t;
  typedef ssize_t net_ssize_t;
  #define NET_BADSOCK     (-1)
  #define net_closesock   close
  #define NET_EWOULDBLOCK EWOULDBLOCK
  #define NET_EAGAIN      EAGAIN
  #define NET_EINTR       EINTR
  #define NET_LASTERR()   errno
#endif

/* Use 1oom's log if building inside the engine, else fall back to stderr so the
   standalone transport test can compile without the rest of the tree. */
#ifdef INC_1OOM_LOG_H
#include "log.h"
#define NET_ERR(...)  log_error(__VA_ARGS__)
#define NET_MSG(...)  log_message(__VA_ARGS__)
#else
#define NET_ERR(...)  fprintf(stderr, "net: " __VA_ARGS__)
#define NET_MSG(...)  fprintf(stdout, "net: " __VA_ARGS__)
#endif

#define NET_HDR_LEN 4 /* u32 length prefix */

/* socket error -> short string (errno on POSIX; Winsock keeps its error elsewhere) */
static const char *net_errstr(void) {
#ifdef _WIN32
    static char b[40];
    snprintf(b, sizeof(b), "winsock error %d", WSAGetLastError());
    return b;
#else
    return strerror(errno);
#endif
}

struct net_listener_s {
    sockfd_t fd;
};

struct net_conn_s {
    sockfd_t fd;
    bool open;
    char addr[48];
    /* inbound frame-assembly buffer */
    uint8_t *rbuf;
    uint32_t rcap;   /* allocated capacity */
    uint32_t rlen;   /* bytes currently buffered */
    uint32_t need;   /* total bytes for the in-progress frame (hdr+payload), 0 = unknown */
};

/* -------------------------------------------------------------------------- */

#ifdef _WIN32
static bool s_wsa_inited = false;
#endif

/* Winsock must be started before any socket call; lazy + idempotent so it works
   whether or not the caller invokes net_init(). No-op on POSIX. */
static void net_wsa_ensure(void) {
#ifdef _WIN32
    if (!s_wsa_inited) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) { s_wsa_inited = true; }
    }
#else
    /* A peer that vanishes mid-send() raises SIGPIPE, whose default action kills the
       process -- the host exited 141 (128+SIGPIPE) when a client window closed. Ignore
       it so send() returns EPIPE instead and the normal disconnect path runs. Idempotent. */
    signal(SIGPIPE, SIG_IGN);
#endif
}

int net_init(void) { net_wsa_ensure(); return 0; }

void net_shutdown(void) {
#ifdef _WIN32
    if (s_wsa_inited) { WSACleanup(); s_wsa_inited = false; }
#endif
}

static void set_nonblocking(sockfd_t fd) {
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) {
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
#endif
}

static void set_nodelay(sockfd_t fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
}

static net_conn_t *conn_new(sockfd_t fd, const struct sockaddr_in *sa) {
    net_conn_t *c = (net_conn_t *)calloc(1, sizeof(*c));
    if (!c) { return NULL; }
    c->fd = fd;
    c->open = true;
    c->rcap = 8192;
    c->rbuf = (uint8_t *)malloc(c->rcap);
    if (!c->rbuf) { free(c); return NULL; }
    if (sa) {
        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, (void *)&sa->sin_addr, ip, sizeof(ip));
        snprintf(c->addr, sizeof(c->addr), "%s:%u", ip, (unsigned)ntohs(sa->sin_port));
    } else {
        snprintf(c->addr, sizeof(c->addr), "?");
    }
    set_nonblocking(fd);
    set_nodelay(fd);
    return c;
}

/* -------------------------------------------------------------------------- */

net_listener_t *net_listen(uint16_t port) {
    net_wsa_ensure();
    sockfd_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == NET_BADSOCK) { NET_ERR("socket: %s\n", net_errstr()); return NULL; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        NET_ERR("bind(%u): %s\n", (unsigned)port, net_errstr());
        net_closesock(fd);
        return NULL;
    }
    if (listen(fd, 8) < 0) {
        NET_ERR("listen: %s\n", net_errstr());
        net_closesock(fd);
        return NULL;
    }
    set_nonblocking(fd);
    net_listener_t *l = (net_listener_t *)calloc(1, sizeof(*l));
    if (!l) { net_closesock(fd); return NULL; }
    l->fd = fd;
    NET_MSG("listening on port %u\n", (unsigned)port);
    return l;
}

net_conn_t *net_accept(net_listener_t *l) {
    if (!l) { return NULL; }
    struct sockaddr_in sa;
    socklen_t slen = sizeof(sa);
    sockfd_t fd = accept(l->fd, (struct sockaddr *)&sa, &slen);
    if (fd == NET_BADSOCK) {
        return NULL; /* would-block when nothing pending */
    }
    net_conn_t *c = conn_new(fd, &sa);
    if (!c) { net_closesock(fd); return NULL; }
    NET_MSG("accepted %s\n", c->addr);
    return c;
}

void net_listener_close(net_listener_t *l) {
    if (l) { net_closesock(l->fd); free(l); }
}

net_conn_t *net_connect(const char *host, uint16_t port) {
    net_wsa_ensure();
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%u", (unsigned)port);
    struct addrinfo hints, *res = NULL, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0) {
        NET_ERR("resolve %s: %s\n", host, net_errstr());
        return NULL;
    }
    sockfd_t fd = NET_BADSOCK;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == NET_BADSOCK) { continue; }
        if (connect(fd, ai->ai_addr, (socklen_t)ai->ai_addrlen) == 0) { break; }
        net_closesock(fd);
        fd = NET_BADSOCK;
    }
    net_conn_t *c = NULL;
    if (fd != NET_BADSOCK) {
        c = conn_new(fd, (const struct sockaddr_in *)res->ai_addr);
        if (c) { NET_MSG("connected to %s\n", c->addr); }
        else { net_closesock(fd); }
    } else {
        NET_ERR("connect %s:%u failed\n", host, (unsigned)port);
    }
    freeaddrinfo(res);
    return c;
}

/* -------------------------------------------------------------------------- */

int net_send(net_conn_t *c, const void *data, uint32_t len) {
    if (!c || !c->open) { return -1; }
    if (len > NET_MSG_MAX) { NET_ERR("send too big %u\n", len); return -1; }
    uint8_t hdr[NET_HDR_LEN];
    uint32_t nlen = htonl(len);
    memcpy(hdr, &nlen, NET_HDR_LEN);
    /* send header then payload, looping over partial / would-block writes */
    const uint8_t *parts[2] = { hdr, (const uint8_t *)data };
    uint32_t sizes[2] = { NET_HDR_LEN, len };
    for (int p = 0; p < 2; ++p) {
        uint32_t off = 0;
        while (off < sizes[p]) {
            net_ssize_t n = send(c->fd, (const char *)(parts[p] + off), (int)(sizes[p] - off), 0);
            if (n > 0) {
                off += (uint32_t)n;
            } else {
                int e = NET_LASTERR();
                if ((n < 0) && (e == NET_EAGAIN || e == NET_EWOULDBLOCK || e == NET_EINTR)) {
                    /* socket buffer full: wait briefly for writability */
                    fd_set wf; FD_ZERO(&wf); FD_SET(c->fd, &wf);
                    struct timeval tv = { 5, 0 };
                    if (select((int)(c->fd + 1), NULL, &wf, NULL, &tv) <= 0) {
                        NET_ERR("send timeout/err to %s\n", c->addr);
                        c->open = false;
                        return -1;
                    }
                } else {
                    NET_ERR("send to %s: %s\n", c->addr, net_errstr());
                    c->open = false;
                    return -1;
                }
            }
        }
    }
    return 0;
}

/* like net_send, but for fire-and-forget streams (spectate): if the kernel send buffer can't hold
   the whole framed message right now, DROP it instead of blocking. Keeps a slow receiver from
   pacing the sender; the receiver resyncs on the next message. Returns 0 (sent or dropped), <0 err.
   (The would-block detection is macOS-only via SO_NWRITE; elsewhere it just sends.) */
int net_send_besteffort(net_conn_t *c, const void *data, uint32_t len) {
    if (!c || !c->open) { return -1; }
    if (len > NET_MSG_MAX) { return -1; }
#if defined(SO_NWRITE)
    {
        int unsent = 0, sndbuf = 0;
        socklen_t sl1 = sizeof(int), sl2 = sizeof(int);
        if ((getsockopt(c->fd, SOL_SOCKET, SO_NWRITE, &unsent, &sl1) == 0)
         && (getsockopt(c->fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, &sl2) == 0)
         && (sndbuf > 0) && ((sndbuf - unsent) < (int)(len + NET_HDR_LEN))) {
            return 0; /* would block -> drop this frame */
        }
    }
#endif
    return net_send(c, data, len);
}

/* pull available bytes from the socket into the assembly buffer (non-blocking) */
static int rbuf_fill(net_conn_t *c) {
    uint8_t tmp[16384];
    int got_any = 0;
    for (;;) {
        net_ssize_t n = recv(c->fd, (char *)tmp, (int)sizeof(tmp), 0);
        if (n > 0) {
            if (c->rlen + (uint32_t)n > c->rcap) {
                uint32_t ncap = c->rcap;
                while (ncap < c->rlen + (uint32_t)n) { ncap *= 2; }
                if (ncap > NET_MSG_MAX + NET_HDR_LEN + 16) {
                    NET_ERR("recv frame too big from %s\n", c->addr);
                    c->open = false;
                    return -1;
                }
                uint8_t *nb = (uint8_t *)realloc(c->rbuf, ncap);
                if (!nb) { c->open = false; return -1; }
                c->rbuf = nb; c->rcap = ncap;
            }
            memcpy(c->rbuf + c->rlen, tmp, (size_t)n);
            c->rlen += (uint32_t)n;
            got_any = 1;
            /* keep draining */
        } else if (n == 0) {
            c->open = false; /* peer closed */
            return got_any ? 1 : -1;
        } else {
            int e = NET_LASTERR();
            if (e == NET_EAGAIN || e == NET_EWOULDBLOCK) { return got_any; }
            if (e == NET_EINTR) { continue; }
            NET_ERR("recv from %s: %s\n", c->addr, net_errstr());
            c->open = false;
            return -1;
        }
    }
}

int net_recv(net_conn_t *c, void *buf, uint32_t bufsize, uint32_t *out_len) {
    if (!c) { return -1; }
    int fr = rbuf_fill(c);
    if (fr < 0 && c->rlen < NET_HDR_LEN) { return -1; }
    /* do we have a full header? */
    if (c->rlen < NET_HDR_LEN) { return c->open ? 0 : -1; }
    uint32_t nlen;
    memcpy(&nlen, c->rbuf, NET_HDR_LEN);
    uint32_t plen = ntohl(nlen);
    if (plen > NET_MSG_MAX) { NET_ERR("bad frame len %u from %s\n", plen, c->addr); c->open = false; return -1; }
    if (c->rlen < NET_HDR_LEN + plen) { return c->open ? 0 : -1; } /* payload incomplete */
    if (plen > bufsize) { NET_ERR("recv buf too small (%u < %u)\n", bufsize, plen); c->open = false; return -1; }
    memcpy(buf, c->rbuf + NET_HDR_LEN, plen);
    *out_len = plen;
    /* shift any trailing bytes (start of next frame) to the front */
    uint32_t consumed = NET_HDR_LEN + plen;
    uint32_t rest = c->rlen - consumed;
    if (rest) { memmove(c->rbuf, c->rbuf + consumed, rest); }
    c->rlen = rest;
    return 1;
}

void net_conn_close(net_conn_t *c) {
    if (c) {
        if (c->fd != NET_BADSOCK) { net_closesock(c->fd); }
        free(c->rbuf);
        free(c);
    }
}

bool net_conn_is_open(const net_conn_t *c) { return c && c->open; }
const char *net_conn_addr(const net_conn_t *c) { return c ? c->addr : "?"; }

/* -------------------------------------------------------------------------- */
/* 1oom-mp: enumerate this machine's IPv4 addresses for the "Host Game" screen, 100.x (Tailscale/
   mesh-VPN) first since that's the one to give a remote player. */

#ifndef _WIN32
#include <ifaddrs.h>
#endif

static bool net_addr_skip(const char *s) {
    return (strncmp(s, "127.", 4) == 0) || (strncmp(s, "169.254.", 8) == 0) || (s[0] == '\0');
}

int net_get_local_ipv4(char addrs[][16], int max)
{
    int n = 0;
    net_wsa_ensure();
#ifdef _WIN32
    {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s != INVALID_SOCKET) {
            INTERFACE_INFO tbl[16];
            DWORD bytes = 0;
            if (WSAIoctl(s, SIO_GET_INTERFACE_LIST, NULL, 0, tbl, sizeof(tbl), &bytes, NULL, NULL) == 0) {
                int cnt = (int)(bytes / sizeof(INTERFACE_INFO));
                for (int i = 0; (i < cnt) && (n < max); ++i) {
                    struct sockaddr_in *sa = (struct sockaddr_in *)&tbl[i].iiAddress;
                    const char *ip = inet_ntoa(sa->sin_addr);
                    if (ip && !net_addr_skip(ip)) {
                        strncpy(addrs[n], ip, 15); addrs[n][15] = '\0'; ++n;
                    }
                }
            }
            closesocket(s);
        }
    }
#else
    {
        struct ifaddrs *ifa0 = NULL;
        if (getifaddrs(&ifa0) == 0) {
            for (struct ifaddrs *ifa = ifa0; ifa && (n < max); ifa = ifa->ifa_next) {
                if (ifa->ifa_addr && (ifa->ifa_addr->sa_family == AF_INET)) {
                    char ip[16];
                    struct sockaddr_in *sa = (struct sockaddr_in *)ifa->ifa_addr;
                    if (inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip)) && !net_addr_skip(ip)) {
                        strncpy(addrs[n], ip, 15); addrs[n][15] = '\0'; ++n;
                    }
                }
            }
            freeifaddrs(ifa0);
        }
    }
#endif
    /* 100.x first: that's the mesh-VPN address remote players connect to */
    for (int i = 0, front = 0; i < n; ++i) {
        if (strncmp(addrs[i], "100.", 4) == 0) {
            char tmp[16];
            memcpy(tmp, addrs[i], 16);
            memmove(addrs[front + 1], addrs[front], (size_t)(i - front) * 16);
            memcpy(addrs[front], tmp, 16);
            ++front;
        }
    }
    return n;
}

/* 1oom-mp: quick local-port probe -- a successful connect means a server (probably a survivor from a
   crashed session) is already there. Localhost connects resolve immediately either way. */
bool net_probe_local_port(uint16_t port)
{
    struct sockaddr_in sa;
    bool ok = false;
#ifdef _WIN32
    SOCKET s;
#else
    int s;
#endif
    net_wsa_ensure();
    s = socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
    if (s == INVALID_SOCKET) { return false; }
#else
    if (s < 0) { return false; }
#endif
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(0x7f000001u); /* 127.0.0.1 */
    ok = (connect(s, (struct sockaddr *)&sa, sizeof(sa)) == 0);
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
    return ok;
}
