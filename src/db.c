#include "db.h"

#include "bep.h"
#include "stfile.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAGIC "aros-syncthing-state 2"
#define MAGIC_V1 "aros-syncthing-state 1"

static void why_set(char *why, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, n, fmt, ap);
    va_end(ap);
}

/* ---- name index ---- */

static size_t hash_name(const char *s)
{
    size_t h = 2166136261u;
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

static int reindex(db *d, size_t want)
{
    size_t slots = 64;
    while (slots < want * 2)
        slots *= 2;
    size_t *s = calloc(slots, sizeof *s);
    if (!s)
        return -1;
    for (size_t i = 0; i < d->n; i++) {
        size_t h = hash_name(d->e[i].f.name) & (slots - 1);
        while (s[h])
            h = (h + 1) & (slots - 1);
        s[h] = i + 1;
    }
    free(d->slot);
    d->slot = s;
    d->slots = slots;
    return 0;
}

db_entry *db_find(db *d, const char *name)
{
    if (!d->slots)
        return NULL;
    for (size_t h = hash_name(name) & (d->slots - 1); d->slot[h]; h = (h + 1) & (d->slots - 1))
        if (strcmp(d->e[d->slot[h] - 1].f.name, name) == 0)
            return &d->e[d->slot[h] - 1];
    return NULL;
}

/* Takes ownership of *f (its fields move into the entry). */
static db_entry *add(db *d, finfo *f)
{
    if (d->n >= DB_MAX_ENTRIES)
        return NULL;
    if (d->n == d->cap) {
        size_t cap = d->cap ? d->cap * 2 : 32;
        db_entry *ne = realloc(d->e, cap * sizeof *ne);
        if (!ne)
            return NULL;
        d->e = ne;
        d->cap = cap;
    }
    if ((d->n + 1) * 2 > d->slots && reindex(d, d->n + 1) != 0)
        return NULL;
    db_entry *e = &d->e[d->n];
    memset(e, 0, sizeof *e);
    e->f = *f;
    memset(f, 0, sizeof *f);
    size_t h = hash_name(e->f.name) & (d->slots - 1);
    while (d->slot[h])
        h = (h + 1) & (d->slots - 1);
    d->slot[h] = ++d->n;
    return e;
}

db_entry *db_put(db *d, const finfo *f)
{
    finfo c;
    if (finfo_copy(&c, f) != 0)
        return NULL;
    db_entry *e = db_find(d, f->name);
    if (e) {
        finfo_free(&e->f);
        e->f = c;
        e->disk_size = e->disk_mtime_s = 0;
        e->disk_mtime_ns = 0;
        e->flags = 0;
        return e;
    }
    if (!(e = add(d, &c)))
        finfo_free(&c);
    return e;
}

void db_remove(db *d, const char *name)
{
    db_entry *e = db_find(d, name);
    if (!e)
        return;
    finfo_free(&e->f);
    *e = d->e[--d->n];
    reindex(d, d->n); /* rare: deletions are applied one by one */
}

int64_t db_next_sequence(db *d)
{
    return ++d->sequence;
}

void db_free(db *d)
{
    for (size_t i = 0; i < d->n; i++)
        finfo_free(&d->e[i].f);
    free(d->e);
    free(d->slot);
    memset(d, 0, sizeof *d);
}

/* ---- file ---- */

static int hex_line(char *line, size_t ll, const char *key, const char *want, char *why, size_t why_len)
{
    size_t kl = strlen(key);
    unsigned char *v;
    long vl;
    if (ll < kl + 1 || strncmp(line, key, kl) != 0 || line[kl] != ' '
        || (vl = stfile_hex_decode(line + kl + 1, ll - kl - 1, &v)) < 0) {
        why_set(why, why_len, "bad %s line", key);
        return -1;
    }
    int same = (size_t)vl == strlen(want) && memcmp(v, want, (size_t)vl) == 0;
    free(v);
    if (!same) {
        why_set(why, why_len, "recorded for another %s", strcmp(key, "dest") == 0 ? "folder path" : key);
        return -1;
    }
    return 0;
}

static int id_line(char *line, const char *key, const unsigned char *id, char *why, size_t why_len)
{
    char text[DEVID_TEXT_SIZE];
    devid_format(id, text);
    size_t kl = strlen(key);
    if (strncmp(line, key, kl) != 0 || line[kl] != ' ' || strcmp(line + kl + 1, text) != 0) {
        why_set(why, why_len, "recorded for another %s", key);
        return -1;
    }
    return 0;
}

static int u64_hex(const char *s, uint64_t *v)
{
    char *e;
    if (strlen(s) < 16 || (s[16] != 0 && s[16] != ' '))
        return -1;
    *v = strtoull(s, &e, 16);
    return e == s + 16 ? 0 : -1;
}

static int i64_dec(const char *s, int64_t *v, char **end)
{
    char *e;
    if (*s == 0 || *s == ' ')
        return -1;
    long long x = strtoll(s, &e, 10);
    if (e == s)
        return -1;
    *v = x;
    *end = e;
    return 0;
}

static int cmp_seq(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}

static int parse(char *buf, size_t len, const db_binding *b, db *out, char *why, size_t why_len)
{
    char *p = buf, *end = buf + len, *line, *e;
    size_t ll;
    if (!(line = stfile_line(&p, end, &ll))) {
        why_set(why, why_len, "empty");
        return -1;
    }
    if (strcmp(line, MAGIC_V1) == 0)
        return 2;
    if (strcmp(line, MAGIC) != 0) {
        why_set(why, why_len, "not a state file of a known version");
        return -1;
    }
    if (!(line = stfile_line(&p, end, &ll)) || hex_line(line, ll, "folder", b->folder_id, why, why_len) != 0)
        return -1;
    if (!(line = stfile_line(&p, end, &ll)) || hex_line(line, ll, "dest", b->dest, why, why_len) != 0)
        return -1;
    if (!(line = stfile_line(&p, end, &ll)) || id_line(line, "peer", b->peer_id, why, why_len) != 0)
        return -1;
    if (!(line = stfile_line(&p, end, &ll)) || id_line(line, "device", b->device_id, why, why_len) != 0)
        return -1;
    if (!(line = stfile_line(&p, end, &ll)) || strncmp(line, "index ", 6) != 0 || ll != 22
        || u64_hex(line + 6, &out->index_id) != 0 || out->index_id == 0) {
        why_set(why, why_len, "bad index line");
        return -1;
    }
    if (!(line = stfile_line(&p, end, &ll)) || strncmp(line, "sequence ", 9) != 0
        || i64_dec(line + 9, &out->sequence, &e) != 0 || *e || out->sequence < 0) {
        why_set(why, why_len, "bad sequence line");
        return -1;
    }
    if (!(line = stfile_line(&p, end, &ll)) || strncmp(line, "peerindex ", 10) != 0 || ll < 28
        || u64_hex(line + 10, &out->peer_index_id) != 0 || line[26] != ' '
        || i64_dec(line + 27, &out->peer_sequence, &e) != 0 || *e || out->peer_sequence < 0) {
        why_set(why, why_len, "bad peerindex line");
        return -1;
    }
    unsigned long count;
    if (!(line = stfile_line(&p, end, &ll)) || strncmp(line, "count ", 6) != 0
        || (count = strtoul(line + 6, &e, 10), *e || e == line + 6) || count > DB_MAX_ENTRIES) {
        why_set(why, why_len, "bad or too large entry count");
        return -1;
    }
    while ((line = stfile_line(&p, end, &ll))) {
        char *t[6];
        int k = 0;
        for (char *q = line; k < 6 && q;) {
            t[k++] = q;
            q = strchr(q, ' ');
            if (q)
                *q++ = 0;
        }
        if (k != 6 || strcmp(t[0], "entry") != 0) {
            why_set(why, why_len, "malformed entry line");
            return -1;
        }
        unsigned char *raw;
        long rl = stfile_hex_decode(t[1], strlen(t[1]), &raw);
        if (rl < 0) {
            why_set(why, why_len, "bad FileInfo");
            return -1;
        }
        finfo f;
        const char *fwhy;
        int rc = finfo_decode((pb_slice){ raw, (size_t)rl }, &f, &fwhy);
        free(raw);
        if (rc != 0) {
            why_set(why, why_len, "bad FileInfo: %s", fwhy);
            return -1;
        }
        int64_t ds, dm, dn, fl;
        if (i64_dec(t[2], &ds, &e) || *e || i64_dec(t[3], &dm, &e) || *e || i64_dec(t[4], &dn, &e) || *e
            || i64_dec(t[5], &fl, &e) || *e || fl < 0 || fl > 0xffff) {
            finfo_free(&f);
            why_set(why, why_len, "bad disk fields");
            return -1;
        }
        if ((f.type != BEP_FILE && f.type != BEP_DIRECTORY) || f.sequence < 1 || f.sequence > out->sequence
            || f.version.n == 0) {
            finfo_free(&f);
            why_set(why, why_len, "entry with an unknown type, a sequence out of range or no version");
            return -1;
        }
        if (out->n >= count || db_find(out, f.name)) {
            finfo_free(&f);
            why_set(why, why_len, out->n >= count ? "more entries than counted" : "duplicate entry");
            return -1;
        }
        db_entry *de = add(out, &f);
        if (!de) {
            finfo_free(&f);
            why_set(why, why_len, "out of memory");
            return -1;
        }
        de->disk_size = ds;
        de->disk_mtime_s = dm;
        de->disk_mtime_ns = (int32_t)dn;
        de->flags = (unsigned)fl;
    }
    if (p != end || out->n != count) {
        why_set(why, why_len, "entry count does not match");
        return -1;
    }
    /* A sequence names one record. */
    int64_t *seq = malloc((out->n ? out->n : 1) * sizeof *seq);
    if (!seq)
        return -1;
    for (size_t i = 0; i < out->n; i++)
        seq[i] = out->e[i].f.sequence;
    qsort(seq, out->n, sizeof *seq, cmp_seq);
    for (size_t i = 1; i < out->n; i++)
        if (seq[i] == seq[i - 1]) {
            free(seq);
            why_set(why, why_len, "two entries with the same sequence");
            return -1;
        }
    free(seq);
    return 0;
}

int db_load(const char *path, const db_binding *b, db *out, char *why, size_t why_len)
{
    char *buf = NULL;
    size_t len = 0;
    memset(out, 0, sizeof *out);
    int rc = stfile_read(path, &buf, &len, why, why_len);
    if (rc != 0)
        return rc;
    rc = parse(buf, len, b, out, why, why_len);
    free(buf);
    if (rc != 0)
        db_free(out);
    return rc;
}

int db_save(const char *path, const db_binding *b, const db *d, char *why, size_t why_len)
{
    pb_writer m;
    pb_writer_init(&m, STFILE_MAX);
    char text[DEVID_TEXT_SIZE], num[96];
    stfile_put_str(&m, MAGIC "\nfolder ");
    stfile_put_hex(&m, (const unsigned char *)b->folder_id, strlen(b->folder_id));
    stfile_put_str(&m, "\ndest ");
    stfile_put_hex(&m, (const unsigned char *)b->dest, strlen(b->dest));
    devid_format(b->peer_id, text);
    stfile_put_str(&m, "\npeer ");
    stfile_put_str(&m, text);
    devid_format(b->device_id, text);
    stfile_put_str(&m, "\ndevice ");
    stfile_put_str(&m, text);
    snprintf(num, sizeof num, "\nindex %016llx\nsequence %lld\npeerindex %016llx %lld\ncount %lu\n",
             (unsigned long long)d->index_id, (long long)d->sequence, (unsigned long long)d->peer_index_id,
             (long long)d->peer_sequence, (unsigned long)d->n);
    stfile_put_str(&m, num);
    pb_writer fi;
    pb_writer_init(&fi, STFILE_MAX);
    for (size_t i = 0; i < d->n && !m.error; i++) {
        const db_entry *e = &d->e[i];
        fi.len = 0;
        if (finfo_encode(&fi, &e->f) != 0) {
            m.error = 1;
            break;
        }
        stfile_put_str(&m, "entry ");
        stfile_put_hex(&m, fi.data, fi.len);
        snprintf(num, sizeof num, " %lld %lld %ld %u\n", (long long)e->disk_size, (long long)e->disk_mtime_s,
                 (long)e->disk_mtime_ns, e->flags);
        stfile_put_str(&m, num);
    }
    pb_writer_free(&fi);
    int rc = stfile_write(path, &m, why, why_len);
    pb_writer_free(&m);
    return rc;
}
