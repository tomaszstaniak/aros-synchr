/* One BEP session with one peer for one folder, receive-only: Hello,
 * ClusterConfig, an Index of our own, then the peer's Index and
 * IndexUpdate messages, reported file by file. An index that ends before
 * the peer's announced sequence is INDEX_INCOMPLETE, never a PASS. With a destination set, the
 * files are also downloaded (fetch.c) and announced once written. */
#ifndef ST_SESSION_H
#define ST_SESSION_H

#include "tls.h"

typedef struct {
    const char *peer_ip;
    unsigned peer_port;
    unsigned char peer_id[DEVID_BYTES];
    const char *folder_id;
    const char *device_name;  /* sent in our Hello */
    long seconds;             /* overall limit for the whole session */
    int want_updates;         /* stop after this many IndexUpdates beyond the complete index (0: none) */
    const char *dest;         /* sync mode: download into this directory; NULL: report only */
    long abort_after_blocks;  /* test hook, see fetch.h; 0 normally */
    const char *identity_dir; /* sync mode: the state file is kept here */
    int state_fault;          /* test hook: 1 makes every state write fail */
    const char *commit_hook;  /* test hook: directory, see fetch_set_commit_hook; NULL normally */
    int two_way;              /* dest is synchronised both ways (tw.h) instead of received only */
    long rescan_seconds;      /* two-way: read the folder again this often during the session (0: at start only) */
    tls_log_fn log;
    tls_log_fn ui;            /* short user-facing lines; NULL for none */
} session_config;

/* Two-way mode: the folder is offered as Send & Receive, our index (kept
 * with a persistent index ID in the state file) is sent from where the
 * peer's ClusterConfig says it has it, the peer's block requests are
 * served, and the session completes when both indexes agree (tw_settled).
 *
 * Runs the session. Returns 0 when the scenario completed (folder offered,
 * the peer's whole index up to the max_sequence of its ClusterConfig
 * received, the wanted updates seen, and in sync mode every accepted
 * entry written without failures, no local conflicts and the state saved),
 * otherwise nonzero; *result names the outcome ("PASS" or a reason code). */
int session_run(const session_config *cfg, const tls_identity *id, const char **result);

#endif
