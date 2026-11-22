/* LZ4 block format: a sequence of (token, literal length extension,
 * literals, 2-byte little-endian match offset, match length extension).
 * The last sequence has literals only. Specification:
 * https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md */
#include "lz4blk.h"

#include <string.h>

/* Reads the 255-run length extension. Returns 0, or -1 when the input ends
 * or the length would overflow size_t. */
static int ext_len(const unsigned char **p, const unsigned char *end, size_t *len)
{
    unsigned char b;
    do {
        if (*p >= end)
            return -1;
        b = *(*p)++;
        if (*len > (size_t)-1 - b)
            return -1;
        *len += b;
    } while (b == 255);
    return 0;
}

long lz4_block_decode(const unsigned char *src, size_t src_len,
                      unsigned char *dst, size_t dst_cap)
{
    const unsigned char *p = src, *end = src + src_len;
    size_t o = 0;

    if (src_len == 0)
        return dst_cap == 0 ? 0 : -1; /* an empty block is not valid LZ4 */
    for (;;) {
        if (p >= end)
            return -1;
        unsigned token = *p++;
        size_t lit = token >> 4;
        if (lit == 15 && ext_len(&p, end, &lit) != 0)
            return -1;
        if (lit > (size_t)(end - p) || lit > dst_cap - o)
            return -1;
        memcpy(dst + o, p, lit);
        p += lit;
        o += lit;
        if (p == end)
            break; /* the last sequence ends after its literals */

        if (end - p < 2)
            return -1;
        size_t offset = (size_t)p[0] | ((size_t)p[1] << 8);
        p += 2;
        if (offset == 0 || offset > o)
            return -1;
        size_t mlen = token & 15;
        if (mlen == 15 && ext_len(&p, end, &mlen) != 0)
            return -1;
        mlen += 4;
        if (mlen > dst_cap - o)
            return -1;
        /* Byte by byte on purpose: the source may overlap the destination
         * (offset < length repeats a pattern), which memcpy must not do. */
        for (size_t i = 0; i < mlen; i++, o++)
            dst[o] = dst[o - offset];
    }
    return (long)o;
}
