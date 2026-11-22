/* One configured device and the folders shared with it, for the engine
 * (engine.h): the connection to the device, made again whenever it is
 * lost, carrying every one of its folders (two-way, tw.h).
 *
 * Nothing here waits for the peer: link_step does what can be done now
 * and returns, so one loop can serve several devices. The exceptions are
 * short and bounded: the farewell at the end of a connection (Close sent
 * and acknowledged, a few seconds at most) and single file-system calls.
 *
 * A folder is opened and read once (in pieces) and then lives as long as
 * the link, attached to a session while there is one (tw.h): its records
 * and counters stay in memory between connections, and without a
 * connection it is still read every rescan interval, so that changes
 * made here are recorded and go out with the next connection's index.
 *
 * An attempt goes through: reading the folders not read yet, the TCP
 * connection and TLS handshake (directly, or through a relay: relay.h),
 * then the session. Each attempt takes the next of the device's
 * addresses: the typed one, or the one it announced on the local
 * network (link_set_address), then those of global discovery
 * (link_set_global), direct ones before relays. After a failure the
 * next address is tried a second later; once all have failed, the
 * pause grows 1, 2, 4 ... 60 s, and starts again at 1 s once a session
 * has received the whole index of a folder.
 *
 * A device without a typed address (`address dynamic`, conf.h) makes no
 * attempt while no address is known: it waits with NO_ADDRESS, its
 * folders not read before the first attempt. A connection the device made to us
 * (link_offer, reach.h) is taken at any time a session is not up, and
 * replaces a session through a relay when it is direct.
 *
 * A folder that cannot be used (its directory or marker is missing) or
 * that the peer does not share or has paused stays out of the session
 * and the other folders go on; a session with no usable folder ends. */
#ifndef ST_LINK_H
#define ST_LINK_H

#include "net.h"
#include "tls.h"

#define LINK_MAX_FOLDERS 100
#define LINK_MAX_GLOBAL 8

typedef struct {
    const char *id;   /* folder ID */
    const char *path; /* the folder on disk */
    long rescan_seconds;
} link_folder;

typedef struct {
    const char *name;         /* the device's name in the configuration, for log lines; NULL: none */
    const char *ip;           /* numeric IPv4; NULL: none yet, given later with link_set_address */
    unsigned port;
    unsigned char peer_id[DEVID_BYTES];
    const char *device_name;  /* ours, sent in the Hello */
    const char *identity_dir; /* the state files are kept here */
    const link_folder *folders;
    size_t nfolders;
    tls_log_fn log;
    tls_log_fn ui;            /* short user-facing lines; NULL for none */
    /* Called with "device" or "device/folder" before lines about that
     * device or folder are written, and with NULL afterwards; NULL when
     * the lines need no such mark (one device, one folder). */
    void (*scope)(const char *text);
} link_config;

typedef struct peerlink peerlink;

/* The configuration and everything it points to must outlive the link. */
peerlink *link_new(const link_config *cfg, const tls_identity *id);

/* Moves on without waiting. *wake: the time (net_now) at which it wants
 * to be called again even if nothing happens on its socket. Returns NULL,
 * or the reason why the program cannot go on at all (OUT_OF_MEMORY,
 * RNG_FAILED, BAD_ARGUMENT). */
const char *link_step(peerlink *l, net_ms now, net_ms *wake);

/* What to wait for until the next step: fills *it (sock -1 when there
 * is no socket to wait on). Returns 1 when the next step should follow
 * at once. */
int link_want(const peerlink *l, net_wait_item *it);

/* Ends a session in progress (state written, Close sent) and releases
 * everything. Returns 1 when the last session that ended, this one
 * included, left a folder's state file behind the folder. */
int link_close(peerlink *l);

/* What the device and its folders are doing, for the control interface
 * (ctl.h). The strings are fixed words, valid until the next step. */
typedef struct {
    const char *name;     /* "" when the device has no name */
    const char *state;    /* waiting, reading, connecting, connected */
    const char *last_end; /* why the last attempt ended, or ""; NO_ADDRESS: none was made, no address is known */
    char address[32];     /* "IP:PORT": of the connection (a relay's, through one; only IP for one that came
                           * in), otherwise the first to try; "" when not known */
    const char *via;      /* connecting, connected: direct, incoming (it connected to us) or relay; otherwise "" */
    int addresses;        /* how many addresses are known to try */
    long retry_seconds;   /* waiting: until the next attempt */
    long sessions;
    size_t nfolders;
} link_status;

typedef struct {
    const char *id, *path;
    /* idle (no connection), reading, unusable, not-shared, paused (on
     * the peer), paused-here (link_pause), index (receiving the peer's
     * index), scanning, syncing, in-sync */
    const char *state;
    const char *why;      /* unusable: the reason, otherwise "" */
    long received, deleted, conflicts, held, failed;
    long long bytes_in, bytes_out;
} link_folder_status;

void link_get_status(const peerlink *l, net_ms now, link_status *out);
void link_get_folder_status(const peerlink *l, size_t i, link_folder_status *out);
/* Has the folder read again now. 0, or -1 when the link has no such
 * folder; a folder that is not in a session is read at its next attempt
 * anyway. */
int link_scan_now(peerlink *l, const char *folder_id);

/* Pauses a folder or takes it up again, for this run only (nothing is
 * written to the configuration): a paused folder is in no session and
 * is not read; a session it was part of ends (FOLDER_PAUSED_HERE) and
 * the next one, at once, goes on with the other folders. Taking it up
 * again ends the session likewise (FOLDER_RESUMED). 0, 1 when the
 * folder already was in that state, -1 when the link has no such
 * folder. */
int link_pause(peerlink *l, const char *folder_id, int paused);

/* The device's Device ID (DEVID_BYTES). */
const unsigned char *link_peer_id(const peerlink *l);

/* Where the device is, from an announcement of its own (disco.h). An
 * address that differs from the one known is logged and used from the
 * next attempt on: a link that waits makes that attempt at once, a
 * connection that exists is left alone. Returns 1 when the address is
 * new, 0 when it is the one already known or the device has a typed
 * address, which announcements do not replace. */
int link_set_address(peerlink *l, const char *ip, unsigned port);

/* A connection that came in from the device (reach.h), verified. 1:
 * taken, it is the link's now and is used at once (an attempt of our
 * own in progress is given up; a session through a relay ends for it,
 * REPLACED_BY_DIRECT, when this one is direct); 0: not taken (a session
 * is up, or one such connection already waits), the caller closes it.
 * via: "incoming" or "relay"; from: its IPv4 address. */
int link_offer(peerlink *l, tls_conn *c, const char *via, const char *from);

/* What global discovery knows of a device without a typed address:
 * tcp:// addresses (numeric IPv4) are tried before relay:// ones, after
 * the one announced on the local network. Returns 1 when the list
 * changed (a waiting link then tries at once). */
int link_set_global(peerlink *l, const char *const *urls, int n);
/* Should global discovery be asked about this device (no typed
 * address, no session)? */
int link_wants_lookup(const peerlink *l);

/* Attempts so far. */
long link_sessions(const peerlink *l);

/* Pause before the attempt after `failures` attempts in a row that did
 * not get as far as the peer's index: 1, 2, 4 ... up to 60 seconds. */
long link_backoff_seconds(int failures);

#endif
