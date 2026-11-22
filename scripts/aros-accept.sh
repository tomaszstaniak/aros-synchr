#!/bin/bash
# Host side of tests/aros/accept.scr (acceptance on one AROS target).
#
#   scripts/aros-accept.sh setup TAG OUT-DIR RESULTS-DIR
#       Creates the peer folders acc-TAG, multi-TAG (1000 files, then 10
#       more in a later index batch) and empty-TAG under $ST_ACCEPT_DIR/TAG
#       (folder IDs new for every run, so no earlier index is mixed in),
#       and dp-TAG (a.txt, b.txt, c.txt for tests/aros/dataprotect.scr),
#       shares them with the test identity, and writes the stage for the
#       guest to OUT-DIR: stpoc, tlsprobe, holdopen, accept.scr,
#       dataprotect.scr, profile.scr, dirbench.scr (plus stgui and dirbench
#       when built), id/, pre/, pretree/. tree-TAG holds the
#       same 1000 contents as multi-TAG's first batch in 10 directories.
#       RESULTS-DIR is the guest's results directory (RESULTS:runs/<run>).
#   scripts/aros-accept.sh watch TAG [LIMIT-SECONDS]
#       Follows the peer's log: after the guest's chg1-TAG marker it changes
#       hello.txt, after dpchg-TAG a/b/c.txt of dp-TAG, and rescans; it
#       returns 0 at the last marker (done-dp-TAG, or done-TAG when
#       WATCH_UNTIL=done), 1 at the real-time limit (default 2400 s).
#
# The peer must be an isolated, running st-peer.sh instance (ST_PEER_HOME,
# ST_PEER_PORT, ...). ST_ACCEPT_ID is the test identity directory (created
# with stpoc identity-create on the host; disposable, never kept as
# evidence). Guests that run at the same time need different identities:
# Syncthing keeps one connection per device, so two guests with one
# Device ID displace each other's sessions (seen 2026-11-09: failed TLS,
# missed markers). The build is taken from build/$ST_TARGET.
set -e
. "$(dirname "$0")/env.sh"
st_require ST_PEER_HOME ST_PEER_PORT ST_ACCEPT_DIR ST_ACCEPT_ID
P="$ST_ROOT/scripts/st-peer.sh"
cmd=${1:?setup or watch}
tag=${2:?tag}
base="$ST_ACCEPT_DIR/$tag"
log="$ST_PEER_HOME/syncthing.log"

share() { # folder-id dir
    ST_PEER_FOLDER_ID=$1 ST_PEER_FOLDER_LABEL=$1 ST_PEER_FOLDER_DIR=$2 "$P" share "$aros_id" "aros-$tag" > /dev/null
    ST_PEER_FOLDER_ID=$1 ST_PEER_FOLDER_DIR=$2 "$P" scan > /dev/null
}

case "$cmd" in
setup)
    out=${3:?out dir}
    results=${4:?guest results dir}
    [ ! -e "$base" ] || { echo "$base exists; use a new tag" >&2; exit 2; }
    [ ! -e "$out" ] || { echo "$out exists" >&2; exit 2; }
    aros_id=$("$ST_ROOT/build/host/stpoc" /dev/null identity-show "$ST_ACCEPT_ID" | grep -oE '[A-Z0-9]{7}(-[A-Z0-9]{7}){7}' | head -1)
    peer_id=$("$P" id)
    # A valid Device ID that is not the peer's, for the refusal test: ours.
    wrong_id=$aros_id
    mkdir -p "$base/acc/sub" "$base/acc/Data" "$base/multi" "$base/empty" "$base/pre" "$base/dp" "$base/tree" "$base/pretree"
    for d in acc multi empty dp tree; do mkdir -p "$base/$d/.stfolder"; done
    for f in a b c; do printf '%s.txt, version 1 from the peer\n' "$f" > "$base/dp/$f.txt"; done
    : > "$base/acc/empty.txt"
    printf 'Hello from the Mac, version 1.\n' > "$base/acc/hello.txt"
    printf 'nested v1\n' > "$base/acc/sub/nested.txt"
    python3 -c 'import random,sys; r=random.Random(11); sys.stdout.buffer.write(bytes(r.getrandbits(8) for _ in range(600000)))' \
        > "$base/acc/Data/Blob.bin"
    python3 - "$base/multi" "$base/pre" <<'EOF'
import os, sys
multi, pre = sys.argv[1:3]
for i in range(1000):
    data = ("file %d of the first batch\n" % i).encode() * 3
    for d in (multi, pre):
        with open(os.path.join(d, "f%04d.txt" % i), "wb") as f:
            f.write(data)
    for d in (os.path.join(os.path.dirname(multi), "tree"), os.path.join(os.path.dirname(multi), "pretree")):
        sub = os.path.join(d, "d%02d" % (i // 100))
        os.makedirs(sub, exist_ok=True)
        with open(os.path.join(sub, "f%04d.txt" % i), "wb") as f:
            f.write(data)
EOF
    share "multi-$tag" "$base/multi"
    sleep 5
    # Added after the first scan: higher sequences, the second index batch.
    for i in 0 1 2 3 4 5 6 7 8 9; do printf 'new file %d, second batch\n' $i > "$base/multi/n00$i.txt"; done
    ST_PEER_FOLDER_ID="multi-$tag" ST_PEER_FOLDER_DIR="$base/multi" "$P" scan > /dev/null
    share "acc-$tag" "$base/acc"
    share "empty-$tag" "$base/empty"
    share "dp-$tag" "$base/dp"
    share "tree-$tag" "$base/tree"
    sleep 5
    mkdir -p "$out/id" "$out/pre"
    cp "$ST_BUILD/stpoc" "$ST_BUILD/tlsprobe" "$ST_BUILD/holdopen" "$out/"
    for opt in stgui dirbench; do if [ -f "$ST_BUILD/$opt" ]; then cp "$ST_BUILD/$opt" "$out/"; fi; done
    sed -e "s|<OUT>|$results|g" -e "s|<DEST>|SYS:stdb-$tag|g" "$ST_ROOT/tests/aros/dirbench.scr" > "$out/dirbench.scr"
    cp "$ST_ACCEPT_ID/cert.pem" "$ST_ACCEPT_ID/key.pem" "$out/id/"
    cp "$base/pre/"* "$out/pre/"
    cp -R "$base/pretree" "$out/pretree"
    stname() { printf 'state-%s.txt' "$(printf '%s' "$1" | shasum -a 256 | cut -c1-32)"; }
    sed -e "s|<TAG>|$tag|g" -e "s|<PEER>|$peer_id|g" -e "s|<PORT>|$ST_PEER_PORT|g" \
        -e "s|<OUT>|$results|g" -e "s|<DEST>|SYS:stpf-$tag|g" -e "s|<STFLAT>|$(stname "multi-$tag")|g" \
        -e "s|<STTREE>|$(stname "tree-$tag")|g" "$ST_ROOT/tests/aros/profile.scr" > "$out/profile.scr"
    sed -e "s|<TAG>|$tag|g" -e "s|<PEER>|$peer_id|g" -e "s|<WRONG>|$wrong_id|g" -e "s|<PORT>|$ST_PEER_PORT|g" \
        -e "s|<OUT>|$results|g" -e "s|<DEST>|SYS:stacc-$tag|g" "$ST_ROOT/tests/aros/accept.scr" > "$out/accept.scr"
    sed -e "s|<TAG>|$tag|g" -e "s|<PEER>|$peer_id|g" -e "s|<PORT>|$ST_PEER_PORT|g" \
        -e "s|<OUT>|$results|g" -e "s|<DEST>|SYS:stdp-$tag|g" "$ST_ROOT/tests/aros/dataprotect.scr" > "$out/dataprotect.scr"
    {
        echo "target $ST_TARGET"
        echo "peer $("$SYNCTHING_BIN" --version)"
        echo "peer binary sha256 $(shasum -a 256 "$SYNCTHING_BIN" | cut -c1-64)"
        echo "peer device $peer_id"
        echo "aros device $aros_id"
        (cd "$out" && shasum -a 256 stpoc tlsprobe holdopen accept.scr dataprotect.scr profile.scr dirbench.scr
         for opt in stgui dirbench; do if [ -f "$opt" ]; then shasum -a 256 "$opt"; fi; done)
    } > "$base/versions.txt"
    cat "$base/versions.txt"
    ;;
watch)
    limit=${3:-2400}
    start=$(date +%s)
    changed=0
    dpchanged=0
    last="done-dp-$tag"
    [ "${WATCH_UNTIL:-}" = done ] && last="done-$tag"
    [ "${WATCH_UNTIL:-}" = pf ] && last="done-pf-$tag"
    while :; do
        now=$(date +%s)
        if [ $((now - start)) -ge "$limit" ]; then
            echo "$(date -u +%H:%M:%S) real-time limit reached"
            exit 1
        fi
        if [ $changed -eq 0 ] && grep -q "remote.name=chg1-$tag " "$log"; then
            printf 'Hello from the Mac, version 2: changed on the peer.\n' > "$base/acc/hello.txt"
            ST_PEER_FOLDER_ID="acc-$tag" ST_PEER_FOLDER_DIR="$base/acc" "$P" scan > /dev/null
            echo "$(date -u +%H:%M:%S) chg1: hello.txt v2 on the peer, $(shasum -a 256 "$base/acc/hello.txt" | cut -c1-64)"
            changed=1
        fi
        if [ $dpchanged -eq 0 ] && grep -q "remote.name=dpchg-$tag " "$log"; then
            for f in a b c; do printf '%s.txt, version 2 from the peer\n' "$f" > "$base/dp/$f.txt"; done
            ST_PEER_FOLDER_ID="dp-$tag" ST_PEER_FOLDER_DIR="$base/dp" "$P" scan > /dev/null
            echo "$(date -u +%H:%M:%S) dpchg: a, b, c version 2 on the peer"
            dpchanged=1
        fi
        if grep -q "remote.name=$last " "$log"; then
            echo "$(date -u +%H:%M:%S) done marker after $((now - start)) s"
            (cd "$base" && find acc multi dp -type f ! -path '*/.stfolder*' -exec shasum -a 256 {} \; | sort -k2) \
                > "$base/peer-final.sha256"
            exit 0
        fi
        sleep 3
    done
    ;;
*)
    echo "usage: $0 setup|watch ..." >&2
    exit 2
    ;;
esac
