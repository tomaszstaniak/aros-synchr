#!/bin/bash
# The engine over time on this host: for MINUTES (default 30) files are
# created, edited and deleted on both sides (each side in its own set of
# names, so no conflict is expected), and the peer is restarted every
# RESTART_EVERY seconds (default 120). At the end both folders must hold
# the same files, the engine must still be the same process, stop cleanly
# and have no sanitizer report.
#
#   scripts/engine-soak-host.sh [MINUTES]     (uses build/host/stpoc-asan; run engine-host-test.sh first)
#
# Own peer (ports 22130/8398) and folders under build/engine-soak; kept
# after a failure, removed after a pass (KEEP=1 keeps them).
set -e
. "$(dirname "$0")/env.sh"
SYNCTHING_BIN=${TW_SYNCTHING_BIN:-$ST_ROOT/deps/downloads/syncthing-v2.1.7/syncthing-macos-arm64-v2.1.7/syncthing}
export SYNCTHING_BIN
MINUTES=${1:-30}
RESTART_EVERY=${RESTART_EVERY:-120}
W="$ST_ROOT/build/engine-soak"
export ST_PEER_HOME="$W/peer" ST_PEER_FOLDER_ID=aros-engine-soak ST_PEER_FOLDER_DIR="$W/peer-folder" \
    ST_PEER_PORT=22130 ST_PEER_GUI_PORT=8398 ST_PEER_FOLDER_TYPE=sendreceive
PEERCTL="$ST_ROOT/scripts/st-peer.sh"
LOCAL="$W/local"
BIN="$ST_ROOT/build/host/stpoc-asan"
LOG="$W/engine.log"
EPID=
PASSED=0
cleanup() {
    [ -n "$EPID" ] && kill -9 "$EPID" 2> /dev/null || true
    "$PEERCTL" stop > /dev/null 2>&1 || true
    if [ "$PASSED" = 1 ] && [ "${KEEP:-0}" != 1 ]; then rm -rf "$W"; fi
}
trap cleanup EXIT
fail() { echo "FAIL: $*"; tail -15 "$LOG"; exit 1; }
[ -x "$BIN" ] || { echo "no $BIN" >&2; exit 2; }

"$PEERCTL" stop > /dev/null 2>&1 || true
rm -rf "$W"
mkdir -p "$W/id" "$LOCAL" "$ST_PEER_FOLDER_DIR"
"$BIN" "$W/id.log" identity-create "$W/id" > /dev/null
ME=$(sed -n 's/.*identity: Device ID \([A-Z0-9-]*\).*/\1/p' "$W/id.log" | head -1)
"$PEERCTL" init > /dev/null
"$PEERCTL" start > /dev/null
"$PEERCTL" share "$ME" aros-engine-soak > /dev/null
PEER=$("$PEERCTL" id)
"$BIN" --log-max 400000 "$LOG" engine "$W/id" 127.0.0.1 "$ST_PEER_PORT" "$PEER" "$ST_PEER_FOLDER_ID" "$LOCAL" 5 \
    > "$W/console.txt" 2>&1 &
EPID=$!

change() { # DIR PREFIX: one random change in that side's own names
    local f="$1/$2$((RANDOM % 40)).dat"
    case $((RANDOM % 5)) in
    0) rm -f "$f" ;;
    1) head -c $((RANDOM * 40 + 1)) /dev/urandom > "$f" ;;   # up to about 1.3 MB
    *) head -c $((RANDOM % 5000 + 1)) /dev/urandom > "$f" ;;
    esac
}
end=$(( $(date +%s) + MINUTES * 60 ))
next_restart=$(( $(date +%s) + RESTART_EVERY ))
ops=0 restarts=0
while [ "$(date +%s)" -lt "$end" ]; do
    kill -0 "$EPID" 2> /dev/null || fail "the engine exited after $ops changes"
    change "$LOCAL" l
    change "$ST_PEER_FOLDER_DIR" p
    ops=$((ops + 2))
    if [ "$(date +%s)" -ge "$next_restart" ]; then
        "$PEERCTL" stop > /dev/null
        change "$LOCAL" l; change "$ST_PEER_FOLDER_DIR" p
        sleep $((RANDOM % 8 + 1))
        "$PEERCTL" start > /dev/null
        restarts=$((restarts + 1))
        next_restart=$(( $(date +%s) + RESTART_EVERY ))
    fi
    sleep $((RANDOM % 3 + 1))
done
"$PEERCTL" scan > /dev/null 2>&1 || true
list() { (cd "$1" && find . -type f ! -path './.stfolder*' ! -name '.syncthing.*' ! -name '.stpoc-*' | LC_ALL=C sort | xargs shasum -a 256 2> /dev/null); }
ok=0
for _ in $(seq 1 240); do
    kill -0 "$EPID" 2> /dev/null || fail "the engine exited during the final wait"
    [ "$(list "$LOCAL")" = "$(list "$ST_PEER_FOLDER_DIR")" ] && { ok=1; break; }
    sleep 0.5
done
[ "$ok" = 1 ] || { diff <(list "$LOCAL") <(list "$ST_PEER_FOLDER_DIR") | head -20; fail "contents differ 120 s after the last change"; }
[ -z "$(find "$LOCAL" "$ST_PEER_FOLDER_DIR" -name '*.sync-conflict-*')" ] || fail "conflict copies exist"
[ -z "$(find "$LOCAL" -name '.stpoc-*')" ] || fail "temporary files left"
kill -INT "$EPID"
for _ in $(seq 1 100); do kill -0 "$EPID" 2> /dev/null || break; sleep 0.2; done
kill -0 "$EPID" 2> /dev/null && fail "still running 20 s after SIGINT"
rc=0; wait "$EPID" || rc=$?; EPID=
[ "$rc" = 0 ] || fail "exit code $rc"
grep -q "ERROR: AddressSanitizer\|runtime error" "$W/console.txt" && fail "sanitizer report"
sessions=$(sed -n 's/.*stopped on request after \([0-9]*\) session.*/\1/p' "$LOG")
echo "PASS: engine soak, $MINUTES min, $ops changes, $restarts peer restarts, $sessions sessions, $(list "$LOCAL" | wc -l | tr -d ' ') files at the end"
PASSED=1
