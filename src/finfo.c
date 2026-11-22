#include "finfo.h"

#include "bep.h"

#include <openssl/evp.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void finfo_free(finfo *f)
{
    free(f->name);
    vv_free(&f->version);
    free(f->blocks);
    memset(f, 0, sizeof *f);
}

static int read_varint(pb_reader *r, int wire, uint64_t *v)
{
    return wire == PB_VARINT ? pb_varint(r, v) : -1;
}

static int decode_block(pb_slice s, finfo_block *b)
{
    pb_reader r;
    uint32_t f;
    int w, n, have_hash = 0;
    uint64_t v;
    pb_slice h;
    memset(b, 0, sizeof *b);
    pb_reader_init(&r, s.data, s.len);
    while ((n = pb_next(&r, &f, &w)) == 1) {
        switch (f) {
        case 1:
            if (read_varint(&r, w, &v) != 0)
                return -1;
            b->offset = (int64_t)v;
            break;
        case 2:
            if (read_varint(&r, w, &v) != 0)
                return -1;
            b->size = (int32_t)v;
            break;
        case 3:
            if (w != PB_LEN || pb_bytes(&r, &h) != 0 || h.len != FINFO_HASH)
                return -1;
            memcpy(b->hash, h.data, FINFO_HASH);
            have_hash = 1;
            break;
        default:
            if (pb_skip(&r, w) != 0)
                return -1;
        }
    }
    return n == 0 && !r.error && have_hash ? 0 : -1;
}

int finfo_decode(pb_slice raw, finfo *out, const char **why)
{
    pb_reader r;
    uint32_t f;
    int w, n;
    uint64_t v;
    pb_slice s;
    const char *dummy;
    if (!why)
        why = &dummy;
    *why = "malformed FileInfo";
    memset(out, 0, sizeof *out);
    pb_reader_init(&r, raw.data, raw.len);
    while ((n = pb_next(&r, &f, &w)) == 1) {
        switch (f) {
        case 1:
            if (w != PB_LEN || pb_bytes(&r, &s) != 0 || memchr(s.data, 0, s.len))
                goto bad;
            free(out->name);
            if (!(out->name = malloc(s.len + 1)))
                goto bad;
            memcpy(out->name, s.data, s.len);
            out->name[s.len] = 0;
            break;
        case 2: if (read_varint(&r, w, &v)) goto bad; out->type = (uint32_t)v; break;
        case 3: if (read_varint(&r, w, &v)) goto bad; out->size = (int64_t)v; break;
        case 4: if (read_varint(&r, w, &v)) goto bad; out->permissions = (uint32_t)v; break;
        case 5: if (read_varint(&r, w, &v)) goto bad; out->modified_s = (int64_t)v; break;
        case 6: if (read_varint(&r, w, &v)) goto bad; out->deleted = v != 0; break;
        case 7: if (read_varint(&r, w, &v)) goto bad; out->invalid = v != 0; break;
        case 8: if (read_varint(&r, w, &v)) goto bad; out->no_permissions = v != 0; break;
        case 9:
            if (w != PB_LEN || pb_bytes(&r, &s) != 0)
                goto bad;
            vv_free(&out->version);
            if (vv_decode(s, &out->version) != 0) {
                *why = "version vector malformed or not in order";
                goto bad_keep_why;
            }
            break;
        case 10: if (read_varint(&r, w, &v)) goto bad; out->sequence = (int64_t)v; break;
        case 11: if (read_varint(&r, w, &v)) goto bad; out->modified_ns = (int32_t)v; break;
        case 12: if (read_varint(&r, w, &v)) goto bad; out->modified_by = v; break;
        case 13: if (read_varint(&r, w, &v)) goto bad; out->block_size = (int32_t)v; break;
        case 14: out->has_platform = 1; if (pb_skip(&r, w)) goto bad; break;
        case 16: {
            if (w != PB_LEN || pb_bytes(&r, &s) != 0)
                goto bad;
            if (out->nblocks >= FINFO_MAX_BLOCKS) {
                *why = "too many blocks";
                goto bad_keep_why;
            }
            if ((out->nblocks & (out->nblocks - 1)) == 0) { /* grow at powers of two */
                finfo_block *nb = realloc(out->blocks, (out->nblocks ? out->nblocks * 2 : 1) * sizeof *nb);
                if (!nb)
                    goto bad;
                out->blocks = nb;
            }
            if (decode_block(s, &out->blocks[out->nblocks]) != 0) {
                *why = "malformed block (the hash must be SHA-256)";
                goto bad_keep_why;
            }
            out->nblocks++;
            break;
        }
        case 17: out->has_symlink_target = 1; if (pb_skip(&r, w)) goto bad; break;
        case 18:
        case 20:
            if (w != PB_LEN || pb_bytes(&r, &s) != 0)
                goto bad;
            if (s.len == 0)
                break;
            if (s.len != FINFO_HASH) {
                *why = "blocks hash is not SHA-256";
                goto bad_keep_why;
            }
            if (f == 18) {
                memcpy(out->blocks_hash, s.data, FINFO_HASH);
                out->has_blocks_hash = 1;
            } else {
                memcpy(out->prev_blocks_hash, s.data, FINFO_HASH);
                out->has_prev_blocks_hash = 1;
            }
            break;
        case 19: out->has_encrypted = 1; if (pb_skip(&r, w)) goto bad; break;
        default:
            if (pb_skip(&r, w) != 0)
                goto bad;
        }
    }
    if (n != 0 || r.error)
        goto bad;
    if (!out->name || !out->name[0]) {
        *why = "empty name";
        goto bad_keep_why;
    }
    if (out->type == BEP_FILE && !out->deleted && !out->invalid) {
        int64_t at = 0;
        for (size_t i = 0; i < out->nblocks; i++) {
            if (out->blocks[i].offset != at || out->blocks[i].size < 0 || out->blocks[i].size > FINFO_MAX_BLOCK) {
                *why = "blocks are not contiguous or out of range";
                goto bad_keep_why;
            }
            at += out->blocks[i].size;
        }
        if (at != out->size || (out->size > 0 && out->nblocks == 0)) {
            *why = "blocks do not add up to the file size";
            goto bad_keep_why;
        }
    }
    *why = NULL;
    return 0;
bad:
    *why = "malformed FileInfo";
bad_keep_why:
    finfo_free(out);
    return -1;
}

int finfo_encode(pb_writer *w, const finfo *f)
{
    /* proto3: fields at their default value are not written. */
    pb_put_string_field(w, 1, f->name);
    if (f->type)
        pb_put_varint_field(w, 2, f->type);
    if (f->size)
        pb_put_varint_field(w, 3, (uint64_t)f->size);
    if (f->permissions)
        pb_put_varint_field(w, 4, f->permissions);
    if (f->modified_s)
        pb_put_varint_field(w, 5, (uint64_t)f->modified_s);
    if (f->deleted)
        pb_put_varint_field(w, 6, 1);
    if (f->invalid)
        pb_put_varint_field(w, 7, 1);
    if (f->no_permissions)
        pb_put_varint_field(w, 8, 1);
    vv_encode(w, 9, &f->version);
    if (f->sequence)
        pb_put_varint_field(w, 10, (uint64_t)f->sequence);
    if (f->modified_ns)
        pb_put_varint_field(w, 11, (uint64_t)(int64_t)f->modified_ns);
    if (f->modified_by)
        pb_put_varint_field(w, 12, f->modified_by);
    if (f->block_size)
        pb_put_varint_field(w, 13, (uint64_t)(int64_t)f->block_size);
    for (size_t i = 0; i < f->nblocks; i++) {
        pb_writer b;
        pb_writer_init(&b, 128);
        if (f->blocks[i].offset)
            pb_put_varint_field(&b, 1, (uint64_t)f->blocks[i].offset);
        if (f->blocks[i].size)
            pb_put_varint_field(&b, 2, (uint64_t)(int64_t)f->blocks[i].size);
        pb_put_bytes_field(&b, 3, f->blocks[i].hash, FINFO_HASH);
        pb_put_message_field(w, 16, &b);
        pb_writer_free(&b);
    }
    if (f->has_blocks_hash)
        pb_put_bytes_field(w, 18, f->blocks_hash, FINFO_HASH);
    if (f->has_prev_blocks_hash)
        pb_put_bytes_field(w, 20, f->prev_blocks_hash, FINFO_HASH);
    return w->error ? -1 : 0;
}

int finfo_copy(finfo *dst, const finfo *src)
{
    *dst = *src;
    dst->name = NULL;
    dst->blocks = NULL;
    dst->version.c = NULL;
    dst->version.n = 0;
    if (!(dst->name = strdup(src->name)) || vv_copy(&dst->version, &src->version) != 0)
        goto bad;
    if (src->nblocks) {
        if (!(dst->blocks = malloc(src->nblocks * sizeof *dst->blocks)))
            goto bad;
        memcpy(dst->blocks, src->blocks, src->nblocks * sizeof *dst->blocks);
    }
    return 0;
bad:
    finfo_free(dst);
    return -1;
}

void finfo_blocks_hash(const finfo_block *b, size_t n, unsigned char out[FINFO_HASH])
{
    EVP_MD_CTX *c = EVP_MD_CTX_new();
    unsigned int len = 0;
    if (c && EVP_DigestInit_ex(c, EVP_sha256(), NULL)) {
        for (size_t i = 0; i < n; i++)
            EVP_DigestUpdate(c, b[i].hash, FINFO_HASH);
        EVP_DigestFinal_ex(c, out, &len);
    }
    if (len != FINFO_HASH)
        memset(out, 0, FINFO_HASH);
    EVP_MD_CTX_free(c);
}

int32_t finfo_block_size_for(int64_t size)
{
    int32_t bs = FINFO_MIN_BLOCK;
    for (; bs <= FINFO_MAX_BLOCK; bs *= 2)
        if (size < (int64_t)FINFO_PER_FILE_BLOCKS * bs)
            return bs;
    return FINFO_MAX_BLOCK;
}

int finfo_same_blocks(const finfo *a, const finfo *b)
{
    if (a->size != b->size || a->nblocks != b->nblocks)
        return 0;
    for (size_t i = 0; i < a->nblocks; i++)
        if (a->blocks[i].size != b->blocks[i].size || memcmp(a->blocks[i].hash, b->blocks[i].hash, FINFO_HASH))
            return 0;
    return 1;
}

static const unsigned char sha256_empty[32] = {
    0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
    0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55
};

struct finfo_hasher {
    FILE *fp;
    int32_t bs;
    size_t want, n;
    int64_t expect, at;
    finfo_block *b;
    unsigned char *buf;
};

finfo_hasher *finfo_hash_begin(const char *path, int64_t expect_size)
{
    int32_t bs = finfo_block_size_for(expect_size);
    size_t want = expect_size > 0 ? (size_t)((expect_size + bs - 1) / bs) : 1;
    if (want > FINFO_MAX_BLOCKS)
        return NULL;
    finfo_hasher *h = calloc(1, sizeof *h);
    if (!h)
        return NULL;
    h->bs = bs;
    h->want = want;
    h->expect = expect_size;
    h->fp = fopen(path, "rb");
    h->b = malloc(want * sizeof *h->b);
    h->buf = malloc((size_t)bs);
    if (!h->fp || !h->b || !h->buf) {
        finfo_hash_abort(h);
        return NULL;
    }
    return h;
}

int finfo_hash_step(finfo_hasher *h)
{
    size_t got = fread(h->buf, 1, (size_t)h->bs, h->fp);
    if (got == 0) {
        if (ferror(h->fp))
            return -1;
        return h->at != h->expect ? 1 : 0; /* shorter than when it was listed */
    }
    if (h->n >= h->want || h->at + (int64_t)got > h->expect)
        return 1; /* longer than when it was listed */
    unsigned int len = 0;
    if (!EVP_Digest(h->buf, got, h->b[h->n].hash, &len, EVP_sha256(), NULL) || len != FINFO_HASH)
        return -1;
    h->b[h->n].offset = h->at;
    h->b[h->n].size = (int32_t)got;
    h->at += (int64_t)got;
    h->n++;
    return 2;
}

void finfo_hash_finish(finfo_hasher *h, finfo *f)
{
    if (h->n == 0) {
        h->b[0].offset = 0;
        h->b[0].size = 0;
        memcpy(h->b[0].hash, sha256_empty, FINFO_HASH);
        h->n = 1;
    }
    free(f->blocks);
    f->blocks = h->b;
    f->nblocks = h->n;
    f->size = h->expect;
    f->block_size = h->bs;
    finfo_blocks_hash(f->blocks, f->nblocks, f->blocks_hash);
    f->has_blocks_hash = 1;
    h->b = NULL;
    finfo_hash_abort(h);
}

void finfo_hash_abort(finfo_hasher *h)
{
    if (!h)
        return;
    if (h->fp)
        fclose(h->fp);
    free(h->b);
    free(h->buf);
    free(h);
}

int finfo_hash_path(const char *path, int64_t expect_size, finfo *f)
{
    finfo_hasher *h = finfo_hash_begin(path, expect_size);
    if (!h)
        return -1;
    int rc;
    while ((rc = finfo_hash_step(h)) == 2)
        ;
    if (rc != 0) {
        finfo_hash_abort(h);
        return rc;
    }
    finfo_hash_finish(h, f);
    return 0;
}

int finfo_path_matches(const char *path, const finfo *f)
{
    finfo g;
    memset(&g, 0, sizeof g);
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return 0;
    fclose(fp);
    if (finfo_hash_path(path, f->size, &g) != 0)
        return 0;
    int same = finfo_same_blocks(&g, f);
    free(g.blocks);
    return same;
}

int finfo_wins_conflict(const finfo *a, const finfo *b)
{
    if (a->invalid != b->invalid)
        return !a->invalid;
    if (a->modified_s != b->modified_s)
        return a->modified_s > b->modified_s;
    if (a->modified_ns != b->modified_ns)
        return a->modified_ns > b->modified_ns;
    return vv_compare(&a->version, &b->version) == VV_CONCURRENT_GREATER;
}

int finfo_in_conflict_with(const finfo *newer, const finfo *prev)
{
    enum vv_order o = vv_compare(&newer->version, &prev->version);
    if (o == VV_GREATER || o == VV_EQUAL)
        return 0;
    /* No content to compare (or a type change): the new entry's own two
     * hashes, as in Syncthing. */
    if (!newer->has_prev_blocks_hash || !newer->has_blocks_hash)
        return 1;
    /* Based on the content we have: not really a conflict. */
    return !prev->has_blocks_hash || memcmp(newer->prev_blocks_hash, prev->blocks_hash, FINFO_HASH) != 0;
}

int finfo_set_deleted(finfo *f, uint64_t by, int64_t now)
{
    if (vv_update(&f->version, by, (uint64_t)now) != 0)
        return -1;
    f->modified_by = by;
    f->deleted = 1;
    f->modified_s = now;
    free(f->blocks);
    f->blocks = NULL;
    f->nblocks = 0;
    f->has_blocks_hash = 0;
    f->size = 0;
    return 0;
}

/* filepath.Ext: from the last '.' after the last '/', or "". */
static const char *ext_of(const char *name)
{
    const char *dot = NULL;
    for (const char *p = name; *p; p++) {
        if (*p == '/')
            dot = NULL;
        else if (*p == '.')
            dot = p;
    }
    return dot ? dot : name + strlen(name);
}

int finfo_conflict_name(const char *name, const struct tm *t, uint64_t modified_by, char *out, size_t n)
{
    const char *ext = ext_of(name);
    char by[8];
    vv_short_id_text(modified_by, by);
    int k = snprintf(out, n, "%.*s.sync-conflict-%04d%02d%02d-%02d%02d%02d-%s%s", (int)(ext - name), name,
                     t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec, by, ext);
    return k > 0 && (size_t)k < n ? 0 : -1;
}

int finfo_is_conflict_name(const char *name)
{
    const char *base = strrchr(name, '/');
    return strstr(base ? base + 1 : name, ".sync-conflict-") != NULL;
}
