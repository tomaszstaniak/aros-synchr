#include "ctl.h"

#include "reach.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void add(char *out, size_t n, size_t *o, const char *fmt, ...)
{
    va_list ap;
    if (*o >= n)
        return;
    va_start(ap, fmt);
    int w = vsnprintf(out + *o, n - *o, fmt, ap);
    va_end(ap);
    if (w > 0)
        *o = (size_t)w >= n - *o ? n : *o + (size_t)w;
}

static const char *dash(const char *s)
{
    return s && *s ? s : "-";
}

/* Does the line start with the word (any case), followed by its end or a
 * space? Returns the rest, without leading spaces, or NULL. */
static const char *word(const char *line, const char *w)
{
    size_t i = 0;
    for (; w[i]; i++)
        if (toupper((unsigned char)line[i]) != w[i])
            return NULL;
    if (line[i] && !isspace((unsigned char)line[i]))
        return NULL;
    while (isspace((unsigned char)line[i]))
        i++;
    return line + i;
}

int ctl_command(const ctl_context *c, const char *line, char *out, size_t n)
{
    char buf[400];
    size_t o = 0, len;
    const char *arg;
    while (isspace((unsigned char)*line))
        line++;
    len = strlen(line);
    while (len > 0 && isspace((unsigned char)line[len - 1]))
        len--;
    if (len >= sizeof buf) {
        snprintf(out, n, "the command is too long");
        return 10;
    }
    memcpy(buf, line, len);
    buf[len] = 0;
    out[0] = 0;

    if ((arg = word(buf, "VERSION")) != NULL && !*arg) {
        snprintf(out, n, "aros-syncthing 0.0.1 control %d", CTL_VERSION);
        return 0;
    }
    if ((arg = word(buf, "STATUS")) != NULL && !*arg) {
        net_ms now = net_now();
        if (c->net) {
            char nl[400];
            reach_status(c->net, nl, sizeof nl);
            add(out, n, &o, "network %s\n", nl);
        }
        for (size_t i = 0; i < c->nlinks; i++) {
            link_status ls;
            link_get_status(c->links[i], now, &ls);
            add(out, n, &o, "device %s %s sessions=%ld retry=%ld last=%s address=%s via=%s addresses=%d\n",
                dash(ls.name), ls.state, ls.sessions, ls.retry_seconds, dash(ls.last_end), dash(ls.address),
                dash(ls.via), ls.addresses);
            for (size_t k = 0; k < ls.nfolders; k++) {
                link_folder_status fs;
                link_get_folder_status(c->links[i], k, &fs);
                add(out, n, &o, "folder %s %s device=%s why=%s received=%ld deleted=%ld conflicts=%ld held=%ld "
                    "failed=%ld in=%lld out=%lld\n", fs.id, fs.state, dash(ls.name), dash(fs.why), fs.received,
                    fs.deleted, fs.conflicts, fs.held, fs.failed, fs.bytes_in, fs.bytes_out);
            }
        }
        if (o > 0 && o < n)
            out[o - 1] = 0; /* no newline after the last line */
        return 0;
    }
    if ((arg = word(buf, "SCAN")) != NULL && *arg) {
        int all = word(arg, "ALL") != NULL && !*word(arg, "ALL"), found = 0;
        for (size_t i = 0; i < c->nlinks; i++) {
            link_status ls;
            link_get_status(c->links[i], 0, &ls);
            for (size_t k = 0; k < ls.nfolders; k++) {
                link_folder_status fs;
                link_get_folder_status(c->links[i], k, &fs);
                if (all || strcmp(fs.id, arg) == 0) {
                    link_scan_now(c->links[i], fs.id);
                    found = 1;
                }
            }
        }
        if (!found) {
            snprintf(out, n, "no folder \"%s\"", arg);
            return 5;
        }
        snprintf(out, n, "OK");
        return 0;
    }
    int pause = (arg = word(buf, "PAUSE")) != NULL;
    if ((pause || (arg = word(buf, "RESUME")) != NULL) && *arg) {
        int rc = -1;
        for (size_t i = 0; i < c->nlinks && rc < 0; i++)
            rc = link_pause(c->links[i], arg, pause);
        if (rc < 0) {
            snprintf(out, n, "no folder \"%s\"", arg);
            return 5;
        }
        snprintf(out, n, rc == 1 ? (pause ? "OK already paused" : "OK not paused") : "OK");
        return 0;
    }
    if ((arg = word(buf, "STOP")) != NULL && !*arg) {
        c->stop();
        snprintf(out, n, "OK");
        return 0;
    }
    snprintf(out, n, "not understood: %.60s", buf);
    return 10;
}
