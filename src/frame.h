/* BEP framing over a byte stream, independent of the transport.
 *
 *   Hello:   4-byte magic 0x2EA7D90B, 2-byte length (<= 32767), Hello
 *   message: 2-byte header length, Header, 4-byte message length, message
 *
 * All lengths big-endian. The receiver accepts input in pieces of any size
 * and yields complete, decompressed message bodies. */
#ifndef ST_FRAME_H
#define ST_FRAME_H

#include "pb.h"

#include <stddef.h>
#include <stdint.h>

enum frame_status {
    FRAME_NEED_MORE = 0,
    FRAME_HELLO,       /* body = Hello */
    FRAME_MESSAGE,     /* type, body = decompressed message */
    FRAME_ERROR        /* error = reason; the stream cannot continue */
};

typedef struct {
    size_t max_message;      /* largest message on the wire */
    size_t max_decompressed; /* largest message after LZ4 */
    int got_hello;
    unsigned char *buf;      /* received, not yet consumed */
    size_t len, cap;
    unsigned char *out;      /* decompression output */
    size_t out_cap;
    size_t consume;          /* bytes to drop before the next parse */
    const char *error;
    /* last result */
    uint32_t type, compression;
    const unsigned char *body;
    size_t body_len, wire_len;
} frame_rx;

void frame_rx_init(frame_rx *rx, size_t max_message, size_t max_decompressed);
void frame_rx_free(frame_rx *rx);

/* Largest single feed; callers read at most this much at a time. */
#define FRAME_FEED_MAX 65536

/* Appends received bytes (at most FRAME_FEED_MAX per call, after
 * frame_rx_next() has returned FRAME_NEED_MORE). Returns 0, or -1 when the
 * buffered data would exceed one frame plus one feed. */
int frame_rx_feed(frame_rx *rx, const void *data, size_t len);

/* Extracts the next complete frame. The body stays valid until the next
 * call to frame_rx_next() or frame_rx_feed(). */
enum frame_status frame_rx_next(frame_rx *rx);

/* Encoders: append a complete frame to w. */
void frame_put_hello(pb_writer *w, const pb_writer *hello);
void frame_put_message(pb_writer *w, uint32_t type, const pb_writer *body);

#endif
