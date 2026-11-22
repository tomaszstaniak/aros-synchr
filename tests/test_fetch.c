/* fetch: a newer entry for a name must cancel earlier work for it, also
 * when the newer entry needs no download (review 2026-10-05). No network:
 * messages "sent" are only recorded. */
#include "../src/fetch.h"
#include "../src/state.h"

extern int fetch_test_fail_state_writes; /* fetch.c built with FETCH_TEST_HOOKS */

#include <openssl/evp.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int requests, announces;
static int32_t last_request_id;

static void quiet(const char *fmt, ...) { (void)fmt; }

static enum tls_result fake_send(void *ctx, uint32_t type, const pb_writer *body)
{
    (void)ctx;
    if (type == BEP_REQUEST) {
        int32_t id;
        pb_slice folder, name;
        bep_decode_request(body->data, body->len, &id, &folder, &name);
        last_request_id = id;
        requests++;
    } else if (type == BEP_INDEX_UPDATE) {
        announces++;
    }
    return TLS_OK;
}

/* An encoded FileInfo for a one-block file with the given content. */
static void fileinfo(pb_writer *out, const char *name, const char *content, uint64_t version)
{
    unsigned char md[32];
    unsigned int n;
    EVP_Digest(content, strlen(content), md, &n, EVP_sha256(), NULL);
    pb_writer blk, ver, cnt;
    pb_writer_init(out, 1 << 20);
    pb_writer_init(&blk, 1 << 20);
    pb_writer_init(&ver, 1 << 20);
    pb_writer_init(&cnt, 1 << 20);
    pb_put_string_field(out, 1, name);
    pb_put_varint_field(out, 3, strlen(content));
    pb_put_varint_field(&cnt, 1, 0x42);
    pb_put_varint_field(&cnt, 2, version);
    pb_put_message_field(&ver, 1, &cnt);
    pb_put_message_field(out, 9, &ver);
    pb_put_varint_field(&blk, 2, strlen(content));
    pb_put_bytes_field(&blk, 3, md, 32);
    pb_put_message_field(out, 16, &blk);
    pb_writer_free(&blk);
    pb_writer_free(&ver);
    pb_writer_free(&cnt);
}

static const char *offer(fetch *f, const pb_writer *raw)
{
    bep_file fi;
    if (bep_decode_fileinfo((pb_slice){ raw->data, raw->len }, &fi) != 0)
        return "BAD_TEST_FILEINFO";
    return fetch_offer(f, &fi);
}

static char *read_file(const char *path)
{
    static char buf[256];
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    size_t n = fread(buf, 1, sizeof buf - 1, fp);
    buf[n] = 0;
    fclose(fp);
    return buf;
}

static void write_file(const char *path, const char *content)
{
    FILE *fp = fopen(path, "wb");
    if (fp) {
        fputs(content, fp);
        fclose(fp);
    }
}

/* Neither the temporary nor the moved-aside file of name is left in dir. */
static int no_leftovers(const char *dir, const char *name)
{
    char p[900];
    snprintf(p, sizeof p, "%s/.stpoc-%s.tmp", dir, name);
    if (access(p, F_OK) == 0)
        return 0;
    snprintf(p, sizeof p, "%s/.stpoc-%s.old", dir, name);
    return access(p, F_OK) != 0;
}

/* a.txt holds content A and the state records exactly that. */
static void reset_a(const char *path, const char *spath, const char *dest, const unsigned char *peer,
                    const pb_writer *a)
{
    char why[300];
    write_file(path, "content A");
    state_list st;
    memset(&st, 0, sizeof st);
    state_put(&st, "a.txt", 0, a->data, a->len);
    if (state_save(spath, "f", dest, peer, &st, why, sizeof why) != 0) {
        failures++;
        printf("FAIL reset_a: %s\n", why);
    }
    state_list_free(&st);
}

int main(int argc, char **argv)
{
    char root[512], dest[600], id[600], path[700], spath[800], why[300];
    unsigned char peer[DEVID_BYTES];
    memset(peer, 0x33, sizeof peer);
    snprintf(root, sizeof root, "%s/stpoc-fetch-%d", argc > 1 ? argv[1] : "/tmp", (int)getpid());
    snprintf(dest, sizeof dest, "%s/dest", root);
    snprintf(id, sizeof id, "%s/id", root);
    mkdir(root, 0755);
    mkdir(dest, 0755);
    mkdir(id, 0755);
    snprintf(path, sizeof path, "%s/a.txt", dest);

    /* Here: a.txt with content A, recorded in the state. */
    FILE *fp = fopen(path, "wb");
    fputs("content A", fp);
    fclose(fp);
    pb_writer a, b;
    fileinfo(&a, "a.txt", "content A", 1);
    fileinfo(&b, "a.txt", "content B, newer", 2);
    state_list st;
    memset(&st, 0, sizeof st);
    state_put(&st, "a.txt", 0, a.data, a.len);
    state_path(id, "f", spath, sizeof spath);
    CHECK(state_save(spath, "f", dest, peer, &st, why, sizeof why) == 0, "state save: %s", why);
    state_list_free(&st);

    for (int started = 0; started <= 1; started++) {
        store *s = store_open(dest, why, sizeof why);
        fetch_state_opts so = { spath, dest, peer, 0 };
        fetch *f = fetch_new("f", s, quiet, NULL, fake_send, NULL, 0, &so);
        CHECK(f && fetch_prepare(f) == 0, "prepare");
        requests = announces = 0;
        net_ms wake;

        CHECK(offer(f, &b) == NULL && !fetch_idle(f), "B was not queued");
        if (started) {
            CHECK(fetch_step(f, net_now(), &wake) == NULL && requests == 1, "no Request for B");
        }
        int32_t old_id = last_request_id;

        /* The peer goes back to content A: nothing may be left to do. */
        CHECK(offer(f, &a) == NULL, "offer A");
        CHECK(fetch_idle(f), "%s: work for B still pending after A", started ? "started" : "queued");
        CHECK(fetch_step(f, net_now(), &wake) == NULL && requests == (started ? 1 : 0),
              "%s: a Request was sent after A", started ? "started" : "queued");

        if (started) {
            /* The late Response to B's request must not be written. */
            pb_slice data = { (const unsigned char *)"content B, newer", 16 };
            CHECK(fetch_response(f, old_id, data, 0) == NULL, "late Response");
            CHECK(fetch_step(f, net_now(), &wake) == NULL, "step after late Response");
        }
        const char *now = read_file(path);
        CHECK(now && strcmp(now, "content A") == 0, "%s: a.txt changed to \"%s\"", started ? "started" : "queued",
              now ? now : "(missing)");
        CHECK(announces == 0, "%s: B was announced", started ? "started" : "queued");
        CHECK(fetch_get_stats(f)->files_done == 0, "a download completed");
        char tmp[800];
        snprintf(tmp, sizeof tmp, "%s/.stpoc-a.txt.tmp", dest);
        CHECK(access(tmp, F_OK) != 0, "%s: temporary file left behind", started ? "started" : "queued");
        fetch_free(f);
        store_close(s);
    }

    /* B started, then C (another download) for the same name: the late
     * Response for B must not end up in C's file or be announced. */
    {
        pb_writer c;
        fileinfo(&c, "a.txt", "content C, newest", 3);
        store *s = store_open(dest, why, sizeof why);
        fetch_state_opts so = { spath, dest, peer, 0 };
        fetch *f = fetch_new("f", s, quiet, NULL, fake_send, NULL, 0, &so);
        CHECK(f && fetch_prepare(f) == 0, "prepare");
        requests = announces = 0;
        net_ms wake;
        CHECK(offer(f, &b) == NULL, "offer B");
        CHECK(fetch_step(f, net_now(), &wake) == NULL && requests == 1, "no Request for B");
        int32_t b_id = last_request_id;
        CHECK(offer(f, &c) == NULL && !fetch_idle(f), "C not queued");
        CHECK(fetch_step(f, net_now(), &wake) == NULL && requests == 2, "no Request for C");
        int32_t c_id = last_request_id;
        CHECK(b_id != c_id, "request ids reused");
        CHECK(fetch_response(f, b_id, (pb_slice){ (const unsigned char *)"content B, newer", 16 }, 0) == NULL,
              "late B");
        CHECK(fetch_step(f, net_now(), &wake) == NULL && announces == 0, "something announced after late B");
        CHECK(fetch_response(f, c_id, (pb_slice){ (const unsigned char *)"content C, newest", 17 }, 0) == NULL,
              "C response");
        CHECK(fetch_step(f, net_now(), &wake) == NULL, "step after C");
        const char *now = read_file(path);
        CHECK(now && strcmp(now, "content C, newest") == 0, "B then C: a.txt is \"%s\"", now ? now : "(missing)");
        CHECK(announces == 1 && fetch_get_stats(f)->files_done == 1, "B then C: %d announce(s), %ld done",
              announces, fetch_get_stats(f)->files_done);
        fetch_free(f);
        store_close(s);
        pb_writer_free(&c);
    }

    /* Local edit after the start-up check, while the newer version is being
     * downloaded: the edit must survive and be reported as a conflict. */
    {
        reset_a(path, spath, dest, peer, &a);
        store *s = store_open(dest, why, sizeof why);
        fetch_state_opts so = { spath, dest, peer, 0 };
        fetch *f = fetch_new("f", s, quiet, NULL, fake_send, NULL, 0, &so);
        CHECK(f && fetch_prepare(f) == 0, "prepare");
        requests = announces = 0;
        net_ms wake;
        CHECK(offer(f, &b) == NULL, "offer B");
        CHECK(fetch_step(f, net_now(), &wake) == NULL && requests == 1, "no Request for B");
        write_file(path, "edited here meanwhile");
        CHECK(fetch_response(f, last_request_id, (pb_slice){ (const unsigned char *)"content B, newer", 16 }, 0)
                  == NULL, "B response");
        CHECK(fetch_step(f, net_now(), &wake) == NULL, "step");
        const char *now = read_file(path);
        CHECK(now && strcmp(now, "edited here meanwhile") == 0, "local edit lost: a.txt is \"%s\"",
              now ? now : "(missing)");
        CHECK(announces == 0, "B announced over a local edit");
        CHECK(fetch_get_stats(f)->conflicts == 1, "edit not reported as conflict (%ld)", fetch_get_stats(f)->conflicts);
        CHECK(no_leftovers(dest, "a.txt"), "temporary or old file left behind");
        fetch_free(f);
        store_close(s);
    }

    /* A new name that is created here while it is being downloaded. */
    {
        pb_writer n;
        char npath[700];
        fileinfo(&n, "new.txt", "from the peer", 1);
        snprintf(npath, sizeof npath, "%s/new.txt", dest);
        remove(npath);
        store *s = store_open(dest, why, sizeof why);
        fetch_state_opts so = { spath, dest, peer, 0 };
        fetch *f = fetch_new("f", s, quiet, NULL, fake_send, NULL, 0, &so);
        CHECK(f && fetch_prepare(f) == 0, "prepare");
        requests = announces = 0;
        net_ms wake;
        CHECK(offer(f, &n) == NULL, "offer new");
        CHECK(fetch_step(f, net_now(), &wake) == NULL && requests == 1, "no Request for new.txt");
        write_file(npath, "created here meanwhile");
        CHECK(fetch_response(f, last_request_id, (pb_slice){ (const unsigned char *)"from the peer", 13 }, 0)
                  == NULL, "new response");
        CHECK(fetch_step(f, net_now(), &wake) == NULL, "step");
        const char *now = read_file(npath);
        CHECK(now && strcmp(now, "created here meanwhile") == 0, "local file replaced: new.txt is \"%s\"",
              now ? now : "(missing)");
        CHECK(announces == 0 && fetch_get_stats(f)->conflicts == 1, "new.txt: %d announce(s), %ld conflict(s)",
              announces, fetch_get_stats(f)->conflicts);
        CHECK(no_leftovers(dest, "new.txt"), "temporary or old file left behind");
        fetch_free(f);
        store_close(s);
        remove(npath);
        pb_writer_free(&n);
    }

    /* Interrupted replacement: the old version was moved aside and the new
     * one never took its place. The next run puts the old one back. */
    {
        reset_a(path, spath, dest, peer, &a);
        char old[800];
        snprintf(old, sizeof old, "%s/.stpoc-a.txt.old", dest);
        CHECK(rename(path, old) == 0, "simulate the interruption");
        store *s = store_open(dest, why, sizeof why);
        fetch_state_opts so = { spath, dest, peer, 0 };
        fetch *f = fetch_new("f", s, quiet, NULL, fake_send, NULL, 0, &so);
        CHECK(f && fetch_prepare(f) == 0, "prepare");
        requests = announces = 0;
        net_ms wake;
        CHECK(offer(f, &a) == NULL && fetch_idle(f), "A queued again after an interrupted replacement");
        CHECK(fetch_step(f, net_now(), &wake) == NULL && requests == 0, "A downloaded again");
        const char *now = read_file(path);
        CHECK(now && strcmp(now, "content A") == 0, "old version not restored: a.txt is \"%s\"",
              now ? now : "(missing)");
        CHECK(access(old, F_OK) != 0, "old version still kept aside");
        fetch_free(f);
        store_close(s);
    }

    /* Both the file and a moved-aside copy are here (review 2026-11-09).
     * (a) The copy is the recorded version: a replacement that completed
     *     just before an interruption; it is dropped, nothing else changes.
     * (b) The copy holds something else: it is kept, the name is a
     *     conflict, nothing is downloaded or replaced. */
    for (int variant = 0; variant < 2; variant++) {
        reset_a(path, spath, dest, peer, &a);
        char old[800];
        snprintf(old, sizeof old, "%s/.stpoc-a.txt.old", dest);
        write_file(old, variant == 0 ? "content A" : "kept local changes");
        store *s = store_open(dest, why, sizeof why);
        fetch_state_opts so = { spath, dest, peer, 0 };
        fetch *f = fetch_new("f", s, quiet, NULL, fake_send, NULL, 0, &so);
        CHECK(f && fetch_prepare(f) == 0, "prepare");
        requests = announces = 0;
        net_ms wake;
        CHECK(offer(f, &b) == NULL, "offer B");
        if (variant == 0) {
            CHECK(access(old, F_OK) != 0, "(a) recorded copy not dropped");
            CHECK(fetch_step(f, net_now(), &wake) == NULL && requests == 1, "(a) B not requested");
            CHECK(fetch_response(f, last_request_id, (pb_slice){ (const unsigned char *)"content B, newer", 16 }, 0)
                      == NULL && fetch_step(f, net_now(), &wake) == NULL, "(a) B");
            const char *now = read_file(path);
            CHECK(now && strcmp(now, "content B, newer") == 0, "(a) a.txt is \"%s\"", now ? now : "(missing)");
        } else {
            CHECK(fetch_idle(f) && fetch_step(f, net_now(), &wake) == NULL && requests == 0, "(b) B requested");
            CHECK(fetch_get_stats(f)->conflicts == 1, "(b) %ld conflict(s)", fetch_get_stats(f)->conflicts);
            const char *now = read_file(old);
            CHECK(now && strcmp(now, "kept local changes") == 0, "(b) kept copy is \"%s\"", now ? now : "(missing)");
            now = read_file(path);
            CHECK(now && strcmp(now, "content A") == 0, "(b) a.txt is \"%s\"", now ? now : "(missing)");
            remove(old);
        }
        fetch_free(f);
        store_close(s);
    }

    /* Review 2026-11-09: what was not received must not end as a full
     * success. A name this system cannot take and a symbolic link make the
     * receipt partial; a deletion is a deliberate skip of this mode. */
    {
        store *s = store_open(dest, why, sizeof why);
        fetch *f = fetch_new("f", s, quiet, NULL, fake_send, NULL, 0, NULL);
        CHECK(f && fetch_prepare(f) == 0, "prepare");
        CHECK(fetch_result(f) == NULL, "fresh fetch not a success");
        pb_writer del;
        pb_writer_init(&del, 1024);
        pb_put_string_field(&del, 1, "gone.txt");
        pb_put_varint_field(&del, 6, 1); /* deleted */
        CHECK(offer(f, &del) == NULL && fetch_result(f) == NULL, "a deletion made the result %s",
              fetch_result(f) ? fetch_result(f) : "?");
        pb_writer_free(&del);
        pb_writer pl;
        fileinfo(&pl, "za\xc5\xbc\xc3\xb3\xc5\x82\xc4\x87.txt", "polish name", 1);
        CHECK(offer(f, &pl) == NULL, "offer non-ASCII");
        const char *r = fetch_result(f);
        CHECK(r && strcmp(r, "PARTIAL") == 0, "rejected name gives %s", r ? r : "success");
        pb_writer_free(&pl);
        fetch_free(f);
        f = fetch_new("f", s, quiet, NULL, fake_send, NULL, 0, NULL);
        pb_writer sl;
        pb_writer_init(&sl, 1024);
        pb_put_string_field(&sl, 1, "link");
        pb_put_varint_field(&sl, 2, 4); /* SYMLINK */
        CHECK(offer(f, &sl) == NULL && (r = fetch_result(f)) && strcmp(r, "PARTIAL") == 0, "symlink gives %s",
              r ? r : "success");
        pb_writer_free(&sl);
        fetch_free(f);
        store_close(s);
    }

    /* Review 2026-11-09: a failed state write is retried; a passing retry
     * leaves nothing unsaved. */
    {
        reset_a(path, spath, dest, peer, &a);
        store *s = store_open(dest, why, sizeof why);
        fetch_state_opts so = { spath, dest, peer, 0 };
        fetch *f = fetch_new("f", s, quiet, NULL, fake_send, NULL, 0, &so);
        CHECK(f && fetch_prepare(f) == 0, "prepare");
        net_ms wake;
        CHECK(offer(f, &b) == NULL && fetch_step(f, net_now(), &wake) == NULL, "B");
        fetch_test_fail_state_writes = 1;
        CHECK(fetch_response(f, last_request_id, (pb_slice){ (const unsigned char *)"content B, newer", 16 }, 0) == NULL
                  && fetch_step(f, net_now(), &wake) == NULL, "B written");
        CHECK(fetch_flush(f) != 0, "flush did not fail with the hook");
        CHECK(fetch_get_stats(f)->state_failures == 1, "%ld state failure(s)", fetch_get_stats(f)->state_failures);
        const char *r = fetch_result(f);
        CHECK(r && strcmp(r, "STATE_NOT_SAVED") == 0, "unsaved state gives %s", r ? r : "success");
        CHECK(fetch_flush(f) == 0, "retry failed");
        CHECK(fetch_result(f) == NULL, "result after a passing retry: %s", fetch_result(f));
        state_list st;
        memset(&st, 0, sizeof st);
        CHECK(state_load(spath, "f", dest, peer, &st, why, sizeof why) == 0, "load");
        state_entry *e = state_find(&st, "a.txt");
        CHECK(e && e->raw_len == b.len && memcmp(e->raw, b.data, b.len) == 0, "B not recorded after retry");
        state_list_free(&st);
        fetch_free(f);
        store_close(s);
    }

    /* Many entries adopted in one go: the state file is not rewritten for
     * each (that was quadratic in the folder size), and after fetch_flush()
     * it holds every one of them. */
    {
        char sub[700];
        snprintf(sub, sizeof sub, "%s/many", dest);
        mkdir(sub, 0755);
        enum { N = 300 };
        pb_writer fi[N];
        for (int i = 0; i < N; i++) {
            char name[64], content[64], p2[800];
            snprintf(name, sizeof name, "many/m%03d.txt", i);
            snprintf(content, sizeof content, "many %d", i);
            snprintf(p2, sizeof p2, "%s/%s", dest, name);
            write_file(p2, content);
            fileinfo(&fi[i], name, content, 1);
        }
        store *s = store_open(dest, why, sizeof why);
        fetch_state_opts so = { spath, dest, peer, 0 };
        fetch *f = fetch_new("f", s, quiet, NULL, fake_send, NULL, 0, &so);
        CHECK(f && fetch_prepare(f) == 0, "prepare");
        long saves0 = fetch_get_stats(f)->state_saves;
        for (int i = 0; i < N; i++)
            CHECK(offer(f, &fi[i]) == NULL, "offer %d", i);
        CHECK(fetch_get_stats(f)->adopted == N, "adopted %ld of %d", fetch_get_stats(f)->adopted, N);
        long saves = fetch_get_stats(f)->state_saves - saves0;
        CHECK(saves <= 2, "state written %ld times for %d adoptions", saves, N);
        CHECK(fetch_flush(f) == 0, "flush");
        state_list st;
        memset(&st, 0, sizeof st);
        CHECK(state_load(spath, "f", dest, peer, &st, why, sizeof why) == 0, "load: %s", why);
        int found = 0;
        for (int i = 0; i < N; i++) {
            char name[64];
            snprintf(name, sizeof name, "many/m%03d.txt", i);
            found += state_find(&st, name) != NULL;
        }
        CHECK(found == N, "%d of %d recorded after flush", found, N);
        state_list_free(&st);
        fetch_free(f);
        store_close(s);
        for (int i = 0; i < N; i++)
            pb_writer_free(&fi[i]);
    }

    /* Review 2026-11-09 (second): while state writes keep failing, the
     * back-off holds even when the pending count is past the batch size.
     * Before the fix every entry after the 256th tried again at once (45
     * attempts in 14 ms for 300 adoptions). */
    {
        char sub[700];
        snprintf(sub, sizeof sub, "%s/fail", dest);
        mkdir(sub, 0755);
        enum { N = 300 };
        pb_writer fi[N];
        for (int i = 0; i < N; i++) {
            char name[64], content[64], p2[800];
            snprintf(name, sizeof name, "fail/m%03d.txt", i);
            snprintf(content, sizeof content, "fail %d", i);
            snprintf(p2, sizeof p2, "%s/%s", dest, name);
            write_file(p2, content);
            fileinfo(&fi[i], name, content, 1);
        }
        store *s = store_open(dest, why, sizeof why);
        fetch_state_opts so = { spath, dest, peer, 0 };
        fetch *f = fetch_new("f", s, quiet, NULL, fake_send, NULL, 0, &so);
        CHECK(f && fetch_prepare(f) == 0, "prepare");
        fetch_test_fail_state_writes = 1000000;
        long saves0 = fetch_get_stats(f)->state_saves;
        for (int i = 0; i < N; i++)
            CHECK(offer(f, &fi[i]) == NULL, "offer %d", i);
        long tries = fetch_get_stats(f)->state_saves - saves0;
        CHECK(tries == 1, "%ld state write attempts for %d adoptions while writes fail (want 1, then back-off)",
              tries, N);
        net_ms wake;
        CHECK(fetch_step(f, net_now(), &wake) == NULL, "step");
        CHECK(fetch_get_stats(f)->state_saves - saves0 == 1, "idle step ignored the back-off");
        const char *r = fetch_result(f);
        CHECK(r && strcmp(r, "STATE_NOT_SAVED") == 0, "result %s", r ? r : "success");
        fetch_test_fail_state_writes = 0;
        CHECK(fetch_flush(f) == 0 && fetch_result(f) == NULL, "final flush");
        fetch_free(f);
        store_close(s);
        for (int i = 0; i < N; i++)
            pb_writer_free(&fi[i]);
    }

    pb_writer_free(&a);
    pb_writer_free(&b);
    char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd) != 0)
        printf("note: could not remove %s\n", root);
    printf("%s: test_fetch\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
