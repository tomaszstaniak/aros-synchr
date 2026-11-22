/* The receive state of one folder: which entries this client wrote (or
 * adopted), as the peer's FileInfo of that version. It records what was
 * written, not what is on disk now; callers verify the files themselves.
 *
 * Text format, version 1 (all variable data hex-encoded, one item a line):
 *
 *   aros-syncthing-state 1
 *   folder <hex of the folder ID>
 *   dest <hex of the destination path>
 *   peer <peer Device ID, canonical text>
 *   count <number of entries>
 *   entry <file|dir> <hex of the name> <hex of the encoded FileInfo>
 *   ...
 *   end <SHA-256 hex of every byte before this line>
 *
 * A file that does not match in any detail (version, folder, destination,
 * peer, count, checksum, a malformed or duplicate entry, a limit) is
 * rejected as a whole. */
#ifndef ST_STATE_H
#define ST_STATE_H

#include "devid.h"

#include <stddef.h>

#define STATE_MAX_ENTRIES 20000
#define STATE_MAX_FILEINFO (4u << 20)  /* bytes of one encoded FileInfo */
#define STATE_MAX_FILE (64u << 20)     /* bytes of the whole state file */

typedef struct {
    char *name;
    int is_dir;
    unsigned char *raw; /* encoded FileInfo */
    size_t raw_len;
    int status;         /* for the caller; kept in memory only, never stored */
} state_entry;

typedef struct {
    state_entry *e;
    size_t n, cap;
} state_list;

/* <identity-dir>/state-<first 32 hex digits of SHA-256(folder ID)>.txt:
 * the folder ID itself never becomes part of a path. */
int state_path(const char *identity_dir, const char *folder_id, char *out, size_t n);

/* 1: no state file; 0: loaded into out; -1: present but rejected (why).
 * A complete earlier version left as <path>.old by an interrupted
 * replacement is used when <path> is missing. */
int state_load(const char *path, const char *folder_id, const char *dest,
               const unsigned char peer[DEVID_BYTES], state_list *out, char *why, size_t why_len);

/* Writes the whole list: <path>.tmp, then moved into place (the previous
 * file moved aside first and restored on failure). Not atomic, no fsync. */
int state_save(const char *path, const char *folder_id, const char *dest,
               const unsigned char peer[DEVID_BYTES], const state_list *l, char *why, size_t why_len);

/* List helpers. state_put replaces an entry of the same name. */
state_entry *state_find(state_list *l, const char *name);
int state_put(state_list *l, const char *name, int is_dir, const unsigned char *raw, size_t raw_len);
void state_remove(state_list *l, const char *name);
void state_list_free(state_list *l);

#endif
