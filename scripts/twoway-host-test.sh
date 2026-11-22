#!/bin/bash
# Two-way first flow against an unmodified Syncthing on this host (stage B
# of docs/backlog/two-way-sync.md): a file created here reaches the peer,
# the peer's edit comes back, a local edit goes out, and no restart causes
# a conflict or announces anything again.
#
#   scripts/twoway-host-test.sh            (needs build/host/stpoc-asan or builds it)
#
# Uses its own peer instance (ports 22110/8396), its own folders and a
# throw-away identity under build/twoway-host-test, all removed at the end
# (KEEP=1 keeps them for inspection). The peer binary is SYNCTHING_BIN, by
# default the v2.1.7 download in deps/downloads.
set -e
. "$(dirname "$0")/env.sh"
st_require HOST_OPENSSL
SYNCTHING_BIN=${TW_SYNCTHING_BIN:-$ST_ROOT/deps/downloads/syncthing-v2.1.7/syncthing-macos-arm64-v2.1.7/syncthing}
[ -x "$SYNCTHING_BIN" ] || { echo "no Syncthing binary at $SYNCTHING_BIN (set TW_SYNCTHING_BIN)" >&2; exit 2; }
export SYNCTHING_BIN
W="$ST_ROOT/build/twoway-host-test"
export ST_PEER_HOME="$W/peer" ST_PEER_FOLDER_ID=aros-tw-test ST_PEER_FOLDER_DIR="$W/peer-folder" \
    ST_PEER_PORT=22110 ST_PEER_GUI_PORT=8396 ST_PEER_FOLDER_TYPE=sendreceive
PEERCTL="$ST_ROOT/scripts/st-peer.sh"
LOCAL="$W/local"
BIN="$ST_ROOT/build/host/stpoc-asan"

cleanup() {
    "$PEERCTL" stop > /dev/null 2>&1 || true
    if [ "${KEEP:-0}" != 1 ]; then rm -rf "$W"; fi
}
trap cleanup EXIT

fail() { echo "FAIL: $*"; exit 1; }

# The client, with AddressSanitizer and UBSan.
SRCS=$(sed -n '/^stpoc)/,/;;/p' "$ST_ROOT/scripts/build.sh" | grep -o '\$ST_ROOT/src/[a-z0-9]*\.c' | sed "s#\$ST_ROOT#$ST_ROOT#")
cc -std=c99 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    -D_POSIX_C_SOURCE=200809L -I"$HOST_OPENSSL/include" $SRCS -L"$HOST_OPENSSL/lib" -lssl -lcrypto -o "$BIN"

"$PEERCTL" stop > /dev/null 2>&1 || true
rm -rf "$W"
mkdir -p "$W/id" "$LOCAL/sub" "$ST_PEER_FOLDER_DIR"
printf 'from the peer\n' > "$ST_PEER_FOLDER_DIR/p1.txt"
printf 'created on AROS\n' > "$LOCAL/a.txt"
printf 'nested here\n' > "$LOCAL/sub/b.txt"
"$BIN" "$W/id.log" identity-create "$W/id" > /dev/null || fail "identity"
ME=$(sed -n 's/.*identity: Device ID \([A-Z0-9-]*\).*/\1/p' "$W/id.log" | head -1)
"$PEERCTL" init > /dev/null
"$PEERCTL" start > /dev/null
"$PEERCTL" share "$ME" aros-tw-test > /dev/null
PEER=$("$PEERCTL" id)
sleep 2

. "$ST_ROOT/scripts/tw-lib.sh"

echo "two-way host test, peer $("$SYNCTHING_BIN" --version | cut -d' ' -f1-2)"
session s1-first
hashes_equal s1; peer_idle s1; no_conflicts s1; versions_equal s1 a.txt
seq1=$(our_sequence)

session s2-restart
[ "$(our_sequence)" = "$seq1" ] || fail "s2: sequence moved from $seq1 to $(our_sequence)"
grep -q ' 0 announced' "$W/s2-restart.log" || fail "s2: something was announced again"
no_conflicts s2

printf 'created on AROS, edited on the peer\n' > "$ST_PEER_FOLDER_DIR/a.txt"
"$PEERCTL" scan > /dev/null
sleep 2
session s3-peer-edit
grep -q '"a.txt": newer version on the peer (row 3)' "$W/s3-peer-edit.log" || fail "s3: the edit was not taken by row 3"
hashes_equal s3; peer_idle s3; no_conflicts s3; versions_equal s3 a.txt
seq3=$(our_sequence)

session s4-restart
[ "$(our_sequence)" = "$seq3" ] || fail "s4: sequence moved from $seq3 to $(our_sequence)"
grep -q ' 0 announced' "$W/s4-restart.log" || fail "s4: something was announced again"

sleep 1
printf 'edited again here, a little longer\n' > "$LOCAL/a.txt"
session s5-local-edit
grep -q '"a.txt" changed here: new version' "$W/s5-local-edit.log" || fail "s5: local edit not found"
hashes_equal s5; peer_idle s5; no_conflicts s5; versions_equal s5 a.txt
seq5=$(our_sequence)

session s6-restart
[ "$(our_sequence)" = "$seq5" ] || fail "s6: sequence moved from $seq5 to $(our_sequence)"
grep -q ' 0 announced' "$W/s6-restart.log" || fail "s6: something was announced again"
hashes_equal s6; no_conflicts s6

echo "PASS: two-way host test (6 sessions)"
