/* Two-way synchronisation of one folder with one peer (milestone in
 * docs/backlog/two-way-sync.md). Owns the state (format 2, db.h), reads
 * the folder (scan.h), publishes our entries, serves the peer's block
 * requests and takes the peer's entries through the decision table
 * (decide.h).
 *
 * Order for every change (section 2 of the plan): the file is final on
 * disk, then its record (version, our sequence) is written to the state
 * file, then the IndexUpdate goes out. A state write that fails holds the
 * announcement back.
 *
 * Applied in this version: local creations and edits (published), the
 * peer's newer files and directories (downloaded), equal content under
 * another version (recorded). Held and reported, nothing changed on disk:
 * deletions in either direction, conflict copies, type changes (stage C
 * of the plan). */
#ifndef ST_TW_H
#define ST_TW_H

#include "bep.h"
#include "net.h"
#include "store.h"
#include "tls.h"

typedef struct tw tw;
typedef enum tls_result (*tw_send_fn)(void *ctx, uint32_t type, const pb_writer *body);

typedef struct {
    const char *folder_id;
    const char *root;       /* the folder on disk */
    const char *state_path; /* state file (state_path() of state.h) */
    const unsigned char *peer_id, *my_id;
    store *st;
    tls_log_fn log, ui;
    tw_send_fn send;
    void *ctx;
    long rescan_seconds; /* 0: the folder is read at start only */
    /* TW_HASH_AUTO: hash every file at each scan where the volume's dates
     * cannot be trusted (scan_dates_reliable), otherwise only files whose
     * size or date changed; TW_HASH_ALWAYS, TW_HASH_TRUST force one. */
    int hash_policy;
    /* > 0: a scan while connected is done in pieces of about this many
     * milliseconds, one per tw_step, so the caller can serve the
     * connection in between; 0: in one call. */
    long slice_ms;
    /* Asked during the scan at start (tw_prepare); non-zero ends it with
     * "STOPPED". May be NULL. */
    int (*stop)(void);
    /* Request IDs are id_base + 1, + 2, ...: with several folders on one
     * connection each gets a range of its own, so that a Response (which
     * carries only the ID) finds its folder. 0 for a single folder. */
    int32_t id_base;
} tw_config;

enum { TW_HASH_AUTO = 0, TW_HASH_ALWAYS, TW_HASH_TRUST };

typedef struct {
    long local_new, local_changed, local_deleted;      /* found by the scans (deletions published) */
    long deleted_here, conflicts, dirs_kept;           /* the peer's deletions applied here; conflict copies made
                                                          here; directories kept because not empty */
    long leftovers_removed, leftovers_restored;        /* after an interruption */
    long downloaded, dirs_made, recorded, adopted;     /* from the peer */
    long served_blocks, refused_requests;              /* requests of the peer */
    long held, unsupported, rejected, failed;
    long announced, state_saves, state_failures;
    long unchecked; /* at the last scan: entries whose content could not be verified */
    long long bytes_in, bytes_out;
} tw_stats;

/* A tw is the folder: its records, what the last scan found, its state
 * file and its counters. It outlives connections. What belongs to one
 * connection (the peer's index as received, downloads and deletions
 * queued or in flight, how far our index was announced) exists between
 * tw_attach and tw_detach only.
 *
 * Detached, tw_step still reads the folder when that is due and writes
 * the state, so that changes made here while there is no connection are
 * recorded and go out with the index of the next one; nothing is sent
 * and nothing in the folder is changed. */
tw *tw_new(const tw_config *cfg);
void tw_free(tw *t);

/* A connection begins: the calls about the peer (tw_peer_cc, tw_offer,
 * tw_request, tw_response, tw_peer_index_complete) are for an attached
 * folder only. A scan in progress goes on. */
void tw_attach(tw *t);
/* The connection is over: a download in progress is given up (its
 * temporary file removed), everything queued and everything known about
 * the peer's index is dropped. Records and counters stay. */
void tw_detach(tw *t);
int tw_attached(const tw *t);

/* Loads (or migrates, or rebuilds) the state, checks .stfolder, reads the
 * folder and records local changes, writes the state. NULL, or an outcome
 * code (FOLDER_UNAVAILABLE, STATE_NOT_SAVED, OUT_OF_MEMORY). */
const char *tw_prepare(tw *t);

/* The same in steps, for a caller that serves other connections in
 * between: tw_prepare_begin does everything up to the reading of the
 * folder; tw_prepare_step reads it until the time `until` (net_now) and
 * sets *done once the folder has been read and the state written. Both
 * return NULL or an outcome code like tw_prepare. */
const char *tw_prepare_begin(tw *t);
const char *tw_prepare_step(tw *t, net_ms until, int *done);
/* Our own device entry for the ClusterConfig. */
void tw_cc_self(const tw *t, uint64_t *index_id, int64_t *max_sequence);

/* The peer's ClusterConfig: what it knows of our index (its device entry
 * for us; listed 0 when there is none). Sends our index from there.
 * NULL or an outcome code. */
const char *tw_peer_cc(tw *t, int listed, uint64_t index_id, int64_t max_sequence);
/* The peer's whole index (as known at connection) has arrived. */
const char *tw_peer_index_complete(tw *t);

const char *tw_offer(tw *t, const bep_file *fi);
const char *tw_request(tw *t, const bep_request *q);
const char *tw_response(tw *t, int32_t id, pb_slice data, uint32_t code);
const char *tw_step(tw *t, net_ms now, net_ms *wake);

/* Nothing queued, in flight or waiting to be written. */
int tw_idle(const tw *t);
/* Every entry on either side is the same version on both, or cannot be
 * synchronised here (held, ignored, rejected). */
int tw_settled(const tw *t);
/* Writes the state if anything is unwritten. 0 or -1. */
int tw_flush(tw *t);
/* For the connection in progress and the time since the one before it:
 * NULL when everything is in sync, otherwise FILES_FAILED,
 * STATE_NOT_SAVED, UNCHECKED (the last scan could not verify the content
 * of some entry: unreadable file or directory, a file that changed while
 * read, a refused Request not yet followed by a scan), NOT_IN_SYNC or
 * PARTIAL (held or unsupported entries). */
const char *tw_result(const tw *t);
/* 1 when everything recorded in memory is in the state file. */
int tw_state_saved(const tw *t);
/* The next reading of the folder is due now instead of after the
 * interval (rescan_seconds > 0 only). */
void tw_scan_soon(tw *t);
/* 1 while a scan started by tw_step has not reached its end. */
int tw_scanning(const tw *t);
/* Since the folder was opened. */
const tw_stats *tw_get_stats(const tw *t);
/* Since the last tw_detach (or since the folder was opened): what a
 * session's summary reports, and what tw_result judges. unchecked is
 * the last scan's in both. */
void tw_get_session_stats(const tw *t, tw_stats *out);
/* 1 when a record could not be kept in memory (out of memory, too many
 * entries): the folder should be opened again from its state file. */
int tw_state_lost(const tw *t);

#endif
