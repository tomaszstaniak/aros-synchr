/* TLS for Syncthing connections: TLS 1.3 only, ALPN "bep/1.0", our device
 * certificate as client certificate, and the peer accepted only when the
 * SHA-256 of its single certificate equals the expected Device ID.
 *
 * Syncthing certificates are self-signed, so there is no CA chain to
 * check; the Device ID pin *is* the authentication. It is applied after the
 * handshake (which already verified the peer's CertificateVerify signature
 * with the certificate's key) and before any application byte is sent. */
#ifndef ST_TLS_H
#define ST_TLS_H

#include "devid.h"
#include "net.h"

#include <stddef.h>

typedef struct tls_conn tls_conn;
typedef struct tls_identity tls_identity;

typedef void (*tls_log_fn)(const char *fmt, ...);

/* Selects the RNG seed source before any other OpenSSL call. In builds
 * with ST_USE_JITTER the only source is OpenSSL's JITTER (jitterentropy);
 * there is no fallback. Then draws once, so a broken source fails here,
 * before any key is made. Returns 0 on success. */
int tls_rng_init(tls_log_fn log);

/* Creates an Ed25519 key and a self-signed certificate (CN "syncthing",
 * as Syncthing does) and writes cert.pem and key.pem into dir, refusing
 * to overwrite existing files. */
int tls_identity_create(const char *dir, tls_log_fn log);

/* Loads cert.pem and key.pem from dir and checks that they belong together. */
tls_identity *tls_identity_load(const char *dir, tls_log_fn log);
void tls_identity_free(tls_identity *id);
void tls_identity_devid(const tls_identity *id, unsigned char out[DEVID_BYTES]);

enum tls_result {
    TLS_OK = 0,
    TLS_E_CONNECT,      /* TCP connection failed */
    TLS_E_TIMEOUT,      /* overall deadline passed */
    TLS_E_HANDSHAKE,    /* TLS handshake failed */
    TLS_E_PEER_CERT,    /* not exactly one peer certificate */
    TLS_E_PEER_ID,      /* peer Device ID differs from the expected one */
    TLS_E_IO,           /* error or unexpected close after the handshake */
    TLS_E_INTERNAL,
    TLS_E_STOPPED,      /* a wait was ended by the function given to net_set_interrupt */
    TLS_E_AGAIN         /* tls_connect_step: not connected yet */
};

const char *tls_result_name(enum tls_result r);

/* Connects, handshakes and verifies the peer. The deadline covers all of
 * it and is not extended. On TLS_OK *out holds the connection. On any
 * other result everything is released and *out is NULL. */
enum tls_result tls_connect(const tls_identity *id, const char *ipv4, unsigned port,
                            const unsigned char expected[DEVID_BYTES], net_ms deadline,
                            tls_log_fn log, tls_conn **out);

/* How the peer is judged after the handshake. */
enum tls_check {
    TLS_CHECK_PIN = 0, /* its Device ID must be `expected` */
    TLS_CHECK_DEVICE,  /* any single certificate; tls_peer_devid says whose */
    TLS_CHECK_NONE     /* not at all: a web server, whose answers are only hints (https.h) */
};

typedef struct {
    const char *ip;        /* connect to ip:port (numeric IPv4), or */
    unsigned port;
    int sock;              /* >= 0: use this connected socket instead; it is the
                            * connection's from now on, closed with it, on failure too */
    int server;            /* our side is the TLS server (a connection that came in) */
    const char *alpn;      /* the one protocol offered or accepted; NULL: "bep/1.0" */
    enum tls_check check;
    const unsigned char *expected; /* TLS_CHECK_PIN */
    const char *sni;       /* the server name sent, or NULL */
    int allow_tls12;       /* TLS 1.2 too (web servers); otherwise 1.3 only */
} tls_opts;

/* tls_start with every choice open: a server side, a socket that is
 * already connected (a relay session, relay.h), another protocol. A
 * server, and a client that asked for a protocol other than bep/1.0,
 * require the peer to agree to it. */
enum tls_result tls_start_opts(const tls_identity *id, const tls_opts *o, tls_log_fn log, tls_conn **out);
/* After TLS_OK with TLS_CHECK_PIN or TLS_CHECK_DEVICE: the peer's Device ID. */
void tls_peer_devid(const tls_conn *c, unsigned char out[DEVID_BYTES]);
/* The connection's socket (for its address). */
int tls_socket(const tls_conn *c);

/* The same in steps, for a caller with its own loop and several
 * connections. tls_start begins the TCP connection and returns at once
 * (TLS_OK: *out is a connection being made). tls_connect_step moves it on
 * without waiting: TLS_E_AGAIN while the peer's part is outstanding,
 * TLS_OK once the peer is verified (the connection is then used like one
 * from tls_connect), anything else is the failure; the caller then calls
 * tls_abort, which is also how an attempt is given up.
 * tls_want says what to wait for: the socket and whether it has to be
 * readable or writable (want_read: the caller would read application
 * data). Returns 1 when there is no need to wait at all (data already
 * decrypted). */
enum tls_result tls_start(const tls_identity *id, const char *ipv4, unsigned port,
                          const unsigned char expected[DEVID_BYTES], tls_log_fn log, tls_conn **out);
enum tls_result tls_connect_step(tls_conn *c);
int tls_want(const tls_conn *c, int want_read, int *sock, int *rd, int *wr);
void tls_abort(tls_conn *c);

/* Reads up to len bytes (at least 1 unless the result is not TLS_OK).
 * TLS_E_IO with *closed set means the peer closed the connection. */
enum tls_result tls_read(tls_conn *c, void *buf, size_t len, size_t *got, int *closed,
                         net_ms deadline);

/* Writes all len bytes, waiting as needed, within the deadline. */
enum tls_result tls_write(tls_conn *c, const void *buf, size_t len, net_ms deadline);

/* The same without waiting, for a caller with its own loop.
 * tls_queue keeps a copy of the bytes to send (TLS_E_INTERNAL when out
 * of memory); tls_flush_some writes what the socket takes now;
 * tls_pending is what is still kept; tls_pending_since the time
 * (net_now) of the last byte written, or of the first byte queued.
 * tls_read_some reads what is there: TLS_OK with *got 0 when nothing is.
 * tls_wait returns TLS_OK when something can be read (want_read) or
 * queued bytes can be written, TLS_E_TIMEOUT at the deadline.
 * tls_flush writes everything queued, waiting, within the deadline. */
enum tls_result tls_queue(tls_conn *c, const void *buf, size_t len);
enum tls_result tls_flush_some(tls_conn *c);
size_t tls_pending(const tls_conn *c);
net_ms tls_pending_since(const tls_conn *c);
enum tls_result tls_read_some(tls_conn *c, void *buf, size_t len, size_t *got, int *closed);
enum tls_result tls_wait(tls_conn *c, int want_read, net_ms deadline);
enum tls_result tls_flush(tls_conn *c, net_ms deadline);

/* Sends close_notify (best effort, bounded by deadline), frees everything
 * and closes the socket. Safe with NULL. */
void tls_close(tls_conn *c, net_ms deadline);

#endif
