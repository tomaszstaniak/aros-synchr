/* Syncthing local discovery, receiving side (Local Discovery Protocol
 * v4): every Syncthing device broadcasts an announcement on the local
 * network about every 30 s, to UDP port 21027:
 *
 *     4 bytes   magic 0x2EA7D90B, big-endian
 *     the rest  protobuf Announce { bytes id = 1;             (32 bytes)
 *                                   repeated string addresses = 2;
 *                                   int64 instance_id = 3; }
 *
 * Addresses are URLs such as tcp://192.168.1.5:22000, tcp://0.0.0.0:22000,
 * tcp://:22000, quic://..., relay://...; a host that is empty or the
 * unspecified address stands for the address the packet came from.
 *
 * A device configured with `address dynamic` (conf.h) gets its address
 * from these. Ours are sent the same way (reach.h), with disco_build and
 * disco_send.
 * An announcement is not authenticated; it only says where to try, and
 * the TLS handshake still checks the Device ID of whoever answers. */
#ifndef ST_DISCO_H
#define ST_DISCO_H

#include "devid.h"

#include <stddef.h>
#include <stdint.h>

#define DISCO_PORT 21027
#define DISCO_MAGIC 0x2EA7D90Bu
/* A longer datagram is not an announcement of ours to use (Syncthing's
 * are a few hundred bytes); it is read cut short and then refused. */
#define DISCO_MAX_PACKET 4096

typedef struct {
    unsigned char id[DEVID_BYTES];
    char ip[16]; /* numeric IPv4 */
    unsigned port;
} disco_found;

/* Reads one packet. sender: the IPv4 address it came from as text, or
 * NULL or "" when that is not known (addresses that need it are then
 * passed over). 0 with *out filled from the first usable address: scheme
 * tcp or tcp4, a numeric IPv4 host (or an empty or unspecified one: the
 * sender), port 1 to 65535. -1 for anything else: wrong magic, malformed
 * or truncated protobuf, an id that is not 32 bytes, no usable address
 * (IPv6 only, quic, relay, host names). */
int disco_parse(const void *packet, size_t len, const char *sender, disco_found *out);

/* Starts listening. 0, with the port in *port; -1 when the socket cannot
 * be had (why says so). The port is DISCO_PORT; on systems other than
 * AROS the environment variable STPOC_DISCOVERY_PORT replaces it, for
 * tests. */
int disco_open(unsigned *port, char *why, size_t n);
/* The socket to wait on for more, or -1. */
int disco_socket(void);
/* Reads every packet that is waiting (a bounded number per call), never
 * waiting for one, and calls fn for each that disco_parse accepts. 0, or
 * -1 when the socket failed (*err: the error number): the caller closes
 * it, since a broken socket would be reported readable for ever. */
int disco_poll(void (*fn)(void *ctx, const disco_found *found), void *ctx, int *err);
void disco_close(void);

/* Our own announcement: the packet into out (cap bytes); its length, or
 * 0 when it does not fit. */
size_t disco_build(const unsigned char id[DEVID_BYTES], const char *const *addresses, int n, int64_t instance,
                   unsigned char *out, size_t cap);
/* Broadcasts a packet from the discovery socket to port (DISCO_PORT, or
 * the test port). 0, or -1 (*err; 0 when the socket is not open). */
int disco_send(const void *packet, size_t len, unsigned port, int *err);

#endif
