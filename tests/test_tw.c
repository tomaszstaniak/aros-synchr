/* The two-way engine (src/tw.c) without a network: a fake peer records
 * every message the engine sends. Covers the rules of
 * docs/backlog/two-way-sync.md that a real peer cannot easily provoke:
 * restart without re-announcing, an edit that keeps size and date,
 * failed state writes, a crash between recording and announcing, a peer
 * that knows more of our index than we recorded, migration of a
 * receive-only state, rebuilding a damaged state, scans that cannot read
 * a directory, a folder without .stfolder, hashing every file where
 * dates cannot be trusted (FAT), scans that cannot verify a file.
 *
 *   test_tw WORK-DIR */
#include "../src/bep.h"
#include "../src/db.h"
#include "../src/finfo.h"
#include "../src/state.h"
#include "../src/store.h"
#include "../src/tw.h"

#include <openssl/evp.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

extern int tw_test_fail_saves;

static int failures, checks;
#define CHECK(c, ...)                                                                                   \
    do {                                                                                                \
        checks++;                                                                                       \
        if (!(c)) {                                                                                     \
            failures++;                                                                                 \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                 \
            printf(__VA_ARGS__);                                                                        \
            printf("\n");                                                                               \
        }                                                                                               \
    } while (0)

static int verbose;
static void tlog(const char *fmt, ...)
{
    if (!verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    printf("    | ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

/* ---- the fake peer ---- */

#define MAX_SENT 256
static struct { uint32_t type; unsigned char *data; size_t len; int done; } sent[MAX_SENT];
static int nsent;

static enum tls_result fake_send(void *ctx, uint32_t type, const pb_writer *body)
{
    (void)ctx;
    if (nsent < MAX_SENT) {
        sent[nsent].type = type;
        sent[nsent].data = malloc(body->len ? body->len : 1);
        memcpy(sent[nsent].data, body->data, body->len);
        sent[nsent].len = body->len;
        sent[nsent].done = 0;
        nsent++;
    }
    return TLS_OK;
}

static void clear_sent(void)
{
    for (int i = 0; i < nsent; i++)
        free(sent[i].data);
    nsent = 0;
}

/* Entries announced in what was sent so far (Index and IndexUpdate). */
typedef struct { char name[64]; int64_t seq; char vec[256]; int deleted; } ann;
static ann anns[512];
static int nanns, index_msgs, update_msgs;

static int on_ann(void *ctx, const bep_file *f)
{
    (void)ctx;
    finfo fi;
    if (nanns < 512 && finfo_decode(f->raw, &fi, NULL) == 0) {
        snprintf(anns[nanns].name, sizeof anns[nanns].name, "%s", fi.name);
        anns[nanns].seq = fi.sequence;
        anns[nanns].deleted = fi.deleted;
        vv_format(&fi.version, anns[nanns].vec, sizeof anns[nanns].vec);
        nanns++;
        finfo_free(&fi);
    }
    return 0;
}

static void collect(void)
{
    nanns = index_msgs = update_msgs = 0;
    for (int i = 0; i < nsent; i++) {
        if (sent[i].type != BEP_INDEX && sent[i].type != BEP_INDEX_UPDATE)
            continue;
        pb_slice folder;
        int64_t last, prev;
        bep_decode_index(sent[i].data, sent[i].len, &folder, &last, &prev, NULL, on_ann);
        if (sent[i].type == BEP_INDEX)
            index_msgs++;
        else
            update_msgs++;
    }
}

static const ann *announced(const char *name)
{
    for (int i = nanns - 1; i >= 0; i--)
        if (strcmp(anns[i].name, name) == 0)
            return &anns[i];
    return NULL;
}

/* The last Response sent: its error code (0 with data). */
static int last_response(size_t *data_len)
{
    for (int i = nsent - 1; i >= 0; i--) {
        if (sent[i].type != BEP_RESPONSE)
            continue;
        int32_t id;
        pb_slice data;
        uint32_t code;
        if (bep_decode_response(sent[i].data, sent[i].len, &id, &data, &code) != 0)
            return -1;
        *data_len = data.len;
        return (int)code;
    }
    return -1;
}

/* ---- the engine ---- */

static char work[600], root[700], state[800];
static unsigned char peer_id[DEVID_BYTES], my_id[DEVID_BYTES];
static store *st;

static int hash_policy = TW_HASH_AUTO; /* the host's dates: trusted */
static long rescan_seconds, slice_ms;

static tw *open_engine(const char **prep)
{
    char why[300];
    st = store_open(root, why, sizeof why);
    if (!st) {
        printf("store_open: %s\n", why);
        exit(2);
    }
    tw_config c = { "tw-test", root, state, peer_id, my_id, st, tlog, NULL, fake_send, NULL, rescan_seconds,
                    hash_policy, slice_ms, NULL, 0 };
    tw *t = tw_new(&c);
    tw_attach(t);
    *prep = tw_prepare(t);
    return t;
}

static void close_engine(tw *t)
{
    tw_free(t);
    store_close(st);
    st = NULL;
}

static void set_mtime(const char *name, time_t when)
{
    char p[900];
    snprintf(p, sizeof p, "%s/%s", root, name);
    struct timeval tv[2] = { { when, 0 }, { when, 0 } };
    utimes(p, tv);
}

/* Written with a date well outside the 2 s window (an ordinary file);
 * put_fresh_file leaves the date at "now". */
static void put_fresh_file(const char *name, const char *content)
{
    char p[900];
    snprintf(p, sizeof p, "%s/%s", root, name);
    FILE *f = fopen(p, "wb");
    fputs(content, f);
    fclose(f);
}

static time_t file_date;
static void put_file(const char *name, const char *content)
{
    put_fresh_file(name, content);
    set_mtime(name, file_date += 10);
}

static db load_state(void)
{
    db d;
    char why[300];
    db_binding b = { "tw-test", root, peer_id, my_id };
    if (db_load(state, &b, &d, why, sizeof why) != 0)
        memset(&d, 0, sizeof d);
    return d;
}

/* What the fake peer serves for a name (its latest offered content). */
static struct { char name[96]; char content[256]; } pcontent[64];
static int npcontent;

static void peer_content_set(const char *name, const char *content)
{
    int i;
    for (i = 0; i < npcontent && strcmp(pcontent[i].name, name) != 0; i++)
        ;
    if (i == npcontent) {
        if (npcontent == 64)
            return;
        npcontent++;
    }
    snprintf(pcontent[i].name, sizeof pcontent[i].name, "%s", name);
    snprintf(pcontent[i].content, sizeof pcontent[i].content, "%s", content);
}

static const char *peer_content(const char *name)
{
    for (int i = 0; i < npcontent; i++)
        if (strcmp(pcontent[i].name, name) == 0)
            return pcontent[i].content;
    return NULL;
}

/* A peer entry, offered as it would arrive in an Index. */
static void offer(tw *t, const char *name, const char *content, const char *vec, int64_t mod)
{
    peer_content_set(name, content);
    finfo f;
    memset(&f, 0, sizeof f);
    f.name = (char *)name;
    f.type = BEP_FILE;
    f.modified_s = mod;
    f.modified_by = vv_short_id(peer_id);
    f.sequence = 1;
    f.no_permissions = 1;
    vv_parse(vec, &f.version);
    size_t n = strlen(content);
    finfo_block b;
    memset(&b, 0, sizeof b);
    b.size = (int32_t)n;
    unsigned int len;
    EVP_Digest(content, n, b.hash, &len, EVP_sha256(), NULL);
    f.blocks = &b;
    f.nblocks = 1;
    f.size = (int64_t)n;
    f.block_size = FINFO_MIN_BLOCK;
    finfo_blocks_hash(&b, 1, f.blocks_hash);
    f.has_blocks_hash = 1;
    pb_writer w;
    pb_writer_init(&w, 1 << 20);
    finfo_encode(&w, &f);
    bep_file bf;
    bep_decode_fileinfo((pb_slice){ w.data, w.len }, &bf);
    tw_offer(t, &bf);
    pb_writer_free(&w);
    vv_free(&f.version);
}

/* The peer's deletion of name (a file, or a directory when dir is set). */
static void offer_deleted(tw *t, const char *name, int dir, const char *vec, int64_t mod)
{
    finfo f;
    memset(&f, 0, sizeof f);
    f.name = (char *)name;
    f.type = dir ? BEP_DIRECTORY : BEP_FILE;
    f.modified_s = mod;
    f.modified_by = vv_short_id(peer_id);
    f.sequence = 1;
    f.deleted = 1;
    f.no_permissions = 1;
    vv_parse(vec, &f.version);
    pb_writer w;
    pb_writer_init(&w, 1 << 20);
    finfo_encode(&w, &f);
    bep_file bf;
    bep_decode_fileinfo((pb_slice){ w.data, w.len }, &bf);
    tw_offer(t, &bf);
    pb_writer_free(&w);
    vv_free(&f.version);
}

static void request_block0(tw *t, const char *name, const char *expected_content)
{
    unsigned char h[32];
    unsigned int len;
    EVP_Digest(expected_content, strlen(expected_content), h, &len, EVP_sha256(), NULL);
    bep_request q;
    memset(&q, 0, sizeof q);
    q.id = 7;
    q.folder = (pb_slice){ (const unsigned char *)"tw-test", 7 };
    q.name = (pb_slice){ (const unsigned char *)name, strlen(name) };
    q.offset = 0;
    q.size = (int32_t)strlen(expected_content);
    q.hash = (pb_slice){ h, 32 };
    tw_request(t, &q);
}

static void reset_dirs(void)
{
    char cmd[2000];
    snprintf(cmd, sizeof cmd, "chmod -R u+rwx '%s' 2>/dev/null; rm -rf '%s' && mkdir -p '%s'", work, work, root);
    if (system(cmd) != 0)
        printf("reset failed\n");
}

static uint64_t index_of_state(void)
{
    db d = load_state();
    uint64_t id = d.index_id;
    db_free(&d);
    return id;
}

/* Everything the fake peer holds (the latest entry per name), replayed as
 * its Index on every connection. */
static struct { char name[64]; unsigned char *raw; size_t len; } held_by_peer[256];
static int nheld;

static void peer_keep(const bep_file *f)
{
    char name[64];
    snprintf(name, sizeof name, "%.*s", (int)f->name.len, (const char *)f->name.data);
    int i;
    for (i = 0; i < nheld && strcmp(held_by_peer[i].name, name) != 0; i++)
        ;
    if (i == nheld) {
        if (nheld == 256)
            return;
        nheld++;
    } else {
        free(held_by_peer[i].raw);
    }
    snprintf(held_by_peer[i].name, sizeof held_by_peer[i].name, "%s", name);
    held_by_peer[i].raw = malloc(f->raw.len ? f->raw.len : 1);
    memcpy(held_by_peer[i].raw, f->raw.data, f->raw.len);
    held_by_peer[i].len = f->raw.len;
}

static void peer_forget_all(void)
{
    for (int i = 0; i < nheld; i++)
        free(held_by_peer[i].raw);
    nheld = 0;
}

static int echo_one(void *ctx, const bep_file *f)
{
    tw_offer(ctx, f);
    peer_keep(f);
    return 0;
}

static void peer_replay(tw *t)
{
    for (int i = 0; i < nheld; i++) {
        bep_file bf;
        if (bep_decode_fileinfo((pb_slice){ held_by_peer[i].raw, held_by_peer[i].len }, &bf) == 0)
            tw_offer(t, &bf);
    }
}

/* What the fake peer knows of our index, as Syncthing tracks it: the
 * index ID and the highest sequence it received. */
static uint64_t peer_knows_index;
static int64_t peer_knows_seq;

/* A connection: the peer's ClusterConfig, its (empty) index, and what we
 * send in answer, collected. Returns tw_peer_cc's outcome. */
static const char *connect_peer(tw *t)
{
    clear_sent();
    const char *r = tw_peer_cc(t, peer_knows_index != 0, peer_knows_index, peer_knows_seq);
    if (!r) {
        peer_replay(t);
        r = tw_peer_index_complete(t);
    }
    collect();
    /* The peer takes what we announced and lists it as its own entry, as
     * Syncthing does once it has the file. */
    static tw *echo_to;
    echo_to = t;
    for (int i = 0; i < nsent; i++) {
        if (sent[i].type != BEP_INDEX && sent[i].type != BEP_INDEX_UPDATE)
            continue;
        pb_slice folder;
        int64_t last, prev;
        bep_decode_index(sent[i].data, sent[i].len, &folder, &last, &prev, echo_to, echo_one);
    }
    if (nanns > 0 || index_msgs > 0) {
        uint64_t idx;
        int64_t max;
        tw_cc_self(t, &idx, &max);
        if (index_msgs > 0)
            peer_knows_seq = 0;
        peer_knows_index = idx;
        for (int i = 0; i < nanns; i++)
            if (anns[i].seq > peer_knows_seq)
                peer_knows_seq = anns[i].seq;
    }
    return r;
}

static int echo_one(void *ctx, const bep_file *f);

/* Runs the engine as a session would until nothing is left to do: the fake
 * peer answers every block request from its content, and keeps what we
 * announce (as Syncthing does once it has it). */
static void pump(tw *t)
{
    int quiet = 0;
    for (int round = 0; round < 2000 && quiet < 3; round++) {
        net_ms wake;
        tw_step(t, net_now(), &wake);
        int found = 0;
        for (int i = 0; i < nsent; i++) {
            if (sent[i].type != BEP_REQUEST || sent[i].done)
                continue;
            sent[i].done = 1;
            bep_request q;
            if (bep_decode_request_full(sent[i].data, sent[i].len, &q) != 0)
                continue;
            found = 1;
            char name[96];
            snprintf(name, sizeof name, "%.*s", (int)q.name.len, (const char *)q.name.data);
            const char *c = peer_content(name);
            if (!c || q.offset + q.size > (int64_t)strlen(c))
                tw_response(t, q.id, (pb_slice){ NULL, 0 }, 2);
            else
                tw_response(t, q.id, (pb_slice){ (const unsigned char *)c + q.offset, (size_t)q.size }, 0);
        }
        quiet = found ? 0 : quiet + 1;
    }
    tw_flush(t);
    for (int i = 0; i < nsent; i++) {
        if ((sent[i].type != BEP_INDEX && sent[i].type != BEP_INDEX_UPDATE) || sent[i].done)
            continue;
        sent[i].done = 1;
        pb_slice folder;
        int64_t last, prev;
        bep_decode_index(sent[i].data, sent[i].len, &folder, &last, &prev, t, echo_one);
    }
    collect();
    for (int i = 0; i < nanns; i++)
        if (anns[i].seq > peer_knows_seq)
            peer_knows_seq = anns[i].seq;
}

static int file_is(const char *name, const char *content)
{
    char p[900], buf[512];
    snprintf(p, sizeof p, "%s/%s", root, name);
    FILE *f = fopen(p, "rb");
    if (!f)
        return content == NULL;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return content && strcmp(buf, content) == 0;
}

/* The conflict copy of name in its directory (first match), or "" */
static void find_conflict(const char *name, char *out, size_t n)
{
    char cmd[1200];
    const char *slash = strrchr(name, '/');
    char dir[300] = "";
    if (slash)
        snprintf(dir, sizeof dir, "/%.*s", (int)(slash - name), name);
    const char *base = slash ? slash + 1 : name;
    const char *dot = strrchr(base, '.');
    snprintf(cmd, sizeof cmd, "cd '%s%s' 2>/dev/null && ls | grep '^%.*s\\.sync-conflict-' | head -1", root, dir,
             (int)(dot ? dot - base : (long)strlen(base)), base);
    out[0] = 0;
    FILE *pp = popen(cmd, "r");
    if (pp) {
        if (fgets(out, (int)n, pp))
            out[strcspn(out, "\n")] = 0;
        pclose(pp);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: test_tw WORK-DIR\n");
        return 2;
    }
    verbose = getenv("TW_VERBOSE") != NULL;
    file_date = time(NULL) - 100000;
    snprintf(work, sizeof work, "%s/tw-work", argv[1]);
    snprintf(root, sizeof root, "%s/folder", work);
    snprintf(state, sizeof state, "%s/state.txt", work);
    for (int i = 0; i < DEVID_BYTES; i++) {
        peer_id[i] = (unsigned char)(0x80 + i);
        my_id[i] = (unsigned char)(0x10 + i);
    }
    const char *prep, *r;
    const ann *a;
    tw *t;
    size_t dl;
    char why[300];

    /* 1. First run: local files are published once the peer's index is
     * complete (their versions are settled against it first). */
    reset_dirs();
    put_file("a.txt", "hello from AROS\n");
    t = open_engine(&prep);
    CHECK(prep == NULL, "first prepare: %s", prep);
    clear_sent();
    tw_peer_cc(t, 0, 0, 0);
    CHECK(nsent == 0, "index sent before the peer's index was complete");
    tw_peer_index_complete(t);
    collect();
    a = announced("a.txt");
    CHECK(index_msgs == 1 && a && a->seq == 1, "first run: a.txt announced in an Index with sequence 1");
    for (int i = 0; i < nsent; i++) { /* the peer takes it */
        pb_slice folder;
        int64_t last, prev;
        if (sent[i].type == BEP_INDEX || sent[i].type == BEP_INDEX_UPDATE)
            bep_decode_index(sent[i].data, sent[i].len, &folder, &last, &prev, t, echo_one);
    }
    peer_knows_index = index_of_state();
    peer_knows_seq = 1;
    char first_vec[256] = "";
    if (a)
        snprintf(first_vec, sizeof first_vec, "%s", a->vec);
    CHECK(peer_knows_index != 0, "index ID stored");
    struct stat sm;
    char marker[900];
    snprintf(marker, sizeof marker, "%s/.stfolder", root);
    CHECK(stat(marker, &sm) == 0 && S_ISDIR(sm.st_mode), ".stfolder created as a directory");

    /* 2. Served block: the content we announced. */
    clear_sent();
    request_block0(t, "a.txt", "hello from AROS\n");
    CHECK(last_response(&dl) == 0 && dl == 16, "block served");
    close_engine(t);

    /* 3. Restart without changes: no state write, nothing announced. */
    t = open_engine(&prep);
    CHECK(prep == NULL && tw_get_stats(t)->state_saves == 0, "restart wrote the state (%ld writes)",
          tw_get_stats(t)->state_saves);
    connect_peer(t);
    CHECK(nanns == 0 && index_msgs == 0, "restart announced %d entries", nanns);
    CHECK(tw_settled(t) && tw_result(t) == NULL, "restart not in sync: %s", tw_result(t) ? tw_result(t) : "");
    close_engine(t);

    /* 4. A file dated within 2 s of its hashing is hashed again on the
     * next scan (FAT dates have 2 s steps): same content, no new version. */
    put_fresh_file("w.txt", "fresh\n");
    t = open_engine(&prep);
    connect_peer(t);
    int64_t wseq = announced("w.txt") ? announced("w.txt")->seq : -1;
    db dw = load_state();
    db_entry *ew = db_find(&dw, "w.txt");
    CHECK(ew && (ew->flags & DB_HASH_NEXT), "fresh file not marked for hashing on the next scan");
    db_free(&dw);
    close_engine(t);
    t = open_engine(&prep);
    connect_peer(t);
    CHECK(wseq == 2 && nanns == 0 && tw_get_stats(t)->local_changed == 0, "fresh file: no new version on rehash");
    close_engine(t);

    /* 5. Where dates are trusted, an edit that keeps size and modification
     * time is not seen by the scan; a Request for the block finds it, and
     * until the next scan the session cannot end in sync (UNCHECKED). The
     * same edit where every file is hashed: section 13. */
    struct stat before;
    char ap[900];
    snprintf(ap, sizeof ap, "%s/a.txt", root);
    stat(ap, &before);
    put_fresh_file("a.txt", "HELLO FROM AROS\n");
    set_mtime("a.txt", before.st_mtime);
    t = open_engine(&prep);
    connect_peer(t);
    CHECK(nanns == 0, "same size and date: the scan was not expected to see the edit");
    clear_sent();
    request_block0(t, "a.txt", "hello from AROS\n");
    CHECK(last_response(&dl) == 2, "changed block served instead of NO_SUCH_FILE");
    tw_flush(t);
    r = tw_result(t);
    CHECK(r && strcmp(r, "UNCHECKED") == 0, "refused Request, no scan since: %s", r ? r : "in sync");
    close_engine(t);
    t = open_engine(&prep);
    connect_peer(t);
    a = announced("a.txt");
    CHECK(a && a->seq == 3 && strcmp(a->vec, first_vec) != 0, "after the refusal the edit is a new version");
    clear_sent();
    request_block0(t, "a.txt", "HELLO FROM AROS\n");
    CHECK(last_response(&dl) == 0, "new content served");
    close_engine(t);

    /* 6. A failed state write holds the announcement back. The peer's
     * entry has our content and a newer version: recorded, then sent. */
    db d = load_state();
    db_entry *ea = db_find(&d, "a.txt");
    char newer[300] = "";
    if (ea) {
        vv nv;
        vv_copy(&nv, &ea->f.version);
        vv_update(&nv, vv_short_id(peer_id), 5);
        vv_format(&nv, newer, sizeof newer);
        vv_free(&nv);
    }
    db_free(&d);
    t = open_engine(&prep);
    connect_peer(t);
    tw_test_fail_saves = 1;
    clear_sent();
    offer(t, "a.txt", "HELLO FROM AROS\n", newer, 10);
    CHECK(tw_flush(t) != 0, "the write did not fail");
    collect();
    CHECK(nanns == 0 && tw_get_stats(t)->state_failures == 1, "announced although the state write failed");
    CHECK(!tw_idle(t), "idle with an unwritten record");
    CHECK(tw_flush(t) == 0, "second write");
    collect();
    a = announced("a.txt");
    CHECK(a && a->seq == 4 && strcmp(a->vec, newer) == 0, "after the write: the peer's version recorded and sent");
    if (a)
        peer_knows_seq = a->seq;
    close_engine(t);

    /* 7. A crash after recording, before announcing: the next run sends
     * the same sequence with the same version, no new number. */
    put_file("b.txt", "second file\n");
    t = open_engine(&prep); /* records b.txt and writes it */
    close_engine(t);        /* "crash": never connected */
    db d2 = load_state();
    db_entry *eb = db_find(&d2, "b.txt");
    char bvec[256] = "";
    int64_t bseq = eb ? eb->f.sequence : -1;
    if (eb)
        vv_format(&eb->f.version, bvec, sizeof bvec);
    db_free(&d2);
    t = open_engine(&prep);
    connect_peer(t);
    a = announced("b.txt");
    CHECK(bseq == 5 && a && a->seq == bseq && strcmp(a->vec, bvec) == 0 && nanns == 1,
          "after the crash: b.txt sent with its recorded sequence %lld", (long long)bseq);
    close_engine(t);

    /* 8. The peer knows more of our index than is recorded (a restored or
     * older state): a new index ID, never a reused number. */
    uint64_t old_index = peer_knows_index;
    peer_knows_seq = 99;
    t = open_engine(&prep);
    r = connect_peer(t);
    CHECK(r && strcmp(r, "INDEX_ID_RENEWED") == 0 && nsent == 0, "peer ahead of us: %s", r ? r : "accepted");
    close_engine(t);
    CHECK(index_of_state() != old_index, "index ID renewed in the state");
    t = open_engine(&prep);
    connect_peer(t);
    CHECK(index_msgs == 1 && announced("a.txt") && announced("b.txt"), "full Index under the new index ID");
    CHECK(peer_knows_index == index_of_state(), "peer follows the new index ID");
    close_engine(t);

    /* 9. A deletion here under a directory that could be read: published
     * as a deleted entry with a newer version. A record under a directory
     * that cannot be read: not a deletion at all (UNCHECKED). */
    char sub[900];
    snprintf(sub, sizeof sub, "%s/sub", root);
    mkdir(sub, 0755);
    put_file("sub/c.txt", "in a subdirectory\n");
    t = open_engine(&prep);
    connect_peer(t);
    close_engine(t);
    char bp[900];
    snprintf(bp, sizeof bp, "%s/b.txt", root);
    remove(bp);
    chmod(sub, 0);
    t = open_engine(&prep);
    connect_peer(t);
    a = announced("b.txt");
    CHECK(nanns == 1 && a && a->deleted && strcmp(a->vec, bvec) != 0,
          "deletion here: %d announcement(s), b.txt %s", nanns, a ? (a->deleted ? "deleted" : "live") : "not sent");
    db d4 = load_state();
    CHECK(db_find(&d4, "sub/c.txt") && !db_find(&d4, "sub/c.txt")->f.deleted,
          "a record under an unreadable directory was changed");
    db_free(&d4);
    r = tw_result(t);
    CHECK(r && strcmp(r, "UNCHECKED") == 0, "result with an unreadable directory: %s", r ? r : "in sync");
    close_engine(t);
    chmod(sub, 0755);
    t = open_engine(&prep);
    connect_peer(t);
    r = tw_result(t);
    CHECK(nanns == 0 && r == NULL, "after the deletion and a readable directory: %d announced, %s", nanns,
          r ? r : "in sync");
    close_engine(t);

    /* 10. No .stfolder with a state present: the folder is unavailable;
     * nothing is scanned or written. */
    char aside[900];
    snprintf(aside, sizeof aside, "%s/stfolder-aside", work);
    rename(marker, aside);
    struct stat s_before, s_after;
    stat(state, &s_before);
    t = open_engine(&prep);
    CHECK(prep && strcmp(prep, "FOLDER_UNAVAILABLE") == 0, "missing .stfolder: %s", prep ? prep : "accepted");
    close_engine(t);
    stat(state, &s_after);
    CHECK(s_before.st_size == s_after.st_size && s_before.st_mtime == s_after.st_mtime, "state written anyway");
    rename(aside, marker);

    /* 11. A damaged state: rebuilt, nothing deleted; a file equal to the
     * peer's entry takes the peer's version, a different one keeps a
     * version of ours (concurrent: the conflict rule). */
    peer_forget_all();
    put_file("sub/d.txt", "ours, different\n");
    FILE *sf = fopen(state, "ab");
    fputs("garbage\n", sf);
    fclose(sf);
    t = open_engine(&prep);
    CHECK(prep == NULL, "rebuild prepare: %s", prep);
    char rej[900];
    snprintf(rej, sizeof rej, "%s.rejected", state);
    CHECK(access(rej, F_OK) == 0, "damaged state not kept as .rejected");
    clear_sent();
    tw_peer_cc(t, 1, peer_knows_index, peer_knows_seq);
    CHECK(nsent == 0, "rebuilt index sent before the peer's index");
    offer(t, "a.txt", "HELLO FROM AROS\n", "5:5,77:1", 10); /* same content as here */
    /* Different content, the peer's older: ours wins, nothing to do here
     * (the peer makes the conflict copy on its side). */
    offer(t, "sub/c.txt", "the peer's other text\n", "77:2", 10);
    /* Different content, the peer's newer: the peer wins; ours becomes a
     * conflict copy (published), the peer's version is downloaded. */
    offer(t, "sub/d.txt", "the peer's newer text\n", "77:3", 2000000000);
    tw_peer_index_complete(t);
    collect();
    a = announced("a.txt");
    CHECK(index_msgs == 1 && a && strcmp(a->vec, "5:5,77:1") == 0, "equal content did not take the peer's version (%s)",
          a ? a->vec : "-");
    a = announced("sub/c.txt");
    CHECK(a && strcmp(a->vec, "77:2") != 0, "different content took the peer's version");
    pump(t);
    char cc[300];
    find_conflict("sub/d.txt", cc, sizeof cc);
    char ccname[400];
    snprintf(ccname, sizeof ccname, "sub/%s", cc);
    CHECK(file_is("sub/d.txt", "the peer's newer text\n") && cc[0] && file_is(ccname, "ours, different\n"),
          "conflict: d.txt %s, conflict copy \"%s\"", file_is("sub/d.txt", "the peer's newer text\n") ? "the peer's" : "not the peer's", cc);
    CHECK(tw_get_stats(t)->conflicts == 1 && tw_get_stats(t)->held == 0 && announced(ccname),
          "conflict: %ld copies, %ld held, copy %s", tw_get_stats(t)->conflicts, tw_get_stats(t)->held,
          announced(ccname) ? "announced" : "not announced");
    CHECK(tw_result(t) == NULL, "after the conflict: %s", tw_result(t) ? tw_result(t) : "");
    close_engine(t);

    /* 12. Migration of a receive-only state (format 1): unchanged files
     * keep the peer's version, a changed one becomes our change on top of
     * it, a missing one is dropped (downloaded again, never deleted). */
    reset_dirs();
    put_file("same.txt", "unchanged\n");
    put_file("edited.txt", "edited here\n");
    state_list v1;
    memset(&v1, 0, sizeof v1);
    const char *names[3] = { "same.txt", "edited.txt", "gone.txt" };
    const char *contents[3] = { "unchanged\n", "as received\n", "was here\n" };
    for (int i = 0; i < 3; i++) {
        finfo f;
        memset(&f, 0, sizeof f);
        f.name = (char *)names[i];
        f.type = BEP_FILE;
        f.sequence = 9;
        vv_parse("77:3", &f.version);
        finfo_block b;
        memset(&b, 0, sizeof b);
        b.size = (int32_t)strlen(contents[i]);
        unsigned int len;
        EVP_Digest(contents[i], strlen(contents[i]), b.hash, &len, EVP_sha256(), NULL);
        f.blocks = &b;
        f.nblocks = 1;
        f.size = b.size;
        pb_writer w;
        pb_writer_init(&w, 1 << 20);
        finfo_encode(&w, &f);
        state_put(&v1, names[i], 0, w.data, w.len);
        pb_writer_free(&w);
        vv_free(&f.version);
    }
    CHECK(state_save(state, "tw-test", root, peer_id, &v1, why, sizeof why) == 0, "v1 save: %s", why);
    state_list_free(&v1);
    peer_knows_index = 0;
    peer_knows_seq = 0;
    peer_forget_all();
    t = open_engine(&prep);
    CHECK(prep == NULL, "migration prepare: %s", prep);
    connect_peer(t);
    a = announced("same.txt");
    CHECK(a && strcmp(a->vec, "77:3") == 0, "unchanged file kept the peer's version (%s)", a ? a->vec : "-");
    a = announced("edited.txt");
    vv base, ed;
    vv_parse("77:3", &base);
    int greater = 0;
    if (a && vv_parse(a->vec, &ed) == 0) {
        greater = vv_compare(&ed, &base) == VV_GREATER;
        vv_free(&ed);
    }
    vv_free(&base);
    CHECK(greater, "edited file is not a newer version of the peer's (%s)", a ? a->vec : "-");
    CHECK(!announced("gone.txt"), "missing file announced (as present or deleted)");
    close_engine(t);
    db d3 = load_state();
    CHECK(d3.n == 2 && !db_find(&d3, "gone.txt"), "migrated state holds %lu entries", (unsigned long)d3.n);
    db_free(&d3);

    /* 13. Every file hashed at each scan (FAT: fat-handler leaves the date
     * of a written file unchanged). An edit keeping size and date is found
     * by the scan alone, with no Request from the peer; unchanged content
     * is neither a new version nor a state write. */
    reset_dirs();
    peer_forget_all();
    peer_knows_index = 0;
    peer_knows_seq = 0;
    hash_policy = TW_HASH_ALWAYS;
    put_file("x.txt", "abcdef\n");
    t = open_engine(&prep);
    CHECK(prep == NULL, "hash-always prepare: %s", prep);
    connect_peer(t);
    a = announced("x.txt");
    int64_t xseq = a ? a->seq : -1;
    char xvec[256] = "";
    if (a)
        snprintf(xvec, sizeof xvec, "%s", a->vec);
    CHECK(a && tw_result(t) == NULL, "hash-always first run: %s", tw_result(t) ? tw_result(t) : "x.txt not sent");
    close_engine(t);
    t = open_engine(&prep);
    connect_peer(t);
    CHECK(nanns == 0 && tw_get_stats(t)->state_saves == 0 && tw_get_stats(t)->local_changed == 0,
          "hash-always restart: %d announced, %ld state write(s)", nanns, tw_get_stats(t)->state_saves);
    CHECK(tw_result(t) == NULL, "hash-always restart: %s", tw_result(t) ? tw_result(t) : "");
    close_engine(t);
    char xp[900];
    snprintf(xp, sizeof xp, "%s/x.txt", root);
    struct stat xs;
    stat(xp, &xs);
    put_fresh_file("x.txt", "ABCDEF\n");
    set_mtime("x.txt", xs.st_mtime);
    t = open_engine(&prep);
    connect_peer(t);
    a = announced("x.txt");
    CHECK(a && a->seq > xseq && strcmp(a->vec, xvec) != 0 && tw_get_stats(t)->local_changed == 1
              && tw_get_stats(t)->refused_requests == 0,
          "same size and date, no Request: the scan did not find the edit");
    CHECK(tw_result(t) == NULL, "after the edit: %s", tw_result(t) ? tw_result(t) : "");
    clear_sent();
    request_block0(t, "x.txt", "ABCDEF\n");
    CHECK(last_response(&dl) == 0, "edited content not served");
    close_engine(t);
    t = open_engine(&prep);
    connect_peer(t);
    CHECK(nanns == 0 && tw_get_stats(t)->state_saves == 0, "restart after the edit: %d announced", nanns);
    close_engine(t);

    /* 14. Content that cannot be verified blocks an in-sync result: an
     * existing file that cannot be read keeps its record and is not sent;
     * a new one is not recorded; both say UNCHECKED, then clear once
     * readable. */
    chmod(xp, 0);
    t = open_engine(&prep);
    connect_peer(t);
    r = tw_result(t);
    CHECK(nanns == 0 && r && strcmp(r, "UNCHECKED") == 0 && tw_get_stats(t)->unchecked == 1,
          "unreadable existing file: %d announced, %s", nanns, r ? r : "in sync");
    close_engine(t);
    db d5 = load_state();
    CHECK(db_find(&d5, "x.txt") && !db_find(&d5, "x.txt")->f.deleted, "unreadable file lost its record");
    db_free(&d5);
    chmod(xp, 0644);
    put_file("n.txt", "new, unreadable\n");
    char np[900];
    snprintf(np, sizeof np, "%s/n.txt", root);
    chmod(np, 0);
    t = open_engine(&prep);
    connect_peer(t);
    r = tw_result(t);
    CHECK(!announced("n.txt") && r && strcmp(r, "UNCHECKED") == 0, "unreadable new file: %s",
          r ? r : "in sync");
    close_engine(t);
    db d6 = load_state();
    CHECK(!db_find(&d6, "n.txt"), "unreadable new file recorded");
    db_free(&d6);
    chmod(np, 0644);
    t = open_engine(&prep);
    connect_peer(t);
    CHECK(announced("n.txt") && tw_result(t) == NULL, "readable again: %s",
          tw_result(t) ? tw_result(t) : "n.txt not sent");
    close_engine(t);

    /* 15. A periodic scan that cannot read a file, after the folder was in
     * sync: no longer in sync (UNCHECKED); the next good scan clears it. */
    rescan_seconds = 1;
    t = open_engine(&prep);
    connect_peer(t);
    CHECK(tw_result(t) == NULL, "before the periodic scan: %s", tw_result(t) ? tw_result(t) : "");
    chmod(xp, 0);
    net_ms wake;
    r = tw_step(t, net_now() + 10000, &wake);
    const char *r2 = tw_result(t);
    CHECK(r == NULL && r2 && strcmp(r2, "UNCHECKED") == 0, "periodic scan of an unreadable file: %s",
          r2 ? r2 : "in sync");
    chmod(xp, 0644);
    r = tw_step(t, net_now() + 20000, &wake);
    tw_flush(t);
    CHECK(r == NULL && tw_result(t) == NULL, "the next periodic scan: %s", tw_result(t) ? tw_result(t) : "");
    close_engine(t);
    rescan_seconds = 0;
    hash_policy = TW_HASH_AUTO;

    /* 16. Stage C, a fresh folder: f1, f2, f3, f4, dir/in.txt, keep/x.txt
     * recorded and in sync with the peer. */
    reset_dirs();
    peer_forget_all();
    npcontent = 0;
    peer_knows_index = 0;
    peer_knows_seq = 0;
    put_file("f1.txt", "one\n");
    put_file("f2.txt", "two\n");
    put_file("f3.txt", "three\n");
    put_file("f4.txt", "four\n");
    char dpath[900];
    snprintf(dpath, sizeof dpath, "%s/dir", root);
    mkdir(dpath, 0755);
    put_file("dir/in.txt", "inside\n");
    snprintf(dpath, sizeof dpath, "%s/keep", root);
    mkdir(dpath, 0755);
    put_file("keep/x.txt", "x\n");
    t = open_engine(&prep);
    connect_peer(t);
    pump(t);
    CHECK(tw_result(t) == NULL, "stage C start: %s", tw_result(t) ? tw_result(t) : "");
    close_engine(t);
    db dc = load_state();
    char v2[256] = "", v3[256] = "", v4[256] = "", vdir[256] = "", vin[256] = "", vkeep[256] = "", vx[256] = "";
    vv_format(&db_find(&dc, "f2.txt")->f.version, v2, sizeof v2);
    vv_format(&db_find(&dc, "f3.txt")->f.version, v3, sizeof v3);
    vv_format(&db_find(&dc, "f4.txt")->f.version, v4, sizeof v4);
    vv_format(&db_find(&dc, "dir")->f.version, vdir, sizeof vdir);
    vv_format(&db_find(&dc, "dir/in.txt")->f.version, vin, sizeof vin);
    vv_format(&db_find(&dc, "keep")->f.version, vkeep, sizeof vkeep);
    vv_format(&db_find(&dc, "keep/x.txt")->f.version, vx, sizeof vx);
    db_free(&dc);
    uint64_t pid = vv_short_id(peer_id);
    char pv[512];

    /* 17. Deleted here: f1 and dir with its file: three deleted entries,
     * each a newer version; a restart announces nothing more. */
    remove(root[0] ? (snprintf(bp, sizeof bp, "%s/f1.txt", root), bp) : bp);
    snprintf(bp, sizeof bp, "%s/dir/in.txt", root);
    remove(bp);
    snprintf(bp, sizeof bp, "%s/dir", root);
    rmdir(bp);
    t = open_engine(&prep);
    connect_peer(t);
    a = announced("f1.txt");
    const ann *ad = announced("dir"), *ai = announced("dir/in.txt");
    CHECK(nanns == 3 && a && a->deleted && ad && ad->deleted && ai && ai->deleted && tw_get_stats(t)->local_deleted == 3,
          "deleted here: %d announced, %ld deletions", nanns, tw_get_stats(t)->local_deleted);
    CHECK(tw_result(t) == NULL, "after deletions here: %s", tw_result(t) ? tw_result(t) : "");
    close_engine(t);
    t = open_engine(&prep);
    connect_peer(t);
    CHECK(nanns == 0 && tw_get_stats(t)->state_saves == 0, "restart after deletions: %d announced", nanns);
    close_engine(t);

    /* 18. Deleted on the peer, unchanged here: f2 is removed and the peer's
     * deleted version recorded. */
    t = open_engine(&prep);
    connect_peer(t);
    snprintf(pv, sizeof pv, "%s,%llu:%llu", v2, (unsigned long long)pid, (unsigned long long)time(NULL));
    offer_deleted(t, "f2.txt", 0, pv, time(NULL));
    pump(t);
    a = announced("f2.txt");
    CHECK(file_is("f2.txt", NULL) && tw_get_stats(t)->deleted_here == 1 && a && a->deleted && strcmp(a->vec, pv) == 0,
          "the peer's deletion: file %s, %ld deleted here, recorded %s", file_is("f2.txt", NULL) ? "gone" : "still here",
          tw_get_stats(t)->deleted_here, a ? a->vec : "-");
    CHECK(tw_result(t) == NULL, "after the peer's deletion: %s", tw_result(t) ? tw_result(t) : "");
    close_engine(t);

    /* 19. Deleted on the peer, edited here (not scanned yet), the edit
     * later: ours wins, the file stays and is published; the deletion is
     * not applied. */
    t = open_engine(&prep);
    connect_peer(t);
    put_file("f3.txt", "three, edited here after the deletion there\n");
    set_mtime("f3.txt", time(NULL));
    snprintf(pv, sizeof pv, "%s,%llu:%llu", v3, (unsigned long long)pid, (unsigned long long)(time(NULL) - 1000));
    offer_deleted(t, "f3.txt", 0, pv, time(NULL) - 1000);
    pump(t);
    a = announced("f3.txt");
    CHECK(file_is("f3.txt", "three, edited here after the deletion there\n") && a && !a->deleted
              && tw_get_stats(t)->deleted_here == 0 && tw_get_stats(t)->conflicts == 0,
          "edit here wins over an earlier deletion: %s, announced %s", file_is("f3.txt", "three, edited here after the deletion there\n") ? "kept" : "lost",
          a ? (a->deleted ? "deleted" : "live") : "nothing");
    close_engine(t);

    /* 20. Deleted on the peer later than the edit here: the deletion wins,
     * ours is kept as a conflict copy (published), the name is deleted. */
    t = open_engine(&prep);
    connect_peer(t);
    put_file("f4.txt", "four, edited here before the deletion there\n");
    snprintf(pv, sizeof pv, "%s,%llu:%llu", v4, (unsigned long long)pid, (unsigned long long)(time(NULL) + 1000));
    offer_deleted(t, "f4.txt", 0, pv, time(NULL) + 1000);
    pump(t);
    find_conflict("f4.txt", cc, sizeof cc);
    a = announced("f4.txt");
    CHECK(file_is("f4.txt", NULL) && cc[0] && file_is(cc, "four, edited here before the deletion there\n")
              && tw_get_stats(t)->conflicts == 1 && a && a->deleted && announced(cc),
          "deletion wins over an earlier edit: name %s, copy \"%s\", %ld conflicts",
          file_is("f4.txt", NULL) ? "gone" : "still here", cc, tw_get_stats(t)->conflicts);
    CHECK(tw_result(t) == NULL, "after the edit/delete conflict: %s", tw_result(t) ? tw_result(t) : "");
    close_engine(t);

    /* 21. A directory deleted on the peer that holds a file only we have:
     * the directory stays and is published again; its known file is
     * deleted. */
    put_file("keep/new-here.txt", "only here\n");
    t = open_engine(&prep);
    connect_peer(t);
    pump(t);
    snprintf(pv, sizeof pv, "%s,%llu:%llu", vx, (unsigned long long)pid, (unsigned long long)time(NULL));
    offer_deleted(t, "keep/x.txt", 0, pv, time(NULL));
    snprintf(pv, sizeof pv, "%s,%llu:%llu", vkeep, (unsigned long long)pid, (unsigned long long)time(NULL));
    offer_deleted(t, "keep", 1, pv, time(NULL));
    pump(t);
    a = announced("keep");
    struct stat kst;
    snprintf(bp, sizeof bp, "%s/keep", root);
    CHECK(stat(bp, &kst) == 0 && file_is("keep/new-here.txt", "only here\n") && file_is("keep/x.txt", NULL)
              && tw_get_stats(t)->dirs_kept == 1 && a && !a->deleted,
          "non-empty directory deleted on the peer: %ld kept, keep %s", tw_get_stats(t)->dirs_kept,
          a ? (a->deleted ? "announced deleted" : "republished") : "not announced");
    close_engine(t);

    /* 22. Edited on both sides, ours later: ours wins, nothing changes
     * here; no conflict copy here (the peer makes its own). */
    put_file("both.txt", "both, original\n");
    t = open_engine(&prep);
    connect_peer(t);
    pump(t);
    close_engine(t);
    db db2 = load_state();
    char vb[256] = "";
    vv_format(&db_find(&db2, "both.txt")->f.version, vb, sizeof vb);
    db_free(&db2);
    put_file("both.txt", "both, edited here later\n");
    set_mtime("both.txt", time(NULL) + 50);
    t = open_engine(&prep);
    connect_peer(t);
    snprintf(pv, sizeof pv, "%s,%llu:%llu", vb, (unsigned long long)pid, (unsigned long long)time(NULL));
    offer(t, "both.txt", "both, edited on the peer earlier\n", pv, time(NULL));
    pump(t);
    find_conflict("both.txt", cc, sizeof cc);
    CHECK(file_is("both.txt", "both, edited here later\n") && !cc[0] && tw_get_stats(t)->conflicts == 0,
          "concurrent edit, ours later: %s, copy \"%s\"", file_is("both.txt", "both, edited here later\n") ? "ours kept" : "replaced", cc);
    close_engine(t);

    /* 23. Interrupted earlier: a temporary download and a moved-aside copy
     * of a recorded file (whose name is empty) are found at the start: the
     * temporary file is deleted, the copy put back; no deletion of it is
     * published. */
    put_file("y.txt", "y content\n");
    t = open_engine(&prep);
    connect_peer(t);
    pump(t);
    close_engine(t);
    char yp[900], yo[900], tp[900];
    snprintf(yp, sizeof yp, "%s/y.txt", root);
    snprintf(yo, sizeof yo, "%s/.stpoc-y.txt.old", root);
    snprintf(tp, sizeof tp, "%s/.stpoc-z.txt.tmp", root);
    rename(yp, yo);
    FILE *tf = fopen(tp, "wb");
    fputs("half a download", tf);
    fclose(tf);
    t = open_engine(&prep);
    connect_peer(t);
    CHECK(file_is("y.txt", "y content\n") && access(yo, F_OK) != 0 && access(tp, F_OK) != 0 && !announced("y.txt")
              && tw_get_stats(t)->leftovers_removed == 1 && tw_get_stats(t)->leftovers_restored == 1,
          "after an interruption: y %s, .old %s, .tmp %s, y announced %s", file_is("y.txt", "y content\n") ? "back" : "missing",
          access(yo, F_OK) == 0 ? "left" : "gone", access(tp, F_OK) == 0 ? "left" : "gone", announced("y.txt") ? "yes" : "no");
    CHECK(tw_result(t) == NULL, "after the clean-up: %s", tw_result(t) ? tw_result(t) : "");
    close_engine(t);

    /* 24. State lost, a file missing: a rebuilt state never publishes a
     * deletion. */
    remove(state);
    snprintf(bp, sizeof bp, "%s/y.txt", root);
    remove(bp);
    t = open_engine(&prep);
    connect_peer(t);
    pump(t);
    a = announced("y.txt");
    CHECK(!(a && a->deleted) && tw_get_stats(t)->local_deleted == 0, "rebuilt state published a deletion");
    close_engine(t);

    /* 25. A scan done in steps while connected: the same findings as a
     * scan in one call; nothing is requested from the peer while it runs;
     * the peer's entries that arrive meanwhile are decided against the
     * folder as the scan leaves it (an edit here not yet looked at becomes
     * a conflict, not an overwritten file). */
    reset_dirs();
    peer_forget_all();
    npcontent = 0;
    peer_knows_index = 0;
    peer_knows_seq = 0;
    put_file("s1.txt", "one\n");
    put_file("s2.txt", "two\n");
    put_file("s3.txt", "three\n");
    put_file("s4.txt", "four\n");
    put_file("s5.txt", "five\n");
    snprintf(dpath, sizeof dpath, "%s/sd", root);
    mkdir(dpath, 0755);
    put_file("sd/in.txt", "inside\n");
    t = open_engine(&prep);
    connect_peer(t);
    pump(t);
    CHECK(tw_result(t) == NULL, "sliced scan start: %s", tw_result(t) ? tw_result(t) : "");
    close_engine(t);
    db ds = load_state();
    char w4[256] = "", w5[256] = "";
    vv_format(&db_find(&ds, "s4.txt")->f.version, w4, sizeof w4);
    vv_format(&db_find(&ds, "s5.txt")->f.version, w5, sizeof w5);
    db_free(&ds);
    rescan_seconds = 1;
    slice_ms = -1;
    t = open_engine(&prep);
    connect_peer(t);
    pump(t);
    put_file("s2.txt", "two, edited here\n");
    put_file("snew.txt", "new here\n");
    put_file("s5.txt", "five, edited here\n");
    snprintf(bp, sizeof bp, "%s/s3.txt", root);
    remove(bp);
    clear_sent();
    r = tw_step(t, net_now() + 10000, &wake);
    CHECK(r == NULL && tw_scanning(t), "the scan did not stay open after one step");
    offer(t, "sp.txt", "from the peer\n", "77:1", 10);
    snprintf(pv, sizeof pv, "%s,%llu:%llu", w4, (unsigned long long)pid, (unsigned long long)time(NULL));
    offer(t, "s4.txt", "four, newer on the peer\n", pv, time(NULL));
    snprintf(pv, sizeof pv, "%s,%llu:%llu", w5, (unsigned long long)pid, (unsigned long long)time(NULL));
    offer(t, "s5.txt", "five, edited on the peer\n", pv, time(NULL));
    int steps = 1, asked = 0;
    while (tw_scanning(t) && steps < 1000) {
        r = tw_step(t, net_now() + 10000, &wake);
        steps++;
        for (int i = 0; i < nsent; i++)
            if (sent[i].type == BEP_REQUEST)
                asked++;
    }
    CHECK(steps > 5 && steps < 1000 && asked == 0 && r == NULL, "sliced scan: %d step(s), %d request(s) during it",
          steps, asked);
    CHECK(file_is("s4.txt", "four\n") && file_is("sp.txt", NULL), "the folder changed during the scan");
    pump(t);
    a = announced("s2.txt");
    const ann *an = announced("snew.txt"), *a3 = announced("s3.txt");
    CHECK(a && !a->deleted && an && a3 && a3->deleted, "sliced scan findings: s2 %s, snew %s, s3 %s",
          a ? "sent" : "missing", an ? "sent" : "missing", a3 && a3->deleted ? "deleted" : "not deleted");
    CHECK(file_is("sp.txt", "from the peer\n") && file_is("s4.txt", "four, newer on the peer\n"),
          "entries offered during the scan were not taken afterwards");
    char sc[300];
    find_conflict("s5.txt", sc, sizeof sc);
    CHECK(tw_get_stats(t)->conflicts == 1 && sc[0]
              && (file_is("s5.txt", "five, edited here\n") || file_is(sc, "five, edited here\n"))
              && (file_is("s5.txt", "five, edited on the peer\n") || file_is(sc, "five, edited on the peer\n")),
          "edit here against an entry offered during the scan: %ld conflict(s), copy \"%s\"",
          tw_get_stats(t)->conflicts, sc);
    CHECK(tw_result(t) == NULL, "after the sliced scan: %s", tw_result(t) ? tw_result(t) : "");
    close_engine(t);
    rescan_seconds = 0;
    slice_ms = 0;

    /* ---- the folder outlives the connection (tw_attach, tw_detach) ---- */
    clear_sent();
    peer_forget_all();
    reset_dirs();
    peer_knows_index = 0;
    peer_knows_seq = 0;
    put_file("k1.txt", "one, here from the start\n");
    rescan_seconds = 1;
    t = open_engine(&prep);
    CHECK(prep == NULL && tw_attached(t), "persistent: prepare: %s", prep ? prep : "");
    connect_peer(t);
    pump(t);
    CHECK(announced("k1.txt") != NULL && tw_result(t) == NULL, "persistent: first connection: %s",
          tw_result(t) ? tw_result(t) : "");
    /* A download is under way when the connection ends. */
    offer(t, "kp.txt", "from the peer, asked for twice\n", "77:1", 10);
    r = tw_step(t, net_now(), &wake);
    CHECK(r == NULL && !tw_idle(t), "persistent: no download in progress before the detach");
    tw_detach(t);
    char cmd0[2000];
    snprintf(cmd0, sizeof cmd0, "ls -a '%s' | grep -c '^\\.stpoc-' > '%s/count.txt'", root, work);
    int left = -1;
    if (system(cmd0) >= 0) { /* grep -c exits 1 for a count of 0 */
        char cp[900];
        snprintf(cp, sizeof cp, "%s/count.txt", work);
        FILE *cf = fopen(cp, "r");
        if (cf) {
            if (fscanf(cf, "%d", &left) != 1)
                left = -1;
            fclose(cf);
        }
    }
    CHECK(!tw_attached(t) && tw_idle(t) && left == 0, "persistent: after the detach: attached %d, idle %d, %d "
          "temporary file(s) left", tw_attached(t), tw_idle(t), left);
    /* Without a connection: the folder is still read, the state written,
     * nothing sent. */
    put_file("k2.txt", "two, made without a connection\n");
    clear_sent();
    net_ms until = net_now() + 5000;
    while (net_now() < until && tw_get_stats(t)->local_new < 2) {
        r = tw_step(t, net_now(), &wake);
        if (r)
            break;
        usleep(50000);
    }
    tw_flush(t);
    db dk = load_state();
    CHECK(r == NULL && db_find(&dk, "k2.txt") != NULL && nsent == 0,
          "persistent: detached scan: %s, k2 %s in the state file, %d message(s) sent", r ? r : "ok",
          db_find(&dk, "k2.txt") ? "is" : "is not", nsent);
    db_free(&dk);
    /* The next connection: only what the peer lacks goes out, the
     * counters go on, the session's own count starts at the detach. */
    tw_attach(t);
    connect_peer(t);
    tw_stats ks;
    tw_get_session_stats(t, &ks);
    CHECK(index_msgs == 0 && announced("k2.txt") != NULL && announced("k1.txt") == NULL,
          "persistent: second connection sent %d Index message(s), k2 %s, k1 %s", index_msgs,
          announced("k2.txt") ? "announced" : "missing", announced("k1.txt") ? "announced again" : "not repeated");
    CHECK(tw_get_stats(t)->local_new == 2 && ks.local_new == 1, "persistent: counters: %ld new in all, %ld since "
          "the detach", tw_get_stats(t)->local_new, ks.local_new);
    offer(t, "kp.txt", "from the peer, asked for twice\n", "77:1", 10);
    pump(t);
    CHECK(file_is("kp.txt", "from the peer, asked for twice\n") && tw_result(t) == NULL,
          "persistent: the download given up at the detach was not done by the next connection: %s",
          tw_result(t) ? tw_result(t) : "");
    close_engine(t);
    rescan_seconds = 0;

    clear_sent();
    peer_forget_all();
    reset_dirs();
    char cmd[1000];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", work);
    if (system(cmd) != 0)
        printf("cleanup failed\n");
    printf("test_tw: %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
