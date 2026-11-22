#!/bin/bash
# The engine with a configuration file and its control port on one AROS
# target: two folders with one device and a third with a device that
# refuses connections, driven by a guest script through `stpoc ctl`
# (no second Shell, no Rexx interpreter).
#
#   scripts/aros-engine-ctl.sh setup TAG OUT-DIR RESULTS-DIR
#       Creates the peer folders cfa-TAG and cfb-TAG under
#       $ST_ACCEPT_DIR/TAG, shares them with the test identity and writes
#       the guest's stage to OUT-DIR: stpoc, id/, engine.conf, ctl.scr.
#       In the guest:  Execute RAM:stp/ctl.scr   (about 150 s)
#   scripts/aros-engine-ctl.sh verify TAG COLLECTED-DIR
#       Checks the answers the guest wrote and both folders on the peer.
#
# The peer must be an isolated, running st-peer.sh instance.
set -e
. "$(dirname "$0")/env.sh"
st_require ST_PEER_HOME ST_PEER_PORT ST_ACCEPT_DIR ST_ACCEPT_ID
P="$ST_ROOT/scripts/st-peer.sh"
cmd=${1:?setup or verify}
tag=${2:?tag}
base="$ST_ACCEPT_DIR/$tag"
export ST_PEER_FOLDER_TYPE=sendreceive

case "$cmd" in
setup)
    out=${3:?out dir}
    R=${4:?guest results dir}
    [ ! -e "$base" ] || { echo "$base exists; use a new tag" >&2; exit 2; }
    [ ! -e "$out" ] || { echo "$out exists" >&2; exit 2; }
    aros_id=$("$ST_ROOT/build/host/stpoc" /dev/null identity-show "$ST_ACCEPT_ID" | grep -oE '[A-Z0-9]{7}(-[A-Z0-9]{7}){7}' | head -1)
    peer_id=$("$P" id)
    mkdir -p "$out/id" "$out/other"
    "$ST_ROOT/build/host/stpoc" /dev/null identity-create "$out/other" > /dev/null
    other_id=$("$ST_ROOT/build/host/stpoc" /dev/null identity-show "$out/other" | grep -oE '[A-Z0-9]{7}(-[A-Z0-9]{7}){7}' | head -1)
    rm -rf "$out/other"
    for f in a b; do
        mkdir -p "$base/$f/.stfolder"
        printf 'from the peer, folder %s\n' $f > "$base/$f/peer-$f.txt"
        ST_PEER_FOLDER_ID=cf$f-$tag ST_PEER_FOLDER_LABEL=cf$f-$tag ST_PEER_FOLDER_DIR="$base/$f" "$P" share "$aros_id" "aros-$tag" > /dev/null
        ST_PEER_FOLDER_ID=cf$f-$tag ST_PEER_FOLDER_DIR="$base/$f" "$P" scan > /dev/null
    done
    cp "${ST_SOAK_BIN:-$ST_BUILD/stpoc}" "$out/stpoc"
    cp "$ST_ACCEPT_ID/cert.pem" "$ST_ACCEPT_ID/key.pem" "$out/id/"
    cat > "$out/engine.conf" <<CONF
# aros-engine-ctl.sh, $tag
name aros-$tag
# Nothing beyond the test: no listener, no public servers, no announcements.
listen off
global-discovery off
relays off
local-announce off

device peer
  id $peer_id
  address 10.0.2.2:$ST_PEER_PORT
device closed
  id $other_id
  address 10.0.2.2:9

folder cfa-$tag
  path RAM:eng-a
  with peer
  rescan 5
folder cfb-$tag
  path RAM:eng-b
  with peer
  rescan 3600
folder cfc-$tag
  path RAM:eng-c
  with closed
CONF
    cat > "$out/ctl.scr" <<EOF2
; The engine with a configuration file and its control port.
FailAt 1000
Stack 262144
Version >$R/0-system.txt
Version rexxsyslib.library >>$R/0-system.txt
MakeDir RAM:eng-a RAM:eng-b RAM:eng-c
Echo "made on AROS, a" >RAM:eng-a/here-a.txt
Echo "made on AROS, b" >RAM:eng-b/here-b.txt
RAM:stp/stpoc ctl STATUS >$R/1-no-engine.txt
Echo "rc=\$RC" >>$R/1-no-engine.txt
Run >NIL: RAM:stp/stpoc $R/ctl-engine.log engine RAM:stp/id RAM:stp/engine.conf
Wait 40
RAM:stp/stpoc ctl VERSION >$R/2-version.txt
RAM:stp/stpoc ctl STATUS >$R/3-status.txt
Echo "rc=\$RC" >>$R/3-status.txt
RAM:stp/stpoc >NIL: $R/second.log engine RAM:stp/id RAM:stp/engine.conf
Echo "rc=\$RC" >$R/4-second-rc.txt
Echo "needs a scan on request" >RAM:eng-b/asked.txt
Wait 10
RAM:stp/stpoc ctl STATUS >$R/5-before-scan.txt
RAM:stp/stpoc ctl scan cfb-$tag >$R/5-scan.txt
Echo "rc=\$RC" >>$R/5-scan.txt
Wait 15
RAM:stp/stpoc ctl STATUS >$R/6-status.txt
RAM:stp/stpoc ctl PAUSE cfb-$tag >$R/6-pause.txt
Echo "rc=\$RC" >>$R/6-pause.txt
Wait 6
Echo "made while paused" >RAM:eng-b/paused.txt
Echo "made beside a paused folder" >RAM:eng-a/beside.txt
Wait 14
RAM:stp/stpoc ctl STATUS >$R/6-paused-status.txt
RAM:stp/stpoc ctl RESUME cfb-$tag >$R/6-resume.txt
Echo "rc=\$RC" >>$R/6-resume.txt
Wait 20
RAM:stp/stpoc ctl STATUS >$R/6-resumed-status.txt
RAM:stp/stpoc ctl FROBNICATE >$R/7-bad.txt
Echo "rc=\$RC" >>$R/7-bad.txt
RAM:stp/stpoc ctl SCAN nosuch >$R/7-nosuch.txt
Echo "rc=\$RC" >>$R/7-nosuch.txt
RAM:stp/stpoc ctl STOP >$R/8-stop.txt
Echo "rc=\$RC" >>$R/8-stop.txt
Wait 12
RAM:stp/stpoc ctl STATUS >$R/8-after-stop.txt
Echo "rc=\$RC" >>$R/8-after-stop.txt
List RAM:eng-a RAM:eng-b >$R/9-list.txt
RAM:stp/stpoc manifest RAM:eng-a $R/9-manifest-a.txt
RAM:stp/stpoc manifest RAM:eng-b $R/9-manifest-b.txt
Echo "done" >$R/9-done.txt
EOF2
    (cd "$out" && shasum -a 256 stpoc engine.conf ctl.scr > versions.txt)
    echo "stage $out"
    ;;
verify)
    got=${3:?collected dir}
    rc=0
    bad() { echo "FAIL: $*"; rc=1; }
    has() { grep -q "$2" "$got/$1" 2> /dev/null || { bad "$1 lacks: $2"; [ -f "$got/$1" ] && sed 's/^/    /' "$got/$1"; }; }
    [ -f "$got/9-done.txt" ] || bad "the guest script did not reach its end"
    has 1-no-engine.txt '^rc=20'
    has 2-version.txt '^aros-syncthing .* control 1'
    has 3-status.txt '^device peer connected sessions=1 '
    has 3-status.txt "^folder cfa-$tag in-sync device=peer "
    has 3-status.txt "^folder cfb-$tag in-sync device=peer "
    has 3-status.txt '^device closed waiting .*last=CONNECT_FAILED'
    has 3-status.txt "^folder cfc-$tag idle device=closed "
    has 3-status.txt '^rc=0'
    has 4-second-rc.txt '^rc=10'
    has second.log 'RESULT FAIL ALREADY_RUNNING'
    has 5-scan.txt '^OK'
    has 6-status.txt "^folder cfb-$tag in-sync device=peer "
    has 6-pause.txt '^OK'
    has 6-paused-status.txt "^folder cfb-$tag paused-here device=peer "
    has 6-paused-status.txt "^folder cfa-$tag in-sync device=peer "
    has 6-resume.txt '^OK'
    has 6-resumed-status.txt "^folder cfb-$tag in-sync device=peer "
    has 7-bad.txt '^rc=10'
    has 7-nosuch.txt '^rc=5'
    has 8-stop.txt '^OK'
    has 8-after-stop.txt '^rc=20'
    tail -2 "$got/ctl-engine.log" 2> /dev/null | grep -q 'RESULT PASS STOPPED' || bad "the engine did not end with RESULT PASS STOPPED"
    grep -q 'engine: control interface open' "$got/ctl-engine.log" 2> /dev/null || bad "no control interface in the log"
    for f in a b; do
        [ -f "$base/$f/here-$f.txt" ] || bad "here-$f.txt did not reach the peer"
        has 9-list.txt "peer-$f.txt"
        want=$(cd "$base/$f" && find . -type f ! -path './.stfolder*' | LC_ALL=C sort | xargs shasum -a 256 | LC_ALL=C sort -k2)
        [ "$(grep -v '^end ' "$got/9-manifest-$f.txt" 2> /dev/null | tr -d '\r' | LC_ALL=C sort -k2)" = "$want" ] || bad "folder $f: the guest's manifest and the peer's folder differ"
        has 9-manifest-$f.txt '^end [0-9]* files, 0 unreadable'
    done
    [ -f "$base/b/paused.txt" ] || bad "paused.txt did not reach the peer after RESUME"
    [ -f "$base/a/beside.txt" ] || bad "beside.txt did not reach the peer while the other folder was paused"
    [ -f "$base/b/asked.txt" ] || bad "asked.txt did not reach the peer after the scan on request"
    [ "$rc" = 0 ] && echo "PASS: engine configuration and control port on AROS ($tag)" || echo "FAIL: engine configuration and control port on AROS ($tag)"
    exit $rc
    ;;
*)
    echo "usage: $0 setup|verify TAG ..." >&2
    exit 2
    ;;
esac
