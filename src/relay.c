#include "relay.h"

#include <openssl/rand.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEADER 12
/* An attempt through a relay, from the first byte to the verified BEP
 * connection (Syncthing allows 10 s for the invitation and 10 s for
 * joining the session). */
#define DIAL_SECONDS 30
#define JOIN_SECONDS 20
/* The relay pings every minute; nothing for this long means it is gone
 * (Syncthing's client waits two minutes). */
#define MEMBER_SILENCE_SECONDS 150

/* ---- codec ---- */

static void put32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static uint32_t get32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

size_t relay_encode(int32_t type, const unsigned char *body, size_t len, unsigned char *out, size_t cap)
{
    if (len > RELAY_MAX_BODY || cap < HEADER + len)
        return 0;
    put32(out, RELAY_MAGIC);
    put32(out + 4, (uint32_t)type);
    put32(out + 8, (uint32_t)len);
    if (len)
        memcpy(out + HEADER, body, len);
    return HEADER + len;
}

long relay_frame(const unsigned char *buf, size_t len, int32_t *type, const unsigned char **body, size_t *blen)
{
    if (len < HEADER)
        return 0;
    if (get32(buf) != RELAY_MAGIC)
        return -1;
    int32_t l = (int32_t)get32(buf + 8);
    if (l < 0 || l > RELAY_MAX_BODY)
        return -1;
    if (len < HEADER + (size_t)l)
        return 0;
    *type = (int32_t)get32(buf + 4);
    *body = buf + HEADER;
    *blen = (size_t)l;
    return HEADER + l;
}

size_t relay_body_bytes(const unsigned char *data, size_t len, unsigned char *out, size_t cap)
{
    size_t padded = (len + 3) & ~(size_t)3;
    if (cap < 4 + padded)
        return 0;
    put32(out, (uint32_t)len);
    if (len)
        memcpy(out + 4, data, len);
    memset(out + 4 + len, 0, padded - len);
    return 4 + padded;
}

/* An XDR opaque or string of at most max bytes at *p (len bytes left). */
static int take_bytes(const unsigned char **p, size_t *left, size_t max, const unsigned char **data, size_t *dlen)
{
    if (*left < 4)
        return -1;
    uint32_t l = get32(*p);
    size_t padded = ((size_t)l + 3) & ~(size_t)3;
    if (l > max || *left - 4 < padded)
        return -1;
    *data = *p + 4;
    *dlen = l;
    *p += 4 + padded;
    *left -= 4 + padded;
    return 0;
}

static int take32(const unsigned char **p, size_t *left, uint32_t *v)
{
    if (*left < 4)
        return -1;
    *v = get32(*p);
    *p += 4;
    *left -= 4;
    return 0;
}

int relay_parse_response(const unsigned char *body, size_t len, int32_t *code, char *msg, size_t msg_cap)
{
    const unsigned char *p = body, *m;
    size_t left = len, ml;
    uint32_t c;
    if (take32(&p, &left, &c) != 0 || take_bytes(&p, &left, RELAY_MAX_BODY, &m, &ml) != 0)
        return -1;
    *code = (int32_t)c;
    if (msg_cap) {
        size_t n = 0;
        for (size_t i = 0; i < ml && n + 1 < msg_cap; i++)
            msg[n++] = m[i] >= 32 && m[i] < 127 ? (char)m[i] : '?';
        msg[n] = 0;
    }
    return 0;
}

int relay_parse_invitation(const unsigned char *body, size_t len, relay_invitation *out)
{
    const unsigned char *p = body, *from, *key, *addr;
    size_t left = len, fl, kl, al;
    uint32_t port, server;
    memset(out, 0, sizeof *out);
    if (take_bytes(&p, &left, 32, &from, &fl) != 0 || take_bytes(&p, &left, 32, &key, &kl) != 0
        || take_bytes(&p, &left, 32, &addr, &al) != 0 || take32(&p, &left, &port) != 0
        || take32(&p, &left, &server) != 0)
        return -1;
    if (fl != DEVID_BYTES || kl == 0 || port > 65535)
        return -1;
    memcpy(out->from, from, DEVID_BYTES);
    memcpy(out->key, key, kl);
    out->key_len = kl;
    /* Only IPv4 (4 bytes, or IPv6's form of an IPv4 address); none or
     * the unspecified address mean the relay's own. */
    const unsigned char *a = NULL;
    static const unsigned char mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
    if (al == 4)
        a = addr;
    else if (al == 16 && memcmp(addr, mapped, 12) == 0)
        a = addr + 12;
    else if (al != 0)
        return -1;
    if (a && (a[0] | a[1] | a[2] | a[3]))
        snprintf(out->ip, sizeof out->ip, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
    out->port = port;
    out->server_socket = server != 0;
    return 0;
}

/* ---- addresses ---- */

/* The value of key in the query string q, decoded (%XX), into out. */
static int query_value(const char *q, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    while (q && *q) {
        const char *end = strchr(q, '&');
        size_t len = end ? (size_t)(end - q) : strlen(q);
        if (len > kl && strncmp(q, key, kl) == 0 && q[kl] == '=') {
            size_t n = 0;
            for (size_t i = kl + 1; i < len; i++) {
                char ch = q[i];
                if (ch == '%') {
                    unsigned v;
                    if (i + 2 >= len)
                        return -1;
                    char hex[3] = { q[i + 1], q[i + 2], 0 };
                    if (sscanf(hex, "%2x", &v) != 1)
                        return -1;
                    ch = (char)v;
                    i += 2;
                } else if (ch == '+') {
                    ch = ' ';
                }
                if (n + 1 >= cap)
                    return -1;
                out[n++] = ch;
            }
            out[n] = 0;
            return 1;
        }
        q = end ? end + 1 : NULL;
    }
    return 0;
}

int relay_parse_url(const char *url, relay_addr *out)
{
    memset(out, 0, sizeof *out);
    if (strlen(url) >= sizeof out->url || strncmp(url, "relay://", 8) != 0)
        return -1;
    snprintf(out->url, sizeof out->url, "%s", url);
    const char *host = url + 8;
    const char *colon = strchr(host, ':');
    const char *slash = strpbrk(host, "/?");
    if (!colon || (slash && colon > slash))
        return -1;
    size_t hl = (size_t)(colon - host);
    if (hl == 0 || hl >= sizeof out->ip)
        return -1;
    memcpy(out->ip, host, hl);
    out->ip[hl] = 0;
    if (!net_is_ipv4(out->ip))
        return -1;
    char *e;
    long port = strtol(colon + 1, &e, 10);
    if (e == colon + 1 || port < 1 || port > 65535 || (*e && *e != '/' && *e != '?'))
        return -1;
    out->port = (unsigned)port;
    const char *q = strchr(url, '?');
    char idtext[DEVID_TEXT_SIZE + 8];
    int r = query_value(q ? q + 1 : NULL, "id", idtext, sizeof idtext);
    if (r < 0)
        return -1;
    if (r == 1) {
        if (devid_parse(idtext, out->id) != 0)
            return -1;
        out->has_id = 1;
    }
    if (query_value(q ? q + 1 : NULL, "token", out->token, sizeof out->token) < 0)
        return -1;
    return 0;
}

/* ---- a TLS connection to a relay, carrying messages ---- */

typedef struct {
    tls_conn *c;
    unsigned char in[HEADER + RELAY_MAX_BODY + 64];
    size_t in_len;
    int closed;
} rconn;

static tls_conn *relay_tls(const tls_identity *id, const relay_addr *r, tls_log_fn log)
{
    tls_opts o;
    tls_conn *c = NULL;
    memset(&o, 0, sizeof o);
    o.ip = r->ip;
    o.port = r->port;
    o.sock = -1;
    o.alpn = "bep-relay";
    o.check = r->has_id ? TLS_CHECK_PIN : TLS_CHECK_DEVICE;
    o.expected = r->has_id ? r->id : NULL;
    return tls_start_opts(id, &o, log, &c) == TLS_OK ? c : NULL;
}

static int rc_send(rconn *rc, int32_t type, const unsigned char *body, size_t len)
{
    unsigned char buf[HEADER + RELAY_MAX_BODY];
    size_t n = relay_encode(type, body, len, buf, sizeof buf);
    return n && tls_queue(rc->c, buf, n) == TLS_OK && tls_flush_some(rc->c) == TLS_OK ? 0 : -1;
}

/* Reads what is there. 0, or -1 when the connection failed or closed
 * and nothing it sent is left to look at: a relay sends an invitation
 * and closes at once, and the invitation still counts. */
static int rc_fill(rconn *rc)
{
    if (rc->closed)
        return rc->in_len ? 0 : -1;
    for (;;) {
        size_t got = 0;
        int closed = 0;
        if (rc->in_len >= sizeof rc->in)
            return 0; /* a whole message fits; relay_frame decides */
        enum tls_result r = tls_read_some(rc->c, rc->in + rc->in_len, sizeof rc->in - rc->in_len, &got, &closed);
        if (r != TLS_OK) {
            rc->closed = 1;
            return rc->in_len ? 0 : -1;
        }
        if (got == 0)
            return 0;
        rc->in_len += got;
    }
}

/* The next whole message: 1 (type, body copied to body[], blen), 0 none
 * yet, -1 not a message. */
static int rc_next(rconn *rc, int32_t *type, unsigned char *body, size_t *blen)
{
    const unsigned char *b;
    long n = relay_frame(rc->in, rc->in_len, type, &b, blen);
    if (n <= 0)
        return (int)n;
    memcpy(body, b, *blen);
    memmove(rc->in, rc->in + n, rc->in_len - (size_t)n);
    rc->in_len -= (size_t)n;
    return 1;
}

static int rc_has_message(const rconn *rc)
{
    int32_t t;
    const unsigned char *b;
    size_t bl;
    return relay_frame(rc->in, rc->in_len, &t, &b, &bl) != 0;
}

/* ---- reaching a device ---- */

enum { D_RELAY = 0, D_ASK, D_TCP, D_JOIN, D_BEP, D_AGAIN };
/* A relay (strelaysrv) takes the two joins of a session one at a time
 * and refuses ("already connected") one that arrives while it is taking
 * the other; the refused key is used up, so the session is lost. Both
 * devices get their invitations at the same moment and would join at
 * the same moment: ours waits a little, a random 100 to 300 ms. */
#define JOIN_DELAY_MS 100
#define JOIN_DELAY_SPREAD_MS 200

struct relay_dial {
    const tls_identity *id;
    tls_log_fn log;
    int stage;
    relay_addr relay;
    unsigned char peer[DEVID_BYTES];
    rconn rc;
    relay_invitation inv;
    int sock;
    unsigned char out[64];
    size_t out_len, out_done;
    unsigned char in[HEADER + RELAY_MAX_BODY];
    size_t in_len, in_want;
    tls_conn *bep;
    net_ms deadline, again_at;
};

static relay_dial *dial_new(const tls_identity *id, tls_log_fn log)
{
    relay_dial *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->id = id;
    d->log = log;
    d->sock = -1;
    d->deadline = net_now() + DIAL_SECONDS * 1000;
    return d;
}

relay_dial *relay_dial_start(const tls_identity *id, const relay_addr *relay, const unsigned char peer[DEVID_BYTES],
                             tls_log_fn log)
{
    relay_dial *d = dial_new(id, log);
    if (!d)
        return NULL;
    d->relay = *relay;
    memcpy(d->peer, peer, DEVID_BYTES);
    log("relay: asking %s:%u for the device", relay->ip, relay->port);
    if (!(d->rc.c = relay_tls(id, relay, log))) {
        free(d);
        return NULL;
    }
    d->stage = D_RELAY;
    return d;
}

/* The join, after the pause (D_AGAIN). */
static void join_later(relay_dial *d)
{
    unsigned char r = 0;
    RAND_bytes(&r, 1);
    d->stage = D_AGAIN;
    d->again_at = net_now() + JOIN_DELAY_MS + (net_ms)r * JOIN_DELAY_SPREAD_MS / 255;
    d->deadline = net_now() + JOIN_SECONDS * 1000;
}

static int join_begin(relay_dial *d)
{
    const char *ip = d->inv.ip[0] ? d->inv.ip : d->relay.ip;
    int err = 0;
    d->log("relay: joining the session at %s:%u", ip, d->inv.port);
    enum net_status st = net_connect_start(ip, d->inv.port, &d->sock, &err);
    if (st != NET_OK && st != NET_AGAIN) {
        d->log("relay: cannot connect to the session (error %d)", err);
        d->sock = -1;
        return -1;
    }
    unsigned char body[40];
    size_t bl = relay_body_bytes(d->inv.key, d->inv.key_len, body, sizeof body);
    d->out_len = relay_encode(RELAY_JOIN_SESSION, body, bl, d->out, sizeof d->out);
    d->out_done = 0;
    d->in_len = 0;
    d->in_want = HEADER;
    d->stage = D_TCP;
    return 0;
}

relay_dial *relay_dial_invited(const tls_identity *id, const relay_invitation *inv, tls_log_fn log)
{
    relay_dial *d = dial_new(id, log);
    if (!d)
        return NULL;
    d->inv = *inv;
    memcpy(d->peer, inv->from, DEVID_BYTES);
    if (!inv->ip[0]) {
        /* An invitation from a relay we belong to always names its
         * address (relay_member fills it in). */
        free(d);
        return NULL;
    }
    snprintf(d->relay.ip, sizeof d->relay.ip, "%s", inv->ip);
    join_later(d);
    return d;
}

int relay_dial_step(relay_dial *d, net_ms now, tls_conn **conn, const char **why)
{
    *conn = NULL;
    if (now > d->deadline) {
        d->log("relay: no connection through the relay within the time allowed");
        *why = "RELAY_TIMEOUT";
        return -1;
    }
    if (d->stage == D_RELAY) {
        enum tls_result r = tls_connect_step(d->rc.c);
        if (r == TLS_E_AGAIN)
            return 0;
        if (r != TLS_OK) {
            *why = r == TLS_E_PEER_ID ? "RELAY_ID_MISMATCH" : "RELAY_UNREACHABLE";
            return -1;
        }
        unsigned char body[40];
        size_t bl = relay_body_bytes(d->peer, DEVID_BYTES, body, sizeof body);
        if (rc_send(&d->rc, RELAY_CONNECT, body, bl) != 0) {
            *why = "RELAY_UNREACHABLE";
            return -1;
        }
        d->stage = D_ASK;
    }
    if (d->stage == D_ASK) {
        int32_t type;
        unsigned char body[RELAY_MAX_BODY];
        size_t bl;
        if (tls_flush_some(d->rc.c) != TLS_OK || rc_fill(&d->rc) != 0) {
            d->log("relay: the relay closed the connection before answering");
            *why = "RELAY_UNREACHABLE";
            return -1;
        }
        int n = rc_next(&d->rc, &type, body, &bl);
        if (n == 0 && d->rc.closed) {
            d->log("relay: the relay closed the connection before answering");
            *why = "RELAY_UNREACHABLE";
            return -1;
        }
        if (n == 0)
            return 0;
        if (n < 0) {
            *why = "RELAY_PROTOCOL_ERROR";
            return -1;
        }
        if (type == RELAY_RESPONSE) {
            int32_t code = 0;
            char msg[80] = "";
            relay_parse_response(body, bl, &code, msg, sizeof msg);
            d->log("relay: the relay answers %d \"%s\"", (int)code, msg);
            *why = code == 1 ? "RELAY_DEVICE_NOT_THERE" : "RELAY_REFUSED";
            return -1;
        }
        if (type != RELAY_INVITATION || relay_parse_invitation(body, bl, &d->inv) != 0) {
            d->log("relay: unexpected answer (message type %d)", (int)type);
            *why = "RELAY_PROTOCOL_ERROR";
            return -1;
        }
        if (memcmp(d->inv.from, d->peer, DEVID_BYTES) != 0) {
            d->log("relay: the invitation is for another device");
            *why = "RELAY_PROTOCOL_ERROR";
            return -1;
        }
        tls_abort(d->rc.c); /* done with the relay itself */
        d->rc.c = NULL;
        join_later(d);
    }
    if (d->stage == D_AGAIN) {
        if (now < d->again_at)
            return 0;
        if (join_begin(d) != 0) {
            *why = "RELAY_UNREACHABLE";
            return -1;
        }
    }
    if (d->stage == D_TCP) {
        int err = 0;
        enum net_status st = net_connect_finish(d->sock, &err);
        if (st == NET_AGAIN)
            return 0;
        if (st != NET_OK) {
            d->log("relay: cannot connect to the session (error %d)", err);
            *why = "RELAY_UNREACHABLE";
            return -1;
        }
        d->stage = D_JOIN;
    }
    if (d->stage == D_JOIN) {
        int err = 0;
        while (d->out_done < d->out_len) {
            size_t n = 0;
            enum net_status st = net_send(d->sock, d->out + d->out_done, d->out_len - d->out_done, &n, &err);
            if (st == NET_AGAIN)
                return 0;
            if (st != NET_OK) {
                *why = "RELAY_UNREACHABLE";
                return -1;
            }
            d->out_done += n;
        }
        /* Exactly the Response: what follows it is the peer's TLS. */
        while (d->in_len < d->in_want) {
            size_t n = 0;
            enum net_status st = net_recv(d->sock, d->in + d->in_len, d->in_want - d->in_len, &n, &err);
            if (st == NET_AGAIN)
                return 0;
            if (st != NET_OK) {
                d->log("relay: the session closed before it began");
                *why = "RELAY_UNREACHABLE";
                return -1;
            }
            d->in_len += n;
            if (d->in_len == HEADER && d->in_want == HEADER) {
                if (get32(d->in) != RELAY_MAGIC || (int32_t)get32(d->in + 8) < 0 || get32(d->in + 8) > RELAY_MAX_BODY) {
                    *why = "RELAY_PROTOCOL_ERROR";
                    return -1;
                }
                d->in_want = HEADER + get32(d->in + 8);
            }
        }
        int32_t code = -1;
        char msg[80] = "";
        if ((int32_t)get32(d->in + 4) != RELAY_RESPONSE
            || relay_parse_response(d->in + HEADER, d->in_len - HEADER, &code, msg, sizeof msg) != 0 || code != 0) {
            d->log("relay: the session was refused (%d \"%s\")", (int)code, msg);
            *why = "RELAY_REFUSED";
            return -1;
        }
        tls_opts o;
        memset(&o, 0, sizeof o);
        o.sock = d->sock;
        o.ip = d->inv.ip[0] ? d->inv.ip : d->relay.ip;
        o.port = d->inv.port;
        o.server = d->inv.server_socket;
        o.check = TLS_CHECK_PIN;
        o.expected = d->peer;
        d->sock = -1; /* the TLS connection's now, also on failure */
        if (tls_start_opts(d->id, &o, d->log, &d->bep) != TLS_OK) {
            *why = "INTERNAL_ERROR";
            return -1;
        }
        d->log("relay: session joined; TLS with the device through the relay%s",
               o.server ? " (we are the server)" : "");
        d->stage = D_BEP;
    }
    /* D_BEP */
    enum tls_result r = tls_connect_step(d->bep);
    if (r == TLS_E_AGAIN)
        return 0;
    if (r != TLS_OK) {
        *why = tls_result_name(r);
        return -1;
    }
    *conn = d->bep;
    d->bep = NULL;
    free(d);
    return 1;
}

void relay_dial_want(const relay_dial *d, int *sock, int *rd, int *wr)
{
    *sock = -1;
    *rd = *wr = 0;
    switch (d->stage) {
    case D_RELAY:
    case D_ASK:
        tls_want(d->rc.c, d->stage == D_ASK, sock, rd, wr);
        break;
    case D_TCP:
        *sock = d->sock;
        *wr = 1;
        break;
    case D_AGAIN:
        break;
    case D_JOIN:
        *sock = d->sock;
        if (d->out_done < d->out_len)
            *wr = 1;
        else
            *rd = 1;
        break;
    default:
        tls_want(d->bep, 0, sock, rd, wr);
        break;
    }
}

net_ms relay_dial_deadline(const relay_dial *d)
{
    return d->stage == D_AGAIN && d->again_at < d->deadline ? d->again_at : d->deadline;
}

void relay_dial_abort(relay_dial *d)
{
    if (!d)
        return;
    if (d->rc.c)
        tls_abort(d->rc.c);
    if (d->bep)
        tls_abort(d->bep);
    net_close(d->sock);
    free(d);
}

/* ---- belonging to a relay ---- */

enum { M_TLS = 0, M_JOIN, M_UP };

struct relay_member {
    tls_log_fn log;
    relay_addr relay;
    rconn rc;
    int stage;
    net_ms last_heard, deadline;
};

relay_member *relay_member_start(const tls_identity *id, const relay_addr *relay, tls_log_fn log)
{
    relay_member *m = calloc(1, sizeof *m);
    if (!m)
        return NULL;
    m->log = log;
    m->relay = *relay;
    log("relay: joining %s:%u", relay->ip, relay->port);
    if (!(m->rc.c = relay_tls(id, relay, log))) {
        free(m);
        return NULL;
    }
    m->stage = M_TLS;
    m->last_heard = net_now();
    m->deadline = m->last_heard + DIAL_SECONDS * 1000;
    return m;
}

int relay_member_step(relay_member *m, net_ms now, relay_invitation *inv, const char **why)
{
    if (m->stage == M_TLS) {
        enum tls_result r = tls_connect_step(m->rc.c);
        if (r == TLS_E_AGAIN) {
            if (now <= m->deadline)
                return RELAY_EV_NONE;
            m->log("relay: no connection within %d s", DIAL_SECONDS);
            *why = "RELAY_TIMEOUT";
            return RELAY_EV_ENDED;
        }
        if (r != TLS_OK) {
            *why = r == TLS_E_PEER_ID ? "RELAY_ID_MISMATCH" : "RELAY_UNREACHABLE";
            return RELAY_EV_ENDED;
        }
        unsigned char body[120];
        size_t bl = relay_body_bytes((const unsigned char *)m->relay.token, strlen(m->relay.token), body, sizeof body);
        if (rc_send(&m->rc, RELAY_JOIN_RELAY, body, bl) != 0) {
            *why = "RELAY_UNREACHABLE";
            return RELAY_EV_ENDED;
        }
        m->stage = M_JOIN;
        m->last_heard = now;
    }
    if (tls_flush_some(m->rc.c) != TLS_OK || rc_fill(&m->rc) != 0) {
        m->log("relay: the relay closed the connection");
        *why = "RELAY_CLOSED";
        return RELAY_EV_ENDED;
    }
    for (;;) {
        int32_t type;
        unsigned char body[RELAY_MAX_BODY];
        size_t bl;
        int n = rc_next(&m->rc, &type, body, &bl);
        if (n == 0 && m->rc.closed) {
            m->log("relay: the relay closed the connection");
            *why = "RELAY_CLOSED";
            return RELAY_EV_ENDED;
        }
        if (n == 0)
            break;
        if (n < 0) {
            *why = "RELAY_PROTOCOL_ERROR";
            return RELAY_EV_ENDED;
        }
        m->last_heard = now;
        if (type == RELAY_FULL) {
            m->log("relay: the relay is full");
            *why = "RELAY_FULL";
            return RELAY_EV_ENDED;
        }
        if (m->stage == M_JOIN) {
            int32_t code = -1;
            char msg[80] = "";
            if (type != RELAY_RESPONSE || relay_parse_response(body, bl, &code, msg, sizeof msg) != 0 || code != 0) {
                m->log("relay: not taken (message type %d, code %d \"%s\")", (int)type, (int)code, msg);
                *why = "RELAY_REFUSED";
                return RELAY_EV_ENDED;
            }
            m->log("relay: joined %s:%u", m->relay.ip, m->relay.port);
            m->stage = M_UP;
            return RELAY_EV_JOINED;
        }
        if (type == RELAY_PING) {
            if (rc_send(&m->rc, RELAY_PONG, NULL, 0) != 0) {
                *why = "RELAY_CLOSED";
                return RELAY_EV_ENDED;
            }
            continue;
        }
        if (type == RELAY_PONG)
            continue;
        if (type == RELAY_INVITATION && relay_parse_invitation(body, bl, inv) == 0) {
            if (!inv->ip[0])
                snprintf(inv->ip, sizeof inv->ip, "%s", m->relay.ip);
            return RELAY_EV_INVITATION;
        }
        m->log("relay: unexpected message type %d", (int)type);
        *why = "RELAY_PROTOCOL_ERROR";
        return RELAY_EV_ENDED;
    }
    if (now - m->last_heard > MEMBER_SILENCE_SECONDS * 1000L) {
        m->log("relay: nothing from the relay for %d s", MEMBER_SILENCE_SECONDS);
        *why = "RELAY_TIMEOUT";
        return RELAY_EV_ENDED;
    }
    return RELAY_EV_NONE;
}

int relay_member_want(const relay_member *m, int *sock, int *rd, int *wr)
{
    if (rc_has_message(&m->rc) || m->rc.closed)
        return 1;
    return tls_want(m->rc.c, m->stage != M_TLS, sock, rd, wr);
}

net_ms relay_member_deadline(const relay_member *m)
{
    return m->stage == M_TLS ? m->deadline : m->last_heard + MEMBER_SILENCE_SECONDS * 1000L + 1000;
}

const relay_addr *relay_member_addr(const relay_member *m)
{
    return &m->relay;
}

void relay_member_close(relay_member *m)
{
    if (!m)
        return;
    if (m->rc.c)
        tls_close(m->rc.c, net_now() + 500);
    free(m);
}
