#include "state.h"

#include "pb.h"
#include "stfile.h"

#include <openssl/evp.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAGIC "aros-syncthing-state 1"

static void why_set(char *why, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(why, n, fmt, ap);
    va_end(ap);
}

static int sha256(const void *data, size_t len, unsigned char out[32])
{
    unsigned int n = 0;
    return EVP_Digest(data, len, out, &n, EVP_sha256(), NULL) && n == 32 ? 0 : -1;
}

int state_path(const char *identity_dir, const char *folder_id, char *out, size_t n)
{
    unsigned char h[32];
    char hex[33];
    if (sha256(folder_id, strlen(folder_id), h) != 0)
        return -1;
    for (int i = 0; i < 16; i++)
        snprintf(hex + 2 * i, 3, "%02x", h[i]);
    size_t l = strlen(identity_dir);
    const char *sep = (l == 0 || identity_dir[l - 1] == '/' || identity_dir[l - 1] == ':') ? "" : "/";
    int r = snprintf(out, n, "%s%sstate-%s.txt", identity_dir, sep, hex);
    return r > 0 && (size_t)r < n ? 0 : -1;
}

/* ---- list ---- */

state_entry *state_find(state_list *l, const char *name)
{
    for (size_t i = 0; i < l->n; i++)
        if (strcmp(l->e[i].name, name) == 0)
            return &l->e[i];
    return NULL;
}

int state_put(state_list *l, const char *name, int is_dir, const unsigned char *raw, size_t raw_len)
{
    unsigned char *copy = malloc(raw_len ? raw_len : 1);
    if (!copy)
        return -1;
    memcpy(copy, raw, raw_len);
    state_entry *e = state_find(l, name);
    if (e) {
        free(e->raw);
        e->raw = copy;
        e->raw_len = raw_len;
        e->is_dir = is_dir;
        return 0;
    }
    if (l->n >= STATE_MAX_ENTRIES) {
        free(copy);
        return -1;
    }
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 32;
        state_entry *ne = realloc(l->e, cap * sizeof *ne);
        if (!ne) {
            free(copy);
            return -1;
        }
        l->e = ne;
        l->cap = cap;
    }
    if (!(l->e[l->n].name = strdup(name))) {
        free(copy);
        return -1;
    }
    l->e[l->n].is_dir = is_dir;
    l->e[l->n].raw = copy;
    l->e[l->n].raw_len = raw_len;
    l->e[l->n].status = 0;
    l->n++;
    return 0;
}

/* Appends without looking for an existing name (duplicates are found
 * afterwards, by sorting). Takes ownership of name and raw. */
static int append(state_list *l, char *name, int is_dir, unsigned char *raw, size_t raw_len)
{
    if (l->n >= STATE_MAX_ENTRIES)
        return -1;
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 32;
        state_entry *ne = realloc(l->e, cap * sizeof *ne);
        if (!ne)
            return -1;
        l->e = ne;
        l->cap = cap;
    }
    l->e[l->n].name = name;
    l->e[l->n].is_dir = is_dir;
    l->e[l->n].raw = raw;
    l->e[l->n].raw_len = raw_len;
    l->e[l->n].status = 0;
    l->n++;
    return 0;
}

void state_remove(state_list *l, const char *name)
{
    state_entry *e = state_find(l, name);
    if (!e)
        return;
    free(e->name);
    free(e->raw);
    *e = l->e[--l->n];
}

void state_list_free(state_list *l)
{
    for (size_t i = 0; i < l->n; i++) {
        free(l->e[i].name);
        free(l->e[i].raw);
    }
    free(l->e);
    memset(l, 0, sizeof *l);
}

/* ---- reading ---- */

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int parse(char *buf, size_t len, const char *folder_id, const char *dest,
                 const unsigned char peer[DEVID_BYTES], state_list *out, char *why, size_t why_len)
{
    char *p = buf, *end = buf + len, *line;
    size_t ll;

    if (!(line = stfile_line(&p, end, &ll)) || strcmp(line, MAGIC) != 0) {
        why_set(why, why_len, "not a version 1 state file");
        return -1;
    }
    unsigned char *v;
    long vl;
    if (!(line = stfile_line(&p, end, &ll)) || strncmp(line, "folder ", 7) != 0
        || (vl = stfile_hex_decode(line + 7, ll - 7, &v)) < 0) {
        why_set(why, why_len, "bad folder line");
        return -1;
    }
    int same = (size_t)vl == strlen(folder_id) && memcmp(v, folder_id, (size_t)vl) == 0;
    free(v);
    if (!same) {
        why_set(why, why_len, "recorded for another folder");
        return -1;
    }
    if (!(line = stfile_line(&p, end, &ll)) || strncmp(line, "dest ", 5) != 0
        || (vl = stfile_hex_decode(line + 5, ll - 5, &v)) < 0) {
        why_set(why, why_len, "bad dest line");
        return -1;
    }
    same = (size_t)vl == strlen(dest) && memcmp(v, dest, (size_t)vl) == 0;
    free(v);
    if (!same) {
        why_set(why, why_len, "recorded for another destination directory");
        return -1;
    }
    char peer_text[DEVID_TEXT_SIZE];
    devid_format(peer, peer_text);
    if (!(line = stfile_line(&p, end, &ll)) || strncmp(line, "peer ", 5) != 0 || strcmp(line + 5, peer_text) != 0) {
        why_set(why, why_len, "recorded for another peer");
        return -1;
    }
    char *num_end;
    if (!(line = stfile_line(&p, end, &ll)) || strncmp(line, "count ", 6) != 0) {
        why_set(why, why_len, "bad count line");
        return -1;
    }
    unsigned long count = strtoul(line + 6, &num_end, 10);
    if (*num_end || num_end == line + 6 || count > STATE_MAX_ENTRIES) {
        why_set(why, why_len, "bad or too large entry count");
        return -1;
    }

    while ((line = stfile_line(&p, end, &ll))) {
        /* entry <file|dir> <hex name> <hex fileinfo> */
        char *t = line + 6, *sp1, *sp2;
        if (strncmp(line, "entry ", 6) != 0 || !(sp1 = strchr(t, ' ')) || !(sp2 = strchr(sp1 + 1, ' '))
            || strchr(sp2 + 1, ' ')) {
            why_set(why, why_len, "malformed entry line");
            return -1;
        }
        int is_dir;
        if ((size_t)(sp1 - t) == 4 && strncmp(t, "file", 4) == 0)
            is_dir = 0;
        else if ((size_t)(sp1 - t) == 3 && strncmp(t, "dir", 3) == 0)
            is_dir = 1;
        else {
            why_set(why, why_len, "unknown entry type");
            return -1;
        }
        unsigned char *name, *raw;
        long nl = stfile_hex_decode(sp1 + 1, (size_t)(sp2 - sp1 - 1), &name);
        if (nl <= 0 || memchr(name, 0, (size_t)nl)) {
            if (nl >= 0)
                free(name);
            why_set(why, why_len, "bad entry name");
            return -1;
        }
        size_t rawhex = strlen(sp2 + 1);
        long rl = rawhex / 2 <= STATE_MAX_FILEINFO ? stfile_hex_decode(sp2 + 1, rawhex, &raw) : -1;
        if (rl < 0) {
            free(name);
            why_set(why, why_len, "bad or too large FileInfo");
            return -1;
        }
        if (out->n >= count) {
            free(name);
            free(raw);
            why_set(why, why_len, "more entries than counted");
            return -1;
        }
        if (append(out, (char *)name, is_dir, raw, (size_t)rl) != 0) {
            free(name);
            free(raw);
            why_set(why, why_len, "out of memory");
            return -1;
        }
    }
    if (p != end || out->n != count) {
        why_set(why, why_len, "entry count does not match");
        return -1;
    }
    /* No two entries may have the same name. */
    char **names = malloc((out->n ? out->n : 1) * sizeof *names);
    if (!names)
        return -1;
    for (size_t i = 0; i < out->n; i++)
        names[i] = out->e[i].name;
    qsort(names, out->n, sizeof *names, cmp_names);
    for (size_t i = 1; i < out->n; i++)
        if (strcmp(names[i - 1], names[i]) == 0) {
            free(names);
            why_set(why, why_len, "duplicate entry");
            return -1;
        }
    free(names);
    return 0;
}

int state_load(const char *path, const char *folder_id, const char *dest,
               const unsigned char peer[DEVID_BYTES], state_list *out, char *why, size_t why_len)
{
    size_t len = 0;
    char *buf = NULL;
    memset(out, 0, sizeof *out);
    int rc = stfile_read(path, &buf, &len, why, why_len);
    if (rc != 0)
        return rc;
    rc = parse(buf, len, folder_id, dest, peer, out, why, why_len);
    free(buf);
    if (rc != 0)
        state_list_free(out);
    return rc;
}

/* ---- writing ---- */

int state_save(const char *path, const char *folder_id, const char *dest,
               const unsigned char peer[DEVID_BYTES], const state_list *l, char *why, size_t why_len)
{
    /* Built in memory so the checksum covers exactly what is written. */
    pb_writer m;
    pb_writer_init(&m, STATE_MAX_FILE);
    char peer_text[DEVID_TEXT_SIZE], num[32];
    devid_format(peer, peer_text);
    stfile_put_str(&m, MAGIC "\nfolder ");
    stfile_put_hex(&m, (const unsigned char *)folder_id, strlen(folder_id));
    stfile_put_str(&m, "\ndest ");
    stfile_put_hex(&m, (const unsigned char *)dest, strlen(dest));
    stfile_put_str(&m, "\npeer ");
    stfile_put_str(&m, peer_text);
    snprintf(num, sizeof num, "\ncount %lu\n", (unsigned long)l->n);
    stfile_put_str(&m, num);
    for (size_t i = 0; i < l->n; i++) {
        stfile_put_str(&m, l->e[i].is_dir ? "entry dir " : "entry file ");
        stfile_put_hex(&m, (const unsigned char *)l->e[i].name, strlen(l->e[i].name));
        stfile_put_str(&m, " ");
        stfile_put_hex(&m, l->e[i].raw, l->e[i].raw_len);
        stfile_put_str(&m, "\n");
    }
    int rc = stfile_write(path, &m, why, why_len);
    pb_writer_free(&m);
    return rc;
}
