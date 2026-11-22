/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Translated from Syncthing's lib/protocol/vector.go, Copyright (C) 2015
 * The Syncthing Authors. */
#include "vv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t vv_short_id(const unsigned char id[DEVID_BYTES])
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = v << 8 | id[i];
    return v;
}

void vv_short_id_text(uint64_t id, char out[8])
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    out[0] = 0;
    if (id == 0)
        return;
    /* 7 characters cover the first 35 bits. */
    for (int i = 0; i < 7; i++)
        out[i] = alphabet[(id >> (64 - 5 * (i + 1))) & 31];
    out[7] = 0;
}

void vv_free(vv *v)
{
    free(v->c);
    v->c = NULL;
    v->n = 0;
}

static int grow(vv *v, size_t n)
{
    if (n > VV_MAX_COUNTERS)
        return -1;
    vv_counter *nc = realloc(v->c, (n ? n : 1) * sizeof *nc);
    if (!nc)
        return -1;
    v->c = nc;
    return 0;
}

int vv_copy(vv *dst, const vv *src)
{
    dst->c = NULL;
    dst->n = 0;
    if (grow(dst, src->n) != 0)
        return -1;
    if (src->n)
        memcpy(dst->c, src->c, src->n * sizeof *src->c);
    dst->n = src->n;
    return 0;
}

static int decode_counter(pb_slice s, vv_counter *c)
{
    pb_reader r;
    uint32_t f;
    int w, n;
    c->id = c->value = 0;
    pb_reader_init(&r, s.data, s.len);
    while ((n = pb_next(&r, &f, &w)) == 1) {
        if ((f == 1 || f == 2) && w != PB_VARINT)
            return -1;
        if (f == 1) {
            if (pb_varint(&r, &c->id) != 0)
                return -1;
        } else if (f == 2) {
            if (pb_varint(&r, &c->value) != 0)
                return -1;
        } else if (pb_skip(&r, w) != 0) {
            return -1;
        }
    }
    return n == 0 && !r.error ? 0 : -1;
}

int vv_decode(pb_slice msg, vv *out)
{
    pb_reader r;
    uint32_t f;
    int w, n;
    out->c = NULL;
    out->n = 0;
    pb_reader_init(&r, msg.data, msg.len);
    while ((n = pb_next(&r, &f, &w)) == 1) {
        if (f != 1) {
            if (pb_skip(&r, w) != 0)
                break;
            continue;
        }
        pb_slice s;
        vv_counter c;
        if (w != PB_LEN || pb_bytes(&r, &s) != 0 || decode_counter(s, &c) != 0)
            goto bad;
        if (out->n > 0 && c.id <= out->c[out->n - 1].id)
            goto bad; /* unsorted or duplicate */
        if (grow(out, out->n + 1) != 0)
            goto bad;
        out->c[out->n++] = c;
    }
    if (n == 0 && !r.error)
        return 0;
bad:
    vv_free(out);
    return -1;
}

void vv_encode(pb_writer *w, uint32_t field, const vv *v)
{
    if (v->n == 0)
        return;
    pb_writer m;
    pb_writer_init(&m, w->limit);
    for (size_t i = 0; i < v->n; i++) {
        pb_writer c;
        pb_writer_init(&c, 64);
        pb_put_varint_field(&c, 1, v->c[i].id);
        pb_put_varint_field(&c, 2, v->c[i].value);
        pb_put_message_field(&m, 1, &c);
        pb_writer_free(&c);
    }
    pb_put_message_field(w, field, &m);
    if (m.error)
        w->error = 1;
    pb_writer_free(&m);
}

int vv_concurrent(enum vv_order o)
{
    return o == VV_CONCURRENT_LESSER || o == VV_CONCURRENT_GREATER;
}

/* Vector.Compare, line by line. */
enum vv_order vv_compare(const vv *a, const vv *b)
{
    size_t ai = 0, bi = 0;
    vv_counter av, bv;
    enum vv_order result = VV_EQUAL;
    while (ai < a->n || bi < b->n) {
        int amiss = 0, bmiss = 0;
        if (ai < a->n) {
            av = a->c[ai];
        } else {
            av.id = av.value = 0;
            amiss = 1;
        }
        if (bi < b->n) {
            bv = b->c[bi];
        } else {
            bv.id = bv.value = 0;
            bmiss = 1;
        }
        if (av.id == bv.id) {
            if (av.value > bv.value) {
                if (result == VV_LESSER)
                    return VV_CONCURRENT_LESSER;
                result = VV_GREATER;
            } else if (av.value < bv.value) {
                if (result == VV_GREATER)
                    return VV_CONCURRENT_GREATER;
                result = VV_LESSER;
            }
        } else if ((!amiss && av.id < bv.id) || bmiss) {
            if (av.value > 0) {
                if (result == VV_LESSER)
                    return VV_CONCURRENT_LESSER;
                result = VV_GREATER;
            }
        } else if ((!bmiss && bv.id < av.id) || amiss) {
            if (bv.value > 0) {
                if (result == VV_GREATER)
                    return VV_CONCURRENT_GREATER;
                result = VV_LESSER;
            }
        }
        if (ai < a->n && (av.id <= bv.id || bmiss))
            ai++;
        if (bi < b->n && (bv.id <= av.id || amiss))
            bi++;
    }
    return result;
}

static uint64_t max_u64(uint64_t a, uint64_t b) { return a > b ? a : b; }

int vv_update(vv *v, uint64_t id, uint64_t now)
{
    size_t i;
    for (i = 0; i < v->n; i++) {
        if (v->c[i].id == id) {
            v->c[i].value = max_u64(v->c[i].value + 1, now);
            return 0;
        }
        if (v->c[i].id > id)
            break;
    }
    if (grow(v, v->n + 1) != 0)
        return -1;
    memmove(v->c + i + 1, v->c + i, (v->n - i) * sizeof *v->c);
    v->c[i].id = id;
    v->c[i].value = max_u64(1, now);
    v->n++;
    return 0;
}

uint64_t vv_counter_value(const vv *v, uint64_t id)
{
    for (size_t i = 0; i < v->n; i++)
        if (v->c[i].id == id)
            return v->c[i].value;
    return 0;
}

/* The result of Syncthing's Vector.Merge for sorted inputs: every ID of
 * either side, the larger value where both have one. */
int vv_merge(vv *v, const vv *b)
{
    vv m = { NULL, 0 };
    if (grow(&m, v->n + b->n) != 0)
        return -1;
    size_t i = 0, j = 0;
    while (i < v->n || j < b->n) {
        if (j >= b->n || (i < v->n && v->c[i].id < b->c[j].id)) {
            m.c[m.n++] = v->c[i++];
        } else if (i >= v->n || b->c[j].id < v->c[i].id) {
            m.c[m.n++] = b->c[j++];
        } else {
            vv_counter c = v->c[i++];
            c.value = max_u64(c.value, b->c[j++].value);
            m.c[m.n++] = c;
        }
    }
    if (m.n > VV_MAX_COUNTERS) {
        vv_free(&m);
        return -1;
    }
    vv_free(v);
    *v = m;
    return 0;
}

int vv_parse(const char *s, vv *out)
{
    out->c = NULL;
    out->n = 0;
    if (strcmp(s, "-") == 0)
        return 0;
    const char *p = s;
    while (*p) {
        char *e;
        unsigned long long id = strtoull(p, &e, 10);
        if (e == p || *e != ':')
            goto bad;
        p = e + 1;
        unsigned long long val = strtoull(p, &e, 10);
        if (e == p || (*e != ',' && *e != 0))
            goto bad;
        if (grow(out, out->n + 1) != 0)
            goto bad;
        out->c[out->n].id = id;
        out->c[out->n].value = val;
        out->n++;
        p = *e ? e + 1 : e;
    }
    return 0;
bad:
    vv_free(out);
    return -1;
}

void vv_format(const vv *v, char *out, size_t n)
{
    size_t o = 0;
    if (n == 0)
        return;
    out[0] = 0;
    if (v->n == 0) {
        snprintf(out, n, "-");
        return;
    }
    for (size_t i = 0; i < v->n && o < n; i++) {
        int k = snprintf(out + o, n - o, "%s%llu:%llu", i ? "," : "", (unsigned long long)v->c[i].id,
                         (unsigned long long)v->c[i].value);
        if (k < 0)
            break;
        o += (size_t)k;
    }
}
