/* What to do with one name when the peer announces an entry for it: the
 * decision table of docs/backlog/two-way-sync.md ("Rules fixed before
 * coding", section 3), as a pure function. The row numbers are the
 * table's; tests/test_decide.c has a case for each.
 *
 * The caller states what is on disk compared with our record; this
 * function never looks at the disk. Whatever it returns, the action that
 * touches the disk still re-checks it at that moment (store_commit, the
 * deletion), so a change made in between is never lost. */
#ifndef ST_DECIDE_H
#define ST_DECIDE_H

#include "finfo.h"

enum disk_state {
    DISK_AS_RECORDED, /* what our record describes (for a deletion: nothing there) */
    DISK_DIFFERENT,   /* something else than the record, or a file we have no record of */
    DISK_MISSING,     /* nothing under the name */
    DISK_UNKNOWN      /* could not be checked (unreadable, scan incomplete) */
};

enum decide_action {
    ACT_NOTHING,          /* in sync, or ours is the version to keep */
    ACT_IGNORE,           /* the peer's entry is not one we take (invalid, unsupported type) */
    ACT_HOLD,             /* not decided safely now: leave everything, report */
    ACT_RESCAN,           /* the disk differs from the record: scan the name, then decide again */
    ACT_DOWNLOAD,         /* fetch the peer's version (a directory: create it) and record it */
    ACT_RECORD,           /* take the peer's version as ours; nothing to change on disk */
    ACT_DELETE,           /* remove ours (it is the recorded version) and record the deletion */
    ACT_CONFLICT_DOWNLOAD,/* ours becomes a conflict copy, then as ACT_DOWNLOAD */
    ACT_CONFLICT_DELETE   /* ours becomes a conflict copy, then record the deletion */
};

typedef struct {
    enum decide_action action;
    int row;          /* the table row; 0 for the preconditions */
    const char *why;  /* short reason for the log */
} decision;

/* local: our record for the name, NULL when there is none. */
decision decide(const finfo *local, enum disk_state disk, const finfo *remote);
const char *decide_action_name(enum decide_action a);

#endif
