/* The decision table (src/decide.h), one case per row of the table in
 * docs/backlog/two-way-sync.md plus its preconditions. */
#include "../src/bep.h"
#include "../src/decide.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

/* An entry: type 'f' file, 'd' directory, 'x' deleted file, 'X' deleted
 * directory; content id (blocks), modification time, vector, previous
 * content id (0: none). */
static finfo E(char kind, int content, int64_t mod, const char *vec, int prev)
{
    finfo f;
    memset(&f, 0, sizeof f);
    f.name = "n";
    f.type = (kind == 'd' || kind == 'X') ? BEP_DIRECTORY : BEP_FILE;
    f.deleted = kind == 'x' || kind == 'X';
    f.modified_s = mod;
    if (vv_parse(vec, &f.version) != 0)
        exit(2);
    if (f.type == BEP_FILE && !f.deleted) {
        f.blocks = calloc(1, sizeof *f.blocks);
        f.blocks[0].size = 1;
        f.blocks[0].hash[0] = (unsigned char)content;
        f.nblocks = 1;
        f.size = 1;
        finfo_blocks_hash(f.blocks, 1, f.blocks_hash);
        f.has_blocks_hash = 1;
    }
    if (prev) {
        finfo_block b = { 0, 1, { (unsigned char)prev } };
        finfo_blocks_hash(&b, 1, f.prev_blocks_hash);
        f.has_prev_blocks_hash = 1;
    }
    return f;
}

static void drop(finfo *f)
{
    free(f->blocks);
    vv_free(&f->version);
}

static void expect(const char *what, finfo *L, enum disk_state disk, finfo *R, enum decide_action a, int row)
{
    decision d = decide(L, disk, R);
    checks++;
    if (d.action != a || d.row != row) {
        failures++;
        printf("FAIL %s: %s row %d (%s), want %s row %d\n", what, decide_action_name(d.action), d.row, d.why,
               decide_action_name(a), row);
    }
    if (L)
        drop(L);
    drop(R);
}

int main(void)
{
    /* Short IDs: 1 = us, 2 = the peer. Later modification time wins a
     * conflict; equal times fall back to the vector comparison. */
    finfo L, R;

    /* Preconditions */
    R = E('f', 1, 10, "2:5", 0); R.invalid = 1;
    expect("peer entry invalid", NULL, DISK_MISSING, &R, ACT_IGNORE, 0);
    R = E('f', 1, 10, "2:5", 0); R.type = BEP_SYMLINK;
    expect("symbolic link", NULL, DISK_MISSING, &R, ACT_IGNORE, 0);
    L = E('f', 1, 10, "1:5", 0); R = E('f', 2, 10, "1:5,2:6", 0);
    expect("disk cannot be checked", &L, DISK_UNKNOWN, &R, ACT_HOLD, 0);
    R = E('f', 1, 10, "2:5", 0);
    expect("unrecorded file on disk", NULL, DISK_DIFFERENT, &R, ACT_RESCAN, 0);
    L = E('f', 1, 10, "1:5", 0); R = E('f', 2, 10, "1:5,2:6", 0);
    expect("unscanned local edit (row 7 premise)", &L, DISK_DIFFERENT, &R, ACT_RESCAN, 0);
    L = E('f', 1, 10, "1:5", 0); R = E('f', 2, 10, "1:5,2:6", 0);
    expect("recorded file missing here", &L, DISK_MISSING, &R, ACT_RESCAN, 0);
    L = E('x', 0, 10, "1:6", 0); R = E('f', 2, 10, "1:5,2:6", 0);
    expect("recorded deletion, but a file is here", &L, DISK_AS_RECORDED, &R, ACT_RESCAN, 0);
    L = E('f', 1, 10, "1:5", 0); R = E('d', 0, 10, "1:5,2:6", 0);
    expect("file here, directory there", &L, DISK_AS_RECORDED, &R, ACT_HOLD, 0);

    /* Row 1 */
    L = E('f', 1, 10, "1:5,2:3", 0); R = E('f', 1, 10, "1:5,2:3", 0);
    expect("row 1 equal", &L, DISK_AS_RECORDED, &R, ACT_NOTHING, 1);
    /* Row 2 */
    L = E('f', 2, 10, "1:6,2:3", 0); R = E('f', 1, 10, "1:5,2:3", 0);
    expect("row 2 ours newer", &L, DISK_AS_RECORDED, &R, ACT_NOTHING, 2);
    L = E('x', 0, 10, "1:6,2:3", 0); R = E('f', 1, 10, "1:5,2:3", 0);
    expect("row 2 our deletion newer", &L, DISK_MISSING, &R, ACT_NOTHING, 2);
    /* Row 3 */
    L = E('f', 1, 10, "1:5", 0); R = E('f', 2, 11, "1:5,2:6", 1);
    expect("row 3 newer on the peer", &L, DISK_AS_RECORDED, &R, ACT_DOWNLOAD, 3);
    L = E('f', 1, 10, "1:5", 0); R = E('f', 1, 11, "1:5,2:6", 1);
    expect("row 3 newer, same content", &L, DISK_AS_RECORDED, &R, ACT_RECORD, 3);
    L = E('x', 0, 10, "1:5", 0); R = E('f', 2, 11, "1:5,2:6", 0);
    expect("row 3 newer than our deletion", &L, DISK_MISSING, &R, ACT_DOWNLOAD, 3);
    L = E('d', 0, 10, "1:5", 0); R = E('d', 0, 11, "1:5,2:6", 0);
    expect("row 3 directory newer", &L, DISK_AS_RECORDED, &R, ACT_DOWNLOAD, 3);
    /* Row 4 */
    L = E('f', 1, 10, "1:5", 0); R = E('x', 0, 12, "1:5,2:6", 0);
    expect("row 4 deleted on the peer", &L, DISK_AS_RECORDED, &R, ACT_DELETE, 4);
    L = E('x', 0, 10, "1:5", 0); R = E('x', 0, 12, "1:5,2:6", 0);
    expect("row 4 deleted on both, theirs newer", &L, DISK_MISSING, &R, ACT_RECORD, 4);
    /* Row 5 */
    R = E('f', 1, 10, "2:5", 0);
    expect("row 5 new on the peer", NULL, DISK_MISSING, &R, ACT_DOWNLOAD, 5);
    R = E('x', 0, 10, "2:5", 0);
    expect("row 5 deletion of something never here", NULL, DISK_MISSING, &R, ACT_NOTHING, 5);
    /* Row 6: concurrent, the peer's winning version was based on ours */
    L = E('f', 1, 10, "1:6,2:4", 0); R = E('f', 2, 20, "1:5,2:5", 1);
    expect("row 6 based on our content", &L, DISK_AS_RECORDED, &R, ACT_DOWNLOAD, 6);
    /* Row 7: concurrent edits */
    L = E('f', 1, 10, "1:6", 0); R = E('f', 2, 20, "2:6", 3);
    expect("row 7 both edited, peer newer", &L, DISK_AS_RECORDED, &R, ACT_CONFLICT_DOWNLOAD, 7);
    L = E('f', 1, 30, "1:6", 0); R = E('f', 2, 20, "2:6", 3);
    expect("row 7 both edited, ours newer", &L, DISK_AS_RECORDED, &R, ACT_NOTHING, 7);
    L = E('f', 1, 30, "1:6", 0); R = E('f', 2, 20, "2:6", 1);
    expect("row 7 ours wins even if theirs was based on ours", &L, DISK_AS_RECORDED, &R, ACT_NOTHING, 7);
    /* Equal times: the vector comparison decides. Compare({2:6}, {1:6})
     * is ConcurrentLesser (the counter missing on the other side is met
     * first for the lower ID), so {1:6} wins. */
    L = E('f', 1, 20, "2:6", 0); R = E('f', 2, 20, "1:6", 0);
    expect("row 7 equal time, vector decides (peer)", &L, DISK_AS_RECORDED, &R, ACT_CONFLICT_DOWNLOAD, 7);
    L = E('f', 1, 20, "1:6", 0); R = E('f', 2, 20, "2:6", 0);
    expect("row 7 equal time, vector decides (ours)", &L, DISK_AS_RECORDED, &R, ACT_NOTHING, 7);
    L = E('f', 1, 10, "1:6,2:4", 0); R = E('x', 0, 20, "1:5,2:5", 0);
    expect("row 7 deleted there after our edit", &L, DISK_AS_RECORDED, &R, ACT_CONFLICT_DELETE, 7);
    L = E('f', 1, 30, "1:6,2:4", 0); R = E('x', 0, 20, "1:5,2:5", 0);
    expect("row 7 our edit after their deletion", &L, DISK_AS_RECORDED, &R, ACT_NOTHING, 7);
    L = E('d', 0, 10, "1:6", 0); R = E('X', 0, 20, "2:6", 0);
    expect("row 7 directory deleted there, deletion wins", &L, DISK_AS_RECORDED, &R, ACT_DELETE, 7);
    /* Row 8 */
    L = E('x', 0, 10, "1:6", 0); R = E('f', 2, 20, "2:6", 0);
    expect("row 8 edit there wins over our deletion", &L, DISK_MISSING, &R, ACT_DOWNLOAD, 8);
    L = E('x', 0, 30, "1:6", 0); R = E('f', 2, 20, "2:6", 0);
    expect("row 8 our deletion wins", &L, DISK_MISSING, &R, ACT_NOTHING, 8);
    /* Row 9 */
    L = E('x', 0, 10, "1:6", 0); R = E('x', 0, 20, "2:6", 0);
    expect("row 9 both deleted, theirs wins", &L, DISK_MISSING, &R, ACT_RECORD, 9);
    L = E('x', 0, 30, "1:6", 0); R = E('x', 0, 20, "2:6", 0);
    expect("row 9 both deleted, ours wins", &L, DISK_MISSING, &R, ACT_NOTHING, 9);
    /* Row 10 */
    L = E('f', 7, 10, "1:6", 0); R = E('f', 7, 20, "2:6", 0);
    expect("row 10 same content, theirs wins", &L, DISK_AS_RECORDED, &R, ACT_RECORD, 10);
    L = E('f', 7, 30, "1:6", 0); R = E('f', 7, 20, "2:6", 0);
    expect("row 10 same content, ours wins", &L, DISK_AS_RECORDED, &R, ACT_NOTHING, 10);
    L = E('d', 0, 10, "1:6", 0); R = E('d', 0, 20, "2:6", 0);
    expect("row 10 directory on both sides", &L, DISK_AS_RECORDED, &R, ACT_RECORD, 10);

    /* Both devices decide the same: for a concurrent pair, exactly one
     * side keeps its version (the other downloads or deletes). */
    static const struct { char lk; int lc; int64_t lm; const char *lv; char rk; int rc; int64_t rm; const char *rv; }
        pairs[] = {
            { 'f', 1, 10, "1:6", 'f', 2, 20, "2:6" }, { 'f', 1, 20, "1:6", 'f', 2, 20, "2:6" },
            { 'f', 1, 10, "1:6,2:4", 'x', 0, 20, "1:5,2:5" }, { 'f', 1, 30, "1:6,2:4", 'x', 0, 20, "1:5,2:5" },
            { 'x', 0, 10, "1:6", 'x', 0, 20, "2:6" }, { 'f', 7, 10, "1:6", 'f', 7, 10, "2:6" },
        };
    for (size_t i = 0; i < sizeof pairs / sizeof *pairs; i++) {
        finfo a = E(pairs[i].lk, pairs[i].lc, pairs[i].lm, pairs[i].lv, 0);
        finfo b = E(pairs[i].rk, pairs[i].rc, pairs[i].rm, pairs[i].rv, 0);
        decision here = decide(&a, a.deleted ? DISK_MISSING : DISK_AS_RECORDED, &b);
        decision there = decide(&b, b.deleted ? DISK_MISSING : DISK_AS_RECORDED, &a);
        int keeps_here = here.action == ACT_NOTHING, keeps_there = there.action == ACT_NOTHING;
        checks++;
        if (keeps_here == keeps_there) {
            failures++;
            printf("FAIL symmetric pair %lu: here %s, there %s\n", (unsigned long)i, decide_action_name(here.action),
                   decide_action_name(there.action));
        }
        drop(&a);
        drop(&b);
    }

    printf("test_decide: %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
