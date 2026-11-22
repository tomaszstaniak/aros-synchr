#include "reach.h"

#include "disco.h"
#include "https.h"
#include "relay.h"

#include <openssl/rand.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_INCOMING 8
#define HANDSHAKE_SECONDS 20
#define MAX_INVITED 4
#define MAX_LOOKUPS 32
#define LOOKUP_SECONDS 300
#define LOOKUP_MAX_PAUSE_SECONDS 900
#define ANNOUNCE_SECONDS 1800
#define ANNOUNCE_RETRY_SECONDS 300
/* After the addresses changed (a relay joined or lost), as Syncthing. */
#define ANNOUNCE_CHANGE_SECONDS 2
#define LOCAL_SECONDS 30
#define POOL_KEEP_SECONDS 3600
#define POOL_MAX 500
/* Relays tried at once; the first to accept the TCP connection is used
 * (nearest, roughly, as Syncthing chooses by latency). */
#define RACE_COUNT 8
#define RACE_SECONDS 5
#define RELAY_RETRY_MAX_SECONDS 300

struct incoming {
    tls_conn *c;
    char from[NET_IPV4_TEXT];
    net_ms deadline;
};

struct invited {
    relay_dial *d;
    unsigned char peer[DEVID_BYTES];
    char from[NET_IPV4_TEXT];
};

struct lookup {
    int used;
    unsigned char peer[DEVID_BYTES];
    net_ms next;
    https_req *req;
};

struct reach {
    reach_options o;
    char lookup_url[300], announce_url[300], pool_url[300], fixed_relay[RELAY_URL_MAX];
    const tls_identity *id;
    unsigned char me[DEVID_BYTES];
    tls_log_fn log;
    reach_callbacks cb;
    int lsock;
    struct incoming in[MAX_INCOMING];
    struct invited inv[MAX_INVITED];

    relay_member *member;
    int joined, relay_failures;
    char joined_url[RELAY_URL_MAX];
    relay_addr *pool;
    int npool;
    net_ms pool_at, relay_next;
    https_req *pool_req;
    int race_sock[RACE_COUNT], race_idx[RACE_COUNT], racing;
    net_ms race_deadline;

    https_req *ann;
    net_ms ann_next;
    int announced; /* -1 not tried, 0 failed, 1 done */

    struct lookup lk[MAX_LOOKUPS];

    net_ms local_next;
    int64_t instance;
    char unknown[DEVID_TEXT_SIZE + NET_IPV4_TEXT + 2];
    int again;
};

static long relay_backoff(int failures)
{
    long s = 10;
    while (failures-- > 0 && s < RELAY_RETRY_MAX_SECONDS)
        s *= 2;
    return s > RELAY_RETRY_MAX_SECONDS ? RELAY_RETRY_MAX_SECONDS : s;
}

static void copy_url(char *dst, size_t cap, const char *src, const char *dflt)
{
    snprintf(dst, cap, "%s", src && *src ? src : dflt);
}

reach *reach_new(const reach_options *o, const tls_identity *id, tls_log_fn log, const reach_callbacks *cb)
{
    reach *r = calloc(1, sizeof *r);
    if (!r)
        return NULL;
    r->o = *o;
    copy_url(r->lookup_url, sizeof r->lookup_url, o->lookup_url, REACH_LOOKUP_URL);
    copy_url(r->announce_url, sizeof r->announce_url, o->announce_url, REACH_ANNOUNCE_URL);
    copy_url(r->pool_url, sizeof r->pool_url, o->pool_url, REACH_POOL_URL);
    copy_url(r->fixed_relay, sizeof r->fixed_relay, o->fixed_relay, "");
    r->id = id;
    tls_identity_devid(id, r->me);
    r->log = log;
    r->cb = *cb;
    r->lsock = -1;
    r->announced = -1;
    for (int i = 0; i < RACE_COUNT; i++)
        r->race_sock[i] = -1;
    if (RAND_bytes((unsigned char *)&r->instance, sizeof r->instance) != 1)
        r->instance = (int64_t)net_now();
    if (o->listen_port) {
        int err = 0;
        if (net_tcp_listen(o->listen_port, &r->lsock, &err) == NET_OK) {
            log("listen: waiting for connections on TCP %u", o->listen_port);
        } else {
            log("listen: cannot listen on TCP %u (error %d); devices can reach us only through a relay", o->listen_port,
                err);
            r->lsock = -1;
        }
    }
    net_ms now = net_now();
    /* A few seconds first, for the relay to be joined: one announcement
     * then carries both. */
    r->ann_next = now + 5000;
    r->local_next = now;
    return r;
}

/* ---- connections that come in ---- */

static void accept_some(reach *r, net_ms now)
{
    for (int k = 0; k < 4 && r->lsock >= 0; k++) {
        int s = -1, err = 0;
        char from[NET_IPV4_TEXT];
        enum net_status st = net_accept(r->lsock, &s, from, &err);
        if (st == NET_AGAIN)
            return;
        if (st != NET_OK) {
            r->log("listen: accept failed (error %d)", err);
            return;
        }
        int slot = -1;
        for (int i = 0; i < MAX_INCOMING && slot < 0; i++)
            if (!r->in[i].c)
                slot = i;
        if (slot < 0) {
            r->log("listen: a connection from %s turned away: too many handshakes at once", from);
            net_close(s);
            continue;
        }
        tls_opts o;
        memset(&o, 0, sizeof o);
        o.sock = s;
        o.ip = from;
        o.server = 1;
        o.check = TLS_CHECK_DEVICE;
        r->log("listen: connection from %s", from);
        if (tls_start_opts(r->id, &o, r->log, &r->in[slot].c) != TLS_OK) {
            r->in[slot].c = NULL;
            continue;
        }
        snprintf(r->in[slot].from, sizeof r->in[slot].from, "%s", from);
        r->in[slot].deadline = now + HANDSHAKE_SECONDS * 1000;
    }
}

static void hand_over(reach *r, const unsigned char peer[DEVID_BYTES], tls_conn *c, const char *via, const char *from)
{
    char text[DEVID_TEXT_SIZE];
    devid_format(peer, text);
    if (r->cb.offer(r->cb.ctx, peer, c, via, from))
        return;
    if (!r->cb.known(r->cb.ctx, peer)) {
        r->log("listen: device %s at %s is not in the configuration; connection closed", text, from);
        snprintf(r->unknown, sizeof r->unknown, "%s@%s", text, from);
    } else {
        r->log("listen: device %s at %s already has a connection; this one closed", text, from);
    }
    tls_abort(c);
}

static void incoming_step(reach *r, net_ms now)
{
    for (int i = 0; i < MAX_INCOMING; i++) {
        struct incoming *in = &r->in[i];
        if (!in->c)
            continue;
        enum tls_result t = tls_connect_step(in->c);
        if (t == TLS_E_AGAIN && now < in->deadline)
            continue;
        if (t != TLS_OK) {
            r->log("listen: the connection from %s ended in the handshake: %s", in->from,
                   t == TLS_E_AGAIN ? "TIMEOUT" : tls_result_name(t));
            tls_abort(in->c);
            in->c = NULL;
            continue;
        }
        unsigned char peer[DEVID_BYTES];
        tls_peer_devid(in->c, peer);
        tls_conn *c = in->c;
        in->c = NULL;
        hand_over(r, peer, c, "incoming", in->from);
    }
    for (int i = 0; i < MAX_INVITED; i++) {
        struct invited *v = &r->inv[i];
        if (!v->d)
            continue;
        tls_conn *c = NULL;
        const char *why = NULL;
        int s = relay_dial_step(v->d, now, &c, &why);
        if (s == 0)
            continue;
        if (s < 0) {
            r->log("relay: the session a device asked for failed: %s", why);
            relay_dial_abort(v->d);
        }
        v->d = NULL;
        if (s > 0)
            hand_over(r, v->peer, c, "relay", v->from);
    }
}

/* ---- the relay ---- */

static void on_pool_url(void *ctx, const char *url)
{
    reach *r = ctx;
    if (r->npool < POOL_MAX && relay_parse_url(url, &r->pool[r->npool]) == 0)
        r->npool++;
}

static void race_end(reach *r)
{
    for (int i = 0; i < RACE_COUNT; i++) {
        net_close(r->race_sock[i]);
        r->race_sock[i] = -1;
    }
    r->racing = 0;
}

static void relay_lost(reach *r, net_ms now, const char *why)
{
    long wait = relay_backoff(r->relay_failures++);
    r->log("relay: %s; another relay in %ld s", why, wait);
    r->relay_next = now + wait * 1000;
    if (r->joined) {
        r->joined = 0;
        r->joined_url[0] = 0;
        r->ann_next = now + ANNOUNCE_CHANGE_SECONDS * 1000;
        r->local_next = now;
    }
}

static void member_begin(reach *r, const relay_addr *a, net_ms now)
{
    if (!(r->member = relay_member_start(r->id, a, r->log)))
        relay_lost(r, now, "cannot start");
}

static void relay_step(reach *r, net_ms now)
{
    if (!r->o.relays)
        return;
    if (r->member) {
        relay_invitation inv;
        const char *why = NULL;
        for (;;) {
            int ev = relay_member_step(r->member, now, &inv, &why);
            if (ev == RELAY_EV_NONE)
                break;
            if (ev == RELAY_EV_JOINED) {
                const relay_addr *a = relay_member_addr(r->member);
                char id[DEVID_TEXT_SIZE];
                devid_format(a->id, id);
                /* What is announced: only the id of the address (Syncthing
                 * keeps no other part either). */
                if (a->has_id)
                    snprintf(r->joined_url, sizeof r->joined_url, "relay://%s:%u/?id=%s", a->ip, a->port, id);
                else
                    snprintf(r->joined_url, sizeof r->joined_url, "relay://%s:%u/", a->ip, a->port);
                r->joined = 1;
                r->relay_failures = 0;
                r->ann_next = now + ANNOUNCE_CHANGE_SECONDS * 1000;
                r->local_next = now;
                continue;
            }
            if (ev == RELAY_EV_INVITATION) {
                char text[DEVID_TEXT_SIZE];
                devid_format(inv.from, text);
                if (!r->cb.known(r->cb.ctx, inv.from)) {
                    r->log("relay: device %s, not in the configuration, asks for a session; not taken", text);
                    snprintf(r->unknown, sizeof r->unknown, "%s@relay", text);
                    continue;
                }
                int slot = -1;
                for (int i = 0; i < MAX_INVITED && slot < 0; i++)
                    if (!r->inv[i].d)
                        slot = i;
                if (slot < 0) {
                    r->log("relay: device %s asks for a session; too many at once, not taken", text);
                    continue;
                }
                r->log("relay: device %s asks for a session", text);
                if ((r->inv[slot].d = relay_dial_invited(r->id, &inv, r->log)) != NULL) {
                    memcpy(r->inv[slot].peer, inv.from, DEVID_BYTES);
                    snprintf(r->inv[slot].from, sizeof r->inv[slot].from, "%s", inv.ip);
                }
                continue;
            }
            /* RELAY_EV_ENDED */
            relay_member_close(r->member);
            r->member = NULL;
            relay_lost(r, now, why ? why : "ended");
            return;
        }
        return;
    }

    if (r->racing) {
        int winner = -1;
        for (int i = 0; i < RACE_COUNT && winner < 0; i++) {
            if (r->race_sock[i] < 0)
                continue;
            int err = 0;
            enum net_status st = net_connect_finish(r->race_sock[i], &err);
            if (st == NET_OK)
                winner = r->race_idx[i];
            else if (st != NET_AGAIN) {
                net_close(r->race_sock[i]);
                r->race_sock[i] = -1;
            }
        }
        int open = 0;
        for (int i = 0; i < RACE_COUNT; i++)
            open += r->race_sock[i] >= 0;
        if (winner < 0 && open && now < r->race_deadline)
            return;
        race_end(r);
        if (winner < 0) {
            relay_lost(r, now, "none of the relays tried answered");
            return;
        }
        member_begin(r, &r->pool[winner], now);
        return;
    }

    if (r->pool_req) {
        int s = https_step(r->pool_req, now);
        if (s == 0)
            return;
        if (s > 0 && https_status(r->pool_req) == 200) {
            size_t len;
            const char *body = https_body(r->pool_req, &len);
            if (!r->pool)
                r->pool = calloc(POOL_MAX, sizeof *r->pool);
            r->npool = 0;
            if (r->pool)
                json_strings(body, len, "url", on_pool_url, r);
            r->pool_at = now;
            r->log("relay: %d relays in the public list", r->npool);
        } else if (s > 0) {
            r->log("relay: the list of relays: HTTP %d", https_status(r->pool_req));
        }
        https_free(r->pool_req);
        r->pool_req = NULL;
        if (r->npool == 0) {
            relay_lost(r, now, "no list of relays");
            return;
        }
    }

    if (now < r->relay_next)
        return;
    if (r->fixed_relay[0]) {
        relay_addr a;
        if (relay_parse_url(r->fixed_relay, &a) != 0) {
            r->log("relay: not a usable relay address: %s", r->fixed_relay);
            r->o.relays = 0;
            return;
        }
        member_begin(r, &a, now);
        return;
    }
    if (r->npool == 0 || now - r->pool_at > POOL_KEEP_SECONDS * 1000L) {
        if (!(r->pool_req = https_start(r->id, r->pool_url, "GET", NULL, r->log)))
            relay_lost(r, now, "cannot ask for the list of relays");
        return;
    }
    /* RACE_COUNT different relays at random. */
    int n = r->npool < RACE_COUNT ? r->npool : RACE_COUNT;
    for (int i = 0; i < n; i++) {
        unsigned v = 0;
        int idx;
        for (int tries = 0;; tries++) {
            RAND_bytes((unsigned char *)&v, sizeof v);
            idx = (int)(v % (unsigned)r->npool);
            int dup = 0;
            for (int k = 0; k < i; k++)
                dup |= r->race_idx[k] == idx;
            if (!dup || tries > 20)
                break;
        }
        int err = 0;
        r->race_idx[i] = idx;
        enum net_status st = net_connect_start(r->pool[idx].ip, r->pool[idx].port, &r->race_sock[i], &err);
        if (st != NET_OK && st != NET_AGAIN)
            r->race_sock[i] = -1;
    }
    r->racing = 1;
    r->race_deadline = now + RACE_SECONDS * 1000;
}

/* ---- announcements ---- */

static int our_addresses(const reach *r, char tcp[40], const char *list[2])
{
    int n = 0;
    if (r->lsock >= 0) {
        snprintf(tcp, 40, "tcp://0.0.0.0:%u", r->o.listen_port);
        list[n++] = tcp;
    }
    if (r->joined)
        list[n++] = r->joined_url;
    return n;
}

static void announce_step(reach *r, net_ms now)
{
    char tcp[40];
    const char *list[2];
    int n = our_addresses(r, tcp, list);

    if (r->o.local_announce && now >= r->local_next) {
        r->local_next = now + LOCAL_SECONDS * 1000;
        unsigned char pkt[DISCO_MAX_PACKET];
        int err = 0;
        size_t len = n ? disco_build(r->me, list, n, r->instance, pkt, sizeof pkt) : 0;
        if (len && disco_send(pkt, len, r->o.local_port, &err) != 0 && err)
            r->log("local: announcement not sent (error %d)", err);
    }

    if (!r->o.global)
        return;
    if (r->ann) {
        int s = https_step(r->ann, now);
        if (s == 0)
            return;
        int status = s > 0 ? https_status(r->ann) : 0;
        if (status >= 200 && status < 300) {
            long again = https_header_seconds(r->ann, "Reannounce-After");
            r->ann_next = now + (again > 0 ? again : ANNOUNCE_SECONDS) * 1000L;
            if (r->announced != 1)
                r->log("global: announced (%s%s%s)", n ? list[0] : "", n > 1 ? ", " : "", n > 1 ? list[1] : "");
            r->announced = 1;
        } else {
            long again = s > 0 ? https_header_seconds(r->ann, "Retry-After") : -1;
            if (s > 0)
                r->log("global: announcement refused: HTTP %d", status);
            r->ann_next = now + (again > 0 ? again : ANNOUNCE_RETRY_SECONDS) * 1000L;
            r->announced = 0;
        }
        https_free(r->ann);
        r->ann = NULL;
        return;
    }
    if (now < r->ann_next || n == 0)
        return;
    char body[RELAY_URL_MAX + 80];
    if (n == 2)
        snprintf(body, sizeof body, "{\"addresses\":[\"%s\",\"%s\"]}", list[0], list[1]);
    else
        snprintf(body, sizeof body, "{\"addresses\":[\"%s\"]}", list[0]);
    if (!(r->ann = https_start(r->id, r->announce_url, "POST", body, r->log))) {
        r->ann_next = now + ANNOUNCE_RETRY_SECONDS * 1000L;
        r->announced = 0;
    }
}

/* ---- lookups ---- */

typedef struct {
    const char *urls[16];
    char store[16][300];
    int n;
} found_list;

static void on_address(void *ctx, const char *url)
{
    found_list *f = ctx;
    if (f->n < 16 && strlen(url) < sizeof f->store[0]) {
        snprintf(f->store[f->n], sizeof f->store[0], "%s", url);
        f->urls[f->n] = f->store[f->n];
        f->n++;
    }
}

void reach_lookup(reach *r, const unsigned char peer[DEVID_BYTES])
{
    if (!r->o.global)
        return;
    struct lookup *l = NULL, *free_slot = NULL;
    for (int i = 0; i < MAX_LOOKUPS && !l; i++) {
        if (r->lk[i].used && memcmp(r->lk[i].peer, peer, DEVID_BYTES) == 0)
            l = &r->lk[i];
        else if (!r->lk[i].used && !free_slot)
            free_slot = &r->lk[i];
    }
    net_ms now = net_now();
    if (!l) {
        if (!free_slot)
            return;
        l = free_slot;
        l->used = 1;
        memcpy(l->peer, peer, DEVID_BYTES);
        l->next = 0;
    }
    if (l->req || now < l->next)
        return;
    char id[DEVID_TEXT_SIZE], url[400];
    devid_format(peer, id);
    snprintf(url, sizeof url, "%s%sdevice=%s", r->lookup_url, strchr(r->lookup_url, '?') ? "&" : "?", id);
    l->next = now + LOOKUP_SECONDS * 1000L;
    l->req = https_start(r->id, url, "GET", NULL, r->log);
    r->again = 1;
}

static void lookup_step(reach *r, net_ms now)
{
    for (int i = 0; i < MAX_LOOKUPS; i++) {
        struct lookup *l = &r->lk[i];
        if (!l->req)
            continue;
        int s = https_step(l->req, now);
        if (s == 0)
            continue;
        char id[DEVID_TEXT_SIZE];
        devid_format(l->peer, id);
        int status = s > 0 ? https_status(l->req) : 0;
        if (status == 200) {
            found_list f;
            size_t len;
            const char *body = https_body(l->req, &len);
            f.n = 0;
            json_strings(body, len, "addresses", on_address, &f);
            r->log("global: device %.7s has %d address(es)", id, f.n);
            r->cb.found(r->cb.ctx, l->peer, f.urls, f.n);
        } else if (s > 0) {
            long again = https_header_seconds(l->req, "Retry-After");
            r->log("global: device %.7s: %s", id, status == 404 ? "not known there" : "lookup refused");
            if (status != 404)
                r->log("global: HTTP %d", status);
            /* The server's pause, but no longer than a quarter of an hour:
             * a device set up just now announces itself within minutes. */
            if (again > 0)
                l->next = now + (again < LOOKUP_MAX_PAUSE_SECONDS ? again : LOOKUP_MAX_PAUSE_SECONDS) * 1000L;
        }
        https_free(l->req);
        l->req = NULL;
    }
}

/* ---- the whole ---- */

void reach_step(reach *r, net_ms now, net_ms *wake)
{
    r->again = 0;
    accept_some(r, now);
    incoming_step(r, now);
    relay_step(r, now);
    announce_step(r, now);
    lookup_step(r, now);

    net_ms w = *wake;
#define EARLIER(t) do { net_ms t_ = (t); if (t_ < w) w = t_; } while (0)
    for (int i = 0; i < MAX_INCOMING; i++)
        if (r->in[i].c)
            EARLIER(r->in[i].deadline);
    for (int i = 0; i < MAX_INVITED; i++)
        if (r->inv[i].d)
            EARLIER(relay_dial_deadline(r->inv[i].d));
    if (r->o.relays) {
        if (r->member)
            EARLIER(relay_member_deadline(r->member));
        else if (r->racing)
            EARLIER(r->race_deadline);
        else if (r->pool_req)
            EARLIER(https_deadline(r->pool_req));
        else
            EARLIER(r->relay_next);
    }
    if (r->o.local_announce)
        EARLIER(r->local_next);
    if (r->o.global)
        EARLIER(r->ann ? https_deadline(r->ann) : r->ann_next);
    for (int i = 0; i < MAX_LOOKUPS; i++)
        if (r->lk[i].req)
            EARLIER(https_deadline(r->lk[i].req));
#undef EARLIER
    if (w < now)
        w = now;
    *wake = w;
}

int reach_want(const reach *r, net_wait_item *it)
{
    int n = 0, sock, rd, wr;
#define ADD(s, a, b) do { if (n < REACH_MAX_ITEMS) { it[n].sock = (s); it[n].rd = (a); it[n].wr = (b); it[n].ready = 0; n++; } } while (0)
    if (r->again)
        return -1;
    if (r->lsock >= 0)
        ADD(r->lsock, 1, 0);
    for (int i = 0; i < MAX_INCOMING; i++)
        if (r->in[i].c) {
            if (tls_want(r->in[i].c, 0, &sock, &rd, &wr))
                return -1;
            ADD(sock, rd, wr);
        }
    for (int i = 0; i < MAX_INVITED; i++)
        if (r->inv[i].d) {
            relay_dial_want(r->inv[i].d, &sock, &rd, &wr);
            ADD(sock, rd, wr);
        }
    if (r->member) {
        if (relay_member_want(r->member, &sock, &rd, &wr))
            return -1;
        ADD(sock, rd, wr);
    }
    for (int i = 0; i < RACE_COUNT; i++)
        if (r->race_sock[i] >= 0)
            ADD(r->race_sock[i], 0, 1);
    const https_req *reqs[2] = { r->pool_req, r->ann };
    for (int i = 0; i < 2; i++)
        if (reqs[i]) {
            if (https_want(reqs[i], &sock, &rd, &wr))
                return -1;
            ADD(sock, rd, wr);
        }
    for (int i = 0; i < MAX_LOOKUPS; i++)
        if (r->lk[i].req) {
            if (https_want(r->lk[i].req, &sock, &rd, &wr))
                return -1;
            ADD(sock, rd, wr);
        }
#undef ADD
    return n;
}

void reach_status(const reach *r, char *out, size_t n)
{
    char port[12] = "-";
    if (r->lsock >= 0)
        snprintf(port, sizeof port, "%u", r->o.listen_port);
    snprintf(out, n, "listen=%s relay=%s announced=%s unknown=%s", port, r->joined ? r->joined_url : "-",
             r->announced < 0 ? "-" : r->announced ? "yes" : "no", r->unknown[0] ? r->unknown : "-");
}

void reach_free(reach *r)
{
    if (!r)
        return;
    for (int i = 0; i < MAX_INCOMING; i++)
        if (r->in[i].c)
            tls_abort(r->in[i].c);
    for (int i = 0; i < MAX_INVITED; i++)
        relay_dial_abort(r->inv[i].d);
    relay_member_close(r->member);
    race_end(r);
    https_free(r->pool_req);
    https_free(r->ann);
    for (int i = 0; i < MAX_LOOKUPS; i++)
        https_free(r->lk[i].req);
    net_close(r->lsock);
    free(r->pool);
    free(r);
}
