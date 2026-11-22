#include "frame.h"

#include "bep.h"
#include "lz4blk.h"

#include <stdlib.h>
#include <string.h>

/* Header protobufs are a few bytes; Syncthing reads the length as int16. */
#define MAX_HEADER 32767

void frame_rx_init(frame_rx *rx, size_t max_message, size_t max_decompressed)
{
    memset(rx, 0, sizeof *rx);
    rx->max_message = max_message;
    rx->max_decompressed = max_decompressed;
}

void frame_rx_free(frame_rx *rx)
{
    free(rx->buf);
    free(rx->out);
    memset(rx, 0, sizeof *rx);
}

static void drop_consumed(frame_rx *rx)
{
    if (rx->consume) {
        memmove(rx->buf, rx->buf + rx->consume, rx->len - rx->consume);
        rx->len -= rx->consume;
        rx->consume = 0;
    }
}

int frame_rx_feed(frame_rx *rx, const void *data, size_t len)
{
    drop_consumed(rx);
    /* The caller drains frame_rx_next() before feeding again, so what is
     * buffered is at most one incomplete frame plus one read of at most
     * FRAME_FEED_MAX bytes. More than that means a frame was not consumed. */
    size_t limit = rx->max_message + MAX_HEADER + 6 + FRAME_FEED_MAX;
    if (len > limit - rx->len)
        return -1;
    if (rx->len + len > rx->cap) {
        size_t cap = rx->cap ? rx->cap : 16384;
        while (cap < rx->len + len)
            cap *= 2;
        if (cap > limit)
            cap = limit;
        unsigned char *b = realloc(rx->buf, cap);
        if (!b)
            return -1;
        rx->buf = b;
        rx->cap = cap;
    }
    memcpy(rx->buf + rx->len, data, len);
    rx->len += len;
    return 0;
}

static uint32_t be16(const unsigned char *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static enum frame_status fail(frame_rx *rx, const char *why)
{
    rx->error = why;
    return FRAME_ERROR;
}

enum frame_status frame_rx_next(frame_rx *rx)
{
    if (rx->error)
        return FRAME_ERROR;
    drop_consumed(rx);
    const unsigned char *p = rx->buf;
    size_t n = rx->len;

    if (!rx->got_hello) {
        if (n < 4)
            return FRAME_NEED_MORE;
        uint32_t magic = be32(p);
        if (magic != BEP_HELLO_MAGIC) {
            /* Syncthing's own reader names the pre-0.14 magics separately. */
            if (magic == 0x00010001 || magic == 0x00010000 || magic == 0x9F79BC40)
                return fail(rx, "peer speaks an older, incompatible protocol version");
            return fail(rx, "unknown Hello magic");
        }
        if (n < 6)
            return FRAME_NEED_MORE;
        uint32_t hl = be16(p + 4);
        if (hl > BEP_HELLO_MAX)
            return fail(rx, "Hello too large");
        if (n < 6 + hl)
            return FRAME_NEED_MORE;
        rx->got_hello = 1;
        rx->body = p + 6;
        rx->body_len = hl;
        rx->wire_len = 6 + hl;
        rx->consume = 6 + hl;
        return FRAME_HELLO;
    }

    if (n < 2)
        return FRAME_NEED_MORE;
    uint32_t hl = be16(p);
    if (hl > MAX_HEADER) /* Syncthing rejects a negative int16 */
        return fail(rx, "header length out of range");
    if (n < 2 + hl + 4)
        return FRAME_NEED_MORE;
    bep_header hdr;
    if (bep_decode_header(p + 2, hl, &hdr) != 0)
        return fail(rx, "malformed header");
    uint32_t ml = be32(p + 2 + hl);
    if (ml > rx->max_message)
        return fail(rx, "message length exceeds the limit");
    if (n < 2 + hl + 4 + ml)
        return FRAME_NEED_MORE;
    const unsigned char *body = p + 2 + hl + 4;

    rx->type = hdr.type;
    rx->compression = hdr.compression;
    rx->wire_len = 2 + hl + 4 + ml;
    rx->consume = rx->wire_len;
    switch (hdr.compression) {
    case BEP_COMPRESSION_NONE:
        rx->body = body;
        rx->body_len = ml;
        break;
    case BEP_COMPRESSION_LZ4: {
        if (ml < 4)
            return fail(rx, "compressed message too short");
        uint32_t size = be32(body);
        if (size > rx->max_decompressed)
            return fail(rx, "decompressed size exceeds the limit");
        if (size > rx->out_cap) {
            unsigned char *o = realloc(rx->out, size ? size : 1);
            if (!o)
                return fail(rx, "out of memory");
            rx->out = o;
            rx->out_cap = size;
        }
        long got = lz4_block_decode(body + 4, ml - 4, rx->out, size);
        /* The prefix is the exact uncompressed size the sender compressed. */
        if (got < 0 || (uint32_t)got != size)
            return fail(rx, "LZ4 data invalid or size mismatch");
        rx->body = rx->out;
        rx->body_len = size;
        break;
    }
    default:
        return fail(rx, "unknown compression");
    }
    return FRAME_MESSAGE;
}

static void put_be(pb_writer *w, uint32_t v, int bytes)
{
    unsigned char b[4];
    for (int i = 0; i < bytes; i++)
        b[i] = (unsigned char)(v >> (8 * (bytes - 1 - i)));
    pb_put_raw(w, b, (size_t)bytes);
}

void frame_put_hello(pb_writer *w, const pb_writer *hello)
{
    if (hello->error || hello->len > BEP_HELLO_MAX) {
        w->error = 1;
        return;
    }
    put_be(w, BEP_HELLO_MAGIC, 4);
    put_be(w, (uint32_t)hello->len, 2);
    pb_put_raw(w, hello->data, hello->len);
}

void frame_put_message(pb_writer *w, uint32_t type, const pb_writer *body)
{
    pb_writer hdr;
    pb_writer_init(&hdr, MAX_HEADER);
    bep_encode_header(&hdr, type, BEP_COMPRESSION_NONE);
    if (hdr.error || body->error || body->len > 0xffffffffu) {
        w->error = 1;
        pb_writer_free(&hdr);
        return;
    }
    put_be(w, (uint32_t)hdr.len, 2);
    pb_put_raw(w, hdr.data, hdr.len);
    put_be(w, (uint32_t)body->len, 4);
    pb_put_raw(w, body->data, body->len);
    pb_writer_free(&hdr);
}
