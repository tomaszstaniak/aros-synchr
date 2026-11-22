/* Relay messages (relay.c) against bytes written by Syncthing's own
 * lib/relay/protocol (WriteMessage, at the revision of
 * docs/research/README.md), relay addresses, and the small HTTPS and
 * JSON helpers (https.c) that global discovery and the relay list use. */
#include "../src/https.h"
#include "../src/relay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Syncthing's encodings (hex). */
#define G_INVITATION "9e79bc400000000600000058000000201111111111111111111111111111111111111111111111111111111111111111000000202222222222222222222222222222222222222222222222222222222222222222000000040a0002020000563300000001"
#define G_INVITATION_NOADDR "9e79bc400000000600000054000000201111111111111111111111111111111111111111111111111111111111111111000000202222222222222222222222222222222222222222222222222222222222222222000000000000000100000000"
#define G_RESPONSE_OK "9e79bc40000000040000001000000000000000077375636365737300"
#define G_RESPONSE_NOTFOUND "9e79bc40000000040000001400000001000000096e6f7420666f756e64000000"
#define G_CONNECT "9e79bc400000000500000024000000203333333333333333333333333333333333333333333333333333333333333333"
#define G_JOIN_SESSION "9e79bc400000000300000024000000202222222222222222222222222222222222222222222222222222222222222222"
#define G_JOIN_RELAY "9e79bc40000000020000000400000000"
#define G_JOIN_RELAY_TOKEN "9e79bc40000000020000000c000000056162636465000000"
#define G_PING "9e79bc400000000000000000"
#define G_PONG "9e79bc400000000100000000"

#define ID_B "P56IOI7-MZJNU2Y-IQGDREY-DM2MGTI-MGL3BXN-PQ6W5BM-TBBZ4TJ-XZWICQ2"

static size_t unhex(const char *h, unsigned char *out)
{
    size_t n = 0;
    for (; h[0] && h[1]; h += 2) {
        unsigned v;
        sscanf(h, "%2x", &v);
        out[n++] = (unsigned char)v;
    }
    return n;
}

/* Our encoding of a message equals Syncthing's. */
static void same(const char *gold, int32_t type, const unsigned char *body, size_t blen, const char *what)
{
    unsigned char want[2000], got[2000];
    size_t wl = unhex(gold, want);
    size_t gl = relay_encode(type, body, blen, got, sizeof got);
    CHECK(gl == wl && memcmp(got, want, wl) == 0, "%s: encoding differs from Syncthing's", what);
}

static int seen;
static char seen_text[8][100];
static void collect(void *ctx, const char *v)
{
    (void)ctx;
    if (seen < 8)
        snprintf(seen_text[seen], sizeof seen_text[0], "%s", v);
    seen++;
}

int main(void)
{
    unsigned char b[200], body[200], key[32], id[32];
    size_t bl;
    memset(key, 0x22, sizeof key);
    memset(id, 0x33, sizeof id);

    /* What we send. */
    bl = relay_body_bytes(id, 32, body, sizeof body);
    same(G_CONNECT, RELAY_CONNECT, body, bl, "ConnectRequest");
    bl = relay_body_bytes(key, 32, body, sizeof body);
    same(G_JOIN_SESSION, RELAY_JOIN_SESSION, body, bl, "JoinSessionRequest");
    bl = relay_body_bytes((const unsigned char *)"", 0, body, sizeof body);
    same(G_JOIN_RELAY, RELAY_JOIN_RELAY, body, bl, "JoinRelayRequest");
    bl = relay_body_bytes((const unsigned char *)"abcde", 5, body, sizeof body);
    same(G_JOIN_RELAY_TOKEN, RELAY_JOIN_RELAY, body, bl, "JoinRelayRequest with a token (padding)");
    same(G_PONG, RELAY_PONG, NULL, 0, "Pong");
    CHECK(relay_body_bytes(key, 32, body, 35) == 0, "a body that does not fit is refused");

    /* What we receive. */
    int32_t type, code;
    const unsigned char *bp;
    size_t n = unhex(G_INVITATION, b);
    CHECK(relay_frame(b, n - 1, &type, &bp, &bl) == 0, "a cut message waits for more");
    CHECK(relay_frame(b, 11, &type, &bp, &bl) == 0, "a cut header waits for more");
    CHECK(relay_frame(b, n, &type, &bp, &bl) == (long)n && type == RELAY_INVITATION, "invitation framed");
    relay_invitation inv;
    CHECK(relay_parse_invitation(bp, bl, &inv) == 0, "invitation parsed");
    CHECK(inv.from[0] == 0x11 && inv.from[31] == 0x11 && inv.key_len == 32 && inv.key[5] == 0x22, "from and key");
    CHECK(strcmp(inv.ip, "10.0.2.2") == 0 && inv.port == 22067 && inv.server_socket == 1, "address, port, server");
    n = unhex(G_INVITATION_NOADDR, b);
    CHECK(relay_frame(b, n, &type, &bp, &bl) == (long)n && relay_parse_invitation(bp, bl, &inv) == 0, "no address");
    CHECK(inv.ip[0] == 0 && inv.port == 1 && inv.server_socket == 0, "no address: the relay's, client side");
    CHECK(relay_parse_invitation(bp, bl - 1, &inv) != 0, "a cut invitation is refused");

    char msg[40];
    n = unhex(G_RESPONSE_OK, b);
    CHECK(relay_frame(b, n, &type, &bp, &bl) == (long)n && type == RELAY_RESPONSE, "response framed");
    CHECK(relay_parse_response(bp, bl, &code, msg, sizeof msg) == 0 && code == 0 && strcmp(msg, "success") == 0,
          "success");
    n = unhex(G_RESPONSE_NOTFOUND, b);
    CHECK(relay_frame(b, n, &type, &bp, &bl) == (long)n
              && relay_parse_response(bp, bl, &code, msg, sizeof msg) == 0 && code == 1
              && strcmp(msg, "not found") == 0, "not found");
    n = unhex(G_PING, b);
    CHECK(relay_frame(b, n, &type, &bp, &bl) == 12 && type == RELAY_PING && bl == 0, "ping");

    /* Two messages in one buffer: the first is taken alone. */
    unsigned char two[100];
    size_t l1 = unhex(G_PING, two), l2 = unhex(G_RESPONSE_OK, two + l1);
    CHECK(relay_frame(two, l1 + l2, &type, &bp, &bl) == (long)l1 && type == RELAY_PING, "first of two");

    /* Malformed. */
    n = unhex(G_PING, b);
    b[0] ^= 1;
    CHECK(relay_frame(b, n, &type, &bp, &bl) == -1, "wrong magic");
    n = unhex("9e79bc400000000000000401", b);
    CHECK(relay_frame(b, n, &type, &bp, &bl) == -1, "length above 1024");
    n = unhex("9e79bc4000000000ffffffff", b);
    CHECK(relay_frame(b, n, &type, &bp, &bl) == -1, "negative length");

    /* Relay addresses. */
    relay_addr a;
    CHECK(relay_parse_url("relay://78.17.71.170:22067/?id=" ID_B "&pingInterval=1m0s&networkTimeout=2m0s", &a) == 0,
          "pool address");
    CHECK(strcmp(a.ip, "78.17.71.170") == 0 && a.port == 22067 && a.has_id && !a.token[0], "pool address parts");
    CHECK(relay_parse_url("relay://10.0.0.1:443/?token=s%20cret&id=" ID_B, &a) == 0 && strcmp(a.token, "s cret") == 0
              && a.has_id, "token, decoded");
    CHECK(relay_parse_url("relay://10.0.0.1:22067", &a) == 0 && !a.has_id, "no id");
    CHECK(relay_parse_url("relay://relay.example.org:22067/?id=" ID_B, &a) != 0, "host names are not used");
    CHECK(relay_parse_url("relay://10.0.0.1:0/", &a) != 0, "port 0");
    CHECK(relay_parse_url("relay://10.0.0.1:22067/?id=BAD", &a) != 0, "bad id");
    CHECK(relay_parse_url("tcp://10.0.0.1:22067/", &a) != 0, "other scheme");
    CHECK(relay_parse_url("relay://[::1]:22067/", &a) != 0, "IPv6");

    /* HTTPS addresses. */
    char host[100], path[200];
    unsigned port;
    CHECK(https_split("https://discovery-lookup.syncthing.net/v2/?device=X", host, sizeof host, &port, path,
                      sizeof path) == 0 && strcmp(host, "discovery-lookup.syncthing.net") == 0 && port == 443
              && strcmp(path, "/v2/?device=X") == 0, "lookup address");
    CHECK(https_split("https://127.0.0.1:8443", host, sizeof host, &port, path, sizeof path) == 0 && port == 8443
              && strcmp(path, "/") == 0, "port, no path");
    CHECK(https_split("http://x/", host, sizeof host, &port, path, sizeof path) != 0, "plain http refused");
    CHECK(https_split("https://x:99999/", host, sizeof host, &port, path, sizeof path) != 0, "bad port");

    /* JSON: the relay list and a lookup answer, as the servers write
     * them (Go escapes & as &). */
    const char *pool = "{\"relays\":[{\"url\":\"relay://1.2.3.4:22067/?id=A\\u0026x=1\",\"location\":{\"city\":\"\\\"url\\\"\"}},"
                       " {\"url\": \"relay://5.6.7.8:443/?id=B\"}]}";
    seen = 0;
    CHECK(json_strings(pool, strlen(pool), "url", collect, NULL) == 2 && seen == 2, "two relay URLs");
    CHECK(strcmp(seen_text[0], "relay://1.2.3.4:22067/?id=A&x=1") == 0, "escape decoded: %s", seen_text[0]);
    CHECK(strcmp(seen_text[1], "relay://5.6.7.8:443/?id=B") == 0, "second URL");
    const char *look = "{\"seen\":\"2026-11-20T00:40:21Z\",\"addresses\":[\"tcp://1.2.3.4:22000\", \"relay://5.6.7.8:22067/?id=B\"]}";
    seen = 0;
    CHECK(json_strings(look, strlen(look), "addresses", collect, NULL) == 2, "lookup addresses");
    CHECK(strcmp(seen_text[0], "tcp://1.2.3.4:22000") == 0, "first address");
    seen = 0;
    CHECK(json_strings("{\"addresses\":[]}", 16, "addresses", collect, NULL) == 0, "none");
    CHECK(json_strings("{\"addresses\":[\"cut", 18, "addresses", collect, NULL) == 0, "cut text gives nothing");
    CHECK(json_strings("[\"addresses\",\"x\"]", 17, "addresses", collect, NULL) == 0, "a value is not a key");

    printf("test_relay: %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
