#!/bin/bash
# The persistent-state scenarios on the host, against the running test peer:
#   1 first run downloads the folder        4a a file deleted here is downloaded again
#   2 restart: zero requests, zero bytes    4b a file changed here (same size) is kept,
#   3 one file changed on the peer: only        not overwritten, not announced
#     that file is requested                5 state write fails: the file stays; the next
#   6 state file damaged or deleted:          run adopts it by hash, no download
#     identical files adopted, no download
#
#   scripts/test-state-host.sh DEVICE-ID IDENTITY-DIR
#
# Uses its own folder "aros-state" (var/st-state, created here) so the demo
# folder is not touched. Each run has a real-time limit. Exit 0 if all pass.
# ST_STATE_SRC / ST_STATE_OUT relocate the folder and logs; ST_PEER_PORT
# selects the peer's port (an isolated peer, see st-peer.sh).
set -e
. "$(dirname "$0")/env.sh"
dev=${1:?device id}
id=${2:?identity dir}
P="$ST_ROOT/scripts/st-peer.sh"
peer=$("$P" id)
SRC=${ST_STATE_SRC:-$ST_ROOT/var/st-state}
OUT=${ST_STATE_OUT:-$ST_ROOT/var/state-test}
PORT=${ST_PEER_PORT:-22100}
DEST="$OUT/dest"
export ST_PEER_FOLDER_ID=aros-state ST_PEER_FOLDER_LABEL="AROS state test" ST_PEER_FOLDER_DIR="$SRC"

rm -rf "$OUT" "$SRC"
# .stfolder is Syncthing's folder marker; without it the peer refuses the
# folder ("folder marker missing").
mkdir -p "$DEST" "$SRC/Documents" "$SRC/Data" "$SRC/.stfolder"
printf 'state test readme\n' > "$SRC/ReadMe.txt"
printf 'notes, version 1\n' > "$SRC/Documents/Notes.txt"
: > "$SRC/Documents/Empty.txt"
python3 -c 'import random,sys; r=random.Random(7); sys.stdout.buffer.write(bytes(r.getrandbits(8) for _ in range(600000)))' > "$SRC/Data/Blob.bin"
"$P" share "$dev" state-test > /dev/null
"$P" scan > /dev/null
sleep 3
# A state file left from an earlier run of this script would describe old files.
rm -f "$id"/state-*.txt "$id"/state-*.txt.old "$id"/state-*.txt.tmp 2>/dev/null || true

fails=0
run() { # tag [extra args]
    local tag=$1; shift
    perl -e 'alarm 120; exec @ARGV' "$ST_BUILD/../host/stpoc" "$OUT/$tag.log" sync "$id" 127.0.0.1 "$PORT" "$peer" \
        aros-state "$DEST" 60 0 "$@" > /dev/null || true
    sum=$(grep 'sync: summary' "$OUT/$tag.log" | tail -1)
    req=$(echo "$sum" | sed -E 's/.* ([0-9]+) request\(s\).*/\1/')
    bytes=$(echo "$sum" | sed -E 's/.* ([0-9]+) bytes received.*/\1/')
    result=$(grep 'RESULT' "$OUT/$tag.log" | tail -1 | sed -E 's/.*RESULT //')
}
check() { # description condition...
    local what=$1; shift
    if "$@"; then echo "PASS  $what"; else echo "FAIL  $what (requests=$req bytes=$bytes result=$result)"; fails=$((fails + 1)); fi
}
same_files() { diff <(cd "$SRC" && find . -type f ! -path './.stfolder*' -exec shasum -a 256 {} \; | sort -k2) \
                    <(cd "$DEST" && find . -type f -exec shasum -a 256 {} \; | sort -k2) > /dev/null; }
eq() { [ "$1" = "$2" ]; }
requested() { grep -c "fetch: \"$1\": " "$OUT/$2.log" || true; }

run s1
check "1 first run: whole folder downloaded" eval '[ "$req" -gt 0 ] && same_files && [ "$result" = PASS ]'

run s2
check "2 restart without changes: 0 requests, 0 bytes" eval '[ "$req" = 0 ] && [ "$bytes" = 0 ] && [ "$result" = PASS ]'

mt_blob=$(stat -f %m "$DEST/Data/Blob.bin")
printf 'notes, version 2 - changed on the peer\n' > "$SRC/Documents/Notes.txt"
"$P" scan > /dev/null; sleep 3
run s3
check "3 one file changed on the peer: only that file requested" eval \
    '[ "$(requested Documents/Notes.txt s3)" = 1 ] && [ "$req" = 1 ] && same_files && [ "$(stat -f %m "$DEST/Data/Blob.bin")" = "$mt_blob" ]'

rm "$DEST/ReadMe.txt"
run s4a
check "4a file deleted here: downloaded again" eval '[ "$(requested ReadMe.txt s4a)" = 1 ] && [ "$req" = 1 ] && same_files'

# Same size, one byte different: only the hashes can tell.
python3 - "$DEST/Data/Blob.bin" <<'EOF'
import sys
p = sys.argv[1]; b = bytearray(open(p, "rb").read()); b[300000] ^= 0xff; open(p, "wb").write(b)
EOF
local_hash=$(shasum -a 256 "$DEST/Data/Blob.bin" | cut -c1-64)
run s4b
check "4b file changed here (same size): kept, not overwritten, conflict reported" eval \
    '[ "$req" = 0 ] && [ "$result" = "FAIL LOCAL_CONFLICTS" ] && [ "$(shasum -a 256 "$DEST/Data/Blob.bin" | cut -c1-64)" = "$local_hash" ] && grep -q "CONFLICT \"Data/Blob.bin\"" "$OUT/s4b.log"'
# Not announced: marked as changed at start, the full Index carries exactly
# the verified entries, and no IndexUpdate announces it later.
verified=$(sed -nE 's/.*state: [0-9]+ recorded, ([0-9]+) verified.*/\1/p' "$OUT/s4b.log")
indexed=$(sed -nE 's/.*sent Index announcing ([0-9]+) entr.*/\1/p' "$OUT/s4b.log")
check "4b the changed file is not announced ($indexed in the Index = $verified verified)" eval \
    'grep -q "\"Data/Blob.bin\" was changed here" "$OUT/s4b.log" && [ -n "$indexed" ] && [ "$indexed" = "$verified" ] && ! grep -q "announced \"Data/Blob.bin\"" "$OUT/s4b.log"'
sleep 2
need=$("$P" completion "$dev" | python3 -c 'import json,sys; print(json.load(sys.stdin)["needItems"])')
check "4b the peer sees the item as still needed (needItems $need)" eval '[ "$need" -ge 1 ]'
rm "$DEST/Data/Blob.bin"   # the user resolves the conflict by deleting the local copy

printf 'notes, version 3 - changed again\n' > "$SRC/Documents/Notes.txt"
"$P" scan > /dev/null; sleep 3
run s5 0 1
check "5 state write fails: file written and kept, failure reported" eval \
    '[ "$result" = "FAIL STATE_NOT_SAVED" ] && same_files'
run s5b
check "5 next run: adopted by hash, nothing downloaded, nothing false announced" eval \
    '[ "$req" = 0 ] && [ "$result" = PASS ] && grep -q "matches the peer.s version; adopted" "$OUT/s5b.log"'

statefile=$(ls "$id"/state-*.txt)
printf 'garbage' >> "$statefile"
run s6
check "6a damaged state: rejected, files adopted by hash, 0 requests" eval \
    '[ "$req" = 0 ] && [ "$result" = PASS ] && grep -q "state: rejected" "$OUT/s6.log"'
rm -f "$statefile"
run s6b
check "6b deleted state: files adopted by hash, 0 requests" eval \
    '[ "$req" = 0 ] && [ "$result" = PASS ] && grep -q "state: none recorded" "$OUT/s6b.log"'

echo "logs: $OUT"
[ "$fails" = 0 ] && echo "ALL PASSED" || echo "$fails FAILED"
exit $fails
