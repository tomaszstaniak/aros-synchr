#!/bin/bash
# Builds the stage directory for the AROS demonstration: stpoc, the AROS
# script run-demo and, when LIMPET_ZIP is set, the Limpet terminal.
#
#   scripts/make-demo-stage.sh TAG STAGE-DIR
#
# run-demo receives the demo folder (aros-demo) into SYS:Syncthing-demo with
# --brief console output; the full log goes to RESULTS:demo-TAG.log. The
# peer's Device ID is written into the script so it is not typed on screen.
set -e
. "$(dirname "$0")/env.sh"
tag=${1:?tag}
stage=${2:?stage dir}
[ -f "$ST_BUILD/stpoc" ] || { echo "build stpoc first (scripts/build.sh aros)" >&2; exit 2; }
peer_id=$("$ST_ROOT/scripts/st-peer.sh" id)

rm -rf "$stage"
mkdir -p "$stage"
cp "$ST_BUILD/stpoc" "$stage/"
if [ -n "${LIMPET_ZIP:-}" ]; then
    (cd "$stage" && unzip -q "$LIMPET_ZIP")
fi
cat > "$stage/run-demo" <<EOF
; aros-syncthing demonstration: receive the "AROS demo" folder from Syncthing.
; Full log: RESULTS:demo-$tag.log. Files stay in SYS:Syncthing-demo.
FailAt 1000
Stack 262144
If NOT EXISTS SYS:Syncthing-demo
  MakeDir SYS:Syncthing-demo
EndIf
RAM:stp/stpoc --brief RESULTS:demo-$tag.log sync SYS:st-poc/id 10.0.2.2 22100 $peer_id aros-demo SYS:Syncthing-demo 300
EOF
(cd "$stage" && shasum -a 256 stpoc run-demo)
