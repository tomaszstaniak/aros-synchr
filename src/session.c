#include "session.h"

#include "bep.h"
#include "fetch.h"
#include "frame.h"
#include "idxprog.h"
#include "prof.h"
#include "state.h"
#include "store.h"
#include "tw.h"

#include <openssl/rand.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

#define CLIENT_NAME "aros-syncthing"
#define CLIENT_VERSION "0.0.1-poc"

/* Limits for what the peer may send. Syncthing allows 500 MB; an index
 * batch is far smaller, and a Response carries at most one 16 MiB block
 * plus a few bytes of framing. */
#define MAX_WIRE_MESSAGE (17u << 20)
#define MAX_DECOMPRESSED (64u << 20)

/* Syncthing gives the Hello exchange 20 s (lib/connections/service.go). */
#define HELLO_SECONDS 20
/* Syncthing pings every 90 s and drops a silent connection after 300 s
 * (lib/protocol). 30 s, because on the ABIv11 QEMU/TCG guest
 * CLOCK_MONOTONIC was measured running about 3.5x slower than real time
 * (2026-10-05): even at 5x a ping still goes out within 150 real seconds. */
#define PING_SECONDS 30
/* More unsent bytes than this: no further message of the peer is taken
 * up (each Request adds a block to send) until the socket has drained. */
#define OUT_HOLD_BYTES (4u << 20)

struct session {
    const session_config *cfg;
    tls_conn *conn;
    frame_rx rx;
    unsigned char my_id[DEVID_BYTES];
    net_ms end, next_ping;
    int got_cc, folder_offered, we_are_listed, peer_listed;
    uint32_t folder_stop_reason;
    int got_index;
    idxprog idx;
    long index_files;      /* entries received until the index was complete */
    int64_t msg_max_seq;   /* highest file sequence in the message being decoded */
    int index_reset;       /* a later ClusterConfig announced another index ID */
    long files_reported;
    int peer_gone; /* the peer closed or sent Close: nothing more to send */
    store *store;
    fetch *fetch;
    char peer_version[120]; /* quoted Hello client_version, same size as its buffer */
    char folder_label[100];
    long index_entries;
    const char *offer_outcome; /* set when fetch_offer stops the session */
    tw *tw;                    /* two-way mode */
    int self_listed;           /* the peer's ClusterConfig lists us for the folder */
    uint64_t self_index_id;    /* ... with this index ID of ours */
    int64_t self_max_seq;      /* ... known up to this sequence */
    int tw_complete_told;
    int closing;  /* close_session set a short deadline of its own */
};

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

static int slice_eq(pb_slice s, const char *t)
{
    return s.len == strlen(t) && memcmp(s.data, t, s.len) == 0;
}

static enum tls_result send_frames(struct session *s, const pb_writer *w)
{
    if (w->error)
        return TLS_E_INTERNAL;
    /* Kept by the connection and written by the session loop as the
     * socket takes it: nothing here waits for the peer. */
    return tls_queue(s->conn, w->data, w->len);
}

static enum tls_result send_message(struct session *s, uint32_t type, const pb_writer *body)
{
    pb_writer f;
    pb_writer_init(&f, MAX_WIRE_MESSAGE + 64);
    frame_put_message(&f, type, body);
    enum tls_result r = send_frames(s, &f);
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

static int on_file(void *ctx, const bep_file *f)
{
    struct session *s = ctx;
    char name[300];
    quote(name, sizeof name, f->name);
    s->cfg->log("file: %-7s size=%lld seq=%lld deleted=%d invalid=%d blocks=%lu modified=%lld name=\"%s\"",
                file_type(f->type), (long long)f->size, (long long)f->sequence, f->deleted, f->invalid,
                (unsigned long)f->blocks, (long long)f->modified_s, name);
    s->files_reported++;
    if (f->sequence > s->msg_max_seq)
        s->msg_max_seq = f->sequence;
    if (s->fetch && !s->offer_outcome && (s->offer_outcome = fetch_offer(s->fetch, f)) != NULL)
        return 1;
    if (s->tw && !s->offer_outcome && (s->offer_outcome = tw_offer(s->tw, f)) != NULL)
        return 1;
    return 0;
}

static int cc_folder(void *ctx, const bep_cc_folder *f)
{
    struct session *s = ctx;
    char id[120], label[120];
    quote(id, sizeof id, f->id);
    quote(label, sizeof label, f->label);
    s->cfg->log("cc: folder \"%s\" label \"%s\" type %u stop_reason %u", id, label, f->type, f->stop_reason);
    if (slice_eq(f->id, s->cfg->folder_id)) {
        s->folder_offered = 1;
        s->folder_stop_reason = f->stop_reason;
        quote(s->folder_label, sizeof s->folder_label, f->label.len ? f->label : f->id);
    }
    return 0;
}

static int cc_device(void *ctx, const bep_cc_folder *f, const bep_cc_device *d)
{
    struct session *s = ctx;
    char text[DEVID_TEXT_SIZE] = "(invalid)";
    if (d->id_valid)
        devid_format(d->id, text);
    s->cfg->log("cc:   device %.7s max_sequence %lld index_id %016llx%s%s", text,
                (long long)d->max_sequence, (unsigned long long)d->index_id,
                d->encryption_token_len ? " encrypted" : "", d->introducer ? " introducer" : "");
    if (slice_eq(f->id, s->cfg->folder_id) && d->id_valid && memcmp(d->id, s->my_id, DEVID_BYTES) == 0) {
        s->we_are_listed = 1;
        s->self_index_id = d->index_id;
        s->self_max_seq = d->max_sequence;
    }
    /* The peer's own entry: how far its index for the folder goes, and
     * under which index ID (a later ClusterConfig with another one means
     * the peer started its index again). */
    if (slice_eq(f->id, s->cfg->folder_id) && d->id_valid && memcmp(d->id, s->cfg->peer_id, DEVID_BYTES) == 0) {
        s->peer_listed = 1;
        if (idxprog_cc(&s->idx, d->max_sequence, d->index_id) == IDX_RESET)
            s->index_reset = 1;
    }
    return 0;
}

/* Handles one complete message. Returns NULL to continue, "" when the
 * scenario is complete, or a failure code. */
static const char *handle(struct session *s, uint32_t type, const unsigned char *body, size_t len)
{
    if (!s->got_cc && type != BEP_CLUSTER_CONFIG) {
        /* Syncthing itself refuses anything before the ClusterConfig. */
        s->cfg->log("bep: message type %u before ClusterConfig", type);
        return "PROTOCOL_ERROR";
    }
    switch (type) {
    case BEP_CLUSTER_CONFIG:
        if (bep_decode_cluster_config(body, len, s, cc_folder, cc_device) != 0) {
            s->cfg->log("bep: malformed ClusterConfig");
            return "PROTOCOL_ERROR";
        }
        s->got_cc = 1;
        if (s->index_reset) {
            s->cfg->log("cc: the peer announces another index ID for the folder; its index starts again, "
                        "which this client does not support within a session");
            return "INDEX_RESET_UNSUPPORTED";
        }
        if (!s->folder_offered) {
            s->cfg->log("cc: the peer does not share folder \"%s\" with us", s->cfg->folder_id);
            return "FOLDER_NOT_SHARED";
        }
        s->cfg->log("cc: folder \"%s\" offered%s", s->cfg->folder_id,
                    s->we_are_listed ? ", our device listed" : ", our device NOT listed");
        if (s->folder_stop_reason != 0) {
            /* A paused folder sends no index (FolderStopReason PAUSED). */
            s->cfg->log("cc: the peer's folder is stopped (stop_reason %u)", s->folder_stop_reason);
            return "FOLDER_PAUSED";
        }
        if (s->peer_listed)
            s->cfg->log("cc: the peer's index goes up to sequence %lld", (long long)s->idx.target);
        else
            s->cfg->log("cc: the peer does not list itself for the folder; completeness of its index "
                        "cannot be established");
        if (s->idx.complete && s->cfg->ui)
            s->cfg->ui("Index received: the folder is empty on the peer");
        if (s->tw) {
            const char *r = tw_peer_cc(s->tw, s->we_are_listed, s->self_index_id, s->self_max_seq);
            if (r)
                return r;
        }
        if (s->cfg->ui)
            s->cfg->ui("Folder \"%s\" is shared with this AROS device", s->folder_label);
        return NULL;

    case BEP_INDEX:
    case BEP_INDEX_UPDATE: {
        pb_slice folder;
        int64_t last, prev;
        const char *kind = type == BEP_INDEX ? "Index" : "IndexUpdate";
        /* Validate first (decode with no callback), report only a valid message. */
        if (bep_decode_index(body, len, &folder, &last, &prev, NULL, NULL) != 0) {
            s->cfg->log("bep: malformed %s", kind);
            return "PROTOCOL_ERROR";
        }
        if (!slice_eq(folder, s->cfg->folder_id)) {
            char f[120];
            quote(f, sizeof f, folder);
            s->cfg->log("bep: %s for folder \"%s\" ignored", kind, f);
            return NULL;
        }
        s->cfg->log("bep: %s, %lu bytes, last_sequence %lld, prev_sequence %lld", kind,
                    (unsigned long)len, (long long)last, (long long)prev);
        /* The chain is checked before any file of the message is acted on. */
        int ic = idxprog_check(&s->idx, type == BEP_INDEX_UPDATE, prev);
        if (ic == IDX_BAD_CHAIN) {
            s->cfg->log("bep: %s follows on from sequence %lld, but %lld was the last one received; "
                        "the peer's index does not hang together; none of its entries used",
                        kind, (long long)prev, (long long)s->idx.seen);
            return "INDEX_SEQUENCE_ERROR";
        }
        if (ic == IDX_RESET) {
            s->cfg->log("bep: a second full Index: the peer started its index again, which this client does "
                        "not support within a session; none of its entries used");
            return "INDEX_RESET_UNSUPPORTED";
        }
        long before = s->files_reported;
        int was_complete = s->idx.complete;
        s->msg_max_seq = 0;
        PROF_BEGIN(ti);
        bep_decode_index(body, len, &folder, &last, &prev, s, on_file);
        PROF_END(PROF_INDEX, ti);
        s->cfg->log("bep: %s carried %ld file(s)", kind, s->files_reported - before);
        if (s->offer_outcome)
            return s->offer_outcome;
        if (type == BEP_INDEX)
            s->got_index = 1;
        int is_update = idxprog_message(&s->idx, type == BEP_INDEX_UPDATE, prev, last, s->msg_max_seq)
                        == IDX_UPDATE;
        if (!was_complete) {
            s->index_files += s->files_reported - before;
            if (s->idx.complete) {
                s->cfg->log("bep: index complete: sequence %lld reached (%lld at connection), %ld entr%s in %ld "
                            "message(s)", (long long)s->idx.seen, (long long)s->idx.target, s->index_files,
                            s->index_files == 1 ? "y" : "ies", s->idx.messages);
                if (s->cfg->ui)
                    s->cfg->ui("Index received: %ld entries", s->index_files);
            } else if (s->idx.have_target) {
                s->cfg->log("bep: index so far up to sequence %lld of %lld", (long long)s->idx.seen,
                            (long long)s->idx.target);
            }
        } else if (is_update) {
            s->cfg->log("bep: update %d after the complete index", s->idx.updates);
        }
        return NULL; /* completion is decided in the main loop */
    }

    case BEP_PING:
        return NULL;

    case BEP_CLOSE: {
        pb_slice reason;
        char r[200] = "";
        if (bep_decode_close(body, len, &reason) == 0)
            quote(r, sizeof r, reason);
        s->cfg->log("bep: peer sent Close: \"%s\"", r);
        s->peer_gone = 1;
        return "PEER_CLOSED";
    }

    case BEP_REQUEST: {
        if (s->tw) {
            bep_request q;
            if (bep_decode_request_full(body, len, &q) != 0)
                return "PROTOCOL_ERROR";
            return tw_request(s->tw, &q);
        }
        int32_t id;
        pb_slice folder, name;
        if (bep_decode_request(body, len, &id, &folder, &name) != 0)
            return "PROTOCOL_ERROR";
        /* This client does not serve data: the PoC's peer folder is
         * send-only and never asks; answer anything else with an error. */
        pb_writer w;
        pb_writer_init(&w, 64);
        bep_encode_response_error(&w, id, 2 /* ERROR_CODE_NO_SUCH_FILE */);
        enum tls_result r = send_message(s, BEP_RESPONSE, &w);
        pb_writer_free(&w);
        s->cfg->log("bep: Request %d answered with NO_SUCH_FILE", id);
        return r == TLS_OK ? NULL : "IO_ERROR";
    }

    case BEP_DOWNLOAD_PROGRESS:
        return NULL;

    case BEP_RESPONSE: {
        int32_t id;
        pb_slice data;
        uint32_t code;
        if (s->tw) {
            if (bep_decode_response(body, len, &id, &data, &code) != 0) {
                s->cfg->log("bep: malformed Response");
                return "PROTOCOL_ERROR";
            }
            return tw_response(s->tw, id, data, code);
        }
        if (!s->fetch) {
            s->cfg->log("bep: Response without a Request");
            return "PROTOCOL_ERROR";
        }
        if (bep_decode_response(body, len, &id, &data, &code) != 0) {
            s->cfg->log("bep: malformed Response");
            return "PROTOCOL_ERROR";
        }
        return fetch_response(s->fetch, id, data, code);
    }

    default:
        /* Syncthing skips message types it does not know, for forward
         * compatibility (errUnknownMessage in lib/protocol); so do we. */
        s->cfg->log("bep: unknown message type %u skipped", type);
        return NULL;
    }
}

static void close_session(struct session *s, const char *reason)
{
    s->closing = 1;
    if (s->conn && s->peer_gone) {
        tls_close(s->conn, net_now() + 2000);
        s->conn = NULL;
    }
    if (s->conn) {
        pb_writer w;
        pb_writer_init(&w, 256);
        bep_encode_close(&w, reason);
        /* Best effort, with a short deadline of its own: what is still
         * queued (the last announcements) goes out first, then the Close. */
        net_ms save = s->end;
        s->end = net_now() + 3000;
        enum tls_result r = send_message(s, BEP_CLOSE, &w);
        if (r == TLS_OK)
            r = tls_flush(s->conn, s->end);
        s->cfg->log("bep: sent Close \"%s\": %s", reason, tls_result_name(r));
        s->end = save;
        pb_writer_free(&w);
        /* Let the peer act on the Close before we disconnect: Syncthing
         * reads the next message while its dispatcher handles the previous
         * one, so an immediate EOF wins the race and it logs "EOF" instead
         * of our reason (lib/protocol readerLoop). Wait for it to close,
         * discarding anything else, for at most 2 s. */
        if (r == TLS_OK) {
            unsigned char sink[4096];
            size_t got;
            int closed = 0;
            net_ms until = net_now() + 2000;
            while (tls_read(s->conn, sink, sizeof sink, &got, &closed, until) == TLS_OK)
                ;
            s->cfg->log("bep: peer %s after our Close", closed ? "closed the connection" : "did not close in 2 s");
        }
        tls_close(s->conn, net_now() + 2000);
        s->conn = NULL;
    }
    frame_rx_free(&s->rx);
    fetch_free(s->fetch);
    s->fetch = NULL;
    tw_free(s->tw);
    s->tw = NULL;
    store_close(s->store);
    s->store = NULL;
}

static enum tls_result fetch_send(void *ctx, uint32_t type, const pb_writer *body)
{
    return send_message(ctx, type, body);
}

int session_run(const session_config *cfg, const tls_identity *id, const char **result)
{
    struct session s;
    memset(&s, 0, sizeof s);
    s.cfg = cfg;
    tls_identity_devid(id, s.my_id);
    s.end = net_now() + cfg->seconds * 1000;
    frame_rx_init(&s.rx, MAX_WIRE_MESSAGE, MAX_DECOMPRESSED);
    if (cfg->dest) {
        char why[300];
        if (!(s.store = store_open(cfg->dest, why, sizeof why))) {
            cfg->log("sync: %s", why);
            *result = "BAD_DESTINATION";
            frame_rx_free(&s.rx);
            return 1;
        }
        char spath[1100];
        if (cfg->two_way) {
            if (!cfg->identity_dir || state_path(cfg->identity_dir, cfg->folder_id, spath, sizeof spath) != 0) {
                *result = "BAD_ARGUMENT";
                close_session(&s, "");
                return 1;
            }
            cfg->log("sync: two-way, state file %s, folder %s", spath, cfg->dest);
            tw_config tc = { cfg->folder_id, cfg->dest, spath, cfg->peer_id, s.my_id, s.store, cfg->log, cfg->ui,
                             fetch_send, &s, cfg->rescan_seconds, TW_HASH_AUTO,
                             0, NULL, 0 };
            const char *pr;
            if (!(s.tw = tw_new(&tc))) {
                *result = "OUT_OF_MEMORY";
                close_session(&s, "");
                return 1;
            }
            tw_attach(s.tw); /* one connection for the whole run */
            if ((pr = tw_prepare(s.tw)) != NULL) {
                *result = pr;
                close_session(&s, "");
                return 1;
            }
        }
        fetch_state_opts so = { NULL, cfg->dest, cfg->peer_id, cfg->state_fault };
        if (cfg->identity_dir) {
            if (state_path(cfg->identity_dir, cfg->folder_id, spath, sizeof spath) != 0) {
                *result = "BAD_ARGUMENT";
                close_session(&s, "");
                return 1;
            }
            so.state_path = spath;
            cfg->log("sync: state file %s", spath);
        }
        if (!cfg->two_way
            && (!(s.fetch = fetch_new(cfg->folder_id, s.store, cfg->log, cfg->ui, fetch_send, &s,
                                      cfg->abort_after_blocks, &so))
                || fetch_prepare(s.fetch) != 0)) {
            *result = "OUT_OF_MEMORY";
            close_session(&s, "");
            return 1;
        }
        if (cfg->commit_hook && s.fetch) {
            fetch_set_commit_hook(s.fetch, cfg->commit_hook);
            cfg->log("sync: TEST HOOK: each commit waits for a go-ahead in %s", cfg->commit_hook);
        }
        cfg->log("sync: writing into %s", cfg->dest);
    }

    /* Wall-clock time alongside the monotonic milliseconds of the log, so
     * a slow or fast guest clock shows up in the report. */
    cfg->log("session: start, wall clock %lld", (long long)time(NULL));
    enum tls_result tr = tls_connect(id, cfg->peer_ip, cfg->peer_port, cfg->peer_id, s.end, cfg->log, &s.conn);
    if (tr != TLS_OK) {
        *result = tls_result_name(tr);
        close_session(&s, "");
        return 1;
    }

    /* Hello, and immediately after it our ClusterConfig and an empty Index:
     * the peer may only treat anything else as valid after both sides have
     * sent their ClusterConfig. */
    uint64_t my_index_id = 0;
    while (my_index_id == 0)
        if (RAND_bytes((unsigned char *)&my_index_id, sizeof my_index_id) != 1) {
            *result = "RNG_FAILED";
            close_session(&s, "internal error");
            return 1;
        }
    pb_writer hello, out, body;
    pb_writer_init(&hello, BEP_HELLO_MAX);
    bep_encode_hello(&hello, cfg->device_name, CLIENT_NAME, CLIENT_VERSION,
                     (int64_t)time(NULL) * 1000000000);
    pb_writer_init(&out, MAX_WIRE_MESSAGE);
    frame_put_hello(&out, &hello);
    tr = send_frames(&s, &out);
    pb_writer_free(&hello);
    pb_writer_free(&out);
    if (tr != TLS_OK) {
        *result = tls_result_name(tr);
        close_session(&s, "error");
        return 1;
    }

    const char *outcome = NULL;
    net_ms hello_deadline = net_now() + HELLO_SECONDS * 1000;
    int got_hello = 0;
    unsigned char buf[FRAME_FEED_MAX];
    s.next_ping = net_now() + PING_SECONDS * 1000;

    while (!outcome) {
        enum frame_status st;
        int backlog = 0; /* messages already received were left for later */
        while (!outcome) {
            if (tls_pending(s.conn) > OUT_HOLD_BYTES) {
                backlog = 1;
                break;
            }
            if ((st = frame_rx_next(&s.rx)) == FRAME_NEED_MORE)
                break;
            if (st == FRAME_ERROR) {
                cfg->log("bep: framing error: %s", s.rx.error);
                outcome = "PROTOCOL_ERROR";
            } else if (st == FRAME_HELLO) {
                bep_hello h;
                char dn[120], cn[120], cv[120];
                if (bep_decode_hello(s.rx.body, s.rx.body_len, &h) != 0) {
                    outcome = "PROTOCOL_ERROR";
                    break;
                }
                quote(dn, sizeof dn, h.device_name);
                quote(cn, sizeof cn, h.client_name);
                quote(cv, sizeof cv, h.client_version);
                cfg->log("bep: peer Hello: device \"%s\", client \"%s\" \"%s\"", dn, cn, cv);
                got_hello = 1;
                snprintf(s.peer_version, sizeof s.peer_version, "%s", cv);
                if (cfg->ui) {
                    char id[DEVID_TEXT_SIZE];
                    devid_format(cfg->peer_id, id);
                    cfg->ui("Connected to Syncthing %s at %s:%u over TLS 1.3", s.peer_version, cfg->peer_ip,
                            cfg->peer_port);
                    cfg->ui("Peer identity verified (device %.7s-...)", id);
                }

                unsigned char peer[DEVID_BYTES];
                memcpy(peer, cfg->peer_id, DEVID_BYTES);
                int64_t my_max = 0;
                if (s.tw)
                    tw_cc_self(s.tw, &my_index_id, &my_max);
                bep_cc_device_out devs[2] = {
                    { peer, "", 0, 0 },                                  /* nothing known: full index */
                    { s.my_id, cfg->device_name, my_max, my_index_id },  /* our index */
                };
                pb_writer_init(&out, MAX_WIRE_MESSAGE);
                pb_writer_init(&body, MAX_WIRE_MESSAGE);
                bep_encode_cluster_config(&body, cfg->folder_id, "",
                                          s.tw ? BEP_FOLDER_SEND_RECEIVE : BEP_FOLDER_RECEIVE_ONLY, devs, 2);
                frame_put_message(&out, BEP_CLUSTER_CONFIG, &body);
                pb_writer_free(&body);
                if (!s.fetch && !s.tw) {
                    pb_writer_init(&body, 4096);
                    bep_encode_empty_index(&body, cfg->folder_id);
                    frame_put_message(&out, BEP_INDEX, &body);
                    pb_writer_free(&body);
                }
                tr = send_frames(&s, &out);
                pb_writer_free(&out);
                if (tr != TLS_OK) {
                    outcome = tls_result_name(tr);
                } else if (s.tw) {
                    cfg->log("bep: sent ClusterConfig (folder \"%s\", send & receive, index_id %016llx, max_sequence "
                             "%lld); our index follows the peer's ClusterConfig",
                             cfg->folder_id, (unsigned long long)my_index_id, (long long)my_max);
                } else if (s.fetch) {
                    /* A full index of what is verified here (possibly empty). */
                    cfg->log("bep: sent ClusterConfig (folder \"%s\", receive-only, index_id %016llx)",
                             cfg->folder_id, (unsigned long long)my_index_id);
                    outcome = fetch_send_index(s.fetch);
                } else {
                    cfg->log("bep: sent ClusterConfig (folder \"%s\", receive-only, index_id %016llx) and an empty Index",
                             cfg->folder_id, (unsigned long long)my_index_id);
                }
            } else {
                if (s.rx.type != BEP_PING)
                    cfg->log("bep: received type %u, %lu bytes on the wire, %s, %lu bytes decoded",
                             s.rx.type, (unsigned long)s.rx.wire_len,
                             s.rx.compression == BEP_COMPRESSION_LZ4 ? "LZ4" : "uncompressed",
                             (unsigned long)s.rx.body_len);
                outcome = handle(&s, s.rx.type, s.rx.body, s.rx.body_len);
            }
        }
        if (outcome)
            break;

        net_ms now = net_now();
        net_ms fetch_wake = now + 3600 * 1000;
        if (s.fetch && s.got_cc && (outcome = fetch_step(s.fetch, now, &fetch_wake)) != NULL)
            break;
        if (s.tw && s.got_cc && s.idx.complete && !s.tw_complete_told) {
            s.tw_complete_told = 1;
            if ((outcome = tw_peer_index_complete(s.tw)) != NULL)
                break;
        }
        if (s.tw && s.got_cc && (outcome = tw_step(s.tw, now, &fetch_wake)) != NULL)
            break;
        if (s.tw && s.idx.complete && s.idx.updates >= cfg->want_updates && tw_settled(s.tw)) {
            outcome = "";
            break;
        }
        /* Done only when the whole index known at connection has arrived,
         * not merely its first batch (idxprog.h). */
        if (!s.tw && s.idx.complete && s.idx.updates >= cfg->want_updates && (!s.fetch || fetch_idle(s.fetch))) {
            outcome = "";
            break;
        }
        if (!got_hello && now >= hello_deadline) {
            outcome = "HELLO_TIMEOUT";
            break;
        }
        if (now >= s.end) {
            outcome = "TIME_LIMIT";
            break;
        }
        if (now >= s.next_ping && got_hello) {
            pb_writer empty;
            pb_writer_init(&empty, 16);
            tr = send_message(&s, BEP_PING, &empty);
            pb_writer_free(&empty);
            if (tr != TLS_OK) {
                outcome = tls_result_name(tr);
                break;
            }
            s.next_ping = now + PING_SECONDS * 1000;
            cfg->log("bep: sent Ping");
        }
        net_ms wait_until = got_hello ? s.next_ping : hello_deadline;
        if (wait_until > fetch_wake)
            wait_until = fetch_wake;
        if (wait_until > s.end)
            wait_until = s.end;

        size_t got = 0;
        int closed = 0;
        if ((tr = tls_flush_some(s.conn)) != TLS_OK) {
            outcome = tls_result_name(tr);
            break;
        }
        size_t unsent = tls_pending(s.conn);
        int hold = unsent > OUT_HOLD_BYTES;
        if (backlog && !hold)
            continue; /* room again: the messages left above come first */
        tr = tls_wait(s.conn, !hold, wait_until);
        if (tr == TLS_E_TIMEOUT)
            continue; /* re-evaluates ping, Hello and overall deadlines */
        if (tr == TLS_OK && !hold)
            tr = tls_read_some(s.conn, buf, sizeof buf, &got, &closed);
        if (tr != TLS_OK) {
            outcome = closed ? "PEER_DISCONNECTED" : tls_result_name(tr);
            s.peer_gone = closed;
            break;
        }
        if (got > 0 && frame_rx_feed(&s.rx, buf, got) != 0) {
            outcome = "PROTOCOL_ERROR";
            cfg->log("bep: receive buffer limit exceeded");
        }
    }

    if (strcmp(outcome, "TIME_LIMIT") == 0 && s.got_cc && !s.idx.complete) {
        if (s.idx.have_target)
            cfg->log("bep: index incomplete: up to sequence %lld of %lld received", (long long)s.idx.seen,
                     (long long)s.idx.target);
        outcome = "INDEX_INCOMPLETE";
    }
    int ok = outcome[0] == 0;
    if (s.fetch) {
        fetch_flush(s.fetch); /* failures are counted in state_failures */
        const fetch_stats *fs = fetch_get_stats(s.fetch);
        cfg->log("sync: summary: %ld file(s) written (%ld replacing an earlier version), %ld dir(s), "
                 "%ld request(s), %lld bytes received, %ld already present, %ld adopted, %ld conflict(s), "
                 "%ld failed, %ld rejected, %ld unsupported, %ld skipped, %ld announced, %ld state write(s), "
                 "%ld state write failure(s)",
                 fs->files_done, fs->files_replaced, fs->dirs_done, fs->requests, fs->bytes, fs->present,
                 fs->adopted, fs->conflicts, fs->failed, fs->rejected, fs->unsupported, fs->skipped,
                 fs->announced, fs->state_saves, fs->state_failures);
        const char *fr = fetch_result(s.fetch);
        if (ok && fr) {
            ok = 0;
            outcome = fr;
        }
        if (cfg->ui && !ok && s.got_cc && !s.idx.complete)
            cfg->ui("Not complete: only part of the peer's index arrived; nothing is reported as done");
        if (cfg->ui && s.idx.complete) {
            char sz[32];
            cfg->ui("Done: %ld file%s downloaded (%s in %ld request%s); %ld already here and verified",
                    fs->files_done, fs->files_done == 1 ? "" : "s", fetch_size_text(fs->bytes, sz, sizeof sz),
                    fs->requests, fs->requests == 1 ? "" : "s", fs->present + fs->adopted);
            cfg->ui("%ld item%s reported to Syncthing as present; %ld conflict%s, %ld failed, %ld not received, "
                    "%ld skipped (deletions, ignored by the peer)",
                    fs->announced, fs->announced == 1 ? "" : "s", fs->conflicts, fs->conflicts == 1 ? "" : "s",
                    fs->failed, fs->rejected + fs->unsupported, fs->skipped);
            if (fs->conflicts > 0)
                cfg->ui("Not fully synchronised: files changed here were kept and not overwritten");
            if (fs->rejected + fs->unsupported > 0)
                cfg->ui("Not fully synchronised: %ld item%s could not be received here (see the log)",
                        fs->rejected + fs->unsupported, fs->rejected + fs->unsupported == 1 ? "" : "s");
            cfg->ui("Files are in %s", cfg->dest);
        }
    }
    if (s.tw) {
        tw_flush(s.tw);
        const tw_stats *ts = tw_get_stats(s.tw);
        cfg->log("tw: summary: here %ld new, %ld changed, %ld deleted; from the peer %ld file(s) written, %ld dir(s), "
                 "%ld recorded, %ld adopted, %ld deleted here; %ld conflict copie(s) made here, %ld dir(s) kept; "
                 "%ld leftover(s) removed, %ld restored; %ld block(s) served, %ld request(s) refused, %lld bytes in, "
                 "%lld bytes out; %ld held, %ld unsupported, %ld rejected, %ld failed, %ld unchecked; %ld announced, "
                 "%ld state write(s), %ld failure(s)",
                 ts->local_new, ts->local_changed, ts->local_deleted, ts->downloaded, ts->dirs_made, ts->recorded,
                 ts->adopted, ts->deleted_here, ts->conflicts, ts->dirs_kept, ts->leftovers_removed,
                 ts->leftovers_restored, ts->served_blocks, ts->refused_requests, ts->bytes_in, ts->bytes_out, ts->held,
                 ts->unsupported, ts->rejected, ts->failed, ts->unchecked, ts->announced, ts->state_saves,
                 ts->state_failures);
        const char *tr2 = tw_result(s.tw);
        if (!tw_state_saved(s.tw))
            cfg->log("tw: the state file is not complete at the end of this session");
        if (ok && tr2) {
            ok = 0;
            outcome = tr2;
        }
        if (cfg->ui)
            cfg->ui("%s: %ld new, %ld changed, %ld deleted here; %ld received, %ld deleted from the peer; %ld conflict "
                    "copie(s); %ld held",
                    tr2 ? "Not fully in sync" : "In sync", ts->local_new, ts->local_changed, ts->local_deleted,
                    ts->downloaded + ts->dirs_made, ts->deleted_here, ts->conflicts, ts->held);
    }
    if (!ok && strcmp(outcome, "TIME_LIMIT") == 0 && s.idx.complete)
        cfg->log("bep: time limit reached after the complete index; %d of %d wanted update(s) seen",
                 s.idx.updates, cfg->want_updates);
    cfg->log("bep: summary: cc %s, folder %s, index %s (sequence %lld; %lld at connection; %ld message(s)), updates %d, "
             "files reported %ld",
             s.got_cc ? "yes" : "no", s.folder_offered ? "offered" : "not offered",
             s.idx.complete ? "complete" : s.got_index ? "INCOMPLETE" : "not received", (long long)s.idx.seen,
             (long long)s.idx.target, s.idx.messages, s.idx.updates, s.files_reported);
    char prof[600];
    prof_format(prof, sizeof prof);
    cfg->log("profile: %s", prof);
    *result = ok ? "PASS" : outcome;
    cfg->log("session: end, wall clock %lld", (long long)time(NULL));
    close_session(&s, ok ? "aros-syncthing: test finished" : "aros-syncthing: stopping");
    return ok ? 0 : 1;
}
