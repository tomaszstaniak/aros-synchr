/* The container shared by the state formats: a text file of lines, the
 * last one "end <SHA-256 hex of every byte before it>", replaced as a
 * whole (<path>.tmp written, the old file moved to <path>.old, the new
 * one moved into place, <path>.old removed). A reader that finds only
 * <path>.old (a replacement interrupted between the two moves) uses it.
 * Not atomic and no fsync: a crash can leave the old content, never a
 * mix (the checksum rejects a torn file). */
#ifndef ST_STFILE_H
#define ST_STFILE_H

#include "pb.h"

#include <stddef.h>

#define STFILE_MAX (64u << 20) /* bytes of one state file */

/* 1: neither <path> nor <path>.old exists; 0: *buf (malloc'd, NUL
 * terminated) and *len hold the file up to (not including) its end line,
 * checksum verified; -1: present but unreadable or damaged (why). */
int stfile_read(const char *path, char **buf, size_t *len, char *why, size_t why_len);

/* Writes body (every line, each ending in '\n') plus its end line as
 * <path>, as described above. 0, or -1 (why; the previous file is kept). */
int stfile_write(const char *path, const pb_writer *body, char *why, size_t why_len);

/* Takes the next '\n'-terminated line (the '\n' replaced by NUL); NULL at
 * the end. */
char *stfile_line(char **p, char *end, size_t *len);

void stfile_put_str(pb_writer *w, const char *s);
void stfile_put_hex(pb_writer *w, const unsigned char *p, size_t n);
/* Decodes exactly len lower-case hex digits; *out is malloc'd with a
 * terminating NUL. The byte count, or -1. */
long stfile_hex_decode(const char *s, size_t len, unsigned char **out);

#endif
