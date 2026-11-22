#include "tls.h"

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct tls_identity {
    X509 *cert;
    EVP_PKEY *key;
};

/* State of our socket BIO. OpenSSL's own socket BIO cannot be used on
 * AROS: on Unix-like targets it reads and closes with read()/close(),
 * which act on C library descriptors, not bsdsocket.library ones. */
struct sock_bio {
    int sock;
    int err;   /* last socket error, for the log */
    int eof;   /* peer closed */
};

struct tls_conn {
    SSL_CTX *ctx;
    SSL *ssl;
    struct sock_bio sb;
    tls_log_fn log;
    unsigned char *out; /* bytes queued for sending: out[out_head .. out_head + out_len) */
    size_t out_head, out_len, out_cap;
    net_ms out_since;
    int w_want; /* what the last write attempt waits for: SSL_ERROR_WANT_READ or _WRITE, 0 none */
    size_t w_len; /* the length offered in that attempt, offered again unchanged; 0 none */
    int r_want_write; /* the last read attempt waits for a writable socket */
    /* A connection being made (tls_start, tls_connect_step). */
    int stage;        /* ST_UP, ST_TCP or ST_HANDSHAKE */
    int hs_want;      /* what the handshake waits for */
    unsigned char expected[DEVID_BYTES];
    unsigned char peer[DEVID_BYTES]; /* the peer's Device ID, once checked */
    enum tls_check check;
    int server;
    unsigned char alpn[32]; /* the protocol wanted, as a length byte and the name */
    char ip[48];
    unsigned port;
    net_ms t0;
};

enum { ST_UP = 0, ST_TCP, ST_HANDSHAKE };

static void log_ssl_errors(tls_log_fn log, const char *what)
{
    unsigned long e;
    const char *data;
    int flags, any = 0;
    char buf[256];
    while ((e = ERR_get_error_all(NULL, NULL, NULL, &data, &flags)) != 0) {
        ERR_error_string_n(e, buf, sizeof buf);
        log("%s: %s%s%s", what, buf, (flags & ERR_TXT_STRING) ? " - " : "",
            (flags & ERR_TXT_STRING) ? data : "");
        any = 1;
    }
    if (!any)
        log("%s: no OpenSSL error recorded", what);
}

int tls_rng_init(tls_log_fn log)
{
    unsigned char probe[32];
    /* No openssl.cnf: its location is compiled in and would otherwise be
     * read on first use; all settings this program needs are made here. */
    if (OPENSSL_init_ssl(OPENSSL_INIT_NO_LOAD_CONFIG, NULL) != 1) {
        log_ssl_errors(log, "openssl: init");
        return -1;
    }
#ifdef ST_USE_JITTER
    if (RAND_set_seed_source_type(NULL, "JITTER", NULL) != 1) {
        log_ssl_errors(log, "rng: selecting JITTER");
        return -1;
    }
    log("rng: seed source JITTER selected");
#else
    log("rng: platform default seed source (host build)");
#endif
    if (RAND_bytes(probe, sizeof probe) != 1) {
        log_ssl_errors(log, "rng: first draw");
        return -1;
    }
    OPENSSL_cleanse(probe, sizeof probe);
    log("rng: DRBG seeded");
    return 0;
}

static void join(char *out, size_t n, const char *dir, const char *file)
{
    size_t l = strlen(dir);
    /* AROS paths may end in a volume or assign name ("RAM:"), which takes
     * no separator. */
    const char *sep = (l == 0 || dir[l - 1] == '/' || dir[l - 1] == ':') ? "" : "/";
    snprintf(out, n, "%s%s%s", dir, sep, file);
}

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f)
        fclose(f);
    return f != NULL;
}

/* Creates path exclusively (never replacing a file) with the given
 * permission bits on systems that have them; the private key gets 0600,
 * not whatever the umask allows. On AROS the bits map to protection flags
 * only as far as the C library maps them; no access control is implied. */
static BIO *create_exclusive(const char *path, int mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, mode);
    if (fd < 0)
        return NULL;
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        return NULL;
    }
    BIO *b = BIO_new_fp(f, BIO_CLOSE);
    if (!b)
        fclose(f);
    return b;
}

static int add_ext(X509 *x, int nid, const char *value)
{
    X509V3_CTX ctx;
    X509V3_set_ctx(&ctx, x, x, NULL, NULL, 0);
    X509_EXTENSION *ext = X509V3_EXT_conf_nid(NULL, &ctx, nid, value);
    if (!ext)
        return 0;
    int ok = X509_add_ext(x, ext, -1);
    X509_EXTENSION_free(ext);
    return ok;
}

int tls_identity_create(const char *dir, tls_log_fn log)
{
    char cert_path[512], key_path[512];
    join(cert_path, sizeof cert_path, dir, "cert.pem");
    join(key_path, sizeof key_path, dir, "key.pem");
    if (file_exists(cert_path) || file_exists(key_path)) {
        log("identity: %s or %s already exists; not overwriting", cert_path, key_path);
        return -1;
    }

    int rc = -1, created_key = 0, created_cert = 0;
    EVP_PKEY *key = NULL;
    X509 *x = NULL;
    BIGNUM *serial = NULL;
    BIO *out = NULL;
    X509_NAME *name = NULL;

    if (!(key = EVP_PKEY_Q_keygen(NULL, NULL, "ED25519"))) {
        log_ssl_errors(log, "identity: Ed25519 key generation");
        goto done;
    }
    /* The certificate mirrors what Syncthing generates for sync
     * connections (lib/tlsutil generateCertificate): CN "syncthing" is
     * what the peer checks by default. */
    if (!(x = X509_new()) || !X509_set_version(x, X509_VERSION_3)
        || !(serial = BN_new()) || !BN_rand(serial, 63, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY)
        || !BN_to_ASN1_INTEGER(serial, X509_get_serialNumber(x))
        || !X509_gmtime_adj(X509_getm_notBefore(x), 0)
        || !X509_time_adj_ex(X509_getm_notAfter(x), 20 * 365, 0, NULL)
        || !(name = X509_NAME_new())
        || !X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, (const unsigned char *)"Syncthing", -1, -1, 0)
        || !X509_NAME_add_entry_by_txt(name, "OU", MBSTRING_ASC,
                                       (const unsigned char *)"Automatically Generated", -1, -1, 0)
        || !X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)"syncthing", -1, -1, 0)
        || !X509_set_subject_name(x, name)
        || !X509_set_issuer_name(x, name)
        || !X509_set_pubkey(x, key)
        || !add_ext(x, NID_basic_constraints, "critical,CA:FALSE")
        || !add_ext(x, NID_key_usage, "critical,digitalSignature")
        || !add_ext(x, NID_ext_key_usage, "serverAuth,clientAuth")
        || !add_ext(x, NID_subject_alt_name, "DNS:syncthing")
        || !X509_sign(x, key, NULL)) {
        log_ssl_errors(log, "identity: certificate");
        goto done;
    }
    if (!(out = create_exclusive(key_path, 0600))) {
        log("identity: cannot create %s", key_path);
        goto done;
    }
    created_key = 1;
    if (!PEM_write_bio_PrivateKey(out, key, NULL, NULL, 0, NULL, NULL) || BIO_flush(out) != 1) {
        log_ssl_errors(log, "identity: writing key");
        goto done;
    }
    BIO_free(out);
    if (!(out = create_exclusive(cert_path, 0644))) {
        log("identity: cannot create %s", cert_path);
        goto done;
    }
    created_cert = 1;
    if (!PEM_write_bio_X509(out, x) || BIO_flush(out) != 1) {
        log_ssl_errors(log, "identity: writing certificate");
        goto done;
    }
    rc = 0;
done:
    BIO_free(out);
    /* A half-written pair would block the next attempt (creation is
     * exclusive) and could pair a key with no certificate. */
    if (rc != 0) {
        if (created_key)
            remove(key_path);
        if (created_cert)
            remove(cert_path);
    }
    X509_NAME_free(name);
    BN_free(serial);
    X509_free(x);
    EVP_PKEY_free(key);
    return rc;
}

tls_identity *tls_identity_load(const char *dir, tls_log_fn log)
{
    char cert_path[512], key_path[512];
    join(cert_path, sizeof cert_path, dir, "cert.pem");
    join(key_path, sizeof key_path, dir, "key.pem");

    tls_identity *id = calloc(1, sizeof *id);
    BIO *in = NULL;
    if (!id)
        return NULL;
    if (!(in = BIO_new_file(cert_path, "r")) || !(id->cert = PEM_read_bio_X509(in, NULL, NULL, NULL))) {
        log_ssl_errors(log, "identity: reading certificate");
        goto fail;
    }
    BIO_free(in);
    if (!(in = BIO_new_file(key_path, "r")) || !(id->key = PEM_read_bio_PrivateKey(in, NULL, NULL, NULL))) {
        log_ssl_errors(log, "identity: reading key");
        goto fail;
    }
    BIO_free(in);
    in = NULL;
    if (X509_check_private_key(id->cert, id->key) != 1) {
        log_ssl_errors(log, "identity: key does not match certificate");
        goto fail;
    }
    return id;
fail:
    BIO_free(in);
    tls_identity_free(id);
    return NULL;
}

void tls_identity_free(tls_identity *id)
{
    if (!id)
        return;
    X509_free(id->cert);
    EVP_PKEY_free(id->key);
    free(id);
}

static int cert_devid(X509 *cert, unsigned char out[DEVID_BYTES])
{
    unsigned char *der = NULL;
    unsigned int mdlen = 0;
    int len = i2d_X509(cert, &der);
    if (len <= 0)
        return -1;
    int ok = EVP_Digest(der, (size_t)len, out, &mdlen, EVP_sha256(), NULL);
    OPENSSL_free(der);
    return ok && mdlen == DEVID_BYTES ? 0 : -1;
}

void tls_identity_devid(const tls_identity *id, unsigned char out[DEVID_BYTES])
{
    if (cert_devid(id->cert, out) != 0)
        memset(out, 0, DEVID_BYTES);
}

const char *tls_result_name(enum tls_result r)
{
    switch (r) {
    case TLS_OK: return "OK";
    case TLS_E_CONNECT: return "CONNECT_FAILED";
    case TLS_E_TIMEOUT: return "TIMEOUT";
    case TLS_E_HANDSHAKE: return "TLS_HANDSHAKE_FAILED";
    case TLS_E_PEER_CERT: return "PEER_CERTIFICATE_INVALID";
    case TLS_E_PEER_ID: return "PEER_ID_MISMATCH";
    case TLS_E_IO: return "IO_ERROR";
    case TLS_E_INTERNAL: return "INTERNAL_ERROR";
    case TLS_E_STOPPED: return "STOPPED";
    case TLS_E_AGAIN: return "NOT_YET";
    }
    return "UNKNOWN";
}

/* ---- socket BIO ---- */

static int sbio_write_ex(BIO *b, const char *data, size_t len, size_t *written)
{
    struct sock_bio *s = BIO_get_data(b);
    BIO_clear_retry_flags(b);
    enum net_status st = net_send(s->sock, data, len, written, &s->err);
    if (st == NET_OK)
        return 1;
    if (st == NET_AGAIN)
        BIO_set_retry_write(b);
    return 0;
}

static int sbio_read_ex(BIO *b, char *data, size_t len, size_t *readbytes)
{
    struct sock_bio *s = BIO_get_data(b);
    BIO_clear_retry_flags(b);
    enum net_status st = net_recv(s->sock, data, len, readbytes, &s->err);
    if (st == NET_OK)
        return 1;
    if (st == NET_AGAIN)
        BIO_set_retry_read(b);
    else if (st == NET_CLOSED)
        s->eof = 1;
    *readbytes = 0;
    return 0;
}

static long sbio_ctrl(BIO *b, int cmd, long num, void *ptr)
{
    struct sock_bio *s = BIO_get_data(b);
    (void)num;
    (void)ptr;
    switch (cmd) {
    case BIO_CTRL_FLUSH:
        return 1;
    case BIO_CTRL_EOF:
        return s && s->eof;
    default:
        return 0;
    }
}

static int sbio_create(BIO *b)
{
    BIO_set_init(b, 1);
    return 1;
}

static BIO_METHOD *sbio_method(void)
{
    static BIO_METHOD *m;
    if (!m) {
        m = BIO_meth_new(BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "aros-syncthing socket");
        if (m && (!BIO_meth_set_write_ex(m, sbio_write_ex) || !BIO_meth_set_read_ex(m, sbio_read_ex)
                  || !BIO_meth_set_ctrl(m, sbio_ctrl) || !BIO_meth_set_create(m, sbio_create))) {
            BIO_meth_free(m);
            m = NULL;
        }
    }
    return m;
}

/* ---- connection ---- */

/* The certificate chain is not judged against any CA: Syncthing devices
 * are self-signed by design. Acceptance is decided by the Device ID pin
 * in tls_connect(). The handshake itself still verifies the peer's
 * CertificateVerify signature with the presented certificate's key. */
static int accept_any_chain(int preverify_ok, X509_STORE_CTX *ctx)
{
    (void)preverify_ok;
    (void)ctx;
    return 1;
}

/* As a server: the client must offer our protocol, which is chosen. */
static int alpn_select(SSL *ssl, const unsigned char **out, unsigned char *outlen, const unsigned char *in,
                       unsigned int inlen, void *arg)
{
    tls_conn *c = arg;
    (void)ssl;
    unsigned char *sel = NULL;
    if (SSL_select_next_proto(&sel, outlen, c->alpn, (unsigned)c->alpn[0] + 1, in, inlen) == OPENSSL_NPN_NEGOTIATED) {
        *out = sel;
        return SSL_TLSEXT_ERR_OK;
    }
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}

static enum tls_result wait_for(tls_conn *c, int ssl_err, net_ms deadline)
{
    int err = 0;
    enum net_status st = net_wait(c->sb.sock, ssl_err == SSL_ERROR_WANT_WRITE, deadline, &err);
    if (st == NET_OK)
        return TLS_OK;
    if (st == NET_TIMEOUT)
        return TLS_E_TIMEOUT;
    if (st == NET_INTERRUPTED)
        return TLS_E_STOPPED;
    c->sb.err = err;
    return TLS_E_IO;
}

void tls_close(tls_conn *c, net_ms deadline)
{
    if (!c)
        return;
    if (c->ssl && !c->sb.eof) {
        /* close_notify only; we do not wait for the peer's, so one
         * attempt plus waits for a writable socket is enough. */
        for (;;) {
            int r = SSL_shutdown(c->ssl);
            if (r >= 0)
                break;
            int e = SSL_get_error(c->ssl, r);
            if ((e != SSL_ERROR_WANT_WRITE && e != SSL_ERROR_WANT_READ) || wait_for(c, e, deadline) != TLS_OK)
                break;
        }
    }
    ERR_clear_error();
    SSL_free(c->ssl); /* frees the BIO too */
    SSL_CTX_free(c->ctx);
    net_close(c->sb.sock);
    free(c->out);
    free(c);
}

void tls_abort(tls_conn *c)
{
    if (!c)
        return;
    /* No close_notify: nothing of ours should reach a peer we did not
     * authenticate beyond what TLS itself requires. */
    c->sb.eof = 1;
    tls_close(c, 0);
}

enum tls_result tls_start_opts(const tls_identity *id, const tls_opts *o, tls_log_fn log, tls_conn **out)
{
    const char *alpn = o->alpn ? o->alpn : "bep/1.0";
    size_t alen = strlen(alpn);
    int err = 0;
    BIO *bio = NULL;
    *out = NULL;

    tls_conn *c = calloc(1, sizeof *c);
    if (!c || alen == 0 || alen >= sizeof c->alpn || (o->check == TLS_CHECK_PIN && !o->expected)) {
        free(c);
        if (o->sock >= 0)
            net_close(o->sock);
        return TLS_E_INTERNAL;
    }
    c->sb.sock = -1;
    c->log = log;
    c->check = o->check;
    c->server = o->server;
    c->alpn[0] = (unsigned char)alen;
    memcpy(c->alpn + 1, alpn, alen);
    if (o->check == TLS_CHECK_PIN)
        memcpy(c->expected, o->expected, DEVID_BYTES);
    snprintf(c->ip, sizeof c->ip, "%s", o->ip ? o->ip : "");
    c->port = o->port;
    c->t0 = net_now();

    if (o->sock >= 0) {
        c->sb.sock = o->sock;
        c->stage = ST_HANDSHAKE;
    } else {
        enum net_status st = net_connect_start(o->ip, o->port, &c->sb.sock, &err);
        if (st != NET_OK && st != NET_AGAIN) {
            log("tcp: connect %s:%u failed (error, error %d)", c->ip, o->port, err);
            free(c);
            return TLS_E_CONNECT;
        }
        c->stage = ST_TCP;
    }

    if (!(c->ctx = SSL_CTX_new(o->server ? TLS_server_method() : TLS_client_method()))
        || !SSL_CTX_set_min_proto_version(c->ctx, o->allow_tls12 ? TLS1_2_VERSION : TLS1_3_VERSION)
        || !SSL_CTX_set_max_proto_version(c->ctx, TLS1_3_VERSION)
        || (!o->server && SSL_CTX_set_alpn_protos(c->ctx, c->alpn, (unsigned)alen + 1) != 0)
        || SSL_CTX_use_certificate(c->ctx, id->cert) != 1
        || SSL_CTX_use_PrivateKey(c->ctx, id->key) != 1
        || !(c->ssl = SSL_new(c->ctx))
        || (o->sni && SSL_set_tlsext_host_name(c->ssl, o->sni) != 1)
        || !sbio_method()
        || !(bio = BIO_new(sbio_method()))) {
        log_ssl_errors(log, "tls: setup");
        BIO_free(bio);
        tls_abort(c);
        return TLS_E_INTERNAL;
    }
    if (o->server) {
        /* The client's certificate is its Device ID: one is required. */
        SSL_CTX_set_alpn_select_cb(c->ctx, alpn_select, c);
        SSL_CTX_set_verify(c->ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, accept_any_chain);
        SSL_set_verify(c->ssl, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, accept_any_chain);
        /* One handshake per connection: no tickets to keep or resume. */
        SSL_set_num_tickets(c->ssl, 0);
        SSL_set_accept_state(c->ssl);
    } else {
        SSL_CTX_set_verify(c->ctx, SSL_VERIFY_PEER, accept_any_chain);
        SSL_set_verify(c->ssl, SSL_VERIFY_PEER, accept_any_chain);
        SSL_set_connect_state(c->ssl);
    }
    BIO_set_data(bio, &c->sb);
    /* Queued output (tls_queue) is offered again from a buffer that may
     * have moved or grown, and is written record by record. */
    SSL_set_mode(c->ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    SSL_set_bio(c->ssl, bio, bio); /* the SSL owns the BIO from here */
    *out = c;
    return TLS_OK;
}

enum tls_result tls_start(const tls_identity *id, const char *ipv4, unsigned port,
                          const unsigned char expected[DEVID_BYTES], tls_log_fn log, tls_conn **out)
{
    tls_opts o;
    memset(&o, 0, sizeof o);
    o.ip = ipv4;
    o.port = port;
    o.sock = -1;
    o.check = TLS_CHECK_PIN;
    o.expected = expected;
    return tls_start_opts(id, &o, log, out);
}

enum tls_result tls_connect_step(tls_conn *c)
{
    if (c->stage == ST_UP)
        return TLS_OK;
    if (c->stage == ST_TCP) {
        int err = 0;
        enum net_status st = net_connect_finish(c->sb.sock, &err);
        if (st == NET_AGAIN)
            return TLS_E_AGAIN;
        if (st != NET_OK) {
            c->log("tcp: connect %s:%u failed (error, error %d)", c->ip, c->port, err);
            return TLS_E_CONNECT;
        }
        c->log("tcp: connected to %s:%u in %lld ms", c->ip, c->port, (long long)(net_now() - c->t0));
        c->stage = ST_HANDSHAKE;
        c->t0 = net_now();
    }

    ERR_clear_error();
    int r = SSL_do_handshake(c->ssl);
    if (r != 1) {
        int e = SSL_get_error(c->ssl, r);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            c->hs_want = e;
            return TLS_E_AGAIN;
        }
        log_ssl_errors(c->log, "tls: handshake");
        if (c->sb.eof)
            c->log("tls: peer closed the connection during the handshake");
        else if (c->sb.err)
            c->log("tls: socket error %d during the handshake", c->sb.err);
        return TLS_E_HANDSHAKE;
    }

    const unsigned char *proto = NULL;
    unsigned int proto_len = 0;
    SSL_get0_alpn_selected(c->ssl, &proto, &proto_len);
    X509 *peer = SSL_get0_peer_certificate(c->ssl);
    STACK_OF(X509) *chain = SSL_get_peer_cert_chain(c->ssl);
    c->log("tls: handshake done in %lld ms%s: %s, %s, ALPN %.*s, peer certificate signature %s",
           (long long)(net_now() - c->t0), c->server ? " (we are the server)" : "", SSL_get_version(c->ssl),
           SSL_get_cipher_name(c->ssl), proto_len ? (int)proto_len : 6, proto_len ? (const char *)proto : "(none)",
           peer ? OBJ_nid2sn(X509_get_signature_nid(peer)) : "(none)");
    if (c->check == TLS_CHECK_NONE) {
        c->stage = ST_UP;
        return TLS_OK;
    }
    /* The protocol asked for, where it matters: a server chose it, a relay
     * must have taken it (as Syncthing checks). A BEP server that did not
     * answer the offer is accepted as before. */
    int want_named = c->server || strcmp((const char *)c->alpn + 1, "bep/1.0") != 0;
    if (want_named && (proto_len != c->alpn[0] || memcmp(proto, c->alpn + 1, proto_len) != 0)) {
        c->log("tls: the peer did not agree to %.*s", (int)c->alpn[0], (const char *)c->alpn + 1);
        return TLS_E_HANDSHAKE;
    }

    /* Like Syncthing (lib/connections/service.go): exactly one certificate,
     * or the peer has no Device ID. A server's list leaves out the
     * client's own certificate. */
    int count = chain ? sk_X509_num(chain) : 0;
    if (c->server && peer)
        count++;
    if (!peer || count != 1) {
        c->log("tls: expected exactly one peer certificate, got %d", count);
        return TLS_E_PEER_CERT;
    }
    char got_text[DEVID_TEXT_SIZE], want_text[DEVID_TEXT_SIZE];
    if (cert_devid(peer, c->peer) != 0) {
        log_ssl_errors(c->log, "tls: hashing peer certificate");
        return TLS_E_INTERNAL;
    }
    devid_format(c->peer, got_text);
    c->log("tls: peer Device ID %s", got_text);
    if (c->check == TLS_CHECK_PIN) {
        devid_format(c->expected, want_text);
        if (CRYPTO_memcmp(c->peer, c->expected, DEVID_BYTES) != 0) {
            c->log("tls: expected Device ID %s; refusing the peer", want_text);
            return TLS_E_PEER_ID;
        }
        c->log("tls: peer Device ID matches the expected one");
    }
    c->stage = ST_UP;
    return TLS_OK;
}

void tls_peer_devid(const tls_conn *c, unsigned char out[DEVID_BYTES])
{
    memcpy(out, c->peer, DEVID_BYTES);
}

int tls_socket(const tls_conn *c)
{
    return c->sb.sock;
}

int tls_want(const tls_conn *c, int want_read, int *sock, int *rd, int *wr)
{
    *sock = c->sb.sock;
    *rd = *wr = 0;
    if (c->stage == ST_TCP) {
        *wr = 1;
        return 0;
    }
    if (c->stage == ST_HANDSHAKE) {
        if (c->hs_want == SSL_ERROR_WANT_WRITE)
            *wr = 1;
        else
            *rd = 1;
        return 0;
    }
    if (want_read && SSL_has_pending(c->ssl))
        return 1; /* decrypted bytes are waiting inside the TLS layer */
    *rd = want_read || (c->out_len > 0 && c->w_want == SSL_ERROR_WANT_READ);
    *wr = (c->out_len > 0 && c->w_want != SSL_ERROR_WANT_READ) || (want_read && c->r_want_write);
    return 0;
}

enum tls_result tls_connect(const tls_identity *id, const char *ipv4, unsigned port,
                            const unsigned char expected[DEVID_BYTES], net_ms deadline,
                            tls_log_fn log, tls_conn **out)
{
    tls_conn *c;
    *out = NULL;
    enum tls_result res = tls_start(id, ipv4, port, expected, log, &c);
    if (res != TLS_OK)
        return res;
    for (;;) {
        res = tls_connect_step(c);
        if (res == TLS_OK) {
            *out = c;
            return TLS_OK;
        }
        if (res != TLS_E_AGAIN)
            break;
        int sock, rd, wr, err = 0, tcp = c->stage == ST_TCP;
        tls_want(c, 0, &sock, &rd, &wr);
        enum net_status st = net_wait_rw(sock, rd, wr, deadline, &err);
        if (st == NET_OK)
            continue;
        res = st == NET_TIMEOUT ? TLS_E_TIMEOUT : st == NET_INTERRUPTED ? TLS_E_STOPPED : tcp ? TLS_E_CONNECT : TLS_E_IO;
        if (tcp)
            log("tcp: connect %s:%u failed (%s, error %d)", ipv4, port,
                st == NET_TIMEOUT ? "timeout" : st == NET_INTERRUPTED ? "stopped on request" : "error", err);
        else
            log("tls: handshake %s after %lld ms (socket error %d)",
                st == NET_TIMEOUT ? "timed out" : st == NET_INTERRUPTED ? "stopped on request" : "failed waiting",
                (long long)(net_now() - c->t0), err);
        break;
    }
    tls_abort(c);
    return res;
}

enum tls_result tls_read(tls_conn *c, void *buf, size_t len, size_t *got, int *closed,
                         net_ms deadline)
{
    *got = 0;
    *closed = 0;
    for (;;) {
        ERR_clear_error();
        if (SSL_read_ex(c->ssl, buf, len, got) == 1)
            return TLS_OK;
        int e = SSL_get_error(c->ssl, 0);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            enum tls_result r = wait_for(c, e, deadline);
            if (r == TLS_OK)
                continue;
            return r;
        }
        if (e == SSL_ERROR_ZERO_RETURN || c->sb.eof) {
            *closed = 1;
            c->sb.eof = 1;
            return TLS_E_IO;
        }
        log_ssl_errors(c->log, "tls: read");
        return TLS_E_IO;
    }
}

enum tls_result tls_queue(tls_conn *c, const void *buf, size_t len)
{
    if (len == 0)
        return TLS_OK;
    if (c->out_head > 0 && (c->out_len == 0 || c->out_head >= c->out_len)) {
        memmove(c->out, c->out + c->out_head, c->out_len);
        c->out_head = 0;
    }
    size_t need = c->out_head + c->out_len + len;
    if (need > c->out_cap) {
        size_t cap = c->out_cap ? c->out_cap : 65536;
        while (cap < need)
            cap *= 2;
        unsigned char *n = realloc(c->out, cap);
        if (!n)
            return TLS_E_INTERNAL;
        c->out = n;
        c->out_cap = cap;
    }
    if (c->out_len == 0)
        c->out_since = net_now();
    memcpy(c->out + c->out_head + c->out_len, buf, len);
    c->out_len += len;
    return TLS_OK;
}

/* One TLS record's worth is offered at a time, and the same amount again
 * after a wait. OpenSSL reports nothing as written while any part of the
 * length it was given is still on its way (after finishing a delayed
 * record it goes on to the next one instead of returning), so a longer
 * length offered to a slow socket would look like no progress at all
 * until its last byte had gone. */
#define TLS_WRITE_CHUNK 16384

enum tls_result tls_flush_some(tls_conn *c)
{
    while (c->out_len > 0) {
        size_t done = 0;
        size_t n = c->w_len ? c->w_len : c->out_len < TLS_WRITE_CHUNK ? c->out_len : TLS_WRITE_CHUNK;
        ERR_clear_error();
        if (SSL_write_ex(c->ssl, c->out + c->out_head, n, &done) == 1) {
            c->out_head += done;
            c->out_len -= done;
            c->out_since = net_now();
            c->w_want = 0;
            c->w_len = 0;
            continue;
        }
        int e = SSL_get_error(c->ssl, 0);
        if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
            c->w_want = e;
            c->w_len = n;
            return TLS_OK;
        }
        log_ssl_errors(c->log, "tls: write");
        return TLS_E_IO;
    }
    c->out_head = 0;
    c->w_want = 0;
    return TLS_OK;
}

size_t tls_pending(const tls_conn *c)
{
    return c->out_len;
}

net_ms tls_pending_since(const tls_conn *c)
{
    return c->out_since;
}

enum tls_result tls_read_some(tls_conn *c, void *buf, size_t len, size_t *got, int *closed)
{
    *got = 0;
    *closed = 0;
    c->r_want_write = 0;
    ERR_clear_error();
    if (SSL_read_ex(c->ssl, buf, len, got) == 1)
        return TLS_OK;
    *got = 0;
    int e = SSL_get_error(c->ssl, 0);
    if (e == SSL_ERROR_WANT_READ)
        return TLS_OK;
    if (e == SSL_ERROR_WANT_WRITE) {
        c->r_want_write = 1;
        return TLS_OK;
    }
    if (e == SSL_ERROR_ZERO_RETURN || c->sb.eof) {
        *closed = 1;
        c->sb.eof = 1;
        return TLS_E_IO;
    }
    log_ssl_errors(c->log, "tls: read");
    return TLS_E_IO;
}

enum tls_result tls_wait(tls_conn *c, int want_read, net_ms deadline)
{
    if (want_read && SSL_has_pending(c->ssl))
        return TLS_OK; /* decrypted bytes are waiting inside the TLS layer */
    int rd = want_read || (c->out_len > 0 && c->w_want == SSL_ERROR_WANT_READ);
    int wr = (c->out_len > 0 && c->w_want != SSL_ERROR_WANT_READ) || (want_read && c->r_want_write);
    if (!rd && !wr)
        rd = 1;
    int err = 0;
    enum net_status st = net_wait_rw(c->sb.sock, rd, wr, deadline, &err);
    if (st == NET_OK)
        return TLS_OK;
    if (st == NET_TIMEOUT)
        return TLS_E_TIMEOUT;
    if (st == NET_INTERRUPTED)
        return TLS_E_STOPPED;
    c->sb.err = err;
    return TLS_E_IO;
}

enum tls_result tls_flush(tls_conn *c, net_ms deadline)
{
    for (;;) {
        enum tls_result r = tls_flush_some(c);
        if (r != TLS_OK || c->out_len == 0)
            return r;
        r = wait_for(c, c->w_want ? c->w_want : SSL_ERROR_WANT_WRITE, deadline);
        if (r != TLS_OK)
            return r;
    }
}

enum tls_result tls_write(tls_conn *c, const void *buf, size_t len, net_ms deadline)
{
    enum tls_result r = tls_queue(c, buf, len);
    return r == TLS_OK ? tls_flush(c, deadline) : r;
}
