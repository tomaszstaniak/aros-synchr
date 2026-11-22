/* Local discovery (disco.c): which announcements are accepted and what
 * is taken from them, that nothing malformed or oversized is, and the
 * socket itself over the loopback interface. */
#include "../src/disco.h"
#include "../src/net.h"
#include "../src/pb.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Not Syncthing's port: one may run on this machine. */
#define TEST_PORT 21931

static unsigned char pkt[70000];
static size_t pkt_len;
static unsigned char the_id[DEVID_BYTES];

static void put_magic(uint32_t magic)
{
    pkt[0] = (unsigned char)(magic >> 24);
    pkt[1] = (unsigned char)(magic >> 16);
    pkt[2] = (unsigned char)(magic >> 8);
    pkt[3] = (unsigned char)magic;
    pkt_len = 4;
}

/* An announcement with the_id and the given addresses (NULL ends). */
static void announce(const char *const *addresses)
{
    pb_writer w;
    pb_writer_init(&w, sizeof pkt - 4);
    pb_put_bytes_field(&w, 1, the_id, sizeof the_id);
    for (; addresses && *addresses; addresses++)
        pb_put_string_field(&w, 2, *addresses);
    pb_put_varint_field(&w, 3, 0x1234567890abcdefULL);
    put_magic(DISCO_MAGIC);
    memcpy(pkt + 4, w.data, w.len);
    pkt_len += w.len;
    pb_writer_free(&w);
}

#define ADDRS(...) announce((const char *const[]){ __VA_ARGS__, NULL })

static void accepted(const char *what, const char *sender, const char *ip, unsigned port)
{
    disco_found f;
    memset(&f, 0xee, sizeof f);
    int rc = disco_parse(pkt, pkt_len, sender, &f);
    CHECK(rc == 0, "%s: refused", what);
    if (rc == 0) {
        CHECK(memcmp(f.id, the_id, DEVID_BYTES) == 0, "%s: id", what);
        CHECK(strcmp(f.ip, ip) == 0 && f.port == port, "%s: %s:%u, expected %s:%u", what, f.ip, f.port, ip, port);
    }
}

static void refused(const char *what, const char *sender)
{
    disco_found f;
    CHECK(disco_parse(pkt, pkt_len, sender, &f) != 0, "%s: accepted", what);
}

static int nfound;
static disco_found last_found;
static void on_found(void *ctx, const disco_found *f)
{
    (void)ctx;
    nfound++;
    last_found = *f;
}

static void send_to(int s, unsigned port, const void *data, size_t len)
{
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(sendto(s, data, len, 0, (struct sockaddr *)&sa, sizeof sa) == (long)len, "sendto %lu bytes", (unsigned long)len);
}

int main(void)
{
    for (int i = 0; i < DEVID_BYTES; i++)
        the_id[i] = (unsigned char)(i * 7 + 1);

    /* ---- accepted ---- */
    ADDRS("tcp://192.168.1.5:22000");
    accepted("an address", "10.0.0.9", "192.168.1.5", 22000);
    accepted("an address, sender unknown", NULL, "192.168.1.5", 22000);
    ADDRS("tcp://0.0.0.0:22000");
    accepted("0.0.0.0 is the sender", "192.168.1.77", "192.168.1.77", 22000);
    ADDRS("tcp://:22001");
    accepted("an empty host is the sender", "192.168.1.77", "192.168.1.77", 22001);
    ADDRS("tcp://[::]:22002");
    accepted("[::] is the sender", "192.168.1.77", "192.168.1.77", 22002);
    ADDRS("tcp4://0.0.0.0:22000");
    accepted("tcp4", "192.168.1.77", "192.168.1.77", 22000);
    ADDRS("quic://0.0.0.0:22000", "relay://198.51.100.7:443/?id=ABC", "tcp://[fe80::1]:22000", "tcp6://:22000",
          "tcp://0.0.0.0:22010", "tcp://192.168.1.5:22000");
    accepted("the first usable of several", "192.168.1.77", "192.168.1.77", 22010);
    ADDRS("tcp://192.168.1.5:22000/", "tcp://192.168.1.6:22000");
    accepted("a path after the port", "10.0.0.9", "192.168.1.5", 22000);
    ADDRS("tcp://192.168.001.005:65535");
    accepted("leading zeros, highest port", "10.0.0.9", "192.168.1.5", 65535);
    {
        /* An unknown field is passed over (a later version of the message). */
        pb_writer w;
        pb_writer_init(&w, 1000);
        pb_put_string_field(&w, 9, "something new");
        pb_put_string_field(&w, 2, "tcp://192.168.1.5:22000");
        pb_put_varint_field(&w, 10, 5);
        pb_put_bytes_field(&w, 1, the_id, sizeof the_id);
        put_magic(DISCO_MAGIC);
        memcpy(pkt + 4, w.data, w.len);
        pkt_len += w.len;
        pb_writer_free(&w);
        accepted("unknown fields, id last", "10.0.0.9", "192.168.1.5", 22000);
    }

    /* ---- refused ---- */
    ADDRS("tcp://192.168.1.5:22000");
    pkt[3] ^= 1;
    refused("wrong magic", "10.0.0.9");
    ADDRS("tcp://192.168.1.5:22000");
    pkt[0] = 0x7d, pkt[1] = 0x79, pkt[2] = 0xbc, pkt[3] = 0x40;
    refused("the magic of the previous protocol version", "10.0.0.9");
    {
        /* Every shorter length: refused, or (cut exactly after a whole
         * field, with id and address complete) the same answer; never
         * anything else. The copy has no byte after the length given, so
         * the sanitizer sees a read past it. */
        ADDRS("quic://0.0.0.0:22000", "tcp://192.168.1.5:22000");
        size_t full = pkt_len, ok = 0;
        for (size_t n = 0; n < full; n++) {
            unsigned char *copy = malloc(n ? n : 1);
            disco_found f;
            memcpy(copy, pkt, n);
            if (disco_parse(n ? copy : NULL, n, "10.0.0.9", &f) == 0) {
                ok++;
                CHECK(strcmp(f.ip, "192.168.1.5") == 0 && f.port == 22000 && memcmp(f.id, the_id, DEVID_BYTES) == 0,
                      "truncated to %lu: a different answer", (unsigned long)n);
            }
            free(copy);
        }
        /* Only the cut before the last field (instance_id) is a whole message. */
        CHECK(ok == 1, "truncated: %lu lengths accepted, expected 1", (unsigned long)ok);
    }
    put_magic(DISCO_MAGIC);
    refused("magic only", "10.0.0.9");
    ADDRS("quic://192.168.1.5:22000", "relay://198.51.100.7:443/?id=ABC");
    refused("no tcp address", "10.0.0.9");
    ADDRS("tcp://[2001:db8::5]:22000", "tcp6://[2001:db8::5]:22000", "tcp://[fe80::1%en0]:22000");
    refused("IPv6 only", "10.0.0.9");
    announce(NULL);
    refused("no address at all", "10.0.0.9");
    ADDRS("tcp://0.0.0.0:22000");
    refused("unspecified host, sender unknown", NULL);
    refused("unspecified host, sender empty", "");
    refused("unspecified host, sender unspecified", "0.0.0.0");
    refused("unspecified host, sender not IPv4", "fe80::1");
    ADDRS("tcp://syncthing.example.org:22000");
    refused("a host name", "10.0.0.9");
    ADDRS("tcp://192.168.1.5");
    refused("no port", "10.0.0.9");
    ADDRS("tcp://192.168.1.5:0");
    refused("port 0", "10.0.0.9");
    ADDRS("tcp://192.168.1.5:65536");
    refused("port 65536", "10.0.0.9");
    ADDRS("tcp://192.168.1.5:22000x");
    refused("letters in the port", "10.0.0.9");
    ADDRS("tcp://192.168.1.5:-22000");
    refused("a sign in the port", "10.0.0.9");
    ADDRS("tcp://192.168.1.256:22000");
    refused("a number above 255", "10.0.0.9");
    ADDRS("tcp://192.168.1:22000");
    refused("three numbers", "10.0.0.9");
    ADDRS("tcp://192.168.1.5.6:22000");
    refused("five numbers", "10.0.0.9");
    ADDRS("tcp://1921.68.1.5:22000");
    refused("four digits", "10.0.0.9");
    ADDRS("tcp://192.168..5:22000");
    refused("an empty number", "10.0.0.9");
    ADDRS("TCP://192.168.1.5:22000");
    refused("another scheme spelling", "10.0.0.9");
    ADDRS("tcp:/192.168.1.5:22000");
    refused("one slash", "10.0.0.9");
    {
        /* Oversized fields: an address of 300 and of 60000 bytes is not
         * looked at (a usable one after it still is), an id of 33 or 31
         * bytes refuses the packet. */
        static char big[60001];
        memset(big, '1', sizeof big - 1);
        memcpy(big, "tcp://", 6);
        big[300] = 0;
        ADDRS(big);
        refused("an address of 300 bytes", "10.0.0.9");
        ADDRS(big, "tcp://192.168.1.5:22000");
        accepted("a usable address after an oversized one", "10.0.0.9", "192.168.1.5", 22000);
        big[300] = '1';
        ADDRS(big);
        refused("an address of 60000 bytes", "10.0.0.9");

        for (int extra = -1; extra <= 1; extra += 2) {
            unsigned char id[DEVID_BYTES + 1] = { 0 };
            pb_writer w;
            pb_writer_init(&w, 1000);
            pb_put_bytes_field(&w, 1, id, (size_t)(DEVID_BYTES + extra));
            pb_put_string_field(&w, 2, "tcp://192.168.1.5:22000");
            put_magic(DISCO_MAGIC);
            memcpy(pkt + 4, w.data, w.len);
            pkt_len += w.len;
            pb_writer_free(&w);
            refused(extra > 0 ? "an id of 33 bytes" : "an id of 31 bytes", "10.0.0.9");
        }
        /* A length that claims more than the packet holds. */
        put_magic(DISCO_MAGIC);
        pkt[pkt_len++] = 0x12; /* field 2, length-delimited */
        pkt[pkt_len++] = 0xff, pkt[pkt_len++] = 0xff, pkt[pkt_len++] = 0xff, pkt[pkt_len++] = 0x7f;
        memcpy(pkt + pkt_len, "tcp://192.168.1.5:22000", 23);
        pkt_len += 23;
        refused("a length beyond the packet", "10.0.0.9");
        /* An address with a zero byte in it. */
        pb_writer w;
        pb_writer_init(&w, 1000);
        pb_put_bytes_field(&w, 1, the_id, sizeof the_id);
        pb_put_bytes_field(&w, 2, "tcp://192.168.1.5:22000\0x", 25);
        put_magic(DISCO_MAGIC);
        memcpy(pkt + 4, w.data, w.len);
        pkt_len += w.len;
        pb_writer_free(&w);
        refused("a zero byte in an address", "10.0.0.9");
    }
    {
        /* Wrong wire types for the known fields, and no id. */
        pb_writer w;
        pb_writer_init(&w, 1000);
        pb_put_varint_field(&w, 1, 7);
        pb_put_string_field(&w, 2, "tcp://192.168.1.5:22000");
        put_magic(DISCO_MAGIC);
        memcpy(pkt + 4, w.data, w.len);
        pkt_len += w.len;
        pb_writer_free(&w);
        refused("id as a number", "10.0.0.9");
        pb_writer_init(&w, 1000);
        pb_put_string_field(&w, 2, "tcp://192.168.1.5:22000");
        put_magic(DISCO_MAGIC);
        memcpy(pkt + 4, w.data, w.len);
        pkt_len += w.len;
        pb_writer_free(&w);
        refused("no id", "10.0.0.9");
        pb_writer_init(&w, 1000);
        pb_put_bytes_field(&w, 1, the_id, sizeof the_id);
        pb_put_string_field(&w, 2, "tcp://192.168.1.5:22000");
        pb_put_string_field(&w, 3, "x");
        put_magic(DISCO_MAGIC);
        memcpy(pkt + 4, w.data, w.len);
        pkt_len += w.len;
        pb_writer_free(&w);
        refused("instance_id as a string", "10.0.0.9");
    }
    {
        /* Random bytes after the magic: whatever the answer, no read
         * outside the buffer (the sanitizer watches). */
        unsigned seed = 12345;
        for (int round = 0; round < 20000; round++) {
            size_t n = 4 + (seed >> 8) % 120;
            unsigned char *copy = malloc(n);
            disco_found f;
            put_magic(DISCO_MAGIC);
            memcpy(copy, pkt, 4);
            for (size_t i = 4; i < n; i++) {
                seed = seed * 1103515245u + 12345u;
                copy[i] = (unsigned char)(seed >> 16);
            }
            (void)disco_parse(copy, n, "10.0.0.9", &f);
            free(copy);
        }
        checks++;
    }

    /* ---- the socket ---- */
    {
        char why[100] = "", port_text[16];
        unsigned port = 0;
        int err = 0, second = -1;
        snprintf(port_text, sizeof port_text, "%d", TEST_PORT);
        setenv("STPOC_DISCOVERY_PORT", "0", 1);
        CHECK(disco_open(&port, why, sizeof why) != 0 && disco_socket() < 0, "port 0 from the environment accepted");
        setenv("STPOC_DISCOVERY_PORT", "21027x", 1);
        CHECK(disco_open(&port, why, sizeof why) != 0 && disco_socket() < 0, "a bad port from the environment accepted");
        setenv("STPOC_DISCOVERY_PORT", port_text, 1);
        CHECK(disco_open(&port, why, sizeof why) == 0 && port == TEST_PORT && disco_socket() >= 0, "open: %s", why);
        /* Another listener on the same port, as a Syncthing on this
         * machine is, does not keep ours out, nor ours it. */
        CHECK(net_udp_listen(TEST_PORT, &second, &err) == NET_OK, "a second listener on the port: error %d", err);
        net_close(second);

        CHECK(disco_poll(on_found, NULL, &err) == 0 && nfound == 0, "poll with nothing waiting");
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        CHECK(s >= 0, "sending socket");
        ADDRS("tcp://0.0.0.0:22000");
        send_to(s, TEST_PORT, pkt, pkt_len);
        send_to(s, TEST_PORT, "junk", 4);
        send_to(s, TEST_PORT, "", 0);
        the_id[0] ^= 0xff;
        ADDRS("quic://0.0.0.0:22000", "tcp://192.168.1.5:22003");
        send_to(s, TEST_PORT, pkt, pkt_len);
        {
            /* A valid announcement in a datagram longer than is read. */
            static char pad[DISCO_MAX_PACKET];
            pb_writer w;
            memset(pad, 'p', sizeof pad - 1);
            pb_writer_init(&w, sizeof pkt - 4);
            pb_put_bytes_field(&w, 1, the_id, sizeof the_id);
            pb_put_string_field(&w, 2, "tcp://192.168.1.9:22009");
            pb_put_string_field(&w, 9, pad);
            put_magic(DISCO_MAGIC);
            memcpy(pkt + 4, w.data, w.len);
            pkt_len += w.len;
            pb_writer_free(&w);
            disco_found f;
            CHECK(pkt_len > DISCO_MAX_PACKET && disco_parse(pkt, pkt_len, "127.0.0.1", &f) == 0, "the long packet is valid as such");
            send_to(s, TEST_PORT, pkt, pkt_len);
        }
        usleep(200000);
        nfound = 0;
        CHECK(disco_poll(on_found, NULL, &err) == 0, "poll: error %d", err);
        CHECK(nfound == 2, "poll: %d announcements, expected 2 of 5 datagrams", nfound);
        CHECK(strcmp(last_found.ip, "192.168.1.5") == 0 && last_found.port == 22003
              && memcmp(last_found.id, the_id, DEVID_BYTES) == 0, "poll: the last announcement");
        the_id[0] ^= 0xff;
        ADDRS("tcp://0.0.0.0:22000");
        send_to(s, TEST_PORT, pkt, pkt_len);
        usleep(200000);
        nfound = 0;
        CHECK(disco_poll(on_found, NULL, &err) == 0 && nfound == 1, "poll: one more");
        CHECK(strcmp(last_found.ip, "127.0.0.1") == 0 && last_found.port == 22000
              && memcmp(last_found.id, the_id, DEVID_BYTES) == 0, "poll: the sender's address was used: %s", last_found.ip);
        close(s);
        disco_close();
        CHECK(disco_socket() < 0 && disco_poll(on_found, NULL, &err) == 0, "closed");
    }

    printf("test_disco: %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
