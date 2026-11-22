#include "https.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUEST_SECONDS 30
#define HEAD_MAX 8192

struct https_req {
    tls_conn *c;
    tls_log_fn log;
    char host[256];
    char *out;
    size_t out_len;
    char *in;
    size_t in_len, in_cap;
    int sent, done, status;
    size_t head_len; /* the header block, CRLF CRLF included */
    char *body;      /* the body, dechunked, NUL-terminated */
    size_t body_len;
    net_ms deadline;
};

int https_split(const char *url, char *host, size_t host_cap, unsigned *port, char *path, size_t path_cap)
{
    if (strncmp(url, "https://", 8) != 0)
        return -1;
    const char *h = url + 8;
    size_t hl = strcspn(h, ":/?");
    if (hl == 0 || hl >= host_cap)
        return -1;
    memcpy(host, h, hl);
    host[hl] = 0;
    const char *rest = h + hl;
    *port = 443;
    if (*rest == ':') {
        char *e;
        long p = strtol(rest + 1, &e, 10);
        if (e == rest + 1 || p < 1 || p > 65535)
            return -1;
        *port = (unsigned)p;
        rest = e;
    }
    if (*rest == 0)
        rest = "/";
    if (*rest != '/' && *rest != '?')
        return -1;
    if (snprintf(path, path_cap, "%s%s", *rest == '?' ? "/" : "", rest) >= (int)path_cap)
        return -1;
    return 0;
}

https_req *https_start(const tls_identity *id, const char *url, const char *method, const char *body, tls_log_fn log)
{
    char host[256], path[600], ip[NET_IPV4_TEXT];
    unsigned port;
    int err = 0;
    if (https_split(url, host, sizeof host, &port, path, sizeof path) != 0) {
        log("https: not a usable address: %s", url);
        return NULL;
    }
    if (net_resolve(host, ip, &err) != NET_OK) {
        log("https: cannot find %s (error %d)", host, err);
        return NULL;
    }
    https_req *r = calloc(1, sizeof *r);
    size_t blen = body ? strlen(body) : 0;
    size_t cap = strlen(method) + strlen(path) + strlen(host) + blen + 256;
    if (!r || !(r->out = malloc(cap))) {
        free(r);
        return NULL;
    }
    snprintf(r->host, sizeof r->host, "%s", host);
    r->log = log;
    if (body)
        r->out_len = (size_t)snprintf(r->out, cap,
                                      "%s %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: Synchr\r\n"
                                      "Content-Type: application/json\r\nContent-Length: %lu\r\n\r\n%s",
                                      method, path, host, (unsigned long)blen, body);
    else
        r->out_len = (size_t)snprintf(r->out, cap, "%s %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: Synchr\r\n\r\n", method,
                                      path, host);
    tls_opts o;
    memset(&o, 0, sizeof o);
    o.ip = ip;
    o.port = port;
    o.sock = -1;
    o.alpn = "http/1.1";
    o.check = TLS_CHECK_NONE;
    o.sni = net_is_ipv4(host) ? NULL : host;
    o.allow_tls12 = 1;
    if (tls_start_opts(id, &o, log, &r->c) != TLS_OK) {
        free(r->out);
        free(r);
        return NULL;
    }
    r->deadline = net_now() + REQUEST_SECONDS * 1000;
    return r;
}

/* The header block is in: status and the end of the headers. */
static int parse_head(https_req *r)
{
    char *end = NULL;
    for (size_t i = 0; i + 3 < r->in_len; i++)
        if (memcmp(r->in + i, "\r\n\r\n", 4) == 0) {
            end = r->in + i;
            break;
        }
    if (!end)
        return r->in_len > HEAD_MAX ? -1 : 0;
    int major, minor, status;
    if (sscanf(r->in, "HTTP/%d.%d %d", &major, &minor, &status) != 3)
        return -1;
    r->status = status;
    r->head_len = (size_t)(end - r->in) + 4;
    return 1;
}

static int same_ci(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return 0;
    return 1;
}

/* A header's value (the first one of that name), copied into out. */
static int header(const https_req *r, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    const char *p = r->in, *end = r->in + r->head_len;
    p = memchr(p, '\n', (size_t)(end - p));
    while (p && ++p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol)
            break;
        if ((size_t)(eol - p) > nl && same_ci(p, name, nl) && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (v < eol && (*v == ' ' || *v == '\t'))
                v++;
            size_t n = (size_t)(eol - v);
            while (n && (v[n - 1] == '\r' || v[n - 1] == ' '))
                n--;
            if (n >= cap)
                n = cap - 1;
            memcpy(out, v, n);
            out[n] = 0;
            return 1;
        }
        p = eol;
    }
    return 0;
}

/* Chunked transfer coding, decoded in place. */
static int dechunk(char *b, size_t len, size_t *out_len)
{
    size_t rd = 0, wr = 0;
    for (;;) {
        char *e;
        if (rd >= len)
            return -1;
        unsigned long n = strtoul(b + rd, &e, 16);
        char *eol = memchr(b + rd, '\n', len - rd);
        if (e == b + rd || !eol)
            return -1;
        rd = (size_t)(eol - b) + 1;
        if (n == 0)
            break;
        if (n > len - rd)
            return -1;
        memmove(b + wr, b + rd, n);
        wr += n;
        rd += n;
        if (rd < len && b[rd] == '\r')
            rd++;
        if (rd < len && b[rd] == '\n')
            rd++;
    }
    *out_len = wr;
    return 0;
}

static int finish(https_req *r)
{
    char v[64];
    if (!r->head_len && parse_head(r) != 1) {
        r->log("https: %s: no complete answer", r->host);
        return -1;
    }
    r->body = r->in + r->head_len;
    r->body_len = r->in_len - r->head_len;
    if (header(r, "Transfer-Encoding", v, sizeof v) && strstr(v, "chunked")) {
        if (dechunk(r->body, r->body_len, &r->body_len) != 0) {
            r->log("https: %s: malformed chunked answer", r->host);
            return -1;
        }
    } else if (header(r, "Content-Length", v, sizeof v)) {
        unsigned long n = strtoul(v, NULL, 10);
        if (n > r->body_len) {
            r->log("https: %s: the answer is cut short (%lu of %lu bytes)", r->host, (unsigned long)r->body_len, n);
            return -1;
        }
        r->body_len = n;
    }
    r->body[r->body_len] = 0;
    r->done = 1;
    return 1;
}

int https_step(https_req *r, net_ms now)
{
    if (r->done)
        return 1;
    if (now > r->deadline) {
        r->log("https: %s: no answer within %d s", r->host, REQUEST_SECONDS);
        return -1;
    }
    enum tls_result t = tls_connect_step(r->c);
    if (t == TLS_E_AGAIN)
        return 0;
    if (t != TLS_OK)
        return -1;
    if (!r->sent) {
        if (tls_queue(r->c, r->out, r->out_len) != TLS_OK)
            return -1;
        r->sent = 1;
    }
    if (tls_flush_some(r->c) != TLS_OK)
        return -1;
    for (;;) {
        if (r->in_cap - r->in_len < 4097) {
            size_t cap = r->in_cap ? r->in_cap * 2 : 16384;
            if (cap > HTTPS_MAX_BODY + HEAD_MAX + 1) {
                r->log("https: %s: the answer is too long", r->host);
                return -1;
            }
            char *n = realloc(r->in, cap);
            if (!n)
                return -1;
            r->in = n;
            r->in_cap = cap;
        }
        size_t got = 0;
        int closed = 0;
        t = tls_read_some(r->c, r->in + r->in_len, r->in_cap - r->in_len - 1, &got, &closed);
        if (t != TLS_OK) {
            /* HTTP/1.0: the end of the connection is the end of the answer
             * (some servers end without close_notify). */
            if (r->in_len > 0)
                return finish(r);
            r->log("https: %s: the connection ended without an answer", r->host);
            return -1;
        }
        if (got == 0)
            break;
        r->in_len += got;
        r->in[r->in_len] = 0;
    }
    /* A complete answer with a length need not wait for the close. */
    if (!r->head_len) {
        int h = parse_head(r);
        if (h < 0) {
            r->log("https: %s: not an HTTP answer", r->host);
            return -1;
        }
    }
    if (r->head_len) {
        char v[32];
        if (header(r, "Content-Length", v, sizeof v) && r->in_len - r->head_len >= strtoul(v, NULL, 10))
            return finish(r);
    }
    return 0;
}

int https_want(const https_req *r, int *sock, int *rd, int *wr)
{
    if (r->done)
        return 1;
    return tls_want(r->c, r->sent, sock, rd, wr);
}

net_ms https_deadline(const https_req *r)
{
    return r->deadline;
}

int https_status(const https_req *r)
{
    return r->status;
}

const char *https_body(const https_req *r, size_t *len)
{
    *len = r->body_len;
    return r->body ? r->body : "";
}

long https_header_seconds(const https_req *r, const char *name)
{
    char v[32];
    if (!r->head_len || !header(r, name, v, sizeof v))
        return -1;
    char *e;
    long s = strtol(v, &e, 10);
    return e == v || s < 0 ? -1 : s;
}

void https_free(https_req *r)
{
    if (!r)
        return;
    if (r->c)
        tls_abort(r->c);
    free(r->out);
    free(r->in);
    free(r);
}

/* ---- JSON strings ---- */

static size_t skip_ws(const char *s, size_t i, size_t len)
{
    while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
        i++;
    return i;
}

/* The string starting at s[i] == '"': decoded into out (cap), *next after
 * it. 0, 1 when it is longer than cap (passed over), -1 malformed. */
static int jstring(const char *s, size_t i, size_t len, char *out, size_t cap, size_t *next)
{
    size_t n = 0;
    int too_long = 0;
    for (i++; i < len; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '"') {
            out[n < cap ? n : cap - 1] = 0;
            *next = i + 1;
            return too_long;
        }
        if (ch == '\\') {
            if (++i >= len)
                return -1;
            switch (s[i]) {
            case '"': ch = '"'; break;
            case '\\': ch = '\\'; break;
            case '/': ch = '/'; break;
            case 'b': ch = '\b'; break;
            case 'f': ch = '\f'; break;
            case 'n': ch = '\n'; break;
            case 'r': ch = '\r'; break;
            case 't': ch = '\t'; break;
            case 'u': {
                unsigned v = 0;
                if (i + 4 >= len)
                    return -1;
                for (int k = 1; k <= 4; k++) {
                    char h = s[i + k];
                    if (!isxdigit((unsigned char)h))
                        return -1;
                    v = v * 16 + (unsigned)(isdigit((unsigned char)h) ? h - '0' : (tolower((unsigned char)h) - 'a' + 10));
                }
                i += 4;
                ch = v >= 32 && v < 127 ? (unsigned char)v : '?';
                break;
            }
            default:
                return -1;
            }
        } else if (ch < 32) {
            return -1;
        } else if (ch >= 128) {
            ch = '?';
        }
        if (n + 1 < cap)
            out[n++] = (char)ch;
        else
            too_long = 1;
    }
    return -1;
}

int json_strings(const char *json, size_t len, const char *key, void (*fn)(void *ctx, const char *value), void *ctx)
{
    static char buf[1024];
    int calls = 0;
    size_t i = 0;
    while (i < len) {
        if (json[i] != '"') {
            i++;
            continue;
        }
        size_t next;
        int r = jstring(json, i, len, buf, sizeof buf, &next);
        if (r < 0)
            return calls;
        i = skip_ws(json, next, len);
        if (i >= len || json[i] != ':' || r != 0 || strcmp(buf, key) != 0)
            continue; /* a value, or another key */
        i = skip_ws(json, i + 1, len);
        if (i < len && json[i] == '"') {
            if ((r = jstring(json, i, len, buf, sizeof buf, &next)) < 0)
                return calls;
            if (r == 0)
                fn(ctx, buf), calls++;
            i = next;
        } else if (i < len && json[i] == '[') {
            i = skip_ws(json, i + 1, len);
            while (i < len && json[i] == '"') {
                if ((r = jstring(json, i, len, buf, sizeof buf, &next)) < 0)
                    return calls;
                if (r == 0)
                    fn(ctx, buf), calls++;
                i = skip_ws(json, next, len);
                if (i < len && json[i] == ',')
                    i = skip_ws(json, i + 1, len);
            }
        }
    }
    return calls;
}
