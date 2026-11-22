#!/bin/bash
# Relays, global discovery and the listener on one AROS target (src/reach.h).
#
#   scripts/aros-reach.sh host TAG WORK OUT-DIR RESULTS-DIR
#       Starts, under WORK (an empty or new directory): Syncthing's relay
#       (on this machine's network address, port 22173; no pool) and
#       discovery server (127.0.0.1:22174, which the guest reaches as
#       10.0.2.2), and three unmodified Syncthing devices:
#         B  reachable only through that relay, announced to that server;
#            the guest finds it there and connects through the relay
#         C  looks the guest up on that server once the guest is
#            announced there, and comes in through the relay
#         D  on Syncthing's public relays and global discovery, as a
#            Syncthing installed with its defaults; the guest's second
#            engine run has the defaults too, and reaches D through them
#       Writes the guest's stage to OUT-DIR (stpoc, id/, loop/, the two
#       configurations, reach.scr) and keeps running until WORK/stop
#       exists or 40 minutes have passed, then stops everything it
#       started. In the guest:  Execute RAM:stp/reach.scr   (about 6 min)
#   scripts/aros-reach.sh verify TAG WORK COLLECTED-DIR
#       Checks the guest's answers and that every folder is the same on
#       both sides (SHA-256 manifests written in the guest).
#
# The guest also connects to its own listener (127.0.0.1:22000) with an
# identity that is not configured, which is turned away by name.
set -e
. "$(dirname "$0")/env.sh"
st_require HOST_OPENSSL
SYNCTHING_BIN=${TW_SYNCTHING_BIN:-$ST_ROOT/deps/downloads/syncthing-v2.1.7/syncthing-macos-arm64-v2.1.7/syncthing}
export SYNCTHING_BIN
INFRA=$ST_ROOT/var/infra
P="$ST_ROOT/scripts/st-peer.sh"
HOSTBIN="$ST_ROOT/build/host/stpoc"
cmd=${1:?host or verify}
tag=${2:?tag}
W=${3:?work dir}
export ST_PEER_FOLDER_TYPE=sendreceive
peer() { # NAME PORT GUIPORT ARGS...
    local n=$1 port=$2 gui=$3
    shift 3
    ST_PEER_HOME="$W/peer$n" ST_PEER_PORT=$port ST_PEER_GUI_PORT=$gui ST_PEER_FOLDER_ID=r$(echo $n | tr BCD bcd)-$tag \
        ST_PEER_FOLDER_LABEL=r$(echo $n | tr BCD bcd)-$tag ST_PEER_FOLDER_DIR="$W/folder-$n" "$P" "$@"
}
pb() { peer B 22177 8412 "$@"; }
pc() { peer C 22178 8413 "$@"; }
pd() { peer D 22179 8414 "$@"; }
devid() { "$HOSTBIN" /dev/null identity-show "$1" | grep -oE '[A-Z0-9]{7}(-[A-Z0-9]{7}){7}' | head -1; }

case "$cmd" in
host)
    out=${4:?out dir}
    R=${5:?guest results dir}
    [ -x "$INFRA/strelaysrv" ] && [ -x "$INFRA/stdiscosrv" ] || { echo "no relay or discovery server in $INFRA (see reach-host-test.sh)" >&2; exit 2; }
    [ ! -e "$out" ] || { echo "$out exists" >&2; exit 2; }
    [ ! -e "$W/host.env" ] || { echo "$W was used already" >&2; exit 2; }
    H=$(ipconfig getifaddr en0 2> /dev/null || ipconfig getifaddr en1 2> /dev/null || true)
    [ -n "$H" ] || { echo "no network address on en0 or en1 for the relay" >&2; exit 2; }
    mkdir -p "$W/relay" "$W/disco" "$out/id" "$out/loop"
    PIDS=()
    stop_all() {
        for p in pb pc pd; do $p stop > /dev/null 2>&1 || true; done
        for pid in "${PIDS[@]}"; do kill "$pid" 2> /dev/null || true; done
    }
    trap stop_all EXIT
    "$INFRA/strelaysrv" -keys "$W/relay" -listen "$H:22173" -pools "" -status-srv "" > "$W/relay.log" 2>&1 &
    PIDS+=($!)
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -subj /CN=disco -days 2 \
        -keyout "$W/disco/key.pem" -out "$W/disco/cert.pem" > /dev/null 2>&1
    "$INFRA/stdiscosrv" --cert "$W/disco/cert.pem" --key "$W/disco/key.pem" --listen 127.0.0.1:22174 \
        --db-dir "$W/disco" > "$W/disco.log" 2>&1 &
    PIDS+=($!)
    RID=
    for _ in $(seq 1 50); do
        RID=$(sed -n 's/.*URI: relay:\/\/[^?]*?id=\([A-Z0-9-]*\).*/\1/p' "$W/relay.log" | head -1)
        [ -n "$RID" ] && break
        sleep 0.2
    done
    [ -n "$RID" ] || { echo "the relay did not start" >&2; exit 1; }
    RELAY="relay://$H:22173/?id=$RID"
    LOCAL_DISCO="https://127.0.0.1:22174/?insecure"

    "$HOSTBIN" /dev/null identity-create "$out/id" > /dev/null
    "$HOSTBIN" /dev/null identity-create "$out/loop" > /dev/null
    ME=$(devid "$out/id")
    LOOP=$(devid "$out/loop")
    for n in B C D; do
        mkdir -p "$W/folder-$n/.stfolder"
        printf 'from the peer, folder %s\n' $n > "$W/folder-$n/peer-$n.txt"
    done
    for p in pb pc pd; do $p init > /dev/null; $p start > /dev/null; $p share "$ME" "aros-$tag" > /dev/null; done
    B=$(pb id) C=$(pc id) D=$(pd id)
    pb api PATCH /rest/config/options "{\"listenAddresses\":[\"$RELAY\"],\"relaysEnabled\":true,\"globalAnnounceEnabled\":true,\"globalAnnounceServers\":[\"$LOCAL_DISCO\"]}" > /dev/null
    pb api PATCH "/rest/config/devices/$ME" '{"addresses":["tcp://127.0.0.1:1"]}' > /dev/null
    pc api PATCH "/rest/config/devices/$ME" '{"addresses":["tcp://127.0.0.1:1"]}' > /dev/null
    pd api PATCH /rest/config/options '{"listenAddresses":["dynamic+https://relays.syncthing.net/endpoint"],"relaysEnabled":true,"globalAnnounceEnabled":true,"globalAnnounceServers":["default"]}' > /dev/null
    printf 'H=%s\nRID=%s\nME=%s\nLOOP=%s\nB=%s\nC=%s\nD=%s\n' "$H" "$RID" "$ME" "$LOOP" "$B" "$C" "$D" > "$W/host.env"

    # D must be findable before the guest looks for it.
    found=
    for _ in $(seq 1 40); do
        if curl -s -m 10 -A Synchr-test "https://discovery-lookup.syncthing.net/v2/?device=$D" | grep -q relay; then found=1; break; fi
        sleep 5
    done
    [ -n "$found" ] || echo "warning: D is not in public discovery yet" >&2

    cp "${ST_REACH_BIN:-$ST_BUILD/stpoc}" "$out/stpoc"
    cat > "$out/engine1.conf" <<CONF
# aros-reach.sh, $tag: this machine's relay and discovery server
name aros-$tag
listen 22000
discovery-server https://10.0.2.2:22174/
relay $RELAY

device beta
  id $B
  address dynamic
device gamma
  id $C
  address dynamic

folder rb-$tag
  path RAM:r-b
  with beta
  rescan 5
folder rc-$tag
  path RAM:r-c
  with gamma
  rescan 5
CONF
    cat > "$out/engine2.conf" <<CONF
# aros-reach.sh, $tag: Syncthing's defaults (public relays and discovery)
name aros-$tag

device delta
  id $D
  address dynamic

folder rd-$tag
  path RAM:r-d
  with delta
  rescan 5
CONF
    cat > "$out/reach.scr" <<EOF2
; Relays, global discovery and the listener.
FailAt 1000
Stack 262144
Version >$R/0-system.txt
MakeDir RAM:r-b RAM:r-c RAM:r-d
Echo "made on AROS, b" >RAM:r-b/here-b.txt
Echo "made on AROS, c" >RAM:r-c/here-c.txt
Echo "made on AROS, d" >RAM:r-d/here-d.txt
Run >NIL: RAM:stp/stpoc $R/engine1.log engine RAM:stp/id RAM:stp/engine1.conf
Wait 45
RAM:stp/stpoc ctl STATUS >$R/1-status.txt
RAM:stp/stpoc >NIL: $R/loop.log run RAM:stp/loop 127.0.0.1 22000 $ME rb-$tag 10
Wait 3
RAM:stp/stpoc ctl STATUS >$R/2-status.txt
Wait 100
RAM:stp/stpoc ctl STATUS >$R/3-status.txt
Echo "later on AROS, b" >RAM:r-b/later-b.txt
Echo "later on AROS, c" >RAM:r-c/later-c.txt
Wait 30
RAM:stp/stpoc ctl STATUS >$R/4-status.txt
RAM:stp/stpoc ctl STOP >$R/4-stop.txt
Wait 10
RAM:stp/stpoc manifest RAM:r-b $R/5-manifest-b.txt
RAM:stp/stpoc manifest RAM:r-c $R/5-manifest-c.txt
Run >NIL: RAM:stp/stpoc $R/engine2.log engine RAM:stp/id RAM:stp/engine2.conf
Wait 90
RAM:stp/stpoc ctl STATUS >$R/6-status.txt
Echo "later on AROS, d" >RAM:r-d/later-d.txt
Wait 30
RAM:stp/stpoc ctl STATUS >$R/7-status.txt
RAM:stp/stpoc ctl STOP >$R/7-stop.txt
Wait 10
RAM:stp/stpoc manifest RAM:r-d $R/8-manifest-d.txt
Echo "done" >$R/9-done.txt
EOF2
    (cd "$out" && shasum -a 256 stpoc engine1.conf engine2.conf reach.scr > versions.txt)
    echo "stage $out (relay $RELAY, guest $ME)"

    # C looks the guest up once the guest is announced (an earlier "not
    # found" would be kept by Syncthing for as long as the server says).
    start=$(date +%s)
    armed=
    while [ ! -e "$W/stop" ] && [ $(($(date +%s) - start)) -lt 2400 ]; do
        if [ -z "$armed" ] && curl -sk -m 5 "https://127.0.0.1:22174/?device=$ME" | grep -q relay; then
            pc api PATCH /rest/config/options "{\"relaysEnabled\":true,\"globalAnnounceEnabled\":true,\"globalAnnounceServers\":[\"$LOCAL_DISCO\"]}" > /dev/null
            pc api PATCH "/rest/config/devices/$ME" '{"addresses":["dynamic"]}' > /dev/null
            echo "$(date +%T) the guest is announced; C looks it up now" >> "$W/host.log"
            armed=1
        fi
        sleep 2
    done
    echo "$(date +%T) stopping" >> "$W/host.log"
    ;;
verify)
    got=${4:?collected dir}
    . "$W/host.env"
    rc=0
    bad() { echo "FAIL: $*"; rc=1; }
    has() { grep -q "$2" "$got/$1" 2> /dev/null || { bad "$1 lacks: $2"; [ -f "$got/$1" ] && sed 's/^/    /' "$got/$1"; }; }
    same() { # MANIFEST PEER-FOLDER
        local m="$got/$1" f="$W/$2"
        [ -f "$m" ] || { bad "no $1"; return; }
        local want have
        have=$(grep -E '^[0-9a-f]{64}  ' "$m" | sed 's#  \./#  #' | LC_ALL=C sort)
        want=$(cd "$f" && find . -type f ! -path './.stfolder*' ! -name '.syncthing.*' | sed 's#^\./##' | LC_ALL=C sort | while read -r n; do printf '%s  %s\n' "$(shasum -a 256 "$n" | cut -c1-64)" "$n"; done | LC_ALL=C sort)
        [ "$have" = "$want" ] || { bad "$1 differs from $2"; diff <(echo "$have") <(echo "$want") | head -8; }
        grep -q ' 0 unreadable$' "$m" || bad "$1: files not read"
    }
    [ -f "$got/9-done.txt" ] || bad "the guest script did not reach its end"
    has 1-status.txt "^network listen=22000 relay=relay://$H:22173/?id=$RID announced=yes "
    has 1-status.txt "^device beta connected .* via=relay "
    has 2-status.txt "unknown=$LOOP@127.0.0.1\$"
    has 3-status.txt "^device gamma connected .* via=relay "
    has 4-status.txt "^folder rb-$tag in-sync "
    has 4-status.txt "^folder rc-$tag in-sync "
    has 4-stop.txt '^OK'
    has engine1.log 'RESULT PASS STOPPED'
    has engine1.log 'relay: session joined; TLS with the device through the relay'
    has engine1.log "relay: device $C asks for a session"
    has engine1.log "tls: handshake done in [0-9]* ms (we are the server)"
    has engine1.log "listen: device $LOOP at 127.0.0.1 is not in the configuration"
    has 6-status.txt '^network listen=22000 relay=relay://.* announced=yes '
    has 6-status.txt '^device delta connected .* via=relay '
    has 7-status.txt "^folder rd-$tag in-sync "
    has engine2.log 'relay: [0-9]* relays in the public list'
    has engine2.log 'RESULT PASS STOPPED'
    same 5-manifest-b.txt folder-B
    same 5-manifest-c.txt folder-C
    same 8-manifest-d.txt folder-D
    [ $rc = 0 ] && echo "PASS: aros-reach $tag"
    exit $rc
    ;;
*)
    echo "usage: $0 host|verify ..." >&2
    exit 2
    ;;
esac
