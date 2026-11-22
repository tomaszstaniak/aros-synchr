/* Host tests for the protobuf reader, BEP message codecs and framing.
 * Compressed frames use the reference LZ4 compressor (liblz4). */
#include "../src/bep.h"
#include "../src/frame.h"
#include "../src/pb.h"

#include <lz4.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int slice_is(pb_slice s, const char *t)
{
    return s.len == strlen(t) && memcmp(s.data, t, s.len) == 0;
}

/* ---- protobuf reader ---- */

static void test_pb(void)
{
    pb_reader r;
    uint64_t v;
    uint32_t f;
    int w;

    const unsigned char max[] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01 };
    pb_reader_init(&r, max, sizeof max);
    CHECK(pb_varint(&r, &v) == 0 && v == UINT64_MAX, "max varint");

    const unsigned char over[] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02 };
    pb_reader_init(&r, over, sizeof over);
    CHECK(pb_varint(&r, &v) != 0, "varint above 64 bits accepted");

    const unsigned char eleven[] = { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00 };
    pb_reader_init(&r, eleven, sizeof eleven);
    CHECK(pb_varint(&r, &v) != 0, "11-byte varint accepted");

    const unsigned char trunc[] = { 0x80 };
    pb_reader_init(&r, trunc, sizeof trunc);
    CHECK(pb_varint(&r, &v) != 0, "truncated varint accepted");

    const unsigned char group[] = { 0x0b }; /* field 1, wire type 3 */
    pb_reader_init(&r, group, sizeof group);
    CHECK(pb_next(&r, &f, &w) == -1, "group wire type accepted");

    const unsigned char field0[] = { 0x00, 0x01 };
    pb_reader_init(&r, field0, sizeof field0);
    CHECK(pb_next(&r, &f, &w) == -1, "field number 0 accepted");

    const unsigned char longlen[] = { 0x0a, 0x05, 'a', 'b' }; /* says 5, has 2 */
    pb_slice s;
    pb_reader_init(&r, longlen, sizeof longlen);
    CHECK(pb_next(&r, &f, &w) == 1 && pb_bytes(&r, &s) != 0, "length past the end accepted");
    CHECK(pb_next(&r, &f, &w) == -1, "error not sticky");
}

/* ---- message codecs ---- */

static void put_file(pb_writer *w, const char *name, uint32_t type, int64_t size,
                     int64_t seq, int with_unknown)
{
    pb_writer fi, ver, cnt, blk;
    pb_writer_init(&fi, 1 << 20);
    pb_put_string_field(&fi, 1, name);
    pb_put_varint_field(&fi, 2, type);
    pb_put_varint_field(&fi, 3, (uint64_t)size);
    pb_put_varint_field(&fi, 5, 1700000000);
    pb_writer_init(&ver, 1 << 20);
    pb_writer_init(&cnt, 1 << 20);
    pb_put_varint_field(&cnt, 1, 0x1234);
    pb_put_varint_field(&cnt, 2, 3);
    pb_put_message_field(&ver, 1, &cnt);
    pb_put_message_field(&fi, 9, &ver);
    pb_put_varint_field(&fi, 10, (uint64_t)seq);
    if (size > 0) {
        pb_writer_init(&blk, 1 << 20);
        pb_put_varint_field(&blk, 2, (uint64_t)size);
        pb_put_message_field(&fi, 16, &blk);
        pb_writer_free(&blk);
    }
    if (with_unknown) {
        /* fields this decoder does not know, one of each wire type */
        const unsigned char unk[] = {
            0xb8, 0x3e, 0x07,                               /* 999 varint */
            0xc1, 0x3e, 1, 2, 3, 4, 5, 6, 7, 8,             /* 1000 fixed64 */
            0xcd, 0x3e, 1, 2, 3, 4,                         /* 1001 fixed32 */
            0xd2, 0x3e, 0x02, 'z', 'z'                      /* 1002 bytes */
        };
        pb_put_raw(&fi, unk, sizeof unk);
    }
    pb_put_message_field(w, 2, &fi);
    pb_writer_free(&fi);
    pb_writer_free(&ver);
    pb_writer_free(&cnt);
}

struct files_seen {
    int n;
    char names[8][64];
    int64_t sizes[8];
    uint32_t types[8];
    size_t blocks[8], counters[8];
};

static int on_file(void *ctx, const bep_file *f)
{
    struct files_seen *s = ctx;
    if (s->n < 8) {
        snprintf(s->names[s->n], 64, "%.*s", (int)f->name.len, (const char *)f->name.data);
        s->sizes[s->n] = f->size;
        s->types[s->n] = f->type;
        s->blocks[s->n] = f->blocks;
        s->counters[s->n] = f->version_counters;
    }
    s->n++;
    return 0;
}

static void test_messages(void)
{
    pb_writer w;
    bep_hello h;

    pb_writer_init(&w, 1 << 20);
    bep_encode_hello(&w, "aros", "aros-syncthing", "0.0.1", 1234567);
    CHECK(bep_decode_hello(w.data, w.len, &h) == 0 && slice_is(h.device_name, "aros")
              && slice_is(h.client_name, "aros-syncthing") && h.timestamp == 1234567,
          "Hello round trip");
    pb_writer_free(&w);

    /* Index: two files, one carrying unknown fields of every wire type */
    pb_writer_init(&w, 1 << 20);
    pb_put_string_field(&w, 1, "aros-poc-1");
    put_file(&w, "empty.txt", BEP_FILE, 0, 1, 1);
    put_file(&w, "sub/nested.txt", BEP_FILE, 12, 2, 0);
    pb_put_varint_field(&w, 3, 2);
    struct files_seen fs = { 0 };
    pb_slice folder;
    int64_t last, prev;
    CHECK(bep_decode_index(w.data, w.len, &folder, &last, &prev, &fs, on_file) == 0, "Index decode");
    CHECK(slice_is(folder, "aros-poc-1") && last == 2 && fs.n == 2, "Index fields");
    CHECK(strcmp(fs.names[0], "empty.txt") == 0 && fs.sizes[0] == 0 && fs.blocks[0] == 0
              && fs.counters[0] == 1,
          "first file");
    CHECK(strcmp(fs.names[1], "sub/nested.txt") == 0 && fs.sizes[1] == 12 && fs.blocks[1] == 1,
          "second file");

    /* every truncation of a valid message is rejected with no file reported */
    for (size_t cut = 1; cut < w.len; cut++) {
        struct files_seen t = { 0 };
        int rc = bep_decode_index(w.data, w.len - cut, &folder, &last, &prev, &t, on_file);
        CHECK(rc != 0 ? t.n == 0 : 1, "truncation by %zu reported files", cut);
    }
    pb_writer_free(&w);

    /* a known field with the wrong wire type: name (1) as varint */
    pb_writer_init(&w, 1 << 20);
    pb_put_string_field(&w, 1, "f");
    pb_writer fi;
    pb_writer_init(&fi, 1 << 20);
    pb_put_varint_field(&fi, 1, 5);
    pb_put_message_field(&w, 2, &fi);
    struct files_seen bad = { 0 };
    CHECK(bep_decode_index(w.data, w.len, &folder, &last, &prev, &bad, on_file) != 0 && bad.n == 0,
          "wrong wire type accepted");
    pb_writer_free(&fi);
    pb_writer_free(&w);

    /* the empty Index we send */
    pb_writer_init(&w, 1 << 20);
    bep_encode_empty_index(&w, "aros-poc-1");
    struct files_seen none = { 0 };
    CHECK(bep_decode_index(w.data, w.len, &folder, &last, &prev, &none, on_file) == 0
              && slice_is(folder, "aros-poc-1") && none.n == 0 && last == 0,
          "empty Index");
    pb_writer_free(&w);
}

/* ---- ClusterConfig decode, with callbacks ---- */

struct cc_seen {
    int folders, devices, idok;
    uint32_t type;
    uint64_t index_id;
};

static int cc_folder(void *ctx, const bep_cc_folder *f)
{
    struct cc_seen *s = ctx;
    s->folders++;
    s->type = f->type;
    return slice_is(f->id, "aros-poc-1") ? 0 : 7;
}

static int cc_device(void *ctx, const bep_cc_folder *f, const bep_cc_device *d)
{
    struct cc_seen *s = ctx;
    (void)f;
    s->devices++;
    s->idok += d->id_valid;
    if (d->index_id)
        s->index_id = d->index_id;
    return 0;
}

static void test_cluster_config(void)
{
    unsigned char a[DEVID_BYTES], b[DEVID_BYTES];
    memset(a, 0xaa, sizeof a);
    memset(b, 0xbb, sizeof b);
    bep_cc_device_out devs[2] = { { a, "peer", 0, 0 }, { b, "aros", 0, 0x0102030405060708ull } };
    pb_writer w;
    pb_writer_init(&w, 1 << 20);
    bep_encode_cluster_config(&w, "aros-poc-1", "", BEP_FOLDER_RECEIVE_ONLY, devs, 2);
    struct cc_seen s = { 0 };
    CHECK(bep_decode_cluster_config(w.data, w.len, &s, cc_folder, cc_device) == 0, "CC decode");
    CHECK(s.folders == 1 && s.devices == 2 && s.idok == 2 && s.type == BEP_FOLDER_RECEIVE_ONLY
              && s.index_id == 0x0102030405060708ull,
          "CC fields: folders %d devices %d ids %d", s.folders, s.devices, s.idok);
    pb_writer_free(&w);
}

/* ---- framing ---- */

static void put_compressed(pb_writer *w, uint32_t type, const pb_writer *body, int size_delta)
{
    int bound = LZ4_compressBound((int)body->len);
    unsigned char *c = malloc((size_t)bound + 4);
    int cl = LZ4_compress_default((const char *)body->data, (char *)c + 4, (int)body->len, bound);
    uint32_t size = (uint32_t)((int)body->len + size_delta);
    c[0] = (unsigned char)(size >> 24);
    c[1] = (unsigned char)(size >> 16);
    c[2] = (unsigned char)(size >> 8);
    c[3] = (unsigned char)size;
    pb_writer hdr;
    pb_writer_init(&hdr, 64);
    bep_encode_header(&hdr, type, BEP_COMPRESSION_LZ4);
    unsigned char be[4] = { 0, 0, 0, (unsigned char)hdr.len };
    pb_put_raw(w, be + 2, 2);
    pb_put_raw(w, hdr.data, hdr.len);
    uint32_t ml = (uint32_t)cl + 4;
    unsigned char mlb[4] = { (unsigned char)(ml >> 24), (unsigned char)(ml >> 16),
                             (unsigned char)(ml >> 8), (unsigned char)ml };
    pb_put_raw(w, mlb, 4);
    pb_put_raw(w, c, ml);
    pb_writer_free(&hdr);
    free(c);
}

static void build_stream(pb_writer *s, int size_delta)
{
    pb_writer h, cc, idx, ping;
    pb_writer_init(&h, 1 << 20);
    bep_encode_hello(&h, "mac", "syncthing", "v2.1.2", 1);
    frame_put_hello(s, &h);
    unsigned char a[DEVID_BYTES] = { 0 };
    bep_cc_device_out d = { a, "x", 0, 0 };
    pb_writer_init(&cc, 1 << 20);
    bep_encode_cluster_config(&cc, "aros-poc-1", "", 0, &d, 1);
    frame_put_message(s, BEP_CLUSTER_CONFIG, &cc);
    pb_writer_init(&idx, 1 << 20);
    pb_put_string_field(&idx, 1, "aros-poc-1");
    for (int i = 0; i < 50; i++)
        put_file(&idx, "some/fairly/repetitive/path/name.txt", BEP_FILE, i, i + 1, i % 2);
    put_compressed(s, BEP_INDEX, &idx, size_delta);
    pb_writer_init(&ping, 16);
    frame_put_message(s, BEP_PING, &ping);
    pb_writer_free(&h);
    pb_writer_free(&cc);
    pb_writer_free(&idx);
    pb_writer_free(&ping);
}

/* Feeds the stream in pieces of `step` bytes; returns the sequence of
 * frame results as letters (H hello, C cc, I index, P ping, E error). */
static void run_stream(const pb_writer *s, size_t step, char *out, size_t max_msg)
{
    frame_rx rx;
    size_t o = 0;
    frame_rx_init(&rx, max_msg, 1 << 20);
    for (size_t off = 0; off < s->len && o < 15; off += step) {
        size_t n = s->len - off < step ? s->len - off : step;
        if (frame_rx_feed(&rx, s->data + off, n) != 0) {
            out[o++] = 'F';
            break;
        }
        enum frame_status st;
        while ((st = frame_rx_next(&rx)) != FRAME_NEED_MORE && o < 15) {
            if (st == FRAME_HELLO) {
                bep_hello h;
                out[o++] = bep_decode_hello(rx.body, rx.body_len, &h) == 0 ? 'H' : 'h';
            } else if (st == FRAME_MESSAGE) {
                if (rx.type == BEP_INDEX) {
                    pb_slice f;
                    int64_t l, p;
                    struct files_seen fs = { 0 };
                    int ok = bep_decode_index(rx.body, rx.body_len, &f, &l, &p, &fs, on_file) == 0
                             && fs.n == 50;
                    out[o++] = ok ? 'I' : 'i';
                } else {
                    out[o++] = "CIUQRDPX"[rx.type & 7];
                }
            } else {
                out[o++] = 'E';
                goto done;
            }
        }
    }
done:
    out[o] = 0;
    frame_rx_free(&rx);
}

static void test_frames(void)
{
    pb_writer s;
    char got[16];

    pb_writer_init(&s, 1 << 22);
    build_stream(&s, 0);
    run_stream(&s, s.len, got, 1 << 20);
    CHECK(strcmp(got, "HCIP") == 0, "whole stream: %s", got);
    run_stream(&s, 1, got, 1 << 20);
    CHECK(strcmp(got, "HCIP") == 0, "byte by byte: %s", got);
    run_stream(&s, 7, got, 1 << 20);
    CHECK(strcmp(got, "HCIP") == 0, "7-byte pieces: %s", got);
    run_stream(&s, s.len, got, 100); /* index frame larger than the limit */
    CHECK(strcmp(got, "HCE") == 0, "message limit: %s", got);
    pb_writer_free(&s);

    pb_writer_init(&s, 1 << 22);
    build_stream(&s, 1); /* size prefix one larger than the data */
    run_stream(&s, s.len, got, 1 << 20);
    CHECK(strcmp(got, "HCE") == 0, "LZ4 size mismatch: %s", got);
    pb_writer_free(&s);

    const struct { const char *name; unsigned char bytes[12]; size_t len; const char *want; } bad[] = {
        { "bad magic", { 0xde, 0xad, 0xbe, 0xef, 0, 0 }, 6, "E" },
        { "old magic", { 0x9f, 0x79, 0xbc, 0x40, 0, 0 }, 6, "E" },
        { "Hello too large", { 0x2e, 0xa7, 0xd9, 0x0b, 0x80, 0x00 }, 6, "E" },
        { "header too long", { 0x2e, 0xa7, 0xd9, 0x0b, 0, 0, 0x80, 0x00, 0, 0, 0, 0 }, 12, "HE" },
        { "unknown compression", { 0x2e, 0xa7, 0xd9, 0x0b, 0, 0, 0, 2, 0x10, 0x02, 0, 0 }, 12, "H" },
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        pb_writer b;
        pb_writer_init(&b, 64);
        pb_put_raw(&b, bad[i].bytes, bad[i].len);
        if (i == 4) { /* complete the frame: 4-byte length 0 */
            const unsigned char z[2] = { 0, 0 };
            pb_put_raw(&b, z, 2);
        }
        run_stream(&b, b.len, got, 1 << 20);
        const char *want = i == 4 ? "HE" : bad[i].want;
        CHECK(strcmp(got, want) == 0, "%s: %s", bad[i].name, got);
        pb_writer_free(&b);
    }
}

/* ---- transfer messages ---- */

static int blk_count;
static int64_t blk_total;
static int on_block(void *ctx, int64_t offset, int32_t size, pb_slice hash)
{
    (void)ctx;
    (void)offset;
    blk_count++;
    blk_total += size;
    return hash.len == 0 ? 0 : 0;
}

struct one_file { pb_slice raw; int64_t seq; char name[64]; size_t blocks; };
static int grab(void *ctx, const bep_file *f)
{
    struct one_file *o = ctx;
    o->raw = f->raw;
    o->seq = f->sequence;
    o->blocks = f->blocks;
    snprintf(o->name, sizeof o->name, "%.*s", (int)f->name.len, (const char *)f->name.data);
    return 0;
}

static void test_transfer(void)
{
    pb_writer idx, out;
    pb_slice folder;
    int64_t last, prev;

    /* a file with one block and unknown/local fields, as the peer sends it */
    pb_writer_init(&idx, 1 << 20);
    pb_put_string_field(&idx, 1, "f");
    put_file(&idx, "dir/data.bin", BEP_FILE, 12, 41, 1);
    struct one_file got = { 0 };
    CHECK(bep_decode_index(idx.data, idx.len, &folder, &last, &prev, &got, grab) == 0, "decode");
    blk_count = 0;
    blk_total = 0;
    CHECK(bep_file_blocks(got.raw, NULL, on_block) == 0 && blk_count == 1 && blk_total == 12, "blocks");

    /* announced copy: same file, our sequence, fields >= 1000 dropped */
    pb_writer_init(&out, 1 << 20);
    bep_encode_index_update_start(&out, "f", 7, 6);
    CHECK(bep_put_announced_file(&out, got.raw, 7) == 0, "announce encode");
    struct one_file back = { 0 };
    CHECK(bep_decode_index(out.data, out.len, &folder, &last, &prev, &back, grab) == 0, "announce decode");
    CHECK(back.seq == 7 && last == 7 && prev == 6 && strcmp(back.name, "dir/data.bin") == 0
              && back.blocks == 1,
          "announced fields: seq %lld last %lld prev %lld", (long long)back.seq, (long long)last,
          (long long)prev);
    CHECK(back.raw.len < got.raw.len, "local/unknown fields >= 1000 were not dropped");
    pb_writer_free(&out);
    pb_writer_free(&idx);

    /* Request fields as Syncthing reads them; Response decoding */
    unsigned char hash[32];
    memset(hash, 0x5a, sizeof hash);
    pb_writer_init(&out, 4096);
    bep_encode_request(&out, 9, "f", "dir/data.bin", 131072, 131072, hash, 32, 1);
    int32_t id;
    pb_slice rf, rn;
    CHECK(bep_decode_request(out.data, out.len, &id, &rf, &rn) == 0 && id == 9 && slice_is(rn, "dir/data.bin"),
          "request round trip");
    pb_writer_free(&out);

    pb_writer_init(&out, 4096);
    pb_put_varint_field(&out, 1, 9);
    pb_put_bytes_field(&out, 2, "abc", 3);
    pb_slice data;
    uint32_t code;
    CHECK(bep_decode_response(out.data, out.len, &id, &data, &code) == 0 && id == 9 && data.len == 3
              && code == 0,
          "response decode");
    pb_writer_free(&out);
    pb_writer_init(&out, 4096);
    bep_encode_response_error(&out, -3, 2);
    CHECK(bep_decode_response(out.data, out.len, &id, &data, &code) == 0 && id == -3 && code == 2
              && data.len == 0,
          "negative id / error code");
    pb_writer_free(&out);
}

int main(void)
{
    test_pb();
    test_messages();
    test_cluster_config();
    test_frames();
    test_transfer();
    printf("%s: test_bep\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
