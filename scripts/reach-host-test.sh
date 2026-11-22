#!/bin/bash
# Being reached and reaching beyond the local network (src/reach.h), on
# this host, with unmodified Syncthing devices and Syncthing's own relay
# and discovery servers (built from the research checkout,
# docs/research/README.md, into var/infra):
#
#   r1  a device connects to us (it has our address, we have none of
#       its): files go both ways; STATUS says via=incoming
#   r2  a device that is not in the configuration connects: turned away,
#       named in STATUS (unknown=)
#   r3  a device reachable only through a relay: found through global
#       discovery (its relay address), reached through the relay, files
#       go both ways; STATUS via=relay
#   r4  we are reachable only through a relay (no listener; the relay
#       and the announcement of it): a device that finds us through
#       global discovery comes in through the relay
#
#   scripts/reach-host-test.sh     (builds build/host/stpoc-asan)
#
# Own ports only: devices 22176-22178 (GUI 8411-8413), relay 22173,
# discovery 22174, our listener 22175. Nothing public is contacted. The
# relay listens on this machine's own network address, not loopback: a
# discovery server drops loopback addresses from announcements, as the
# public one does (it is reachable from the local network for the few
# minutes of the test, serves no pool and has no status page).
# Everything under build/reach-test, removed at the end (KEEP=1 keeps it).
set -e
. "$(dirname "$0")/env.sh"
st_require HOST_OPENSSL
SYNCTHING_BIN=${TW_SYNCTHING_BIN:-$ST_ROOT/deps/downloads/syncthing-v2.1.7/syncthing-macos-arm64-v2.1.7/syncthing}
[ -x "$SYNCTHING_BIN" ] || { echo "no Syncthing binary at $SYNCTHING_BIN (set TW_SYNCTHING_BIN)" >&2; exit 2; }
export SYNCTHING_BIN
INFRA=$ST_ROOT/var/infra
[ -x "$INFRA/strelaysrv" ] && [ -x "$INFRA/stdiscosrv" ] || {
    echo "build the servers first: (cd references/syncthing && go build -o ../../aros-syncthing/var/infra/strelaysrv ./cmd/strelaysrv && go build -o ../../aros-syncthing/var/infra/stdiscosrv ./cmd/stdiscosrv)" >&2
    exit 2
}
W="$ST_ROOT/build/reach-test"
PEERCTL="$ST_ROOT/scripts/st-peer.sh"
BIN="$ST_ROOT/build/host/stpoc-asan"
LOG="$W/engine.log"
EPID= RPID= DPID=
pa() { ST_PEER_HOME="$W/peerA" ST_PEER_PORT=22176 ST_PEER_GUI_PORT=8411 ST_PEER_FOLDER_TYPE=sendreceive ST_PEER_FOLDER_ID=reach-a ST_PEER_FOLDER_LABEL=reach-a ST_PEER_FOLDER_DIR="$W/peer-a" "$PEERCTL" "$@"; }
pb() { ST_PEER_HOME="$W/peerB" ST_PEER_PORT=22177 ST_PEER_GUI_PORT=8412 ST_PEER_FOLDER_TYPE=sendreceive ST_PEER_FOLDER_ID=reach-b ST_PEER_FOLDER_LABEL=reach-b ST_PEER_FOLDER_DIR="$W/peer-b" "$PEERCTL" "$@"; }
pc() { ST_PEER_HOME="$W/peerC" ST_PEER_PORT=22178 ST_PEER_GUI_PORT=8413 ST_PEER_FOLDER_TYPE=sendreceive ST_PEER_FOLDER_ID=reach-c ST_PEER_FOLDER_LABEL=reach-c ST_PEER_FOLDER_DIR="$W/peer-c" "$PEERCTL" "$@"; }

stop_engine() {
    [ -n "$EPID" ] || return 0
    "$BIN" ctl STOP > /dev/null 2>&1 || kill "$EPID" 2> /dev/null || true
    for _ in $(seq 1 50); do kill -0 "$EPID" 2> /dev/null || break; sleep 0.2; done
    kill -9 "$EPID" 2> /dev/null || true
    wait "$EPID" 2> /dev/null || true
    EPID=
}
cleanup() {
    stop_engine
    [ -n "$RPID" ] && kill "$RPID" 2> /dev/null || true
    [ -n "$DPID" ] && kill "$DPID" 2> /dev/null || true
    pa stop > /dev/null 2>&1 || true
    pb stop > /dev/null 2>&1 || true
    pc stop > /dev/null 2>&1 || true
    if [ "${KEEP:-0}" != 1 ]; then rm -rf "$W"; fi
}
trap cleanup EXIT
fail() { echo "FAIL: $*"; [ -f "$LOG" ] && tail -25 "$LOG"; exit 1; }

SRCS=$(sed -n '/^stpoc)/,/;;/p' "$ST_ROOT/scripts/build.sh" | grep -o '\$ST_ROOT/src/[a-z0-9]*\.c' | sed "s#\$ST_ROOT#$ST_ROOT#")
cc -std=c99 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    -D_POSIX_C_SOURCE=200809L -DNET_TEST_HOOKS -I"$HOST_OPENSSL/include" $SRCS -L"$HOST_OPENSSL/lib" -lssl -lcrypto -o "$BIN"

pa stop > /dev/null 2>&1 || true
pb stop > /dev/null 2>&1 || true
pc stop > /dev/null 2>&1 || true
rm -rf "$W"
mkdir -p "$W/id" "$W/relay" "$W/disco"
for f in a b c; do
    mkdir -p "$W/peer-$f" "$W/local-$f"
    printf 'from the peer, folder %s\n' $f > "$W/peer-$f/peer-$f.txt"
    printf 'made here, folder %s\n' $f > "$W/local-$f/here-$f.txt"
done
"$BIN" "$W/id.log" identity-create "$W/id" > /dev/null || fail "identity"
ME=$(sed -n 's/.*identity: Device ID \([A-Z0-9-]*\).*/\1/p' "$W/id.log" | head -1)
[ -n "$ME" ] || fail "identity"

# The relay (no pool, no status page) and the discovery server (its own
# certificate; the devices are told not to check it).
H=$(ipconfig getifaddr en0 2> /dev/null || ipconfig getifaddr en1 2> /dev/null || true)
[ -n "$H" ] || fail "no network address on en0 or en1 for the relay"
"$INFRA/strelaysrv" -keys "$W/relay" -listen "$H:22173" -pools "" -status-srv "" -ping-interval 10s \
    > "$W/relay.log" 2>&1 &
RPID=$!
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -subj /CN=disco -days 2 \
    -keyout "$W/disco/key.pem" -out "$W/disco/cert.pem" > /dev/null 2>&1
"$INFRA/stdiscosrv" --cert "$W/disco/cert.pem" --key "$W/disco/key.pem" --listen 127.0.0.1:22174 \
    --db-dir "$W/disco" > "$W/disco.log" 2>&1 &
DPID=$!
RURL=
for _ in $(seq 1 50); do
    RURL=$(sed -n 's/.*URI: \(relay:\/\/[^ ]*\).*/\1/p' "$W/relay.log" | head -1)
    [ -n "$RURL" ] && break
    sleep 0.2
done
[ -n "$RURL" ] || { cat "$W/relay.log"; fail "the relay did not start"; }
RID=$(echo "$RURL" | sed -n 's/.*id=\([A-Z0-9-]*\).*/\1/p')
RELAY="relay://$H:22173/?id=$RID"
DISCO_FOR_PEERS="https://127.0.0.1:22174/?insecure"
echo "reach host test, peer $("$SYNCTHING_BIN" --version | cut -d' ' -f1-2), relay $RID"

for p in pa pb pc; do $p init > /dev/null; $p start > /dev/null; done
pa share "$ME" aros-reach > /dev/null
pb share "$ME" aros-reach > /dev/null
pc share "$ME" aros-reach > /dev/null
A=$(pa id) B=$(pb id) C=$(pc id)

# A: knows our address and dials it.
pa api PATCH "/rest/config/devices/$ME" '{"addresses":["tcp://127.0.0.1:22175"]}' > /dev/null
# B: reachable only through the relay, announced to the discovery
# server; it never dials us (an address where nothing listens).
pb api PATCH /rest/config/options "{\"listenAddresses\":[\"$RELAY\"],\"relaysEnabled\":true,\"globalAnnounceEnabled\":true,\"globalAnnounceServers\":[\"$DISCO_FOR_PEERS\"]}" > /dev/null
pb api PATCH "/rest/config/devices/$ME" '{"addresses":["tcp://127.0.0.1:1"]}' > /dev/null
# C: not in our first configuration; it dials our listener (r2). In r4
# it finds us through discovery and comes in through the relay.
pc api PATCH "/rest/config/devices/$ME" '{"addresses":["tcp://127.0.0.1:22175"]}' > /dev/null

list() { (cd "$1" && find . -type f ! -path './.stfolder*' ! -name '.syncthing.*' ! -name '.stpoc-*' | LC_ALL=C sort | xargs shasum -a 256 2> /dev/null); }
same() { [ "$(list "$W/local-$1")" = "$(list "$W/peer-$1")" ]; }
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
    for f in "$@"; do same $f || diff <(list "$W/local-$f") <(list "$W/peer-$f") | head -6; done
    fail "$name: folders differ after $secs s"
}
export STPOC_CONTROL="$W/control"
export STPOC_DISCOVERY_PORT=21932
ctl() { "$BIN" ctl "$@"; }
status_has() { # SECONDS PATTERN
    for _ in $(seq 1 $(($1 * 2))); do
        ctl STATUS > "$W/status.txt" 2> /dev/null && grep -q "$2" "$W/status.txt" && return 0
        sleep 0.5
    done
    cat "$W/status.txt"
    fail "STATUS lacks $2"
}

# ---- run 1: a listener, no relay, no global discovery ----
cat > "$W/engine.conf" <<CONF
name aros-reach
listen 22175
global-discovery off
relays off
local-announce off

device alpha
  id $A
  address dynamic
device beta
  id $B
  address dynamic

folder reach-a
  path $W/local-a
  with alpha
  rescan 2
folder reach-b
  path $W/local-b
  with beta
  rescan 2
CONF
"$BIN" --log-max 8000000 "$LOG" engine "$W/id" "$W/engine.conf" > "$W/console.txt" 2>&1 &
EPID=$!

converge r1-first 60 a
status_has 10 '^device alpha connected sessions=1 retry=0 last=- address=127.0.0.1 via=incoming addresses=0$'
status_has 5 '^network listen=22175 relay=- announced=- unknown='
printf 'from the peer, later\n' > "$W/peer-a/later.txt"
pa scan > /dev/null
printf 'made here, later\n' > "$W/local-a/later-here.txt"
converge r1-changes 30 a
grep -q 'tls: handshake done in [0-9]* ms (we are the server): TLSv1.3' "$LOG" || fail "r1: no server-side handshake in the log"
echo "  r1: the device connected to us; files both ways"

status_has 60 "unknown=$C@127.0.0.1\$"
grep -q "listen: device $C at 127.0.0.1 is not in the configuration; connection closed" "$LOG" || fail "r2: no log line"
[ -z "$(ls -A "$W/local-c")" ] || [ "$(ls "$W/local-c")" = here-c.txt ] || fail "r2: something arrived from the unknown device"
echo "  r2: a device not in the configuration was turned away and named in STATUS"
status_has 5 '^device beta waiting sessions=0 retry=0 last=NO_ADDRESS address=- via=- addresses=0$'
stop_engine
grep -q 'RESULT PASS STOPPED' "$LOG" || fail "run 1 did not end with a clean stop"

# ---- run 2: no listener; the relay; global discovery ----
LOG="$W/engine2.log"
cat > "$W/engine.conf" <<CONF
name aros-reach
listen off
global-discovery on
relays on
local-announce off
discovery-server https://127.0.0.1:22174/
relay $RELAY

device beta
  id $B
  address dynamic
device gamma
  id $C
  address dynamic

folder reach-b
  path $W/local-b
  with beta
  rescan 2
folder reach-c
  path $W/local-c
  with gamma
  rescan 2
CONF
"$BIN" --log-max 8000000 "$LOG" engine "$W/id" "$W/engine.conf" > "$W/console2.txt" 2>&1 &
EPID=$!

status_has 30 "^network listen=- relay=relay://$H:22173/?id=$RID announced=yes unknown=-\$"
echo "  r4: joined the relay and announced it"
# C looks us up, now that we are announced (an earlier "not found" is
# kept by Syncthing for as long as the server says). Syncthing looks up
# only with announcing on; its own announcement, a loopback address, is
# dropped by the server, so we never learn where C is.
pc api PATCH /rest/config/options "{\"relaysEnabled\":true,\"globalAnnounceEnabled\":true,\"globalAnnounceServers\":[\"$DISCO_FOR_PEERS\"],\"listenAddresses\":[\"tcp://127.0.0.1:22178\"]}" > /dev/null
pc api PATCH "/rest/config/devices/$ME" '{"addresses":["dynamic"]}' > /dev/null
converge r3-first 120 b
status_has 10 "^device beta connected sessions=[0-9]* retry=0 last=[-A-Z_]* address=$H:22173 via=relay addresses=1\$"
grep -q 'relay: session joined; TLS with the device through the relay' "$LOG" || fail "r3: no relay session in the log"
printf 'from the peer, through the relay\n' > "$W/peer-b/relayed.txt"
pb scan > /dev/null
dd if=/dev/urandom of="$W/local-b/big-here.bin" bs=65536 count=40 2> /dev/null
converge r3-changes 60 b
echo "  r3: found through global discovery, reached through the relay; files both ways (2.5 MB here)"

converge r4-first 120 c
status_has 10 "^device gamma connected sessions=1 retry=0 last=- address=$H via=relay addresses=0\$"
grep -q "relay: device $C asks for a session" "$LOG" || fail "r4: no invitation in the log"
printf 'from the peer, in through the relay\n' > "$W/peer-c/in.txt"
pc scan > /dev/null
converge r4-changes 30 c
echo "  r4: the device came in through the relay; files both ways"

stop_engine
grep -q 'RESULT PASS STOPPED' "$LOG" || fail "run 2 did not end with a clean stop"
echo "PASS"
