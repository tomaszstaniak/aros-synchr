#!/bin/bash
# Waits until DEVICE-ID is connected to the test peer, gives the initial
# index a moment to go out, then changes sub/nested.txt in the test folder
# and asks the peer to rescan, so the client sees an IndexUpdate.
#
#   scripts/peer-change-after-connect.sh DEVICE-ID [WAIT-SECONDS]
set -e
. "$(dirname "$0")/env.sh"
id=${1:?device id}
limit=${2:-180}
start=$(date +%s)
until "$ST_ROOT/scripts/st-peer.sh" connected "$id" 2>/dev/null; do
    if [ $(( $(date +%s) - start )) -ge "$limit" ]; then
        echo "device did not connect within $limit s" >&2
        exit 1
    fi
    sleep 1
done
echo "connected after $(( $(date +%s) - start )) s"
# The peer sends its index right after the ClusterConfig exchange; the
# change must come after it to arrive as an IndexUpdate.
sleep 5
printf 'nested v2, changed %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "${ST_PEER_FOLDER_DIR:-$ST_ROOT/var/st-folder}/sub/nested.txt"
"$ST_ROOT/scripts/st-peer.sh" scan
echo "changed sub/nested.txt and requested a scan at $(date -u +%H:%M:%S)"
