#include "link.h"

#include "bep.h"
#include "frame.h"
#include "idxprog.h"
#include "prof.h"
#include "relay.h"
#include "scan.h"
#include "state.h"
#include "store.h"
#include "tw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define CLIENT_NAME "aros-syncthing"
#define CLIENT_VERSION "0.0.1-poc"

/* The limits and intervals of session.c, which see for the reasons. */
#define MAX_WIRE_MESSAGE (17u << 20)
#define MAX_DECOMPRESSED (64u << 20)
#define HELLO_SECONDS 20
#define PING_SECONDS 30
#define SEND_SECONDS 120
#define CONNECT_SECONDS 30
#define OUT_HOLD_BYTES (4u << 20)
#define SCAN_SLICE_MS 100
#define BACKOFF_MAX_SECONDS 60
/* A folder left out of a session is looked at again this often. */
#define SKIP_RETRY_SECONDS 60

enum { L_WAIT = 0, L_PREPARE, L_CONNECT, L_UP };

struct lfolder {
    const link_folder *cfg;
    char state_path[1100];
    store *st;
    tw *tw;
    /* The folder itself (store and tw) is opened once and kept for the
     * whole run, whatever happens to the connection; it is opened again
     * only after it became unusable (its directory or marker gone). */
    int begun;        /* tw_prepare_begin done, the folder is being read */
    int prepared;     /* read: ready for a connection */
    const char *skip; /* why it could not be opened or read, or NULL */
    int paused;       /* by link_pause: left out of sessions, not read */
    const char *lost; /* became unusable during the session: closed at its end */
    /* for the connection in progress */
    int joined;       /* named in our ClusterConfig */
    int active;       /* exchanged with the peer */
    const char *inactive_why;
    int offered, we_listed, peer_listed, index_reset;
    uint32_t stop_reason;
    uint64_t self_index_id;
    int64_t self_max_seq;
    idxprog idx;
    int got_index, complete_told, in_sync;
    long index_files, files_reported;
    char label[100];
};

struct peerlink {
    link_config cfg;
    /* A device without a typed address (cfg.ip NULL): known_ip is the
     * address it announced on the local network, "" none. */
    int dynamic, no_address_told;
    char known_ip[NET_IPV4_TEXT];
    unsigned known_port;
    const tls_identity *id;
    unsigned char my_id[DEVID_BYTES];
    struct lfolder *f;
    size_t nf;
    int state;
    net_ms retry_at;
    int failures, index_seen, unsaved;
    const char *last_end;
    long sessions;
    size_t prep_i;
    const char *end_request; /* a reason to end the session at its next step (pause, resume) */
    /* Addresses from global discovery (link_set_global): direct ones
     * first, then relays. An attempt takes the next address of the
     * list made of the typed or announced one and these. */
    struct cand {
        int relay;
        char ip[NET_IPV4_TEXT];
        unsigned port;
        relay_addr ra;
    } g[LINK_MAX_GLOBAL];
    int ng, cand_next, cand_used, round_end;
    const char *typed_ip;    /* the typed address, or NULL */
    relay_dial *rdial;       /* the attempt in progress goes through a relay */
    /* A connection that came in (link_offer), for the next attempt. */
    tls_conn *offered;
    const char *offered_via;
    char offered_from[NET_IPV4_TEXT];
    const char *via;         /* of the attempt or session: direct, incoming, relay */
    char addr_ip[NET_IPV4_TEXT];
    unsigned addr_port;      /* the address of the attempt or session, as shown */
    int offline_again;       /* a folder without a connection has more to do at once */

    tls_conn *conn;
    frame_rx rx;
    int rx_live;
    net_ms connect_deadline, hello_deadline, next_ping, skip_retry_at;
    int got_hello, got_cc, peer_gone, hold, again;
    long cc_count;
    char peer_version[120];
    struct lfolder *cur;       /* the folder of the index message being decoded */
    int64_t msg_max_seq;
    const char *offer_outcome;
};

/* ---- small things ---- */

long link_backoff_seconds(int failures)
{
    long s = 1;
    while (failures-- > 0 && s < BACKOFF_MAX_SECONDS)
        s *= 2;
    return s > BACKOFF_MAX_SECONDS ? BACKOFF_MAX_SECONDS : s;
}

static void scope(const peerlink *l, const struct lfolder *f)
{
    char text[200];
    if (!l->cfg.scope)
        return;
    if (f)
        snprintf(text, sizeof text, "%s/%s", l->cfg.name ? l->cfg.name : "", f->cfg->id);
    else
        snprintf(text, sizeof text, "%s", l->cfg.name ? l->cfg.name : "");
    l->cfg.scope(text);
}

static void unscope(const peerlink *l)
{
    if (l->cfg.scope)
        l->cfg.scope(NULL);
}

#define UI(l, ...) do { if ((l)->cfg.ui) (l)->cfg.ui(__VA_ARGS__); } while (0)

/* Folder and file names come from the peer: quote them, escaping control
 * characters and anything non-ASCII, so a log line stays one line. */
static void quote(char *out, size_t cap, pb_slice s)
{
    size_t o = 0;
    for (size_t i = 0; i < s.len && o + 5 < cap; i++) {
        unsigned char c = s.data[i];
        if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') {
            out[o++] = (char)c;
        } else {
            o += (size_t)snprintf(out + o, cap - o, "\\x%02x", c);
        }
    }
    if (o + 5 >= cap && cap > 4)
        memcpy(out + cap - 4, "...", 4), o = cap - 1;
    out[o] = 0;
}

static long send_seconds(void)
{
#ifdef NET_TEST_HOOKS
    if (getenv("STPOC_TEST_SEND_SECONDS"))
        return atol(getenv("STPOC_TEST_SEND_SECONDS"));
#endif
    return SEND_SECONDS;
}

static int fatal(const char *r)
{
    return strcmp(r, "BAD_ARGUMENT") == 0 || strcmp(r, "OUT_OF_MEMORY") == 0 || strcmp(r, "RNG_FAILED") == 0;
}

static struct lfolder *find_folder(peerlink *l, pb_slice id)
{
    for (size_t i = 0; i < l->nf; i++)
        if (id.len == strlen(l->f[i].cfg->id) && memcmp(id.data, l->f[i].cfg->id, id.len) == 0)
            return &l->f[i];
    return NULL;
}

static enum tls_result send_frames(peerlink *l, const pb_writer *w)
{
    if (w->error)
        return TLS_E_INTERNAL;
    return tls_queue(l->conn, w->data, w->len);
}

static enum tls_result send_message(void *ctx, uint32_t type, const pb_writer *body)
{
    peerlink *l = ctx;
    if (!l->conn || l->state != L_UP)
        return TLS_E_IO;
    pb_writer f;
    pb_writer_init(&f, MAX_WIRE_MESSAGE + 64);
    frame_put_message(&f, type, body);
    enum tls_result r = send_frames(l, &f);
    pb_writer_free(&f);
    return r;
}

static const char *file_type(uint32_t t)
{
    switch (t) {
    case BEP_FILE: return "file";
    case BEP_DIRECTORY: return "dir";
    case BEP_SYMLINK: case BEP_SYMLINK_FILE: case BEP_SYMLINK_DIRECTORY: return "symlink";
    default: return "unknown";
    }
}

/* ---- folders ---- */

static void folder_drop(struct lfolder *f)
{
    if (f->tw && f->prepared)
        tw_flush(f->tw); /* a folder not read to its end is not written */
    tw_free(f->tw);
    f->tw = NULL;
    store_close(f->st);
    f->st = NULL;
    f->begun = f->prepared = 0;
}

/* Opens a folder up to the point where it is read. NULL, a reason to
 * leave it out (in *skip), or a fatal code. */
static const char *folder_begin(peerlink *l, struct lfolder *f, size_t index, const char **skip)
{
    char why[300];
    *skip = NULL;
    if (!(f->st = store_open(f->cfg->path, why, sizeof why))) {
        l->cfg.log("sync: %s", why);
        *skip = "BAD_DESTINATION";
        return NULL;
    }
    if (!l->cfg.identity_dir
        || state_path(l->cfg.identity_dir, f->cfg->id, f->state_path, sizeof f->state_path) != 0) {
        folder_drop(f);
        return "BAD_ARGUMENT";
    }
    l->cfg.log("sync: two-way, state file %s, folder %s", f->state_path, f->cfg->path);
    tw_config tc = { f->cfg->id, f->cfg->path, f->state_path, l->cfg.peer_id, l->my_id, f->st, l->cfg.log, l->cfg.ui,
                     send_message, l, f->cfg->rescan_seconds, TW_HASH_AUTO, SCAN_SLICE_MS, NULL,
                     (int32_t)((index + 1) << 24) };
    if (!(f->tw = tw_new(&tc))) {
        folder_drop(f);
        return "OUT_OF_MEMORY";
    }
    const char *r = tw_prepare_begin(f->tw);
    if (r) {
        folder_drop(f);
        if (fatal(r))
            return r;
        *skip = r;
        return NULL;
    }
    f->begun = 1;
    return NULL;
}

/* One piece of the reading of the folders. *more: not all read yet. */
static const char *prepare_some(peerlink *l, net_ms now, int *more)
{
    *more = 0;
    for (; l->prep_i < l->nf; l->prep_i++) {
        struct lfolder *f = &l->f[l->prep_i];
        const char *r;
        if (f->prepared || f->paused)
            continue;
        scope(l, f);
        if (!f->begun) {
            if ((r = folder_begin(l, f, l->prep_i, &f->skip)) != NULL)
                return r;
            if (f->skip) {
                l->cfg.log("engine: folder \"%s\" is left out of this attempt: %s", f->cfg->id, f->skip);
                UI(l, "Folder \"%s\" cannot be used: %s", f->cfg->id, f->skip);
                continue;
            }
        }
        int done = 0;
        r = tw_prepare_step(f->tw, now + SCAN_SLICE_MS, &done);
        if (r) {
            folder_drop(f);
            if (fatal(r))
                return r;
            f->skip = r;
            l->cfg.log("engine: folder \"%s\" is left out of this attempt: %s", f->cfg->id, r);
            UI(l, "Folder \"%s\" cannot be used: %s", f->cfg->id, r);
            continue;
        }
        if (!done) {
            *more = 1;
            return NULL;
        }
        f->prepared = 1;
        l->cfg.log("sync: writing into %s", f->cfg->path);
    }
    return NULL;
}

/* ---- attempts ---- */

static void conn_drop(peerlink *l)
{
    if (l->conn) {
        tls_abort(l->conn);
        l->conn = NULL;
    }
    if (l->rdial) {
        relay_dial_abort(l->rdial);
        l->rdial = NULL;
    }
    if (l->rx_live) {
        frame_rx_free(&l->rx);
        l->rx_live = 0;
    }
}

/* The attempt is over without a session, or after one: the next follows
 * after a pause. */
static void end_attempt(peerlink *l, const char *reason, net_ms now)
{
    scope(l, NULL);
    long wait;
    if (!l->index_seen && !l->round_end) {
        wait = 1; /* the next address of the list, without growing the pause */
    } else {
        l->failures = l->index_seen ? 0 : l->failures + 1;
        wait = link_backoff_seconds(l->failures);
    }
    l->cfg.log("engine: session %ld ended: %s; next attempt in %ld s", l->sessions, reason, wait);
    UI(l, "Not connected (%s); next attempt in %ld s", reason, wait);
    l->retry_at = now + wait * 1000;
    l->state = L_WAIT;
    if (l->index_seen && l->cand_used >= 0)
        l->cand_next = l->cand_used; /* what worked is tried first again */
    l->index_seen = 0;
    l->last_end = reason;
}

static const char *session_begin(peerlink *l, net_ms now)
{
    frame_rx_init(&l->rx, MAX_WIRE_MESSAGE, MAX_DECOMPRESSED);
    l->rx_live = 1;
    l->got_hello = l->got_cc = l->peer_gone = l->hold = l->again = 0;
    l->cc_count = 0;
    l->cur = NULL;
    l->offer_outcome = NULL;
    l->skip_retry_at = now + SKIP_RETRY_SECONDS * 1000;
    for (size_t i = 0; i < l->nf; i++) {
        struct lfolder *f = &l->f[i];
        f->joined = f->active = f->prepared && !f->paused;
        f->inactive_why = NULL;
        f->lost = NULL;
        if (f->joined)
            tw_attach(f->tw);
        f->offered = f->we_listed = f->peer_listed = f->index_reset = 0;
        f->stop_reason = 0;
        memset(&f->idx, 0, sizeof f->idx);
        f->got_index = f->complete_told = 0;
        f->in_sync = -1;
        f->index_files = f->files_reported = 0;
        f->label[0] = 0;
    }
    l->state = L_UP;

    pb_writer hello, out;
    pb_writer_init(&hello, BEP_HELLO_MAX);
    bep_encode_hello(&hello, l->cfg.device_name, CLIENT_NAME, CLIENT_VERSION, (int64_t)time(NULL) * 1000000000);
    pb_writer_init(&out, MAX_WIRE_MESSAGE);
    frame_put_hello(&out, &hello);
    enum tls_result tr = send_frames(l, &out);
    pb_writer_free(&hello);
    pb_writer_free(&out);
    l->hello_deadline = now + HELLO_SECONDS * 1000;
    l->next_ping = now + PING_SECONDS * 1000;
    return tr == TLS_OK ? NULL : tls_result_name(tr);
}

/* The end of a session: summaries, the state of every folder written,
 * Close, everything of the connection released. The folders stay open
 * (detached), except one that became unusable or lost a record. */
static void session_end(peerlink *l, const char *outcome)
{
    int unsaved = 0;
    for (size_t i = 0; i < l->nf; i++) {
        struct lfolder *f = &l->f[i];
        if (!f->tw || !f->joined)
            continue;
        scope(l, f);
        tw_flush(f->tw);
        tw_stats session, *ts = &session;
        tw_get_session_stats(f->tw, ts);
        l->cfg.log("tw: summary: here %ld new, %ld changed, %ld deleted; from the peer %ld file(s) written, %ld dir(s), "
                   "%ld recorded, %ld adopted, %ld deleted here; %ld conflict copie(s) made here, %ld dir(s) kept; "
                   "%ld leftover(s) removed, %ld restored; %ld block(s) served, %ld request(s) refused, %lld bytes in, "
                   "%lld bytes out; %ld held, %ld unsupported, %ld rejected, %ld failed, %ld unchecked; %ld announced, "
                   "%ld state write(s), %ld failure(s)",
                   ts->local_new, ts->local_changed, ts->local_deleted, ts->downloaded, ts->dirs_made, ts->recorded,
                   ts->adopted, ts->deleted_here, ts->conflicts, ts->dirs_kept, ts->leftovers_removed,
                   ts->leftovers_restored, ts->served_blocks, ts->refused_requests, ts->bytes_in, ts->bytes_out,
                   ts->held, ts->unsupported, ts->rejected, ts->failed, ts->unchecked, ts->announced, ts->state_saves,
                   ts->state_failures);
        const char *tr2 = tw_result(f->tw);
        if (!tw_state_saved(f->tw)) {
            /* A write that failed is tried again while the folder stays
             * open (link_close looks once more); a lost record is not. */
            if (tw_state_lost(f->tw))
                unsaved = 1;
            l->cfg.log("tw: the state file is not complete at the end of this session");
        }
        if (f->active)
            UI(l, "%s: %ld new, %ld changed, %ld deleted here; %ld received, %ld deleted from the peer; %ld conflict "
                  "copie(s); %ld held",
               tr2 ? "Not fully in sync" : "In sync", ts->local_new, ts->local_changed, ts->local_deleted,
               ts->downloaded + ts->dirs_made, ts->deleted_here, ts->conflicts, ts->held);
        l->cfg.log("bep: summary: cc %s, folder %s, index %s (sequence %lld; %lld at connection; %ld message(s)), "
                   "updates %d, files reported %ld",
                   l->got_cc ? "yes" : "no", f->offered ? "offered" : "not offered",
                   f->idx.complete ? "complete" : f->got_index ? "INCOMPLETE" : "not received",
                   (long long)f->idx.seen, (long long)f->idx.target, f->idx.messages, f->idx.updates,
                   f->files_reported);
    }
    l->unsaved = unsaved;
    scope(l, NULL);
    char prof[600];
    prof_format(prof, sizeof prof);
    l->cfg.log("profile: %s", prof);
    l->cfg.log("session: end, wall clock %lld", (long long)time(NULL));

    if (l->conn && l->peer_gone) {
        tls_close(l->conn, net_now() + 2000);
        l->conn = NULL;
    }
    if (l->conn) {
        const char *reason = "aros-syncthing: stopping";
        pb_writer w;
        pb_writer_init(&w, 256);
        bep_encode_close(&w, reason);
        /* Best effort, with a short deadline: what is still queued (the
         * last announcements) goes out first, then the Close. */
        enum tls_result r = send_message(l, BEP_CLOSE, &w);
        if (r == TLS_OK)
            r = tls_flush(l->conn, net_now() + 3000);
        l->cfg.log("bep: sent Close \"%s\": %s", reason, tls_result_name(r));
        pb_writer_free(&w);
        /* Let the peer act on the Close before we disconnect (see
         * session.c): wait for it to close, for at most 2 s. */
        if (r == TLS_OK) {
            unsigned char sink[4096];
            size_t got;
            int closed = 0;
            net_ms until = net_now() + 2000;
            while (tls_read(l->conn, sink, sizeof sink, &got, &closed, until) == TLS_OK)
                ;
            l->cfg.log("bep: %s after our Close", closed ? "peer closed the connection"
                       : net_now() < until ? "the connection ended with an error" : "peer did not close in 2 s");
        }
        tls_close(l->conn, net_now() + 2000);
        l->conn = NULL;
    }
    conn_drop(l);
    for (size_t i = 0; i < l->nf; i++) {
        struct lfolder *f = &l->f[i];
        if (!f->tw || !f->joined)
            continue;
        tw_detach(f->tw);
        f->joined = f->active = 0;
        if (f->lost) {
            folder_drop(f);
            f->skip = f->lost;
            f->lost = NULL;
        } else if (tw_state_lost(f->tw)) {
            /* What is in memory is not the whole folder any more: the
             * next attempt opens it again from its state file. */
            folder_drop(f);
        }
    }
    l->end_request = NULL;
    (void)outcome;
    unscope(l);
}

/* ---- messages ---- */

static int on_file(void *ctx, const bep_file *fi)
{
    peerlink *l = ctx;
    struct lfolder *f = l->cur;
    char name[300];
    quote(name, sizeof name, fi->name);
    l->cfg.log("file: %-7s size=%lld seq=%lld deleted=%d invalid=%d blocks=%lu modified=%lld name=\"%s\"",
               file_type(fi->type), (long long)fi->size, (long long)fi->sequence, fi->deleted, fi->invalid,
               (unsigned long)fi->blocks, (long long)fi->modified_s, name);
    f->files_reported++;
    if (fi->sequence > l->msg_max_seq)
        l->msg_max_seq = fi->sequence;
    if (!l->offer_outcome && (l->offer_outcome = tw_offer(f->tw, fi)) != NULL)
        return 1;
    return 0;
}

static int cc_folder(void *ctx, const bep_cc_folder *cf)
{
    peerlink *l = ctx;
    char id[120], label[120];
    quote(id, sizeof id, cf->id);
    quote(label, sizeof label, cf->label);
    l->cfg.log("cc: folder \"%s\" label \"%s\" type %u stop_reason %u", id, label, cf->type, cf->stop_reason);
    struct lfolder *f = find_folder(l, cf->id);
    if (f) {
        f->offered = 1;
        f->stop_reason = cf->stop_reason;
        quote(f->label, sizeof f->label, cf->label.len ? cf->label : cf->id);
    }
    return 0;
}

static int cc_device(void *ctx, const bep_cc_folder *cf, const bep_cc_device *d)
{
    peerlink *l = ctx;
    char text[DEVID_TEXT_SIZE] = "(invalid)";
    if (d->id_valid)
        devid_format(d->id, text);
    l->cfg.log("cc:   device %.7s max_sequence %lld index_id %016llx%s%s", text, (long long)d->max_sequence,
               (unsigned long long)d->index_id, d->encryption_token_len ? " encrypted" : "",
               d->introducer ? " introducer" : "");
    struct lfolder *f = find_folder(l, cf->id);
    if (!f || !d->id_valid)
        return 0;
    if (memcmp(d->id, l->my_id, DEVID_BYTES) == 0) {
        f->we_listed = 1;
        f->self_index_id = d->index_id;
        f->self_max_seq = d->max_sequence;
    }
    /* The peer's own entry: how far its index for the folder goes, and
     * under which index ID (a later ClusterConfig with another one means
     * the peer started its index again). */
    if (memcmp(d->id, l->cfg.peer_id, DEVID_BYTES) == 0) {
        f->peer_listed = 1;
        if (f->joined && idxprog_cc(&f->idx, d->max_sequence, d->index_id) == IDX_RESET)
            f->index_reset = 1;
    }
    return 0;
}

static const char *on_cluster_config(peerlink *l, const unsigned char *body, size_t len)
{
    for (size_t i = 0; i < l->nf; i++) {
        struct lfolder *f = &l->f[i];
        f->offered = f->we_listed = f->peer_listed = f->index_reset = 0;
        f->stop_reason = 0;
    }
    if (bep_decode_cluster_config(body, len, l, cc_folder, cc_device) != 0) {
        l->cfg.log("bep: malformed ClusterConfig");
        return "PROTOCOL_ERROR";
    }
    l->got_cc = 1;
    l->cc_count++;
    const char *none = "FOLDER_NOT_SHARED";
    int any = 0;
    for (size_t i = 0; i < l->nf; i++) {
        struct lfolder *f = &l->f[i];
        if (!f->joined)
            continue;
        scope(l, f);
        if (f->index_reset) {
            l->cfg.log("cc: the peer announces another index ID for the folder; its index starts again, "
                       "which this client does not support within a session");
            return "INDEX_RESET_UNSUPPORTED";
        }
        int usable = f->offered && f->stop_reason == 0;
        if (l->cc_count > 1 && usable != f->active) {
            /* What the peer shares with us changed: a new connection takes it up. */
            l->cfg.log("cc: folder \"%s\" is %s by the peer; reconnecting", f->cfg->id,
                       usable ? "now offered" : "no longer offered");
            return "CLUSTER_CONFIG_CHANGED";
        }
        if (!f->offered) {
            l->cfg.log("cc: the peer does not share folder \"%s\" with us", f->cfg->id);
            UI(l, "Folder \"%s\" is not shared by the peer", f->cfg->id);
            f->active = 0;
            f->inactive_why = "FOLDER_NOT_SHARED";
            continue;
        }
        l->cfg.log("cc: folder \"%s\" offered%s", f->cfg->id,
                   f->we_listed ? ", our device listed" : ", our device NOT listed");
        if (f->stop_reason != 0) {
            /* A paused folder sends no index (FolderStopReason PAUSED). */
            l->cfg.log("cc: the peer's folder is stopped (stop_reason %u)", f->stop_reason);
            UI(l, "Folder \"%s\" is paused on the peer", f->label);
            f->active = 0;
            f->inactive_why = none = "FOLDER_PAUSED";
            continue;
        }
        if (f->peer_listed)
            l->cfg.log("cc: the peer's index goes up to sequence %lld", (long long)f->idx.target);
        else
            l->cfg.log("cc: the peer does not list itself for the folder; completeness of its index "
                       "cannot be established");
        if (f->idx.complete)
            UI(l, "Index received: the folder is empty on the peer");
        const char *r = tw_peer_cc(f->tw, f->we_listed, f->self_index_id, f->self_max_seq);
        if (r)
            return r;
        UI(l, "Folder \"%s\" is shared with this AROS device", f->label);
        any = 1;
    }
    scope(l, NULL);
    return any ? NULL : none;
}

static const char *on_index(peerlink *l, uint32_t type, const unsigned char *body, size_t len)
{
    pb_slice folder;
    int64_t last, prev;
    const char *kind = type == BEP_INDEX ? "Index" : "IndexUpdate";
    /* Validate first (decode with no callback), report only a valid message. */
    if (bep_decode_index(body, len, &folder, &last, &prev, NULL, NULL) != 0) {
        l->cfg.log("bep: malformed %s", kind);
        return "PROTOCOL_ERROR";
    }
    struct lfolder *f = find_folder(l, folder);
    if (!f || !f->active) {
        char q[120];
        quote(q, sizeof q, folder);
        l->cfg.log("bep: %s for folder \"%s\" ignored", kind, q);
        return NULL;
    }
    scope(l, f);
    l->cfg.log("bep: %s, %lu bytes, last_sequence %lld, prev_sequence %lld", kind, (unsigned long)len,
               (long long)last, (long long)prev);
    /* The chain is checked before any file of the message is acted on. */
    int ic = idxprog_check(&f->idx, type == BEP_INDEX_UPDATE, prev);
    if (ic == IDX_BAD_CHAIN) {
        l->cfg.log("bep: %s follows on from sequence %lld, but %lld was the last one received; "
                   "the peer's index does not hang together; none of its entries used",
                   kind, (long long)prev, (long long)f->idx.seen);
        return "INDEX_SEQUENCE_ERROR";
    }
    if (ic == IDX_RESET) {
        l->cfg.log("bep: a second full Index: the peer started its index again, which this client does "
                   "not support within a session; none of its entries used");
        return "INDEX_RESET_UNSUPPORTED";
    }
    long before = f->files_reported;
    int was_complete = f->idx.complete;
    l->msg_max_seq = 0;
    l->cur = f;
    PROF_BEGIN(ti);
    bep_decode_index(body, len, &folder, &last, &prev, l, on_file);
    PROF_END(PROF_INDEX, ti);
    l->cur = NULL;
    l->cfg.log("bep: %s carried %ld file(s)", kind, f->files_reported - before);
    if (l->offer_outcome)
        return l->offer_outcome;
    if (type == BEP_INDEX)
        f->got_index = 1;
    int is_update = idxprog_message(&f->idx, type == BEP_INDEX_UPDATE, prev, last, l->msg_max_seq) == IDX_UPDATE;
    if (!was_complete) {
        f->index_files += f->files_reported - before;
        if (f->idx.complete) {
            l->cfg.log("bep: index complete: sequence %lld reached (%lld at connection), %ld entr%s in %ld "
                       "message(s)", (long long)f->idx.seen, (long long)f->idx.target, f->index_files,
                       f->index_files == 1 ? "y" : "ies", f->idx.messages);
            UI(l, "Index received: %ld entries", f->index_files);
        } else if (f->idx.have_target) {
            l->cfg.log("bep: index so far up to sequence %lld of %lld", (long long)f->idx.seen,
                       (long long)f->idx.target);
        }
    } else if (is_update) {
        l->cfg.log("bep: update %d after the complete index", f->idx.updates);
    }
    return NULL;
}

static const char *refuse_request(peerlink *l, int32_t id)
{
    pb_writer w;
    pb_writer_init(&w, 64);
    bep_encode_response_error(&w, id, 2 /* ERROR_CODE_NO_SUCH_FILE */);
    enum tls_result r = send_message(l, BEP_RESPONSE, &w);
    pb_writer_free(&w);
    return r == TLS_OK ? NULL : "IO_ERROR";
}

/* Handles one complete message. NULL to continue, or the reason the
 * session ends. */
static const char *handle(peerlink *l, uint32_t type, const unsigned char *body, size_t len)
{
    if (!l->got_cc && type != BEP_CLUSTER_CONFIG) {
        /* Syncthing itself refuses anything before the ClusterConfig. */
        l->cfg.log("bep: message type %u before ClusterConfig", type);
        return "PROTOCOL_ERROR";
    }
    switch (type) {
    case BEP_CLUSTER_CONFIG:
        return on_cluster_config(l, body, len);

    case BEP_INDEX:
    case BEP_INDEX_UPDATE:
        return on_index(l, type, body, len);

    case BEP_PING:
    case BEP_DOWNLOAD_PROGRESS:
        return NULL;

    case BEP_CLOSE: {
        pb_slice reason;
        char r[200] = "";
        if (bep_decode_close(body, len, &reason) == 0)
            quote(r, sizeof r, reason);
        l->cfg.log("bep: peer sent Close: \"%s\"", r);
        l->peer_gone = 1;
        return "PEER_CLOSED";
    }

    case BEP_REQUEST: {
        bep_request q;
        if (bep_decode_request_full(body, len, &q) != 0)
            return "PROTOCOL_ERROR";
        struct lfolder *f = find_folder(l, q.folder);
        if (!f || !f->active) {
            char name[120];
            quote(name, sizeof name, q.folder);
            l->cfg.log("bep: Request %d for folder \"%s\" answered with NO_SUCH_FILE", q.id, name);
            return refuse_request(l, q.id);
        }
        scope(l, f);
        return tw_request(f->tw, &q);
    }

    case BEP_RESPONSE: {
        int32_t id;
        pb_slice data;
        uint32_t code;
        if (bep_decode_response(body, len, &id, &data, &code) != 0) {
            l->cfg.log("bep: malformed Response");
            return "PROTOCOL_ERROR";
        }
        /* Each folder asks with IDs of its own range (folder_begin). */
        size_t i = (size_t)((uint32_t)id >> 24);
        if (i < 1 || i > l->nf || !l->f[i - 1].active) {
            l->cfg.log("bep: Response %d belongs to no request of ours", id);
            return "PROTOCOL_ERROR";
        }
        scope(l, &l->f[i - 1]);
        return tw_response(l->f[i - 1].tw, id, data, code);
    }

    default:
        /* Syncthing skips message types it does not know, for forward
         * compatibility (errUnknownMessage in lib/protocol); so do we. */
        l->cfg.log("bep: unknown message type %u skipped", type);
        return NULL;
    }
}

static const char *on_hello(peerlink *l)
{
    bep_hello h;
    char dn[120], cn[120], cv[120];
    if (bep_decode_hello(l->rx.body, l->rx.body_len, &h) != 0)
        return "PROTOCOL_ERROR";
    quote(dn, sizeof dn, h.device_name);
    quote(cn, sizeof cn, h.client_name);
    quote(cv, sizeof cv, h.client_version);
    l->cfg.log("bep: peer Hello: device \"%s\", client \"%s\" \"%s\"", dn, cn, cv);
    l->got_hello = 1;
    snprintf(l->peer_version, sizeof l->peer_version, "%s", cv);
    if (l->cfg.ui) {
        char id[DEVID_TEXT_SIZE];
        devid_format(l->cfg.peer_id, id);
        UI(l, "Connected to Syncthing %s at %s:%u over TLS 1.3%s", l->peer_version, l->addr_ip, l->addr_port,
           strcmp(l->via, "relay") == 0 ? ", through a relay" : strcmp(l->via, "incoming") == 0 ? " (it connected to us)" : "");
        UI(l, "Peer identity verified (device %.7s-...)", id);
    }

    /* Our ClusterConfig: every folder that could be read, each with the
     * peer (nothing known: its full index is wanted) and ourselves. */
    unsigned char peer[DEVID_BYTES];
    memcpy(peer, l->cfg.peer_id, DEVID_BYTES);
    pb_writer out, body;
    pb_writer_init(&out, MAX_WIRE_MESSAGE);
    pb_writer_init(&body, MAX_WIRE_MESSAGE);
    for (size_t i = 0; i < l->nf; i++) {
        struct lfolder *f = &l->f[i];
        if (!f->joined)
            continue;
        uint64_t my_index_id = 0;
        int64_t my_max = 0;
        tw_cc_self(f->tw, &my_index_id, &my_max);
        bep_cc_device_out devs[2] = {
            { peer, "", 0, 0 },
            { l->my_id, l->cfg.device_name, my_max, my_index_id },
        };
        bep_encode_cluster_config(&body, f->cfg->id, "", BEP_FOLDER_SEND_RECEIVE, devs, 2);
        scope(l, f);
        l->cfg.log("bep: sent ClusterConfig (folder \"%s\", send & receive, index_id %016llx, max_sequence "
                   "%lld); our index follows the peer's ClusterConfig",
                   f->cfg->id, (unsigned long long)my_index_id, (long long)my_max);
    }
    scope(l, NULL);
    frame_put_message(&out, BEP_CLUSTER_CONFIG, &body);
    pb_writer_free(&body);
    enum tls_result tr = send_frames(l, &out);
    pb_writer_free(&out);
    return tr == TLS_OK ? NULL : tls_result_name(tr);
}

/* A folder left out of the session because its directory could not be
 * opened or its marker was missing: is that over? Other reasons are
 * looked at again only by the next attempt. */
static int skipped_usable(struct lfolder *f)
{
    char why[300];
    int marker = strcmp(f->skip, "FOLDER_UNAVAILABLE") == 0;
    if (!marker && strcmp(f->skip, "BAD_DESTINATION") != 0)
        return 0;
    store *st = store_open(f->cfg->path, why, sizeof why);
    if (!st)
        return 0;
    int usable = 1;
    if (marker) {
        char *p = store_full_path(st, SCAN_MARKER);
        struct stat sb;
        usable = p && stat(p, &sb) == 0;
        free(p);
    }
    store_close(st);
    return usable;
}

/* One round of a session. NULL, or the reason it ends. */
static const char *up_step(peerlink *l, net_ms now, net_ms *wake)
{
    const char *outcome;
    int backlog = 0;
    l->again = 0;
    if (l->end_request) {
        l->index_seen = 1; /* no pause to speak of */
        return l->end_request;
    }
    for (;;) {
        if (tls_pending(l->conn) > OUT_HOLD_BYTES) {
            backlog = 1; /* messages already received are left for later */
            break;
        }
        enum frame_status st = frame_rx_next(&l->rx);
        if (st == FRAME_NEED_MORE)
            break;
        scope(l, NULL);
        if (st == FRAME_ERROR) {
            l->cfg.log("bep: framing error: %s", l->rx.error);
            return "PROTOCOL_ERROR";
        }
        if (st == FRAME_HELLO) {
            outcome = on_hello(l);
        } else {
            if (l->rx.type != BEP_PING)
                l->cfg.log("bep: received type %u, %lu bytes on the wire, %s, %lu bytes decoded", l->rx.type,
                           (unsigned long)l->rx.wire_len,
                           l->rx.compression == BEP_COMPRESSION_LZ4 ? "LZ4" : "uncompressed",
                           (unsigned long)l->rx.body_len);
            outcome = handle(l, l->rx.type, l->rx.body, l->rx.body_len);
        }
        if (outcome)
            return outcome;
    }

    net_ms w = now + 3600 * 1000;
    for (size_t i = 0; l->got_cc && i < l->nf; i++) {
        struct lfolder *f = &l->f[i];
        if (!f->active)
            continue;
        scope(l, f);
        if (f->idx.complete && !f->complete_told) {
            f->complete_told = 1;
            if ((outcome = tw_peer_index_complete(f->tw)) != NULL)
                return outcome;
        }
        net_ms fw = w;
        if ((outcome = tw_step(f->tw, now, &fw)) != NULL) {
            if (strcmp(outcome, "FOLDER_UNAVAILABLE") == 0) {
                f->lost = outcome;
                l->cfg.log("engine: folder \"%s\" cannot be used any more; reconnecting without it", f->cfg->id);
                UI(l, "Folder \"%s\" cannot be used: %s", f->cfg->id, outcome);
            }
            return outcome;
        }
        if (fw < w)
            w = fw;
        if (f->idx.complete)
            l->index_seen = 1;
        int in_sync = f->idx.complete && tw_settled(f->tw) && tw_idle(f->tw);
        if (in_sync != f->in_sync && (in_sync || f->in_sync == 1)) {
            l->cfg.log("engine: %s", in_sync ? "in sync with the peer" : "changes to synchronise");
            UI(l, in_sync ? "In sync" : "Synchronising");
        }
        f->in_sync = in_sync;
    }
    scope(l, NULL);

    if (now >= l->skip_retry_at) {
        l->skip_retry_at = now + SKIP_RETRY_SECONDS * 1000;
        for (size_t i = 0; i < l->nf; i++)
            if (!l->f[i].joined && !l->f[i].tw && !l->f[i].paused && l->f[i].skip && skipped_usable(&l->f[i])) {
                l->cfg.log("engine: folder \"%s\" can be used again; reconnecting to take it up", l->f[i].cfg->id);
                l->index_seen = 1; /* no pause to speak of */
                return "FOLDER_AVAILABLE";
            }
    }
    if (!l->got_hello && now >= l->hello_deadline)
        return "HELLO_TIMEOUT";
    if (now >= l->next_ping && l->got_hello) {
        pb_writer empty;
        pb_writer_init(&empty, 16);
        enum tls_result tr = send_message(l, BEP_PING, &empty);
        pb_writer_free(&empty);
        if (tr != TLS_OK)
            return tls_result_name(tr);
        l->next_ping = now + PING_SECONDS * 1000;
        l->cfg.log("bep: sent Ping");
    }
    net_ms until = l->got_hello ? l->next_ping : l->hello_deadline;
    if (until > w)
        until = w;
    if (until > l->skip_retry_at)
        until = l->skip_retry_at;
    *wake = until;

    enum tls_result tr = tls_flush_some(l->conn);
    if (tr != TLS_OK)
        return tls_result_name(tr);
    size_t unsent = tls_pending(l->conn);
    if (unsent > 0 && net_now() - tls_pending_since(l->conn) > send_seconds() * 1000) {
        l->cfg.log("bep: the peer took nothing for %ld s (%lu bytes unsent)", send_seconds(), (unsigned long)unsent);
        return "SEND_TIMEOUT";
    }
    l->hold = unsent > OUT_HOLD_BYTES;
    if (l->hold)
        return NULL; /* wait for the socket to take more */
    if (backlog) {
        l->again = 1; /* room again: the messages left above come first */
        return NULL;
    }
    unsigned char buf[FRAME_FEED_MAX];
    size_t got = 0;
    int closed = 0;
    tr = tls_read_some(l->conn, buf, sizeof buf, &got, &closed);
    if (tr != TLS_OK) {
        l->peer_gone = closed;
        return closed ? "PEER_DISCONNECTED" : tls_result_name(tr);
    }
    if (got > 0) {
        if (frame_rx_feed(&l->rx, buf, got) != 0) {
            l->cfg.log("bep: receive buffer limit exceeded");
            return "PROTOCOL_ERROR";
        }
        l->again = 1;
    }
    return NULL;
}

/* ---- the link ---- */

peerlink *link_new(const link_config *cfg, const tls_identity *id)
{
    if (cfg->nfolders < 1 || cfg->nfolders > LINK_MAX_FOLDERS)
        return NULL;
    peerlink *l = calloc(1, sizeof *l);
    if (!l)
        return NULL;
    l->cfg = *cfg;
    l->dynamic = cfg->ip == NULL;
    l->typed_ip = cfg->ip;
    l->via = "direct";
    if (l->dynamic)
        l->cfg.port = 0;
    l->id = id;
    tls_identity_devid(id, l->my_id);
    l->nf = cfg->nfolders;
    if (!(l->f = calloc(l->nf, sizeof *l->f))) {
        free(l);
        return NULL;
    }
    for (size_t i = 0; i < l->nf; i++)
        l->f[i].cfg = &cfg->folders[i];
    l->state = L_WAIT;
    return l;
}

/* The folders that are in no session: read when that is due, their state
 * written. *wake: when the next of them wants to go on. */
static void offline_step(peerlink *l, net_ms now, net_ms *wake)
{
    l->offline_again = 0;
    for (size_t i = 0; i < l->nf; i++) {
        struct lfolder *f = &l->f[i];
        if (!f->tw || !f->prepared || f->paused || tw_attached(f->tw))
            continue;
        scope(l, f);
        net_ms fw = now + 3600 * 1000;
        const char *r = tw_step(f->tw, now, &fw);
        if (r) {
            l->cfg.log("engine: folder \"%s\" cannot be used any more: %s", f->cfg->id, r);
            UI(l, "Folder \"%s\" cannot be used: %s", f->cfg->id, r);
            folder_drop(f);
            f->skip = r;
            continue;
        }
        if (tw_scanning(f->tw))
            l->offline_again = 1;
        if (fw < *wake)
            *wake = fw;
    }
    unscope(l);
}

/* The addresses to try, in order: the typed one, or the one announced
 * on the local network, then those of global discovery. */
static int candidates(const peerlink *l)
{
    int primary = l->dynamic ? l->known_ip[0] != 0 : 1;
    return primary + l->ng;
}

/* Starts the attempt at the next address: NULL, or why it could not. */
static const char *attempt_begin(peerlink *l, net_ms now)
{
    (void)now;
    int count = candidates(l);
    int primary = count - l->ng;
    int k = l->cand_next % count;
    l->cand_used = k;
    l->cand_next = k + 1;
    l->round_end = l->cand_next >= count;
    if (l->round_end)
        l->cand_next = 0;
    l->via = "direct";
    if (k < primary) {
        snprintf(l->addr_ip, sizeof l->addr_ip, "%s", l->dynamic ? l->known_ip : l->typed_ip);
        l->addr_port = l->dynamic ? l->known_port : l->cfg.port;
    } else {
        const struct cand *c = &l->g[k - primary];
        snprintf(l->addr_ip, sizeof l->addr_ip, "%s", c->relay ? c->ra.ip : c->ip);
        l->addr_port = c->relay ? c->ra.port : c->port;
        if (c->relay) {
            l->via = "relay";
            if (!(l->rdial = relay_dial_start(l->id, &c->ra, l->cfg.peer_id, l->cfg.log)))
                return "RELAY_UNREACHABLE";
            return NULL;
        }
    }
    enum tls_result tr = tls_start(l->id, l->addr_ip, l->addr_port, l->cfg.peer_id, l->cfg.log, &l->conn);
    if (tr != TLS_OK) {
        l->conn = NULL;
        return tls_result_name(tr);
    }
    return NULL;
}

static const char *conn_step(peerlink *l, net_ms now, net_ms *wake);

const char *link_step(peerlink *l, net_ms now, net_ms *wake)
{
    net_ms ow = now + 3600 * 1000;
    offline_step(l, now, &ow);
    const char *r = conn_step(l, now, wake);
    if (ow < *wake)
        *wake = ow;
    return r;
}

static const char *conn_step(peerlink *l, net_ms now, net_ms *wake)
{
    const char *r;
    *wake = now + 3600 * 1000;
    l->again = 0;

    if (l->state == L_WAIT) {
        int count = candidates(l);
        if (count == 0 && !l->offered) {
            /* Nothing to try: not an attempt, and no pause that grows. */
            if (!l->no_address_told) {
                l->no_address_told = 1;
                scope(l, NULL);
                l->cfg.log("engine: no address for this device yet; waiting for it to be found or to connect");
                UI(l, "Waiting to find the device");
                unscope(l);
            }
            l->last_end = "NO_ADDRESS";
            return NULL;
        }
        if (now < l->retry_at && !l->offered) {
            *wake = l->retry_at;
            return NULL;
        }
        l->sessions++;
        scope(l, NULL);
        l->cfg.log("engine: session %ld", l->sessions);
        l->state = L_PREPARE;
        l->prep_i = 0;
    }

    if (l->state == L_PREPARE) {
        int more = 0;
        r = prepare_some(l, now, &more);
        unscope(l);
        if (r)
            return r;
        if (more) {
            l->again = 1;
            return NULL;
        }
        const char *why = "BAD_DESTINATION";
        int usable = 0, paused = 0;
        for (size_t i = 0; i < l->nf; i++) {
            if (l->f[i].paused)
                paused++;
            else if (l->f[i].prepared)
                usable = 1;
            else if (l->f[i].skip)
                why = l->f[i].skip;
        }
        if (paused == (int)l->nf)
            why = "ALL_FOLDERS_PAUSED";
        if (!usable) {
            if (l->offered) {
                tls_abort(l->offered);
                l->offered = NULL;
            }
            l->round_end = 1;
            end_attempt(l, why, now);
            unscope(l);
            *wake = l->retry_at;
            return NULL;
        }
        scope(l, NULL);
        /* Wall-clock time alongside the monotonic milliseconds of the log,
         * so a slow or fast guest clock shows up in the report. */
        l->cfg.log("session: start, wall clock %lld", (long long)time(NULL));
        if (!l->offered && (r = attempt_begin(l, now)) != NULL) {
            end_attempt(l, r, now);
            unscope(l);
            *wake = l->retry_at;
            return NULL;
        }
        l->connect_deadline = now + CONNECT_SECONDS * 1000;
        l->state = L_CONNECT;
    }

    if (l->state == L_CONNECT) {
        scope(l, NULL);
        if (l->offered) {
            /* A connection that came in replaces our own attempt. */
            if (l->conn || l->rdial)
                l->cfg.log("engine: the device connected to us; our own attempt is given up");
            conn_drop(l);
            l->conn = l->offered;
            l->offered = NULL;
            l->via = l->offered_via;
            snprintf(l->addr_ip, sizeof l->addr_ip, "%s", l->offered_from);
            l->addr_port = 0;
            l->round_end = 1;
            l->cand_used = -1;
            l->cfg.log("engine: using the connection from %s (%s)", l->addr_ip, l->via);
        } else if (l->rdial) {
            tls_conn *c = NULL;
            const char *why = NULL;
            int st = relay_dial_step(l->rdial, now, &c, &why);
            if (st == 0) {
                unscope(l);
                *wake = relay_dial_deadline(l->rdial);
                return NULL;
            }
            if (st < 0) {
                conn_drop(l);
                end_attempt(l, why, now);
                unscope(l);
                *wake = l->retry_at;
                return NULL;
            }
            l->rdial = NULL;
            l->conn = c;
        } else {
            enum tls_result tr = tls_connect_step(l->conn);
            if (tr == TLS_E_AGAIN && now < l->connect_deadline) {
                unscope(l);
                *wake = l->connect_deadline;
                return NULL;
            }
            if (tr != TLS_OK) {
                if (tr == TLS_E_AGAIN) {
                    l->cfg.log("tls: no connection to %s:%u within %d s", l->addr_ip, l->addr_port, CONNECT_SECONDS);
                    tr = TLS_E_TIMEOUT;
                }
                conn_drop(l);
                end_attempt(l, tls_result_name(tr), now);
                unscope(l);
                *wake = l->retry_at;
                return NULL;
            }
        }
        if ((r = session_begin(l, now)) != NULL) {
            session_end(l, r);
            end_attempt(l, r, now);
            unscope(l);
            *wake = l->retry_at;
            return NULL;
        }
    }

    r = up_step(l, now, wake);
    if (r) {
        session_end(l, r);
        if (fatal(r)) {
            scope(l, NULL);
            l->cfg.log("engine: cannot continue: %s", r);
            unscope(l);
            return r;
        }
        end_attempt(l, r, now);
        *wake = l->retry_at;
    }
    unscope(l);
    return NULL;
}

int link_want(const peerlink *l, net_wait_item *it)
{
    it->sock = -1;
    it->rd = it->wr = it->ready = 0;
    if (l->again || l->offline_again)
        return 1;
    if (l->offered && l->state != L_UP && l->state != L_PREPARE)
        return 1;
    if (l->state == L_CONNECT && l->rdial) {
        relay_dial_want(l->rdial, &it->sock, &it->rd, &it->wr);
        return 0;
    }
    if ((l->state == L_CONNECT || l->state == L_UP) && l->conn)
        return tls_want(l->conn, l->state == L_UP && !l->hold, &it->sock, &it->rd, &it->wr);
    return 0;
}

int link_close(peerlink *l)
{
    if (!l)
        return 0;
    if (l->state == L_UP) {
        session_end(l, "STOPPED");
    } else {
        if (l->state == L_CONNECT) {
            scope(l, NULL);
            l->cfg.log("tls: connection attempt stopped on request");
            unscope(l);
        }
        conn_drop(l);
    }
    if (l->offered)
        tls_abort(l->offered);
    int unsaved = l->unsaved;
    for (size_t i = 0; i < l->nf; i++) {
        struct lfolder *f = &l->f[i];
        if (f->tw && f->prepared) {
            /* What was recorded since the last session (folders are read
             * without a connection too) is written now. */
            scope(l, f);
            if (tw_flush(f->tw) != 0 || !tw_state_saved(f->tw)) {
                unsaved = 1;
                l->cfg.log("tw: the state file is not complete at the end of the run");
            }
        }
        folder_drop(f);
    }
    unscope(l);
    free(l->f);
    free(l);
    return unsaved;
}

void link_get_status(const peerlink *l, net_ms now, link_status *out)
{
    static const char *const names[] = { "waiting", "reading", "connecting", "connected" };
    out->name = l->cfg.name ? l->cfg.name : "";
    out->state = names[l->state];
    out->last_end = l->last_end ? l->last_end : "";
    out->retry_seconds = l->state == L_WAIT && l->retry_at > now ? (long)((l->retry_at - now + 999) / 1000) : 0;
    out->sessions = l->sessions;
    out->nfolders = l->nf;
    out->address[0] = 0;
    out->via = "";
    if (l->state == L_CONNECT || l->state == L_UP) {
        out->via = l->via;
        if (l->addr_port)
            snprintf(out->address, sizeof out->address, "%s:%u", l->addr_ip, l->addr_port);
        else
            snprintf(out->address, sizeof out->address, "%s", l->addr_ip);
    } else if (!l->dynamic) {
        snprintf(out->address, sizeof out->address, "%.15s:%u", l->typed_ip, l->cfg.port);
    } else if (l->known_ip[0]) {
        snprintf(out->address, sizeof out->address, "%s:%u", l->known_ip, l->known_port);
    } else if (l->ng) {
        const struct cand *c = &l->g[0];
        snprintf(out->address, sizeof out->address, "%s:%u", c->relay ? c->ra.ip : c->ip, c->relay ? c->ra.port : c->port);
    }
    out->addresses = candidates(l);
}

int link_offer(peerlink *l, tls_conn *c, const char *via, const char *from)
{
    if (l->offered)
        return 0;
    if (l->state == L_UP) {
        /* A direct connection is better than one through a relay. */
        if (strcmp(l->via, "relay") != 0 || strcmp(via, "relay") == 0)
            return 0;
        l->end_request = "REPLACED_BY_DIRECT";
    }
    l->offered = c;
    l->offered_via = strcmp(via, "relay") == 0 ? "relay" : "incoming";
    snprintf(l->offered_from, sizeof l->offered_from, "%s", from ? from : "");
    if (l->last_end && strcmp(l->last_end, "NO_ADDRESS") == 0)
        l->last_end = NULL;
    scope(l, NULL);
    l->cfg.log("engine: the device connected to us from %s (%s)", l->offered_from, l->offered_via);
    unscope(l);
    l->again = 1;
    return 1;
}

/* tcp://IP:PORT (tcp4 too) or relay://...; host names, IPv6 and other
 * schemes are passed over. */
static int parse_candidate(const char *url, struct cand *c)
{
    memset(c, 0, sizeof *c);
    if (strncmp(url, "relay://", 8) == 0) {
        c->relay = 1;
        return relay_parse_url(url, &c->ra);
    }
    const char *h = strncmp(url, "tcp://", 6) == 0 ? url + 6 : strncmp(url, "tcp4://", 7) == 0 ? url + 7 : NULL;
    if (!h)
        return -1;
    const char *colon = strchr(h, ':');
    if (!colon || (size_t)(colon - h) >= sizeof c->ip)
        return -1;
    memcpy(c->ip, h, (size_t)(colon - h));
    c->ip[colon - h] = 0;
    char *e;
    long port = strtol(colon + 1, &e, 10);
    if (!net_is_ipv4(c->ip) || strcmp(c->ip, "0.0.0.0") == 0 || port < 1 || port > 65535 || (*e && *e != '/'))
        return -1;
    c->port = (unsigned)port;
    return 0;
}

int link_set_global(peerlink *l, const char *const *urls, int n)
{
    if (!l->dynamic)
        return 0;
    struct cand list[LINK_MAX_GLOBAL];
    int k = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < n && k < LINK_MAX_GLOBAL; i++) {
            struct cand c;
            if (parse_candidate(urls[i], &c) != 0 || c.relay != pass)
                continue;
            int dup = 0;
            for (int j = 0; j < k; j++)
                dup |= strcmp(list[j].relay ? list[j].ra.url : list[j].ip, c.relay ? c.ra.url : c.ip) == 0
                       && list[j].port == c.port;
            if (!dup)
                list[k++] = c;
        }
    int same = k == l->ng;
    for (int i = 0; same && i < k; i++)
        same = list[i].relay == l->g[i].relay && list[i].port == l->g[i].port
               && strcmp(list[i].relay ? list[i].ra.url : list[i].ip, l->g[i].relay ? l->g[i].ra.url : l->g[i].ip) == 0;
    if (same)
        return 0;
    scope(l, NULL);
    l->cfg.log("discovery: global discovery gives %d usable address(es) of %d", k, n);
    for (int i = 0; i < k; i++)
        l->cfg.log("discovery:   %s%s:%u", list[i].relay ? "relay " : "", list[i].relay ? list[i].ra.ip : list[i].ip,
                   list[i].relay ? list[i].ra.port : list[i].port);
    unscope(l);
    int had = candidates(l);
    memcpy(l->g, list, sizeof list[0] * (size_t)k);
    l->ng = k;
    l->cand_next = 0;
    if (l->state == L_WAIT && (had == 0 || l->failures > 0) && k > 0) {
        if (l->last_end && strcmp(l->last_end, "NO_ADDRESS") == 0)
            l->last_end = NULL;
        l->retry_at = 0;
        l->failures = 0;
        l->again = 1;
    }
    return 1;
}

int link_wants_lookup(const peerlink *l)
{
    return l->dynamic && l->state != L_UP;
}

const unsigned char *link_peer_id(const peerlink *l)
{
    return l->cfg.peer_id;
}

int link_set_address(peerlink *l, const char *ip, unsigned port)
{
    if (!l->dynamic || !ip || !*ip || strlen(ip) >= sizeof l->known_ip || port < 1 || port > 65535)
        return 0;
    if (strcmp(l->known_ip, ip) == 0 && l->known_port == port)
        return 0;
    scope(l, NULL);
    if (l->known_ip[0])
        l->cfg.log("discovery: the device announces %s:%u now (was %s:%u)%s", ip, port, l->known_ip, l->known_port,
                   l->state == L_WAIT ? "" : "; used from the next attempt on");
    else
        l->cfg.log("discovery: the device announces %s:%u", ip, port);
    unscope(l);
    if (!l->known_ip[0] && l->last_end && strcmp(l->last_end, "NO_ADDRESS") == 0)
        l->last_end = NULL; /* no attempt has ended yet */
    memcpy(l->known_ip, ip, strlen(ip) + 1);
    l->known_port = port;
    if (l->state == L_WAIT) {
        /* Somewhere new to try: at once, whatever the pause was. */
        l->retry_at = 0;
        l->failures = 0;
        l->again = 1;
    }
    return 1;
}

void link_get_folder_status(const peerlink *l, size_t i, link_folder_status *out)
{
    const struct lfolder *f = &l->f[i];
    memset(out, 0, sizeof *out);
    out->id = f->cfg->id;
    out->path = f->cfg->path;
    out->why = "";
    if (f->tw) {
        const tw_stats *ts = tw_get_stats(f->tw);
        out->received = ts->downloaded + ts->dirs_made;
        out->deleted = ts->deleted_here;
        out->conflicts = ts->conflicts;
        out->held = ts->held;
        out->failed = ts->failed;
        out->bytes_in = ts->bytes_in;
        out->bytes_out = ts->bytes_out;
    }
    if (f->paused) {
        out->state = "paused-here";
    } else if (f->begun && !f->prepared) {
        out->state = "reading";
    } else if (!f->prepared && f->skip) {
        out->state = "unusable";
        out->why = f->skip;
    } else if (l->state != L_UP || !f->joined) {
        out->state = "idle";
    } else if (!f->active && l->got_cc) {
        out->state = f->inactive_why && strcmp(f->inactive_why, "FOLDER_PAUSED") == 0 ? "paused" : "not-shared";
    } else if (!f->idx.complete) {
        out->state = "index";
    } else if (tw_scanning(f->tw)) {
        out->state = "scanning";
    } else {
        out->state = f->in_sync == 1 ? "in-sync" : "syncing";
    }
}

int link_scan_now(peerlink *l, const char *folder_id)
{
    for (size_t i = 0; i < l->nf; i++)
        if (strcmp(l->f[i].cfg->id, folder_id) == 0) {
            if (l->f[i].tw && l->f[i].prepared)
                tw_scan_soon(l->f[i].tw);
            l->again = 1;
            return 0;
        }
    return -1;
}

int link_pause(peerlink *l, const char *folder_id, int paused)
{
    for (size_t i = 0; i < l->nf; i++) {
        struct lfolder *f = &l->f[i];
        if (strcmp(f->cfg->id, folder_id) != 0)
            continue;
        if (f->paused == !!paused)
            return 1;
        f->paused = !!paused;
        scope(l, f);
        l->cfg.log("engine: folder \"%s\" %s on request", f->cfg->id, paused ? "paused" : "resumed");
        UI(l, "Folder \"%s\" %s", f->cfg->id, paused ? "paused" : "resumed");
        unscope(l);
        if (paused) {
            if (l->state == L_UP && f->joined)
                l->end_request = "FOLDER_PAUSED_HERE";
        } else {
            if (f->tw && f->prepared)
                tw_scan_soon(f->tw); /* what changed here meanwhile */
            if (l->state == L_UP || (l->state == L_CONNECT && !f->prepared))
                l->end_request = "FOLDER_RESUMED";
            else if (l->state == L_PREPARE)
                l->prep_i = 0;
            else if (l->state == L_WAIT)
                l->retry_at = 0, l->failures = 0;
        }
        l->again = 1;
        return 0;
    }
    return -1;
}

long link_sessions(const peerlink *l)
{
    return l->sessions;
}
