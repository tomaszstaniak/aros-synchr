/* Syncthing relays (Relay Protocol v1, lib/relay/protocol in Syncthing):
 * a public server that passes a connection between two devices that
 * cannot reach each other directly. Nothing of the files is visible to
 * it: the two devices make their own TLS connection (BEP) through it,
 * each checking the other's Device ID as on a direct connection.
 *
 * Messages, in both directions, over TLS with ALPN "bep-relay" to the
 * relay (our device certificate is the client certificate):
 *
 *     uint32 magic 0x9E79BC40, int32 type, int32 length, then the body
 *     in XDR (every number 4 bytes big-endian; bytes and strings: a
 *     4-byte length, the bytes, zeros to a multiple of 4)
 *
 *     0 Ping, 1 Pong, 7 RelayFull         no body
 *     2 JoinRelayRequest  { string token }
 *     3 JoinSessionRequest { bytes key }
 *     4 Response           { int32 code; string message }   0: success
 *     5 ConnectRequest     { bytes id }                     (32 bytes)
 *     6 SessionInvitation  { bytes from, key, address; uint16 port; bool server_socket }
 *
 * Being reachable: a device joins one relay (JoinRelayRequest), stays
 * connected and answers its Pings; the relay's address is announced
 * (global discovery). When another device asks the relay for it, the
 * relay sends a SessionInvitation.
 *
 * Reaching a device: ask its relay with ConnectRequest; the answer is a
 * SessionInvitation (or Response 1, not found).
 *
 * Either way, the invitation is used the same: a plain TCP connection
 * to its address (none or 0.0.0.0: the relay's) and port, a
 * JoinSessionRequest with its key in plain text, Response 0, and from
 * then on the connection carries the two devices' own TLS, ours as a
 * server when server_socket is set. */
#ifndef ST_RELAY_H
#define ST_RELAY_H

#include "devid.h"
#include "net.h"
#include "tls.h"

#include <stddef.h>
#include <stdint.h>

#define RELAY_MAGIC 0x9E79BC40u
#define RELAY_MAX_BODY 1024 /* longer is refused, as by Syncthing */
#define RELAY_URL_MAX 300

enum {
    RELAY_PING = 0,
    RELAY_PONG,
    RELAY_JOIN_RELAY,
    RELAY_JOIN_SESSION,
    RELAY_RESPONSE,
    RELAY_CONNECT,
    RELAY_INVITATION,
    RELAY_FULL
};

typedef struct {
    char ip[NET_IPV4_TEXT];
    unsigned port;
    unsigned char id[DEVID_BYTES]; /* the relay's own Device ID ... */
    int has_id;                    /* ... when the address names one */
    char token[100];               /* "token" of the address, for a private relay; "" none */
    char url[RELAY_URL_MAX];       /* as given */
} relay_addr;

/* relay://IP:PORT/?id=DEVICE-ID[&token=...][&other=...]. The host must
 * be a numeric IPv4 address (as the public pool gives them). 0, or -1
 * for anything else. */
int relay_parse_url(const char *url, relay_addr *out);

typedef struct {
    unsigned char from[DEVID_BYTES];
    unsigned char key[32];
    size_t key_len;
    char ip[NET_IPV4_TEXT]; /* "" when the invitation names none */
    unsigned port;
    int server_socket;
} relay_invitation;

/* Codec (exposed for tests). relay_encode writes header and body into
 * out (cap bytes) and returns the length, 0 when it does not fit.
 * relay_frame looks at buffered bytes: the length of the first whole
 * message (header included) with its type and body, 0 when more bytes
 * are needed, -1 when they are not a message (wrong magic, a length
 * that is negative or above RELAY_MAX_BODY). */
size_t relay_encode(int32_t type, const unsigned char *body, size_t len, unsigned char *out, size_t cap);
long relay_frame(const unsigned char *buf, size_t len, int32_t *type, const unsigned char **body, size_t *blen);
/* XDR bodies. Each returns the body's length (0 when it does not fit),
 * or for parsing 0 on success and -1 for a malformed body. */
size_t relay_body_bytes(const unsigned char *data, size_t len, unsigned char *out, size_t cap);
int relay_parse_response(const unsigned char *body, size_t len, int32_t *code, char *msg, size_t msg_cap);
int relay_parse_invitation(const unsigned char *body, size_t len, relay_invitation *out);

/* Reaching a device through a relay: relay_dial_start asks the relay
 * (ConnectRequest), relay_dial_invited takes an invitation that came
 * from a relay we belong to; both then join the session and make the
 * BEP connection over it, checking that the device at the other end is
 * peer (relay_dial_start) or inv->from. relay_dial_step moves on
 * without waiting: 0 not yet, 1 done (*conn: the connection, the
 * caller's), -1 failed (*why: a fixed word). relay_dial_want: what to
 * wait for, as tls_want. relay_dial_abort ends it at any point and is
 * also called after a failure (not after success). */
typedef struct relay_dial relay_dial;
relay_dial *relay_dial_start(const tls_identity *id, const relay_addr *relay, const unsigned char peer[DEVID_BYTES],
                             tls_log_fn log);
relay_dial *relay_dial_invited(const tls_identity *id, const relay_invitation *inv, tls_log_fn log);
int relay_dial_step(relay_dial *d, net_ms now, tls_conn **conn, const char **why);
void relay_dial_want(const relay_dial *d, int *sock, int *rd, int *wr);
void relay_dial_abort(relay_dial *d);
/* When the next step is due anyway (a time limit). */
net_ms relay_dial_deadline(const relay_dial *d);

/* Belonging to a relay, to be reachable through it. relay_member_step
 * moves on without waiting and returns:
 *   RELAY_EV_NONE        nothing new
 *   RELAY_EV_JOINED      the relay took us: it can be announced
 *   RELAY_EV_INVITATION  a device asks for us: *inv
 *   RELAY_EV_ENDED       the membership is over (*why); close it */
enum { RELAY_EV_NONE = 0, RELAY_EV_JOINED, RELAY_EV_INVITATION, RELAY_EV_ENDED };
typedef struct relay_member relay_member;
relay_member *relay_member_start(const tls_identity *id, const relay_addr *relay, tls_log_fn log);
int relay_member_step(relay_member *m, net_ms now, relay_invitation *inv, const char **why);
/* 1: step again at once (a message is waiting); otherwise *sock etc. */
int relay_member_want(const relay_member *m, int *sock, int *rd, int *wr);
net_ms relay_member_deadline(const relay_member *m);
const relay_addr *relay_member_addr(const relay_member *m);
void relay_member_close(relay_member *m);

#endif
