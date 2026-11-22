/* Syncthing Device IDs: the SHA-256 of a device certificate's DER encoding,
 * written as base32 with a Luhn mod-32 check character after every 13
 * characters, in groups of seven separated by '-'. */
#ifndef ST_DEVID_H
#define ST_DEVID_H

#include <stddef.h>

#define DEVID_BYTES 32
#define DEVID_TEXT_SIZE 64 /* 8 groups of 7, 7 dashes, NUL */

/* Writes the canonical form ("XXXXXXX-...-XXXXXXX") into out. */
void devid_format(const unsigned char id[DEVID_BYTES], char out[DEVID_TEXT_SIZE]);

/* Accepts what Syncthing accepts: any case, groups separated by '-' or
 * spaces, the look-alike digits 0, 1 and 8 for O, I and B, and the old
 * 52-character form without check characters. Check characters, when
 * present, must be correct. Returns 0 on success, -1 otherwise. */
int devid_parse(const char *text, unsigned char id[DEVID_BYTES]);

#endif
