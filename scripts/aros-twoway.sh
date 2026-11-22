#!/bin/bash
# Host side of tests/aros/twoway.scr (two-way first flow on one AROS target).
#
#   scripts/aros-twoway.sh setup TAG OUT-DIR RESULTS-DIR
#       Creates the peer folders tw-TAG (Send & Receive, p1.txt from the
#       peer) and mk-TAG (empty, for the marker connections) under
#       $ST_ACCEPT_DIR/TAG, shares them with the test identity, and writes
#       the guest's stage to OUT-DIR: stpoc, twoway.scr, id/. RESULTS-DIR is
#       the guest's results directory (RESULTS:runs/<run>).
#   scripts/aros-twoway.sh watch TAG [LIMIT-SECONDS]
#       Follows the peer's log: after twchg-TAG it edits a.txt in tw-TAG and
#       rescans; returns 0 at done-TAG, 1 at the real-time limit (2400 s).
#   scripts/aros-twoway.sh verify TAG COLLECTED-DIR
#       Checks the collected results: every session PASS, nothing announced
#       and no new sequence on a restart, the peer's edit received, the
#       local edit on the peer, the same files with the same SHA-256 on both
#       sides, no conflict copies, the peer idle with nothing needed.
#
# The peer must be an isolated, running st-peer.sh instance (ST_PEER_HOME,
# ST_PEER_PORT, ...). ST_ACCEPT_ID is the test identity directory
# (disposable). The build is taken from build/$ST_TARGET.
set -e
. "$(dirname "$0")/env.sh"
st_require ST_PEER_HOME ST_PEER_PORT ST_ACCEPT_DIR ST_ACCEPT_ID
P="$ST_ROOT/scripts/st-peer.sh"
cmd=${1:?setup, watch or verify}
tag=${2:?tag}
base="$ST_ACCEPT_DIR/$tag"
log="$ST_PEER_HOME/syncthing.log"
EDIT='created on AROS, edited on the peer'

case "$cmd" in
setup)
    out=${3:?out dir}
    results=${4:?guest results dir}
    [ ! -e "$base" ] || { echo "$base exists; use a new tag" >&2; exit 2; }
    [ ! -e "$out" ] || { echo "$out exists" >&2; exit 2; }
    aros_id=$("$ST_ROOT/build/host/stpoc" /dev/null identity-show "$ST_ACCEPT_ID" | grep -oE '[A-Z0-9]{7}(-[A-Z0-9]{7}){7}' | head -1)
    peer_id=$("$P" id)
    mkdir -p "$base/tw/.stfolder" "$base/mk/.stfolder"
    printf 'from the peer\n' > "$base/tw/p1.txt"
    ST_PEER_FOLDER_ID=tw-$tag ST_PEER_FOLDER_LABEL=tw-$tag ST_PEER_FOLDER_DIR=$base/tw ST_PEER_FOLDER_TYPE=sendreceive \
        "$P" share "$aros_id" "aros-$tag" > /dev/null
    ST_PEER_FOLDER_ID=tw-$tag ST_PEER_FOLDER_DIR=$base/tw "$P" scan > /dev/null
    ST_PEER_FOLDER_ID=mk-$tag ST_PEER_FOLDER_LABEL=mk-$tag ST_PEER_FOLDER_DIR=$base/mk "$P" share "$aros_id" "aros-$tag" > /dev/null
    mkdir -p "$out/id"
    cp "$ST_BUILD/stpoc" "$out/"
    cp "$ST_ACCEPT_ID/cert.pem" "$ST_ACCEPT_ID/key.pem" "$out/id/"
    sed -e "s|<TAG>|$tag|g" -e "s|<PEER>|$peer_id|g" -e "s|<PORT>|$ST_PEER_PORT|g" \
        -e "s|<OUT>|$results|g" -e "s|<DEST>|SYS:sttw-$tag|g" "$ST_ROOT/tests/aros/twoway.scr" > "$out/twoway.scr"
    shasum -a 256 "$out/stpoc" "$out/twoway.scr" | sed "s|$out/||" > "$out/versions.txt"
    echo "peer device $peer_id"
    echo "aros device $aros_id"
    echo "stage $out"
    ;;
watch)
    limit=${3:-2400}
    start=$(date +%s)
    changed=0
    while :; do
        now=$(date +%s)
        [ $((now - start)) -lt "$limit" ] || { echo "time limit $limit s"; exit 1; }
        if [ $changed -eq 0 ] && grep -q "remote.name=twchg-$tag " "$log"; then
            printf '%s\n' "$EDIT" > "$base/tw/a.txt"
            ST_PEER_FOLDER_ID=tw-$tag ST_PEER_FOLDER_DIR=$base/tw "$P" scan > /dev/null
            changed=1
            echo "$(date -u +%H:%M:%S) twchg: a.txt edited on the peer"
        fi
        if grep -q "remote.name=done-$tag " "$log"; then
            echo "$(date -u +%H:%M:%S) done marker after $((now - start)) s"
            exit 0
        fi
        sleep 2
    done
    ;;
verify)
    dir=${3:?collected dir}
    fails=0
    bad() { echo "FAIL: $*"; fails=$((fails + 1)); }
    for s in 1-first 2-restart 4-peer-edit 5-restart 6-local-edit 7-restart; do
        f=$(find "$dir" -name "$s.log" | head -1)
        [ -n "$f" ] || { bad "$s.log missing"; continue; }
        last=$(tail -1 "$f" | sed 's/^\[ *[0-9]* ms\] //')
        [ "$last" = "RESULT PASS" ] || bad "$s: $last"
        printf '  %-13s %s; %s\n' "$s" "$last" "$(grep -o 'here [0-9]* new, [0-9]* changed[^;]*;[^;]*' "$f" | head -1)"
    done
    echo "  scan policy: $(grep -hoE 'scan: .*(are trusted|at each scan).*' "$(find "$dir" -name 1-first.log | head -1)" | head -1 | sed 's/^scan: //')"
    seq_at_start() { sed -n 's/.*state: [0-9]* entr[a-z]*, index ID [0-9a-f]*, sequence \([0-9]*\).*/\1/p' "$(find "$dir" -name "$1.log" | head -1)"; }
    for pair in "2-restart 4-peer-edit" "5-restart 6-local-edit" "7-restart"; do
        set -- $pair
        f=$(find "$dir" -name "$1.log" | head -1)
        grep -q ' 0 announced' "$f" || bad "$1 announced something"
        if [ -n "${2:-}" ]; then
            [ "$(seq_at_start "$1")" = "$(seq_at_start "$2")" ] || bad "$1 moved the sequence ($(seq_at_start "$1") -> $(seq_at_start "$2"))"
        fi
    done
    f4=$(find "$dir" -name 4-a.txt | head -1)
    [ "$(cat "$f4" 2>/dev/null)" = "$EDIT" ] || bad "the peer's edit did not arrive (4-a.txt: $(cat "$f4" 2>/dev/null))"
    grep -q '"a.txt": newer version on the peer (row 3)' "$(find "$dir" -name 4-peer-edit.log)" || bad "4: not taken by row 3"
    grep -q '"a.txt" changed here: new version' "$(find "$dir" -name 6-local-edit.log)" || bad "6: local edit not found"
    tw=$(find "$dir" -type d -name tw | head -1)
    a=$(cd "$tw" && find . -type f ! -path './.stfolder*' ! -name '*.info' | LC_ALL=C sort | xargs shasum -a 256)
    b=$(cd "$base/tw" && find . -type f ! -path './.stfolder*' | LC_ALL=C sort | xargs shasum -a 256)
    if [ "$a" = "$b" ]; then echo "  same files and SHA-256 on both sides:"; echo "$a" | sed 's/^/    /'; else bad "contents differ"; echo "AROS:"; echo "$a"; echo "peer:"; echo "$b"; fi
    [ -z "$(find "$tw" "$base/tw" -name '*.sync-conflict-*')" ] || bad "conflict copies exist"
    ST_PEER_FOLDER_ID=tw-$tag "$P" folder-status | python3 -c 'import sys,json; d=json.load(sys.stdin); print("  peer:", d["state"], "needFiles", d["needFiles"], "pullErrors", d["pullErrors"]); sys.exit(0 if d["state"]=="idle" and d["needFiles"]==0 and d["pullErrors"]==0 else 1)' || bad "the peer is not idle with nothing needed"
    ST_PEER_FOLDER_ID=tw-$tag "$P" file a.txt | python3 -c 'import sys,json; d=json.load(sys.stdin); print("  peer a.txt version", ",".join(d["global"]["version"]))'
    [ "$(cat "$(find "$dir" -name 9-cleanup.txt | head -1)" 2>/dev/null)" = "SYS:sttw-$tag removed" ] || bad "guest cleanup not confirmed"
    [ $fails -eq 0 ] && echo "PASS: two-way flow on AROS ($tag)" || { echo "FAILED: $fails check(s)"; exit 1; }
    ;;
*) echo "usage: $0 setup|watch|verify TAG ..." >&2; exit 2 ;;
esac
