/* Field numbers are those of Syncthing's proto/bep/bep.proto (v2.1.2;
 * unchanged at the studied revision). A known field arriving with the
 * wrong wire type is treated as malformed rather than as unknown: nothing
 * Syncthing sends does that, so it can only be corruption. */
#include "bep.h"

#include <string.h>

#define WANT(r, wire, expected) \
    do { if ((wire) != (expected)) { (r)->error = 1; return -1; } } while (0)

static int read_u64(pb_reader *r, int wire, uint64_t *v)
{
    WANT(r, wire, PB_VARINT);
    return pb_varint(r, v);
}

/* int32/int64/enum fields: proto3 sign-extends negatives to 64 bits. */
static int read_i64(pb_reader *r, int wire, int64_t *v)
{
    uint64_t u;
    if (read_u64(r, wire, &u) != 0)
        return -1;
    *v = (int64_t)u;
    return 0;
}

static int read_i32(pb_reader *r, int wire, int32_t *v)
{
    uint64_t u;
    if (read_u64(r, wire, &u) != 0)
        return -1;
    *v = (int32_t)(uint32_t)u;
    return 0;
}

static int read_u32(pb_reader *r, int wire, uint32_t *v)
{
    uint64_t u;
    if (read_u64(r, wire, &u) != 0)
        return -1;
    *v = (uint32_t)u;
    return 0;
}

static int read_bool(pb_reader *r, int wire, int *v)
{
    uint64_t u;
    if (read_u64(r, wire, &u) != 0)
        return -1;
    *v = u != 0;
    return 0;
}

static int read_slice(pb_reader *r, int wire, pb_slice *s)
{
    WANT(r, wire, PB_LEN);
    return pb_bytes(r, s);
}

/* Ends a decode loop: 0 if the whole buffer was consumed cleanly. */
static int finish(pb_reader *r, int next)
{
    return (next == 0 && !r->error) ? 0 : -1;
}

int bep_decode_hello(const void *buf, size_t len, bep_hello *out)
{
    pb_reader r;
    uint32_t f;
    int w, n, rc = 0;
    memset(out, 0, sizeof *out);
    pb_reader_init(&r, buf, len);
    while ((n = pb_next(&r, &f, &w)) == 1 && rc == 0) {
        switch (f) {
        case 1: rc = read_slice(&r, w, &out->device_name); break;
        case 2: rc = read_slice(&r, w, &out->client_name); break;
        case 3: rc = read_slice(&r, w, &out->client_version); break;
        case 4: rc = read_i64(&r, w, &out->num_connections); break;
        case 5: rc = read_i64(&r, w, &out->timestamp); break;
        default: rc = pb_skip(&r, w);
        }
    }
    return rc ? -1 : finish(&r, n);
}

int bep_decode_header(const void *buf, size_t len, bep_header *out)
{
    pb_reader r;
    uint32_t f;
    int w, n, rc = 0;
    memset(out, 0, sizeof *out);
    pb_reader_init(&r, buf, len);
    while ((n = pb_next(&r, &f, &w)) == 1 && rc == 0) {
        switch (f) {
        case 1: rc = read_u32(&r, w, &out->type); break;
        case 2: rc = read_u32(&r, w, &out->compression); break;
        default: rc = pb_skip(&r, w);
        }
    }
    return rc ? -1 : finish(&r, n);
}

static int decode_cc_device(const pb_slice *s, bep_cc_device *d)
{
    pb_reader r;
    uint32_t f;
    int w, n, rc = 0;
    pb_slice id = { 0 }, token = { 0 };
    memset(d, 0, sizeof *d);
    pb_reader_init(&r, s->data, s->len);
    while ((n = pb_next(&r, &f, &w)) == 1 && rc == 0) {
        switch (f) {
        case 1: rc = read_slice(&r, w, &id); break;
        case 2: rc = read_slice(&r, w, &d->name); break;
        case 6: rc = read_i64(&r, w, &d->max_sequence); break;
        case 7: rc = read_bool(&r, w, &d->introducer); break;
        case 8: rc = read_u64(&r, w, &d->index_id); break;
        case 10: rc = read_slice(&r, w, &token); break;
        default: rc = pb_skip(&r, w); /* addresses, compression, cert name, ... */
        }
    }
    if (rc || finish(&r, n))
        return -1;
    if (id.len == DEVID_BYTES) {
        memcpy(d->id, id.data, DEVID_BYTES);
        d->id_valid = 1;
    }
    d->encryption_token_len = token.len;
    return 0;
}

static int decode_cc_folder(const pb_slice *s, void *ctx, bep_folder_cb on_folder,
                            bep_device_cb on_device)
{
    pb_reader r;
    uint32_t f;
    int w, n, rc = 0;
    bep_cc_folder folder;
    memset(&folder, 0, sizeof folder);

    /* Two passes: the folder's scalar fields may follow its devices on the
     * wire, and callbacks want the complete folder first. */
    pb_reader_init(&r, s->data, s->len);
    while ((n = pb_next(&r, &f, &w)) == 1 && rc == 0) {
        switch (f) {
        case 1: rc = read_slice(&r, w, &folder.id); break;
        case 2: rc = read_slice(&r, w, &folder.label); break;
        case 3: rc = read_u32(&r, w, &folder.type); break;
        case 7: rc = read_u32(&r, w, &folder.stop_reason); break;
        default: rc = pb_skip(&r, w);
        }
    }
    if (rc || finish(&r, n))
        return -1;
    if (on_folder && (rc = on_folder(ctx, &folder)) != 0)
        return rc;

    pb_reader_init(&r, s->data, s->len);
    while ((n = pb_next(&r, &f, &w)) == 1) {
        if (f != 16) {
            if (pb_skip(&r, w) != 0)
                return -1;
            continue;
        }
        pb_slice ds;
        bep_cc_device d;
        if (read_slice(&r, w, &ds) != 0 || decode_cc_device(&ds, &d) != 0)
            return -1;
        if (on_device && (rc = on_device(ctx, &folder, &d)) != 0)
            return rc;
    }
    return finish(&r, n);
}

int bep_decode_cluster_config(const void *buf, size_t len, void *ctx,
                              bep_folder_cb on_folder, bep_device_cb on_device)
{
    pb_reader r;
    uint32_t f;
    int w, n, rc = 0;
    pb_reader_init(&r, buf, len);
    while ((n = pb_next(&r, &f, &w)) == 1) {
        if (f == 1) {
            pb_slice fs;
            if (read_slice(&r, w, &fs) != 0)
                return -1;
            if ((rc = decode_cc_folder(&fs, ctx, on_folder, on_device)) != 0)
                return rc;
        } else if (pb_skip(&r, w) != 0) {
            return -1;
        }
    }
    return finish(&r, n);
}

/* Counts the elements of a repeated message field inside a message. */
static int count_field(const pb_slice *s, uint32_t field, size_t *count)
{
    pb_reader r;
    uint32_t f;
    int w, n;
    *count = 0;
    pb_reader_init(&r, s->data, s->len);
    while ((n = pb_next(&r, &f, &w)) == 1) {
        if (f == field && w != PB_LEN)
            return -1;
        if (pb_skip(&r, w) != 0)
            return -1;
        if (f == field)
            (*count)++;
    }
    return finish(&r, n);
}

static int decode_file(const pb_slice *s, bep_file *fi)
{
    pb_reader r;
    uint32_t f;
    int w, n, rc = 0;
    pb_slice sub;
    memset(fi, 0, sizeof *fi);
    pb_reader_init(&r, s->data, s->len);
    while ((n = pb_next(&r, &f, &w)) == 1 && rc == 0) {
        switch (f) {
        case 1: rc = read_slice(&r, w, &fi->name); break;
        case 2: rc = read_u32(&r, w, &fi->type); break;
        case 3: rc = read_i64(&r, w, &fi->size); break;
        case 4: rc = read_u32(&r, w, &fi->permissions); break;
        case 5: rc = read_i64(&r, w, &fi->modified_s); break;
        case 6: rc = read_bool(&r, w, &fi->deleted); break;
        case 7: rc = read_bool(&r, w, &fi->invalid); break;
        case 8: rc = read_bool(&r, w, &fi->no_permissions); break;
        case 9:
            rc = read_slice(&r, w, &sub);
            if (rc == 0)
                rc = count_field(&sub, 1, &fi->version_counters);
            break;
        case 10: rc = read_i64(&r, w, &fi->sequence); break;
        case 11: rc = read_i32(&r, w, &fi->modified_ns); break;
        case 12: rc = read_u64(&r, w, &fi->modified_by); break;
        case 13: rc = read_i32(&r, w, &fi->block_size); break;
        case 16:
            rc = read_slice(&r, w, &sub);
            fi->blocks++;
            break;
        case 17: rc = read_slice(&r, w, &fi->symlink_target); break;
        default: rc = pb_skip(&r, w);
        }
    }
    return rc ? -1 : finish(&r, n);
}

int bep_decode_index(const void *buf, size_t len, pb_slice *folder, int64_t *last_sequence,
                     int64_t *prev_sequence, void *ctx, bep_file_cb on_file)
{
    pb_reader r;
    uint32_t f;
    int w, n, rc = 0;
    memset(folder, 0, sizeof *folder);
    *last_sequence = *prev_sequence = 0;

    /* First pass: scalars and a full validation of every FileInfo, so a
     * malformed message produces no file reports at all. */
    pb_reader_init(&r, buf, len);
    while ((n = pb_next(&r, &f, &w)) == 1 && rc == 0) {
        pb_slice fs;
        bep_file fi;
        switch (f) {
        case 1: rc = read_slice(&r, w, folder); break;
        case 2:
            rc = read_slice(&r, w, &fs);
            if (rc == 0)
                rc = decode_file(&fs, &fi);
            break;
        case 3: rc = read_i64(&r, w, last_sequence); break;
        case 4: rc = read_i64(&r, w, prev_sequence); break;
        default: rc = pb_skip(&r, w);
        }
    }
    if (rc || finish(&r, n))
        return -1;

    pb_reader_init(&r, buf, len);
    while ((n = pb_next(&r, &f, &w)) == 1) {
        pb_slice fs;
        bep_file fi;
        if (f != 2) {
            pb_skip(&r, w);
            continue;
        }
        pb_bytes(&r, &fs);
        decode_file(&fs, &fi);
        fi.raw = fs;
        if (on_file && (rc = on_file(ctx, &fi)) != 0)
            return rc;
    }
    return 0;
}

int bep_decode_close(const void *buf, size_t len, pb_slice *reason)
{
    pb_reader r;
    uint32_t f;
    int w, n, rc = 0;
    memset(reason, 0, sizeof *reason);
    pb_reader_init(&r, buf, len);
    while ((n = pb_next(&r, &f, &w)) == 1 && rc == 0)
        rc = f == 1 ? read_slice(&r, w, reason) : pb_skip(&r, w);
    return rc ? -1 : finish(&r, n);
}

int bep_decode_request(const void *buf, size_t len, int32_t *id, pb_slice *folder, pb_slice *name)
{
    pb_reader r;
    uint32_t f;
    int w, n, rc = 0;
    *id = 0;
    memset(folder, 0, sizeof *folder);
    memset(name, 0, sizeof *name);
    pb_reader_init(&r, buf, len);
    while ((n = pb_next(&r, &f, &w)) == 1 && rc == 0) {
        switch (f) {
        case 1: rc = read_i32(&r, w, id); break;
        case 2: rc = read_slice(&r, w, folder); break;
        case 3: rc = read_slice(&r, w, name); break;
        default: rc = pb_skip(&r, w);
        }
    }
    return rc ? -1 : finish(&r, n);
}

int bep_decode_request_full(const void *buf, size_t len, bep_request *q)
{
    pb_reader r;
    uint32_t f;
    int w, n, rc = 0;
    memset(q, 0, sizeof *q);
    pb_reader_init(&r, buf, len);
    while ((n = pb_next(&r, &f, &w)) == 1 && rc == 0) {
        switch (f) {
        case 1: rc = read_i32(&r, w, &q->id); break;
        case 2: rc = read_slice(&r, w, &q->folder); break;
        case 3: rc = read_slice(&r, w, &q->name); break;
        case 4: rc = read_i64(&r, w, &q->offset); break;
        case 5: rc = read_i32(&r, w, &q->size); break;
        case 6: rc = read_slice(&r, w, &q->hash); break;
        case 7: rc = read_bool(&r, w, &q->from_temporary); break;
        case 9: rc = read_i32(&r, w, &q->block_no); break;
        default: rc = pb_skip(&r, w);
        }
    }
    return rc ? -1 : finish(&r, n);
}

/* ---- encoders ---- */

void bep_encode_response_data(pb_writer *w, int32_t id, const void *data, size_t len)
{
    pb_put_varint_field(w, 1, (uint64_t)(int64_t)id);
    pb_put_bytes_field(w, 2, data, len);
}

void bep_encode_hello(pb_writer *w, const char *device_name, const char *client_name,
                      const char *client_version, int64_t timestamp)
{
    pb_put_string_field(w, 1, device_name);
    pb_put_string_field(w, 2, client_name);
    pb_put_string_field(w, 3, client_version);
    pb_put_varint_field(w, 5, (uint64_t)timestamp);
}

void bep_encode_header(pb_writer *w, uint32_t type, uint32_t compression)
{
    pb_put_varint_field(w, 1, type);
    pb_put_varint_field(w, 2, compression);
}

void bep_encode_cluster_config(pb_writer *w, const char *folder_id, const char *folder_label,
                               uint32_t folder_type, const bep_cc_device_out *devices, size_t n)
{
    pb_writer folder;
    pb_writer_init(&folder, w->limit);
    pb_put_string_field(&folder, 1, folder_id);
    pb_put_string_field(&folder, 2, folder_label);
    pb_put_varint_field(&folder, 3, folder_type);
    for (size_t i = 0; i < n; i++) {
        pb_writer dev;
        pb_writer_init(&dev, w->limit);
        pb_put_bytes_field(&dev, 1, devices[i].id, DEVID_BYTES);
        pb_put_string_field(&dev, 2, devices[i].name);
        pb_put_varint_field(&dev, 6, (uint64_t)devices[i].max_sequence);
        pb_put_varint_field(&dev, 8, devices[i].index_id);
        pb_put_message_field(&folder, 16, &dev);
        pb_writer_free(&dev);
    }
    pb_put_message_field(w, 1, &folder);
    pb_writer_free(&folder);
}

void bep_encode_empty_index(pb_writer *w, const char *folder_id)
{
    /* No files and last_sequence 0, with the folder ID. Sent explicitly so
     * the peer is told we hold nothing for the folder, instead of leaving
     * it to infer that from max_sequence 0 in the ClusterConfig. */
    pb_put_string_field(w, 1, folder_id);
}

void bep_encode_close(pb_writer *w, const char *reason)
{
    pb_put_string_field(w, 1, reason);
}

void bep_encode_response_error(pb_writer *w, int32_t id, uint32_t code)
{
    pb_put_varint_field(w, 1, (uint64_t)(int64_t)id);
    pb_put_varint_field(w, 3, code);
}

int bep_file_blocks(pb_slice raw, void *ctx, bep_block_cb cb)
{
    pb_reader r;
    uint32_t f;
    int w, n, rc;
    pb_reader_init(&r, raw.data, raw.len);
    while ((n = pb_next(&r, &f, &w)) == 1) {
        if (f != 16) {
            if (pb_skip(&r, w) != 0)
                return -1;
            continue;
        }
        pb_slice bs, hash = { 0 };
        int64_t offset = 0;
        int32_t size = 0;
        if (read_slice(&r, w, &bs) != 0)
            return -1;
        pb_reader br;
        uint32_t bf;
        int bw, bn;
        rc = 0;
        pb_reader_init(&br, bs.data, bs.len);
        while ((bn = pb_next(&br, &bf, &bw)) == 1 && rc == 0) {
            switch (bf) {
            case 1: rc = read_i64(&br, bw, &offset); break;
            case 2: rc = read_i32(&br, bw, &size); break;
            case 3: rc = read_slice(&br, bw, &hash); break;
            default: rc = pb_skip(&br, bw);
            }
        }
        if (rc || finish(&br, bn))
            return -1;
        if ((rc = cb(ctx, offset, size, hash)) != 0)
            return rc;
    }
    return finish(&r, n);
}

int bep_decode_response(const void *buf, size_t len, int32_t *id, pb_slice *data, uint32_t *code)
{
    pb_reader r;
    uint32_t f;
    int w, n, rc = 0;
    *id = 0;
    *code = 0;
    memset(data, 0, sizeof *data);
    pb_reader_init(&r, buf, len);
    while ((n = pb_next(&r, &f, &w)) == 1 && rc == 0) {
        switch (f) {
        case 1: rc = read_i32(&r, w, id); break;
        case 2: rc = read_slice(&r, w, data); break;
        case 3: rc = read_u32(&r, w, code); break;
        default: rc = pb_skip(&r, w);
        }
    }
    return rc ? -1 : finish(&r, n);
}

void bep_encode_request(pb_writer *w, int32_t id, const char *folder, const char *name,
                        int64_t offset, int32_t size, const unsigned char *hash, size_t hash_len,
                        int32_t block_no)
{
    pb_put_varint_field(w, 1, (uint64_t)(int64_t)id);
    pb_put_string_field(w, 2, folder);
    pb_put_string_field(w, 3, name);
    pb_put_varint_field(w, 4, (uint64_t)offset);
    pb_put_varint_field(w, 5, (uint64_t)(int64_t)size);
    pb_put_bytes_field(w, 6, hash, hash_len);
    pb_put_varint_field(w, 9, (uint64_t)(int64_t)block_no);
}

void bep_encode_index_update_start(pb_writer *w, const char *folder, int64_t last_sequence,
                                   int64_t prev_sequence)
{
    pb_put_string_field(w, 1, folder);
    pb_put_varint_field(w, 3, (uint64_t)last_sequence);
    pb_put_varint_field(w, 4, (uint64_t)prev_sequence);
}

int bep_put_announced_file(pb_writer *w, pb_slice raw, int64_t sequence)
{
    pb_reader r;
    pb_writer fi;
    uint32_t f;
    int wt, n;
    pb_writer_init(&fi, w->limit);
    pb_reader_init(&r, raw.data, raw.len);
    for (;;) {
        const unsigned char *start = r.p;
        n = pb_next(&r, &f, &wt);
        if (n != 1 || pb_skip(&r, wt) != 0)
            break;
        /* Our own sequence replaces the peer's; fields from 1000 up are
         * local bookkeeping in bep.proto (local_flags, version_hash, ...)
         * and do not describe the file. */
        if (f == 10 || f >= 1000)
            continue;
        pb_put_raw(&fi, start, (size_t)(r.p - start));
    }
    if (n != 0 || r.error) {
        pb_writer_free(&fi);
        return -1;
    }
    pb_put_varint_field(&fi, 10, (uint64_t)sequence);
    pb_put_message_field(w, 2, &fi);
    int err = fi.error;
    pb_writer_free(&fi);
    return err ? -1 : 0;
}

int bep_decode_fileinfo(pb_slice raw, bep_file *out)
{
    if (decode_file(&raw, out) != 0)
        return -1;
    out->raw = raw;
    return 0;
}
