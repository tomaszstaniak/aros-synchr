#include "conf.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_MAX_LEN 1200

static char *dup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

void conf_free(conf *c)
{
    for (size_t i = 0; i < c->ndevices; i++) {
        free(c->devices[i].name);
        free(c->devices[i].ip);
    }
    for (size_t i = 0; i < c->nfolders; i++) {
        free(c->folders[i].id);
        free(c->folders[i].path);
    }
    free(c->devices);
    free(c->folders);
    free(c->name);
    free(c->discovery_server);
    free(c->relay);
    memset(c, 0, sizeof *c);
}

void conf_init(conf *c)
{
    memset(c, 0, sizeof *c);
    c->listen_port = CONF_LISTEN_DEFAULT;
    c->global_discovery = c->relays = c->local_announce = 1;
}

static int has_space(const char *s)
{
    for (; *s; s++)
        if (isspace((unsigned char)*s))
            return 1;
    return 0;
}

/* "a.b.c.d:port", numbers only. */
int conf_allow_no_folder;

static int parse_address(const char *v, char **ip, unsigned *port)
{
    unsigned a, b, c, d, p;
    char tail;
    if (sscanf(v, "%3u.%3u.%3u.%3u:%5u%c", &a, &b, &c, &d, &p, &tail) != 5)
        return -1;
    if (a > 255 || b > 255 || c > 255 || d > 255 || p < 1 || p > 65535)
        return -1;
    char text[32];
    snprintf(text, sizeof text, "%u.%u.%u.%u", a, b, c, d);
    if (!(*ip = dup(text)))
        return -1;
    *port = p;
    return 0;
}

/* Paths are compared as the file systems here do: without regard to
 * case, and without a trailing separator. */
static int same_path(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    while (la > 1 && a[la - 1] == '/')
        la--;
    while (lb > 1 && b[lb - 1] == '/')
        lb--;
    if (la != lb)
        return 0;
    for (size_t i = 0; i < la; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return 0;
    return 1;
}

struct pending {
    char *device; /* a folder's device name, resolved at the end */
    int line;
    int has_id, has_address, has_path;
};

int conf_parse(const char *text, conf *c, char *why, size_t n)
{
    enum { TOP, DEVICE, FOLDER } in = TOP;
    struct pending *fp = NULL; /* one per folder */
    struct pending dev = { NULL, 0, 0, 0, 0 };
    int line = 0, rc = -1;
    conf_init(c);
#define FAIL(...) do { snprintf(why, n, __VA_ARGS__); goto out; } while (0)
#define END_DEVICE() do { \
        if (in == DEVICE && !dev.has_id) FAIL("line %d: device \"%s\" has no id", dev.line, c->devices[c->ndevices - 1].name); \
    } while (0)
#define END_FOLDER() do { \
        if (in == FOLDER && !fp[c->nfolders - 1].has_path) FAIL("line %d: folder \"%s\" has no path", fp[c->nfolders - 1].line, c->folders[c->nfolders - 1].id); \
        if (in == FOLDER && !fp[c->nfolders - 1].device) FAIL("line %d: folder \"%s\" names no device", fp[c->nfolders - 1].line, c->folders[c->nfolders - 1].id); \
    } while (0)

    while (*text) {
        char buf[LINE_MAX_LEN];
        size_t len = strcspn(text, "\n");
        line++;
        if (len >= sizeof buf)
            FAIL("line %d: too long", line);
        memcpy(buf, text, len);
        buf[len] = 0;
        text += len + (text[len] == '\n');

        char *key = buf;
        while (isspace((unsigned char)*key))
            key++;
        size_t e = strlen(key);
        while (e > 0 && isspace((unsigned char)key[e - 1]))
            key[--e] = 0;
        if (!*key || *key == '#')
            continue;
        char *v = key;
        while (*v && !isspace((unsigned char)*v))
            v++;
        if (*v) {
            *v++ = 0;
            while (isspace((unsigned char)*v))
                v++;
        }
        if (!*v)
            FAIL("line %d: \"%s\" has no value", line, key);

        if (strcmp(key, "device") == 0) {
            END_DEVICE();
            END_FOLDER();
            if (has_space(v))
                FAIL("line %d: a device name has no spaces", line);
            for (size_t i = 0; i < c->ndevices; i++)
                if (strcmp(c->devices[i].name, v) == 0)
                    FAIL("line %d: device \"%s\" is defined twice", line, v);
            if (c->ndevices >= CONF_MAX_DEVICES)
                FAIL("line %d: more than %d devices", line, CONF_MAX_DEVICES);
            conf_device *nd = realloc(c->devices, (c->ndevices + 1) * sizeof *nd);
            if (!nd)
                FAIL("out of memory");
            c->devices = nd;
            memset(&nd[c->ndevices], 0, sizeof *nd);
            if (!(nd[c->ndevices].name = dup(v)))
                FAIL("out of memory");
            c->ndevices++;
            in = DEVICE;
            dev.line = line;
            dev.has_id = dev.has_address = 0;
        } else if (strcmp(key, "folder") == 0) {
            END_DEVICE();
            END_FOLDER();
            if (has_space(v))
                FAIL("line %d: a folder ID has no spaces", line);
            for (size_t i = 0; i < c->nfolders; i++)
                if (strcmp(c->folders[i].id, v) == 0)
                    FAIL("line %d: folder \"%s\" is defined twice", line, v);
            if (c->nfolders >= CONF_MAX_FOLDERS)
                FAIL("line %d: more than %d folders", line, CONF_MAX_FOLDERS);
            conf_folder *nf = realloc(c->folders, (c->nfolders + 1) * sizeof *nf);
            struct pending *np = nf ? realloc(fp, (c->nfolders + 1) * sizeof *np) : NULL;
            if (nf)
                c->folders = nf;
            if (np)
                fp = np;
            if (!nf || !np)
                FAIL("out of memory");
            memset(&nf[c->nfolders], 0, sizeof *nf);
            memset(&np[c->nfolders], 0, sizeof *np);
            np[c->nfolders].line = line;
            nf[c->nfolders].rescan_seconds = CONF_RESCAN_DEFAULT;
            if (!(nf[c->nfolders].id = dup(v)))
                FAIL("out of memory");
            c->nfolders++;
            in = FOLDER;
        } else if (in == TOP && strcmp(key, "name") == 0) {
            if (c->name)
                FAIL("line %d: name is given twice", line);
            if (!(c->name = dup(v)))
                FAIL("out of memory");
        } else if (in == TOP && strcmp(key, "listen") == 0) {
            char *end;
            long p = strtol(v, &end, 10);
            if (strcmp(v, "off") == 0)
                c->listen_port = 0;
            else if (*end || p < 1 || p > 65535)
                FAIL("line %d: listen is a port number (1 to 65535) or off", line);
            else
                c->listen_port = (unsigned)p;
        } else if (in == TOP && (strcmp(key, "global-discovery") == 0 || strcmp(key, "relays") == 0
                                 || strcmp(key, "local-announce") == 0)) {
            int on = strcmp(v, "on") == 0;
            if (!on && strcmp(v, "off") != 0)
                FAIL("line %d: %s is on or off", line, key);
            *(key[0] == 'g' ? &c->global_discovery : key[0] == 'r' ? &c->relays : &c->local_announce) = on;
        } else if (in == TOP && strcmp(key, "discovery-server") == 0) {
            if (c->discovery_server || strncmp(v, "https://", 8) != 0 || has_space(v))
                FAIL("line %d: discovery-server is one https:// address", line);
            if (!(c->discovery_server = dup(v)))
                FAIL("out of memory");
        } else if (in == TOP && strcmp(key, "relay") == 0) {
            if (c->relay || strncmp(v, "relay://", 8) != 0 || has_space(v))
                FAIL("line %d: relay is one relay:// address", line);
            if (!(c->relay = dup(v)))
                FAIL("out of memory");
        } else if (in == DEVICE && strcmp(key, "id") == 0) {
            conf_device *d = &c->devices[c->ndevices - 1];
            if (dev.has_id)
                FAIL("line %d: id is given twice", line);
            if (devid_parse(v, d->id) != 0)
                FAIL("line %d: \"%s\" is not a Device ID", line, v);
            dev.has_id = 1;
        } else if (in == DEVICE && strcmp(key, "address") == 0) {
            conf_device *d = &c->devices[c->ndevices - 1];
            if (dev.has_address)
                FAIL("line %d: address is given twice", line);
            /* `dynamic`: ip stays NULL and port 0, as without the line. */
            if (strcmp(v, "dynamic") != 0 && parse_address(v, &d->ip, &d->port) != 0)
                FAIL("line %d: \"%s\" is not an address of the form 192.168.1.10:22000, nor the word dynamic", line,
                     v);
            dev.has_address = 1;
        } else if (in == FOLDER && strcmp(key, "path") == 0) {
            conf_folder *f = &c->folders[c->nfolders - 1];
            if (f->path)
                FAIL("line %d: path is given twice", line);
            if (!(f->path = dup(v)))
                FAIL("out of memory");
            fp[c->nfolders - 1].has_path = 1;
        } else if (in == FOLDER && strcmp(key, "with") == 0) {
            if (fp[c->nfolders - 1].device)
                FAIL("line %d: a folder is shared with one device in this version", line);
            if (!(fp[c->nfolders - 1].device = dup(v)))
                FAIL("out of memory");
        } else if (in == FOLDER && strcmp(key, "rescan") == 0) {
            char *end;
            long s = strtol(v, &end, 10);
            if (*end || s < 1 || s > 86400)
                FAIL("line %d: rescan is a number of seconds from 1 to 86400", line);
            c->folders[c->nfolders - 1].rescan_seconds = s;
        } else {
            FAIL("line %d: \"%s\" is not known here", line, key);
        }
    }
    END_DEVICE();
    END_FOLDER();
    if (c->nfolders == 0 && !conf_allow_no_folder)
        FAIL("no folder is defined");
    for (size_t i = 0; i < c->nfolders; i++) {
        size_t d;
        for (d = 0; d < c->ndevices; d++)
            if (strcmp(c->devices[d].name, fp[i].device) == 0)
                break;
        if (d == c->ndevices)
            FAIL("line %d: folder \"%s\" names device \"%s\", which is not defined", fp[i].line, c->folders[i].id,
                 fp[i].device);
        c->folders[i].device = d;
        for (size_t j = 0; j < i; j++)
            if (same_path(c->folders[i].path, c->folders[j].path))
                FAIL("line %d: folders \"%s\" and \"%s\" have the same path", fp[i].line, c->folders[j].id,
                     c->folders[i].id);
    }
    for (size_t i = 0; i < c->ndevices; i++)
        for (size_t j = 0; j < i; j++)
            if (memcmp(c->devices[i].id, c->devices[j].id, DEVID_BYTES) == 0)
                FAIL("devices \"%s\" and \"%s\" have the same Device ID", c->devices[j].name, c->devices[i].name);
    if (!c->name && !(c->name = dup("aros")))
        FAIL("out of memory");
    rc = 0;
out:
    for (size_t i = 0; fp && i < c->nfolders; i++)
        free(fp[i].device);
    free(fp);
    if (rc != 0)
        conf_free(c);
    return rc;
#undef FAIL
#undef END_DEVICE
#undef END_FOLDER
}

int conf_load(const char *path, conf *c, char *why, size_t n)
{
    memset(c, 0, sizeof *c);
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(why, n, "cannot open %s", path);
        return -1;
    }
    char *text = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        if (len + 4096 + 1 > cap) {
            cap = cap ? cap * 2 : 8192;
            char *t = cap > (1u << 20) ? NULL : realloc(text, cap);
            if (!t) {
                snprintf(why, n, "%s is too large", path);
                free(text);
                fclose(f);
                return -1;
            }
            text = t;
        }
        size_t got = fread(text + len, 1, 4096, f);
        len += got;
        if (got < 4096)
            break;
    }
    int bad = ferror(f);
    fclose(f);
    if (bad || memchr(text, 0, len)) {
        snprintf(why, n, "%s cannot be read as text", path);
        free(text);
        return -1;
    }
    text[len] = 0;
    int rc = conf_parse(text, c, why, n);
    free(text);
    return rc;
}

static int word_ok(const char *s)
{
    if (!s || !*s || *s == '#')
        return 0;
    for (; *s; s++)
        if (isspace((unsigned char)*s) || (unsigned char)*s < 32)
            return 0;
    return 1;
}

static int line_ok(const char *s)
{
    if (!s || !*s || isspace((unsigned char)*s) || isspace((unsigned char)s[strlen(s) - 1]))
        return 0;
    for (; *s; s++)
        if (*s == '\n' || *s == '\r')
            return 0;
    return 1;
}

int conf_save(const char *path, const conf *c, char *why, size_t why_len)
{
    char tmp[1200];
    if (snprintf(tmp, sizeof tmp, "%s.new", path) >= (int)sizeof tmp) {
        snprintf(why, why_len, "path too long");
        return -1;
    }
    if ((c->discovery_server && !word_ok(c->discovery_server)) || (c->relay && !word_ok(c->relay))) {
        snprintf(why, why_len, "the discovery server and relay addresses must be single words");
        return -1;
    }
    if (c->name && !word_ok(c->name)) {
        snprintf(why, why_len, "this device's name must be one word");
        return -1;
    }
    for (size_t i = 0; i < c->ndevices; i++)
        if (!word_ok(c->devices[i].name)
            || (c->devices[i].ip ? !word_ok(c->devices[i].ip) || c->devices[i].port == 0 : c->devices[i].port != 0)) {
            snprintf(why, why_len, "device %lu: name and address must be single words", (unsigned long)i + 1);
            return -1;
        }
    for (size_t i = 0; i < c->nfolders; i++)
        if (!word_ok(c->folders[i].id) || !line_ok(c->folders[i].path) || c->folders[i].device >= c->ndevices) {
            snprintf(why, why_len, "folder %lu: bad ID, path or device", (unsigned long)i + 1);
            return -1;
        }
    FILE *f = fopen(tmp, "w");
    if (!f) {
        snprintf(why, why_len, "cannot write %s", tmp);
        return -1;
    }
    fprintf(f, "# Written by the program; see src/conf.h for the format.\n");
    if (c->name)
        fprintf(f, "name %s\n", c->name);
    if (c->listen_port)
        fprintf(f, "listen %u\n", c->listen_port);
    else
        fprintf(f, "listen off\n");
    fprintf(f, "global-discovery %s\nrelays %s\nlocal-announce %s\n", c->global_discovery ? "on" : "off",
            c->relays ? "on" : "off", c->local_announce ? "on" : "off");
    if (c->discovery_server)
        fprintf(f, "discovery-server %s\n", c->discovery_server);
    if (c->relay)
        fprintf(f, "relay %s\n", c->relay);
    for (size_t i = 0; i < c->ndevices; i++) {
        char id[DEVID_TEXT_SIZE];
        devid_format(c->devices[i].id, id);
        fprintf(f, "\ndevice %s\n  id %s\n", c->devices[i].name, id);
        if (c->devices[i].ip)
            fprintf(f, "  address %s:%u\n", c->devices[i].ip, c->devices[i].port);
        else
            fprintf(f, "  address dynamic\n");
    }
    for (size_t i = 0; i < c->nfolders; i++)
        fprintf(f, "\nfolder %s\n  path %s\n  with %s\n  rescan %ld\n", c->folders[i].id, c->folders[i].path,
                c->devices[c->folders[i].device].name, c->folders[i].rescan_seconds);
    int bad = ferror(f);
    bad |= fclose(f) != 0;
    if (bad) {
        remove(tmp);
        snprintf(why, why_len, "cannot write %s", tmp);
        return -1;
    }
    remove(path); /* rename does not replace an existing file everywhere */
    if (rename(tmp, path) != 0) {
        snprintf(why, why_len, "cannot replace %s (the new file is %s)", path, tmp);
        return -1;
    }
    return 0;
}
