#!/bin/bash
# Build the pinned upstream source for the selected ABI; never reuse another ABI's archive.
set -eu
. "$(dirname "$0")/env.sh"
st_require JENT_TARBALL
# The source is checked before any tool runs.
EXPECTED=f5eaccc9d2977c83308651be9379f09f34348398f419e8f8b5bbd95928c777ed
GOT=$(shasum -a 256 "$JENT_TARBALL" | cut -d' ' -f1)
[ "$GOT" = "$EXPECTED" ] || { echo "JitterEntropy tarball hash mismatch" >&2; exit 1; }
st_profile
mkdir -p "$ST_DEPS"
TEMP=$(mktemp -d "$ST_DEPS/.jitter-build.XXXXXX")
trap 'rm -rf "$TEMP"' EXIT
WORK="$TEMP/jitterentropy-library-3.7.0"
NEXT="$TEMP/package"
DEST="$ST_DEPS/jitterentropy/JitterEntropy-3.7.0-local"
mkdir -p "$NEXT/include" "$NEXT/lib" "$TEMP/obj"
tar -xzf "$JENT_TARBALL" -C "$TEMP"
# The release platform header must not shadow the AROS adapter.
rm "$WORK/jitterentropy-base-user.h"
cp "$WORK/jitterentropy.h" "$ST_ROOT/config/jitterentropy/jitterentropy-base-user.h" "$NEXT/include/"
for c in "$WORK"/src/*.c; do
    # -O0 is required by jitterentropy: optimisation removes the timed work.
    "${ST_CROSS}gcc" --sysroot="$AROS_SDK" -O0 -fno-pic -std=gnu99 -fwrapv \
        -I"$NEXT/include" -I"$WORK/src" -c "$c" -o "$TEMP/obj/$(basename "$c" .c).o"
done
"${ST_CROSS}ar" rcs "$NEXT/lib/libjitterentropy.a" "$TEMP/obj"/*.o
cp "$WORK/LICENSE" "$WORK/LICENSE.bsd" "$WORK/LICENSE.gplv2" "$NEXT/"
cp "$ST_ROOT/config/jitterentropy/LICENSE.port" "$NEXT/"
{
    echo "target $ST_TARGET"
    echo "source https://github.com/smuellerDD/jitterentropy-library/releases/tag/v3.7.0"
    echo "tarball SHA256 $EXPECTED"
    echo "compiler ${ST_CROSS}gcc: $("${ST_CROSS}gcc" --version 2>/dev/null | head -1)"
    echo "sdk $AROS_SDK"
    shasum -a 256 "$AROS_SDK/include/aros/system.h" "$ST_ROOT/config/jitterentropy/jitterentropy-base-user.h"
    echo "library SHA256 $(shasum -a 256 "$NEXT/lib/libjitterentropy.a" | cut -d' ' -f1)"
} > "$NEXT/BUILD.txt"
# Publish only a complete successful build; the archive is replaced atomically.
mkdir -p "$DEST/include" "$DEST/lib"
cp "$NEXT/include/"* "$DEST/include/"
cp "$NEXT/LICENSE" "$NEXT/LICENSE.bsd" "$NEXT/LICENSE.gplv2" "$NEXT/LICENSE.port" "$DEST/"
mv "$NEXT/BUILD.txt" "$DEST/BUILD.txt"
mv "$NEXT/lib/libjitterentropy.a" "$DEST/lib/libjitterentropy.a"
echo "$DEST"
