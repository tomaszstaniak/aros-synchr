#include "engine.h"

#include "ctl.h"
#include "ctlport.h"
#include "disco.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>

#ifdef __AROS__
#include <dos/dos.h>
#include <proto/exec.h>
#endif

static volatile sig_atomic_t stop_flag;

static void on_stop_signal(int sig)
{
    (void)sig;
    stop_flag = 1;
}

void engine_stop_init(void)
{
    signal(SIGINT, on_stop_signal);
#ifndef __AROS__
    signal(SIGTERM, on_stop_signal);
    signal(SIGPIPE, SIG_IGN); /* a control client that went away must not end the engine */
#endif
}

int engine_stop_requested(void)
{
#ifdef __AROS__
    /* Ctrl-C arrives as a task signal. It is taken here, so that the C
     * library does not end the program in the middle of a write. */
    if (SetSignal(0L, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C)
        stop_flag = 1;
#endif
    return stop_flag != 0;
}

static void request_stop(void)
{
    stop_flag = 1;
}

static int on_command(void *ctx, const char *line, char *out, size_t n)
{
    return ctl_command(ctx, line, out, n);
}

typedef struct {
    peerlink **links;
    size_t n;
    int relays; /* relay addresses from global discovery are used */
} found_context;

/* An announcement (disco.h): the address of the device it names, if it
 * is one of ours. */
static void on_found(void *ctx, const disco_found *f)
{
    found_context *fc = ctx;
    for (size_t i = 0; i < fc->n; i++)
        if (memcmp(link_peer_id(fc->links[i]), f->id, DEVID_BYTES) == 0)
            link_set_address(fc->links[i], f->ip, f->port);
}

static int find_link(const found_context *fc, const unsigned char peer[DEVID_BYTES])
{
    for (size_t i = 0; i < fc->n; i++)
        if (memcmp(link_peer_id(fc->links[i]), peer, DEVID_BYTES) == 0)
            return (int)i;
    return -1;
}

static int on_known(void *ctx, const unsigned char peer[DEVID_BYTES])
{
    return find_link(ctx, peer) >= 0;
}

static int on_offer(void *ctx, const unsigned char peer[DEVID_BYTES], tls_conn *c, const char *via, const char *from)
{
    found_context *fc = ctx;
    int i = find_link(fc, peer);
    return i >= 0 && link_offer(fc->links[i], c, via, from);
}

static void on_global(void *ctx, const unsigned char peer[DEVID_BYTES], const char *const *urls, int n)
{
    found_context *fc = ctx;
    int i = find_link(fc, peer);
    const char *keep[16];
    int k = 0;
    for (int j = 0; j < n && k < 16; j++)
        if (fc->relays || strncmp(urls[j], "relay://", 8) != 0)
            keep[k++] = urls[j];
    if (i >= 0)
        link_set_global(fc->links[i], keep, k);
}

/* How often the loop looks for a stop request while nothing happens. */
#define STOP_POLL_MS 1000

int engine_run(const engine_config *cfg, const tls_identity *id, const char **result)
{
    size_t n = cfg->ndevices;
    peerlink **links = calloc(n ? n : 1, sizeof *links);
    /* One item per link, and one more for the discovery socket. */
    net_wait_item *items = calloc(n + 1 + REACH_MAX_ITEMS, sizeof *items);
    const char *fatal = NULL;
    if (!links || !items || n == 0)
        fatal = n == 0 ? "BAD_ARGUMENT" : "OUT_OF_MEMORY";
    for (size_t i = 0; !fatal && i < n; i++)
        if (!(links[i] = link_new(&cfg->devices[i], id)))
            fatal = "OUT_OF_MEMORY";

    ctl_context cc = { links, n, request_stop, NULL };
    int control = 0;
    if (!fatal) {
        char why[200] = "";
        int rc = ctlport_open(why, sizeof why);
        if (rc == 1) {
            cfg->log("engine: %s", why);
            if (cfg->ui)
                cfg->ui("Already running");
            fatal = "ALREADY_RUNNING";
        } else if (rc != 0) {
            cfg->log("engine: no control interface: %s", why);
        } else {
            control = 1;
            cfg->log("engine: control interface open");
        }
    }

    /* Local discovery: listened to when a device has no typed address,
     * and our own announcements are sent from the same socket. Without
     * it such a device can still be found globally, or connect to us. */
    found_context fc = { links, n, cfg->reach && cfg->reach->relays };
    int discovery = cfg->reach && cfg->reach->local_announce;
    for (size_t i = 0; !fatal && i < n && !discovery; i++)
        discovery = cfg->devices[i].ip == NULL;
    unsigned dport = 0;
    if (discovery) {
        char why[100] = "";
        unsigned port = 0;
        if (disco_open(&port, why, sizeof why) == 0) {
            dport = port;
            cfg->log("discovery: listening on UDP %u", port);
        } else {
            cfg->log("discovery: cannot listen on UDP %u (%s); nothing is found or announced on the local network",
                     port, why);
            discovery = 0;
        }
    }

    reach *net = NULL;
    if (!fatal && cfg->reach) {
        reach_options ro = *cfg->reach;
        if (!discovery)
            ro.local_announce = 0;
        ro.local_port = dport; /* where the announcements of others arrive (the test port on hosts) */
        reach_callbacks cb = { &fc, on_known, on_offer, on_global };
        if (!(net = reach_new(&ro, id, cfg->log, &cb)))
            fatal = "OUT_OF_MEMORY";
        cc.net = net;
    }

    while (!fatal && !engine_stop_requested()) {
        if (control)
            ctlport_poll(on_command, &cc);
        if (discovery) {
            int derr = 0;
            if (disco_poll(on_found, &fc, &derr) != 0) {
                cfg->log("discovery: the socket failed (error %d); no longer listening", derr);
                disco_close();
                discovery = 0;
            }
        }
        net_ms now = net_now(), wake = now + STOP_POLL_MS;
        int again = 0, err = 0, nitems = (int)n + 1;
        if (net) {
            for (size_t i = 0; i < n; i++)
                if (link_wants_lookup(links[i]))
                    reach_lookup(net, link_peer_id(links[i]));
            reach_step(net, now, &wake);
            int k = reach_want(net, &items[n + 1]);
            if (k < 0)
                again = 1;
            else
                nitems += k;
        }
        for (size_t i = 0; i < n; i++) {
            net_ms w = wake;
            if ((fatal = link_step(links[i], now, &w)) != NULL)
                break;
            if (w < wake)
                wake = w;
        }
        if (fatal)
            break;
        for (size_t i = 0; i < n; i++)
            if (link_want(links[i], &items[i]))
                again = 1;
        if (again)
            continue;
        /* The wait ends for an announcement as for a link's socket; the
         * next turn reads it. */
        items[n].sock = discovery ? disco_socket() : -1;
        items[n].rd = discovery;
        items[n].wr = items[n].ready = 0;
        if (net_wait_many(items, nitems, wake, &err) == NET_ERROR)
            net_wait_many(NULL, 0, net_now() + 200, &err); /* the link concerned finds out when it reads */
    }

    if (control)
        ctlport_close();
    reach_free(net);
    disco_close();
    long sessions = 0;
    int unsaved = 0;
    for (size_t i = 0; links && i < n; i++) {
        if (!links[i])
            continue;
        sessions += link_sessions(links[i]);
        if (link_close(links[i]))
            unsaved = 1;
    }
    free(links);
    free(items);
    if (fatal) {
        *result = fatal;
        return 1;
    }
    cfg->log("engine: stopped on request after %ld session(s)", sessions);
    if (unsaved) {
        /* The folder itself is intact; the next start reads it again. */
        cfg->log("engine: the last state write failed, so the state file is behind the folder");
        if (cfg->ui)
            cfg->ui("Stopped, but the local state could not be saved");
        *result = "STATE_NOT_SAVED";
        return 1;
    }
    *result = "STOPPED";
    return 0;
}
