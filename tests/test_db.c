/* State format 2 (src/db.c): round trip, binding to folder, path, peer
 * and our device, rejection of damaged or inconsistent files (also when
 * the checksum is right), format 1 detection, the .old fallback and the
 * name index.
 *
 *   test_db WORK-DIR */
#include "../src/bep.h"
#include "../src/db.h"
#include "../src/stfile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static char path[700];
static unsigned char peer[DEVID_BYTES], me[DEVID_BYTES], other[DEVID_BYTES];
static db_binding bind = { "aros-demo", "SYS:dest", peer, me };

static finfo file_entry(const char *name, int64_t seq, const char *vec)
{
    finfo f;
    memset(&f, 0, sizeof f);
    f.name = strdup(name);
    f.type = BEP_FILE;
    f.size = 3;
    f.modified_s = 1794240757;
    f.modified_by = 42;
    f.no_permissions = 1;
    f.permissions = 0644;
    f.sequence = seq;
    f.block_size = FINFO_MIN_BLOCK;
    vv_parse(vec, &f.version);
    f.blocks = calloc(1, sizeof *f.blocks);
    f.blocks[0].size = 3;
    f.blocks[0].hash[0] = 7;
    f.nblocks = 1;
    finfo_blocks_hash(f.blocks, 1, f.blocks_hash);
    f.has_blocks_hash = 1;
    return f;
}

/* Reads the saved file back as text (without its end line). */
static char *saved(size_t *len)
{
    char *buf, why[300];
    return stfile_read(path, &buf, len, why, sizeof why) == 0 ? buf : NULL;
}

/* Writes text with a correct checksum: only the content can be wrong. */
static void write_signed(const char *text)
{
    pb_writer w;
    char why[300];
    pb_writer_init(&w, STFILE_MAX);
    pb_put_raw(&w, text, strlen(text));
    if (stfile_write(path, &w, why, sizeof why) != 0)
        printf("cannot write test file: %s\n", why);
    pb_writer_free(&w);
}

/* Replaces the first occurrence of from by to in the saved text, writes
 * it signed, and expects a rejection. */
static void expect_rejected(const char *orig, const char *what, const char *from, const char *to)
{
    const char *at = strstr(orig, from);
    CHECK(at != NULL, "%s: pattern %s not in the file", what, from);
    if (!at)
        return;
    size_t n = strlen(orig) + strlen(to) + 1;
    char *t = malloc(n);
    snprintf(t, n, "%.*s%s%s", (int)(at - orig), orig, to, at + strlen(from));
    write_signed(t);
    free(t);
    db d;
    char why[300] = "";
    int rc = db_load(path, &bind, &d, why, sizeof why);
    CHECK(rc == -1 && d.n == 0, "%s was accepted (rc %d)", what, rc);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: test_db WORK-DIR\n");
        return 2;
    }
    snprintf(path, sizeof path, "%s/test-db-state.txt", argv[1]);
    for (int i = 0; i < DEVID_BYTES; i++) {
        peer[i] = (unsigned char)i;
        me[i] = (unsigned char)(100 + i);
        other[i] = (unsigned char)(200 + i);
    }
    remove(path);
    char why[300];
    db d, r;

    CHECK(db_load(path, &bind, &r, why, sizeof why) == 1, "missing file not reported as missing");

    memset(&d, 0, sizeof d);
    d.index_id = 0x0123456789abcdefull;
    d.peer_index_id = 0xfedcba9876543210ull;
    d.peer_sequence = 1010;
    finfo a = file_entry("a.txt", db_next_sequence(&d), "42:1794240757");
    finfo b = file_entry("dir/b x.txt", db_next_sequence(&d), "7:3,42:1794240758");
    db_entry *ea = db_put(&d, &a), *eb = db_put(&d, &b);
    CHECK(ea && eb, "put");
    ea->disk_size = 3;
    ea->disk_mtime_s = 1794240757;
    ea->disk_mtime_ns = 5;
    ea->flags = DB_HASH_NEXT;
    finfo_free(&a);
    finfo_free(&b);
    CHECK(db_save(path, &bind, &d, why, sizeof why) == 0, "save: %s", why);
    CHECK(db_load(path, &bind, &r, why, sizeof why) == 0, "load: %s", why);
    db_entry *ra = db_find(&r, "a.txt"), *rb = db_find(&r, "dir/b x.txt");
    CHECK(r.n == 2 && ra && rb && r.index_id == d.index_id && r.sequence == 2 && r.peer_index_id == d.peer_index_id
              && r.peer_sequence == 1010,
          "header round trip");
    CHECK(ra && ra->f.sequence == 1 && ra->disk_size == 3 && ra->disk_mtime_s == 1794240757 && ra->disk_mtime_ns == 5
              && ra->flags == DB_HASH_NEXT && ra->f.nblocks == 1 && ra->f.has_blocks_hash
              && vv_compare(&ra->f.version, &d.e[0].f.version) == VV_EQUAL,
          "entry round trip");
    db_free(&r);

    /* Bound to folder, path, peer and our own device. */
    db_binding wrong = bind;
    wrong.folder_id = "other";
    CHECK(db_load(path, &wrong, &r, why, sizeof why) == -1, "other folder accepted");
    wrong = bind;
    wrong.dest = "SYS:other";
    CHECK(db_load(path, &wrong, &r, why, sizeof why) == -1, "other path accepted");
    wrong = bind;
    wrong.peer_id = other;
    CHECK(db_load(path, &wrong, &r, why, sizeof why) == -1, "other peer accepted");
    wrong = bind;
    wrong.device_id = other;
    CHECK(db_load(path, &wrong, &r, why, sizeof why) == -1, "other device accepted");

    /* Damage, with a correct checksum: the content itself is checked. */
    size_t len;
    char *orig = saved(&len);
    CHECK(orig != NULL, "cannot read back");
    if (orig) {
        expect_rejected(orig, "sequence below an entry's", "sequence 2\n", "sequence 1\n");
        expect_rejected(orig, "zero index ID", "index 0123456789abcdef", "index 0000000000000000");
        expect_rejected(orig, "short index ID", "index 0123456789abcdef", "index 0123456789abcde");
        expect_rejected(orig, "count too high", "count 2\n", "count 3\n");
        expect_rejected(orig, "count too low", "count 2\n", "count 1\n");
        expect_rejected(orig, "unknown version", "aros-syncthing-state 2", "aros-syncthing-state 3");
        expect_rejected(orig, "bad flags", " 1\n", " x\n");
        /* The same entry twice. */
        char *first = strstr(orig, "entry "), *nl = first ? strchr(first, '\n') : NULL;
        if (first && nl) {
            size_t el = (size_t)(nl - first) + 1;
            char *dup = malloc(len + el + 1);
            snprintf(dup, len + el + 1, "%.*s%.*s%s", (int)(nl + 1 - orig), orig, (int)el, first, nl + 1);
            expect_rejected(dup, "duplicate entry", "count 2\n", "count 3\n");
            free(dup);
        }
        /* An unsigned edit: the checksum rejects it. */
        FILE *fp = fopen(path, "wb");
        fprintf(fp, "%s", orig);
        fprintf(fp, "end 0000000000000000000000000000000000000000000000000000000000000000\n");
        fclose(fp);
        CHECK(db_load(path, &bind, &r, why, sizeof why) == -1 && strstr(why, "checksum"), "bad checksum: %s", why);

        /* Two entries with one sequence. */
        write_signed(orig);
        CHECK(db_load(path, &bind, &r, why, sizeof why) == 0, "original no longer loads: %s", why);
        db_entry *e2 = db_find(&r, "dir/b x.txt");
        if (e2)
            e2->f.sequence = 1;
        CHECK(db_save(path, &bind, &r, why, sizeof why) == 0, "save dup seq");
        db_free(&r);
        CHECK(db_load(path, &bind, &r, why, sizeof why) == -1 && strstr(why, "same sequence"), "dup sequence: %s",
              why);
        free(orig);
    }

    /* Format 1 is recognised, not parsed. */
    write_signed("aros-syncthing-state 1\nfolder 00\n");
    CHECK(db_load(path, &bind, &r, why, sizeof why) == 2, "format 1 not recognised");

    /* An interrupted replacement: only <path>.old is there. */
    CHECK(db_save(path, &bind, &d, why, sizeof why) == 0, "save");
    char old[800];
    snprintf(old, sizeof old, "%s.old", path);
    rename(path, old);
    CHECK(db_load(path, &bind, &r, why, sizeof why) == 0 && r.n == 2, "the .old copy was not used: %s", why);
    db_free(&r);
    remove(old);

    /* The name index through many puts and removals. */
    db big;
    memset(&big, 0, sizeof big);
    big.index_id = 1;
    char name[32];
    for (int i = 0; i < 3000; i++) {
        snprintf(name, sizeof name, "f%04d", i);
        finfo f = file_entry(name, db_next_sequence(&big), "1:1");
        db_put(&big, &f);
        finfo_free(&f);
    }
    for (int i = 0; i < 3000; i += 3) {
        snprintf(name, sizeof name, "f%04d", i);
        db_remove(&big, name);
    }
    int ok = big.n == 2000;
    for (int i = 0; i < 3000 && ok; i++) {
        snprintf(name, sizeof name, "f%04d", i);
        db_entry *e = db_find(&big, name);
        ok = (i % 3 == 0) ? e == NULL : (e && strcmp(e->f.name, name) == 0 && e->f.sequence == i + 1);
    }
    CHECK(ok, "name index after puts and removals");
    finfo again = file_entry("f0001", 9999, "1:2");
    db_entry *e = db_put(&big, &again);
    CHECK(big.n == 2000 && e && e->f.sequence == 9999 && db_find(&big, "f0001") == e, "put replaces by name");
    finfo_free(&again);
    db_free(&big);

    db_free(&d);
    remove(path);
    printf("test_db: %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
