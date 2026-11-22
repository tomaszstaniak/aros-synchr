/* A small HTTPS client for Syncthing's public servers: global discovery
 * (gdisco.h) and the list of public relays. One request per connection
 * (HTTP/1.0, so the answer ends with the connection), stepped without
 * waiting like everything else in the engine, except for the host name
 * lookup in https_start, which waits for the system's resolver.
 *
 * The server's certificate is not checked against certificate
 * authorities: AROS has no common store of them, and nothing these
 * servers say is trusted. An address from discovery is only where to
 * try (the device there is checked by its Device ID), a relay is
 * checked by the Device ID in its address. Someone who can change
 * these answers can keep the devices apart, not read or change what
 * they exchange. */
#ifndef ST_HTTPS_H
#define ST_HTTPS_H

#include "net.h"
#include "tls.h"

#include <stddef.h>

#define HTTPS_MAX_BODY 262144

/* https://HOST[:PORT]/PATH... split; 0, or -1 when it is not such a URL. */
int https_split(const char *url, char *host, size_t host_cap, unsigned *port, char *path, size_t path_cap);

typedef struct https_req https_req;

/* method "GET" or "POST" (body: JSON, or NULL). Our certificate is the
 * client certificate (global discovery identifies us by it). NULL when
 * the host cannot be found or nothing can be started (logged). */
https_req *https_start(const tls_identity *id, const char *url, const char *method, const char *body, tls_log_fn log);
/* 0 not yet, 1 done (the answer is complete), -1 failed (logged). */
int https_step(https_req *r, net_ms now);
/* What to wait for; 1 when the next step should follow at once. */
int https_want(const https_req *r, int *sock, int *rd, int *wr);
net_ms https_deadline(const https_req *r);
/* After 1: the status code, the body (NUL-terminated) and a header's
 * value in seconds (Retry-After, Reannounce-After; -1 when absent). */
int https_status(const https_req *r);
const char *https_body(const https_req *r, size_t *len);
long https_header_seconds(const https_req *r, const char *name);
void https_free(https_req *r);

/* Calls fn with every string value of key in the JSON text: "key":
 * "value", or each string of "key": [ ... ]. Escapes are decoded;
 * characters outside ASCII become '?'. Values longer than 1023 bytes
 * are passed over. Returns the number of calls. Nothing else of the
 * structure is checked: the caller judges each value. */
int json_strings(const char *json, size_t len, const char *key, void (*fn)(void *ctx, const char *value), void *ctx);

#endif
