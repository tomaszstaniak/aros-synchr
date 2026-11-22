#!/bin/bash
# The engine over time on one AROS target, timed on both sides so that
# nothing has to be typed in the guest after the start.
#
#   scripts/aros-soak.sh setup TAG OUT-DIR RESULTS-DIR [MINUTES]
#       Creates the peer folder tw-TAG under $ST_ACCEPT_DIR/TAG, shares it
#       with the test identity and writes the guest's stage to OUT-DIR:
#       stpoc, id/, blob.bin, soak.scr (the guest's own changes, one every
#       6 s for MINUTES, default 60) and go.scr. In the guest:
#           Execute RAM:stp/go.scr
#       go.scr starts soak.scr in the background and the engine in front.
#       soak.scr waits 240 s after its last change and stops the engine
#       through its control port (stpoc ctl STOP; should that fail, with
#       Ctrl-C to CLI \$SOAK_CLI, default 1). go.scr then writes the SHA-256
#       of every file in the folder to RESULTS-DIR/manifest.txt (stpoc
#       manifest): a few seconds, where a copy of the folder to a FAT
#       results volume took minutes.
#   scripts/aros-soak.sh drive TAG [MINUTES]
#       Start it right after go.scr. Changes the peer's side in its own
#       names for MINUTES (a change about every 4 s, files up to about
#       1.3 MB), restarts the peer every RESTART_EVERY seconds (default
#       180) and leaves it running. Then waits until the guest's side
#       has ended: the engine disconnects when the guest's script stops
#       it (the guest's clock may be slower than this one).
#   scripts/aros-soak.sh verify TAG COLLECTED-DIR
#       The engine ended with RESULT PASS STOPPED, the guest's manifest
#       is complete and lists the same files with the same SHA-256 as the
#       peer's folder, no conflict copies.
#
# The peer must be an isolated, running st-peer.sh instance of its own
# (one per guest: the drive step restarts it).
set -e
. "$(dirname "$0")/env.sh"
st_require ST_PEER_HOME ST_PEER_PORT ST_ACCEPT_DIR ST_ACCEPT_ID
P="$ST_ROOT/scripts/st-peer.sh"
cmd=${1:?setup, drive or verify}
tag=${2:?tag}
base="$ST_ACCEPT_DIR/$tag"
export ST_PEER_FOLDER_ID=tw-$tag ST_PEER_FOLDER_DIR=$base/tw ST_PEER_FOLDER_LABEL=tw-$tag ST_PEER_FOLDER_TYPE=sendreceive
list() { (cd "$1" && find . -type f ! -path './.stfolder*' ! -name '.syncthing.*' ! -name '.stpoc-*' | LC_ALL=C sort | xargs shasum -a 256 2> /dev/null); }

case "$cmd" in
setup)
    out=${3:?out dir}
    results=${4:?guest results dir}
    minutes=${5:-60}
    [ ! -e "$base" ] || { echo "$base exists; use a new tag" >&2; exit 2; }
    [ ! -e "$out" ] || { echo "$out exists" >&2; exit 2; }
    aros_id=$("$ST_ROOT/build/host/stpoc" /dev/null identity-show "$ST_ACCEPT_ID" | grep -oE '[A-Z0-9]{7}(-[A-Z0-9]{7}){7}' | head -1)
    peer_id=$("$P" id)
    mkdir -p "$base/tw/.stfolder" "$out/id"
    printf 'p0, from the peer\n' > "$base/tw/p0.dat"
    "$P" share "$aros_id" "aros-$tag" > /dev/null
    "$P" scan > /dev/null
    cp "${ST_SOAK_BIN:-$ST_BUILD/stpoc}" "$out/stpoc"
    cp "$ST_ACCEPT_ID/cert.pem" "$ST_ACCEPT_ID/key.pem" "$out/id/"
    head -c 700000 /dev/urandom > "$out/blob.bin"
    {
        echo "; The guest's own changes: $((minutes * 10)) rounds, one every 6 s."
        echo "FailAt 1000"
        n=0
        while [ $n -lt $((minutes * 10)) ]; do
            n=$((n + 1))
            k=$((n % 30))
            echo "Echo \"guest round $n\" >RAM:eng/g$k.dat"
            [ $((n % 8)) != 0 ] || echo "Join RAM:eng/g$k.dat RAM:stp/blob.bin AS RAM:eng/gb$((n / 8 % 4)).dat"
            [ $((n % 7)) != 0 ] || echo "Delete RAM:eng/g$(((n + 15) % 30)).dat QUIET"
            echo "Wait 6"
        done
        echo "Echo \"rounds done\" >$results/soak-rounds-done.txt"
        echo "Wait 240"
        echo "RAM:stp/stpoc ctl STOP >$results/stop.txt"
        echo "If WARN"
        echo "  Echo \"ctl STOP failed, Ctrl-C sent\" >>$results/stop.txt"
        echo "  Break ${SOAK_CLI:-1} C"
        echo "EndIf"
    } > "$out/soak.scr"
    cat > "$out/go.scr" <<EOF
; Engine soak. Run in the first Shell of a fresh boot: Execute RAM:stp/go.scr
FailAt 1000
Stack 262144
Version >$results/0-system.txt
MakeDir RAM:eng
Run >NIL: Execute RAM:stp/soak.scr
RAM:stp/stpoc --log-max 400000 $results/engine.log engine RAM:stp/id 10.0.2.2 $ST_PEER_PORT $peer_id tw-$tag RAM:eng 5
Echo "rc=\$RC" >$results/engine-rc.txt
RAM:stp/stpoc manifest RAM:eng $results/manifest.txt
Echo "rc=\$RC" >$results/manifest-rc.txt
Avail >$results/avail-end.txt
Echo "done" >$results/go-done.txt
EOF
    (cd "$out" && shasum -a 256 stpoc soak.scr go.scr > versions.txt)
    echo "peer device $peer_id"
    echo "aros device $aros_id"
    echo "stage $out ($minutes minutes)"
    ;;
drive)
    minutes=${3:-60}
    every=${RESTART_EVERY:-180}
    start=$(date +%s)
    end=$((start + minutes * 60))
    next=$((start + every))
    ops=0 restarts=0
    while [ "$(date +%s)" -lt "$end" ]; do
        f="$base/tw/p$((RANDOM % 40)).dat"
        case $((RANDOM % 5)) in
        0) rm -f "$f" ;;
        1) head -c $((RANDOM * 40 + 1)) /dev/urandom > "$f" ;;
        *) head -c $((RANDOM % 5000 + 1)) /dev/urandom > "$f" ;;
        esac
        ops=$((ops + 1))
        "$P" scan > /dev/null 2>&1 || true
        if [ "$(date +%s)" -ge "$next" ]; then
            "$P" stop > /dev/null
            sleep $((RANDOM % 8 + 1))
            "$P" start > /dev/null
            restarts=$((restarts + 1))
            next=$(( $(date +%s) + every ))
        fi
        sleep $((RANDOM % 5 + 2))
    done
    "$P" status > /dev/null || "$P" start > /dev/null
    "$P" scan > /dev/null 2>&1 || true
    echo "peer side: $ops changes, $restarts restarts in $minutes min; waiting for the guest's end"
    # The guest's script runs on the guest's clock, which may be slow: its
    # end is seen here as the engine's disconnection (20 s without the
    # device), looked for until twice the planned time has passed.
    aros_id=$("$ST_ROOT/build/host/stpoc" /dev/null identity-show "$ST_ACCEPT_ID" | grep -oE '[A-Z0-9]{7}(-[A-Z0-9]{7}){7}' | head -1)
    limit=$((start + minutes * 120 + 900))
    gone=0
    while [ "$(date +%s)" -lt "$limit" ] && [ $gone -lt 4 ]; do
        if "$P" connected "$aros_id" > /dev/null 2>&1; then gone=0; else gone=$((gone + 1)); fi
        sleep 5
    done
    [ $gone -ge 4 ] && echo "the engine disconnected $(( $(date +%s) - start )) s after the start" \
        || echo "the engine was still connected at the time limit"
    sleep 60 # go.scr writes the manifest after the engine's end
    echo "drive done"
    ;;
verify)
    got=${3:?collected dir}
    rc=0
    bad() { echo "FAIL: $*"; rc=1; }
    [ -f "$got/engine.log" ] || { echo "FAIL: no engine.log in $got"; exit 1; }
    tail -3 "$got/engine.log" | grep -q "RESULT PASS STOPPED" || bad "the engine did not end with RESULT PASS STOPPED: $(tail -1 "$got/engine.log")"
    [ -f "$got/go-done.txt" ] || bad "go.scr did not reach its end"
    [ -f "$got/manifest.txt" ] || { bad "no manifest of the guest's folder"; exit 1; }
    tail -1 "$got/manifest.txt" | grep -q '^end [0-9]* files, 0 unreadable' || bad "the manifest is incomplete: $(tail -1 "$got/manifest.txt")"
    guest() { grep -v '^end ' "$got/manifest.txt" | tr -d '\r' | LC_ALL=C sort -k2; }
    peer() { list "$base/tw" | LC_ALL=C sort -k2; }
    if [ "$(guest)" != "$(peer)" ]; then
        diff <(guest) <(peer) | head -20
        bad "the guest's folder and the peer's folder differ"
    fi
    [ -z "$(find "$base/tw" -name '*.sync-conflict-*')" ] && ! grep -q 'sync-conflict' "$got/manifest.txt" || bad "conflict copies exist"
    sessions=$(sed -n 's/.*stopped on request after \([0-9]*\) session.*/\1/p' "$got/engine.log" | tail -1)
    secs=$(tail -1 "$got/engine.log" | sed -n 's/^\[ *\([0-9]*\) ms\].*/\1/p')
    echo "sessions $sessions, engine time $((${secs:-0} / 1000)) s, $(guest | wc -l | tr -d ' ') files at the end"
    [ "$rc" = 0 ] && echo "PASS: engine soak on AROS ($tag)" || echo "FAIL: engine soak on AROS ($tag)"
    exit $rc
    ;;
*)
    echo "usage: $0 setup|drive|verify TAG ..." >&2
    exit 2
    ;;
esac
