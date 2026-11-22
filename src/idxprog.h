/* When is the peer's index complete?
 *
 * Syncthing sends its index in batches of at most 1000 files or 5000
 * blocks: the first as an Index, the rest as IndexUpdates
 * (lib/model/indexhandler.go sendIndexBatch, fileinfobatch.go), and no
 * Index at all when it has nothing to send. Receiving an Index therefore
 * says nothing about completeness. The peer's ClusterConfig lists the
 * peer itself for the folder with max_sequence, its highest sequence at
 * that moment (lib/model/model.go generateClusterConfig); files are sent
 * in sequence order and every batch carries the highest sequence in it.
 * The index known at connection time is complete once a sequence at least
 * that high has arrived. Changes made after the ClusterConfig arrive as
 * further IndexUpdates; those that lie wholly beyond it count as updates.
 *
 * The batches form a chain: every IndexUpdate's prev_sequence is the
 * last_sequence of the batch before it (indexhandler.go keeps
 * sentPrevSequence; the receiver checks it the same way and logs an
 * anomaly). A batch that does not follow on is an error here, never part
 * of a complete index. A second full Index, or a ClusterConfig announcing
 * another index ID for the peer, starts the peer's index again; this
 * client does not support that within a session. Sequences of files inside
 * the chain may have gaps. Peers must send last/prev_sequence (Syncthing
 * v2); only an Index without last_sequence falls back to its files'
 * highest sequence. */
#ifndef ST_IDXPROG_H
#define ST_IDXPROG_H

#include <stdint.h>

enum {
    IDX_OK = 0,         /* accepted */
    IDX_UPDATE = 1,     /* accepted; an update after the complete index */
    IDX_BAD_CHAIN = -1, /* prev_sequence does not follow on from the last batch */
    IDX_RESET = -2      /* the peer started its index again */
};

typedef struct {
    int have_target;
    uint64_t index_id; /* the peer's index ID from the first ClusterConfig */  /* the ClusterConfig gave the peer's own max_sequence */
    int64_t target;
    int64_t seen;     /* highest sequence received */
    long messages;    /* Index/IndexUpdate messages for the folder */
    int complete;     /* everything up to target has arrived */
    int updates;      /* IndexUpdates beyond target, received once complete */
} idxprog;

/* From the peer's ClusterConfig: the peer's max_sequence and index ID for
 * the folder. The first sets the target; a later one with another index ID
 * returns IDX_RESET. */
int idxprog_cc(idxprog *p, int64_t max_sequence, uint64_t index_id);
/* idxprog_cc without an index ID (tests). */
void idxprog_target(idxprog *p, int64_t max_sequence);

/* Would a message of this kind with this prev_sequence be accepted?
 * IDX_OK, IDX_BAD_CHAIN or IDX_RESET; changes nothing. Check before acting
 * on any file of the message. */
int idxprog_check(const idxprog *p, int is_update, int64_t prev);

/* One Index or IndexUpdate for the folder: its prev/last_sequence fields
 * (0 when absent) and the highest sequence among its files (0 if none).
 * Returns IDX_OK, IDX_UPDATE, IDX_BAD_CHAIN or IDX_RESET; after an error
 * the state is not changed. */
int idxprog_message(idxprog *p, int is_update, int64_t prev, int64_t last, int64_t max_file_seq);

#endif
