#!/bin/bash
# Regenerates tests/compat/syncthing-cases.txt with Syncthing's own code
# (tests/compat/oracle.go). Needs Go and a Syncthing source tree:
#   SYNCTHING_SRC=/path/to/syncthing scripts/compat-fixtures.sh
# Go may download Syncthing's module dependencies into its module cache.
set -e
. "$(dirname "$0")/env.sh"
SRC="${SYNCTHING_SRC:?set SYNCTHING_SRC to a Syncthing source tree}"
VERSION=$(git -C "$SRC" describe --tags 2>/dev/null || echo unknown)
WORK="$ST_ROOT/build/compat-oracle"
rm -rf "$WORK"
mkdir -p "$WORK"
cp "$ST_ROOT/tests/compat/oracle.go" "$WORK/main.go"
cp "$SRC/go.sum" "$WORK/go.sum"
GOVER=$(sed -n 's/^go //p' "$SRC/go.mod")
cat > "$WORK/go.mod" <<MOD
module compatoracle

go $GOVER

require github.com/syncthing/syncthing v0.0.0

replace github.com/syncthing/syncthing => $SRC
MOD
(cd "$WORK" && GOFLAGS=-mod=mod SYNCTHING_VERSION="$VERSION" go run . 20261109 400) \
    > "$WORK/cases.txt"
mv "$WORK/cases.txt" "$ST_ROOT/tests/compat/syncthing-cases.txt"
rm -rf "$WORK"
echo "wrote tests/compat/syncthing-cases.txt ($(wc -l < "$ST_ROOT/tests/compat/syncthing-cases.txt") lines, Syncthing $VERSION)"
