/* Being reachable and finding devices beyond the local network, as
 * Syncthing does by default:
 *
 * - Connections that come in: a TCP listener (port 22000 unless the
 *   configuration says otherwise). The device that connects is known by
 *   its certificate after the TLS handshake (we are the server); one
 *   that is not configured is turned away and noted (reach_status).
 * - A relay (relay.h): one from the public pool (the list at
 *   relays.syncthing.net; the one that answers first of a few tried at
 *   once), or a fixed one from the configuration. Devices that cannot
 *   reach us directly come in through it.
 * - Global discovery (https.h): where we can be reached (the listening
 *   port, which the server completes with the address the announcement
 *   came from, and the relay) is announced every half hour; devices
 *   without a typed address are looked up there, at most every five
 *   minutes each, as long as the engine asks (reach_lookup).
 * - Local discovery (disco.h): the same addresses broadcast on the
 *   local network every 30 s, so that a Syncthing nearby finds us.
 *
 * A connection that came in, directly or through the relay, and has
 * passed the handshake is offered to the engine (offer); one it does
 * not take is closed. Nothing here waits, except for host name lookups
 * (https_start). */
#ifndef ST_REACH_H
#define ST_REACH_H

#include "net.h"
#include "tls.h"

#define REACH_DEFAULT_PORT 22000
#define REACH_LOOKUP_URL "https://discovery-lookup.syncthing.net/v2/"
#define REACH_ANNOUNCE_URL "https://discovery-announce-v4.syncthing.net/v2/"
#define REACH_POOL_URL "https://relays.syncthing.net/endpoint"
/* Wait items reach_want may fill. */
#define REACH_MAX_ITEMS 32

typedef struct {
    unsigned listen_port;    /* 0: no listener */
    int global;              /* announce and look up through global discovery */
    int relays;              /* belong to a relay */
    int local_announce;      /* broadcast our addresses on the local network ... */
    unsigned local_port;     /* ... to this port (the one disco_open listens on) */
    const char *lookup_url, *announce_url; /* NULL: Syncthing's */
    const char *pool_url;    /* NULL: Syncthing's */
    const char *fixed_relay; /* a relay:// address instead of the pool; NULL none */
} reach_options;

typedef struct {
    void *ctx;
    /* Is this one of our devices? (Before a relay session is joined.) */
    int (*known)(void *ctx, const unsigned char peer[DEVID_BYTES]);
    /* A verified connection from peer: 1 taken (it is the engine's now),
     * 0 not (it is closed here). via: "incoming" or "relay". */
    int (*offer)(void *ctx, const unsigned char peer[DEVID_BYTES], tls_conn *c, const char *via, const char *from);
    /* What global discovery knows of peer: n addresses (URLs). */
    void (*found)(void *ctx, const unsigned char peer[DEVID_BYTES], const char *const *urls, int n);
} reach_callbacks;

typedef struct reach reach;

/* Starts the listener at once (a port that is taken is logged; the
 * other parts go on). NULL when out of memory. */
reach *reach_new(const reach_options *o, const tls_identity *id, tls_log_fn log, const reach_callbacks *cb);
void reach_step(reach *r, net_ms now, net_ms *wake);
/* Fills up to REACH_MAX_ITEMS wait items; returns their number, or -1
 * when the next step should follow at once. */
int reach_want(const reach *r, net_wait_item *items);
/* Asks global discovery where peer is (not more often than every five
 * minutes, or what the server asks for). */
void reach_lookup(reach *r, const unsigned char peer[DEVID_BYTES]);
/* For STATUS: "listen=PORT|- relay=URL|- announced=yes|no|- unknown=ID@IP|-" */
void reach_status(const reach *r, char *out, size_t n);
void reach_free(reach *r);

#endif
