#!/bin/bash
# Host side of tests/aros/stagec.scr (two-way stage C on one AROS target).
#
#   scripts/aros-stagec.sh setup TAG OUT-DIR RESULTS-DIR
#       Creates the peer folders tw-TAG (Send & Receive; p1.txt and p2.txt
#       from the peer) and mk-TAG (for the marker connections) under
#       $ST_ACCEPT_DIR/TAG, shares them with the test identity and writes the
#       guest's stage to OUT-DIR: stpoc, stagec.scr, id/.
#   scripts/aros-stagec.sh watch TAG [LIMIT-SECONDS]
#       Follows the peer's log and makes the peer's changes at the guest's
#       markers: s3 deletes b.txt, s4 edits c.txt (now), s5 edits d.txt
#       (dated 30 days back), s6 deletes e.txt, s7 edits p1.txt (dated 30
#       days ahead), s8 adds big.bin (BIG_MB, default 16 MB; with
#       CUT_KBPS set the peer sends at most that many KiB/s from then to
#       the end, which makes the cut of step 8 certain with a small file:
#       BIG_MB=4 CUT_KBPS=100). s4 edits
#       c.txt dated 30 days ahead. Returns 0 at done-TAG, 1 at the
#       real-time limit (7200 s).
#   scripts/aros-stagec.sh verify TAG COLLECTED-DIR
#       Checks every step's log and the end state: the same files with the
#       same SHA-256 and the same directories on both sides, three conflict
#       copies holding the losing edits, the peer idle with nothing needed.
#
# The peer must be an isolated, running st-peer.sh instance (ST_PEER_HOME,
# ST_PEER_PORT, ...); ST_ACCEPT_ID is the test identity directory.
set -e
. "$(dirname "$0")/env.sh"
st_require ST_PEER_HOME ST_PEER_PORT ST_ACCEPT_DIR ST_ACCEPT_ID
P="$ST_ROOT/scripts/st-peer.sh"
cmd=${1:?setup, watch or verify}
tag=${2:?tag}
base="$ST_ACCEPT_DIR/$tag"
log="$ST_PEER_HOME/syncthing.log"
peer() { ST_PEER_FOLDER_ID=tw-$tag ST_PEER_FOLDER_DIR=$base/tw "$P" "$@"; }
at() { python3 -c 'import os,sys,time; t=time.time()+float(sys.argv[2]); os.utime(sys.argv[1],(t,t))' "$1" "$2"; }

case "$cmd" in
setup)
    out=${3:?out dir}
    results=${4:?guest results dir}
    [ ! -e "$base" ] || { echo "$base exists; use a new tag" >&2; exit 2; }
    [ ! -e "$out" ] || { echo "$out exists" >&2; exit 2; }
    aros_id=$("$ST_ROOT/build/host/stpoc" /dev/null identity-show "$ST_ACCEPT_ID" | grep -oE '[A-Z0-9]{7}(-[A-Z0-9]{7}){7}' | head -1)
    peer_id=$("$P" id)
    mkdir -p "$base/tw/.stfolder" "$base/mk/.stfolder"
    printf 'p1, from the peer\n' > "$base/tw/p1.txt"
    printf 'p2, from the peer\n' > "$base/tw/p2.txt"
    ST_PEER_FOLDER_LABEL=tw-$tag ST_PEER_FOLDER_TYPE=sendreceive peer share "$aros_id" "aros-$tag" > /dev/null
    peer scan > /dev/null
    ST_PEER_FOLDER_ID=mk-$tag ST_PEER_FOLDER_LABEL=mk-$tag ST_PEER_FOLDER_DIR=$base/mk "$P" share "$aros_id" "aros-$tag" > /dev/null
    state=$(python3 -c 'import hashlib,sys; print("state-%s.txt" % hashlib.sha256(sys.argv[1].encode()).hexdigest()[:32])' "tw-$tag")
    mkdir -p "$out/id"
    cp "$ST_BUILD/stpoc" "$out/"
    cp "$ST_ACCEPT_ID/cert.pem" "$ST_ACCEPT_ID/key.pem" "$out/id/"
    sed -e "s|<TAG>|$tag|g" -e "s|<PEER>|$peer_id|g" -e "s|<PORT>|$ST_PEER_PORT|g" -e "s|<OUT>|$results|g" \
        -e "s|<DEST>|SYS:stc-$tag|g" -e "s|<STATE>|$state|g" -e "s|<BIGMB>|${BIG_MB:-16}|g" "$ST_ROOT/tests/aros/stagec.scr" > "$out/stagec.scr"
    shasum -a 256 "$out/stpoc" "$out/stagec.scr" | sed "s|$out/||" > "$out/versions.txt"
    echo "peer device $peer_id"
    echo "aros device $aros_id"
    echo "stage $out"
    ;;
watch)
    limit=${3:-7200}
    start=$(date +%s)
    done_steps=" "
    step() { # MARKER: true once, when the guest connected with it
        case "$done_steps" in *" $1 "*) return 1 ;; esac
        grep -q "remote.name=$1-$tag " "$log" || return 1
        done_steps="$done_steps$1 "
        return 0
    }
    say() { echo "$(date -u +%H:%M:%S) $*"; }
    while :; do
        now=$(date +%s)
        [ $((now - start)) -lt "$limit" ] || { say "time limit $limit s"; exit 1; }
        if step s3; then rm "$base/tw/b.txt"; peer scan > /dev/null; say "s3: b.txt deleted on the peer"; fi
        if step s4; then printf 'c, edited on the peer later\n' > "$base/tw/c.txt"; at "$base/tw/c.txt" 2592000; peer scan > /dev/null; say "s4: c.txt edited on the peer, dated 30 days ahead"; fi
        if step s5; then printf 'd, edited on the peer first\n' > "$base/tw/d.txt"; at "$base/tw/d.txt" -2592000; peer scan > /dev/null; say "s5: d.txt edited on the peer, dated 30 days back"; fi
        if step s6; then rm "$base/tw/e.txt"; peer scan > /dev/null; say "s6: e.txt deleted on the peer"; fi
        if step s7; then printf 'p1, edited on the peer after the deletion there\n' > "$base/tw/p1.txt"; at "$base/tw/p1.txt" 2592000; peer scan > /dev/null; say "s7: p1.txt edited on the peer, dated 30 days ahead"; fi
        if step s8; then
            # With CUT_KBPS the peer sends no faster than that from here on, so
            # the 15 s session of step 8 ends in the middle of big.bin whatever
            # the speed of the guest (BIG_MB * 1000 / CUT_KBPS must exceed 15 s).
            if [ -n "${CUT_KBPS:-}" ]; then "$P" limit "$CUT_KBPS" > /dev/null; say "s8: the peer sends at most $CUT_KBPS KiB/s"; fi
            head -c $((${BIG_MB:-16} * 1000000)) /dev/urandom > "$base/tw/big.bin"; peer scan > /dev/null; say "s8: big.bin (${BIG_MB:-16} MB) added on the peer"
        fi
        if step done; then
            if [ -n "${CUT_KBPS:-}" ]; then "$P" limit 0 > /dev/null; fi
            say "done marker after $((now - start)) s"; exit 0
        fi
        sleep 2
    done
    ;;
verify)
    dir=${3:?collected dir}
    fails=0
    bad() { echo "FAIL: $*"; fails=$((fails + 1)); }
    f() { find "$dir" -name "$1" | head -1; }
    for s in 01-first 02-deleted-here 03-deleted-there 04-both-peer-wins 04b-again 05-both-ours-wins 05b-again \
             06-edit-then-delete 06b-again 07-delete-then-edit 07b-again 08b-complete 09-restart 10-state-lost 10b-again; do
        l=$(f "$s.log")
        [ -n "$l" ] || { bad "$s.log missing"; continue; }
        last=$(tail -1 "$l" | sed 's/^\[ *[0-9]* ms\] //')
        [ "$last" = "RESULT PASS" ] || bad "$s: $last"
        printf '  %-20s %s; %s\n' "$s" "$last" "$(grep -o 'here [0-9]* new, [0-9]* changed, [0-9]* deleted; from the peer [0-9]* file(s) written[^;]*; [0-9]* conflict' "$l" | head -1 | sed 's/, [0-9]* dir(s), [0-9]* recorded, [0-9]* adopted//')"
    done
    grep -q '"a.txt" deleted here' "$(f 02-deleted-here.log)" || bad "02: a.txt deletion not published"
    grep -q '"dir" deleted here' "$(f 02-deleted-here.log)" || bad "02: dir deletion not published"
    grep -q '"b.txt" deleted (deleted on the peer)' "$(f 03-deleted-there.log)" || bad "03: the peer's deletion not applied"
    grep -q '"c.txt" edited on both sides, the peer.s version wins; ours kept as' "$(f 04-both-peer-wins.log)" || bad "04: no conflict copy made here"
    grep -q '"e.txt" deleted on the peer (the deletion wins); ours kept as' "$(f 06-edit-then-delete.log)" || bad "06: our edit not kept as a conflict copy"
    l8=$(f 08-cut.log)
    if [ -n "$l8" ] && tail -1 "$l8" | grep -q 'TIME_LIMIT'; then
        echo "  08-cut               $(tail -1 "$l8" | sed 's/^\[ *[0-9]* ms\] //') (cut during the download, as intended)"
    else
        echo "  NOTE: 08-cut did not end by its time limit ($(tail -1 "$l8" 2>/dev/null | sed 's/^\[ *[0-9]* ms\] //')): no interrupted download"
    fi
    grep -q ' 0 announced' "$(f 09-restart.log)" || bad "09: the restart announced something"
    grep -q 'scan: "c.txt" deleted here' "$(f 10-state-lost.log)" && bad "10: a deletion was published after the state loss"
    tail -1 "$(f 11-no-marker.log)" | grep -q FOLDER_UNAVAILABLE || bad "11: without .stfolder: $(tail -1 "$(f 11-no-marker.log)")"
    tw=$(find "$dir" -type d -name tw | head -1)
    grep -q '"big.bin": equal to the peer.s entry; its version taken' "$(f 10-state-lost.log)" || bad "10: big.bin not found equal to the peer's entry"
    grep -q 'big.bin' "$(f 12-list.txt)" || bad "big.bin not listed here at the end"
    if [ -z "$tw" ]; then bad "no collected tw/ directory: contents not compared"; else
    a=$(cd "$tw" && find . -type f ! -path './.stfolder*' ! -name '*.info' ! -name big.bin | LC_ALL=C sort | xargs shasum -a 256)
    b=$(cd "$base/tw" && find . -type f ! -path './.stfolder*' ! -name big.bin | LC_ALL=C sort | xargs shasum -a 256)
    if [ "$a" = "$b" ]; then echo "  same files and SHA-256 on both sides (big.bin by its blocks, step 10):"; echo "$a" | sed 's/^/    /'; else bad "contents differ"; echo "AROS:"; echo "$a"; echo "peer:"; echo "$b"; fi
    da=$(cd "$tw" && find . -type d ! -path './.stfolder*' | LC_ALL=C sort)
    db=$(cd "$base/tw" && find . -type d ! -path './.stfolder*' | LC_ALL=C sort)
    [ "$da" = "$db" ] || bad "directories differ (AROS: $da; peer: $db)"
    [ -z "$(find "$tw" -name '.stpoc-*')" ] || bad "temporary files left: $(find "$tw" -name '.stpoc-*')"
    fi
    for n in a.txt b.txt e.txt dir; do [ ! -e "$base/tw/$n" ] || bad "$n still on the peer"; done
    [ "$(cat "$base/tw/c.txt")" = "c, edited on the peer later" ] || bad "c.txt is not the peer's edit"
    [ "$(cat "$base/tw/d.txt")" = "d, edited here later" ] || bad "d.txt is not our edit"
    [ "$(cat "$base/tw/p1.txt")" = "p1, edited on the peer after the deletion there" ] || bad "p1.txt did not come back"
    n=$(find "$base/tw" -name '*.sync-conflict-*' | wc -l | tr -d ' ')
    [ "$n" = 3 ] || bad "$n conflict copies on the peer, 3 expected"
    grep -l 'c, edited here first' "$base"/tw/c.sync-conflict-* > /dev/null 2>&1 || bad "our edit of c.txt is not kept"
    grep -l 'd, edited on the peer first' "$base"/tw/d.sync-conflict-* > /dev/null 2>&1 || bad "the peer's edit of d.txt is not kept"
    grep -l 'e, edited here before the deletion' "$base"/tw/e.sync-conflict-* > /dev/null 2>&1 || bad "our edit of e.txt is not kept"
    peer folder-status | python3 -c 'import sys,json; d=json.load(sys.stdin); print("  peer:", d["state"], "needFiles", d["needFiles"], "pullErrors", d["pullErrors"]); sys.exit(0 if d["state"]=="idle" and d["needFiles"]==0 and d["pullErrors"]==0 else 1)' || bad "the peer is not idle with nothing needed"
    [ "$(cat "$(f 13-cleanup.txt)" 2>/dev/null)" = "SYS:stc-$tag removed" ] || bad "guest cleanup not confirmed"
    [ $fails -eq 0 ] && echo "PASS: stage C on AROS ($tag)" || { echo "FAILED: $fails check(s)"; exit 1; }
    ;;
*) echo "usage: $0 setup|watch|verify TAG ..." >&2; exit 2 ;;
esac
