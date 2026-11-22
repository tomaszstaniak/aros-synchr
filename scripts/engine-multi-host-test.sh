#!/bin/bash
# The engine with a configuration file (src/conf.h) on this host: two
# Syncthing devices, the first with two folders and the second with one,
# all kept in sync at once; a device that refuses connections and one
# that accepts them and never answers hold up nothing; a folder whose
# directory is missing and a folder the peer does not share stay out
# while the others go on, and the missing one joins once its directory
# exists; a restart of one device does not touch the other's session; a
# folder is read and recorded without a connection and keeps its
# counters over a reconnection; PAUSE and RESUME through the control
# interface. The second device has no typed address (`address dynamic`):
# it is found by a local discovery announcement, made up here and sent to
# the engine over the loopback interface.
#
#   scripts/engine-multi-host-test.sh      (builds build/host/stpoc-asan)
#
# Own peers (ports 22152/8401 and 22153/8402; 22154 is left closed and
# 22155 is the silent listener; announcements go to UDP 21930, not to
# Syncthing's 21027), everything under build/engine-multi-test,
# removed at the end (KEEP=1 keeps it).
set -e
. "$(dirname "$0")/env.sh"
st_require HOST_OPENSSL
SYNCTHING_BIN=${TW_SYNCTHING_BIN:-$ST_ROOT/deps/downloads/syncthing-v2.1.7/syncthing-macos-arm64-v2.1.7/syncthing}
[ -x "$SYNCTHING_BIN" ] || { echo "no Syncthing binary at $SYNCTHING_BIN (set TW_SYNCTHING_BIN)" >&2; exit 2; }
export SYNCTHING_BIN
W="$ST_ROOT/build/engine-multi-test"
PEERCTL="$ST_ROOT/scripts/st-peer.sh"
BIN="$ST_ROOT/build/host/stpoc-asan"
LOG="$W/engine.log"
EPID=
SPID=
pa() { ST_PEER_HOME="$W/peerA" ST_PEER_PORT=22152 ST_PEER_GUI_PORT=8401 ST_PEER_FOLDER_TYPE=sendreceive "$PEERCTL" "$@"; }
pb() { ST_PEER_HOME="$W/peerB" ST_PEER_PORT=22153 ST_PEER_GUI_PORT=8402 ST_PEER_FOLDER_TYPE=sendreceive "$PEERCTL" "$@"; }
# FOLDER-ID of peer A or B: its directory there
pdir() { echo "$W/peer-$1"; }
ldir() { echo "$W/local-$1"; }

cleanup() {
    [ -n "$EPID" ] && kill -9 "$EPID" 2> /dev/null || true
    [ -n "$SPID" ] && kill "$SPID" 2> /dev/null || true
    pa stop > /dev/null 2>&1 || true
    pb stop > /dev/null 2>&1 || true
    if [ "${KEEP:-0}" != 1 ]; then rm -rf "$W"; fi
}
trap cleanup EXIT
fail() { echo "FAIL: $*"; [ -f "$LOG" ] && tail -20 "$LOG"; exit 1; }

SRCS=$(sed -n '/^stpoc)/,/;;/p' "$ST_ROOT/scripts/build.sh" | grep -o '\$ST_ROOT/src/[a-z0-9]*\.c' | sed "s#\$ST_ROOT#$ST_ROOT#")
cc -std=c99 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    -D_POSIX_C_SOURCE=200809L -DNET_TEST_HOOKS -I"$HOST_OPENSSL/include" $SRCS -L"$HOST_OPENSSL/lib" -lssl -lcrypto -o "$BIN"

pa stop > /dev/null 2>&1 || true
pb stop > /dev/null 2>&1 || true
rm -rf "$W"
mkdir -p "$W/id"
for f in a1 a2 b1 b2; do
    mkdir -p "$(pdir $f)" "$(ldir $f)"
    printf 'from the peer, folder %s\n' $f > "$(pdir $f)/peer-$f.txt"
    printf 'made here, folder %s\n' $f > "$(ldir $f)/here-$f.txt"
done
mkdir -p "$(pdir a3)" "$(ldir dead)" "$(ldir silent)" "$(ldir notshared)"
printf 'from the peer, folder a3\n' > "$(pdir a3)/peer-a3.txt"
"$BIN" "$W/id.log" identity-create "$W/id" > /dev/null || fail "identity"
ME=$(sed -n 's/.*identity: Device ID \([A-Z0-9-]*\).*/\1/p' "$W/id.log" | head -1)
"$BIN" "$W/other.log" identity-create "$W/other-id" > /dev/null 2>&1 || { mkdir -p "$W/other-id"; "$BIN" "$W/other.log" identity-create "$W/other-id" > /dev/null; }
OTHER=$(sed -n 's/.*identity: Device ID \([A-Z0-9-]*\).*/\1/p' "$W/other.log" | head -1)
"$BIN" "$W/other2.log" identity-create "$W/other2-id" > /dev/null 2>&1 || { mkdir -p "$W/other2-id"; "$BIN" "$W/other2.log" identity-create "$W/other2-id" > /dev/null; }
OTHER2=$(sed -n 's/.*identity: Device ID \([A-Z0-9-]*\).*/\1/p' "$W/other2.log" | head -1)
[ -n "$ME" ] && [ -n "$OTHER" ] && [ -n "$OTHER2" ] || fail "identities"
pa init > /dev/null
pb init > /dev/null
pa start > /dev/null
pb start > /dev/null
for f in a1 a2 a3; do
    ST_PEER_FOLDER_ID=multi-$f ST_PEER_FOLDER_LABEL=multi-$f ST_PEER_FOLDER_DIR="$(pdir $f)" pa share "$ME" aros-multi > /dev/null
done
for f in b1 b2; do
    ST_PEER_FOLDER_ID=multi-$f ST_PEER_FOLDER_LABEL=multi-$f ST_PEER_FOLDER_DIR="$(pdir $f)" pb share "$ME" aros-multi > /dev/null
done
A=$(pa id)
B=$(pb id)

cat > "$W/engine.conf" <<CONF
# engine-multi-host-test
name aros-multi
# Nothing beyond the test: no listener, no public servers, no announcements.
listen off
global-discovery off
relays off
local-announce off

device alpha
  id $A
  address 127.0.0.1:22152
device beta
  id $B
  address dynamic
device closed
  id $OTHER
  address 127.0.0.1:22154
device silent
  id $OTHER2
  address 127.0.0.1:22155

folder multi-a1
  path $(ldir a1)
  with alpha
  rescan 2
folder multi-b1
  path $(ldir b1)
  with beta
  rescan 2
folder multi-b2
  path $(ldir b2)
  with beta
  rescan 3600
folder multi-a2
  path $(ldir a2)
  with alpha
  rescan 2
folder multi-a3
  path $(ldir a3)
  with alpha
  rescan 2
folder multi-none
  path $(ldir notshared)
  with alpha
  rescan 2
folder multi-dead
  path $(ldir dead)
  with closed
folder multi-silent
  path $(ldir silent)
  with silent
CONF

list() { (cd "$1" && find . -type f ! -path './.stfolder*' ! -name '.syncthing.*' ! -name '.stpoc-*' | LC_ALL=C sort | xargs shasum -a 256 2> /dev/null); }
same() { [ "$(list "$(ldir $1)")" = "$(list "$(pdir $1)")" ]; }
converge() { # NAME SECONDS FOLDER...
    local name=$1 secs=$2 f ok
    shift 2
    for _ in $(seq 1 $((secs * 2))); do
        kill -0 "$EPID" 2> /dev/null || fail "$name: the engine exited"
        ok=1
        for f in "$@"; do same $f || ok=0; done
        [ $ok = 1 ] && { echo "  $name: both sides equal ($*)"; return 0; }
        sleep 0.5
    done
    for f in "$@"; do same $f || diff <(list "$(ldir $f)") <(list "$(pdir $f)") | head -6; done
    fail "$name: folders differ after $secs s"
}
# DEVICE-ID ADDRESS...: one local discovery announcement of that device
# with these addresses, sent from 127.0.0.1 to the engine's test port.
DISCO_PORT=21930
announce() {
    python3 - "$DISCO_PORT" "$@" <<'PY'
import base64, socket, struct, sys
port, devid, addresses = int(sys.argv[1]), sys.argv[2].replace("-", ""), sys.argv[3:]
assert len(devid) == 56
text = "".join(c for i, c in enumerate(devid) if i % 14 != 13)  # without the check characters
raw = base64.b32decode(text + "====")
assert len(raw) == 32
msg = b"\x0a\x20" + raw
for a in addresses:
    a = a.encode()
    assert len(a) < 128
    msg += b"\x12" + bytes([len(a)]) + a
msg += b"\x18\x2a"  # instance_id 42
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.sendto(struct.pack(">I", 0x2EA7D90B) + msg, ("127.0.0.1", port))
PY
}
count() { local c; c=$(grep -c "$1" "$LOG" 2> /dev/null) || true; echo "${c:-0}"; }

python3 -c '
import socket
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 22155)); s.listen(8)
keep = []
s.settimeout(600)
try:
    while True: keep.append(s.accept())
except Exception: pass
' &
SPID=$!
sleep 1

echo "engine multi-folder host test, peer $("$SYNCTHING_BIN" --version | cut -d' ' -f1-2)"
export STPOC_CONTROL="$W/control"
export STPOC_DISCOVERY_PORT=$DISCO_PORT
ctl() { "$BIN" ctl "$@"; }
rc=0
ctl STATUS > "$W/ctl.txt" 2>&1 || rc=$?
[ "$rc" = 20 ] || fail "ctl without an engine returned $rc"
"$BIN" --log-max 4000000 "$LOG" engine "$W/id" "$W/engine.conf" > "$W/console.txt" 2>&1 &
EPID=$!

# d1: the device without a typed address. Nothing is tried until an
# announcement with its Device ID arrives; packets that are not that
# change nothing; with it the device is connected at once. An
# announcement of another address during the connection leaves it alone.
converge d1-typed-address 60 a1 a2
grep -q '^\[[^]]*\] discovery: listening on UDP 21930$' "$LOG" || fail "d1: discovery did not start"
[ "$(count 'discovery: listening')" = 1 ] || fail "d1: discovery started more than once"
dstatus() { ctl STATUS > "$W/status.txt"; grep -q "$1" "$W/status.txt" || { cat "$W/status.txt"; fail "d1: STATUS lacks: $1"; }; }
dstatus '^device beta waiting sessions=0 retry=0 last=NO_ADDRESS address=- via=- addresses=0$'
dstatus '^folder multi-b1 idle device=beta '
announce "$OTHER" tcp://127.0.0.1:22153             # another device's announcement
announce "$B" quic://127.0.0.1:22153 'relay://127.0.0.1:22153/?id=x' 'tcp://[::1]:22153'   # nothing usable
printf 'not an announcement' | python3 -c 'import socket,sys; socket.socket(socket.AF_INET, socket.SOCK_DGRAM).sendto(sys.stdin.buffer.read(), ("127.0.0.1", int(sys.argv[1])))' "$DISCO_PORT"
sleep 3
dstatus '^device beta waiting sessions=0 retry=0 last=NO_ADDRESS address=- via=- addresses=0$'
[ "$(count '\[beta\] engine: no address for this device yet')" = 1 ] || fail "d1: the wait for an address is not logged exactly once"
# That line and its short form for the user, nothing else.
[ "$(count ' ms\] \[beta\] ')" = 1 ] && [ "$(count '\[beta\]')" = 2 ] \
    || fail "d1: lines about the device without an address: $(grep -c '\[beta\]' "$LOG"), expected 2"
[ "$(count '\[beta/')" = 0 ] || fail "d1: a folder of the device without an address was touched"
[ ! -e "$(ldir b1)/peer-b1.txt" ] || fail "d1: synchronised without an address"
grep -q 'discovery: the device announces' "$LOG" && fail "d1: an address was taken from a packet that has none for us"
# The host is unspecified: the address is the one the packet came from.
t0=$(date +%s)
announce "$B" quic://0.0.0.0:22153 tcp://0.0.0.0:22153 tcp://127.0.0.1:22154
for _ in $(seq 1 20); do grep -q '\[beta\] tcp: connected' "$LOG" && break; sleep 0.2; done
grep -q '^\[[^]]*\] \[beta\] discovery: the device announces 127.0.0.1:22153$' "$LOG" || fail "d1: the address was not learned"
grep -q '\[beta\] tcp: connected' "$LOG" || fail "d1: no connection within 4 s of the announcement"
converge d1-found 60 b1 b2
echo "  d1: found $(( $(date +%s) - t0 )) s after the announcement"
announce "$B" tcp://0.0.0.0:22153                   # the same again: nothing to say
announce "$B" tcp://127.0.0.1:22154                 # another address while connected
for _ in $(seq 1 20); do grep -q 'discovery: the device announces 127.0.0.1:22154 now' "$LOG" && break; sleep 0.2; done
grep -q '\[beta\] discovery: the device announces 127.0.0.1:22154 now (was 127.0.0.1:22153); used from the next attempt on$' "$LOG" \
    || fail "d1: the changed address was not noted"
sleep 2
dstatus '^device beta connected sessions=1 retry=0 last=- address=127.0.0.1:22153 via=direct addresses=1$'
[ "$(count '\[beta\] engine: session [0-9]* ended')" = 0 ] || fail "d1: the connection was dropped for a new address"
announce "$B" tcp://127.0.0.1:22153                 # and back, for the reconnections below
for _ in $(seq 1 20); do grep -q 'discovery: the device announces 127.0.0.1:22153 now' "$LOG" && break; sleep 0.2; done
[ "$(count '\[beta\] discovery: the device announces')" = 3 ] || fail "d1: expected three address lines, have $(count '\[beta\] discovery: the device announces')"
echo "  d1: no attempt without an address; found by its announcement; a new address does not end the connection"

# m1: three folders on two devices come into sync; the unusable ones are named.
converge m1-first 60 a1 a2 b1 b2
for _ in $(seq 1 20); do grep -q '\[alpha/multi-none\].*does not share' "$LOG" && break; sleep 0.5; done
grep -q '\[alpha/multi-a3\] engine: folder "multi-a3" is left out of this attempt: BAD_DESTINATION' "$LOG" \
    || fail "m1: the folder without a directory was not left out"
grep -q '\[alpha/multi-none\] cc: the peer does not share folder "multi-none"' "$LOG" \
    || fail "m1: the folder the peer does not share was not reported"
grep -q '\[closed\] engine: session 1 ended: CONNECT_FAILED' "$LOG" || fail "m1: the closed device was not tried"
grep -q '\[silent\] tcp: connected' "$LOG" || fail "m1: the silent device was not tried"
echo "  m1: missing directory, folder not shared, closed and silent devices reported; the rest in sync"

# m2: changes in every folder, both ways, while two devices never connect.
t0=$(date +%s)
for f in a1 a2 b1; do
    printf 'second file here, %s\n' $f > "$(ldir $f)/here2-$f.txt"
    head -c 300000 /dev/urandom > "$(pdir $f)/peer-big-$f.bin"
    rm "$(pdir $f)/peer-$f.txt"
done
for f in a1 a2; do ST_PEER_FOLDER_ID=multi-$f ST_PEER_FOLDER_DIR="$(pdir $f)" pa scan > /dev/null; done
ST_PEER_FOLDER_ID=multi-b1 ST_PEER_FOLDER_DIR="$(pdir b1)" pb scan > /dev/null
converge m2-changes 30 a1 a2 b1
took=$(( $(date +%s) - t0 ))
[ "$took" -le 20 ] || fail "m2: changes took $took s: held up by the devices that do not answer?"
[ ! -e "$(ldir a1)/peer-a1.txt" ] || fail "m2: the peer's deletion did not arrive"
echo "  m2: changes both ways in three folders within $took s"

# c1: the control interface: status of every device and folder, a scan
# on request, and what is refused.
ctl VERSION | grep -q '^aros-syncthing .* control 1$' || fail "c1: VERSION: $(ctl VERSION)"
for _ in $(seq 1 40); do ctl STATUS > "$W/status.txt"; [ "$(grep -c '^folder multi-[ab][12] in-sync' "$W/status.txt")" = 4 ] && break; sleep 0.5; done
st() { grep -q "$1" "$W/status.txt" || { cat "$W/status.txt"; fail "c1: STATUS lacks: $1"; }; }
st '^device alpha connected sessions=1 retry=0 last=- address=127.0.0.1:22152 via=direct addresses=1$'
st '^device beta connected sessions=1 '
st '^device beta connected sessions=1 retry=0 last=- address=127.0.0.1:22153 via=direct addresses=1$'
st '^device closed waiting sessions=[0-9]* retry=[0-9]* last=CONNECT_FAILED address=127.0.0.1:22154 via=- addresses=1$'
st '^device silent connecting '
st '^folder multi-a1 in-sync device=alpha why=- received=2 deleted=1 conflicts=0 held=0 failed=0 in=[0-9]* out=[0-9]*$'
st '^folder multi-b2 in-sync device=beta '
st '^folder multi-a3 unusable device=alpha why=BAD_DESTINATION '
st '^folder multi-none not-shared device=alpha '
st '^folder multi-dead idle device=closed '
st '^network listen=- relay=- announced=- unknown=-$'
[ "$(wc -l < "$W/status.txt" | tr -d ' ')" = 13 ] || fail "c1: STATUS has $(wc -l < "$W/status.txt") lines, expected 13"
printf 'needs a scan on request\n' > "$(ldir b2)/asked.txt"
sleep 4
[ ! -e "$(pdir b2)/asked.txt" ] || fail "c1: the file arrived without a scan (rescan is 3600 s)"
[ "$(ctl scan multi-b2)" = OK ] || fail "c1: SCAN refused"
converge c1-scan-on-request 15 b2
rc=0; ctl SCAN nosuch > /dev/null || rc=$?
[ "$rc" = 5 ] || fail "c1: SCAN of an unknown folder returned $rc"
rc=0; ctl FROBNICATE > /dev/null || rc=$?
[ "$rc" = 10 ] || fail "c1: an unknown command returned $rc"
rc=0
"$BIN" "$W/twice.log" engine "$W/id" "$W/engine.conf" > /dev/null 2>&1 || rc=$?
[ "$rc" = 10 ] && grep -q 'RESULT FAIL ALREADY_RUNNING' "$W/twice.log" || fail "c1: a second engine was not refused (rc $rc)"
kill -0 "$EPID" 2> /dev/null || fail "c1: the engine exited"
echo "  c1: status, scan on request, refusals, second engine refused"

# m3: the silent device's attempt ends by its time limit and is repeated,
# and none of that ended the other sessions.
for _ in $(seq 1 80); do grep -q '\[silent\] engine: session 1 ended: TIMEOUT' "$LOG" && break; sleep 0.5; done
grep -q '\[silent\] engine: session 1 ended: TIMEOUT' "$LOG" || fail "m3: the silent attempt did not end by its limit"
[ "$(count '\[alpha\] engine: session [0-9]* ended')" = 0 ] || fail "m3: the session with alpha ended"
[ "$(count '\[beta\] engine: session [0-9]* ended')" = 0 ] || fail "m3: the session with beta ended"
echo "  m3: the unanswered attempt ended by its limit; other sessions untouched"

# m4: the missing directory appears: the folder joins.
mkdir -p "$(ldir a3)"
printf 'made here, folder a3\n' > "$(ldir a3)/here-a3.txt"
converge m4-folder-joins 100 a3 a1 a2
grep -q 'engine: folder "multi-a3" can be used again' "$LOG" || fail "m4: no notice that the folder can be used again"
[ "$(count '\[beta\] engine: session [0-9]* ended')" = 0 ] || fail "m4: the session with beta ended"
echo "  m4: the folder joined once its directory existed"

# m5: one device is restarted with changes on both sides; the other's session stays.
pa stop > /dev/null
printf 'while alpha was away\n' > "$(ldir a1)/away.txt"
printf 'on alpha before it came back\n' > "$(pdir a2)/back.txt"
printf 'for beta meanwhile\n' > "$(ldir b1)/meanwhile.txt"
converge m5-other-device 20 b1
pa start > /dev/null
converge m5-restart 90 a1 a2 a3
[ "$(count '\[beta\] engine: session [0-9]* ended')" = 0 ] || fail "m5: the session with beta ended"
[ -z "$(find "$W" -name '*.sync-conflict-*')" ] || fail "m5: conflict copies exist"
echo "  m5: one device restarted, the other's session stayed"

# p1: the folder object outlives the connection. With alpha away, a file
# made here is found and recorded by the periodic reading; after the
# reconnection it goes out although the folder was not opened or read
# anew for it, and the folder's counters go on from where they were.
fstat() { ctl STATUS | sed -n "s/^folder $1 [^ ]* .* $2=\([0-9]*\).*/\1/p"; }
rec0=$(fstat multi-a1 received)
del0=$(fstat multi-a1 deleted)
[ "${rec0:-0}" -ge 2 ] && [ "${del0:-0}" -ge 1 ] || fail "p1: counters before the restart: received=$rec0 deleted=$del0"
ended0=$(count '\[alpha\] engine: session [0-9]* ended')
pa stop > /dev/null
for _ in $(seq 1 40); do [ "$(count '\[alpha\] engine: session [0-9]* ended')" -gt "$ended0" ] && break; sleep 0.5; done
printf 'made while there was no connection\n' > "$(ldir a1)/offline.txt"
for _ in $(seq 1 40); do grep -q '\[alpha/multi-a1\] scan: "offline.txt" new here' "$LOG" && break; sleep 0.5; done
grep -q '\[alpha/multi-a1\] scan: "offline.txt" new here' "$LOG" || fail "p1: the file was not recorded while disconnected"
ctl STATUS | grep -q '^device alpha waiting ' || fail "p1: alpha was connected when the file was recorded"
[ "$(fstat multi-a1 received)" = "$rec0" ] || fail "p1: counters changed or were lost while disconnected: $(fstat multi-a1 received)"
pa start > /dev/null
converge p1-offline-change 90 a1 a2 a3
[ "$(count '\[alpha/multi-a1\] sync: two-way, state file')" = 1 ] || fail "p1: the folder was opened again ($(count '\[alpha/multi-a1\] sync: two-way, state file') times)"
[ "$(count '\[alpha/multi-a1\] scan: "offline.txt" new here')" = 1 ] || fail "p1: the file was recorded twice"
for _ in $(seq 1 40); do ctl STATUS | grep -q '^folder multi-a1 in-sync' && break; sleep 0.5; done
rec1=$(fstat multi-a1 received)
[ "${rec1:-0}" -ge "$rec0" ] && [ "$(fstat multi-a1 deleted)" = "$del0" ] || fail "p1: counters after the reconnection: received=$rec1 (was $rec0) deleted=$(fstat multi-a1 deleted) (was $del0)"
echo "  p1: recorded without a connection, sent after it; folder opened once; counters kept (received=$rec1)"

# p2: PAUSE and RESUME. The paused folder leaves the session, the other
# folder of the device goes on; changes on both sides wait and arrive
# after RESUME.
[ "$(ctl PAUSE multi-b1)" = OK ] || fail "p2: PAUSE refused"
for _ in $(seq 1 40); do grep -q '\[beta\] engine: session [0-9]* ended: FOLDER_PAUSED_HERE' "$LOG" && break; sleep 0.5; done
grep -q '\[beta\] engine: session [0-9]* ended: FOLDER_PAUSED_HERE' "$LOG" || fail "p2: the session did not end for the paused folder"
for _ in $(seq 1 40); do ctl STATUS > "$W/status.txt"; grep -q '^folder multi-b2 in-sync' "$W/status.txt" && break; sleep 0.5; done
grep -q '^folder multi-b1 paused-here device=beta ' "$W/status.txt" || { cat "$W/status.txt"; fail "p2: STATUS does not show the pause"; }
grep -q '^folder multi-b2 in-sync' "$W/status.txt" || { cat "$W/status.txt"; fail "p2: the other folder did not come back"; }
[ "$(ctl pause multi-b1)" = "OK already paused" ] || fail "p2: a repeated PAUSE answered: $(ctl pause multi-b1)"
rc=0; ctl PAUSE nosuch > /dev/null || rc=$?
[ "$rc" = 5 ] || fail "p2: PAUSE of an unknown folder returned $rc"
rc=0; ctl RESUME nosuch > /dev/null || rc=$?
[ "$rc" = 5 ] || fail "p2: RESUME of an unknown folder returned $rc"
rc=0; ctl PAUSE > /dev/null || rc=$?
[ "$rc" = 10 ] || fail "p2: PAUSE without a folder returned $rc"
printf 'made here while paused\n' > "$(ldir b1)/paused-here.txt"
printf 'made on the peer while paused\n' > "$(pdir b1)/paused-peer.txt"
ST_PEER_FOLDER_ID=multi-b1 ST_PEER_FOLDER_DIR="$(pdir b1)" pb scan > /dev/null
printf 'the other folder meanwhile\n' > "$(ldir b2)/during-pause.txt"
[ "$(ctl SCAN multi-b2)" = OK ] || fail "p2: SCAN refused"
converge p2-other-folder 20 b2
sleep 5
[ ! -e "$(pdir b1)/paused-here.txt" ] && [ ! -e "$(ldir b1)/paused-peer.txt" ] || fail "p2: the paused folder was synchronised"
[ "$(ctl RESUME multi-b1)" = OK ] || fail "p2: RESUME refused"
converge p2-resume 40 b1 b2
[ "$(ctl resume multi-b1)" = "OK not paused" ] || fail "p2: a repeated RESUME answered: $(ctl resume multi-b1)"
ctl STATUS | grep -q '^folder multi-b1 paused-here' && fail "p2: still shown as paused"
[ -z "$(find "$W" -name '*.sync-conflict-*')" ] || fail "p2: conflict copies exist"
kill -0 "$EPID" 2> /dev/null || fail "p2: the engine exited"
echo "  p2: paused folder left out, the other went on; both changes arrived after RESUME"

# m6: stop, asked for through the control interface.
[ "$(ctl STOP)" = OK ] || fail "m6: STOP refused"
for _ in $(seq 1 75); do kill -0 "$EPID" 2> /dev/null || break; sleep 0.2; done
kill -0 "$EPID" 2> /dev/null && fail "m6: still running 15 s after SIGINT"
rc=0
wait "$EPID" || rc=$?
EPID=
[ "$rc" = 0 ] || fail "m6: exit code $rc"
[ "$(tail -1 "$LOG" | sed 's/^\[[^]]*\] //')" = "RESULT PASS STOPPED" ] || fail "m6: last line $(tail -1 "$LOG")"
grep -q "ERROR: AddressSanitizer\|runtime error" "$W/console.txt" && fail "m6: sanitizer report"
# One per folder that could be read, connected or not (all eight).
[ "$(ls "$W/id" | grep -c '^state-')" = 8 ] || fail "m6: expected eight state files, have: $(ls "$W/id")"
echo "  m6: stopped cleanly, a state file for every folder"

# m7: a second start announces nothing again and a bad configuration is refused.
LOG="$W/second.log"
"$BIN" "$LOG" engine "$W/id" "$W/engine.conf" > "$W/console.txt" 2>&1 &
EPID=$!
# d2: this time the device is announced at an address where nothing
# listens: the attempt goes there and fails; the address announced next
# is tried at once, not after the pause.
for _ in $(seq 1 40); do grep -q "discovery: listening on UDP 21930" "$LOG" 2> /dev/null && break; sleep 0.25; done
announce "$B" tcp://127.0.0.1:22154
for _ in $(seq 1 40); do grep -q '\[beta\] engine: session 1 ended: CONNECT_FAILED' "$LOG" && break; sleep 0.25; done
grep -q '\[beta\] discovery: the device announces 127.0.0.1:22154$' "$LOG" || fail "d2: the first address was not learned"
grep -q '\[beta\] engine: session 1 ended: CONNECT_FAILED' "$LOG" || fail "d2: the announced address was not tried"
# Two more failures, so that the pause before the next attempt is 4 s or more.
for _ in $(seq 1 40); do grep -q '\[beta\] engine: session 3 ended: CONNECT_FAILED; next attempt in [0-9]* s' "$LOG" && break; sleep 0.25; done
grep -q '\[beta\] engine: session 3 ended: CONNECT_FAILED; next attempt in [0-9]* s' "$LOG" || fail "d2: no third attempt"
ctl STATUS > "$W/status.txt"
grep -q '^device beta waiting sessions=3 retry=[1-9][0-9]* last=CONNECT_FAILED address=127.0.0.1:22154 via=- addresses=1$' "$W/status.txt" \
    || { cat "$W/status.txt"; fail "d2: STATUS does not show the failed address"; }
announce "$B" tcp://:22153
for _ in $(seq 1 10); do grep -q '\[beta\] tcp: connected' "$LOG" && break; sleep 0.2; done
grep -q '\[beta\] discovery: the device announces 127.0.0.1:22153 now (was 127.0.0.1:22154)$' "$LOG" || fail "d2: the new address was not learned"
grep -q '\[beta\] tcp: connected' "$LOG" || fail "d2: the new address was not tried within 2 s (the pause was not cut short)"
echo "  d2: the announced address is tried; a new one is tried at once"
for _ in $(seq 1 60); do [ "$(count 'engine: in sync with the peer')" -ge 5 ] && break; sleep 0.5; done
[ "$(count 'engine: in sync with the peer')" -ge 5 ] || fail "m7: five folders were not in sync after the second start"
kill -INT "$EPID"
wait "$EPID" || fail "m7: exit code"
EPID=
[ "$(grep -c 'tw: summary' "$LOG")" = 6 ] || fail "m7: expected six folder summaries (five in use, one not shared)"
grep 'tw: summary' "$LOG" | grep -qv ' 0 announced' && fail "m7: entries announced again after a restart"
printf 'folder x\n  path /tmp\n  with nobody\n' > "$W/bad.conf"
rc=0
"$BIN" "$W/bad.log" engine "$W/id" "$W/bad.conf" > /dev/null 2>&1 || rc=$?
[ "$rc" = 10 ] && grep -q 'RESULT FAIL BAD_CONFIGURATION' "$W/bad.log" || fail "m7: a bad configuration was not refused (rc $rc)"
echo "  m7: nothing announced again; a bad configuration is refused"

echo "PASS: engine multi-folder host test"
