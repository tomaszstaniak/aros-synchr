#include "stfile.h"

#include "absent.h"

#include <openssl/evp.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>

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

void stfile_put_str(pb_writer *w, const char *s)
{
    pb_put_raw(w, s, strlen(s));
}

void stfile_put_hex(pb_writer *w, const unsigned char *p, size_t n)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        char two[2] = { digits[p[i] >> 4], digits[p[i] & 15] };
        pb_put_raw(w, two, 2);
    }
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1; /* only lower case is written, so only lower case is read */
}

long stfile_hex_decode(const char *s, size_t len, unsigned char **out)
{
    if (len % 2)
        return -1;
    unsigned char *b = malloc(len / 2 + 1);
    if (!b)
        return -1;
    for (size_t i = 0; i < len; i += 2) {
        int hi = hexval(s[i]), lo = hexval(s[i + 1]);
        if (hi < 0 || lo < 0) {
            free(b);
            return -1;
        }
        b[i / 2] = (unsigned char)(hi << 4 | lo);
    }
    b[len / 2] = 0;
    *out = b;
    return (long)(len / 2);
}

char *stfile_line(char **p, char *end, size_t *len)
{
    char *s = *p;
    if (s >= end)
        return NULL;
    char *nl = memchr(s, '\n', (size_t)(end - s));
    if (!nl)
        return NULL;
    *nl = 0;
    *len = (size_t)(nl - s);
    *p = nl + 1;
    return s;
}

static char *read_all(const char *path, size_t *len, int *missing)
{
    *missing = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        /* Absence is not taken from the errno of the failed open (see
         * absent.h): a missing state would be read as a damaged one. */
        struct stat st;
        errno = 0;
        *missing = stat(path, &st) != 0 && path_absent(path, errno);
        return NULL;
    }
    size_t cap = 65536, n = 0;
    char *b = malloc(cap + 1);
    while (b) {
        if (n == cap) {
            if (cap >= STFILE_MAX) {
                free(b);
                b = NULL;
                break;
            }
            cap *= 2;
            char *nb = realloc(b, cap + 1);
            if (!nb) {
                free(b);
                b = NULL;
                break;
            }
            b = nb;
        }
        size_t r = fread(b + n, 1, cap - n, f);
        n += r;
        if (r == 0)
            break;
    }
    if (b && ferror(f)) {
        free(b);
        b = NULL;
    }
    fclose(f);
    if (b) {
        b[n] = 0;
        *len = n;
    }
    return b;
}

int stfile_read(const char *path, char **out, size_t *out_len, char *why, size_t why_len)
{
    size_t len = 0;
    int missing;
    char *buf = read_all(path, &len, &missing);
    if (!buf && missing) {
        char old[1100];
        snprintf(old, sizeof old, "%s.old", path);
        buf = read_all(old, &len, &missing);
        if (!buf && missing)
            return 1;
    }
    if (!buf) {
        why_set(why, why_len, "cannot read (or larger than %u bytes)", STFILE_MAX);
        return -1;
    }
    if (memchr(buf, 0, len)) {
        why_set(why, why_len, "contains a NUL byte");
        free(buf);
        return -1;
    }
    /* The checksum covers every byte before the "end " line. */
    char *p = buf, *end = buf + len, *endline = NULL;
    for (char *q = end - 1; q > p; q--)
        if (q[-1] == '\n' && strncmp(q, "end ", 4) == 0) {
            endline = q;
            break;
        }
    if (!endline || end[-1] != '\n') {
        why_set(why, why_len, "no end line (truncated)");
        free(buf);
        return -1;
    }
    unsigned char sum[32];
    char sumhex[65];
    if (sha256(buf, (size_t)(endline - p), sum) != 0) {
        why_set(why, why_len, "checksum could not be computed");
        free(buf);
        return -1;
    }
    for (int i = 0; i < 32; i++)
        snprintf(sumhex + 2 * i, 3, "%02x", sum[i]);
    if ((size_t)(end - endline) != 4 + 64 + 1 || strncmp(endline + 4, sumhex, 64) != 0) {
        why_set(why, why_len, "checksum mismatch");
        free(buf);
        return -1;
    }
    *endline = 0;
    *out = buf;
    *out_len = (size_t)(endline - p);
    return 0;
}

int stfile_write(const char *path, const pb_writer *m, char *why, size_t why_len)
{
    char tmp[1100], old[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    snprintf(old, sizeof old, "%s.old", path);
    unsigned char sum[32];
    if (m->error || sha256(m->data, m->len, sum) != 0) {
        why_set(why, why_len, "state too large or out of memory");
        return -1;
    }
    char sumline[4 + 64 + 2];
    memcpy(sumline, "end ", 4);
    for (int i = 0; i < 32; i++)
        snprintf(sumline + 4 + 2 * i, 3, "%02x", sum[i]);
    sumline[68] = '\n';

    FILE *f = fopen(tmp, "wb");
    int bad = !f;
    if (f) {
        bad |= fwrite(m->data, 1, m->len, f) != m->len;
        bad |= fwrite(sumline, 1, 69, f) != 69;
        bad |= fflush(f) != 0 || ferror(f);
        bad |= fclose(f) != 0;
    }
    if (bad) {
        why_set(why, why_len, "writing %s failed (errno %d)", tmp, errno);
        remove(tmp);
        return -1;
    }
    struct stat st;
    int had = stat(path, &st) == 0;
    remove(old);
#ifdef TW_TEST_HOOKS
    /* A crash after the new state is written next to the old one, before
     * it takes its place (host tests of interruptions). */
    if (getenv("STPOC_TEST_EXIT_IN_STATE_WRITE"))
        _exit(137);
#endif
    if (had && rename(path, old) != 0) {
        why_set(why, why_len, "cannot move %s aside (errno %d); it was not changed", path, errno);
        remove(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        int e = errno;
        remove(tmp);
        if (had && rename(old, path) != 0)
            why_set(why, why_len, "rename to %s failed (errno %d); previous state kept as %s", path, e, old);
        else
            why_set(why, why_len, "rename to %s failed (errno %d); previous state kept", path, e);
        return -1;
    }
    remove(old);
    return 0;
}
