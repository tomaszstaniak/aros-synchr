# Shell functions shared by the host tests against a real Syncthing peer
# (twoway-host-test.sh, stagec-host-test.sh). Expects BIN, W, LOCAL,
# PEERCTL, PEER and the ST_PEER_* variables of the caller.

session() { # NAME: one session, must PASS
    "$BIN" "$W/$1.log" twoway "$W/id" 127.0.0.1 "$ST_PEER_PORT" "$PEER" "$ST_PEER_FOLDER_ID" "$LOCAL" "${SESSION_SECONDS:-60}" \
        > /dev/null 2>&1 || { grep -E 'tw:|RESULT' "$W/$1.log" | tail -20; fail "$1: $(tail -1 "$W/$1.log")"; }
    grep -q "ERROR: AddressSanitizer\|runtime error" "$W/$1.log" && fail "$1: sanitizer report"
    echo "  $1: PASS ($(grep -o 'here [0-9]* new, [0-9]* changed' "$W/$1.log"); $(grep -o '[0-9]* file(s) written' "$W/$1.log") from the peer; $(grep -o '[0-9]* announced' "$W/$1.log"))"
}
hashes_equal() {
    local a b
    a=$(cd "$LOCAL" && find . -type f ! -path './.stfolder*' | LC_ALL=C sort | xargs shasum -a 256)
    b=$(cd "$ST_PEER_FOLDER_DIR" && find . -type f ! -path './.stfolder*' | LC_ALL=C sort | xargs shasum -a 256)
    [ "$a" = "$b" ] || { echo "local:"; echo "$a"; echo "peer:"; echo "$b"; fail "$1: contents differ"; }
}
no_conflicts() {
    [ -z "$(find "$LOCAL" "$ST_PEER_FOLDER_DIR" -name '*.sync-conflict-*')" ] || fail "$1: conflict copies exist"
}
our_sequence() { sed -n 's/^sequence //p' "$W"/id/state-*.txt; }
peer_idle() {
    for _ in $(seq 1 20); do
        "$PEERCTL" folder-status | python3 -c 'import sys,json; d=json.load(sys.stdin); sys.exit(0 if d["state"]=="idle" and d["needFiles"]==0 and d["pullErrors"]==0 else 1)' && return 0
        sleep 0.5
    done
    "$PEERCTL" folder-status
    fail "$1: the peer is not idle with nothing needed"
}
versions_equal() { # NAME FILE: the peer's global version equals our record
    local ours theirs
    ours=$(python3 - "$W"/id/state-*.txt "$2" <<'EOF'
import sys
# Our record's version vector, decoded from the hex FileInfo (field 9).
def varint(b, i):
    v = s = 0
    while True:
        c = b[i]; i += 1; v |= (c & 0x7f) << s; s += 7
        if c < 0x80: return v, i
def fields(b):
    i = 0
    while i < len(b):
        k, i = varint(b, i); f, w = k >> 3, k & 7
        if w == 0: v, i = varint(b, i); yield f, v
        elif w == 2:
            n, i = varint(b, i); yield f, b[i:i+n]; i += n
        else: raise SystemExit("unexpected wire type")
for line in open(sys.argv[1]):
    if not line.startswith("entry "): continue
    fi = bytes.fromhex(line.split()[1]); d = {}
    for f, v in fields(fi): d.setdefault(f, v)
    if d.get(1, b"").decode() != sys.argv[2]: continue
    import base64, struct
    out = []
    for f, c in fields(d.get(9, b"")):
        cd = dict(fields(c))
        # Syncthing's ShortID text: base32 of the 8 bytes, first 7 characters.
        text = base64.b32encode(struct.pack(">Q", cd.get(1, 0))).decode()[:7]
        out.append("%s:%d" % (text, cd.get(2, 0)))
    print(",".join(sorted(out)))
EOF
)
    theirs=$("$PEERCTL" file "$2" | python3 -c '
import sys, json
print(",".join(sorted(json.load(sys.stdin)["global"]["version"])))')
    [ -n "$ours" ] && [ "$ours" = "$theirs" ] || fail "$1: version of $2: ours $ours, the peer's $theirs"
    echo "  $1: $2 version $ours on both sides"
}

