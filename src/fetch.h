/* Receiving files of one folder: queue entries from the peer's index,
 * request their blocks one at a time, verify each block's length and
 * SHA-256, write through the store, and announce a file in an IndexUpdate
 * only after it is complete under its final name.
 *
 * With a state file, what was written is remembered between runs. At start
 * every recorded file is hashed again; only files whose content still
 * matches are announced (in a full Index) and not downloaded again. A file
 * changed here is kept, not overwritten and not announced (a conflict). */
#ifndef ST_FETCH_H
#define ST_FETCH_H

#include "bep.h"
#include "net.h"
#include "store.h"
#include "tls.h"

typedef struct fetch fetch;

/* Sends one BEP message (body only; the caller frames it). */
typedef enum tls_result (*fetch_send_fn)(void *ctx, uint32_t type, const pb_writer *body);

typedef struct {
    long files_done, files_replaced, dirs_done, failed, rejected, skipped, announced;
    long requests, present, adopted, conflicts, state_failures;
    long state_saves; /* times the state file was written */
    long unsupported; /* entries of a type this client cannot receive (symbolic links, unknown) */
    long long bytes;
} fetch_stats;

typedef struct {
    const char *state_path;       /* NULL: nothing is remembered between runs */
    const char *dest;             /* recorded in the state and checked on load */
    const unsigned char *peer_id; /* likewise */
    int state_fault;              /* test hook: 1 makes every state write fail */
} fetch_state_opts;

/* abort_after_blocks > 0 is a test hook: after that many blocks have been
 * written in this run, the download stops as a crash would, leaving the
 * temporary file and the previous version untouched. */
fetch *fetch_new(const char *folder, store *st, tls_log_fn log, tls_log_fn ui, fetch_send_fn send,
                 void *ctx, long abort_after_blocks, const fetch_state_opts *so);
void fetch_free(fetch *f);

/* Loads the state and verifies every recorded entry against the disk.
 * Returns 0, or -1 when out of memory. A missing or rejected state is not
 * an error: the run starts as if nothing had been recorded. */
int fetch_prepare(fetch *f);

/* Sends the full Index of what is verified present (in batches: an Index,
 * then IndexUpdates). Returns NULL or an outcome code. */
const char *fetch_send_index(fetch *f);

/* Human-readable size ("20 B", "1.2 KB", "2.0 MB") into buf. */
const char *fetch_size_text(long long bytes, char *buf, size_t n);

/* Takes one entry of an Index/IndexUpdate. Unsupported or invalid entries
 * are logged and skipped. Returns NULL, or an outcome code when the
 * session cannot continue (out of memory, a failed send). */
const char *fetch_offer(fetch *f, const bep_file *fi);

/* Handles a Response. Returns NULL to continue or an outcome code. */
const char *fetch_response(fetch *f, int32_t id, pb_slice data, uint32_t code);

/* Advances the work: sends the next Request when none is outstanding,
 * finishes and announces completed files, enforces the per-request time
 * limit. *wake gets the time it needs to be called again. Returns NULL or
 * an outcome code. */
const char *fetch_step(fetch *f, net_ms now, net_ms *wake);

/* Writes the state file now if anything was recorded since it was last
 * written. Returns 0, or -1 when the write failed (also counted in
 * state_failures). Call before reporting the outcome. */
int fetch_flush(fetch *f);

/* The outcome of what was offered so far: NULL when every accepted entry
 * is here and recorded, otherwise "FILES_FAILED", "LOCAL_CONFLICTS",
 * "STATE_NOT_SAVED" (records not written even after the last attempt) or
 * "PARTIAL" (entries rejected here: names this system cannot take, invalid
 * block lists, symbolic links). Deletions and entries the peer marks
 * invalid are deliberate skips and do not count. */
const char *fetch_result(const fetch *f);

/* Test hook: before each file is committed, create DIR/at-<basename> and
 * wait until DIR/go-<basename> exists (at most 120 s), so a test can act
 * on the destination at exactly that point. Both are deleted afterwards.
 * NULL: off. */
void fetch_set_commit_hook(fetch *f, const char *dir);

/* Nothing queued and nothing in flight. */
int fetch_idle(const fetch *f);
const fetch_stats *fetch_get_stats(const fetch *f);

#endif
