/* A FileInfo as a value: what we record and publish for an entry of our
 * own, and the peer's entries decoded for decisions. Field numbers follow
 * Syncthing's proto/bep/bep.proto; the rules (conflict winner, conflict
 * test, conflict copy names, block sizes, deletions) follow
 * lib/protocol/bep_fileinfo.go and lib/model/folder_sendrecv.go of
 * v2.1.7. */
#ifndef ST_FINFO_H
#define ST_FINFO_H

#include "pb.h"
#include "vv.h"

#include <stdint.h>
#include <time.h>

#define FINFO_HASH 32
#define FINFO_MIN_BLOCK (128 << 10)
#define FINFO_MAX_BLOCK (16 << 20)
#define FINFO_PER_FILE_BLOCKS 2000
#define FINFO_MAX_BLOCKS 1000000

typedef struct {
    int64_t offset;
    int32_t size;
    unsigned char hash[FINFO_HASH];
} finfo_block;

typedef struct {
    char *name;
    uint32_t type; /* BEP_FILE, BEP_DIRECTORY, ... (bep.h) */
    int64_t size;
    uint32_t permissions;
    int64_t modified_s;
    int32_t modified_ns;
    uint64_t modified_by;
    int deleted, invalid, no_permissions;
    vv version;
    int64_t sequence;
    int32_t block_size;
    finfo_block *blocks;
    size_t nblocks;
    int has_blocks_hash, has_prev_blocks_hash;
    unsigned char blocks_hash[FINFO_HASH], prev_blocks_hash[FINFO_HASH];
    int has_platform, has_symlink_target, has_encrypted; /* present on the wire, not kept */
} finfo;

/* Decodes everything we use. Block hashes must be 32 bytes, blocks
 * contiguous from 0 and add up to the size (for a live file); anything
 * else is -1 with a reason. 0 on success. */
int finfo_decode(pb_slice raw, finfo *out, const char **why);
/* The FileInfo message body (sequence included). 0 or -1. */
int finfo_encode(pb_writer *w, const finfo *f);
int finfo_copy(finfo *dst, const finfo *src);
void finfo_free(finfo *f);

/* SHA-256 over the block hashes in order (protocol.BlocksHash). */
void finfo_blocks_hash(const finfo_block *b, size_t n, unsigned char out[FINFO_HASH]);
/* The block size Syncthing chooses for a file of this size. */
int32_t finfo_block_size_for(int64_t size);
int finfo_same_blocks(const finfo *a, const finfo *b);

/* Reads the file at path in blocks of the size Syncthing would choose
 * and fills size, block_size, blocks and blocks_hash of f (an empty file
 * has one empty block, as in Syncthing). 0; 1 when the size read is not
 * expect_size (the file changed while it was read; nothing filled); -1
 * when it cannot be read. */
int finfo_hash_path(const char *path, int64_t expect_size, finfo *f);
/* The same in steps, one block per call, so that a long file does not
 * hold up the caller. begin: NULL when the file cannot be read. step: 2
 * more to read, 0 complete, 1 changed while it was read, -1 cannot be
 * read. finish (after 0) fills f as finfo_hash_path does and releases the
 * hasher; abort releases it at any other time (NULL allowed). */
typedef struct finfo_hasher finfo_hasher;
finfo_hasher *finfo_hash_begin(const char *path, int64_t expect_size);
int finfo_hash_step(finfo_hasher *h);
void finfo_hash_finish(finfo_hasher *h, finfo *f);
void finfo_hash_abort(finfo_hasher *h);
/* 1 when the regular file at path holds exactly the blocks of f. */
int finfo_path_matches(const char *path, const finfo *f);

/* FileInfo.WinsConflict: 1 when a is chosen over b. */
int finfo_wins_conflict(const finfo *a, const finfo *b);
/* FileInfo.InConflictWith: 1 when newer (arriving) conflicts with prev. */
int finfo_in_conflict_with(const finfo *newer, const finfo *prev);

/* Marks f deleted the way FileInfo.SetDeleted does: modified_by = by,
 * version updated for by at `now`, modified_s = now (nanoseconds kept),
 * no blocks, no blocks hash, size 0. 0 or -1. */
int finfo_set_deleted(finfo *f, uint64_t by, int64_t now);

/* conflictName: <name without extension>.sync-conflict-YYYYMMDD-HHMMSS-
 * <modifier><extension>, the extension as filepath.Ext gives it (from the
 * last dot of the last element, which may be the whole element). 0, or -1
 * when out is too small. */
int finfo_conflict_name(const char *name, const struct tm *local, uint64_t modified_by, char *out, size_t n);
/* isConflict: the last element contains ".sync-conflict-". */
int finfo_is_conflict_name(const char *name);

#endif
