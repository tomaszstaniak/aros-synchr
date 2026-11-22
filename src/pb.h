/* Minimal Protocol Buffers (proto3 wire format) reader and writer for the
 * BEP messages. Readers never run past their buffer; every malformed or
 * truncated input is an error, never a partial success. */
#ifndef ST_PB_H
#define ST_PB_H

#include <stddef.h>
#include <stdint.h>

enum pb_wire { PB_VARINT = 0, PB_FIXED64 = 1, PB_LEN = 2, PB_FIXED32 = 5 };

typedef struct {
    const unsigned char *p, *end;
    int error; /* sticky: once set, every further read fails */
} pb_reader;

typedef struct {
    const unsigned char *data;
    size_t len;
} pb_slice;

void pb_reader_init(pb_reader *r, const void *data, size_t len);
static inline int pb_done(const pb_reader *r) { return r->error || r->p >= r->end; }

/* Reads the next field key. Returns 1 with *field and *wire set, 0 at the
 * end of the buffer, -1 on error (also sets r->error). Field number 0 and
 * the group wire types (3, 4), which proto3 does not use, are errors. */
int pb_next(pb_reader *r, uint32_t *field, int *wire);

/* Value readers for the wire type just returned by pb_next(). A wrong wire
 * type for the field is the caller's to detect; these check only bounds. */
int pb_varint(pb_reader *r, uint64_t *v);
int pb_bytes(pb_reader *r, pb_slice *s);
int pb_skip(pb_reader *r, int wire);

/* Growable output buffer with a hard size limit. */
typedef struct {
    unsigned char *data;
    size_t len, cap, limit;
    int error;
} pb_writer;

void pb_writer_init(pb_writer *w, size_t limit);
void pb_writer_free(pb_writer *w);
void pb_put_varint_field(pb_writer *w, uint32_t field, uint64_t v);
void pb_put_bytes_field(pb_writer *w, uint32_t field, const void *data, size_t len);
void pb_put_string_field(pb_writer *w, uint32_t field, const char *s);
/* Appends a nested message (already encoded in sub) as a length-delimited field. */
void pb_put_message_field(pb_writer *w, uint32_t field, const pb_writer *sub);
void pb_put_raw(pb_writer *w, const void *data, size_t len);

#endif
