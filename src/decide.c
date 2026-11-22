#include "decide.h"

#include "bep.h"

static decision D(enum decide_action a, int row, const char *why)
{
    decision d = { a, row, why };
    return d;
}

const char *decide_action_name(enum decide_action a)
{
    switch (a) {
    case ACT_NOTHING: return "nothing";
    case ACT_IGNORE: return "ignore";
    case ACT_HOLD: return "hold";
    case ACT_RESCAN: return "rescan";
    case ACT_DOWNLOAD: return "download";
    case ACT_RECORD: return "record";
    case ACT_DELETE: return "delete";
    case ACT_CONFLICT_DOWNLOAD: return "conflict-copy+download";
    case ACT_CONFLICT_DELETE: return "conflict-copy+delete";
    }
    return "?";
}

static int supported(uint32_t type) { return type == BEP_FILE || type == BEP_DIRECTORY; }

decision decide(const finfo *L, enum disk_state disk, const finfo *R)
{
    if (R->invalid)
        return D(ACT_IGNORE, 0, "the peer marks it invalid");
    if (!supported(R->type))
        return D(ACT_IGNORE, 0, "entry type not supported here (symbolic link or unknown)");
    if (disk == DISK_UNKNOWN)
        return D(ACT_HOLD, 0, "what is on disk could not be checked");

    if (!L) {
        if (disk != DISK_MISSING)
            return D(ACT_RESCAN, 0, "something is here that we have no record of");
        if (R->deleted)
            return D(ACT_NOTHING, 5, "deleted on the peer, never here");
        return D(ACT_DOWNLOAD, 5, "new on the peer");
    }
    /* The disk must be what L says before L can be compared. */
    if (L->deleted ? disk != DISK_MISSING : disk != DISK_AS_RECORDED)
        return D(ACT_RESCAN, 0, "changed here since it was recorded");
    if (!L->deleted && !R->deleted && L->type != R->type)
        return D(ACT_HOLD, 0, "changed between file and directory (not handled yet)");

    int dir = (L->deleted ? R->type : L->type) == BEP_DIRECTORY;
    switch (vv_compare(&R->version, &L->version)) {
    case VV_EQUAL:
        return D(ACT_NOTHING, 1, "same version");
    case VV_LESSER:
        return D(ACT_NOTHING, 2, "ours is newer; the peer takes ours");
    case VV_GREATER:
        if (R->deleted)
            return L->deleted ? D(ACT_RECORD, 4, "deleted on both sides; the peer's version is newer")
                              : D(ACT_DELETE, 4, "deleted on the peer after our version");
        if (L->deleted)
            return D(ACT_DOWNLOAD, 3, "the peer's version is newer than our deletion");
        if (!dir && finfo_same_blocks(L, R))
            return D(ACT_RECORD, 3, "newer version on the peer, same content as here");
        return D(ACT_DOWNLOAD, 3, "newer version on the peer");
    case VV_CONCURRENT_LESSER:
    case VV_CONCURRENT_GREATER:
        break;
    }

    /* Concurrent: both devices pick the same winner (WinsConflict). */
    int r_wins = finfo_wins_conflict(R, L);
    if (L->deleted && R->deleted)
        return r_wins ? D(ACT_RECORD, 9, "deleted on both sides; the peer's version wins")
                      : D(ACT_NOTHING, 9, "deleted on both sides; our version wins");
    if (L->deleted)
        return r_wins ? D(ACT_DOWNLOAD, 8, "edited on the peer, deleted here: the edit wins")
                      : D(ACT_NOTHING, 8, "edited on the peer, deleted here: the deletion wins");
    if (dir) {
        if (R->deleted)
            return r_wins ? D(ACT_DELETE, 7, "directory deleted on the peer, the deletion wins")
                          : D(ACT_NOTHING, 7, "directory deleted on the peer, ours wins");
        return r_wins ? D(ACT_RECORD, 10, "directory on both sides; the peer's version wins")
                      : D(ACT_NOTHING, 10, "directory on both sides; our version wins");
    }
    if (R->deleted)
        return r_wins ? D(ACT_CONFLICT_DELETE, 7, "deleted on the peer, edited here: the deletion wins")
                      : D(ACT_NOTHING, 7, "deleted on the peer, edited here: the edit wins");
    if (finfo_same_blocks(L, R))
        return r_wins ? D(ACT_RECORD, 10, "changed on both sides to the same content; the peer's version wins")
                      : D(ACT_NOTHING, 10, "changed on both sides to the same content; our version wins");
    /* Only the winning version is ever pulled; the loser's side makes the
     * conflict copy (or none, when the winner was based on its content). */
    if (!r_wins)
        return D(ACT_NOTHING, 7, "edited on both sides: our version wins");
    if (!finfo_in_conflict_with(R, L))
        return D(ACT_DOWNLOAD, 6, "the peer's winning version was based on the content we have");
    return D(ACT_CONFLICT_DOWNLOAD, 7, "edited on both sides: the peer's version wins");
}
