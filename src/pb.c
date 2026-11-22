#include "pb.h"

#include <stdlib.h>
#include <string.h>

void pb_reader_init(pb_reader *r, const void *data, size_t len)
{
    r->p = data;
    r->end = r->p + len;
    r->error = 0;
}

static int fail(pb_reader *r)
{
    r->error = 1;
    return -1;
}

int pb_varint(pb_reader *r, uint64_t *v)
{
    uint64_t out = 0;
    if (r->error)
        return -1;
    for (int shift = 0; shift < 64; shift += 7) {
        if (r->p >= r->end)
            return fail(r);
        unsigned char b = *r->p++;
        /* The tenth byte may carry only the top bit of a 64-bit value. */
        if (shift == 63 && b > 1)
            return fail(r);
        out |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) {
            *v = out;
            return 0;
        }
    }
    return fail(r);
}

int pb_next(pb_reader *r, uint32_t *field, int *wire)
{
    uint64_t key;
    if (r->error)
        return -1;
    if (r->p >= r->end)
        return 0;
    if (pb_varint(r, &key) != 0)
        return -1;
    uint64_t f = key >> 3;
    int w = (int)(key & 7);
    if (f == 0 || f > 0x1fffffff || (w != PB_VARINT && w != PB_FIXED64 && w != PB_LEN && w != PB_FIXED32))
        return fail(r);
    *field = (uint32_t)f;
    *wire = w;
    return 1;
}

int pb_bytes(pb_reader *r, pb_slice *s)
{
    uint64_t n;
    if (pb_varint(r, &n) != 0)
        return -1;
    if (n > (uint64_t)(r->end - r->p))
        return fail(r);
    s->data = r->p;
    s->len = (size_t)n;
    r->p += n;
    return 0;
}

int pb_skip(pb_reader *r, int wire)
{
    uint64_t v;
    pb_slice s;
    switch (wire) {
    case PB_VARINT:
        return pb_varint(r, &v);
    case PB_LEN:
        return pb_bytes(r, &s);
    case PB_FIXED64:
    case PB_FIXED32: {
        size_t n = wire == PB_FIXED64 ? 8 : 4;
        if ((size_t)(r->end - r->p) < n)
            return fail(r);
        r->p += n;
        return 0;
    }
    default:
        return fail(r);
    }
}

/* ---- writer ---- */

void pb_writer_init(pb_writer *w, size_t limit)
{
    memset(w, 0, sizeof *w);
    w->limit = limit;
}

void pb_writer_free(pb_writer *w)
{
    free(w->data);
    memset(w, 0, sizeof *w);
}

void pb_put_raw(pb_writer *w, const void *data, size_t len)
{
    if (w->error)
        return;
    if (len > w->limit - w->len) {
        w->error = 1;
        return;
    }
    if (w->len + len > w->cap) {
        size_t cap = w->cap ? w->cap : 256;
        while (cap < w->len + len)
            cap *= 2;
        if (cap > w->limit)
            cap = w->limit;
        unsigned char *d = realloc(w->data, cap);
        if (!d) {
            w->error = 1;
            return;
        }
        w->data = d;
        w->cap = cap;
    }
    if (len)
        memcpy(w->data + w->len, data, len);
    w->len += len;
}

static void put_varint(pb_writer *w, uint64_t v)
{
    unsigned char buf[10];
    size_t n = 0;
    do {
        buf[n] = (unsigned char)(v & 0x7f);
        v >>= 7;
        if (v)
            buf[n] |= 0x80;
        n++;
    } while (v);
    pb_put_raw(w, buf, n);
}

void pb_put_varint_field(pb_writer *w, uint32_t field, uint64_t v)
{
    /* proto3 omits default (zero) scalars; Syncthing's decoder treats a
     * missing field as zero, so writing it would only cost bytes. */
    if (v == 0)
        return;
    put_varint(w, ((uint64_t)field << 3) | PB_VARINT);
    put_varint(w, v);
}

void pb_put_bytes_field(pb_writer *w, uint32_t field, const void *data, size_t len)
{
    if (len == 0)
        return;
    put_varint(w, ((uint64_t)field << 3) | PB_LEN);
    put_varint(w, len);
    pb_put_raw(w, data, len);
}

void pb_put_string_field(pb_writer *w, uint32_t field, const char *s)
{
    pb_put_bytes_field(w, field, s, strlen(s));
}

void pb_put_message_field(pb_writer *w, uint32_t field, const pb_writer *sub)
{
    if (sub->error) {
        w->error = 1;
        return;
    }
    /* An empty nested message is still written: for repeated message
     * fields its presence is the information. */
    put_varint(w, ((uint64_t)field << 3) | PB_LEN);
    put_varint(w, sub->len);
    pb_put_raw(w, sub->data, sub->len);
}
