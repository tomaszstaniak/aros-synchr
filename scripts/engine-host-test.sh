#!/bin/bash
# The engine (stpoc engine, src/engine.h) against an unmodified Syncthing
# on this host: started before the peer is there, it connects when the
# peer appears, carries changes both ways while it runs, reconnects after
# the peer was restarted, keeps its log within the limit and stops cleanly
# on SIGINT; a second start announces nothing again. A large file is
# hashed in steps. A stop request is served while the peer takes no data
# and during a handshake that never completes. A stop while the
# state cannot be written is reported as a failure, and a log that cannot
# be rotated is continued, not emptied.
#
#   scripts/engine-host-test.sh            (builds build/host/stpoc-asan)
#
# Own peer instance (ports 22150/8397), folders and identity under
# build/engine-host-test, removed at the end (KEEP=1 keeps them).
set -e
. "$(dirname "$0")/env.sh"
st_require HOST_OPENSSL
SYNCTHING_BIN=${TW_SYNCTHING_BIN:-$ST_ROOT/deps/downloads/syncthing-v2.1.7/syncthing-macos-arm64-v2.1.7/syncthing}
[ -x "$SYNCTHING_BIN" ] || { echo "no Syncthing binary at $SYNCTHING_BIN (set TW_SYNCTHING_BIN)" >&2; exit 2; }
export SYNCTHING_BIN
W="$ST_ROOT/build/engine-host-test"
export ST_PEER_HOME="$W/peer" ST_PEER_FOLDER_ID=aros-engine-test ST_PEER_FOLDER_DIR="$W/peer-folder" \
    ST_PEER_PORT=22150 ST_PEER_GUI_PORT=8397 ST_PEER_FOLDER_TYPE=sendreceive
PEERCTL="$ST_ROOT/scripts/st-peer.sh"
LOCAL="$W/local"
BIN="$ST_ROOT/build/host/stpoc-asan"
LOG="$W/engine.log"
LOGMAX=60000
EPID=
TPID=
SPID=

cleanup() {
    [ -n "$EPID" ] && kill -9 "$EPID" 2> /dev/null || true
    [ -n "$TPID" ] && kill "$TPID" 2> /dev/null || true
    [ -n "$SPID" ] && kill "$SPID" 2> /dev/null || true
    pkill -CONT -f "syncthing.*$ST_PEER_HOME" 2> /dev/null || true
    "$PEERCTL" stop > /dev/null 2>&1 || true
    chmod u+w "$W/id" 2> /dev/null || true
    if [ "${KEEP:-0}" != 1 ]; then rm -rf "$W"; fi
}
trap cleanup EXIT
fail() { echo "FAIL: $*"; [ -f "$LOG" ] && tail -15 "$LOG"; exit 1; }

SRCS=$(sed -n '/^stpoc)/,/;;/p' "$ST_ROOT/scripts/build.sh" | grep -o '\$ST_ROOT/src/[a-z0-9]*\.c' | sed "s#\$ST_ROOT#$ST_ROOT#")
cc -std=c99 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    -D_POSIX_C_SOURCE=200809L -DNET_TEST_HOOKS -I"$HOST_OPENSSL/include" $SRCS -L"$HOST_OPENSSL/lib" -lssl -lcrypto -o "$BIN"

"$PEERCTL" stop > /dev/null 2>&1 || true
rm -rf "$W"
mkdir -p "$W/id" "$LOCAL/sub" "$ST_PEER_FOLDER_DIR"
printf 'from the peer\n' > "$ST_PEER_FOLDER_DIR/p1.txt"
printf 'created here\n' > "$LOCAL/a.txt"
printf 'nested here\n' > "$LOCAL/sub/b.txt"
"$BIN" "$W/id.log" identity-create "$W/id" > /dev/null || fail "identity"
ME=$(sed -n 's/.*identity: Device ID \([A-Z0-9-]*\).*/\1/p' "$W/id.log" | head -1)
"$PEERCTL" init > /dev/null
"$PEERCTL" start > /dev/null
"$PEERCTL" share "$ME" aros-engine-test > /dev/null
PEER=$("$PEERCTL" id)
"$PEERCTL" stop > /dev/null

. "$ST_ROOT/scripts/tw-lib.sh"

same() {
    [ "$(cd "$LOCAL" && find . -type f ! -path './.stfolder*' | LC_ALL=C sort | xargs shasum -a 256)" = \
      "$(cd "$ST_PEER_FOLDER_DIR" && find . -type f ! -path './.stfolder*' | LC_ALL=C sort | xargs shasum -a 256)" ]
}
converge() { # NAME [SECONDS]: both folders hold the same files
    for _ in $(seq 1 $(( ${2:-40} * 2 ))); do
        kill -0 "$EPID" 2> /dev/null || fail "$1: the engine exited"
        same && { echo "  $1: both sides equal"; return 0; }
        sleep 0.5
    done
    hashes_equal "$1"
}
start_engine() {
    "$BIN" --log-max $LOGMAX "$LOG" engine "$W/id" 127.0.0.1 "$ST_PEER_PORT" "$PEER" "$ST_PEER_FOLDER_ID" "$LOCAL" 2 \
        > "$W/console.txt" 2>&1 &
    EPID=$!
}
stop_engine() { # NAME: SIGINT, clean exit within 10 s
    kill -INT "$EPID"
    for _ in $(seq 1 50); do kill -0 "$EPID" 2> /dev/null || break; sleep 0.2; done
    kill -0 "$EPID" 2> /dev/null && fail "$1: still running 10 s after SIGINT"
    local rc=0
    wait "$EPID" || rc=$?
    EPID=
    [ "$rc" = 0 ] || fail "$1: exit code $rc"
    [ "$(tail -1 "$LOG" | sed 's/^\[[^]]*\] //')" = "RESULT PASS STOPPED" ] || fail "$1: last line $(tail -1 "$LOG")"
    grep -q "ERROR: AddressSanitizer\|runtime error" "$W/console.txt" && fail "$1: sanitizer report"
    echo "  $1: stopped cleanly"
}

echo "engine host test, peer $("$SYNCTHING_BIN" --version | cut -d' ' -f1-2)"

# e1: the engine starts with no peer listening and keeps trying.
start_engine
sleep 4
kill -0 "$EPID" 2> /dev/null || fail "e1: the engine exited without a peer"
grep -q 'engine: session 1 ended: .*next attempt in' "$LOG" || fail "e1: no second attempt planned"
echo "  e1: running without a peer, retrying"
"$PEERCTL" start > /dev/null
converge e1-first 90
no_conflicts e1

# e2: a file created here while it runs.
printf 'made while running\n' > "$LOCAL/c.txt"
converge e2-local-new
# e3: an edit on the peer.
printf 'created here, edited on the peer\n' > "$ST_PEER_FOLDER_DIR/a.txt"
"$PEERCTL" scan > /dev/null
converge e3-peer-edit
# e4: a deletion here.
rm "$LOCAL/sub/b.txt"
converge e4-local-delete
no_conflicts e4

# e5: the peer is restarted; changes on both sides meanwhile.
"$PEERCTL" stop > /dev/null
printf 'written while the peer was away\n' > "$LOCAL/d.txt"
printf 'written on the peer before it came back\n' > "$ST_PEER_FOLDER_DIR/p2.txt"
sleep 3
kill -0 "$EPID" 2> /dev/null || fail "e5: the engine exited when the peer went away"
"$PEERCTL" start > /dev/null
converge e5-peer-restart 90
no_conflicts e5
n=$(cat "$LOG" "$LOG.old" 2> /dev/null | grep -c 'engine: session [0-9]* ended') || true
[ "$n" -ge 2 ] || fail "e5: expected at least two ended sessions, saw $n"

# e6: many changes, so the log passes its limit.
for i in $(seq 1 300); do printf 'file %d\n' "$i" > "$LOCAL/n$i.txt"; done
converge e6-many 120
peer_idle e6
[ -f "$LOG.old" ] || fail "e6: the log was not rotated"
for f in "$LOG" "$LOG.old"; do
    sz=$(wc -c < "$f")
    [ "$sz" -le $((LOGMAX + 2000)) ] || fail "e6: $f has $sz bytes, limit $LOGMAX"
done
echo "  e6: log rotated, both files within the limit"

# e6b: a large file made here: its hash is done in steps between which the
# connection is served, and it arrives on the peer.
dd if=/dev/urandom of="$W/seed" bs=1048576 count=4 2> /dev/null
# The log is rotated during the transfer: the line is caught as it is written.
(tail -n 0 -F "$LOG" 2> /dev/null | grep --line-buffered 'scan: done in [0-9]* step' > "$W/steps.txt") 2> /dev/null &
TPID=$!
for _ in $(seq 1 64); do cat "$W/seed"; done > "$LOCAL/large.bin"
converge e6b-large 180
pkill -P "$TPID" 2> /dev/null || true
kill "$TPID" 2> /dev/null || true
wait "$TPID" 2> /dev/null || true
TPID=
[ -s "$W/steps.txt" ] || fail "e6b: no scan was done in steps"
echo "  e6b: $(sed 's/^\[[^]]*\] //' "$W/steps.txt" | sort -t' ' -k4 -n | tail -1)"
rm "$LOCAL/large.bin"
converge e6b-removed 60

# e7: stop, then a second run announces nothing again.
sleep 3
stop_engine e7-stop
seq7=$(our_sequence)
rm -f "$LOG"
start_engine
for _ in $(seq 1 60); do grep -q 'engine: in sync with the peer' "$LOG" 2> /dev/null && break; sleep 0.5; done
grep -q 'engine: in sync with the peer' "$LOG" || fail "e8: not in sync after the restart"
stop_engine e8-restart
[ "$(our_sequence)" = "$seq7" ] || fail "e8: sequence moved from $seq7 to $(our_sequence)"
grep -q ' 0 announced' "$LOG" || fail "e8: something was announced again"
same || fail "e8: contents differ"
no_conflicts e8

# e9: the state cannot be written when the stop arrives: not reported as a
# clean stop; the next run publishes the change.
rm -f "$LOG"
start_engine
for _ in $(seq 1 60); do grep -q 'engine: in sync with the peer' "$LOG" 2> /dev/null && break; sleep 0.5; done
grep -q 'engine: in sync with the peer' "$LOG" || fail "e9: not in sync"
chmod u-w "$W/id"
printf 'made while the state could not be written\n' > "$LOCAL/unsaved.txt"
for _ in $(seq 1 60); do grep -q 'state: could not be written' "$LOG" && break; sleep 0.5; done
grep -q 'state: could not be written' "$LOG" || fail "e9: no failed state write seen"
kill -INT "$EPID"
rc=0
wait "$EPID" || rc=$?
EPID=
chmod u+w "$W/id"
[ "$rc" = 10 ] || fail "e9: exit code $rc after a stop with unsaved state, expected 10"
[ "$(tail -1 "$LOG" | sed 's/^\[[^]]*\] //')" = "RESULT FAIL STATE_NOT_SAVED" ] || fail "e9: last line $(tail -1 "$LOG")"
[ -f "$ST_PEER_FOLDER_DIR/unsaved.txt" ] && fail "e9: the change reached the peer although the state was not saved"
echo "  e9: stop with unsaved state reported as STATE_NOT_SAVED"
rm -f "$LOG"
start_engine
converge e9-next-run
stop_engine e9-next-run
no_conflicts e9

# e10: the previous log cannot be replaced: the log keeps its beginning.
LOG="$W/norotate.log"
mkdir "$LOG.old"
touch "$LOG.old/in-the-way"
LOGMAX=3000
start_engine
for _ in $(seq 1 60); do grep -q 'log: not rotated' "$LOG" 2> /dev/null && break; sleep 0.5; done
for i in $(seq 1 20); do printf 'more %d\n' "$i" > "$LOCAL/m$i.txt"; done
converge e10-changes
stop_engine e10-stop
grep -q 'log: not rotated' "$LOG" || fail "e10: no note about the failed rotation"
head -1 "$LOG" | grep -q 'aros-syncthing PoC' || fail "e10: the log lost its beginning"
[ "$(wc -c < "$LOG")" -gt $LOGMAX ] || fail "e10: the log did not grow past the limit"
[ -f "$LOG.old/in-the-way" ] || fail "e10: the directory in the way was changed"
echo "  e10: log kept when it could not be rotated"

# e11: the peer stops taking data in the middle of an upload: the engine
# still stops within seconds (nothing waits inside a write).
LOG="$W/engine.log"
LOGMAX=60000
rm -f "$LOG"
start_engine
for _ in $(seq 1 60); do grep -q 'engine: in sync with the peer' "$LOG" 2> /dev/null && break; sleep 0.5; done
grep -q 'engine: in sync with the peer' "$LOG" || fail "e11: not in sync"
for _ in $(seq 1 64); do cat "$W/seed"; done > "$LOCAL/upload.bin"
for _ in $(seq 1 120); do
    cat "$LOG" "$LOG.old" 2> /dev/null | grep -q 'upload.bin' && [ -n "$(find "$ST_PEER_FOLDER_DIR" -name '*upload.bin*' 2> /dev/null)" ] && break
    sleep 0.25
done
PPID_ST=$(pgrep -f "syncthing.*$ST_PEER_HOME" | head -1)
[ -n "$PPID_ST" ] || fail "e11: the peer's process was not found"
pkill -STOP -f "syncthing.*$ST_PEER_HOME"
sleep 3
t_stop=$(date +%s)
kill -INT "$EPID"
for _ in $(seq 1 100); do kill -0 "$EPID" 2> /dev/null || break; sleep 0.1; done
took=$(( $(date +%s) - t_stop ))
pkill -CONT -f "syncthing.*$ST_PEER_HOME"
kill -0 "$EPID" 2> /dev/null && fail "e11: still running 10 s after SIGINT with a peer that takes nothing"
rc=0
wait "$EPID" || rc=$?
EPID=
[ "$rc" = 0 ] || fail "e11: exit code $rc"
[ "$(tail -1 "$LOG" | sed 's/^\[[^]]*\] //')" = "RESULT PASS STOPPED" ] || fail "e11: last line $(tail -1 "$LOG")"
echo "  e11: stopped in ${took} s while the peer took nothing"
rm -f "$LOG"
start_engine
converge e11-upload-finished 180
stop_engine e11-next-run

# e12: a peer that accepts the connection and never answers the TLS
# handshake: a stop request ends the attempt at once.
python3 -c '
import socket, sys, time
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 22151)); s.listen(4)
keep = []
s.settimeout(60)
try:
    while True: keep.append(s.accept())
except Exception: pass
' &
SPID=$!
sleep 1
LOG="$W/silent.log"
"$BIN" "$LOG" engine "$W/id" 127.0.0.1 22151 "$PEER" "$ST_PEER_FOLDER_ID" "$LOCAL" 2 > "$W/console.txt" 2>&1 &
EPID=$!
for _ in $(seq 1 40); do grep -q 'tcp: connected' "$LOG" 2> /dev/null && break; sleep 0.25; done
grep -q 'tcp: connected' "$LOG" || fail "e12: no connection to the silent peer"
sleep 1
kill -INT "$EPID"
for _ in $(seq 1 20); do kill -0 "$EPID" 2> /dev/null || break; sleep 0.1; done
kill -0 "$EPID" 2> /dev/null && fail "e12: still running 2 s after SIGINT during the handshake"
rc=0
wait "$EPID" || rc=$?
EPID=
kill "$SPID" 2> /dev/null || true
wait "$SPID" 2> /dev/null || true
SPID=
[ "$rc" = 0 ] || fail "e12: exit code $rc"
grep -q 'tls: connection attempt stopped on request' "$LOG" || fail "e12: the attempt was not ended by the stop request"
[ "$(tail -1 "$LOG" | sed 's/^\[[^]]*\] //')" = "RESULT PASS STOPPED" ] || fail "e12: last line $(tail -1 "$LOG")"
echo "  e12: stop during a handshake that never completes"

# e13: a socket that takes less than one TLS record at a time and a peer
# that takes data slowly: an upload longer than the send time limit still
# counts as progress (the limit is 10 s here; the upload takes longer).
"$PEERCTL" limit 150 recv > /dev/null
LOG="$W/slow.log"
head -c 4000000 /dev/urandom > "$LOCAL/slow.bin"
STPOC_TEST_SEND_PIECE=3000 STPOC_TEST_SEND_SECONDS=10 "$BIN" "$LOG" engine "$W/id" 127.0.0.1 "$ST_PEER_PORT" "$PEER" \
    "$ST_PEER_FOLDER_ID" "$LOCAL" 2 > "$W/console.txt" 2>&1 &
EPID=$!
t_up=$(date +%s)
converge e13-slow-upload 120
took=$(( $(date +%s) - t_up ))
"$PEERCTL" limit 0 recv > /dev/null
grep -q 'SEND_TIMEOUT' "$LOG" && fail "e13: a session ended with SEND_TIMEOUT while data was moving"
[ "$took" -ge 15 ] || fail "e13: the upload took only $took s, the send limit of 10 s was not passed"
stop_engine e13-stop
echo "  e13: slow upload over $took s without a send timeout"

echo "PASS: engine host test"
