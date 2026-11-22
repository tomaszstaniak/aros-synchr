/* TLS probe for aros-syncthing: identity, RNG seeding and a TLS 1.3
 * connection to a Syncthing device with Device ID pinning. No BEP: the
 * probe only reads what the peer sends first (its Hello) and closes.
 *
 * Usage:
 *   tlsprobe LOGFILE create DIR
 *   tlsprobe LOGFILE show DIR
 *   tlsprobe LOGFILE connect DIR IPV4 PORT PEER-DEVICE-ID [SECONDS]
 *   tlsprobe LOGFILE fail-jitter-init DIR        (test build instrumentation)
 *   tlsprobe LOGFILE no-jitter-select            (draw without selecting JITTER)
 *
 * The last line of the log is "RESULT OK <what>" or "RESULT ERROR <code>". */
#include "../../src/devid.h"
#include "../../src/net.h"
#include "../../src/tls.h"

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/rand.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ST_TARGET
#define ST_TARGET "host"
#endif

static FILE *logf;
static net_ms log_t0;

static void plog(const char *fmt, ...)
{
    va_list ap;
    char line[600];
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    long long t = (long long)(net_now() - log_t0);
    if (logf) {
        fprintf(logf, "[%6lld ms] %s\n", t, line);
        fflush(logf);
    }
    printf("[%6lld ms] %s\n", t, line);
    fflush(stdout);
}

#ifdef ST_USE_JITTER
/* Counters on the jitterentropy entry points that OpenSSL's JITTER seed
 * source calls, linked with -Wl,--wrap. They show that the DRBG really
 * drew from jitterentropy, which a successful configuration call alone
 * does not show. fail_init makes initialisation report "timer service not
 * available" (jitterentropy's ENOTIME, 1) without calling the library. */
#include <sys/types.h>
struct rand_data;
int __real_jent_entropy_init_ex(unsigned int osr, unsigned int flags);
ssize_t __real_jent_read_entropy(struct rand_data *ec, char *data, size_t len);

static int fail_init;
static unsigned init_calls, read_calls;
static int last_init_rc = -1000;
static unsigned last_init_flags;
static unsigned long long read_bytes;
static net_ms init_ms, read_ms;

int __wrap_jent_entropy_init_ex(unsigned int osr, unsigned int flags)
{
    init_calls++;
    last_init_flags = flags;
    if (fail_init)
        return last_init_rc = 1;
    net_ms t = net_now();
    last_init_rc = __real_jent_entropy_init_ex(osr, flags);
    init_ms += net_now() - t;
    return last_init_rc;
}

ssize_t __wrap_jent_read_entropy(struct rand_data *ec, char *data, size_t len)
{
    read_calls++;
    net_ms t = net_now();
    ssize_t r = __real_jent_read_entropy(ec, data, len);
    read_ms += net_now() - t;
    if (r > 0)
        read_bytes += (unsigned long long)r;
    return r;
}

static void log_jitter(void)
{
    plog("jitter: init calls %u (last rc %d, flags 0x%x, %lld ms), reads %u (%llu bytes, %lld ms)",
         init_calls, last_init_rc, last_init_flags, (long long)init_ms, read_calls, read_bytes,
         (long long)read_ms);
}
#else
static void log_jitter(void)
{
}
#endif

static int finish(int ok, const char *what)
{
    log_jitter();
    plog("RESULT %s %s", ok ? "OK" : "ERROR", what);
    if (logf)
        fclose(logf);
    return ok ? 0 : 10;
}

static int show(const char *dir)
{
    tls_identity *id = tls_identity_load(dir, plog);
    if (!id)
        return 0;
    unsigned char raw[DEVID_BYTES];
    char text[DEVID_TEXT_SIZE];
    tls_identity_devid(id, raw);
    devid_format(raw, text);
    plog("identity: Device ID %s", text);
    tls_identity_free(id);
    return 1;
}

static int connect_peer(const char *dir, const char *ip, const char *port_s, const char *peer_s,
                        const char *secs_s)
{
    unsigned char expected[DEVID_BYTES];
    if (devid_parse(peer_s, expected) != 0) {
        plog("connect: \"%s\" is not a valid Device ID", peer_s);
        return finish(0, "BAD_ARGUMENT");
    }
    long secs = secs_s ? strtol(secs_s, NULL, 10) : 30;
    long port = strtol(port_s, NULL, 10);
    if (secs < 1 || secs > 600 || port < 1 || port > 65535) {
        plog("connect: bad port or time limit");
        return finish(0, "BAD_ARGUMENT");
    }
    if (!show(dir))
        return finish(0, "IDENTITY");
    tls_identity *id = tls_identity_load(dir, plog);
    if (!id)
        return finish(0, "IDENTITY");

    /* One deadline for everything, never extended by progress. */
    net_ms deadline = net_now() + secs * 1000;
    tls_conn *c = NULL;
    enum tls_result r = tls_connect(id, ip, (unsigned)port, expected, deadline, plog, &c);
    tls_identity_free(id);
    if (r != TLS_OK)
        return finish(0, tls_result_name(r));

    /* The peer starts the BEP exchange with its Hello: 4-byte magic
     * 0x2EA7D90B. Reading it shows that application data flows through
     * our BIO after the handshake; we send nothing and close. */
    unsigned char buf[64];
    size_t have = 0;
    int closed = 0;
    while (have < 4) {
        size_t got = 0;
        r = tls_read(c, buf + have, sizeof buf - have, &got, &closed, deadline);
        if (r != TLS_OK)
            break;
        have += got;
    }
    if (have >= 4)
        plog("bep: first bytes from peer %02x%02x%02x%02x (%s)", buf[0], buf[1], buf[2], buf[3],
             memcmp(buf, "\x2e\xa7\xd9\x0b", 4) == 0 ? "Hello magic" : "unexpected");
    else
        plog("bep: no Hello from peer (%s%s)", tls_result_name(r), closed ? ", peer closed" : "");
    tls_close(c, net_now() + 2000);
    plog("connect: connection closed and released");
    return finish(have >= 4 && memcmp(buf, "\x2e\xa7\xd9\x0b", 4) == 0, have >= 4 ? "PEER_VERIFIED" : "NO_HELLO");
}

int main(int argc, char **argv)
{
    log_t0 = net_now();
    if (argc < 3) {
        fprintf(stderr, "usage: see the comment at the top of tlsprobe.c\n");
        return 20;
    }
    logf = fopen(argv[1], "w");
    if (!logf) {
        fprintf(stderr, "cannot open log %s\n", argv[1]);
        return 20;
    }
    const char *cmd = argv[2];
    plog("tlsprobe target %s, %s", ST_TARGET, OpenSSL_version(OPENSSL_VERSION));
    plog("command %s", cmd);
    if (net_init() != 0)
        return finish(0, "NET_INIT");

    if (strcmp(cmd, "no-jitter-select") == 0) {
        /* Deliberately skips tls_rng_init(): with --with-rand-seed=none
         * there must be no source to fall back to, so this must fail. */
        unsigned char b[32];
        int ok = RAND_bytes(b, sizeof b) == 1;
        if (!ok) {
            unsigned long e = ERR_peek_error();
            char msg[256];
            ERR_error_string_n(e, msg, sizeof msg);
            plog("rng: draw without a selected source failed as required: %s", msg);
        } else {
            plog("rng: draw without a selected source SUCCEEDED - another source is active");
        }
        return finish(!ok, ok ? "UNEXPECTED_SEED_SOURCE" : "NO_FALLBACK_SOURCE");
    }

#ifdef ST_USE_JITTER
    if (strcmp(cmd, "fail-jitter-init") == 0)
        fail_init = 1;
#endif
    net_ms t = net_now();
    if (tls_rng_init(plog) != 0) {
        plog("rng: unusable after %lld ms; no key is generated, no connection made",
             (long long)(net_now() - t));
        return finish(0, "RNG_FAILED");
    }
    plog("rng: ready in %lld ms", (long long)(net_now() - t));

    if (strcmp(cmd, "create") == 0 || strcmp(cmd, "fail-jitter-init") == 0) {
        if (argc < 4)
            return finish(0, "BAD_ARGUMENT");
        t = net_now();
        if (tls_identity_create(argv[3], plog) != 0)
            return finish(0, "IDENTITY_CREATE");
        plog("identity: created in %lld ms", (long long)(net_now() - t));
        return finish(show(argv[3]), "IDENTITY_CREATED");
    }
    if (strcmp(cmd, "show") == 0 && argc >= 4)
        return finish(show(argv[3]), "IDENTITY_LOADED");
    if (strcmp(cmd, "connect") == 0 && argc >= 7)
        return connect_peer(argv[3], argv[4], argv[5], argv[6], argc > 7 ? argv[7] : NULL);
    return finish(0, "BAD_ARGUMENT");
}
