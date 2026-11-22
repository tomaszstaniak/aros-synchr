#!/bin/bash
# Creates the neutral demonstration folder var/st-demo (texts, an empty
# file, a PNG picture viewable on AROS, a 2 MiB data file of 16 blocks).
# Existing content is left alone; delete var/st-demo to recreate it.
set -e
. "$(dirname "$0")/env.sh"
D="$ST_ROOT/var/st-demo"
if [ -d "$D" ]; then
    echo "$D exists; not touching it"
    exit 0
fi
mkdir -p "$D/Documents" "$D/Pictures" "$D/Data"

cat > "$D/ReadMe.txt" <<'EOF'
This folder was sent by an unmodified Syncthing to aros-syncthing,
a proof-of-concept Syncthing client running natively on AROS.

Every file was transferred block by block over TLS 1.3, each block
checked against its SHA-256 hash, and written under its final name
only after it was complete.
EOF
cat > "$D/Documents/About this demo.txt" <<'EOF'
aros-syncthing is receive-only for now: it downloads a shared folder
and tells the other device which files it has. It does not yet keep
a local index between runs, apply deletions, or send changes back.
EOF
: > "$D/Documents/Empty file.txt"

python3 - "$D/Pictures/Gradient.png" <<'EOF'
# A 320x200 RGB gradient, written with zlib only (no imaging library).
import struct, sys, zlib
w, h = 320, 200
rows = bytearray()
for y in range(h):
    rows.append(0)
    for x in range(w):
        rows += bytes((x * 255 // (w - 1), y * 255 // (h - 1), 160))
def chunk(t, d):
    c = struct.pack(">I", len(d)) + t + d
    return c + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + chunk(b"IEND", b"")
open(sys.argv[1], "wb").write(png)
EOF

# 2 MiB of reproducible pseudo-random data: 16 blocks of 128 KiB.
python3 -c 'import random,sys; r=random.Random(20261005); sys.stdout.buffer.write(bytes(r.getrandbits(8) for _ in range(2*1024*1024)))' > "$D/Data/Sample 2 MiB.bin"

(cd "$D" && find . -type f | sort | while read -r f; do shasum -a 256 "$f"; done) > "$ST_ROOT/var/st-demo.sha256"
echo "created $D"
cat "$ST_ROOT/var/st-demo.sha256"
