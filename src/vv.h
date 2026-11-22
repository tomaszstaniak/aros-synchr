/* Version vectors, as Syncthing's lib/protocol/vector.go defines them:
 * counters sorted by short device ID, a missing counter counts as 0.
 * Compare, Update and Merge follow that file exactly (v2.1.7); the
 * compatibility test checks them against cases produced by Syncthing's
 * own code (tests/compat).
 *
 * vv.c translates vector.go of Syncthing (Copyright (C) 2015 The
 * Syncthing Authors) and is therefore covered by the Mozilla Public
 * License 2.0, like the original: https://mozilla.org/MPL/2.0/ */
#ifndef ST_VV_H
#define ST_VV_H

#include "devid.h"
#include "pb.h"

#include <stddef.h>
#include <stdint.h>

#define VV_MAX_COUNTERS 1024 /* far above any real folder (one per device) */

typedef struct {
    uint64_t id, value;
} vv_counter;

typedef struct {
    vv_counter *c;
    size_t n;
} vv;

/* Order of a relative to b. "Concurrent lesser/greater" give concurrent
 * vectors a stable order (Syncthing uses it as the tie break of a
 * conflict); both mean concurrent. */
enum vv_order { VV_EQUAL = 0, VV_GREATER, VV_LESSER, VV_CONCURRENT_LESSER, VV_CONCURRENT_GREATER };

/* Syncthing's ShortID: the first 8 bytes of the Device ID, big endian. */
uint64_t vv_short_id(const unsigned char id[DEVID_BYTES]);
/* Its text form (conflict copy names): the first 7 characters of the
 * base32 (RFC 4648, upper case) encoding of those 8 bytes; "" for 0.
 * out needs 8 bytes. */
void vv_short_id_text(uint64_t id, char out[8]);

/* Decodes a Vector message (repeated Counter counters = 1). Counters must
 * be in strictly increasing ID order (Syncthing always keeps them so);
 * anything else is rejected, because the comparison depends on it.
 * 0, or -1 (malformed, unsorted, duplicate, too many, out of memory). */
int vv_decode(pb_slice msg, vv *out);
/* Appends the Vector as field `field` of w (nothing for an empty vector,
 * as proto3 does). */
void vv_encode(pb_writer *w, uint32_t field, const vv *v);

enum vv_order vv_compare(const vv *a, const vv *b);
int vv_concurrent(enum vv_order o);

/* Our counter: max(value + 1, now); a new counter max(1, now), inserted
 * in order (Vector.updateWithNow). 0 or -1 (out of memory, too many). */
int vv_update(vv *v, uint64_t id, uint64_t now);
/* The per-counter maximum of v and b, into v. 0 or -1. */
int vv_merge(vv *v, const vv *b);
uint64_t vv_counter_value(const vv *v, uint64_t id);
int vv_copy(vv *dst, const vv *src);
void vv_free(vv *v);

/* "id:value,id:value" (decimal), "-" for an empty vector: the text form
 * of the test fixtures. vv_parse accepts the same; it does not require
 * the order (fixtures may hold any vector Syncthing can be given). */
int vv_parse(const char *s, vv *out);
void vv_format(const vv *v, char *out, size_t n);

#endif
