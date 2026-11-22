/* Decoder for the LZ4 block format (no frame), as used by BEP message
 * compression. Written for untrusted input: every length and offset is
 * checked against both buffers. */
#ifndef ST_LZ4BLK_H
#define ST_LZ4BLK_H

#include <stddef.h>

/* Decodes src into dst. Returns the number of bytes written, or -1 if the
 * block is malformed or would not fit in dst_cap. */
long lz4_block_decode(const unsigned char *src, size_t src_len,
                      unsigned char *dst, size_t dst_cap);

#endif
