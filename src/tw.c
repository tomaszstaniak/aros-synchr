#include "tw.h"

#include "db.h"
#include "decide.h"
#include "finfo.h"
#include "scan.h"
#include "state.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Per request, in guest time; the session's overall limit still applies. */
#define REQUEST_SECONDS 60
/* Body size at which an index is split into another message. */
#define INDEX_BATCH (1u << 20)
/* State writes: at most this often while changes arrive; after a failure
 * the next attempt waits this long, doubled up to the maximum. */
#define SAVE_MS 5000
#define RETRY_MS 5000
#define RETRY_MAX_MS 60000
/* FAT keeps modification dates in 2 s steps: a file dated within this
 * window of the moment it was hashed may change again without its date
 * changing, so it is hashed again on the next scan. */
#define DATE_WINDOW_S 2

/* db_entry.status of our records */
enum { TS_OK = 0, TS_UNKNOWN, TS_PROVISIONAL, TS_HELD };
/* db_entry.status of the peer's entries */
enum { PS_OPEN = 0, PS_BLOCKED, PS_DEFERRED /* arrived during a scan: decided when it ends */ };

/* A scan in progress (rescan_step). */
enum { RS_IDLE = 0, RS_LIST, RS_OBSERVE, RS_HASH, RS_MISSING };
struct rescan {
    int phase;
    scan_run *run;
    scan_result sr;
    size_t i;          /* the item being looked at */
    int64_t now;
    finfo_hasher *h;   /* its content being hashed */
    int64_t ms;        /* ... and its date, read before the hash */
    int32_t mns;
    int relisted;      /* listed again after the clean-up of leftovers */
    char **present;    /* the new list of unrecorded names, in use once the scan ends */
    size_t npresent;
    long deferred;     /* entries of the peer waiting for the end of the scan */
    long slices;
    net_ms started;
};

struct dl {
    char *name;
    finfo r;      /* the peer's entry to install (or its deletion) */
    int conflict; /* ours becomes a conflict copy first */
};

struct tw {
    char *folder, *root, *state_path;
    unsigned char peer_id[DEVID_BYTES], my_id[DEVID_BYTES];
    uint64_t me;
    store *st;
    tls_log_fn log, ui;
    tw_send_fn send;
    void *ctx;
    long rescan_seconds;
    net_ms next_rescan;
    int hash_policy, trust_dates;
    long slice_ms;       /* 0: a scan runs to its end in one call */
    int (*stop)(void);
    struct rescan rs;

    db d;     /* our records */
    db peer;  /* the peer's latest entry per name (its sequence) */
    char **present; /* on disk at the last scan, not recorded (unsupported, not accepted) */
    size_t npresent;

    int rebuild;       /* versions of our own were made up: resolve against the peer's index first */
    int peer_complete; /* the peer's index (as at connection) is here */
    int have_peer_cc, index_started;
    int64_t start_seq, saved_seq, announced_seq;

    int dirty, state_lost;
    long fail_run;
    net_ms saved_at, gap;

    struct dl *q;     /* downloads, in order */
    size_t n, cap;
    struct dl *del;   /* deletions, applied once the downloads are done */
    size_t ndel, capdel;
    int scanned;      /* the first scan (and the clean-up after an interruption) is done */
    store_file *file;
    size_t next_block;
    int waiting;
    int32_t req_id, next_id;
    int prep_fresh;   /* tw_prepare_begin found no usable state: it is written even when nothing changed */
    net_ms deadline;

    int attached;     /* between tw_attach and tw_detach */
    tw_stats stats;
    tw_stats base;    /* stats at the last tw_detach */
};

#define UI(t, ...) do { if ((t)->ui) (t)->ui(__VA_ARGS__); } while (0)

static int64_t now_unix(void)
{
    return (int64_t)time(NULL);
}

tw *tw_new(const tw_config *c)
{
    tw *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->folder = strdup(c->folder_id);
    t->root = strdup(c->root);
    t->state_path = strdup(c->state_path);
    if (!t->folder || !t->root || !t->state_path) {
        tw_free(t);
        return NULL;
    }
    memcpy(t->peer_id, c->peer_id, DEVID_BYTES);
    memcpy(t->my_id, c->my_id, DEVID_BYTES);
    t->me = vv_short_id(t->my_id);
    t->st = c->st;
    t->log = c->log;
    t->ui = c->ui;
    t->send = c->send;
    t->ctx = c->ctx;
    t->rescan_seconds = c->rescan_seconds;
    t->hash_policy = c->hash_policy;
    t->slice_ms = c->slice_ms;
    t->stop = c->stop;
    t->next_id = c->id_base + 1;
    return t;
}

static void free_dl(struct dl *x)
{
    free(x->name);
    finfo_free(&x->r);
}

/* Drops a scan in progress; what it recorded so far stays. */
static void rescan_drop(tw *t)
{
    struct rescan *rs = &t->rs;
    scan_end(rs->run);
    finfo_hash_abort(rs->h);
    if (rs->phase != RS_IDLE)
        scan_free(&rs->sr);
    for (size_t i = 0; i < rs->npresent; i++)
        free(rs->present[i]);
    free(rs->present);
    long deferred = rs->deferred;
    memset(rs, 0, sizeof *rs);
    rs->deferred = deferred;
}

void tw_free(tw *t)
{
    if (!t)
        return;
    store_abort(t->file);
    rescan_drop(t);
    for (size_t i = 0; i < t->n; i++)
        free_dl(&t->q[i]);
    free(t->q);
    for (size_t i = 0; i < t->ndel; i++)
        free_dl(&t->del[i]);
    free(t->del);
    for (size_t i = 0; i < t->npresent; i++)
        free(t->present[i]);
    free(t->present);
    db_free(&t->d);
    db_free(&t->peer);
    free(t->folder);
    free(t->root);
    free(t->state_path);
    free(t);
}

const tw_stats *tw_get_stats(const tw *t) { return &t->stats; }

void tw_get_session_stats(const tw *t, tw_stats *o)
{
    const tw_stats *s = &t->stats, *b = &t->base;
    *o = *s;
#define D(f) o->f = s->f - b->f
    D(local_new); D(local_changed); D(local_deleted); D(deleted_here); D(conflicts); D(dirs_kept);
    D(leftovers_removed); D(leftovers_restored); D(downloaded); D(dirs_made); D(recorded); D(adopted);
    D(served_blocks); D(refused_requests); D(held); D(unsupported); D(rejected); D(failed); D(announced);
    D(state_saves); D(state_failures); D(bytes_in); D(bytes_out);
#undef D
}

int tw_state_lost(const tw *t) { return t->state_lost; }
int tw_attached(const tw *t) { return t->attached; }

void tw_attach(tw *t)
{
    if (t->attached)
        tw_detach(t);
    t->attached = 1;
}

void tw_detach(tw *t)
{
    store_abort(t->file);
    t->file = NULL;
    t->waiting = 0;
    t->next_block = 0;
    for (size_t i = 0; i < t->n; i++)
        free_dl(&t->q[i]);
    t->n = 0;
    for (size_t i = 0; i < t->ndel; i++)
        free_dl(&t->del[i]);
    t->ndel = 0;
    db_free(&t->peer);
    t->rs.deferred = 0; /* they were entries of that index */
    t->peer_complete = t->have_peer_cc = t->index_started = 0;
    t->start_seq = t->announced_seq = 0;
    t->base = t->stats;
    t->attached = 0;
}

static int new_index_id(uint64_t *id)
{
    *id = 0;
    while (*id == 0)
        if (RAND_bytes((unsigned char *)id, sizeof *id) != 1)
            return -1;
    return 0;
}

static int is_present(const tw *t, const char *name)
{
    for (size_t i = 0; i < t->npresent; i++)
        if (strcmp(t->present[i], name) == 0)
            return 1;
    return 0;
}

static void note_in(char ***list, size_t *n, const char *name)
{
    for (size_t i = 0; i < *n; i++)
        if (strcmp((*list)[i], name) == 0)
            return;
    char **np = realloc(*list, (*n + 1) * sizeof *np);
    if (!np)
        return;
    *list = np;
    if (((*list)[*n] = strdup(name)) != NULL)
        (*n)++;
}



/* ---- state file ---- */

static db_binding binding(const tw *t)
{
    db_binding b = { t->folder, t->root, t->peer_id, t->my_id };
    return b;
}

static void announce_new(tw *t);

#ifdef TW_TEST_HOOKS
int tw_test_fail_saves; /* the next N state writes fail */
#endif

static int save(tw *t)
{
    char why[1200];
    db_binding b = binding(t);
    t->stats.state_saves++;
    net_ms started = net_now();
    int rc;
#ifdef TW_TEST_HOOKS
    if (tw_test_fail_saves > 0) {
        tw_test_fail_saves--;
        snprintf(why, sizeof why, "test hook: state write made to fail");
        rc = -1;
    } else
#endif
        rc = db_save(t->state_path, &b, &t->d, why, sizeof why);
    t->saved_at = net_now();
    if (rc == 0) {
        t->dirty = 0;
        t->fail_run = 0;
        t->saved_seq = t->d.sequence;
        t->gap = (t->saved_at - started) * 4;
        if (t->gap < SAVE_MS)
            t->gap = SAVE_MS;
        announce_new(t);
        return 0;
    }
    t->stats.state_failures++;
    t->fail_run++;
    t->gap = RETRY_MS;
    for (long i = 1; i < t->fail_run && t->gap < RETRY_MAX_MS; i++)
        t->gap *= 2;
    if (t->gap > RETRY_MAX_MS)
        t->gap = RETRY_MAX_MS;
    t->log("state: could not be written: %s; nothing new is announced until it is; retrying in %lld s", why,
           (long long)(t->gap / 1000));
    UI(t, "  WARNING the local state could not be saved (%s); changes are not announced yet", why);
    return -1;
}

int tw_flush(tw *t)
{
    return t->dirty ? save(t) : 0;
}

/* A new or replaced record: takes the next sequence. */
static db_entry *put_record(tw *t, finfo *f)
{
    f->sequence = db_next_sequence(&t->d);
    db_entry *e = db_put(&t->d, f);
    if (!e) {
        t->state_lost = 1;
        t->log("state: cannot record \"%s\" (out of memory or too many entries)", f->name);
        return NULL;
    }
    t->dirty = 1;
    return e;
}

/* ---- announcing ---- */

static int cmp_entry_seq(const void *a, const void *b)
{
    int64_t x = (*(db_entry *const *)a)->f.sequence, y = (*(db_entry *const *)b)->f.sequence;
    return x < y ? -1 : x > y;
}

/* Sends our records with from < sequence <= saved_seq, in sequence
 * order: as an Index when full (from 0, the first message), otherwise as
 * IndexUpdates following on from `from`. */
static const char *send_records(tw *t, int64_t from, int full)
{
    db_entry **list = malloc((t->d.n ? t->d.n : 1) * sizeof *list);
    if (!list)
        return "OUT_OF_MEMORY";
    size_t k = 0;
    for (size_t i = 0; i < t->d.n; i++) {
        db_entry *e = &t->d.e[i];
        if (e->f.sequence > from && e->f.sequence <= t->saved_seq && e->status != TS_PROVISIONAL)
            list[k++] = e;
    }
    qsort(list, k, sizeof *list, cmp_entry_seq);
    size_t i = 0;
    int64_t prev = from;
    do {
        pb_writer files, w;
        pb_writer_init(&files, (size_t)64 << 20);
        pb_writer_init(&w, (size_t)64 << 20);
        int64_t last = prev;
        size_t count = 0;
        for (; i < k && files.len < INDEX_BATCH; i++) {
            pb_writer fi;
            pb_writer_init(&fi, (size_t)32 << 20);
            if (finfo_encode(&fi, &list[i]->f) != 0) {
                pb_writer_free(&fi);
                continue;
            }
            pb_put_message_field(&files, 2, &fi);
            pb_writer_free(&fi);
            last = list[i]->f.sequence;
            count++;
        }
        if (full) {
            pb_put_string_field(&w, 1, t->folder);
            pb_put_varint_field(&w, 3, (uint64_t)last);
        } else {
            if (count == 0) {
                pb_writer_free(&files);
                pb_writer_free(&w);
                break;
            }
            bep_encode_index_update_start(&w, t->folder, last, prev);
        }
        pb_put_raw(&w, files.data, files.len);
        pb_writer_free(&files);
        enum tls_result r = t->send(t->ctx, full ? BEP_INDEX : BEP_INDEX_UPDATE, &w);
        pb_writer_free(&w);
        if (r != TLS_OK) {
            free(list);
            return tls_result_name(r);
        }
        t->stats.announced += (long)count;
        t->log("tw: sent %s with %lu entr%s (sequence %lld..%lld)", full ? "Index" : "IndexUpdate",
               (unsigned long)count, count == 1 ? "y" : "ies", (long long)(count ? prev + 1 : prev),
               (long long)last);
        if (last > t->announced_seq)
            t->announced_seq = last;
        prev = last;
        full = 0;
    } while (i < k);
    if (t->announced_seq < t->saved_seq)
        t->announced_seq = t->saved_seq; /* provisional records are announced after their resolution */
    free(list);
    return NULL;
}

static void announce_new(tw *t)
{
    if (!t->index_started || t->saved_seq <= t->announced_seq)
        return;
    const char *r = send_records(t, t->announced_seq, 0);
    if (r)
        t->log("tw: announcing failed: %s", r);
}

static const char *send_index(tw *t)
{
    t->index_started = 1;
    t->announced_seq = t->start_seq;
    return send_records(t, t->start_seq, t->start_seq == 0);
}

void tw_cc_self(const tw *t, uint64_t *index_id, int64_t *max_sequence)
{
    *index_id = t->d.index_id;
    *max_sequence = t->saved_seq;
}

/* ---- reading the folder ---- */

static void set_disk(db_entry *e, const scan_item *it)
{
    e->disk_size = it->size;
    e->disk_mtime_s = it->mtime_s;
    e->disk_mtime_ns = it->mtime_ns;
}

static int in_window(int64_t mtime_utc, int64_t at)
{
    int64_t d = at - mtime_utc;
    return d <= DATE_WINDOW_S && d >= -DATE_WINDOW_S;
}

/* Hashes name into f (blocks, size). 0, 1 when it changed while read, -1. */
static int hash_name(tw *t, const char *name, int64_t size, finfo *f)
{
    char *path = store_full_path(t->st, name);
    int rc = path ? finfo_hash_path(path, size, f) : -1;
    free(path);
    return rc;
}

/* One listed file or directory against our record, in three parts so
 * that the hash in the middle can be done in steps.
 * observe_pre: OBS_DONE when nothing more is to do; OBS_DIR for a
 * directory to record; OBS_HASH when the content must be hashed first
 * (*ms, *mns then hold the date to publish). */
enum { OBS_DONE = 0, OBS_DIR, OBS_HASH };

static int observe_pre(tw *t, const char *name, const scan_item *it, int64_t *ms, int32_t *mns)
{
    db_entry *e = db_find(&t->d, name);
    int is_dir = it->kind == SCAN_DIR;
    if (e && !e->f.deleted && (e->f.type == BEP_DIRECTORY) != is_dir) {
        if (e->status != TS_HELD)
            t->log("scan: \"%s\" changed between file and directory here; held (not handled yet)", name);
        e->status = TS_HELD;
        return OBS_DONE;
    }
    if (e && !e->f.deleted && is_dir) {
        if (e->status != TS_PROVISIONAL)
            e->status = TS_OK;
        return OBS_DONE;
    }
    if (t->trust_dates && e && !e->f.deleted && e->disk_size == it->size && e->disk_mtime_s == it->mtime_s
        && e->disk_mtime_ns == it->mtime_ns && !(e->flags & DB_HASH_NEXT)) {
        if (e->status != TS_PROVISIONAL)
            e->status = TS_OK;
        return OBS_DONE;
    }
    *ms = 0;
    *mns = 0;
    if (scan_mtime_utc(t->root, name, ms, mns) != 0) {
        if (e)
            e->status = TS_UNKNOWN;
        t->stats.unchecked++;
        t->log("scan: \"%s\": date cannot be read; unchecked", name);
        return OBS_DONE;
    }
    return is_dir ? OBS_DIR : OBS_HASH;
}

/* The content could not be hashed: h > 0 changed while read, else unreadable. */
static void observe_unhashed(tw *t, const char *name, int h)
{
    db_entry *e = db_find(&t->d, name);
    if (e) {
        e->status = TS_UNKNOWN;
        e->flags |= DB_HASH_NEXT;
    }
    t->stats.unchecked++;
    t->log("scan: \"%s\": %s; unchecked, read again on the next scan", name,
           h > 0 ? "changed while it was read" : "cannot be read");
}

/* Records a new or changed entry (new version and sequence), or confirms
 * it. hashed: the content's blocks for a file (taken over), NULL for a
 * directory. */
static void observe_post(tw *t, const char *name, const scan_item *it, int64_t now, int64_t ms, int32_t mns,
                         finfo *hashed)
{
    db_entry *e = db_find(&t->d, name);
    int is_dir = it->kind == SCAN_DIR;
    finfo nf;
    memset(&nf, 0, sizeof nf);
    if (hashed) {
        nf = *hashed;
        memset(hashed, 0, sizeof *hashed);
    }
    if (!is_dir) {
        if (e && !e->f.deleted && finfo_same_blocks(&nf, &e->f)) {
            /* Same content: only the date on disk moved (or the window
             * check asked for this hash). Not a new version: dates of files
             * written here are not the peer's (store.c sets none). */
            unsigned flags = in_window(ms, now) ? DB_HASH_NEXT : 0;
            if (e->disk_size != it->size || e->disk_mtime_s != it->mtime_s || e->disk_mtime_ns != it->mtime_ns
                || e->flags != flags) {
                set_disk(e, it);
                e->flags = flags;
                t->dirty = 1; /* only then: hashing every file must not write the state each scan */
            }
            if (e->status != TS_PROVISIONAL)
                e->status = TS_OK;
            finfo_free(&nf);
            return;
        }
    }
    int was_live = e && !e->f.deleted;
    if (!(nf.name = strdup(name))) {
        finfo_free(&nf);
        t->state_lost = 1;
        return;
    }
    nf.type = is_dir ? BEP_DIRECTORY : BEP_FILE;
    nf.no_permissions = 1;
    nf.permissions = is_dir ? 0755 : 0644;
    nf.modified_s = ms;
    nf.modified_ns = mns;
    nf.modified_by = t->me;
    if (e && vv_copy(&nf.version, &e->f.version) != 0) {
        finfo_free(&nf);
        t->state_lost = 1;
        return;
    }
    if (was_live && e->f.has_blocks_hash) {
        memcpy(nf.prev_blocks_hash, e->f.blocks_hash, FINFO_HASH);
        nf.has_prev_blocks_hash = 1;
    }
    if (vv_update(&nf.version, t->me, (uint64_t)now) != 0) {
        finfo_free(&nf);
        t->state_lost = 1;
        return;
    }
    int provisional = t->rebuild && !e;
    db_entry *ne = put_record(t, &nf);
    finfo_free(&nf);
    if (!ne)
        return;
    set_disk(ne, it);
    ne->flags = !is_dir && in_window(ms, now) ? DB_HASH_NEXT : 0;
    ne->status = provisional ? TS_PROVISIONAL : TS_OK;
    if (was_live) {
        t->stats.local_changed++;
        t->log("scan: \"%s\" changed here: new version, sequence %lld", name, (long long)ne->f.sequence);
        UI(t, "  local  %-30s changed here, will be sent", name);
    } else {
        t->stats.local_new++;
        t->log("scan: \"%s\" new here (%s): sequence %lld%s", name, is_dir ? "directory" : "file",
               (long long)ne->f.sequence, provisional ? ", version provisional until the peer's index is here" : "");
        UI(t, "  local  %-30s new here, will be sent", name);
    }
}

/* All three parts at once (one name, outside a scan). */
static void observe(tw *t, const char *name, const scan_item *it, int64_t now)
{
    int64_t ms;
    int32_t mns;
    int k = observe_pre(t, name, it, &ms, &mns);
    if (k == OBS_DONE)
        return;
    if (k == OBS_DIR) {
        observe_post(t, name, it, now, ms, mns, NULL);
        return;
    }
    finfo nf;
    memset(&nf, 0, sizeof nf);
    int h = hash_name(t, name, it->size, &nf);
    if (h != 0) {
        finfo_free(&nf);
        observe_unhashed(t, name, h);
        return;
    }
    observe_post(t, name, it, now, ms, mns, &nf);
}

/* A file or directory deleted here: a new version of ours, as Syncthing's
 * FileInfo.SetDeleted makes it (modified now, no blocks, size 0). */
static void publish_deletion(tw *t, const char *name, int64_t now)
{
    db_entry *e = db_find(&t->d, name);
    if (!e || e->f.deleted)
        return;
    int is_dir = e->f.type == BEP_DIRECTORY;
    finfo nf;
    if (finfo_copy(&nf, &e->f) != 0 || finfo_set_deleted(&nf, t->me, now) != 0) {
        finfo_free(&nf);
        t->state_lost = 1;
        return;
    }
    db_entry *ne = put_record(t, &nf);
    finfo_free(&nf);
    if (!ne)
        return;
    ne->disk_size = 0;
    ne->disk_mtime_s = 0;
    ne->disk_mtime_ns = 0;
    ne->flags = 0;
    ne->status = TS_OK;
    t->stats.local_deleted++;
    t->log("scan: \"%s\" deleted here (%s): new version, sequence %lld", name, is_dir ? "directory" : "file",
           (long long)ne->f.sequence);
    UI(t, "  local  %-30s deleted here, will be sent", name);
}

/* Before the first scan's results are used: what an interrupted run left.
 * A temporary file is an unfinished download (never anyone's data): it is
 * deleted. A moved-aside ".old" copy is put back when its name is free;
 * when both are there nothing is touched and the name is reported.
 * Returns 1 when a file was put back (the folder is read again). */
static int clean_leftovers(tw *t, const scan_result *sr)
{
    int restored = 0;
    char why[1200];
    for (size_t i = 0; i < sr->nl; i++) {
        const char *name = sr->leftovers[i];
        size_t n = strlen(name);
        if (n > 4 && strcmp(name + n - 4, ".tmp") == 0) {
            if (store_remove_temp(t->st, name, why, sizeof why) == 0) {
                t->stats.leftovers_removed++;
                t->log("tw: removed \"%s\", left by an interrupted download", name);
            } else {
                t->log("tw: \"%s\" (left by an interrupted download) not removed: %s", name, why);
            }
            continue;
        }
        /* dir/.stpoc-<x>.old -> dir/<x> */
        const char *base = strrchr(name, '/');
        size_t dirlen = base ? (size_t)(base - name + 1) : 0;
        base = name + dirlen;
        size_t blen = strlen(base);
        if (blen <= 7 + 4)
            continue;
        char orig[1100];
        snprintf(orig, sizeof orig, "%.*s%.*s", (int)dirlen, name, (int)(blen - 7 - 4), base + 7);
        int rc = store_recover(t->st, orig, why, sizeof why);
        if (rc == 1) {
            t->stats.leftovers_restored++;
            restored = 1;
            t->log("tw: \"%s\" put back from \"%s\" (a replacement or deletion was interrupted)", orig, name);
        } else if (rc == 2 || rc < 0) {
            t->stats.held++;
            t->log("tw: \"%s\": %s; nothing changed, check it by hand", orig, why);
            UI(t, "  HELD   %s (an earlier copy %s is still here)", orig, name);
        }
    }
    return restored;
}

static const char *evaluate(tw *t, db_entry *p);

static const char *rescan_fail(tw *t)
{
    rescan_drop(t);
    t->log("scan: the folder %s cannot be read", t->root);
    return "FOLDER_UNAVAILABLE";
}

/* The end of a scan: deletions here, then the peer's entries that
 * arrived meanwhile. */
static const char *rescan_finish(tw *t)
{
    struct rescan *rs = &t->rs;
    scan_result *sr = &rs->sr;
    int64_t now = rs->now;
    long missing = 0;
    char **gone = NULL;
    size_t ngone = 0;
    for (size_t i = 0; i < t->d.n; i++) {
        db_entry *e = &t->d.e[i];
        if (e->f.deleted || scan_find(sr, e->f.name))
            continue;
        if (scan_absence_is_deletion(sr, e->f.name)) {
            /* Listed by a complete read of its directory, with .stfolder
             * present: a deletion here. Published after the loop (a new
             * record moves entries). */
            char **ng = realloc(gone, (ngone + 1) * sizeof *ng);
            if (!ng || !(ng[ngone] = strdup(e->f.name))) {
                gone = ng ? ng : gone;
                t->state_lost = 1;
                break;
            }
            gone = ng;
            ngone++;
        } else {
            t->stats.unchecked++;
            e->status = TS_UNKNOWN;
            t->log("scan: \"%s\" not listed, but its directory could not be read completely; unchecked",
                   e->f.name);
        }
    }
    for (size_t i = 0; i < ngone; i++) {
        publish_deletion(t, gone[i], now);
        free(gone[i]);
        missing++;
    }
    free(gone);
    /* What an unread directory holds is not known either. */
    t->stats.unchecked += sr->failed_dirs;
    t->log("scan: %lu item(s) listed, %ld director%s not read completely, %ld deleted here, %ld not verified",
           (unsigned long)sr->n, sr->failed_dirs, sr->failed_dirs == 1 ? "y" : "ies", missing, t->stats.unchecked);
    if (rs->slices > 1)
        t->log("scan: done in %ld step(s) over %lld ms", rs->slices, (long long)(net_now() - rs->started));
    for (size_t i = 0; i < t->npresent; i++)
        free(t->present[i]);
    free(t->present);
    t->present = rs->present;
    t->npresent = rs->npresent;
    rs->present = NULL;
    rs->npresent = 0;
    long deferred = rs->deferred;
    rs->deferred = 0;
    rescan_drop(t);
    if (deferred > 0) {
        t->log("tw: %ld entr%s of the peer arrived during the scan; decided now", deferred,
               deferred == 1 ? "y" : "ies");
        for (size_t i = 0; i < t->peer.n; i++) {
            if (t->peer.e[i].status != PS_DEFERRED)
                continue;
            const char *r = evaluate(t, &t->peer.e[i]);
            if (r)
                return r;
        }
    }
    return NULL;
}

/* Reads the folder and compares it with our records, until `until` (a
 * scan not finished then continues with the next call; t->rs.phase is
 * RS_IDLE once it is done). While it runs nothing of ours changes the
 * folder: tw_step starts no download or deletion, and the peer's new
 * entries wait (PS_DEFERRED), so what was listed stays true to the end. */
static const char *rescan_step(tw *t, net_ms until)
{
    struct rescan *rs = &t->rs;
    scan_result *sr = &rs->sr;
    if (rs->phase == RS_IDLE) {
        rs->started = net_now();
        rs->slices = 0;
        rs->relisted = 0;
        if (!(rs->run = scan_begin(t->root, sr)))
            return rescan_fail(t);
        rs->phase = RS_LIST;
    }
    rs->slices++;
    for (;;) {
        switch (rs->phase) {
        case RS_LIST: {
            int rc = scan_step(rs->run, 32);
            if (rc < 0)
                return rescan_fail(t);
            if (rc > 0)
                break;
            scan_end(rs->run);
            rs->run = NULL;
            if (!t->scanned && sr->marker && sr->nl > 0 && !rs->relisted) {
                t->scanned = 1;
                if (clean_leftovers(t, sr)) {
                    scan_free(sr);
                    rs->relisted = 1;
                    if (!(rs->run = scan_begin(t->root, sr)))
                        return rescan_fail(t);
                    break;
                }
            }
            t->scanned = 1;
            if (!sr->marker) {
                rescan_drop(t);
                t->log("scan: %s has no %s: the folder is taken as unavailable (not mounted, wrong path); nothing "
                       "is scanned, sent, received or deleted", t->root, SCAN_MARKER);
                UI(t, "Folder unavailable: %s has no %s", t->root, SCAN_MARKER);
                return "FOLDER_UNAVAILABLE";
            }
            rs->now = now_unix();
            t->stats.unchecked = 0;
            rs->i = 0;
            rs->phase = RS_OBSERVE;
            break;
        }
        case RS_OBSERVE: {
            if (rs->i >= sr->n) {
                rs->phase = RS_MISSING;
                break;
            }
            const scan_item *it = &sr->items[rs->i];
            char why[300];
            if (it->kind == SCAN_OTHER) {
                note_in(&rs->present, &rs->npresent, it->name);
                t->log("scan: \"%s\": symbolic link or special entry; not synchronised", it->name);
                rs->i++;
                break;
            }
            if (store_accept(t->st, it->name, it->kind == SCAN_DIR, why, sizeof why) != 0) {
                note_in(&rs->present, &rs->npresent, it->name);
                t->log("scan: \"%s\" not synchronised: %s", it->name, why);
                rs->i++;
                break;
            }
            int k = observe_pre(t, it->name, it, &rs->ms, &rs->mns);
            if (k == OBS_HASH) {
                char *path = store_full_path(t->st, it->name);
                rs->h = path ? finfo_hash_begin(path, it->size) : NULL;
                free(path);
                if (rs->h) {
                    rs->phase = RS_HASH;
                    break;
                }
                observe_unhashed(t, it->name, -1);
            } else if (k == OBS_DIR) {
                observe_post(t, it->name, it, rs->now, rs->ms, rs->mns, NULL);
            }
            rs->i++;
            break;
        }
        case RS_HASH: {
            int rc = finfo_hash_step(rs->h);
            if (rc == 2)
                break;
            const scan_item *it = &sr->items[rs->i];
            if (rc == 0) {
                finfo nf;
                memset(&nf, 0, sizeof nf);
                finfo_hash_finish(rs->h, &nf);
                rs->h = NULL;
                observe_post(t, it->name, it, rs->now, rs->ms, rs->mns, &nf);
            } else {
                finfo_hash_abort(rs->h);
                rs->h = NULL;
                observe_unhashed(t, it->name, rc);
            }
            rs->i++;
            rs->phase = RS_OBSERVE;
            break;
        }
        default:
            return rescan_finish(t);
        }
        if (net_now() >= until)
            return NULL;
    }
}

/* A whole scan before anything else happens (at start). A stop request
 * ends it: what was read so far is not written. */
static const char *scan_now(tw *t)
{
    for (;;) {
        const char *r = rescan_step(t, net_now() + (t->stop ? 200 : 3600 * 1000));
        if (r || t->rs.phase == RS_IDLE)
            return r;
        if (t->stop && t->stop()) {
            rescan_drop(t);
            t->log("scan: stopped on request before the folder was read to its end");
            return "STOPPED";
        }
    }
}

/* ---- preparing ---- */

static int ensure_marker(tw *t)
{
    char *p = store_full_path(t->st, SCAN_MARKER);
    if (!p)
        return -1;
    struct stat st;
    int rc = 0;
    if (stat(p, &st) != 0) {
        rc = mkdir(p, 0755);
        if (rc == 0)
            t->log("tw: created %s in the folder (as Syncthing does)", SCAN_MARKER);
    }
    free(p);
    return rc;
}

/* Format 1 (receive-only) to format 2: entries still on disk become our
 * records with the peer's version (hashed again by the scan that
 * follows: a file changed since becomes a change of our own); missing
 * ones are dropped, so they are downloaded again rather than deleted. */
static void migrate(tw *t)
{
    char why[1200];
    state_list v1;
    if (state_load(t->state_path, t->folder, t->root, t->peer_id, &v1, why, sizeof why) != 0) {
        t->log("state: format 1 present but rejected (%s); rebuilding", why);
        t->rebuild = 1;
        return;
    }
    long kept = 0, dropped = 0;
    for (size_t i = 0; i < v1.n; i++) {
        state_entry *s = &v1.e[i];
        finfo r;
        const char *fw;
        scan_item it;
        if (finfo_decode((pb_slice){ s->raw, s->raw_len }, &r, &fw) != 0) {
            t->log("state: format 1 entry \"%s\" not usable (%s); dropped", s->name, fw);
            dropped++;
            continue;
        }
        int k = scan_one(t->root, s->name, &it);
        int kind_ok = k == 0 && (it.kind == (s->is_dir ? SCAN_DIR : SCAN_FILE));
        if (!kind_ok || strcmp(r.name, s->name) != 0) {
            t->log("state: format 1 entry \"%s\" %s; dropped (it is downloaded again if the peer has it)", s->name,
                   k == 1 ? "is missing" : "is not what was recorded");
            finfo_free(&r);
            dropped++;
            continue;
        }
        db_entry *e = put_record(t, &r);
        finfo_free(&r);
        if (e) {
            set_disk(e, &it);
            e->flags = s->is_dir ? 0 : DB_HASH_NEXT;
            kept++;
        }
    }
    state_list_free(&v1);
    t->log("state: migrated from format 1: %ld entr%s kept (hashed again now), %ld dropped", kept,
           kept == 1 ? "y" : "ies", dropped);
    UI(t, "Local state migrated to two-way format: %ld entries kept, %ld to download again", kept, dropped);
}

const char *tw_prepare_begin(tw *t)
{
    char why[1200];
    db_binding b = binding(t);
    t->saved_at = net_now();
    t->gap = SAVE_MS;
    int rc = db_load(t->state_path, &b, &t->d, why, sizeof why);
    int first = 0;
    if (rc == 0) {
        t->saved_seq = t->d.sequence;
        t->log("state: %lu entr%s, index ID %016llx, sequence %lld", (unsigned long)t->d.n,
               t->d.n == 1 ? "y" : "ies", (unsigned long long)t->d.index_id, (long long)t->d.sequence);
    } else {
        memset(&t->d, 0, sizeof t->d);
        if (new_index_id(&t->d.index_id) != 0)
            return "RNG_FAILED";
        if (rc == 2) {
            migrate(t);
            first = 1;
        } else if (rc == 1) {
            t->log("state: none yet (first run); versions of local files are settled against the peer's index");
            t->rebuild = 1;
            first = 1;
        } else {
            char aside[1200];
            snprintf(aside, sizeof aside, "%s.rejected", t->state_path);
            remove(aside);
            int moved = rename(t->state_path, aside) == 0;
            t->log("state: rejected (%s); %s; STATE_REBUILT: new index ID %016llx, every file hashed, versions "
                   "settled against the peer's index, no deletions derived", why,
                   moved ? "kept as .rejected" : "could not be moved aside", (unsigned long long)t->d.index_id);
            UI(t, "Local state damaged (%s): rebuilt from the files here", why);
            t->rebuild = 1;
        }
    }
    if (first && ensure_marker(t) != 0) {
        t->log("tw: cannot create %s in %s", SCAN_MARKER, t->root);
        return "FOLDER_UNAVAILABLE";
    }
    if (t->hash_policy == TW_HASH_AUTO) {
        char why[200];
        t->trust_dates = scan_dates_reliable(t->root, why, sizeof why);
        t->log("scan: %s", why);
    } else {
        t->trust_dates = t->hash_policy == TW_HASH_TRUST;
        t->log("scan: %s", t->trust_dates ? "size and date are trusted (set)" : "every file is hashed at each scan (set)");
    }
    t->prep_fresh = rc != 0;
    return NULL;
}

static const char *prepare_end(tw *t)
{
    /* Written when anything changed, and always for a new, migrated or
     * rebuilt state (its index ID must be on disk before it is sent). */
    if ((t->dirty || t->prep_fresh) && save(t) != 0)
        return "STATE_NOT_SAVED";
    t->next_rescan = net_now() + t->rescan_seconds * 1000;
    return NULL;
}

const char *tw_prepare_step(tw *t, net_ms until, int *done)
{
    *done = 0;
    const char *r = rescan_step(t, until);
    if (r)
        return r;
    if (t->rs.phase != RS_IDLE)
        return NULL;
    *done = 1;
    return prepare_end(t);
}

const char *tw_prepare(tw *t)
{
    const char *r = tw_prepare_begin(t);
    if (!r)
        r = scan_now(t);
    return r ? r : prepare_end(t);
}

/* ---- the peer's ClusterConfig and index ---- */

static const char *evaluate(tw *t, db_entry *p);

static void resolve_provisional(tw *t)
{
    long adopted = 0, kept = 0;
    for (size_t i = 0; i < t->d.n; i++) {
        db_entry *e = &t->d.e[i];
        if (e->status != TS_PROVISIONAL)
            continue;
        db_entry *p = db_find(&t->peer, e->f.name);
        int same = p && !p->f.deleted && !p->f.invalid && p->f.type == e->f.type
                   && (e->f.type == BEP_DIRECTORY || finfo_same_blocks(&p->f, &e->f));
        if (same) {
            /* The content here is the peer's version: take its version,
             * nothing changes on either side. */
            finfo c;
            if (finfo_copy(&c, &p->f) == 0) {
                int64_t ds = e->disk_size, dm = e->disk_mtime_s;
                int32_t dn = e->disk_mtime_ns;
                unsigned fl = e->flags;
                db_entry *ne = put_record(t, &c);
                finfo_free(&c);
                if (ne) {
                    ne->disk_size = ds;
                    ne->disk_mtime_s = dm;
                    ne->disk_mtime_ns = dn;
                    ne->flags = fl;
                    ne->status = TS_OK;
                    t->log("tw: \"%s\": equal to the peer's entry; its version taken", ne->f.name);
                    adopted++;
                    t->stats.adopted++;
                    continue;
                }
            }
        }
        e->status = TS_OK;
        kept++;
    }
    t->rebuild = 0;
    t->log("tw: versions settled: %ld entr%s found equal to the peer's (its version taken), %ld of our own", adopted,
           adopted == 1 ? "y" : "ies", kept);
}

const char *tw_peer_cc(tw *t, int listed, uint64_t index_id, int64_t max_sequence)
{
    if (t->have_peer_cc)
        return NULL; /* a later ClusterConfig: index already started */
    t->have_peer_cc = 1;
    if (listed && index_id == t->d.index_id && max_sequence > t->saved_seq) {
        /* The peer has more of this index than we have recorded: our state
         * is older than what was announced (restored, or lost writes).
         * Never reuse those numbers: a new index ID, a full index next time. */
        uint64_t old = t->d.index_id;
        if (new_index_id(&t->d.index_id) != 0)
            return "RNG_FAILED";
        t->log("tw: the peer has our index %016llx up to sequence %lld, but only %lld is recorded here; new index "
               "ID %016llx, the next session sends a full index", (unsigned long long)old, (long long)max_sequence,
               (long long)t->saved_seq, (unsigned long long)t->d.index_id);
        t->dirty = 1;
        return save(t) == 0 ? "INDEX_ID_RENEWED" : "STATE_NOT_SAVED";
    }
    if (listed && index_id == t->d.index_id) {
        t->start_seq = max_sequence;
        t->log("tw: the peer has our index up to sequence %lld (of %lld): sending what follows", (long long)max_sequence,
               (long long)t->saved_seq);
    } else {
        t->start_seq = 0;
        t->log("tw: the peer has %s of our index %016llx: sending it in full",
               listed && index_id ? "another version" : "nothing", (unsigned long long)t->d.index_id);
    }
    if (t->rebuild) {
        t->log("tw: our index is sent once the peer's is complete (versions to settle first)");
        return NULL;
    }
    return send_index(t);
}

const char *tw_peer_index_complete(tw *t)
{
    if (t->peer_complete)
        return NULL;
    t->peer_complete = 1;
    if (!t->rebuild)
        return NULL;
    resolve_provisional(t);
    for (size_t i = 0; i < t->peer.n; i++) {
        const char *r = evaluate(t, &t->peer.e[i]);
        if (r)
            return r;
    }
    if (save(t) != 0)
        return NULL; /* announced once a write works */
    return t->have_peer_cc ? send_index(t) : NULL;
}

/* ---- taking the peer's entries ---- */

static void drop_deletion(tw *t, const char *name);

static void drop_queued(tw *t, const char *name)
{
    drop_deletion(t, name);
    for (size_t i = 0; i < t->n; i++) {
        if (strcmp(t->q[i].name, name) != 0)
            continue;
        if (i == 0 && (t->file || t->waiting)) {
            t->log("tw: newer entry for \"%s\" arrived during its download; download stopped", name);
            store_abort(t->file);
            t->file = NULL;
            t->waiting = 0;
        }
        free_dl(&t->q[i]);
        memmove(t->q + i, t->q + i + 1, (t->n - i - 1) * sizeof *t->q);
        t->n--;
        if (i == 0)
            t->next_block = 0;
        return;
    }
}

static const char *queue_into(struct dl **q, size_t *n, size_t *cap, const finfo *r, int conflict)
{
    if (*n == *cap) {
        size_t nc = *cap ? *cap * 2 : 16;
        struct dl *nq = realloc(*q, nc * sizeof *nq);
        if (!nq)
            return "OUT_OF_MEMORY";
        *q = nq;
        *cap = nc;
    }
    struct dl *x = &(*q)[*n];
    if (!(x->name = strdup(r->name)) || finfo_copy(&x->r, r) != 0) {
        free(x->name);
        return "OUT_OF_MEMORY";
    }
    x->conflict = conflict;
    (*n)++;
    return NULL;
}

static const char *queue(tw *t, const finfo *r, int conflict)
{
    return queue_into(&t->q, &t->n, &t->cap, r, conflict);
}

static void drop_deletion(tw *t, const char *name)
{
    for (size_t i = 0; i < t->ndel; i++) {
        if (strcmp(t->del[i].name, name) != 0)
            continue;
        free_dl(&t->del[i]);
        memmove(t->del + i, t->del + i + 1, (t->ndel - i - 1) * sizeof *t->del);
        t->ndel--;
        return;
    }
}

static void block(tw *t, db_entry *p, const char *what, const char *why)
{
    p->status = PS_BLOCKED;
    t->log("tw: \"%s\" %s: %s", p->f.name, what, why);
}

static const char *evaluate(tw *t, db_entry *p)
{
    const finfo *R = &p->f;
    db_entry *L = db_find(&t->d, R->name);
    enum disk_state disk;
    if (L)
        disk = L->status == TS_UNKNOWN || L->status == TS_HELD ? DISK_UNKNOWN
               : L->f.deleted                                  ? DISK_MISSING
                                                               : DISK_AS_RECORDED;
    else
        disk = is_present(t, R->name) ? DISK_DIFFERENT : DISK_MISSING;
    decision d = decide(L ? &L->f : NULL, disk, R);
    p->status = PS_OPEN;
    switch (d.action) {
    case ACT_NOTHING:
        if (d.row != 1)
            t->log("tw: \"%s\": %s (row %d)", R->name, d.why, d.row);
        return NULL;
    case ACT_IGNORE:
        t->stats.unsupported++;
        block(t, p, "not taken", d.why);
        UI(t, "  MISSED %s (%s)", R->name, d.why);
        return NULL;
    case ACT_HOLD:
    case ACT_RESCAN:
        t->stats.held++;
        block(t, p, "held", d.action == ACT_RESCAN ? "something here differs from the record; not touched" : d.why);
        UI(t, "  HELD   %s (%s)", R->name, d.why);
        return NULL;
    case ACT_DELETE:
    case ACT_CONFLICT_DELETE:
        t->log("tw: \"%s\": %s (row %d); deletion queued", R->name, d.why, d.row);
        return queue_into(&t->del, &t->ndel, &t->capdel, R, d.action == ACT_CONFLICT_DELETE);
    case ACT_CONFLICT_DOWNLOAD:
        t->log("tw: \"%s\": %s (row %d); ours becomes a conflict copy, the peer's version queued", R->name, d.why,
               d.row);
        return queue(t, R, 1);
    case ACT_RECORD: {
        finfo c;
        if (finfo_copy(&c, R) != 0)
            return "OUT_OF_MEMORY";
        int64_t ds = L ? L->disk_size : 0, dm = L ? L->disk_mtime_s : 0;
        int32_t dn = L ? L->disk_mtime_ns : 0;
        unsigned fl = L ? L->flags : 0;
        db_entry *e = put_record(t, &c);
        finfo_free(&c);
        if (e) {
            e->disk_size = ds;
            e->disk_mtime_s = dm;
            e->disk_mtime_ns = dn;
            e->flags = fl;
            t->stats.recorded++;
            t->log("tw: \"%s\": %s (row %d); the peer's version recorded", R->name, d.why, d.row);
        }
        return NULL;
    }
    case ACT_DOWNLOAD:
        t->log("tw: \"%s\": %s (row %d); queued", R->name, d.why, d.row);
        return queue(t, R, 0);
    }
    return NULL;
}

const char *tw_offer(tw *t, const bep_file *fi)
{
    finfo r;
    const char *why;
    char name[300];
    snprintf(name, sizeof name, "%.*s", (int)(fi->name.len < 200 ? fi->name.len : 200), (const char *)fi->name.data);
    if (finfo_decode(fi->raw, &r, &why) != 0) {
        t->stats.rejected++;
        t->log("tw: entry \"%s\" from the peer rejected: %s", name, why);
        return NULL;
    }
    drop_queued(t, r.name);
    db_entry *p = db_put(&t->peer, &r);
    if (!p) {
        finfo_free(&r);
        return "OUT_OF_MEMORY";
    }
    char w[300];
    if ((r.type == BEP_FILE || r.type == BEP_DIRECTORY) && !r.invalid
        && store_accept(t->st, r.name, r.type == BEP_DIRECTORY, w, sizeof w) != 0) {
        t->stats.rejected++;
        block(t, p, "not taken", w);
        UI(t, "  skip   %s (name not accepted here: %s)", r.name, w);
        finfo_free(&r);
        return NULL;
    }
    finfo_free(&r);
    if (t->rebuild && !t->peer_complete)
        return NULL; /* decided when the peer's index is complete */
    if (t->rs.phase != RS_IDLE) {
        /* Decided against the folder as the scan leaves it. */
        p->status = PS_DEFERRED;
        t->rs.deferred++;
        return NULL;
    }
    return evaluate(t, p);
}

/* ---- serving the peer's requests ---- */

static const char *respond_error(tw *t, int32_t id, uint32_t code)
{
    pb_writer w;
    pb_writer_init(&w, 64);
    bep_encode_response_error(&w, id, code);
    enum tls_result r = t->send(t->ctx, BEP_RESPONSE, &w);
    pb_writer_free(&w);
    t->stats.refused_requests++;
    return r == TLS_OK ? NULL : tls_result_name(r);
}

/* Reads one block at offset into buf; 0 when exactly size bytes came. */
static int read_block(const char *path, int64_t offset, unsigned char *buf, size_t size)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return -1;
    int rc = fseek(fp, (long)offset, SEEK_SET) == 0 && fread(buf, 1, size, fp) == size ? 0 : -1;
    fclose(fp);
    return rc;
}

const char *tw_request(tw *t, const bep_request *q)
{
    char name[300];
    snprintf(name, sizeof name, "%.*s", (int)(q->name.len < 200 ? q->name.len : 200), (const char *)q->name.data);
    if (q->folder.len != strlen(t->folder) || memcmp(q->folder.data, t->folder, q->folder.len) != 0) {
        t->log("tw: Request %d for another folder; refused", q->id);
        return respond_error(t, q->id, 1 /* GENERIC */);
    }
    db_entry *e = q->name.len < sizeof name ? db_find(&t->d, name) : NULL;
    const finfo_block *b = NULL;
    if (e && !e->f.deleted && e->f.type == BEP_FILE && e->status == TS_OK && e->f.sequence <= t->announced_seq) {
        for (size_t i = 0; i < e->f.nblocks; i++)
            if (e->f.blocks[i].offset == q->offset) {
                b = &e->f.blocks[i];
                break;
            }
    }
    if (!b || b->size != q->size || q->hash.len != FINFO_HASH || memcmp(q->hash.data, b->hash, FINFO_HASH) != 0) {
        t->log("tw: Request %d \"%s\" offset %lld size %d: not a block of what we announce; NO_SUCH_FILE", q->id, name,
               (long long)q->offset, q->size);
        return respond_error(t, q->id, 2 /* NO_SUCH_FILE */);
    }
    if (store_local_kind(t->st, name) != 1) {
        t->log("tw: Request %d \"%s\": not a regular file here now; NO_SUCH_FILE", q->id, name);
        return respond_error(t, q->id, 2);
    }
    unsigned char *buf = malloc(b->size ? (size_t)b->size : 1), md[32];
    unsigned int mdlen = 0;
    char *path = store_full_path(t->st, name);
    int ok = buf && path && read_block(path, b->offset, buf, (size_t)b->size) == 0
             && EVP_Digest(buf, (size_t)b->size, md, &mdlen, EVP_sha256(), NULL) && mdlen == 32
             && memcmp(md, b->hash, 32) == 0;
    free(path);
    if (!ok) {
        free(buf);
        /* The file is not what we announced (an edit that kept size and
         * date can do this): refuse, and hash it in full on the next scan. */
        e->flags |= DB_HASH_NEXT;
        if (e->status == TS_OK)
            e->status = TS_UNKNOWN; /* a change here not yet scanned: no PASS until it is */
        t->dirty = 1;
        if (t->rescan_seconds > 0)
            t->next_rescan = net_now();
        t->log("tw: Request %d \"%s\" block at %lld: the content here no longer matches what we announced; "
               "NO_SUCH_FILE, the file is hashed again on the next scan", q->id, name, (long long)b->offset);
        return respond_error(t, q->id, 2);
    }
    pb_writer w;
    pb_writer_init(&w, (size_t)b->size + 64);
    bep_encode_response_data(&w, q->id, buf, (size_t)b->size);
    free(buf);
    enum tls_result r = t->send(t->ctx, BEP_RESPONSE, &w);
    pb_writer_free(&w);
    if (r != TLS_OK)
        return tls_result_name(r);
    t->stats.served_blocks++;
    t->stats.bytes_out += b->size;
    return NULL;
}

/* ---- downloading ---- */

static void pop(tw *t)
{
    free_dl(&t->q[0]);
    memmove(t->q, t->q + 1, (t->n - 1) * sizeof *t->q);
    t->n--;
    t->next_block = 0;
}

static void fail_current(tw *t, const char *why)
{
    t->log("tw: \"%s\" failed: %s; what was here is left as it was", t->q[0].name, why);
    UI(t, "  FAILED %s: %s", t->q[0].name, why);
    store_abort(t->file);
    t->file = NULL;
    t->waiting = 0;
    t->stats.failed++;
    db_entry *p = db_find(&t->peer, t->q[0].name);
    if (p)
        p->status = PS_BLOCKED;
    pop(t);
}

const char *tw_response(tw *t, int32_t id, pb_slice data, uint32_t code)
{
    if (!t->waiting || id != t->req_id) {
        t->log("tw: Response %d matches no outstanding request; ignored", id);
        return NULL;
    }
    t->waiting = 0;
    struct dl *x = &t->q[0];
    const finfo_block *b = &x->r.blocks[t->next_block];
    char why[160];
    unsigned char md[32];
    unsigned int mdlen = 0;
    if (code != 0) {
        snprintf(why, sizeof why, "block %lu: the peer answered error %u", (unsigned long)t->next_block, code);
        fail_current(t, why);
    } else if (data.len != (size_t)b->size) {
        snprintf(why, sizeof why, "block %lu: %lu bytes received, %ld expected", (unsigned long)t->next_block,
                 (unsigned long)data.len, (long)b->size);
        fail_current(t, why);
    } else if (!EVP_Digest(data.data, data.len, md, &mdlen, EVP_sha256(), NULL) || mdlen != 32
               || memcmp(md, b->hash, 32) != 0) {
        snprintf(why, sizeof why, "block %lu: SHA-256 mismatch", (unsigned long)t->next_block);
        fail_current(t, why);
    } else if (store_write(t->file, b->offset, data.data, data.len) != 0) {
        fail_current(t, "write to the temporary file failed");
    } else {
        t->next_block++;
        t->stats.bytes_in += (long long)data.len;
#ifdef TW_TEST_HOOKS
        /* A crash in the middle of a download (host tests of interruptions). */
        const char *stop = getenv("STPOC_TEST_EXIT_AFTER_BLOCKS");
        if (stop && (long)t->next_block >= atol(stop)) {
            fflush(NULL);
            _exit(137);
        }
#endif
    }
    return NULL;
}

struct expect {
    const finfo *f;
};

/* store_commit check: the file under the final name is still our record. */
static int still_ours(void *ctx, const char *path)
{
    const struct expect *x = ctx;
    return finfo_path_matches(path, x->f) ? 0 : -1;
}

/* Records the peer's entry as ours after it was installed. */
static void installed(tw *t, const finfo *r)
{
    finfo c;
    scan_item it;
    if (finfo_copy(&c, r) != 0) {
        t->state_lost = 1;
        return;
    }
    db_entry *e = put_record(t, &c);
    finfo_free(&c);
    if (!e)
        return;
    if (scan_one(t->root, r->name, &it) == 0)
        set_disk(e, &it);
    else
        e->flags |= DB_HASH_NEXT;
    e->status = TS_OK;
}

/* store_commit / store_remove_file check that accepts anything: a conflict
 * copy that loses again is not copied a second time (Syncthing's rule). */
static int accept_any(void *ctx, const char *path)
{
    (void)ctx;
    (void)path;
    return 0;
}

/* The name our version gets when the peer's wins: Syncthing's conflictName,
 * with the winning entry's modifier and the local time now. 0, or -1 when
 * the name cannot be used here (too long, not accepted). */
static int conflict_name_for(tw *t, const char *name, uint64_t by, char *out, size_t n)
{
    char why[300];
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    if (!lt || finfo_conflict_name(name, lt, by, out, n) != 0) {
        t->log("tw: \"%s\": no conflict copy name can be made", name);
        return -1;
    }
    if (store_accept(t->st, out, 0, why, sizeof why) != 0) {
        t->log("tw: \"%s\": the conflict copy name \"%s\" is not accepted here: %s", name, out, why);
        return -1;
    }
    return 0;
}

/* A file that appeared here (a conflict copy): recorded and published now. */
static void observe_name(tw *t, const char *name)
{
    scan_item it;
    memset(&it, 0, sizeof it);
    if (scan_one(t->root, name, &it) == 0)
        observe(t, name, &it, now_unix());
}

/* Takes the peer's deleted entry as ours (nothing left on disk). */
static void record_deleted(tw *t, const finfo *R)
{
    finfo c;
    if (finfo_copy(&c, R) != 0) {
        t->state_lost = 1;
        return;
    }
    db_entry *e = put_record(t, &c);
    finfo_free(&c);
    if (!e)
        return;
    e->disk_size = 0;
    e->disk_mtime_s = 0;
    e->disk_mtime_ns = 0;
    e->flags = 0;
    e->status = TS_OK;
}

/* Next deletion: files first, then directories, deepest first, so that a
 * directory is removed after what it held. */
static size_t next_deletion(const tw *t)
{
    size_t best = 0;
    int best_depth = -1;
    for (size_t i = 0; i < t->ndel; i++) {
        const db_entry *L = db_find((db *)&t->d, t->del[i].name);
        if (!L || L->f.type != BEP_DIRECTORY)
            return i;
        int depth = 0;
        for (const char *c = t->del[i].name; *c; c++)
            depth += *c == '/';
        if (depth > best_depth) {
            best_depth = depth;
            best = i;
        }
    }
    return best;
}

static void apply_deletion(tw *t, struct dl *x)
{
    char why[1200];
    const finfo *R = &x->r;
    db_entry *L = db_find(&t->d, x->name);
    db_entry *p = db_find(&t->peer, x->name);
    int rc;
    if (!L || L->f.deleted) {
        record_deleted(t, R);
        return;
    }
    if (L->f.type == BEP_DIRECTORY) {
        rc = store_remove_dir(t->st, x->name, why, sizeof why);
        if (rc == 0) {
            record_deleted(t, R);
            t->stats.deleted_here++;
            t->log("tw: directory \"%s\" deleted (deleted on the peer)", x->name);
            UI(t, "  del    %s/", x->name);
        } else if (rc == 1) {
            /* Something we do not know (or keep) is still in it: the
             * directory stays and is published again, as a newer version
             * than the deletion, so the peer creates it again. */
            finfo nf;
            int64_t now = now_unix();
            if (finfo_copy(&nf, &L->f) != 0 || vv_merge(&nf.version, &R->version) != 0
                || vv_update(&nf.version, t->me, (uint64_t)now) != 0) {
                finfo_free(&nf);
                t->state_lost = 1;
                return;
            }
            nf.modified_s = now;
            nf.modified_ns = 0;
            nf.modified_by = t->me;
            db_entry *ne = put_record(t, &nf);
            finfo_free(&nf);
            if (ne)
                ne->status = TS_OK;
            t->stats.dirs_kept++;
            t->log("tw: directory \"%s\" deleted on the peer, but %s; kept and published again", x->name, why);
            UI(t, "  kept   %s/ (not empty here)", x->name);
        } else {
            t->stats.failed++;
            t->log("tw: directory \"%s\" not deleted: %s", x->name, why);
        }
        return;
    }
    char cname[1100] = "";
    if (x->conflict && !finfo_is_conflict_name(x->name)) {
        if (conflict_name_for(t, x->name, R->modified_by, cname, sizeof cname) != 0) {
            t->stats.held++;
            if (p)
                p->status = PS_BLOCKED;
            UI(t, "  HELD   %s (deleted on the peer, changed here; no conflict copy name)", x->name);
            return;
        }
        rc = store_rename_conflict(t->st, x->name, cname, why, sizeof why);
    } else if (x->conflict) {
        rc = store_remove_file(t->st, x->name, accept_any, NULL, why, sizeof why);
    } else {
        struct expect ex = { &L->f };
        rc = store_remove_file(t->st, x->name, still_ours, &ex, why, sizeof why);
    }
    if (rc == 0) {
        record_deleted(t, R);
        if (cname[0]) {
            t->stats.conflicts++;
            t->log("tw: \"%s\" deleted on the peer (the deletion wins); ours kept as \"%s\"", x->name, cname);
            UI(t, "  CONFL  %s -> %s (deleted on the peer)", x->name, cname);
            observe_name(t, cname);
        } else {
            t->stats.deleted_here++;
            t->log("tw: \"%s\" deleted (deleted on the peer)%s%s", x->name, why[0] ? "; " : "", why);
            UI(t, "  del    %s", x->name);
        }
        return;
    }
    if (rc == 1) {
        /* Changed here since it was recorded: scanned now, which makes it
         * a new version of ours; the peer's deletion is then decided
         * against it once (a conflict, or ours wins). */
        int64_t before = L->f.sequence;
        t->log("tw: \"%s\" not deleted: %s", x->name, why);
        L->flags |= DB_HASH_NEXT;
        observe_name(t, x->name);
        L = db_find(&t->d, x->name);
        if (p && L && L->f.sequence != before && !x->conflict) {
            evaluate(t, p);
        } else {
            t->stats.held++;
            if (p)
                p->status = PS_BLOCKED;
            UI(t, "  HELD   %s (deleted on the peer; %s)", x->name, why);
        }
        return;
    }
    t->stats.failed++;
    t->log("tw: \"%s\" not deleted: %s", x->name, why);
}

const char *tw_step(tw *t, net_ms now, net_ms *wake)
{
    char why[1024];
    *wake = now + 3600 * 1000;
    if (t->waiting) {
        if (now < t->deadline) {
            *wake = t->deadline;
            return NULL;
        }
        fail_current(t, "no Response within the request time limit");
    }
    while (t->rs.phase == RS_IDLE && t->n > 0) {
        struct dl *x = &t->q[0];
        if (x->r.type == BEP_DIRECTORY) {
            if (store_mkdir(t->st, x->name, why, sizeof why) != 0) {
                fail_current(t, why);
                continue;
            }
            t->stats.dirs_made++;
            t->log("tw: directory \"%s\" present", x->name);
            UI(t, "  dir    %s/", x->name);
            installed(t, &x->r);
            pop(t);
            continue;
        }
        if (!t->file) {
            if (!(t->file = store_begin(t->st, x->name, why, sizeof why))) {
                fail_current(t, why);
                continue;
            }
            t->next_block = 0;
        }
        while (t->next_block < x->r.nblocks && x->r.blocks[t->next_block].size == 0)
            t->next_block++;
        if (t->next_block < x->r.nblocks) {
            const finfo_block *b = &x->r.blocks[t->next_block];
            pb_writer w;
            pb_writer_init(&w, 4096);
            t->req_id = t->next_id++;
            if ((t->next_id & 0xffffff) == 0)
                t->next_id -= 0xffffff; /* stays within the folder's range (tw_config.id_base) */
            bep_encode_request(&w, t->req_id, t->folder, x->name, b->offset, b->size, b->hash, 32,
                               (int32_t)t->next_block);
            enum tls_result tr = t->send(t->ctx, BEP_REQUEST, &w);
            pb_writer_free(&w);
            if (tr != TLS_OK)
                return tls_result_name(tr);
            t->waiting = 1;
            t->deadline = now + REQUEST_SECONDS * 1000;
            *wake = t->deadline;
            return NULL;
        }
        /* What was here when the download was decided must still be
         * here: our recorded version, or nothing. */
        db_entry *L = db_find(&t->d, x->name);
        struct expect ex = { L && !L->f.deleted ? &L->f : NULL };
        int replaced = 0, cr;
        char cname[1100] = "";
        store_file *done = t->file;
        t->file = NULL;
        why[0] = 0;
        if (x->conflict && ex.f && finfo_is_conflict_name(x->name)) {
            cr = store_commit(done, accept_any, NULL, &replaced, why, sizeof why);
        } else if (x->conflict && ex.f) {
            if (conflict_name_for(t, x->name, x->r.modified_by, cname, sizeof cname) != 0) {
                store_abort(done);
                t->stats.held++;
                db_entry *p = db_find(&t->peer, x->name);
                if (p)
                    p->status = PS_BLOCKED;
                UI(t, "  HELD   %s (edited on both sides; no conflict copy name)", x->name);
                pop(t);
                continue;
            }
            cr = store_commit_conflict(done, cname, &replaced, why, sizeof why);
            if (!replaced)
                cname[0] = 0;
        } else {
            cr = store_commit(done, ex.f ? still_ours : NULL, &ex, &replaced, why, sizeof why);
        }
        if (cr != 0) {
            if (cr == 1) {
                t->stats.held++;
                db_entry *p = db_find(&t->peer, x->name);
                if (p)
                    p->status = PS_BLOCKED;
                t->log("tw: \"%s\": changed here during the download (%s); kept, the peer's version not installed",
                       x->name, why);
                UI(t, "  HELD   %s (changed here during the download; kept)", x->name);
                if (L)
                    L->flags |= DB_HASH_NEXT;
            } else {
                t->stats.failed++;
                t->log("tw: \"%s\" failed: %s", x->name, why);
            }
            pop(t);
            continue;
        }
        now = net_now();
        char sz[32];
        t->stats.downloaded++;
        t->log("tw: wrote \"%s\" (%lld bytes, %s)", x->name, (long long)x->r.size,
               replaced ? "replaced our previous version" : "new");
        if (why[0])
            t->log("tw: \"%s\": %s", x->name, why);
        snprintf(sz, sizeof sz, "%lld B", (long long)x->r.size);
        UI(t, "  file   %-30s %9s   SHA-256 verified%s", x->name, sz, replaced ? ", replaced" : "");
        installed(t, &x->r);
        if (cname[0]) {
            t->stats.conflicts++;
            t->log("tw: \"%s\" edited on both sides, the peer's version wins; ours kept as \"%s\"", x->name, cname);
            UI(t, "  CONFL  %s -> %s", x->name, cname);
            observe_name(t, cname);
        }
        pop(t);
    }
    while (t->rs.phase == RS_IDLE && t->n == 0 && !t->file && t->ndel > 0) {
        size_t i = next_deletion(t);
        struct dl x = t->del[i];
        memmove(t->del + i, t->del + i + 1, (t->ndel - i - 1) * sizeof *t->del);
        t->ndel--;
        apply_deletion(t, &x);
        free_dl(&x);
    }
    if (t->rs.phase != RS_IDLE
        || (t->rescan_seconds > 0 && now >= t->next_rescan && t->n == 0 && t->ndel == 0 && !t->file
            && (t->peer_complete || !t->attached))) {
        /* slice_ms < 0 (tests): one piece of work per call */
        const char *r = rescan_step(t, t->slice_ms > 0   ? net_now() + t->slice_ms
                                       : t->slice_ms < 0 ? 0
                                                         : net_now() + 3600 * 1000);
        if (r)
            return r;
        if (t->rs.phase == RS_IDLE)
            t->next_rescan = net_now() + t->rescan_seconds * 1000;
    }
    if (t->dirty && net_now() - t->saved_at >= t->gap)
        save(t);
    if (t->dirty) {
        net_ms at = t->saved_at + t->gap;
        if (at < *wake)
            *wake = at;
    }
    if (t->rescan_seconds > 0 && t->next_rescan < *wake && (t->peer_complete || !t->attached))
        *wake = t->next_rescan > now ? t->next_rescan : now;
    if (t->rs.phase != RS_IDLE)
        *wake = now; /* the scan continues at once: only look at the connection in between */
    return NULL;
}

void tw_scan_soon(tw *t)
{
    t->next_rescan = net_now();
}

int tw_scanning(const tw *t)
{
    return t->rs.phase != RS_IDLE;
}

int tw_idle(const tw *t)
{
    return t->n == 0 && t->ndel == 0 && !t->waiting && !t->dirty;
}

int tw_settled(const tw *t)
{
    if (!tw_idle(t) || !t->index_started || !t->peer_complete || t->announced_seq < t->saved_seq)
        return 0;
    for (size_t i = 0; i < t->d.n; i++) {
        const db_entry *e = &t->d.e[i];
        if (e->status == TS_HELD || e->status == TS_UNKNOWN)
            continue;
        db_entry *p = db_find((db *)&t->peer, e->f.name);
        if (!p) {
            if (e->f.deleted)
                continue;
            return 0; /* the peer does not have it yet */
        }
        if (p->status == PS_BLOCKED)
            continue;
        if (vv_compare(&p->f.version, &e->f.version) != VV_EQUAL)
            return 0;
    }
    for (size_t i = 0; i < t->peer.n; i++) {
        const db_entry *p = &t->peer.e[i];
        if (p->status == PS_BLOCKED || db_find((db *)&t->d, p->f.name))
            continue; /* blocked, or compared above */
        if (!p->f.deleted && !p->f.invalid)
            return 0;
    }
    return 1;
}

int tw_state_saved(const tw *t)
{
    return !t->dirty && !t->state_lost;
}

const char *tw_result(const tw *t)
{
    tw_stats ss;
    tw_get_session_stats(t, &ss);
    if (ss.failed > 0)
        return "FILES_FAILED";
    if (t->dirty || t->state_lost)
        return "STATE_NOT_SAVED";
    if (t->stats.unchecked > 0)
        return "UNCHECKED";
    for (size_t i = 0; i < t->d.n; i++)
        if (t->d.e[i].status == TS_UNKNOWN)
            return "UNCHECKED";
    if (!tw_settled(t))
        return "NOT_IN_SYNC";
    if (ss.held > 0 || ss.unsupported > 0 || ss.rejected > 0)
        return "PARTIAL";
    return NULL;
}
