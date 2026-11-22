/* TCP transport with an overall deadline.
 *
 * The socket belongs to this module: net_connect() creates it, net_close()
 * is the only way it is closed. On AROS the descriptor is a bsdsocket.library
 * descriptor, which is not a C library file descriptor, so nothing else
 * (read(), write(), close(), OpenSSL's socket BIO) may be given it. */
#ifndef ST_NET_H
#define ST_NET_H

#include <stddef.h>
#include <stdint.h>

typedef int64_t net_ms;

enum net_status {
    NET_OK = 0,
    NET_AGAIN,     /* would block; wait and retry */
    NET_TIMEOUT,   /* the deadline passed */
    NET_CLOSED,    /* orderly shutdown by the peer */
    NET_ERROR,     /* any other failure; errno-style code in *err */
    NET_INTERRUPTED /* the function set with net_set_interrupt asked to stop waiting */
};

/* Milliseconds from a clock that does not jump with wall-clock changes
 * where the platform has one. Only differences are meaningful. */
net_ms net_now(void);

/* Connects to a numeric IPv4 address. On success *sock is a non-blocking
 * connected socket owned by the caller until net_close(). */
enum net_status net_connect(const char *ipv4, unsigned port, net_ms deadline,
                            int *sock, int *err);

/* The same in two steps, for a caller with its own loop.
 * net_connect_start: NET_OK when already connected, NET_AGAIN while the
 * attempt is under way (*sock is valid in both cases and the caller's to
 * close), NET_ERROR otherwise. net_connect_finish looks once, without
 * waiting: NET_OK connected, NET_AGAIN not yet, NET_ERROR failed (the
 * socket is not closed). */
enum net_status net_connect_start(const char *ipv4, unsigned port, int *sock, int *err);
enum net_status net_connect_finish(int sock, int *err);

/* Several sockets at once: waits until any item is readable (rd) or
 * writable (wr) as asked, and marks those in ready; items with sock < 0
 * or nothing asked are skipped. NET_OK, or NET_TIMEOUT at the deadline
 * (with no usable item this is a plain wait). Not cut short by the
 * function of net_set_interrupt: the caller chooses a short deadline. */
typedef struct {
    int sock;
    int rd, wr;
    int ready;
} net_wait_item;
enum net_status net_wait_many(net_wait_item *it, int n, net_ms deadline, int *err);
/* Something else that ends net_wait_many early (NET_OK), for the control
 * interface: a descriptor that becomes readable (-1: none), or on AROS
 * task signals (0: none). */
void net_wait_extra(int fd);
void net_wait_signals(unsigned long mask);

/* Waits until the socket is readable (for_write 0) or writable (1), or the
 * deadline passes. */
enum net_status net_wait(int sock, int for_write, net_ms deadline, int *err);
/* The same for either direction, whichever comes first (at least one of
 * rd and wr set). A deadline already passed still looks once. */
enum net_status net_wait_rw(int sock, int rd, int wr, net_ms deadline, int *err);

/* While fn is set, every wait in this module is done in pieces of at
 * most 200 ms and ends with NET_INTERRUPTED once fn() returns non-zero
 * (a stop request during a connection attempt). NULL clears it. */
void net_set_interrupt(int (*fn)(void));

/* One non-blocking send/recv. *done receives the byte count on NET_OK. */
enum net_status net_send(int sock, const void *buf, size_t len, size_t *done, int *err);
enum net_status net_recv(int sock, void *buf, size_t len, size_t *done, int *err);

/* A non-blocking UDP socket bound to every local IPv4 address at port,
 * for datagrams sent to that port (broadcasts included). The address may
 * be shared with other programs on the machine (SO_REUSEADDR, and
 * SO_REUSEPORT where the system has it). NET_OK with *sock set (closed
 * with net_close), or NET_ERROR. */
enum net_status net_udp_listen(unsigned port, int *sock, int *err);

/* One datagram, without waiting: NET_OK with its length in *done (a
 * datagram longer than len is cut to len) and the sender's IPv4 address
 * as text in from_ip ("" when the sender is not IPv4), NET_AGAIN when
 * none is waiting, NET_ERROR otherwise. */
#define NET_IPV4_TEXT 16
enum net_status net_udp_recv(int sock, void *buf, size_t len, size_t *done, char from_ip[NET_IPV4_TEXT], int *err);

/* A non-blocking TCP socket listening on every local IPv4 address at
 * port. NET_OK with *sock set, or NET_ERROR (the port is taken, ...). */
enum net_status net_tcp_listen(unsigned port, int *sock, int *err);
/* One waiting connection, without waiting: NET_OK with a non-blocking
 * *sock (the caller's, closed with net_close) and the caller's IPv4
 * address in from_ip, NET_AGAIN when none waits, NET_ERROR otherwise. */
enum net_status net_accept(int lsock, int *sock, char from_ip[NET_IPV4_TEXT], int *err);

/* One datagram to every machine on the local network (255.255.255.255)
 * at port, from the UDP socket sock. */
enum net_status net_udp_broadcast(int sock, unsigned port, const void *buf, size_t len, int *err);

/* 1 when text is a numeric IPv4 address (a.b.c.d), 0 otherwise. */
int net_is_ipv4(const char *text);
/* The IPv4 address of a host name (a numeric address is returned as it
 * is). This one waits: for the system's resolver, as long as it takes. */
enum net_status net_resolve(const char *host, char ip[NET_IPV4_TEXT], int *err);
/* The IPv4 address at the other end of a connected socket. */
enum net_status net_peer_ip(int sock, char ip[NET_IPV4_TEXT], int *err);

void net_close(int sock);

/* Opens the socket library where the platform needs it; 0 on success. */
int net_init(void);

#endif
