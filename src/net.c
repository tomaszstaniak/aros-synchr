#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __AROS__
#include <proto/bsdsocket.h>
#include <proto/dos.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
/* bsdsocket.library keeps its own error number per opener; the C
 * library's errno is not updated by these calls. */
#define SOCK_ERRNO() ((int)Errno())
#define SOCK_CLOSE(s) CloseSocket(s)
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#define SOCK_ERRNO() errno
#define SOCK_CLOSE(s) close(s)
#endif

/* Strict POSIX feature macros hide it on some hosts (macOS). */
#ifndef INADDR_NONE
#define INADDR_NONE 0xffffffffU
#endif
#if defined(__APPLE__) && !defined(SO_REUSEPORT)
#define SO_REUSEPORT 0x0200 /* likewise hidden; the value of <sys/socket.h> */
#endif

net_ms net_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (net_ms)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int net_init(void)
{
    /* On AROS the SDK's startup code opens bsdsocket.library (SocketBase)
     * before main() and refuses to start without it. */
    return 0;
}

static int set_nonblocking(int s)
{
#ifdef __AROS__
    long on = 1;
    return IoctlSocket(s, FIONBIO, (char *)&on);
#else
    int flags = fcntl(s, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}

/* Formatted here: inet_ntoa() is a library call on AROS and keeps its
 * answer in a shared buffer everywhere. */
static void ipv4_text(unsigned long a, char out[NET_IPV4_TEXT])
{
    snprintf(out, NET_IPV4_TEXT, "%lu.%lu.%lu.%lu", (a >> 24) & 255, (a >> 16) & 255, (a >> 8) & 255, a & 255);
}

static int would_block(int e)
{
    return e == EWOULDBLOCK || e == EAGAIN || e == EINTR;
}

static int (*interrupt_fn)(void);

void net_set_interrupt(int (*fn)(void))
{
    interrupt_fn = fn;
}

enum net_status net_wait_rw(int sock, int rd, int wr, net_ms deadline, int *err)
{
    for (;;) {
        if (interrupt_fn && interrupt_fn())
            return NET_INTERRUPTED;
        net_ms left = deadline - net_now();
        if (left < 0)
            left = 0;
        if (interrupt_fn && left > 200)
            left = 200;
        fd_set rset, wset;
        FD_ZERO(&rset);
        FD_ZERO(&wset);
        if (rd)
            FD_SET(sock, &rset);
        if (wr)
            FD_SET(sock, &wset);
        struct timeval tv;
        tv.tv_sec = (long)(left / 1000);
        tv.tv_usec = (long)(left % 1000) * 1000;
#ifdef __AROS__
        int r = WaitSelect(sock + 1, rd ? &rset : NULL, wr ? &wset : NULL, NULL, &tv, NULL);
#else
        int r = select(sock + 1, rd ? &rset : NULL, wr ? &wset : NULL, NULL, &tv);
#endif
        if (r > 0)
            return NET_OK;
        if (r < 0) {
            int e = SOCK_ERRNO();
            if (e != EINTR) {
                *err = e;
                return NET_ERROR;
            }
        }
        if (net_now() >= deadline)
            return NET_TIMEOUT; /* otherwise: select may wake early, or a piece ended */
    }
}

enum net_status net_wait(int sock, int for_write, net_ms deadline, int *err)
{
    return net_wait_rw(sock, !for_write, for_write, deadline, err);
}

enum net_status net_connect_start(const char *ipv4, unsigned port, int *sock, int *err)
{
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    /* inet_addr() rather than inet_pton(): on ABIv11 inet_pton is a
     * miami.library call, and a numeric dotted quad is all we accept. */
    sa.sin_addr.s_addr = inet_addr((char *)ipv4);
    if (port == 0 || port > 65535 || sa.sin_addr.s_addr == INADDR_NONE) {
        *err = EINVAL;
        return NET_ERROR;
    }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        *err = SOCK_ERRNO();
        return NET_ERROR;
    }
#if !defined(__AROS__) && defined(SO_NOSIGPIPE)
    /* A write to a connection the peer has closed must return EPIPE, not
     * kill the process with SIGPIPE (macOS; Linux uses MSG_NOSIGNAL). */
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
#ifdef NET_TEST_HOOKS
    /* A small send buffer, so that the socket takes less than one TLS
     * record at a time, as a slow link does (host tests). */
    if (getenv("STPOC_TEST_SNDBUF")) {
        int sz = atoi(getenv("STPOC_TEST_SNDBUF"));
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
    }
#endif
    if (set_nonblocking(s) < 0) {
        *err = SOCK_ERRNO();
        SOCK_CLOSE(s);
        return NET_ERROR;
    }
    *sock = s;
    if (connect(s, (struct sockaddr *)&sa, sizeof sa) < 0) {
        int e = SOCK_ERRNO();
        if (e != EINPROGRESS && !would_block(e)) {
            *err = e;
            SOCK_CLOSE(s);
            *sock = -1;
            return NET_ERROR;
        }
        return NET_AGAIN;
    }
    return NET_OK;
}

enum net_status net_connect_finish(int sock, int *err)
{
    int (*fn)(void) = interrupt_fn;
    interrupt_fn = NULL; /* one look, no waiting */
    enum net_status st = net_wait_rw(sock, 0, 1, 0, err);
    interrupt_fn = fn;
    if (st == NET_TIMEOUT)
        return NET_AGAIN;
    if (st != NET_OK)
        return st;
    int soerr = 0;
    socklen_t len = sizeof soerr;
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &soerr, &len) < 0)
        soerr = SOCK_ERRNO();
    if (soerr != 0) {
        *err = soerr;
        return NET_ERROR;
    }
    return NET_OK;
}

enum net_status net_connect(const char *ipv4, unsigned port, net_ms deadline,
                            int *sock, int *err)
{
    int s = -1;
    enum net_status st = net_connect_start(ipv4, port, &s, err);
    if (st == NET_AGAIN) {
        st = net_wait(s, 1, deadline, err);
        if (st == NET_OK)
            st = net_connect_finish(s, err);
        if (st == NET_AGAIN)
            st = NET_TIMEOUT;
        if (st != NET_OK)
            SOCK_CLOSE(s);
    }
    if (st == NET_OK)
        *sock = s;
    return st;
}

static int extra_fd = -1;
static unsigned long extra_signals;

void net_wait_extra(int fd)
{
    extra_fd = fd;
}

void net_wait_signals(unsigned long mask)
{
    extra_signals = mask;
}

enum net_status net_wait_many(net_wait_item *it, int n, net_ms deadline, int *err)
{
    for (;;) {
        net_ms left = deadline - net_now();
        if (left < 0)
            left = 0;
        fd_set rset, wset;
        int max = -1, any_r = 0, any_w = 0;
        FD_ZERO(&rset);
        FD_ZERO(&wset);
        for (int i = 0; i < n; i++) {
            it[i].ready = 0;
            if (it[i].sock < 0 || (!it[i].rd && !it[i].wr))
                continue;
            if (it[i].rd)
                FD_SET(it[i].sock, &rset), any_r = 1;
            if (it[i].wr)
                FD_SET(it[i].sock, &wset), any_w = 1;
            if (it[i].sock > max)
                max = it[i].sock;
        }
#ifdef __AROS__
        if (max < 0 && !extra_signals) {
            /* No socket: a plain pause (a tick is 20 ms). */
            if (left > 0)
                Delay((ULONG)((left + 19) / 20));
            return NET_TIMEOUT;
        }
#endif
        struct timeval tv;
        tv.tv_sec = (long)(left / 1000);
        tv.tv_usec = (long)(left % 1000) * 1000;
#ifdef __AROS__
        ULONG sigs = (ULONG)extra_signals;
        int r = WaitSelect(max + 1, any_r ? &rset : NULL, any_w ? &wset : NULL, NULL, &tv, sigs ? &sigs : NULL);
        if (r == 0 && extra_signals && (sigs & extra_signals))
            return NET_OK; /* a message at the control port */
#else
        if (extra_fd >= 0) {
            FD_SET(extra_fd, &rset);
            any_r = 1;
            if (extra_fd > max)
                max = extra_fd;
        }
        int r = select(max + 1, any_r ? &rset : NULL, any_w ? &wset : NULL, NULL, &tv);
#endif
        if (r > 0) {
            for (int i = 0; i < n; i++)
                if (it[i].sock >= 0
                    && ((it[i].rd && FD_ISSET(it[i].sock, &rset)) || (it[i].wr && FD_ISSET(it[i].sock, &wset))))
                    it[i].ready = 1;
            return NET_OK;
        }
        if (r < 0) {
            int e = SOCK_ERRNO();
            if (e != EINTR) {
                *err = e;
                return NET_ERROR;
            }
            return NET_TIMEOUT; /* a signal: the caller looks at its stop request */
        }
        if (net_now() >= deadline)
            return NET_TIMEOUT;
    }
}

enum net_status net_send(int sock, const void *buf, size_t len, size_t *done, int *err)
{
#ifdef NET_TEST_HOOKS
    /* A socket that takes a little at a time and is full every second
     * time it is asked, as a slow link looks to the caller (host tests). */
    static long piece = -1, calls;
    if (piece < 0)
        piece = getenv("STPOC_TEST_SEND_PIECE") ? atol(getenv("STPOC_TEST_SEND_PIECE")) : 0;
    if (piece > 0) {
        if (calls++ % 2)
            return NET_AGAIN;
        if (len > (size_t)piece)
            len = (size_t)piece;
    }
#endif
#if !defined(__AROS__) && defined(MSG_NOSIGNAL)
    long r = send(sock, (void *)buf, len, MSG_NOSIGNAL);
#else
    long r = send(sock, (void *)buf, len, 0);
#endif
    if (r >= 0) {
        *done = (size_t)r;
        return NET_OK;
    }
    int e = SOCK_ERRNO();
    if (would_block(e))
        return NET_AGAIN;
    *err = e;
    return NET_ERROR;
}

enum net_status net_recv(int sock, void *buf, size_t len, size_t *done, int *err)
{
    long r = recv(sock, buf, len, 0);
    if (r > 0) {
        *done = (size_t)r;
        return NET_OK;
    }
    if (r == 0)
        return NET_CLOSED;
    int e = SOCK_ERRNO();
    if (would_block(e))
        return NET_AGAIN;
    *err = e;
    return NET_ERROR;
}

enum net_status net_udp_listen(unsigned port, int *sock, int *err)
{
    struct sockaddr_in sa;
    if (port == 0 || port > 65535) {
        *err = EINVAL;
        return NET_ERROR;
    }
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        *err = SOCK_ERRNO();
        return NET_ERROR;
    }
    /* Another program on this machine (a Syncthing) may listen on the
     * same port; neither option is essential, so a refusal is ignored
     * and bind() decides. */
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (void *)&one, sizeof one);
#ifdef SO_REUSEPORT
    one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, (void *)&one, sizeof one);
#endif
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&sa, sizeof sa) < 0 || set_nonblocking(s) < 0) {
        *err = SOCK_ERRNO();
        SOCK_CLOSE(s);
        return NET_ERROR;
    }
    *sock = s;
    return NET_OK;
}

enum net_status net_udp_recv(int sock, void *buf, size_t len, size_t *done, char from_ip[NET_IPV4_TEXT], int *err)
{
    struct sockaddr_in sa;
    socklen_t salen = sizeof sa;
    memset(&sa, 0, sizeof sa);
    from_ip[0] = 0;
    long r = recvfrom(sock, buf, len, 0, (struct sockaddr *)&sa, &salen);
    if (r < 0) {
        int e = SOCK_ERRNO();
        if (would_block(e))
            return NET_AGAIN;
        *err = e;
        return NET_ERROR;
    }
    if (salen >= (socklen_t)sizeof sa && sa.sin_family == AF_INET) {
        ipv4_text(ntohl(sa.sin_addr.s_addr), from_ip);
    }
    *done = (size_t)r;
    return NET_OK;
}

enum net_status net_tcp_listen(unsigned port, int *sock, int *err)
{
    struct sockaddr_in sa;
    if (port == 0 || port > 65535) {
        *err = EINVAL;
        return NET_ERROR;
    }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        *err = SOCK_ERRNO();
        return NET_ERROR;
    }
    /* A port left in TIME_WAIT by the previous run is taken again at once.
     * (Not SO_REUSEPORT: a second program must not share the port.) */
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (void *)&one, sizeof one);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(s, 8) < 0 || set_nonblocking(s) < 0) {
        *err = SOCK_ERRNO();
        SOCK_CLOSE(s);
        return NET_ERROR;
    }
    *sock = s;
    return NET_OK;
}

enum net_status net_accept(int lsock, int *sock, char from_ip[NET_IPV4_TEXT], int *err)
{
    struct sockaddr_in sa;
    socklen_t salen = sizeof sa;
    memset(&sa, 0, sizeof sa);
    from_ip[0] = 0;
    int s = accept(lsock, (struct sockaddr *)&sa, &salen);
    if (s < 0) {
        int e = SOCK_ERRNO();
        if (would_block(e) || e == ECONNABORTED)
            return NET_AGAIN;
        *err = e;
        return NET_ERROR;
    }
#if !defined(__AROS__) && defined(SO_NOSIGPIPE)
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    if (set_nonblocking(s) < 0) {
        *err = SOCK_ERRNO();
        SOCK_CLOSE(s);
        return NET_ERROR;
    }
    if (sa.sin_family == AF_INET)
        ipv4_text(ntohl(sa.sin_addr.s_addr), from_ip);
    *sock = s;
    return NET_OK;
}

enum net_status net_udp_broadcast(int sock, unsigned port, const void *buf, size_t len, int *err)
{
    struct sockaddr_in sa;
    int one = 1;
    /* Asked for every time: cheap, and the socket may be one that was
     * opened for listening only. */
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (void *)&one, sizeof one) < 0) {
        *err = SOCK_ERRNO();
        return NET_ERROR;
    }
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr.s_addr = htonl(0xffffffffUL);
    long r = sendto(sock, (void *)buf, len, 0, (struct sockaddr *)&sa, sizeof sa);
    if (r < 0) {
        int e = SOCK_ERRNO();
        if (would_block(e))
            return NET_AGAIN;
        *err = e;
        return NET_ERROR;
    }
    return NET_OK;
}

int net_is_ipv4(const char *text)
{
    unsigned a, b, c, d;
    char tail;
    return sscanf(text, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) == 4 && a < 256 && b < 256 && c < 256 && d < 256;
}

enum net_status net_resolve(const char *host, char ip[NET_IPV4_TEXT], int *err)
{
    if (net_is_ipv4(host)) {
        snprintf(ip, NET_IPV4_TEXT, "%s", host);
        return NET_OK;
    }
#ifdef __AROS__
    /* bsdsocket.library's resolver; getaddrinfo() is not a plain C call
     * on every AROS (a miami.library call on ABIv11). */
    struct hostent *h = gethostbyname((char *)host);
    if (!h || h->h_addrtype != AF_INET || h->h_length != 4 || !h->h_addr_list || !h->h_addr_list[0]) {
        *err = h ? EAFNOSUPPORT : ENOENT;
        return NET_ERROR;
    }
    const unsigned char *p = (const unsigned char *)h->h_addr_list[0];
    ipv4_text(((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) | p[3], ip);
    return NET_OK;
#else
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0 || !res) {
        *err = ENOENT;
        return NET_ERROR;
    }
    const struct sockaddr_in *sa = (const struct sockaddr_in *)res->ai_addr;
    ipv4_text(ntohl(sa->sin_addr.s_addr), ip);
    freeaddrinfo(res);
    return NET_OK;
#endif
}

enum net_status net_peer_ip(int sock, char ip[NET_IPV4_TEXT], int *err)
{
    struct sockaddr_in sa;
    socklen_t salen = sizeof sa;
    memset(&sa, 0, sizeof sa);
    if (getpeername(sock, (struct sockaddr *)&sa, &salen) < 0) {
        *err = SOCK_ERRNO();
        return NET_ERROR;
    }
    if (sa.sin_family != AF_INET) {
        *err = EAFNOSUPPORT;
        return NET_ERROR;
    }
    ipv4_text(ntohl(sa.sin_addr.s_addr), ip);
    return NET_OK;
}

void net_close(int sock)
{
    if (sock >= 0)
        SOCK_CLOSE(sock);
}
