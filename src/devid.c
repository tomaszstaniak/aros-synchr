/* Device ID text form, following lib/protocol/deviceid.go and luhn.go of
 * Syncthing (behaviour only; no code is copied). */
#include "devid.h"

#include <string.h>

static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

static int codepoint(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= '2' && c <= '7')
        return c - '2' + 26;
    return -1;
}

/* Luhn mod N over the base32 alphabet. Syncthing starts with factor 1 on
 * the first character, which differs from the textbook Luhn (factor 2 on
 * the rightmost); matching it is what makes the IDs interoperate. */
static char luhn32(const char *s, size_t n)
{
    int factor = 1, sum = 0;
    for (size_t i = 0; i < n; i++) {
        int addend = factor * codepoint(s[i]);
        factor = factor == 2 ? 1 : 2;
        sum += addend / 32 + addend % 32;
    }
    return alphabet[(32 - sum % 32) % 32];
}

static void base32_encode(const unsigned char in[DEVID_BYTES], char out[53])
{
    unsigned buffer = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < DEVID_BYTES; i++) {
        buffer = (buffer << 8) | in[i];
        bits += 8;
        while (bits >= 5) {
            out[o++] = alphabet[(buffer >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    /* 256 bits leave 1 bit; padded with zeros like RFC 4648. */
    if (bits > 0)
        out[o++] = alphabet[(buffer << (5 - bits)) & 31];
    out[o] = 0;
}

/* Strict decoder: 52 characters, and the unused low bits of the last one
 * must be zero, so every ID has exactly one accepted spelling. */
static int base32_decode(const char in[52], unsigned char out[DEVID_BYTES])
{
    unsigned buffer = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < 52; i++) {
        int v = codepoint(in[i]);
        if (v < 0)
            return -1;
        buffer = (buffer << 5) | (unsigned)v;
        bits += 5;
        if (bits >= 8) {
            out[o++] = (unsigned char)(buffer >> (bits - 8));
            bits -= 8;
        }
    }
    if (o != DEVID_BYTES || (buffer & ((1u << bits) - 1)) != 0)
        return -1;
    return 0;
}

void devid_format(const unsigned char id[DEVID_BYTES], char out[DEVID_TEXT_SIZE])
{
    char b32[53], luhned[56];
    base32_encode(id, b32);
    for (int i = 0; i < 4; i++) {
        memcpy(luhned + i * 14, b32 + i * 13, 13);
        luhned[i * 14 + 13] = luhn32(b32 + i * 13, 13);
    }
    size_t o = 0;
    for (int i = 0; i < 8; i++) {
        if (i > 0)
            out[o++] = '-';
        memcpy(out + o, luhned + i * 7, 7);
        o += 7;
    }
    out[o] = 0;
}

int devid_parse(const char *text, unsigned char id[DEVID_BYTES])
{
    char clean[57], b32[52];
    size_t n = 0;
    for (const char *p = text; *p; p++) {
        char c = *p;
        if (c == '-' || c == ' ')
            continue;
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        if (c == '0')
            c = 'O';
        else if (c == '1')
            c = 'I';
        else if (c == '8')
            c = 'B';
        if (n == sizeof clean - 1)
            return -1;
        clean[n++] = c;
    }
    clean[n] = 0;
    if (n == 56) {
        for (int i = 0; i < 4; i++) {
            const char *chunk = clean + i * 14;
            for (int k = 0; k < 13; k++)
                if (codepoint(chunk[k]) < 0)
                    return -1;
            if (chunk[13] != luhn32(chunk, 13))
                return -1;
            memcpy(b32 + i * 13, chunk, 13);
        }
    } else if (n == 52) {
        memcpy(b32, clean, 52);
    } else {
        return -1;
    }
    return base32_decode(b32, id);
}
