#include "fetch.h"

#include "prof.h"
#include "state.h"

#include <openssl/evp.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Syncthing's largest block is 16 MiB (lib/protocol MaxBlockSize). */
#define MAX_BLOCK (16 << 20)
/* Per request, in guest time; the session's overall limit still applies. */
#define REQUEST_SECONDS 60
/* The state file is rewritten as a whole; doing that for every entry is
 * quadratic in the folder size (1000 adoptions took over 15 minutes on the
 * Raspberry Pi's FAT32, 2026-11-09). Recorded entries are written at most
 * this often, after this many, when the queue becomes idle and at the end
 * of the session (fetch_flush). Until then only the record can be lost,
 * never a file: the next run adopts what is here by its hash. */
#define STATE_SAVE_MS 5000
#define STATE_SAVE_ENTRIES 256
/* A write of a large state file on slow media can take longer than
 * STATE_SAVE_MS (seconds on the Pi's FAT32); the next one then waits at
 * least this many times as long as the last took, so writing never takes
 * more than about a fifth of the time. */
#define STATE_SAVE_FACTOR 4
/* After a failed write the next attempt waits this long, doubled after
 * every further failure, up to STATE_RETRY_MAX_MS; fetch_flush (the end of
 * the session) always makes one more attempt. */
#define STATE_RETRY_MS 5000
#define STATE_RETRY_MAX_MS 60000

#ifdef FETCH_TEST_HOOKS
int fetch_test_fail_state_writes; /* the next N state writes fail */
#endif
/* Body size at which the initial index is split into another message. */
#define INDEX_BATCH (1u << 20)

/* Status of a recorded entry, found by checking the disk at start. */
enum { ST_UNCHECKED = 0, ST_PRESENT, ST_CHANGED };

/* SHA-256 of nothing: the hash Syncthing gives the one block of an empty file. */
static const unsigned char sha256_empty[32] = {
    0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
    0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55
};

struct block {
    int64_t offset;
    int32_t size;
    unsigned char hash[32];
};

struct item {
    char *name;
    unsigned char *raw; /* the peer's FileInfo, announced back when done */
    size_t raw_len;
    int is_dir;
    int64_t size;
    struct block *blocks;
    size_t nblocks;
};

struct fetch {
    char *folder;
    store *st;
    tls_log_fn log, ui;
    fetch_send_fn send;
    void *ctx;
    long abort_after;

    struct item *q; /* q[0] is the one being worked on */
    size_t n, cap;

    store_file *file;
    size_t next_block;
    int waiting;
    int32_t req_id, next_id;
    net_ms req_deadline;
    int64_t my_seq;
    long blocks_written;
    fetch_stats stats;

    /* persistent state */
    char *state_path, *dest;
    unsigned char peer_id[DEVID_BYTES];
    int state_fault;
    state_list state;
    int state_dirty;     /* recorded entries not yet written to the file */
    long state_pending;  /* how many */
    net_ms state_saved_at; /* when the last write ended */
    net_ms state_save_gap; /* time to wait after it */
    int state_lost;        /* an entry could not even be recorded in memory */
    char *hook_dir;        /* test hook, see fetch_set_commit_hook */
    int state_fail_run;    /* failed writes in a row */
};

/* User-facing line, when the caller wants them. */
#define UI(f, ...) do { if ((f)->ui) (f)->ui(__VA_ARGS__); } while (0)

const char *fetch_size_text(long long bytes, char *buf, size_t n)
{
    if (bytes < 1024)
        snprintf(buf, n, "%lld B", bytes);
    else if (bytes < 1024 * 1024)
        snprintf(buf, n, "%.1f KB", bytes / 1024.0);
    else
        snprintf(buf, n, "%.1f MB", bytes / (1024.0 * 1024.0));
    return buf;
}

fetch *fetch_new(const char *folder, store *st, tls_log_fn log, tls_log_fn ui, fetch_send_fn send,
                 void *ctx, long abort_after_blocks, const fetch_state_opts *so)
{
    fetch *f = calloc(1, sizeof *f);
    if (!f || !(f->folder = strdup(folder))) {
        free(f);
        return NULL;
    }
    f->st = st;
    f->log = log;
    f->ui = ui;
    f->send = send;
    f->ctx = ctx;
    f->abort_after = abort_after_blocks;
    f->next_id = 1;
    if (so && so->state_path) {
        f->state_path = strdup(so->state_path);
        f->dest = strdup(so->dest);
        memcpy(f->peer_id, so->peer_id, DEVID_BYTES);
        f->state_fault = so->state_fault;
        if (!f->state_path || !f->dest) {
            fetch_free(f);
            return NULL;
        }
    }
    return f;
}

static void free_item(struct item *it)
{
    free(it->name);
    free(it->raw);
    free(it->blocks);
    memset(it, 0, sizeof *it);
}

void fetch_free(fetch *f)
{
    if (!f)
        return;
    store_abort(f->file);
    for (size_t i = 0; i < f->n; i++)
        free_item(&f->q[i]);
    free(f->q);
    free(f->folder);
    free(f->state_path);
    free(f->dest);
    free(f->hook_dir);
    state_list_free(&f->state);
    free(f);
}

void fetch_set_commit_hook(fetch *f, const char *dir)
{
    free(f->hook_dir);
    f->hook_dir = dir ? strdup(dir) : NULL;
}

static int file_exists(const char *p)
{
    FILE *fp = fopen(p, "rb");
    if (fp)
        fclose(fp);
    return fp != NULL;
}

/* The commit hook: announce the point, wait for the test's go-ahead. */
static void commit_hook(fetch *f, const char *name)
{
    if (!f->hook_dir)
        return;
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    char at[1200], go[1200];
    snprintf(at, sizeof at, "%s/at-%s", f->hook_dir, base);
    snprintf(go, sizeof go, "%s/go-%s", f->hook_dir, base);
    FILE *fp = fopen(at, "wb");
    if (fp)
        fclose(fp);
    f->log("fetch: TEST HOOK: \"%s\" downloaded; waiting for %s before the commit", name, go);
    net_ms until = net_now() + 120 * 1000;
    while (!file_exists(go) && net_now() < until)
        sleep(1);
    f->log("fetch: TEST HOOK: %s, committing \"%s\"", file_exists(go) ? "go-ahead" : "no go-ahead in 120 s", name);
    remove(go);
    remove(at);
}

int fetch_idle(const fetch *f) { return f->n == 0 && !f->waiting; }
const fetch_stats *fetch_get_stats(const fetch *f) { return &f->stats; }

static void pop(fetch *f)
{
    free_item(&f->q[0]);
    memmove(f->q, f->q + 1, (f->n - 1) * sizeof *f->q);
    f->n--;
    f->next_block = 0;
}

static void fail_current(fetch *f, const char *why)
{
    f->log("fetch: \"%s\" failed: %s; previous version (if any) left as it was", f->q[0].name, why);
    UI(f, "  FAILED %s: %s", f->q[0].name, why);
    store_abort(f->file);
    f->file = NULL;
    f->waiting = 0;
    f->stats.failed++;
    pop(f);
}

/* ---- block lists and local content ---- */

struct collect {
    struct block *b;
    size_t n, cap;
    int64_t expect_offset;
    const char *error;
};

static int collect_block(void *ctx, int64_t offset, int32_t size, pb_slice hash)
{
    struct collect *c = ctx;
    if (hash.len != 32) {
        c->error = "block hash is not SHA-256";
        return 1;
    }
    if (size < 0 || size > MAX_BLOCK) {
        c->error = "block size out of range";
        return 1;
    }
    if (offset != c->expect_offset) {
        c->error = "blocks are not contiguous";
        return 1;
    }
    if (c->n == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 16;
        struct block *nb = realloc(c->b, cap * sizeof *nb);
        if (!nb) {
            c->error = "out of memory";
            return 1;
        }
        c->b = nb;
        c->cap = cap;
    }
    c->b[c->n].offset = offset;
    c->b[c->n].size = size;
    memcpy(c->b[c->n].hash, hash.data, 32);
    c->n++;
    c->expect_offset += size;
    return 0;
}

/* Validates a FileInfo's block list against its size. On success c->b
 * holds the blocks (caller frees). */
static const char *blocks_of(pb_slice raw, int64_t size, struct collect *c)
{
    memset(c, 0, sizeof *c);
    if (bep_file_blocks(raw, c, collect_block) != 0 && !c->error)
        c->error = "malformed block list";
    if (!c->error && c->expect_offset != size)
        c->error = "blocks do not add up to the file size";
    if (!c->error && size > 0 && c->n == 0)
        c->error = "no blocks listed (not supported)";
    if (c->error) {
        free(c->b);
        c->b = NULL;
    }
    return c->error;
}

/* Same content: same size and the same block hashes, in order. */
static int same_content(pb_slice a, pb_slice b)
{
    bep_file fa, fb;
    struct collect ca, cb;
    if (bep_decode_fileinfo(a, &fa) != 0 || bep_decode_fileinfo(b, &fb) != 0 || fa.size != fb.size)
        return 0;
    if (blocks_of(a, fa.size, &ca) || blocks_of(b, fb.size, &cb)) {
        free(ca.b);
        return 0;
    }
    int same = ca.n == cb.n;
    for (size_t i = 0; same && i < ca.n; i++)
        same = ca.b[i].size == cb.b[i].size && memcmp(ca.b[i].hash, cb.b[i].hash, 32) == 0;
    free(ca.b);
    free(cb.b);
    return same;
}

enum { LOCAL_MATCH, LOCAL_MISSING, LOCAL_DIFFERENT };

/* Does the regular file at path hold exactly the content described by raw
 * (every block hashed, nothing more)? */
static int path_matches_(const char *path, pb_slice raw);
static int path_matches(const char *path, pb_slice raw)
{
    PROF_BEGIN(t);
    int r = path_matches_(path, raw);
    PROF_END(PROF_HASH, t);
    return r;
}
static int path_matches_(const char *path, pb_slice raw)
{
    bep_file fi;
    struct collect c;
    if (bep_decode_fileinfo(raw, &fi) != 0 || blocks_of(raw, fi.size, &c))
        return 0;
    FILE *fp = path ? fopen(path, "rb") : NULL;
    int match = fp != NULL;
    unsigned char *buf = NULL;
    for (size_t i = 0; fp && i < c.n && match; i++) {
        unsigned char *nb = realloc(buf, c.b[i].size ? (size_t)c.b[i].size : 1);
        unsigned char md[32];
        unsigned int mdlen = 0;
        if (!nb) {
            match = 0;
            break;
        }
        buf = nb;
        if (fread(buf, 1, (size_t)c.b[i].size, fp) != (size_t)c.b[i].size
            || !EVP_Digest(buf, (size_t)c.b[i].size, md, &mdlen, EVP_sha256(), NULL)
            || memcmp(md, c.b[i].hash, 32) != 0)
            match = 0;
    }
    if (fp && match && fgetc(fp) != EOF)
        match = 0; /* longer than recorded */
    if (fp)
        fclose(fp);
    free(buf);
    free(c.b);
    return match;
}

/* Does what is on disk at name hold exactly the content described by raw
 * (a file: every block hashed; a directory: exists as one)? Anything that
 * cannot be checked counts as different, so it is never announced. */
static int local_check(fetch *f, const char *name, int is_dir, pb_slice raw)
{
    PROF_BEGIN(t);
    int kind = store_local_kind(f->st, name);
    PROF_END(PROF_LOOKUP, t);
    if (kind == 0)
        return LOCAL_MISSING;
    if (is_dir)
        return kind == 2 ? LOCAL_MATCH : LOCAL_DIFFERENT;
    if (kind != 1)
        return LOCAL_DIFFERENT;
    char *path = store_full_path(f->st, name);
    int m = path_matches(path, raw);
    free(path);
    return m ? LOCAL_MATCH : LOCAL_DIFFERENT;
}

/* store_commit check: the file still holds the recorded version. */
static int still_recorded(void *ctx, const char *path)
{
    const state_entry *e = ctx;
    return path_matches(path, (pb_slice){ e->raw, e->raw_len }) ? 0 : -1;
}

/* Deals with a moved-aside copy of name left by an earlier run. Puts it
 * back when the file itself is missing. When both are here, the copy is
 * dropped only if it is exactly the recorded version (a replacement that
 * completed just before an interruption: nothing but the peer's earlier
 * version would be lost); anything else is kept and the name is blocked.
 * Returns 1 when the name is blocked by a kept copy, 0 otherwise. */
static int recover(fetch *f, const char *name)
{
    char why[1200];
    PROF_BEGIN(t);
    int r = store_recover(f->st, name, why, sizeof why);
    PROF_END(PROF_LOOKUP, t);
    if (r == 1) {
        f->log("store: \"%s\": previous version put back after an interrupted replacement", name);
        UI(f, "  restored %s (previous version, after an interrupted replacement)", name);
        return 0;
    }
    if (r < 0) {
        f->log("store: \"%s\": %s", name, why);
        return 1;
    }
    if (r != 2)
        return 0;
    state_entry *e = f->state_path ? state_find(&f->state, name) : NULL;
    char *old = store_old_path(f->st, name);
    int recorded = e && !e->is_dir && old && path_matches(old, (pb_slice){ e->raw, e->raw_len });
    if (recorded && store_discard_old(f->st, name, why, sizeof why) == 0) {
        f->log("store: \"%s\": earlier copy %s was the recorded version; dropped", name, old);
        free(old);
        return 0;
    }
    f->log("store: \"%s\": an earlier copy %s is kept (%s); the file is not changed until it is dealt with",
           name, old ? old : "?", recorded ? why : "not the recorded version");
    free(old);
    return 1;
}

/* ---- persistent state ---- */

static int save_state(fetch *f)
{
    char why[1200];
    if (!f->state_path)
        return 0;
    f->stats.state_saves++;
    net_ms started = net_now();
    PROF_BEGIN(pt);
    int fault = f->state_fault;
#ifdef FETCH_TEST_HOOKS
    if (fetch_test_fail_state_writes > 0) {
        fetch_test_fail_state_writes--;
        fault = 1;
    }
#endif
    int rc = fault ? -1 : state_save(f->state_path, f->folder, f->dest, f->peer_id, &f->state, why, sizeof why);
    if (fault)
        snprintf(why, sizeof why, "test hook: state write made to fail");
    PROF_END(PROF_STATE_WRITE, pt);
    f->state_saved_at = net_now();
    if (rc == 0) {
        /* Only a write that worked takes the entries off the to-do list. */
        f->state_dirty = 0;
        f->state_pending = 0;
        f->state_fail_run = 0;
        f->state_save_gap = (f->state_saved_at - started) * STATE_SAVE_FACTOR;
        if (f->state_save_gap < STATE_SAVE_MS)
            f->state_save_gap = STATE_SAVE_MS;
        return 0;
    }
    f->stats.state_failures++;
    f->state_fail_run++;
    f->state_save_gap = STATE_RETRY_MS;
    for (int i = 1; i < f->state_fail_run && f->state_save_gap < STATE_RETRY_MAX_MS; i++)
        f->state_save_gap *= 2;
    if (f->state_save_gap > STATE_RETRY_MAX_MS)
        f->state_save_gap = STATE_RETRY_MAX_MS;
    f->log("state: could not record the state: %s; the written files stay; retrying in %lld s",
           why, (long long)(f->state_save_gap / 1000));
    UI(f, "  WARNING the local state could not be saved (%s); files are kept, retrying", why);
    return -1;
}

int fetch_flush(fetch *f)
{
    return f->state_dirty ? save_state(f) : 0;
}

/* The idle write in fetch_step honours the spacing (and the back-off
 * after failures); fetch_flush does not. */
static void flush_when_due(fetch *f)
{
    if (f->state_dirty && net_now() - f->state_saved_at >= f->state_save_gap)
        save_state(f);
}

const char *fetch_result(const fetch *f)
{
    if (f->stats.failed > 0)
        return "FILES_FAILED";
    if (f->stats.conflicts > 0)
        return "LOCAL_CONFLICTS";
    if (f->state_dirty || f->state_lost)
        return "STATE_NOT_SAVED";
    if (f->stats.rejected > 0 || f->stats.unsupported > 0)
        return "PARTIAL";
    return NULL;
}

/* Records an entry as present (in memory and on disk). */
static void record(fetch *f, const char *name, int is_dir, const unsigned char *raw, size_t raw_len)
{
    if (!f->state_path)
        return;
    PROF_BEGIN(t);
    int pr = state_put(&f->state, name, is_dir, raw, raw_len);
    if (pr == 0)
        state_find(&f->state, name)->status = ST_PRESENT;
    PROF_END(PROF_STATE_MEM, t);
    if (pr != 0) {
        f->stats.state_failures++;
        f->state_lost = 1;
        f->log("state: cannot record \"%s\" (out of memory or too many entries)", name);
        return;
    }
    f->state_dirty = 1;
    f->state_pending++;
    /* After a failed write only the back-off decides; the pending count
     * stays past the batch size and would otherwise retry for every entry. */
    int due = net_now() - f->state_saved_at >= f->state_save_gap;
    if (due || (f->state_fail_run == 0 && f->state_pending >= STATE_SAVE_ENTRIES))
        save_state(f);
}

int fetch_prepare(fetch *f)
{
    char why[1200];
    f->state_saved_at = net_now();
    f->state_save_gap = STATE_SAVE_MS;
    if (!f->state_path)
        return 0;
    int rc = state_load(f->state_path, f->folder, f->dest, f->peer_id, &f->state, why, sizeof why);
    if (rc == 1) {
        f->log("state: none recorded (%s)", f->state_path);
        UI(f, "Local state: none yet (first run)");
        return 0;
    }
    if (rc != 0) {
        f->log("state: rejected (%s): %s; starting as if nothing were recorded", f->state_path, why);
        UI(f, "Local state rejected (%s); files here are checked against the peer's index", why);
        return 0;
    }
    long present = 0, missing = 0, changed = 0, total = (long)f->state.n;
    for (size_t i = 0; i < f->state.n;) {
        state_entry *e = &f->state.e[i];
        if (store_accept(f->st, e->name, e->is_dir, why, sizeof why) != 0) {
            /* The name rules may differ from those of the version that wrote
             * the state; such an entry is simply not trusted. */
            f->log("state: entry \"%s\" not accepted (%s); ignored", e->name, why);
            state_remove(&f->state, e->name);
            continue;
        }
        if (!e->is_dir)
            (void)recover(f, e->name); /* a blocked name is reported when it is offered */
        int r = local_check(f, e->name, e->is_dir, (pb_slice){ e->raw, e->raw_len });
        if (r == LOCAL_MISSING) {
            f->log("state: \"%s\" is missing here; it will be downloaded again", e->name);
            missing++;
            state_remove(&f->state, e->name);
            continue;
        }
        e->status = r == LOCAL_MATCH ? ST_PRESENT : ST_CHANGED;
        if (r == LOCAL_MATCH)
            present++;
        else {
            changed++;
            f->log("state: \"%s\" was changed here since it was written; kept, not announced", e->name);
        }
        i++;
    }
    f->log("state: %ld recorded, %ld verified by SHA-256, %ld missing, %ld changed here", total, present, missing,
           changed);
    UI(f, "Local state: %ld entries recorded; %ld verified by SHA-256, %ld missing, %ld changed here", total,
       present, missing, changed);
    return 0;
}

const char *fetch_send_index(fetch *f)
{
    /* A full index of what is verified present, numbered from 1. With a new
     * index ID every session the peer drops what it knew about us and takes
     * this as our complete state (lib/model/indexhandler.go). */
    size_t i = 0;
    int first = 1;
    do {
        pb_writer w;
        pb_writer_init(&w, (size_t)64 << 20);
        int64_t prev = f->my_seq, seq = f->my_seq;
        pb_writer files;
        pb_writer_init(&files, (size_t)64 << 20);
        for (; i < f->state.n && files.len < INDEX_BATCH; i++) {
            state_entry *e = &f->state.e[i];
            if (e->status != ST_PRESENT)
                continue;
            if (bep_put_announced_file(&files, (pb_slice){ e->raw, e->raw_len }, seq + 1) != 0) {
                pb_writer_free(&files);
                pb_writer_free(&w);
                return "INTERNAL_ERROR";
            }
            seq++;
        }
        if (first) {
            pb_put_string_field(&w, 1, f->folder);
            pb_put_varint_field(&w, 3, (uint64_t)seq);
        } else {
            bep_encode_index_update_start(&w, f->folder, seq, prev);
        }
        pb_put_raw(&w, files.data, files.len);
        pb_writer_free(&files);
        enum tls_result r = f->send(f->ctx, first ? BEP_INDEX : BEP_INDEX_UPDATE, &w);
        pb_writer_free(&w);
        if (r != TLS_OK)
            return tls_result_name(r);
        f->stats.announced += (long)(seq - prev);
        f->log("fetch: sent %s announcing %lld entr%s (our sequence %lld..%lld)",
               first ? "Index" : "IndexUpdate", (long long)(seq - prev), seq - prev == 1 ? "y" : "ies",
               (long long)(prev + 1), (long long)seq);
        f->my_seq = seq;
        first = 0;
    } while (i < f->state.n);
    return NULL;
}

/* ---- announcing ---- */

static const char *announce_raw_(fetch *f, const char *name, const unsigned char *raw, size_t raw_len);
static const char *announce_raw(fetch *f, const char *name, const unsigned char *raw, size_t raw_len)
{
    PROF_BEGIN(t);
    const char *r = announce_raw_(f, name, raw, raw_len);
    PROF_END(PROF_ANNOUNCE, t);
    return r;
}
static const char *announce_raw_(fetch *f, const char *name, const unsigned char *raw, size_t raw_len)
{
    pb_writer w;
    pb_writer_init(&w, (size_t)64 << 20);
    bep_encode_index_update_start(&w, f->folder, f->my_seq + 1, f->my_seq);
    if (bep_put_announced_file(&w, (pb_slice){ raw, raw_len }, f->my_seq + 1) != 0) {
        pb_writer_free(&w);
        f->log("fetch: cannot re-encode \"%s\" for announcing", name);
        return "INTERNAL_ERROR";
    }
    enum tls_result r = f->send(f->ctx, BEP_INDEX_UPDATE, &w);
    pb_writer_free(&w);
    if (r != TLS_OK)
        return tls_result_name(r);
    f->my_seq++;
    f->stats.announced++;
    f->log("fetch: announced \"%s\" as present (our sequence %lld)", name, (long long)f->my_seq);
    return NULL;
}

static const char *announce(fetch *f, struct item *it)
{
    return announce_raw(f, it->name, it->raw, it->raw_len);
}

/* ---- accepting index entries ---- */

/* Forgets any earlier work for name: a queued entry is removed, a started
 * download is stopped and its temporary file deleted. Its Response, if it
 * still arrives, matches no outstanding request and is ignored, so nothing
 * of the older version is written or announced. */
static void drop_queued(fetch *f, const char *name)
{
    for (size_t i = 0; i < f->n; i++) {
        if (strcmp(f->q[i].name, name) != 0)
            continue;
        if (i == 0 && (f->file || f->waiting)) {
            f->log("fetch: newer entry for \"%s\" arrived during its download; download stopped", name);
            store_abort(f->file);
            f->file = NULL;
            f->waiting = 0;
            f->next_block = 0;
        } else {
            f->log("fetch: newer entry for \"%s\" replaces the queued one", name);
        }
        free_item(&f->q[i]);
        memmove(f->q + i, f->q + i + 1, (f->n - i - 1) * sizeof *f->q);
        f->n--;
        if (i == 0)
            f->next_block = 0;
        return; /* a name is queued at most once */
    }
}

static void conflict(fetch *f, const char *name, const char *why)
{
    f->stats.conflicts++;
    f->log("fetch: CONFLICT \"%s\": %s; kept as it is, not overwritten, not announced", name, why);
    UI(f, "  CONFLICT %s: %s; kept, not synchronised", name, why);
}

const char *fetch_offer(fetch *f, const bep_file *fi)
{
    char why[1024], sz[32];
    if (fi->name.len == 0 || memchr(fi->name.data, 0, fi->name.len)) {
        f->log("fetch: entry with an empty or NUL-containing name rejected");
        f->stats.rejected++;
        return NULL;
    }
    char *name = malloc(fi->name.len + 1);
    if (!name)
        return "OUT_OF_MEMORY";
    memcpy(name, fi->name.data, fi->name.len);
    name[fi->name.len] = 0;

    /* The newest entry for a name decides what happens to it: whatever was
     * planned for an earlier one (queued or downloading) is dropped first,
     * whether this one ends in a download, "already here", an adoption, a
     * conflict or a skip. */
    drop_queued(f, name);

    const char *skip = NULL;
    if (fi->deleted)
        skip = "deletion (not applied by this version)";
    else if (fi->invalid)
        skip = "marked invalid by the peer";
    else if (fi->type == BEP_SYMLINK || fi->type == BEP_SYMLINK_FILE || fi->type == BEP_SYMLINK_DIRECTORY)
        skip = "symbolic link (not supported)";
    else if (fi->type != BEP_FILE && fi->type != BEP_DIRECTORY)
        skip = "unknown entry type";
    if (skip) {
        int deliberate = fi->deleted || fi->invalid;
        f->log("fetch: %s \"%s\": %s", deliberate ? "skipped" : "NOT RECEIVED", name, skip);
        UI(f, "  %s %s (%s)", deliberate ? "skip  " : "MISSED", name, skip);
        if (deliberate)
            f->stats.skipped++;
        else
            f->stats.unsupported++;
        free(name);
        return NULL;
    }
    int is_dir = fi->type == BEP_DIRECTORY;
    PROF_BEGIN(ta);
    int acc = store_accept(f->st, name, is_dir, why, sizeof why);
    PROF_END(PROF_ACCEPT, ta);
    if (acc != 0) {
        f->log("fetch: rejected \"%s\": %s", name, why);
        UI(f, "  skip   %s (name not accepted here: %s)", name, why);
        f->stats.rejected++;
        free(name);
        return NULL;
    }

    struct item it;
    memset(&it, 0, sizeof it);
    it.name = name;
    it.is_dir = is_dir;
    it.size = fi->size;
    if (!is_dir) {
        struct collect c;
        const char *err = blocks_of(fi->raw, fi->size, &c);
        if (err) {
            f->log("fetch: rejected \"%s\": %s", name, err);
            f->stats.rejected++;
            free(name);
            return NULL;
        }
        it.blocks = c.b;
        it.nblocks = c.n;
    }
    if (!(it.raw = malloc(fi->raw.len ? fi->raw.len : 1))) {
        free_item(&it);
        return "OUT_OF_MEMORY";
    }
    memcpy(it.raw, fi->raw.data, fi->raw.len);
    it.raw_len = fi->raw.len;

    /* What do we already have under this name? */
    if (!is_dir && recover(f, name)) {
        conflict(f, name, "an earlier copy of it is kept here (.stpoc-...old); move or delete that copy");
        free_item(&it);
        return NULL;
    }
    PROF_BEGIN(tf);
    state_entry *e = f->state_path ? state_find(&f->state, name) : NULL;
    PROF_END(PROF_STATE_MEM, tf);
    const char *r = NULL;
    if (e && e->status == ST_CHANGED) {
        /* It differs from what we recorded; it may still be exactly what
         * the peer offers now (a file written in a run whose state write
         * failed). Only a real difference is a local change. */
        if (e->is_dir == is_dir && local_check(f, name, is_dir, fi->raw) == LOCAL_MATCH) {
            f->stats.adopted++;
            UI(f, "  same   %-30s %9s   found here, SHA-256 matches", name, fetch_size_text(it.size, sz, sizeof sz));
            f->log("fetch: \"%s\" differs from the record but matches the peer's version; adopted", name);
            record(f, name, is_dir, it.raw, it.raw_len);
            r = announce(f, &it);
            free_item(&it);
            return r;
        }
        conflict(f, name, "changed here since it was received");
        free_item(&it);
        return NULL;
    }
    if (e && e->status == ST_PRESENT) {
        if (e->is_dir != is_dir) {
            conflict(f, name, "the peer changed it between file and directory");
            free_item(&it);
            return NULL;
        }
        if (is_dir || same_content((pb_slice){ e->raw, e->raw_len }, fi->raw)) {
            /* Content already here; record and announce the peer's current
             * FileInfo if it differs (a newer version with the same data). */
            int newer = e->raw_len != it.raw_len || memcmp(e->raw, it.raw, it.raw_len) != 0;
            if (!is_dir) {
                f->stats.present++;
                UI(f, "  same   %-30s %9s   already here, SHA-256 verified", name,
                   fetch_size_text(it.size, sz, sizeof sz));
            }
            if (newer) {
                record(f, name, is_dir, it.raw, it.raw_len);
                r = announce(f, &it);
            }
            free_item(&it);
            return r;
        }
        /* The peer has different content and ours is exactly what we wrote:
         * safe to replace (queued below). */
    } else if (f->state_path) {
        /* Not recorded (or recorded but missing): look at the disk. */
        int kind = store_local_kind(f->st, name);
        if (kind != 0) {
            int lc = local_check(f, name, is_dir, fi->raw);
            if (lc == LOCAL_MATCH) {
                if (!is_dir) {
                    f->stats.adopted++;
                    UI(f, "  same   %-30s %9s   found here, SHA-256 matches", name,
                       fetch_size_text(it.size, sz, sizeof sz));
                }
                record(f, name, is_dir, it.raw, it.raw_len);
                r = announce(f, &it);
                free_item(&it);
                return r;
            }
            conflict(f, name, kind == -1 ? "something else is here" : "a different version is here");
            free_item(&it);
            return NULL;
        }
    }

    if (f->n == f->cap) {
        size_t cap = f->cap ? f->cap * 2 : 16;
        struct item *nq = realloc(f->q, cap * sizeof *nq);
        if (!nq) {
            free_item(&it);
            return "OUT_OF_MEMORY";
        }
        f->q = nq;
        f->cap = cap;
    }
    f->q[f->n++] = it;
    return NULL;
}

/* ---- the transfer ---- */

const char *fetch_response(fetch *f, int32_t id, pb_slice data, uint32_t code)
{
    if (!f->waiting || id != f->req_id) {
        f->log("fetch: Response %d matches no outstanding request; ignored", id);
        return NULL;
    }
    f->waiting = 0;
    struct item *it = &f->q[0];
    struct block *b = &it->blocks[f->next_block];
    char why[160];
    if (code != 0) {
        static const char *names[] = { "NO_ERROR", "GENERIC", "NO_SUCH_FILE", "INVALID_FILE" };
        snprintf(why, sizeof why, "block %lu: peer answered %s", (unsigned long)f->next_block,
                 code < 4 ? names[code] : "an unknown error code");
        fail_current(f, why);
        return NULL;
    }
    if (data.len != (size_t)b->size) {
        snprintf(why, sizeof why, "block %lu: %lu bytes received, %ld expected", (unsigned long)f->next_block,
                 (unsigned long)data.len, (long)b->size);
        fail_current(f, why);
        return NULL;
    }
    unsigned char md[32];
    unsigned int mdlen = 0;
    if (!EVP_Digest(data.data, data.len, md, &mdlen, EVP_sha256(), NULL) || mdlen != 32
        || memcmp(md, b->hash, 32) != 0) {
        snprintf(why, sizeof why, "block %lu: SHA-256 mismatch", (unsigned long)f->next_block);
        fail_current(f, why);
        return NULL;
    }
    if (store_write(f->file, b->offset, data.data, data.len) != 0) {
        fail_current(f, "write to the temporary file failed");
        return NULL;
    }
    f->next_block++;
    f->blocks_written++;
    f->stats.bytes += (long long)data.len;
    if (f->abort_after > 0 && f->blocks_written >= f->abort_after) {
        f->log("fetch: test hook: stopping after %ld block(s) during \"%s\" (block %lu of %lu); "
               "temporary file left, final name untouched",
               f->blocks_written, it->name, (unsigned long)f->next_block, (unsigned long)it->nblocks);
        store_abandon(f->file);
        f->file = NULL;
        return "TEST_INTERRUPTED";
    }
    return NULL;
}

const char *fetch_step(fetch *f, net_ms now, net_ms *wake)
{
    char why[1024];
    *wake = now + 3600 * 1000;
    if (f->waiting) {
        if (now >= f->req_deadline) {
            fail_current(f, "no Response within the request time limit");
        } else {
            *wake = f->req_deadline;
            return NULL;
        }
    }
    while (f->n > 0) {
        struct item *it = &f->q[0];
        const char *r;
        if (it->is_dir) {
            if (store_mkdir(f->st, it->name, why, sizeof why) != 0) {
                f->log("fetch: directory \"%s\" failed: %s", it->name, why);
                f->stats.failed++;
                pop(f);
                continue;
            }
            f->log("fetch: directory \"%s\" present", it->name);
            UI(f, "  dir    %s/", it->name);
            f->stats.dirs_done++;
            record(f, it->name, 1, it->raw, it->raw_len);
            if ((r = announce(f, it)) != NULL)
                return r;
            pop(f);
            continue;
        }
        if (!f->file) {
            if (!(f->file = store_begin(f->st, it->name, why, sizeof why))) {
                f->log("fetch: \"%s\" failed: %s", it->name, why);
                f->stats.failed++;
                pop(f);
                continue;
            }
            f->next_block = 0;
            f->log("fetch: \"%s\": %lld bytes in %lu block(s)", it->name, (long long)it->size,
                   (unsigned long)it->nblocks);
        }
        /* Zero-length blocks (an empty file has one) carry no data to ask for. */
        while (f->next_block < it->nblocks && it->blocks[f->next_block].size == 0) {
            if (memcmp(it->blocks[f->next_block].hash, sha256_empty, 32) != 0) {
                fail_current(f, "empty block with a non-empty hash");
                it = NULL;
                break;
            }
            f->next_block++;
        }
        if (!it)
            continue;
        if (f->next_block < it->nblocks) {
            struct block *b = &it->blocks[f->next_block];
            pb_writer w;
            pb_writer_init(&w, 4096);
            f->req_id = f->next_id++;
            bep_encode_request(&w, f->req_id, f->folder, it->name, b->offset, b->size, b->hash, 32,
                               (int32_t)f->next_block);
            enum tls_result tr = f->send(f->ctx, BEP_REQUEST, &w);
            pb_writer_free(&w);
            if (tr != TLS_OK)
                return tls_result_name(tr);
            f->stats.requests++;
            f->waiting = 1;
            f->req_deadline = now + REQUEST_SECONDS * 1000;
            *wake = f->req_deadline;
            return NULL;
        }
        commit_hook(f, it->name);
        int replaced = 0;
        store_file *done = f->file;
        f->file = NULL;
        /* What was here when the download was planned must still be here:
         * the recorded version, or nothing. A change made here meanwhile
         * wins over the peer's version. */
        state_entry *e = f->state_path ? state_find(&f->state, it->name) : NULL;
        if (e && e->status != ST_PRESENT)
            e = NULL;
        why[0] = 0;
        int cr = store_commit(done, e ? still_recorded : NULL, e, &replaced, why, sizeof why);
        if (cr == 1) {
            if (e)
                e->status = ST_CHANGED;
            conflict(f, it->name, why);
            pop(f);
            continue;
        }
        if (cr != 0) {
            f->log("fetch: \"%s\" failed: %s", it->name, why);
            f->stats.failed++;
            pop(f);
            continue;
        }
        f->log("fetch: wrote \"%s\" (%lld bytes, %lu block(s), %s)", it->name, (long long)it->size,
               (unsigned long)it->nblocks, replaced ? "replaced the previous version" : "new");
        if (why[0])
            f->log("fetch: \"%s\": %s", it->name, why);
        /* A commit can take long (slow media, the test hook); the next
         * request's time limit must start now, not before it. */
        now = net_now();
        char sz[32];
        UI(f, "  file   %-30s %9s   SHA-256 verified, %lu block%s", it->name,
           fetch_size_text(it->size, sz, sizeof sz), (unsigned long)it->nblocks, it->nblocks == 1 ? "" : "s");
        f->stats.files_done++;
        if (replaced)
            f->stats.files_replaced++;
        /* Written under its final name first, then recorded (written to the
         * state file in batches) and announced: a failure in between can
         * only lose a record, never claim a file that is not here. */
        record(f, it->name, 0, it->raw, it->raw_len);
        if ((r = announce(f, it)) != NULL)
            return r;
        pop(f);
    }
    /* Nothing left to do for now: write what was recorded, when due. */
    flush_when_due(f);
    return NULL;
}
