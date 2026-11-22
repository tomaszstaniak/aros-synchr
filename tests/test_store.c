/* Destination store: name checks, collisions, replacement by rename, and an
 * interrupted write leaving the previous version intact. Runs in a fresh
 * directory under the given path (default /tmp). */
#include "../src/store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char root[512];
extern unsigned store_test_fail_renames; /* store.c built with STORE_TEST_HOOKS */
extern int store_test_fail_writes;
extern int store_test_keep_old;

/* Replacement check that accepts whatever is there (a plain overwrite). */
static int any(void *ctx, const char *path)
{
    (void)ctx;
    (void)path;
    return 0;
}

/* Replacement check: the file must hold exactly the string ctx. */
static int holds(void *ctx, const char *path)
{
    char buf[256];
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    return strcmp(buf, ctx) == 0 ? 0 : -1;
}

static int commit_for(store *s, const char *name, store_file *f, char *why, size_t n)
{
    char p[1024];
    struct stat st;
    int replaced;
    snprintf(p, sizeof p, "%s/%s", root, name);
    (void)s;
    return store_commit(f, lstat(p, &st) == 0 ? any : NULL, NULL, &replaced, why, n);
}

static int exists_at(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

static int put_why(store *s, const char *name, const char *content, char *why, size_t n)
{
    store_file *f = store_begin(s, name, why, n);
    int replaced;
    if (!f)
        return -1;
    if (store_write(f, 0, content, strlen(content)) != 0) {
        store_abort(f);
        return -1;
    }
    (void)replaced;
    return commit_for(s, name, f, why, n);
}

static int read_file(const char *rel, char *buf, size_t cap)
{
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    FILE *f = fopen(p, "rb");
    if (!f)
        return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = 0;
    fclose(f);
    return (int)n;
}

static int put(store *s, const char *name, const char *content)
{
    char why[1024];
    store_file *f = store_begin(s, name, why, sizeof why);
    int replaced;
    if (!f)
        return -1;
    if (store_write(f, 0, content, strlen(content)) != 0) {
        store_abort(f);
        return -1;
    }
    (void)replaced;
    return commit_for(s, name, f, why, sizeof why);
}

int main(int argc, char **argv)
{
    char why[1024], buf[256];
    snprintf(root, sizeof root, "%s/stpoc-store-%d", argc > 1 ? argv[1] : "/tmp", (int)getpid());
    mkdir(root, 0755);
    store *s = store_open(root, why, sizeof why);
    CHECK(s != NULL, "open: %s", why);

    const char *bad[] = { "", "/etc/passwd", "../x", "a/../../x", "a/./b", "a//b", "a/", "vol:x",
                          "a\\b", "tab\there", "caf\xc3\xa9", ".stpoc-a.tmp", "dir/.stpoc-x",
                          "trailing.", "trailing ",
                          "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
        CHECK(store_accept(s, bad[i], 0, why, sizeof why) != 0, "accepted bad name [%zu] %s", i, bad[i]);

    CHECK(store_accept(s, "sub", 1, why, sizeof why) == 0, "dir: %s", why);
    CHECK(store_accept(s, "sub/nested.txt", 0, why, sizeof why) == 0, "file: %s", why);
    CHECK(store_accept(s, "sub/nested.txt", 0, why, sizeof why) == 0, "same name again: %s", why);
    CHECK(store_accept(s, "SUB/Nested.TXT", 0, why, sizeof why) != 0, "case collision accepted");
    CHECK(store_accept(s, "sub", 0, why, sizeof why) != 0, "dir re-registered as file");
    CHECK(store_accept(s, "sub/nested.txt/x", 0, why, sizeof why) != 0, "file used as parent");
    CHECK(store_accept(s, "Hello.txt", 0, why, sizeof why) == 0, "file: %s", why);
    CHECK(store_accept(s, "hello.txt", 0, why, sizeof why) != 0, "case collision accepted (2)");

    /* new file, then a replacement */
    CHECK(put(s, "sub/nested.txt", "version 1") == 0, "first write");
    CHECK(read_file("sub/nested.txt", buf, sizeof buf) == 9 && strcmp(buf, "version 1") == 0, "v1 content");
    CHECK(put(s, "sub/nested.txt", "version 2, longer") == 0, "replacement");
    CHECK(read_file("sub/nested.txt", buf, sizeof buf) > 0 && strcmp(buf, "version 2, longer") == 0, "v2 content");

    /* interrupted download: abandoned and aborted temporaries leave v2 */
    store_file *f = store_begin(s, "sub/nested.txt", why, sizeof why);
    CHECK(f && store_write(f, 0, "partial", 7) == 0, "partial write");
    CHECK(store_write(f, 3, "x", 1) != 0, "out-of-order offset accepted");
    store_abandon(f);
    CHECK(read_file("sub/nested.txt", buf, sizeof buf) > 0 && strcmp(buf, "version 2, longer") == 0,
          "previous version damaged by an interrupted write");
    CHECK(read_file("sub/.stpoc-nested.txt.tmp", buf, sizeof buf) == 7, "abandoned temp not kept");
    f = store_begin(s, "sub/nested.txt", why, sizeof why);
    store_abort(f);
    CHECK(read_file("sub/.stpoc-nested.txt.tmp", buf, sizeof buf) < 0, "aborted temp not removed");
    CHECK(read_file("sub/nested.txt", buf, sizeof buf) > 0 && strcmp(buf, "version 2, longer") == 0,
          "previous version damaged by an aborted write");

    /* empty file */
    CHECK(store_accept(s, "empty.txt", 0, why, sizeof why) == 0 && put(s, "empty.txt", "") == 0, "empty");
    CHECK(read_file("empty.txt", buf, sizeof buf) == 0, "empty content");

    /* review 2026-10-05: the reserved prefix in any letter case */
    CHECK(store_accept(s, ".STPOC-hello.txt.tmp", 0, why, sizeof why) != 0, "upper-case prefix accepted");
    CHECK(store_accept(s, "sub/.Stpoc-x", 0, why, sizeof why) != 0, "mixed-case prefix accepted");

    /* review 2026-10-05: existing symbolic links must not lead outside */
    char outside[700], link[700], p[800];
    snprintf(outside, sizeof outside, "%s-outside", root);
    mkdir(outside, 0755);
    snprintf(link, sizeof link, "%s/linkdir", root);
    CHECK(symlink(outside, link) == 0, "cannot create test link");
    CHECK(store_accept(s, "linkdir/file.txt", 0, why, sizeof why) == 0, "name itself is valid: %s", why);
    CHECK(store_begin(s, "linkdir/file.txt", why, sizeof why) == NULL, "write through a linked directory");
    snprintf(p, sizeof p, "%s/file.txt", outside);
    CHECK(!exists_at(p), "a file was created outside the destination");
    CHECK(store_mkdir(s, "linkdir/newdir", why, sizeof why) != 0, "mkdir through a linked directory");
    snprintf(p, sizeof p, "%s/newdir", outside);
    CHECK(!exists_at(p), "a directory was created outside the destination");

    snprintf(p, sizeof p, "%s/target.txt", outside);
    snprintf(link, sizeof link, "%s/linked.txt", root);
    CHECK(symlink(p, link) == 0, "cannot create file link");
    CHECK(store_accept(s, "linked.txt", 0, why, sizeof why) == 0
              && store_begin(s, "linked.txt", why, sizeof why) == NULL,
          "write to a linked file name");
    CHECK(!exists_at(p), "link target outside was created");

    snprintf(p, sizeof p, "%s/evil", outside);
    snprintf(link, sizeof link, "%s/.stpoc-t.txt.tmp", root);
    CHECK(symlink(p, link) == 0, "cannot create temp link");
    CHECK(store_accept(s, "t.txt", 0, why, sizeof why) == 0 && store_begin(s, "t.txt", why, sizeof why) == NULL,
          "temporary name that is a link was used");
    CHECK(!exists_at(p), "write followed the temporary-name link outside");

    /* review 2026-10-05: an lstat() failure other than ENOENT is an error,
     * not "does not exist" (a directory without search permission) */
    snprintf(p, sizeof p, "%s/locked", root);
    mkdir(p, 0755);
    chmod(p, 0);
    if (geteuid() != 0) {
        CHECK(store_accept(s, "locked/inner.txt", 0, why, sizeof why) == 0
                  && store_begin(s, "locked/inner.txt", why, sizeof why) == NULL && strstr(why, "cannot check"),
              "unreadable path not reported: %s", why);
    }
    chmod(p, 0755);

    /* review 2026-10-05: a failed restore must say where the old version is */
    CHECK(store_accept(s, "r.txt", 0, why, sizeof why) == 0 && put(s, "r.txt", "old") == 0, "r.txt v1");
    store_test_fail_renames = 1u << 0; /* moving the old file aside fails */
    CHECK(put_why(s, "r.txt", "new", why, sizeof why) != 0 && strstr(why, "not changed"), "aside: %s", why);
    CHECK(read_file("r.txt", buf, sizeof buf) == 3 && strcmp(buf, "old") == 0, "aside failure changed r.txt");
    store_test_fail_renames = 1u << 1; /* new into place fails, restore works */
    CHECK(put_why(s, "r.txt", "new", why, sizeof why) != 0 && strstr(why, "old version restored"),
          "restore: %s", why);
    CHECK(read_file("r.txt", buf, sizeof buf) == 3 && strcmp(buf, "old") == 0, "restored content");
    store_test_fail_renames = (1u << 1) | (1u << 2); /* and the restore fails too */
    CHECK(put_why(s, "r.txt", "new", why, sizeof why) != 0 && strstr(why, "kept as")
              && strstr(why, ".stpoc-r.txt.old") && !strstr(why, "restored"),
          "failed restore reported as: %s", why);
    CHECK(read_file(".stpoc-r.txt.old", buf, sizeof buf) == 3 && strcmp(buf, "old") == 0,
          "previous version not where the message says");
    store_test_fail_renames = 0;

    /* Expectations at commit time (2026-11-09). Nothing expected, but a
     * file was created here meanwhile: it stays. */
    int replaced;
    CHECK(store_accept(s, "x.txt", 0, why, sizeof why) == 0, "x.txt");
    f = store_begin(s, "x.txt", why, sizeof why);
    CHECK(f && store_write(f, 0, "peer", 4) == 0, "x.txt write");
    snprintf(p, sizeof p, "%s/x.txt", root);
    FILE *lf = fopen(p, "wb");
    fputs("local", lf);
    fclose(lf);
    CHECK(store_commit(f, NULL, NULL, &replaced, why, sizeof why) == 1, "created meanwhile: %s", why);
    CHECK(read_file("x.txt", buf, sizeof buf) == 5 && strcmp(buf, "local") == 0, "created meanwhile: replaced");
    CHECK(read_file(".stpoc-x.txt.tmp", buf, sizeof buf) < 0, "temporary left after conflict");

    /* A version is expected but another one is here: put back unchanged. */
    f = store_begin(s, "x.txt", why, sizeof why);
    CHECK(f && store_write(f, 0, "peer", 4) == 0, "x.txt write 2");
    CHECK(store_commit(f, holds, "expected", &replaced, why, sizeof why) == 1 && !replaced, "changed: %s", why);
    CHECK(read_file("x.txt", buf, sizeof buf) == 5 && strcmp(buf, "local") == 0, "changed version not kept");
    CHECK(read_file(".stpoc-x.txt.old", buf, sizeof buf) < 0 && read_file(".stpoc-x.txt.tmp", buf, sizeof buf) < 0,
          "leftovers after a refused replacement");
    /* The expected version is here: replaced. */
    f = store_begin(s, "x.txt", why, sizeof why);
    CHECK(f && store_write(f, 0, "peer", 4) == 0, "x.txt write 3");
    CHECK(store_commit(f, holds, "local", &replaced, why, sizeof why) == 0 && replaced, "expected: %s", why);
    CHECK(read_file("x.txt", buf, sizeof buf) == 4 && strcmp(buf, "peer") == 0, "not replaced");
    /* Expected version, but the name vanished meanwhile: nothing written. */
    f = store_begin(s, "x.txt", why, sizeof why);
    CHECK(f && store_write(f, 0, "next", 4) == 0, "x.txt write 4");
    remove(p);
    CHECK(store_commit(f, holds, "peer", &replaced, why, sizeof why) == 1, "vanished: %s", why);
    CHECK(read_file("x.txt", buf, sizeof buf) < 0, "written although the file was removed here");

    /* Interrupted between the two renames: recover puts the old one back. */
    snprintf(p, sizeof p, "%s/.stpoc-x.txt.old", root);
    lf = fopen(p, "wb");
    fputs("previous", lf);
    fclose(lf);
    CHECK(store_recover(s, "x.txt", why, sizeof why) == 1, "recover: %s", why);
    CHECK(read_file("x.txt", buf, sizeof buf) == 8 && strcmp(buf, "previous") == 0, "not recovered");
    CHECK(store_recover(s, "x.txt", why, sizeof why) == 0, "second recover did something");
    /* Both present (interrupted after the second rename): nothing moved. */
    lf = fopen(p, "wb");
    fputs("older", lf);
    fclose(lf);
    CHECK(store_recover(s, "x.txt", why, sizeof why) == 2
              && read_file("x.txt", buf, sizeof buf) == 8 && strcmp(buf, "previous") == 0,
          "both present not reported, or recover replaced a present file");
    /* Review 2026-11-09: a following correct commit must not delete the
     * moved-aside copy (it may hold changes kept after a failed restore). */
    f = store_begin(s, "x.txt", why, sizeof why);
    CHECK(f && store_write(f, 0, "peer", 4) == 0, "x.txt write 5");
    CHECK(store_commit(f, holds, "previous", &replaced, why, sizeof why) == 1 && strstr(why, ".stpoc-x.txt.old"),
          "commit with a kept copy present: %s", why);
    CHECK(read_file(".stpoc-x.txt.old", buf, sizeof buf) == 5 && strcmp(buf, "older") == 0, "kept copy deleted");
    CHECK(read_file("x.txt", buf, sizeof buf) == 8 && strcmp(buf, "previous") == 0, "x.txt replaced anyway");
    CHECK(read_file(".stpoc-x.txt.tmp", buf, sizeof buf) < 0, "temporary left after refusal");
    /* Only an explicit decision removes it. */
    CHECK(store_discard_old(s, "x.txt", why, sizeof why) == 0 && read_file(".stpoc-x.txt.old", buf, sizeof buf) < 0,
          "discard: %s", why);

    /* The previous version cannot be deleted after a replacement (on AROS:
     * still open): the replacement stands, the copy stays and is reported. */
    store_test_keep_old = 1;
    f = store_begin(s, "x.txt", why, sizeof why);
    CHECK(f && store_write(f, 0, "peer2", 5) == 0, "x.txt write 6");
    CHECK(store_commit(f, holds, "previous", &replaced, why, sizeof why) == 0 && replaced && strstr(why, "kept as"),
          "kept copy not reported: %s", why);
    store_test_keep_old = 0;
    CHECK(read_file("x.txt", buf, sizeof buf) == 5 && strcmp(buf, "peer2") == 0, "not replaced");
    CHECK(read_file(".stpoc-x.txt.old", buf, sizeof buf) == 8 && strcmp(buf, "previous") == 0, "copy not kept");
    CHECK(store_recover(s, "x.txt", why, sizeof why) == 2, "kept copy not found by recover");
    snprintf(p, sizeof p, "%s/.stpoc-x.txt.old", root);
    remove(p);
    snprintf(p, sizeof p, "%s/x.txt", root);
    lf = fopen(p, "wb");
    fputs("previous", lf);
    fclose(lf);

    /* A write that fails (no space left): the previous version stays and
     * no temporary file remains after the abort. */
    store_test_fail_writes = 1;
    f = store_begin(s, "x.txt", why, sizeof why);
    CHECK(f && store_write(f, 0, "will not fit", 12) != 0, "failing write reported success");
    store_abort(f);
    store_test_fail_writes = 0;
    CHECK(read_file("x.txt", buf, sizeof buf) == 8 && strcmp(buf, "previous") == 0, "write failure damaged x.txt");
    CHECK(read_file(".stpoc-x.txt.tmp", buf, sizeof buf) < 0, "temporary left after a failed write");
    /* Failing only when flushed at commit (buffered data, full disk). */
    store_test_fail_writes = 2;
    f = store_begin(s, "x.txt", why, sizeof why);
    CHECK(f && store_write(f, 0, "buffered", 8) == 0, "buffered write");
    CHECK(store_commit(f, any, NULL, &replaced, why, sizeof why) == -1 && strstr(why, "writing"),
          "flush failure: %s", why);
    store_test_fail_writes = 0;
    CHECK(read_file("x.txt", buf, sizeof buf) == 8 && strcmp(buf, "previous") == 0, "flush failure damaged x.txt");
    CHECK(read_file(".stpoc-x.txt.tmp", buf, sizeof buf) < 0, "temporary left after a failed flush");

    store_close(s);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", outside);
    if (system(cmd) != 0)
        printf("note: could not remove %s\n", outside);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd) != 0)
        printf("note: could not remove %s\n", root);
    printf("%s: test_store\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
