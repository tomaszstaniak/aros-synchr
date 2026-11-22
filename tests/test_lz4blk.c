/* LZ4 block decoder against the reference compressor (liblz4, host only),
 * plus hand-made malformed blocks. */
#include "../src/lz4blk.h"

#include <lz4.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void roundtrip(const char *what, const unsigned char *in, size_t n)
{
    int bound = LZ4_compressBound((int)n);
    unsigned char *c = malloc((size_t)bound + 1), *d = malloc(n + 1);
    int cl = LZ4_compress_default((const char *)in, (char *)c, (int)n, bound);
    CHECK(cl > 0 || n == 0, "%s: reference compression failed", what);
    long dl = lz4_block_decode(c, (size_t)cl, d, n);
    CHECK(dl == (long)n && memcmp(d, in, n) == 0, "%s: decoded %ld of %zu", what, dl, n);
    if (n > 0) /* one byte short of room must be refused, not truncated */
        CHECK(lz4_block_decode(c, (size_t)cl, d, n - 1) == -1, "%s: overflow not detected", what);
    if (cl > 1) /* every truncation must be refused */
        for (int cut = 1; cut < cl; cut += (cl > 200 ? cl / 50 : 1))
            CHECK(lz4_block_decode(c, (size_t)(cl - cut), d, n) != (long)n
                      || memcmp(d, in, n) != 0,
                  "%s: truncation by %d accepted", what, cut);
    free(c);
    free(d);
}

int main(void)
{
    unsigned char buf[300000];
    srand(1);
    roundtrip("one byte", (const unsigned char *)"x", 1);
    roundtrip("text", (const unsigned char *)"hello hello hello hello hello world", 35);
    memset(buf, 'A', sizeof buf);
    roundtrip("run of 300000", buf, sizeof buf);
    for (size_t i = 0; i < sizeof buf; i++)
        buf[i] = (unsigned char)rand();
    roundtrip("random 300000", buf, sizeof buf);
    for (size_t i = 0; i < sizeof buf; i++)
        buf[i] = (unsigned char)("abcdefgh"[i % 8] ^ (i % 977 == 0));
    roundtrip("pattern", buf, sizeof buf);

    unsigned char out[64];
    /* token: 1 literal, match follows with offset 0 */
    const unsigned char off0[] = { 0x10, 'a', 0x00, 0x00, 0x00 };
    CHECK(lz4_block_decode(off0, sizeof off0, out, sizeof out) == -1, "offset 0 accepted");
    /* offset reaching before the start of the output */
    const unsigned char before[] = { 0x10, 'a', 0x02, 0x00, 0x00 };
    CHECK(lz4_block_decode(before, sizeof before, out, sizeof out) == -1, "offset before start accepted");
    /* literal length larger than the input */
    const unsigned char litover[] = { 0x50, 'a', 'b' };
    CHECK(lz4_block_decode(litover, sizeof litover, out, sizeof out) == -1, "literal overrun accepted");
    /* extension bytes running off the end */
    const unsigned char extend[] = { 0xf0, 0xff, 0xff };
    CHECK(lz4_block_decode(extend, sizeof extend, out, sizeof out) == -1, "open extension accepted");
    /* match longer than the output room */
    const unsigned char mover[] = { 0x1f, 'a', 0x01, 0x00, 0xff, 0x40, 0x00 };
    CHECK(lz4_block_decode(mover, sizeof mover, out, sizeof out) == -1, "match overrun accepted");
    /* empty input */
    CHECK(lz4_block_decode(out, 0, out, sizeof out) == -1, "empty block accepted");

    printf("%s: test_lz4blk\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
