/* Completeness of the peer's index (idxprog.c), including the case that
 * ended sessions early: a first batch that needs nothing, then more. */
#include "../src/idxprog.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    idxprog p;

    /* Empty folder: max_sequence 0, Syncthing sends no Index at all. */
    memset(&p, 0, sizeof p);
    CHECK(!p.complete, "complete before the ClusterConfig");
    idxprog_target(&p, 0);
    CHECK(p.complete, "empty folder not complete after the ClusterConfig");

    /* Three batches: 1..1000 as Index, 1001..2000 and 2001..2005 as updates. */
    memset(&p, 0, sizeof p);
    idxprog_target(&p, 2005);
    CHECK(idxprog_message(&p, 0, 0, 1000, 1000) == 0, "Index counted as update");
    CHECK(!p.complete, "first batch taken for the whole index");
    CHECK(idxprog_message(&p, 1, 1000, 2000, 2000) == 0, "index batch counted as update");
    CHECK(!p.complete, "second batch taken for the whole index");
    CHECK(idxprog_message(&p, 1, 2000, 2005, 2005) == 0, "last batch counted as update");
    CHECK(p.complete, "not complete after the last batch");
    /* A change made after the ClusterConfig. */
    CHECK(idxprog_message(&p, 1, 2005, 2006, 2006) == IDX_UPDATE && p.updates == 1, "update not counted");

    /* A batch that also carries changes made after the ClusterConfig
     * completes the index but is not an update itself. */
    memset(&p, 0, sizeof p);
    idxprog_target(&p, 10);
    CHECK(idxprog_message(&p, 0, 0, 12, 12) == 0 && p.complete, "batch beyond the target");
    CHECK(idxprog_message(&p, 1, 12, 13, 13) == IDX_UPDATE, "following update not counted");

    /* An Index without last_sequence: its files' highest sequence counts;
     * the following IndexUpdate must follow on from that. */
    memset(&p, 0, sizeof p);
    idxprog_target(&p, 7);
    CHECK(idxprog_message(&p, 0, 0, 0, 4) == IDX_OK && !p.complete, "file sequence 4 of 7");
    CHECK(idxprog_message(&p, 1, 4, 7, 7) == IDX_OK && p.complete, "not complete at 7");

    /* The target comes from the first ClusterConfig only. */
    memset(&p, 0, sizeof p);
    idxprog_target(&p, 3);
    idxprog_target(&p, 300);
    idxprog_message(&p, 0, 0, 3, 3);
    CHECK(p.complete && p.target == 3, "later ClusterConfig moved the target");

    /* Without a target (the peer did not list itself) nothing is complete. */
    memset(&p, 0, sizeof p);
    idxprog_message(&p, 0, 0, 5, 5);
    CHECK(!p.complete, "complete without a target");

    /* Review 2026-11-09: a batch that does not follow on from the last
     * one is an error, not a completed index. */
    memset(&p, 0, sizeof p);
    idxprog_target(&p, 30);
    CHECK(idxprog_message(&p, 0, 0, 10, 10) == IDX_OK, "first batch");
    CHECK(idxprog_check(&p, 1, 20) == IDX_BAD_CHAIN && idxprog_check(&p, 1, 10) == IDX_OK, "check");
    CHECK(idxprog_message(&p, 1, 20, 30, 30) == IDX_BAD_CHAIN, "prev 20 after 10 accepted");
    CHECK(!p.complete, "complete after a broken chain");

    /* An IndexUpdate without prev_sequence after earlier batches. */
    memset(&p, 0, sizeof p);
    idxprog_target(&p, 30);
    idxprog_message(&p, 0, 0, 10, 10);
    CHECK(idxprog_message(&p, 1, 0, 30, 30) == IDX_BAD_CHAIN, "prev 0 after 10 accepted");

    /* A second full Index (after completion, or before) is a reset. */
    memset(&p, 0, sizeof p);
    idxprog_target(&p, 10);
    idxprog_message(&p, 0, 0, 10, 10);
    CHECK(p.complete, "not complete");
    CHECK(idxprog_message(&p, 0, 0, 5, 5) == IDX_RESET, "second Index accepted");
    memset(&p, 0, sizeof p);
    idxprog_target(&p, 30);
    idxprog_message(&p, 0, 0, 10, 10);
    CHECK(idxprog_message(&p, 0, 0, 20, 20) == IDX_RESET, "second Index before completion accepted");

    /* A later ClusterConfig with another index ID for the peer: reset. */
    memset(&p, 0, sizeof p);
    CHECK(idxprog_cc(&p, 10, 0x1111) == IDX_OK, "first cc");
    CHECK(idxprog_cc(&p, 12, 0x1111) == IDX_OK, "same index ID again");
    CHECK(p.target == 10, "target moved by a later cc");
    CHECK(idxprog_cc(&p, 3, 0x2222) == IDX_RESET, "new index ID accepted");

    /* Gaps between file sequences inside the chain are normal. */
    memset(&p, 0, sizeof p);
    idxprog_target(&p, 50);
    CHECK(idxprog_message(&p, 0, 0, 17, 17) == IDX_OK, "batch 1");
    CHECK(idxprog_message(&p, 1, 17, 50, 50) == IDX_OK && p.complete, "batch 2 with sparse sequences");

    printf("%s: test_idxprog\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
