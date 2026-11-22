#include "idxprog.h"

static void settle(idxprog *p)
{
    if (p->have_target && p->seen >= p->target)
        p->complete = 1;
}

int idxprog_cc(idxprog *p, int64_t max_sequence, uint64_t index_id)
{
    if (p->have_target)
        return index_id == p->index_id ? IDX_OK : IDX_RESET;
    p->have_target = 1;
    p->index_id = index_id;
    p->target = max_sequence < 0 ? 0 : max_sequence;
    settle(p);
    return IDX_OK;
}

void idxprog_target(idxprog *p, int64_t max_sequence)
{
    (void)idxprog_cc(p, max_sequence, 0);
}

int idxprog_check(const idxprog *p, int is_update, int64_t prev)
{
    if (!is_update) /* only the first message of the peer's index is an Index */
        return p->messages > 0 ? IDX_RESET : IDX_OK;
    /* The chain: an IndexUpdate follows on from the batch before. */
    return p->messages == 0 || prev != p->seen ? IDX_BAD_CHAIN : IDX_OK;
}

int idxprog_message(idxprog *p, int is_update, int64_t prev, int64_t last, int64_t max_file_seq)
{
    int64_t high = last > max_file_seq ? last : max_file_seq;
    int c = idxprog_check(p, is_update, prev);
    if (c != IDX_OK)
        return c;
    int was_complete = p->complete;
    p->messages++;
    if (high > p->seen)
        p->seen = high;
    settle(p);
    /* An update: sent after the complete index and wholly beyond what the
     * ClusterConfig covered. */
    if (is_update && was_complete && prev >= p->target) {
        p->updates++;
        return IDX_UPDATE;
    }
    return IDX_OK;
}
