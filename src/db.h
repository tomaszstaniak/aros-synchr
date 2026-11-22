/* The two-way state of one folder (state format 2): our record of every
 * entry, as the FileInfo we announce for it (our version vector and
 * sequence), with what the disk showed when the record was made, plus
 * our index ID and the highest sequence ever recorded. Same file as
 * format 1 (state_path), same container (stfile.h).
 *
 *   aros-syncthing-state 2
 *   folder <hex of the folder ID>
 *   dest <hex of the folder path>
 *   peer <peer Device ID>
 *   device <our Device ID>
 *   index <our index ID, 16 hex digits>
 *   sequence <highest sequence recorded>
 *   peerindex <the peer's index ID, 16 hex digits> <its highest sequence processed>
 *   count <number of entries>
 *   entry <hex of the encoded FileInfo> <disk size> <disk mtime s> <disk mtime ns> <flags>
 *   ...
 *   end <SHA-256>
 *
 * Rule (docs/backlog/two-way-sync.md, section 2): a record is written
 * here before its sequence is announced, so every announced sequence is
 * in this file and none is used twice. A file that does not match in any
 * detail is rejected as a whole. */
#ifndef ST_DB_H
#define ST_DB_H

#include "devid.h"
#include "finfo.h"

#include <stddef.h>
#include <stdint.h>

#define DB_MAX_ENTRIES 20000
/* The disk date was within the file system's date resolution of the
 * scan, or a served block did not match: hash on the next scan whatever
 * size and date say. */
#define DB_HASH_NEXT 1u

typedef struct {
    finfo f;
    int64_t disk_size, disk_mtime_s;
    int32_t disk_mtime_ns;
    unsigned flags;
    int status; /* for the caller, kept in memory only */
} db_entry;

typedef struct {
    uint64_t index_id;
    int64_t sequence;
    uint64_t peer_index_id;
    int64_t peer_sequence;
    db_entry *e;
    size_t n, cap;
    size_t *slot; /* open-addressing name index: entry position + 1, 0 empty */
    size_t slots;
} db;

typedef struct {
    const char *folder_id, *dest;
    const unsigned char *peer_id, *device_id;
} db_binding;

/* 1: no state file; 0: loaded; 2: a format 1 (receive-only) state is
 * there, to be migrated by the caller (state_load reads it); -1: present
 * but rejected (why). */
int db_load(const char *path, const db_binding *b, db *out, char *why, size_t why_len);
int db_save(const char *path, const db_binding *b, const db *d, char *why, size_t why_len);

db_entry *db_find(db *d, const char *name);
/* Copies f in; replaces an entry of the same name (its disk fields and
 * flags are reset). NULL when out of memory or full. */
db_entry *db_put(db *d, const finfo *f);
void db_remove(db *d, const char *name);
/* The next sequence number for a new record (and counts it as used). */
int64_t db_next_sequence(db *d);
void db_free(db *d);

#endif
