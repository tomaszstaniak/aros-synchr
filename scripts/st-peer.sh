#!/bin/bash
# A dedicated, unmodified Syncthing instance used as the test peer.
# It has its own home directory (var/st-peer), listens on loopback only and
# has discovery, relays, NAT traversal and usage reporting off, so it never
# touches the user's own Syncthing configuration or other devices.
#
#   scripts/st-peer.sh init      generate identity and configuration
#   scripts/st-peer.sh start     run in the background (log: var/st-peer/syncthing.log)
#   scripts/st-peer.sh stop      stop it and wait for the process to exit
#   scripts/st-peer.sh status    running or not
#   scripts/st-peer.sh id        print its Device ID
#   scripts/st-peer.sh reset     stop and delete var/st-peer entirely
#   scripts/st-peer.sh share ID NAME   (running) add device ID and share the
#                                test folder aros-poc-1 (var/st-folder) with it
#   scripts/st-peer.sh scan      (running) rescan the test folder now
#   scripts/st-peer.sh limit KBPS [recv]   (running) limit what the peer sends
#                                (or, with recv, takes) to KBPS KiB/s, also on
#                                the local network (0: none)
#   scripts/st-peer.sh connected ID    exit 0 if device ID is connected
#   scripts/st-peer.sh completion ID   the peer's view of how complete device
#                                ID is for the test folder (JSON)
#   scripts/st-peer.sh file NAME       (running) the peer's local and global
#                                entry for NAME in the test folder (JSON)
#   scripts/st-peer.sh folder-status   (running) the folder's state (JSON)
#   scripts/st-peer.sh api METHOD PATH [JSON]   (running) any REST call
#
# ST_PEER_FOLDER_TYPE sets the folder type on share (default sendonly: the
# receive-only tests never let the peer take changes; sendreceive for the
# two-way tests).
#
# The REST calls use this instance's own API key, read from its config.
#
# A QEMU guest with user networking reaches the listener at 10.0.2.2.
set -e
. "$(dirname "$0")/env.sh"
st_require SYNCTHING_BIN

HOME_DIR=${ST_PEER_HOME:-$ST_ROOT/var/st-peer}
case "$HOME_DIR" in /*) ;; *) echo "ST_PEER_HOME must be absolute" >&2; exit 2 ;; esac
# The test folder; ST_PEER_FOLDER_* select another one (e.g. a demo folder).
FOLDER_ID=${ST_PEER_FOLDER_ID:-aros-poc-1}
FOLDER_DIR=${ST_PEER_FOLDER_DIR:-$ST_ROOT/var/st-folder}
FOLDER_LABEL=${ST_PEER_FOLDER_LABEL:-$FOLDER_ID}
FOLDER_TYPE=${ST_PEER_FOLDER_TYPE:-sendonly}
case "$FOLDER_TYPE" in sendonly|sendreceive) ;; *) echo "ST_PEER_FOLDER_TYPE: sendonly or sendreceive" >&2; exit 2 ;; esac
LISTEN_PORT=${ST_PEER_PORT:-22100}
GUI_PORT=${ST_PEER_GUI_PORT:-8385}
PIDFILE="$HOME_DIR/syncthing.pid"
# Never inherit locations that point at another Syncthing.
unset STCONFDIR STDATADIR STHOMEDIR

api() {
    local method=$1 path=$2 data=${3:-}
    local key
    key=$(python3 -c 'import sys,xml.etree.ElementTree as E; print(E.parse(sys.argv[1]).getroot().find("gui").findtext("apikey"))' "$HOME_DIR/config.xml")
    curl -fsS -X "$method" -H "X-API-Key: $key" ${data:+-H "Content-Type: application/json" --data "$data"} \
        "http://127.0.0.1:$GUI_PORT$path"
}

running_pid() {
    [ -f "$PIDFILE" ] || return 1
    local pid
    pid=$(cat "$PIDFILE")
    ps -p "$pid" -o command= 2>/dev/null | grep -q -- "--home=$HOME_DIR" && echo "$pid"
}

case "${1:-}" in
init)
    if [ -f "$HOME_DIR/config.xml" ]; then
        echo "already initialised: $HOME_DIR"
        exit 0
    fi
    mkdir -p "$HOME_DIR"
    "$SYNCTHING_BIN" generate --home="$HOME_DIR" --no-port-probing > "$HOME_DIR/generate.log" 2>&1
    python3 - "$HOME_DIR/config.xml" "$LISTEN_PORT" "$GUI_PORT" <<'EOF'
import sys, xml.etree.ElementTree as ET
path, listen, gui = sys.argv[1], sys.argv[2], sys.argv[3]
tree = ET.parse(path); root = tree.getroot()
for f in root.findall('folder'):   # no default folder
    root.remove(f)
o = root.find('options')
def setopt(name, value):
    for e in o.findall(name):
        o.remove(e)
    e = ET.SubElement(o, name); e.text = value
setopt('listenAddress', 'tcp://127.0.0.1:' + listen)
for k, v in [('globalAnnounceEnabled', 'false'), ('localAnnounceEnabled', 'false'),
             ('relaysEnabled', 'false'), ('natEnabled', 'false'), ('startBrowser', 'false'),
             ('urAccepted', '-1'), ('crashReportingEnabled', 'false'),
             ('autoUpgradeIntervalH', '0')]:
    setopt(k, v)
g = root.find('gui')
g.find('address').text = '127.0.0.1:' + gui
tree.write(path)
EOF
    echo "initialised $HOME_DIR"
    ;;
start)
    [ -f "$HOME_DIR/config.xml" ] || { echo "run '$0 init' first" >&2; exit 2; }
    if pid=$(running_pid); then echo "already running, pid $pid"; exit 0; fi
    nohup "$SYNCTHING_BIN" serve --home="$HOME_DIR" --no-browser --no-restart \
        --no-upgrade --logfile="$HOME_DIR/syncthing.log" > "$HOME_DIR/stdout.log" 2>&1 &
    echo $! > "$PIDFILE"
    sleep 2
    pid=$(running_pid) || { echo "did not start; see $HOME_DIR/stdout.log" >&2; exit 1; }
    echo "running, pid $pid, listen tcp://127.0.0.1:$LISTEN_PORT, gui 127.0.0.1:$GUI_PORT"
    ;;
stop)
    if pid=$(running_pid); then
        kill "$pid"
        for _ in $(seq 1 50); do ps -p "$pid" > /dev/null 2>&1 || break; sleep 0.2; done
        ps -p "$pid" > /dev/null 2>&1 && { echo "pid $pid did not exit" >&2; exit 1; }
        echo "stopped"
    else
        echo "not running"
    fi
    rm -f "$PIDFILE"
    ;;
status)
    if pid=$(running_pid); then echo "running, pid $pid"; else echo "not running"; fi
    ;;
id)
    "$SYNCTHING_BIN" device-id --home="$HOME_DIR"
    ;;
share)
    [ -n "${2:-}" ] && [ -n "${3:-}" ] || { echo "usage: $0 share DEVICE-ID NAME" >&2; exit 2; }
    running_pid > /dev/null || { echo "not running" >&2; exit 2; }
    self=$("$0" id)
    if [ "$FOLDER_ID" = aros-poc-1 ]; then
        mkdir -p "$FOLDER_DIR/sub"
        # Test content: an empty file, a regular file, a file in a subdirectory.
        [ -e "$FOLDER_DIR/empty.txt" ] || : > "$FOLDER_DIR/empty.txt"
        [ -e "$FOLDER_DIR/hello.txt" ] || printf 'Hello from the Mac.\n' > "$FOLDER_DIR/hello.txt"
        [ -e "$FOLDER_DIR/sub/nested.txt" ] || printf 'nested v1\n' > "$FOLDER_DIR/sub/nested.txt"
    fi
    [ -d "$FOLDER_DIR" ] || { echo "folder directory $FOLDER_DIR does not exist" >&2; exit 2; }
    api PUT "/rest/config/devices/$2" "{\"deviceID\":\"$2\",\"name\":\"$3\",\"addresses\":[\"dynamic\"],\"compression\":\"metadata\"}"
    # send-only by default: the peer never takes changes from the PoC.
    # Add the device to the folder's existing device list, never replace it.
    existing=$(api GET "/rest/config/folders/$FOLDER_ID" 2>/dev/null || echo '{}')
    devices=$(python3 -c 'import sys,json
f=json.loads(sys.argv[1] or "{}"); ids=[d["deviceID"] for d in f.get("devices",[])]
for i in sys.argv[2:]:
    if i not in ids: ids.append(i)
print(json.dumps([{"deviceID":i} for i in ids]))' "$existing" "$self" "$2")
    api PUT "/rest/config/folders/$FOLDER_ID" "{\"id\":\"$FOLDER_ID\",\"label\":\"$FOLDER_LABEL\",\"path\":\"$FOLDER_DIR\",\"type\":\"$FOLDER_TYPE\",\"rescanIntervalS\":3600,\"fsWatcherEnabled\":false,\"devices\":$devices}"
    echo "shared $FOLDER_ID ($FOLDER_DIR, $FOLDER_TYPE) with $2"
    ;;
scan)
    api POST "/rest/db/scan?folder=$FOLDER_ID"
    echo "scan requested"
    ;;
limit)
    # Sending rate in KiB/s for every device, also on the local network; 0: no limit.
    kbps=${2:?KiB/s, 0 for none}
    what=maxSendKbps
    [ "${3:-send}" = recv ] && what=maxRecvKbps
    api PATCH /rest/config/options "{\"$what\": $kbps, \"limitBandwidthInLan\": true}" > /dev/null
    echo "${3:-send} limit $kbps KiB/s"
    ;;
connected)
    api GET /rest/system/connections | python3 -c 'import sys,json; c=json.load(sys.stdin)["connections"].get(sys.argv[1],{}); sys.exit(0 if c.get("connected") else 1)' "${2:?device id}"
    ;;
completion)
    api GET "/rest/db/completion?folder=$FOLDER_ID&device=${2:?device id}"
    echo
    ;;
file)
    api GET "/rest/db/file?folder=$FOLDER_ID&file=$(python3 -c 'import sys,urllib.parse; print(urllib.parse.quote(sys.argv[1]))' "${2:?name}")"
    echo
    ;;
folder-status)
    api GET "/rest/db/status?folder=$FOLDER_ID"
    echo
    ;;
api)
    api "${2:?method}" "${3:?path}" "${4:-}"
    echo
    ;;
reset)
    "$0" stop
    rm -rf "$HOME_DIR" "$FOLDER_DIR"
    echo "removed $HOME_DIR and $FOLDER_DIR"
    ;;
*)
    echo "usage: $0 init|start|stop|status|id|reset|share|scan|connected|completion|file|folder-status" >&2
    exit 2
    ;;
esac
