/* Version vectors and FileInfo rules against Syncthing: the tables of
 * lib/protocol/vector_test.go (ported by hand), then every case of
 * tests/compat/syncthing-cases.txt, which Syncthing's own code produced
 * (scripts/compat-fixtures.sh).
 *
 *   test_vv CASES-FILE */
#include "../src/bep.h"
#include "../src/finfo.h"
#include "../src/vv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

#define CHECK(cond, ...)                                                                                \
    do {                                                                                                \
        checks++;                                                                                       \
        if (!(cond)) {                                                                                  \
            failures++;                                                                                 \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                 \
            printf(__VA_ARGS__);                                                                        \
            printf("\n");                                                                               \
        }                                                                                               \
    } while (0)

static vv V(const char *s)
{
    vv v;
    if (vv_parse(s, &v) != 0) {
        printf("bad vector literal %s\n", s);
        exit(2);
    }
    return v;
}

static void expect_cmp(const char *a, const char *b, enum vv_order o)
{
    vv x = V(a), y = V(b);
    enum vv_order r = vv_compare(&x, &y);
    CHECK(r == o, "compare %s %s = %d, want %d", a, b, r, o);
    vv_free(&x);
    vv_free(&y);
}

static void expect_vec(const vv *got, const char *want, const char *what)
{
    char g[2048];
    vv_format(got, g, sizeof g);
    CHECK(strcmp(g, want) == 0, "%s: %s, want %s", what, g, want);
}

static void vector_tables(void)
{
    /* TestUpdate */
    vv v = { NULL, 0 };
    vv_update(&v, 42, 5);
    expect_vec(&v, "42:5", "append");
    vv_update(&v, 36, 6);
    expect_vec(&v, "36:6,42:5", "insert at front");
    vv_update(&v, 37, 7);
    expect_vec(&v, "36:6,37:7,42:5", "insert in middle");
    vv_update(&v, 37, 1);
    expect_vec(&v, "36:6,37:8,42:5", "update existing");
    vv_update(&v, 37, 100);
    expect_vec(&v, "36:6,37:100,42:5", "update with higher time");
    vv_update(&v, 37, 50);
    expect_vec(&v, "36:6,37:101,42:5", "update with lower time");
    vv_free(&v);

    /* TestMerge */
    static const char *merge[][3] = {
        { "-", "-", "-" },
        { "22:1,42:1", "22:1,42:1", "22:1,42:1" },
        { "-", "22:1,42:1", "22:1,42:1" },
        { "22:1", "42:1", "22:1,42:1" },
        { "22:1", "22:1,42:1", "22:1,42:1" },
        { "22:1,42:1", "22:1,23:2,42:1", "22:1,23:2,42:1" },
        { "42:1", "22:1", "22:1,42:1" },
        { "22:1,42:2", "22:2,42:1", "22:2,42:2" },
        { "10:1,20:2,30:1", "5:1,10:2,15:1,20:1,25:1,35:1", "5:1,10:2,15:1,20:2,25:1,30:1,35:1" },
    };
    for (size_t i = 0; i < sizeof merge / sizeof *merge; i++) {
        vv a = V(merge[i][0]), b = V(merge[i][1]);
        vv_merge(&a, &b);
        expect_vec(&a, merge[i][2], "merge");
        vv_free(&a);
        vv_free(&b);
    }

    /* TestCounterValue */
    vv c = V("42:1,64:5");
    CHECK(vv_counter_value(&c, 42) == 1 && vv_counter_value(&c, 64) == 5 && vv_counter_value(&c, 72) == 0,
          "counter values");
    vv_free(&c);

    /* TestCompare */
    expect_cmp("-", "-", VV_EQUAL);
    expect_cmp("-", "42:0", VV_EQUAL);
    expect_cmp("42:0", "-", VV_EQUAL);
    expect_cmp("42:0", "77:0", VV_EQUAL);
    expect_cmp("42:33", "42:33", VV_EQUAL);
    expect_cmp("42:33,77:24", "42:33,77:24", VV_EQUAL);
    expect_cmp("42:1", "-", VV_GREATER);
    expect_cmp("0:1", "0:0", VV_GREATER);
    expect_cmp("42:1", "42:0", VV_GREATER);
    expect_cmp("18446744073709551615:1", "18446744073709551615:0", VV_GREATER);
    expect_cmp("0:18446744073709551615", "0:0", VV_GREATER);
    expect_cmp("0:18446744073709551615", "0:18446744073709551614", VV_GREATER);
    expect_cmp("42:2", "42:1", VV_GREATER);
    expect_cmp("22:22,42:2", "22:22,42:1", VV_GREATER);
    expect_cmp("42:2,77:3", "42:1,77:3", VV_GREATER);
    expect_cmp("22:22,42:2,77:3", "22:22,42:1,77:3", VV_GREATER);
    expect_cmp("22:23,42:2,77:4", "22:22,42:1,77:3", VV_GREATER);
    expect_cmp("-", "42:1", VV_LESSER);
    expect_cmp("42:0", "42:1", VV_LESSER);
    expect_cmp("42:1", "42:2", VV_LESSER);
    expect_cmp("22:22,42:1", "22:22,42:2", VV_LESSER);
    expect_cmp("42:1,77:3", "42:2,77:3", VV_LESSER);
    expect_cmp("22:22,42:1,77:3", "22:22,42:2,77:3", VV_LESSER);
    expect_cmp("22:22,42:1,77:3", "22:23,42:2,77:4", VV_LESSER);
    expect_cmp("42:2", "43:1", VV_CONCURRENT_GREATER);
    expect_cmp("43:1", "42:2", VV_CONCURRENT_LESSER);
    expect_cmp("22:23,42:1", "22:22,42:2", VV_CONCURRENT_GREATER);
    expect_cmp("22:21,42:2", "22:22,42:1", VV_CONCURRENT_LESSER);
    expect_cmp("22:21,42:2,43:1", "20:1,22:22,42:1", VV_CONCURRENT_LESSER);
}

static void wire_rules(void)
{
    /* Counters out of order or repeated are refused on decode. */
    unsigned char sorted[] = { 0x0a, 0x04, 0x08, 0x01, 0x10, 0x01, 0x0a, 0x04, 0x08, 0x02, 0x10, 0x01 };
    unsigned char unsorted[] = { 0x0a, 0x04, 0x08, 0x02, 0x10, 0x01, 0x0a, 0x04, 0x08, 0x01, 0x10, 0x01 };
    unsigned char dup[] = { 0x0a, 0x04, 0x08, 0x01, 0x10, 0x01, 0x0a, 0x04, 0x08, 0x01, 0x10, 0x02 };
    vv v;
    CHECK(vv_decode((pb_slice){ sorted, sizeof sorted }, &v) == 0 && v.n == 2, "sorted vector decodes");
    vv_free(&v);
    CHECK(vv_decode((pb_slice){ unsorted, sizeof unsorted }, &v) != 0, "unsorted vector refused");
    CHECK(vv_decode((pb_slice){ dup, sizeof dup }, &v) != 0, "duplicate counter refused");

    /* Our encoding decodes back to the same values. */
    finfo f;
    memset(&f, 0, sizeof f);
    f.name = "dir/x.txt";
    f.type = BEP_FILE;
    f.size = 5;
    f.modified_s = 1794240757;
    f.modified_ns = 1;
    f.modified_by = 0x1234567890abcdefull;
    f.no_permissions = 1;
    f.permissions = 0644;
    f.sequence = 7;
    f.block_size = FINFO_MIN_BLOCK;
    f.version = V("42:3,1311768467294899695:9");
    finfo_block b = { 0, 5, { 1, 2, 3 } };
    f.blocks = &b;
    f.nblocks = 1;
    finfo_blocks_hash(&b, 1, f.blocks_hash);
    f.has_blocks_hash = 1;
    pb_writer w;
    pb_writer_init(&w, 1 << 20);
    CHECK(finfo_encode(&w, &f) == 0, "encode");
    finfo g;
    const char *why;
    CHECK(finfo_decode((pb_slice){ w.data, w.len }, &g, &why) == 0, "decode own encoding: %s", why ? why : "");
    CHECK(strcmp(g.name, f.name) == 0 && g.size == 5 && g.modified_s == f.modified_s && g.modified_ns == 1
              && g.modified_by == f.modified_by && g.no_permissions && g.permissions == 0644 && g.sequence == 7
              && g.block_size == FINFO_MIN_BLOCK && g.nblocks == 1 && g.blocks[0].size == 5
              && memcmp(g.blocks[0].hash, b.hash, 32) == 0 && g.has_blocks_hash && !g.has_prev_blocks_hash
              && vv_compare(&g.version, &f.version) == VV_EQUAL,
          "round trip");
    finfo_free(&g);
    pb_writer_free(&w);
    vv_free(&f.version);

    /* A deletion as SetDeleted makes it. */
    finfo d;
    memset(&d, 0, sizeof d);
    d.name = strdup("gone");
    d.size = 10;
    d.modified_ns = 5;
    d.version = V("42:1");
    d.blocks = calloc(1, sizeof *d.blocks);
    d.nblocks = 1;
    d.has_blocks_hash = 1;
    CHECK(finfo_set_deleted(&d, 43, 2000) == 0, "set deleted");
    expect_vec(&d.version, "42:1,43:2000", "deletion version");
    CHECK(d.deleted && d.modified_s == 2000 && d.modified_ns == 5 && d.modified_by == 43 && d.size == 0
              && d.nblocks == 0 && !d.has_blocks_hash,
          "deletion fields");
    finfo_free(&d);
}

static int unhex(const char *s, unsigned char *out, size_t cap)
{
    size_t n = strlen(s);
    if (strcmp(s, "-") == 0)
        return 0;
    if (n % 2 || n / 2 > cap)
        return -1;
    for (size_t i = 0; i < n / 2; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1)
            return -1;
        out[i] = (unsigned char)v;
    }
    return (int)(n / 2);
}

/* "inv/modS/modNs/vector" */
static void parse_win(char *s, finfo *f)
{
    memset(f, 0, sizeof *f);
    char *p[4];
    for (int i = 0; i < 4; i++)
        p[i] = strsep(&s, "/");
    f->invalid = atoi(p[0]);
    f->modified_s = atoll(p[1]);
    f->modified_ns = atoi(p[2]);
    f->version = V(p[3]);
}

/* "vector/blocksHash/prevBlocksHash" */
static void parse_icw(char *s, finfo *f)
{
    memset(f, 0, sizeof *f);
    char *v = strsep(&s, "/"), *h = strsep(&s, "/"), *ph = strsep(&s, "/");
    f->version = V(v);
    f->has_blocks_hash = unhex(h, f->blocks_hash, 32) == 32;
    f->has_prev_blocks_hash = unhex(ph, f->prev_blocks_hash, 32) == 32;
}

static void fixture(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        printf("FAIL cannot open %s\n", path);
        failures++;
        return;
    }
    char line[16384], version[64] = "?";
    long n = 0;
    while (fgets(line, sizeof line, fp)) {
        line[strcspn(line, "\n")] = 0;
        char *t[20] = { 0 }, *s = line;
        int k = 0;
        while (k < 20 && (t[k] = strsep(&s, " ")) != NULL)
            k++;
        n++;
        if (strcmp(t[0], "version") == 0) {
            snprintf(version, sizeof version, "%s", t[1]);
        } else if (strcmp(t[0], "cmp") == 0) {
            expect_cmp(t[1], t[2], (enum vv_order)atoi(t[3]));
        } else if (strcmp(t[0], "upd") == 0) {
            vv v = V(t[1]);
            vv_update(&v, strtoull(t[2], NULL, 10), strtoull(t[3], NULL, 10));
            expect_vec(&v, t[4], "update");
            vv_free(&v);
        } else if (strcmp(t[0], "mrg") == 0) {
            vv a = V(t[1]), b = V(t[2]);
            vv_merge(&a, &b);
            expect_vec(&a, t[3], "merge");
            vv_free(&a);
            vv_free(&b);
        } else if (strcmp(t[0], "win") == 0) {
            finfo a, b;
            parse_win(t[1], &a);
            parse_win(t[2], &b);
            CHECK(finfo_wins_conflict(&a, &b) == atoi(t[3]), "wins_conflict line %ld", n);
            vv_free(&a.version);
            vv_free(&b.version);
        } else if (strcmp(t[0], "icw") == 0) {
            finfo a, b;
            parse_icw(t[1], &a);
            parse_icw(t[2], &b);
            CHECK(finfo_in_conflict_with(&a, &b) == atoi(t[3]), "in_conflict_with line %ld", n);
            vv_free(&a.version);
            vv_free(&b.version);
        } else if (strcmp(t[0], "sid") == 0) {
            char txt[8];
            vv_short_id_text(strtoull(t[1], NULL, 10), txt);
            CHECK(strcmp(txt, t[2] ? t[2] : "") == 0, "short id text %s: %s, want %s", t[1], txt, t[2]);
        } else if (strcmp(t[0], "bsz") == 0) {
            CHECK(finfo_block_size_for(atoll(t[1])) == atoi(t[2]), "block size for %s", t[1]);
        } else if (strcmp(t[0], "bh") == 0) {
            finfo_block b[8];
            size_t nb = 0;
            if (strcmp(t[1], "-") != 0)
                for (char *q = t[1], *h; (h = strsep(&q, ",")) != NULL && nb < 8; nb++)
                    unhex(h, b[nb].hash, 32);
            unsigned char want[32], got[32];
            unhex(t[2], want, 32);
            finfo_blocks_hash(b, nb, got);
            CHECK(memcmp(got, want, 32) == 0, "blocks hash line %ld", n);
        } else if (strcmp(t[0], "fi") == 0) {
            static unsigned char raw[8192];
            int rl = unhex(t[1], raw, sizeof raw);
            finfo f;
            const char *why;
            int rc = finfo_decode((pb_slice){ raw, (size_t)rl }, &f, &why);
            CHECK(rc == 0, "decode Syncthing FileInfo line %ld: %s", n, why ? why : "");
            if (rc == 0) {
                char name[100];
                snprintf(name, sizeof name, "%s", t[2]);
                for (char *q; (q = strstr(name, "%20")) != NULL;)
                    memmove(q + 1, q + 3, strlen(q + 3) + 1), *q = ' ';
                char vec[2048];
                vv_format(&f.version, vec, sizeof vec);
                unsigned char bh[32], pbh[32];
                int hb = unhex(t[16], bh, 32) == 32, hp = unhex(t[17], pbh, 32) == 32;
                CHECK(strcmp(f.name, name) == 0 && f.type == (uint32_t)atoi(t[3]) && f.size == atoll(t[4])
                          && f.modified_s == atoll(t[5]) && f.modified_ns == atoi(t[6])
                          && f.modified_by == strtoull(t[7], NULL, 10) && f.deleted == atoi(t[8])
                          && f.invalid == atoi(t[9]) && f.no_permissions == atoi(t[10])
                          && f.permissions == (uint32_t)atoi(t[11]) && f.sequence == atoll(t[12])
                          && f.block_size == atoi(t[13]) && strcmp(vec, t[14]) == 0
                          && f.nblocks == (size_t)atoi(t[15]) && f.has_blocks_hash == hb
                          && (!hb || memcmp(f.blocks_hash, bh, 32) == 0) && f.has_prev_blocks_hash == hp
                          && (!hp || memcmp(f.prev_blocks_hash, pbh, 32) == 0),
                      "FileInfo fields line %ld", n);
                /* Re-encoded by us, it decodes to the same values. */
                pb_writer w;
                pb_writer_init(&w, 1 << 20);
                finfo g;
                CHECK(finfo_encode(&w, &f) == 0 && finfo_decode((pb_slice){ w.data, w.len }, &g, &why) == 0,
                      "re-encode line %ld", n);
                if (!w.error) {
                    char gv[2048];
                    vv_format(&g.version, gv, sizeof gv);
                    CHECK(strcmp(gv, vec) == 0 && g.nblocks == f.nblocks && g.size == f.size
                              && g.modified_by == f.modified_by && g.sequence == f.sequence,
                          "re-encoded fields line %ld", n);
                    finfo_free(&g);
                }
                pb_writer_free(&w);
                finfo_free(&f);
            }
        } else if (strcmp(t[0], "cfn") == 0) {
            struct tm tm;
            memset(&tm, 0, sizeof tm);
            sscanf(t[2], "%4d%2d%2d%2d%2d%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min,
                   &tm.tm_sec);
            tm.tm_year -= 1900;
            tm.tm_mon -= 1;
            char out[512];
            finfo_conflict_name(t[1], &tm, strtoull(t[3], NULL, 10), out, sizeof out);
            CHECK(strcmp(out, t[4]) == 0, "conflict name %s: %s, want %s", t[1], out, t[4]);
            CHECK(finfo_is_conflict_name(out) && !finfo_is_conflict_name(t[1]), "is conflict name %s", out);
        } else {
            CHECK(0, "unknown fixture line %ld: %s", n, t[0]);
        }
    }
    fclose(fp);
    printf("test_vv: %ld fixture lines from Syncthing %s\n", n, version);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: test_vv CASES-FILE\n");
        return 2;
    }
    vector_tables();
    wire_rules();
    fixture(argv[1]);
    printf("test_vv: %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
