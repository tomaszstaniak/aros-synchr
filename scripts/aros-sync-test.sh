#!/bin/bash
# Runs tests/aros/sync.scr on one AROS pool machine against the local test
# peer, with a real-time limit enforced from the host (guest time is not
# reliable under emulation).
#
#   scripts/aros-sync-test.sh MACHINE TAG STAGE-DIR EVIDENCE-DIR [LIMIT-SECONDS]
#
# STAGE-DIR holds stpoc and sync.scr. AROS_VM_OWNER must be set (a session
# identity, see the pool guide) and AROS_VM_DIR must point at the pool's
# vm.sh directory. The peer must be running with the folder shared with the
# AROS device. The machine is always stopped and released at the end; the
# guest's results land in EVIDENCE-DIR/guest, host-side records in
# EVIDENCE-DIR/host.
set -e
. "$(dirname "$0")/env.sh"
st_require AROS_VM_DIR AROS_VM_OWNER SYNCTHING_BIN

machine=${1:?machine}
tag=${2:?tag}
stage=$(cd "${3:?stage dir}" && pwd)
evidence=${4:?evidence dir}
limit=${5:-1200}
aros_id=${AROS_DEVICE_ID:?set AROS_DEVICE_ID to the AROS client Device ID}
peer_id=$("$ST_ROOT/scripts/st-peer.sh" id)
peer_log="$ST_ROOT/var/st-peer/syncthing.log"
short=${aros_id:0:7}

mkdir -p "$evidence/host"
evidence=$(cd "$evidence" && pwd) # the script changes directory below
hostlog="$evidence/host/run-$tag.txt"
say() { echo "$(date -u +%H:%M:%S) $*" | tee -a "$hostlog"; }

# What actually ran: the peer binary and the staged files.
{
    echo "peer binary: $SYNCTHING_BIN"
    "$SYNCTHING_BIN" --version
    shasum -a 256 "$SYNCTHING_BIN"
    echo "staged:"
    (cd "$stage" && shasum -a 256 *)
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
        mkdir -p "$evidence/guest"
        grep -E " sync-$tag-" "$tmp/COLLECT-MANIFEST.sha256" >> "$evidence/guest/COLLECT-MANIFEST.sha256"
        for f in $(grep -E " sync-$tag-" "$tmp/COLLECT-MANIFEST.sha256" | awk '{print $2}'); do
            cp -p "$tmp/$f" "$evidence/guest/"
        done
        rm -rf "$tmp"
        say "collected $(grep -c " sync-$tag-" "$evidence/guest/COLLECT-MANIFEST.sha256") file(s)"
    else
        say "collect failed"
    fi
    ./vm.sh release "$machine" > /dev/null && say "released $machine"
}
out=$(./vm.sh acquire --abi v11 --project aros-syncthing --machine "$machine")
export AROS_VM_RESERVATION=$(echo "$out" | python3 -c 'import json,sys; print(json.load(sys.stdin)["reservation"])')
# From here on every exit path stops and releases the machine.
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
sleep 10
python3 tools/vmctl.py shot "$evidence/host/console-$tag-unzip.png" > /dev/null

say "peer big.bin before the run: SHA-256 $(shasum -a 256 "$ST_ROOT/var/st-folder/big.bin" | cut -c1-64)"
log_start=$(wc -c < "$peer_log")
new_log() { tail -c +"$((log_start + 1))" "$peer_log"; }
python3 tools/vmctl.py type "Execute RAM:stp/sync.scr $tag $peer_id" ret
start=$(date +%s)
say "guest script started; real-time limit $limit s"

changed=0
while :; do
    now=$(date +%s)
    if [ $((now - start)) -ge "$limit" ]; then
        say "REAL-TIME LIMIT reached; stopping the machine"
        exit 1
    fi
    if [ $changed -eq 0 ] && new_log | grep -q "Connection closed (device=$short"; then
        # Step 1 finished: give step 2 a new big.bin.
        head -c 1060921 /dev/urandom > "$ST_ROOT/var/st-folder/big.bin"
        "$ST_ROOT/scripts/st-peer.sh" scan > /dev/null
        say "step 1 done; big.bin replaced on the peer, SHA-256 $(shasum -a 256 "$ST_ROOT/var/st-folder/big.bin" | cut -c1-64)"
        changed=1
    fi
    if new_log | grep -q "remote.name=done-$tag"; then
        say "done marker seen after $((now - start)) s"
        sleep 10
        exit 0
    fi
    sleep 2
done
