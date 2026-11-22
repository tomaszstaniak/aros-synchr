#include "disco.h"

#include "net.h"
#include "pb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The longest address text looked at; Syncthing's are well below. */
#define ADDRESS_MAX 255
/* Packets read in one disco_poll, so that a flood cannot hold the loop. */
#define POLL_MAX_PACKETS 64

/* Decimal digits only, at most max; -1 otherwise. */
static long number(const char *s, size_t len, long max)
{
    long v = 0;
    if (len == 0 || len > 5)
        return -1;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v > max ? -1 : v;
}

/* "a.b.c.d", numbers 0 to 255 only, into out as the same in canonical
 * form. 0, 1 when it is 0.0.0.0, -1 when it is not such an address. */
static int ipv4(const char *s, size_t len, char out[16])
{
    long part[4];
    size_t at = 0;
    for (int i = 0; i < 4; i++) {
        size_t start = at;
        while (at < len && s[at] != '.')
            at++;
        if (at - start > 3 || (part[i] = number(s + start, at - start, 255)) < 0)
            return -1;
        if (i < 3) {
            if (at >= len)
                return -1;
            at++; /* the dot */
        }
    }
    if (at != len)
        return -1;
    snprintf(out, 16, "%ld.%ld.%ld.%ld", part[0], part[1], part[2], part[3]);
    return part[0] == 0 && part[1] == 0 && part[2] == 0 && part[3] == 0;
}

/* One address of an announcement. 0 with ip and port set when it is one
 * this program can connect to, -1 otherwise. */
static int usable(const unsigned char *a, size_t len, const char *sender, char ip[16], unsigned *port)
{
    char text[ADDRESS_MAX + 1];
    char tmp[16];
    if (len == 0 || len > ADDRESS_MAX || memchr(a, 0, len))
        return -1;
    memcpy(text, a, len);
    text[len] = 0;

    const char *host;
    if (strncmp(text, "tcp://", 6) == 0)
        host = text + 6;
    else if (strncmp(text, "tcp4://", 7) == 0)
        host = text + 7;
    else
        return -1; /* tcp6, quic, relay, anything else */
    size_t hlen = strcspn(host, "/?#");
    /* host:port, the port after the last colon ("[::]:22000" too). */
    size_t colon = hlen;
    while (colon > 0 && host[colon - 1] != ':')
        colon--;
    if (colon == 0)
        return -1; /* no port */
    long p = number(host + colon, hlen - colon, 65535);
    if (p < 1)
        return -1;
    hlen = colon - 1;

    int unspecified = hlen == 0 || (hlen == 4 && memcmp(host, "[::]", 4) == 0);
    if (!unspecified) {
        int r = ipv4(host, hlen, ip);
        if (r < 0)
            return -1; /* IPv6 or a name */
        unspecified = r == 1;
    }
    if (unspecified) {
        /* The address the packet came from, when that is IPv4 and real. */
        if (!sender || ipv4(sender, strlen(sender), tmp) != 0)
            return -1;
        memcpy(ip, tmp, sizeof tmp);
    }
    *port = (unsigned)p;
    return 0;
}

int disco_parse(const void *packet, size_t len, const char *sender, disco_found *out)
{
    const unsigned char *p = packet;
    pb_reader r;
    uint32_t field;
    int wire, k, have_id = 0, have_address = 0;
    disco_found f;

    if (!p || len < 4)
        return -1;
    if ((((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]) != DISCO_MAGIC)
        return -1;
    memset(&f, 0, sizeof f);
    pb_reader_init(&r, p + 4, len - 4);
    while ((k = pb_next(&r, &field, &wire)) == 1) {
        pb_slice s;
        if (field == 1 || field == 2) {
            if (wire != PB_LEN || pb_bytes(&r, &s) != 0)
                return -1;
            if (field == 1) {
                if (s.len != DEVID_BYTES)
                    return -1;
                memcpy(f.id, s.data, DEVID_BYTES); /* the last one counts, as in protobuf */
                have_id = 1;
            } else if (!have_address && usable(s.data, s.len, sender, f.ip, &f.port) == 0) {
                have_address = 1;
            }
        } else if (field == 3) {
            uint64_t v;
            if (wire != PB_VARINT || pb_varint(&r, &v) != 0)
                return -1;
        } else if (pb_skip(&r, wire) != 0) {
            return -1;
        }
    }
    if (k != 0 || r.error || !have_id || !have_address)
        return -1;
    *out = f;
    return 0;
}

static int sock = -1;

int disco_open(unsigned *port, char *why, size_t n)
{
    unsigned p = DISCO_PORT;
    int err = 0;
#ifndef __AROS__
    const char *e = getenv("STPOC_DISCOVERY_PORT");
    if (e && *e) {
        long v = number(e, strlen(e), 65535);
        if (v < 1) {
            snprintf(why, n, "STPOC_DISCOVERY_PORT is not a port number");
            return -1;
        }
        p = (unsigned)v;
    }
#endif
    disco_close();
    *port = p;
    if (net_udp_listen(p, &sock, &err) != NET_OK) {
        sock = -1;
        snprintf(why, n, "error %d", err);
        return -1;
    }
    return 0;
}

int disco_socket(void)
{
    return sock;
}

int disco_poll(void (*fn)(void *ctx, const disco_found *found), void *ctx, int *err)
{
    /* One byte more than is accepted, so that a longer datagram is seen
     * to be longer. */
    static unsigned char buf[DISCO_MAX_PACKET + 1];
    if (sock < 0)
        return 0;
    for (int i = 0; i < POLL_MAX_PACKETS; i++) {
        char from[NET_IPV4_TEXT];
        size_t len = 0;
        disco_found f;
        enum net_status st = net_udp_recv(sock, buf, sizeof buf, &len, from, err);
        if (st == NET_ERROR)
            return -1;
        if (st != NET_OK)
            break; /* nothing more */
        if (len <= DISCO_MAX_PACKET && disco_parse(buf, len, from, &f) == 0)
            fn(ctx, &f);
    }
    return 0;
}

void disco_close(void)
{
    if (sock >= 0)
        net_close(sock);
    sock = -1;
}

size_t disco_build(const unsigned char id[DEVID_BYTES], const char *const *addresses, int n, int64_t instance,
                   unsigned char *out, size_t cap)
{
    pb_writer w;
    size_t len = 0;
    pb_writer_init(&w, DISCO_MAX_PACKET - 4);
    pb_put_bytes_field(&w, 1, id, DEVID_BYTES);
    for (int i = 0; i < n; i++)
        pb_put_string_field(&w, 2, addresses[i]);
    pb_put_varint_field(&w, 3, (uint64_t)instance);
    if (!w.error && cap >= 4 + w.len) {
        out[0] = (unsigned char)(DISCO_MAGIC >> 24);
        out[1] = (unsigned char)(DISCO_MAGIC >> 16);
        out[2] = (unsigned char)(DISCO_MAGIC >> 8);
        out[3] = (unsigned char)DISCO_MAGIC;
        memcpy(out + 4, w.data, w.len);
        len = 4 + w.len;
    }
    pb_writer_free(&w);
    return len;
}

int disco_send(const void *packet, size_t len, unsigned port, int *err)
{
    if (sock < 0) {
        *err = 0;
        return -1;
    }
    return net_udp_broadcast(sock, port, packet, len, err) == NET_OK ? 0 : -1;
}
