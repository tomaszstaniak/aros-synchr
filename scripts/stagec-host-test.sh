#!/bin/bash
# Stage C of docs/backlog/two-way-sync.md against an unmodified Syncthing on
# this host: deletions both ways, edits on both sides (each winner), edit
# against deletion (both orders), a directory deleted here, a download cut
# off by killing the client, a restart that changes nothing, a lost state
# and a missing .stfolder. After every step both folders hold the same
# files with the same SHA-256 and the same directories, and the peer is
# idle with nothing needed.
#
#   scripts/stagec-host-test.sh
#
# Own peer instance (ports 22111/8397), folders and identity under
# build/stagec-host-test, removed at the end (KEEP=1 keeps them).
set -e
. "$(dirname "$0")/env.sh"
st_require HOST_OPENSSL
SYNCTHING_BIN=${TW_SYNCTHING_BIN:-$ST_ROOT/deps/downloads/syncthing-v2.1.7/syncthing-macos-arm64-v2.1.7/syncthing}
[ -x "$SYNCTHING_BIN" ] || { echo "no Syncthing binary at $SYNCTHING_BIN (set TW_SYNCTHING_BIN)" >&2; exit 2; }
export SYNCTHING_BIN
W="$ST_ROOT/build/stagec-host-test"
export ST_PEER_HOME="$W/peer" ST_PEER_FOLDER_ID=aros-stagec ST_PEER_FOLDER_DIR="$W/peer-folder" \
    ST_PEER_PORT=22111 ST_PEER_GUI_PORT=8397 ST_PEER_FOLDER_TYPE=sendreceive
PEERCTL="$ST_ROOT/scripts/st-peer.sh"
LOCAL="$W/local"
P="$ST_PEER_FOLDER_DIR"
BIN="$ST_ROOT/build/host/stpoc-asan"

cleanup() {
    "$PEERCTL" stop > /dev/null 2>&1 || true
    if [ "${KEEP:-0}" != 1 ]; then rm -rf "$W"; fi
}
trap cleanup EXIT
fail() { echo "FAIL: $*"; exit 1; }

SRCS=$(sed -n '/^stpoc)/,/;;/p' "$ST_ROOT/scripts/build.sh" | grep -o '\$ST_ROOT/src/[a-z0-9]*\.c' | sed "s#\$ST_ROOT#$ST_ROOT#")
cc -std=c99 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    -DTW_TEST_HOOKS -D_POSIX_C_SOURCE=200809L -I"$HOST_OPENSSL/include" $SRCS -L"$HOST_OPENSSL/lib" -lssl -lcrypto -o "$BIN"

. "$ST_ROOT/scripts/tw-lib.sh"

at() { # FILE SECONDS-FROM-NOW: set the modification time
    python3 -c 'import os,sys,time; t=time.time()+float(sys.argv[2]); os.utime(sys.argv[1],(t,t))' "$1" "$2"
}
peer_scan() { "$PEERCTL" scan > /dev/null; sleep 3; }
dirs_equal() {
    local a b
    a=$(cd "$LOCAL" && find . -type d ! -path './.stfolder*' | LC_ALL=C sort)
    b=$(cd "$P" && find . -type d ! -path './.stfolder*' | LC_ALL=C sort)
    [ "$a" = "$b" ] || { echo "local: $a"; echo "peer: $b"; fail "$1: directories differ"; }
}
conflicts() { # DIR: number of conflict copies
    find "$1" -name '*.sync-conflict-*' | wc -l | tr -d ' '
}
converged() { # NAME [VERSION-FILES...]
    local n=$1
    shift
    hashes_equal "$n"
    dirs_equal "$n"
    peer_idle "$n"
    for f in "$@"; do versions_equal "$n" "$f"; done
    [ -z "$(find "$LOCAL" -name '.stpoc-*')" ] || fail "$n: leftover temporary files here: $(find "$LOCAL" -name '.stpoc-*')"
}
gone_both() { # NAME FILE
    [ ! -e "$LOCAL/$2" ] && [ ! -e "$P/$2" ] || fail "$1: $2 still exists (here: $([ -e "$LOCAL/$2" ] && echo yes || echo no), peer: $([ -e "$P/$2" ] && echo yes || echo no))"
}

"$PEERCTL" stop > /dev/null 2>&1 || true
rm -rf "$W"
mkdir -p "$W/id" "$LOCAL/dir/deep" "$P"
for f in a b c d e; do printf '%s, created here\n' "$f" > "$LOCAL/$f.txt"; done
printf 'inside\n' > "$LOCAL/dir/in.txt"
printf 'deeper\n' > "$LOCAL/dir/deep/x.txt"
printf 'from the peer\n' > "$P/p1.txt"
printf 'also from the peer\n' > "$P/p2.txt"
"$BIN" "$W/id.log" identity-create "$W/id" > /dev/null || fail "identity"
ME=$(sed -n 's/.*identity: Device ID \([A-Z0-9-]*\).*/\1/p' "$W/id.log" | head -1)
"$PEERCTL" init > /dev/null
"$PEERCTL" start > /dev/null
"$PEERCTL" share "$ME" aros-stagec > /dev/null
PEER=$("$PEERCTL" id)
sleep 2

echo "stage C host test, peer $("$SYNCTHING_BIN" --version | cut -d' ' -f1-2)"
session c1-first
converged c1 a.txt p1.txt

# Deleted here: a file, and a directory with everything in it.
rm "$LOCAL/a.txt"
rm -r "$LOCAL/dir"
session c2-deleted-here
grep -q '"a.txt" deleted here' "$W/c2-deleted-here.log" || fail "c2: deletion not published"
peer_idle c2
gone_both c2 a.txt
gone_both c2 dir
converged c2

# Deleted on the peer.
rm "$P/b.txt"
peer_scan
session c3-deleted-there
grep -q '"b.txt" deleted (deleted on the peer)' "$W/c3-deleted-there.log" || fail "c3: the peer's deletion not applied"
gone_both c3 b.txt
converged c3

# Edited on both sides, the peer later: the peer's version wins, ours is
# kept here as a conflict copy and reaches the peer.
printf 'c, edited here first\n' > "$LOCAL/c.txt"
at "$LOCAL/c.txt" -120
printf 'c, edited on the peer later\n' > "$P/c.txt"
peer_scan
session c4-both-peer-wins
grep -q '"c.txt" edited on both sides, the peer.s version wins; ours kept as' "$W/c4-both-peer-wins.log" || fail "c4: no conflict copy made here"
[ "$(cat "$LOCAL/c.txt")" = "c, edited on the peer later" ] || fail "c4: c.txt is not the peer's version"
session c4b-again
converged c4 c.txt
[ "$(conflicts "$LOCAL")" = 1 ] || fail "c4: $(conflicts "$LOCAL") conflict copies here, 1 expected"
grep -l 'c, edited here first' "$LOCAL"/c.sync-conflict-* > /dev/null || fail "c4: our edit is not in the conflict copy"

# Edited on both sides, ours later: ours wins; the peer keeps its own as a
# conflict copy, which comes here.
printf 'd, edited on the peer first\n' > "$P/d.txt"
at "$P/d.txt" -120
peer_scan
printf 'd, edited here later\n' > "$LOCAL/d.txt"
session c5-both-ours-wins
session c5b-again
sleep 3
session c5c-again
converged c5 d.txt
[ "$(cat "$LOCAL/d.txt")" = "d, edited here later" ] || fail "c5: d.txt is not our version"
[ "$(conflicts "$LOCAL")" = 2 ] || fail "c5: $(conflicts "$LOCAL") conflict copies here, 2 expected"
grep -l 'd, edited on the peer first' "$LOCAL"/d.sync-conflict-* > /dev/null || fail "c5: the peer's edit is not kept"

# Edited here, deleted on the peer later: the deletion wins, our edit is
# kept as a conflict copy.
printf 'e, edited here before the deletion\n' > "$LOCAL/e.txt"
at "$LOCAL/e.txt" -120
rm "$P/e.txt"
peer_scan
session c6-edit-then-delete
grep -q '"e.txt" deleted on the peer (the deletion wins); ours kept as' "$W/c6-edit-then-delete.log" || fail "c6: no conflict copy for the edit"
session c6b-again
gone_both c6 e.txt
converged c6
grep -l 'e, edited here before the deletion' "$LOCAL"/e.sync-conflict-* > /dev/null || fail "c6: our edit is lost"

# Deleted here, edited on the peer later: the edit wins, the file comes back.
rm "$LOCAL/p1.txt"
sleep 2
printf 'p1, edited on the peer after the deletion here\n' > "$P/p1.txt"
at "$P/p1.txt" 60
peer_scan
session c7-delete-then-edit
session c7b-again
[ "$(cat "$LOCAL/p1.txt")" = "p1, edited on the peer after the deletion here" ] || fail "c7: p1.txt did not come back"
converged c7 p1.txt

# A download cut off: the client stops dead (as at a crash or power loss)
# after 40 of 306 blocks of a large file; the next session removes the
# temporary file and completes the download.
head -c 40000000 /dev/urandom > "$P/big.bin"
peer_scan
STPOC_TEST_EXIT_AFTER_BLOCKS=40 "$BIN" "$W/c8-killed.log" twoway "$W/id" 127.0.0.1 "$ST_PEER_PORT" "$PEER" \
    "$ST_PEER_FOLDER_ID" "$LOCAL" 120 > /dev/null 2>&1 && fail "c8: the session was not stopped"
[ -n "$(find "$LOCAL" -name '.stpoc-big.bin.tmp')" ] || fail "c8: no temporary file left by the stopped session"
[ ! -e "$LOCAL/big.bin" ] || fail "c8: big.bin present after the stop"
echo "  c8-killed: stopped during the download ($(du -k "$LOCAL/.stpoc-big.bin.tmp" | cut -f1) KiB written)"
session c9-after-kill
grep -q 'removed ".stpoc-big.bin.tmp", left by an interrupted download' "$W/c9-after-kill.log" || fail "c9: the temporary file was not cleaned up"
converged c9 big.bin

# A state write cut off: the peer adds a file and deletes another, the
# client stops dead after writing the new state next to the old one; the
# next session works from the old state and the files on disk.
printf 'p3, from the peer\n' > "$P/p3.txt"
rm "$P/p2.txt"
peer_scan
STPOC_TEST_EXIT_IN_STATE_WRITE=1 "$BIN" "$W/c9s-killed.log" twoway "$W/id" 127.0.0.1 "$ST_PEER_PORT" "$PEER" \
    "$ST_PEER_FOLDER_ID" "$LOCAL" 60 > /dev/null 2>&1 && fail "c9s: the session was not stopped"
echo "  c9s-killed: stopped in the state write ($(ls "$W"/id | tr '\n' ' '))"
session c9t-after-state-kill
session c9u-again
gone_both c9u p2.txt
converged c9u p3.txt
[ "$(conflicts "$LOCAL")" = 3 ] || fail "c9u: $(conflicts "$LOCAL") conflict copies, 3 expected"

# Restart: nothing to do, nothing announced, no new conflict copies.
seq9=$(our_sequence)
session c10-restart
[ "$(our_sequence)" = "$seq9" ] || fail "c10: sequence moved from $seq9 to $(our_sequence)"
grep -q ' 0 announced' "$W/c10-restart.log" || fail "c10: something was announced again"
[ "$(conflicts "$LOCAL")" = 3 ] || fail "c10: $(conflicts "$LOCAL") conflict copies, 3 expected"
converged c10

# State lost and a file missing here: rebuilt, no deletion published, the
# file comes back from the peer.
rm "$W"/id/state-*.txt
rm "$LOCAL/c.txt"
session c11-state-lost
grep -q 'deleted here' "$W/c11-state-lost.log" && grep -q 'scan: "c.txt" deleted here' "$W/c11-state-lost.log" && fail "c11: deletion published after a state loss"
[ -e "$P/c.txt" ] || fail "c11: c.txt deleted on the peer"
session c11b-again
converged c11 c.txt

# No .stfolder: unavailable, nothing changes on either side.
mv "$LOCAL/.stfolder" "$W/stfolder-aside"
"$BIN" "$W/c12-no-marker.log" twoway "$W/id" 127.0.0.1 "$ST_PEER_PORT" "$PEER" "$ST_PEER_FOLDER_ID" "$LOCAL" 60 > /dev/null 2>&1 && fail "c12: PASS without .stfolder"
tail -1 "$W/c12-no-marker.log" | grep -q 'FOLDER_UNAVAILABLE' || fail "c12: $(tail -1 "$W/c12-no-marker.log")"
mv "$W/stfolder-aside" "$LOCAL/.stfolder"
converged c12

echo "  files: $(cd "$LOCAL" && find . -type f ! -path './.stfolder*' | LC_ALL=C sort | tr '\n' ' ')"
echo "PASS: stage C host test"
