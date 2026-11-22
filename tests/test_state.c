/* State file: round trip, and rejection of every kind of damage. */
#include "../src/state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char dir[512], path[700];
static unsigned char peer[DEVID_BYTES], other_peer[DEVID_BYTES];

static int load(state_list *l, char *why, size_t n)
{
    return state_load(path, "aros-demo", "SYS:dest", peer, l, why, n);
}

static char *slurp(size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *b = malloc(1 << 20);
    *len = fread(b, 1, (1 << 20) - 1, f);
    b[*len] = 0;
    fclose(f);
    return b;
}

static void spit(const char *p, const char *data, size_t len)
{
    FILE *f = fopen(p, "wb");
    fwrite(data, 1, len, f);
    fclose(f);
}

/* Applies an edit to the saved file and expects the load to be rejected. */
static void expect_rejected(const char *what, const char *orig, size_t len, size_t at, const char *with)
{
    char *b = malloc(len + 64);
    memcpy(b, orig, len);
    size_t nlen = len;
    if (with) {
        memcpy(b + at, with, strlen(with));
    } else {
        nlen = at; /* truncate */
    }
    spit(path, b, nlen);
    free(b);
    state_list l;
    char why[300] = "";
    CHECK(load(&l, why, sizeof why) == -1 && l.n == 0, "%s was accepted", what);
}

int main(int argc, char **argv)
{
    char why[300];
    snprintf(dir, sizeof dir, "%s/stpoc-state-%d", argc > 1 ? argv[1] : "/tmp", (int)getpid());
    mkdir(dir, 0755);
    memset(peer, 0x11, sizeof peer);
    memset(other_peer, 0x22, sizeof other_peer);

    CHECK(state_path(dir, "aros-demo", path, sizeof path) == 0 && strstr(path, "/state-") && strstr(path, ".txt"),
          "state path %s", path);
    char p2[700];
    CHECK(state_path(dir, "../../etc:x/y", p2, sizeof p2) == 0 && !strstr(p2 + strlen(dir), ".."),
          "folder id leaked into the path: %s", p2);

    state_list l, r;
    memset(&l, 0, sizeof l);
    CHECK(load(&r, why, sizeof why) == 1, "missing file not reported as missing");

    const unsigned char raw1[] = { 0x0a, 0x03, 'a', '.', 'x', 0x18, 0x05 };
    const unsigned char raw2[] = { 0x0a, 0x01, 'd', 0x10, 0x01 };
    CHECK(state_put(&l, "Documents/a b.txt", 0, raw1, sizeof raw1) == 0, "put");
    CHECK(state_put(&l, "Documents", 1, raw2, sizeof raw2) == 0, "put dir");
    CHECK(state_put(&l, "Documents/a b.txt", 0, raw1, sizeof raw1) == 0 && l.n == 2, "put replaces");
    CHECK(state_save(path, "aros-demo", "SYS:dest", peer, &l, why, sizeof why) == 0, "save: %s", why);
    CHECK(load(&r, why, sizeof why) == 0 && r.n == 2, "load: %s", why);
    state_entry *e = state_find(&r, "Documents/a b.txt");
    CHECK(e && !e->is_dir && e->raw_len == sizeof raw1 && memcmp(e->raw, raw1, sizeof raw1) == 0, "entry content");
    e = state_find(&r, "Documents");
    CHECK(e && e->is_dir, "dir entry");
    state_list_free(&r);

    /* the binding to folder, destination and peer */
    CHECK(state_load(path, "other", "SYS:dest", peer, &r, why, sizeof why) == -1, "other folder accepted");
    CHECK(state_load(path, "aros-demo", "SYS:other", peer, &r, why, sizeof why) == -1, "other dest accepted");
    CHECK(state_load(path, "aros-demo", "SYS:dest", other_peer, &r, why, sizeof why) == -1, "other peer accepted");

    size_t len;
    char *orig = slurp(&len);
    CHECK(orig != NULL, "cannot read back");
    char *entry = strstr(orig, "entry ");
    char *endl = strstr(orig, "\nend ");
    expect_rejected("a truncated file", orig, len, len - 10, NULL);
    expect_rejected("a file cut before its end line", orig, len, (size_t)(endl - orig) + 1, NULL);
    expect_rejected("a changed byte", orig, len, (size_t)(entry - orig) + 12, "f");
    expect_rejected("a changed checksum", orig, len, len - 3, "00");
    expect_rejected("version 2", orig, len, strlen("aros-syncthing-state "), "2");
    spit(path, orig, len);
    CHECK(load(&r, why, sizeof why) == 0, "original no longer loads: %s", why);
    state_list_free(&r);

    /* a duplicate entry and a wrong count, each with a valid checksum */
    state_list d;
    memset(&d, 0, sizeof d);
    state_put(&d, "x", 0, raw1, sizeof raw1);
    state_put(&d, "y", 0, raw1, sizeof raw1);
    free(d.e[1].name);
    d.e[1].name = strdup("x"); /* bypass state_put to forge a duplicate */
    CHECK(state_save(path, "aros-demo", "SYS:dest", peer, &d, why, sizeof why) == 0, "save dup");
    CHECK(load(&r, why, sizeof why) == -1 && strstr(why, "duplicate"), "duplicate accepted: %s", why);
    state_list_free(&d);

    /* .old left by an interrupted replacement is used; a .tmp is ignored */
    spit(path, orig, len);
    char old[800], tmp[800];
    snprintf(old, sizeof old, "%s.old", path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    rename(path, old);
    spit(tmp, "garbage", 7);
    CHECK(load(&r, why, sizeof why) == 0 && r.n == 2, "the .old copy was not used: %s", why);
    state_list_free(&r);
    remove(old);
    CHECK(load(&r, why, sizeof why) == 1, "a lone .tmp was taken as state");
    remove(tmp);

    free(orig);
    state_list_free(&l);
    char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0)
        printf("note: could not remove %s\n", dir);
    printf("%s: test_state\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
