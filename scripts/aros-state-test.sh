#!/bin/bash
# Runs tests/aros/state.scr (the persistent-state scenarios) on one AROS
# pool machine against the local test peer, with a real-time limit on the
# host. Resets the peer's "aros-state" folder (var/st-state), shares it with
# the AROS device, changes Documents/Notes.txt after the guest's 2nd and 5th
# session, and waits for the "done" marker session.
#
#   AROS_VM_OWNER=<session> AROS_DEVICE_ID=<id> \
#     scripts/aros-state-test.sh MACHINE TAG EVIDENCE-DIR [LIMIT-SECONDS]
#
# Needs the AROS build of stpoc. The machine is always stopped and released.
set -e
. "$(dirname "$0")/env.sh"
st_require AROS_VM_DIR AROS_VM_OWNER SYNCTHING_BIN
machine=${1:?machine}
tag=${2:?tag}
evidence=${3:?evidence dir}
limit=${4:-1500}
aros_id=${AROS_DEVICE_ID:?set AROS_DEVICE_ID}
short=${aros_id:0:7}
P="$ST_ROOT/scripts/st-peer.sh"
peer_id=$("$P" id)
peer_log="$ST_ROOT/var/st-peer/syncthing.log"
SRC="$ST_ROOT/var/st-state"
export ST_PEER_FOLDER_ID=aros-state ST_PEER_FOLDER_LABEL="AROS state test" ST_PEER_FOLDER_DIR="$SRC"

mkdir -p "$evidence/host"
evidence=$(cd "$evidence" && pwd)
hostlog="$evidence/host/run-$tag.txt"
say() { echo "$(date -u +%H:%M:%S) $*" | tee -a "$hostlog"; }

# Peer folder in its initial state (same content as scripts/test-state-host.sh).
rm -rf "$SRC"
mkdir -p "$SRC/Documents" "$SRC/Data" "$SRC/.stfolder"
printf 'state test readme\n' > "$SRC/ReadMe.txt"
printf 'notes, version 1\n' > "$SRC/Documents/Notes.txt"
: > "$SRC/Documents/Empty.txt"
python3 -c 'import random,sys; r=random.Random(7); sys.stdout.buffer.write(bytes(r.getrandbits(8) for _ in range(600000)))' > "$SRC/Data/Blob.bin"
"$P" share "$aros_id" aros-v11 > /dev/null
"$P" scan > /dev/null
sleep 3

# Stage: stpoc and the guest script with its parameters filled in.
state_file=state-$(printf '%s' aros-state | shasum -a 256 | cut -c1-32).txt
stage="$ST_BUILD/stage-state-$tag"
rm -rf "$stage"
mkdir -p "$stage"
cp "$ST_BUILD/stpoc" "$stage/"
# Every parameter is filled in here and the .key line dropped, so the guest
# runs the script without arguments (a run with a missing .key argument
# executed nothing useful, 2026-10-05, tag p1005a).
sed -e '/^\.key /d' -e "s/<STATE>/$state_file/g" -e "s/<TAG>/$tag/g" -e "s/<PEER>/$peer_id/g" \
    "$ST_ROOT/tests/aros/state.scr" > "$stage/state.scr"
{
    echo "peer binary: $SYNCTHING_BIN"
    "$SYNCTHING_BIN" --version
    shasum -a 256 "$SYNCTHING_BIN"
    echo "staged:"
    (cd "$stage" && shasum -a 256 *)
    echo "peer folder at start:"
    (cd "$SRC" && find . -type f ! -path './.stfolder*' -exec shasum -a 256 {} \; | sort -k2)
} > "$evidence/host/versions-$tag.txt"

cd "$AROS_VM_DIR"
finish() {
    set +e
    cd "$AROS_VM_DIR"
    python3 tools/vmctl.py shot "$evidence/host/console-$tag.png" > /dev/null 2>&1
    ./vm.sh stop "$machine" > /dev/null
    tmp=$(mktemp -d)
    rmdir "$tmp"
    if ./vm.sh collect "$machine" "$tmp" > /dev/null; then
        python3 - "$tmp" "$evidence/guest" "state-$tag" <<'EOF'
import os, shutil, sys
src, dst, prefix = sys.argv[1:4]
os.makedirs(dst, exist_ok=True)
with open(os.path.join(dst, "COLLECT-MANIFEST.sha256"), "a") as m:
    for line in open(os.path.join(src, "COLLECT-MANIFEST.sha256")):
        h, path = line.rstrip("\n").split("  ", 1)
        rel = path[2:] if path.startswith("./") else path
        if rel.startswith(prefix):
            os.makedirs(os.path.dirname(os.path.join(dst, rel)) or dst, exist_ok=True)
            shutil.copy2(os.path.join(src, rel), os.path.join(dst, rel))
            m.write(line)
EOF
        rm -rf "$tmp"
        say "collected $(grep -c " state-$tag" "$evidence/guest/COLLECT-MANIFEST.sha256") file(s)"
    else
        say "collect failed"
    fi
    ./vm.sh release "$machine" > /dev/null && say "released $machine"
}
out=$(./vm.sh acquire --abi v11 --project aros-syncthing --machine "$machine")
export AROS_VM_RESERVATION=$(echo "$out" | python3 -c 'import json,sys; print(json.load(sys.stdin)["reservation"])')
trap finish EXIT
say "acquired $machine, reservation $AROS_VM_RESERVATION"

./vm.sh stage "$machine" "$stage" > /dev/null
eval "$(./vm.sh env "$machine")"
./vm.sh start "$machine" > /dev/null
say "started; waiting for the desktop"
sleep 75
python3 tools/vmctl.py click 500 300
python3 tools/vmctl.py key meta_r-w
sleep 4
python3 tools/vmctl.py type "C:UnZip >RAM:unzip-log POOLINPUT:payload.zip -d RAM:stp" ret
sleep 12

log_start=$(wc -c < "$peer_log")
new_log() { tail -c +"$((log_start + 1))" "$peer_log"; }
closes() { new_log | grep -c "Connection closed (device=$short" || true; }
python3 tools/vmctl.py type "Execute RAM:stp/state.scr" ret
start=$(date +%s)
say "guest script started; real-time limit $limit s"

done1=0
done2=0
while :; do
    now=$(date +%s)
    if [ $((now - start)) -ge "$limit" ]; then
        say "REAL-TIME LIMIT reached; stopping the machine"
        exit 1
    fi
    c=$(closes)
    if [ $done1 -eq 0 ] && [ "$c" -ge 2 ]; then
        printf 'notes, version 2 - changed on the peer\n' > "$SRC/Documents/Notes.txt"
        "$P" scan > /dev/null
        say "after session $c: Notes.txt v2 on the peer, SHA-256 $(shasum -a 256 "$SRC/Documents/Notes.txt" | cut -c1-64)"
        done1=1
    fi
    if [ $done2 -eq 0 ] && [ "$c" -ge 5 ]; then
        printf 'notes, version 3 - changed again\n' > "$SRC/Documents/Notes.txt"
        "$P" scan > /dev/null
        say "after session $c: Notes.txt v3 on the peer, SHA-256 $(shasum -a 256 "$SRC/Documents/Notes.txt" | cut -c1-64)"
        done2=1
    fi
    if new_log | grep -q "remote.name=done-$tag"; then
        say "done marker seen after $((now - start)) s"
        (cd "$SRC" && find . -type f ! -path './.stfolder*' -exec shasum -a 256 {} \; | sort -k2) > "$evidence/host/peer-final-$tag.sha256"
        new_log | grep "device=$short" | sed -E 's/connection=[^ ]+ //; s/connection\.[a-z]+=[^ ]+ //g' > "$evidence/host/peer-log-$tag.txt"
        sleep 10
        exit 0
    fi
    sleep 2
done
