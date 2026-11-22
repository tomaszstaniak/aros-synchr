#!/bin/bash
# Build static libcrypto/libssl for one AROS target into deps/<target>/openssl,
# with jitterentropy as the only seed source. The shared SDK is not touched.
#
# Usage: scripts/build-openssl.sh            (target from env/local.env)
#
# An installed tree is reused only when its INPUTS.txt equals the manifest
# of the current inputs (compiler, SDK contents, sources, patches, recipe,
# options; scripts/dep-inputs.py). Anything else rebuilds; the previous
# tree stays in place until the new one is complete.
set -e
. "$(dirname "$0")/env.sh"
st_require OPENSSL_TARBALL
st_profile

OPENSSL_VERSION=4.0.1
OPENSSL_SHA256=2db3f3a0d6ea4b59e1f094ace2c8cd536dffb87cdc39084c5afa1e6f7f37dd09

OSSL_TARGET=$ST_OSSL_TARGET

got=$(shasum -a 256 "$OPENSSL_TARBALL" | cut -d' ' -f1)
if [ "$got" != "$OPENSSL_SHA256" ]; then
    echo "OpenSSL tarball SHA-256 $got, expected $OPENSSL_SHA256" >&2
    exit 1
fi

SRC="$ST_DEPS/src/openssl-$OPENSSL_VERSION"
JENT="$ST_DEPS/jitterentropy"
# Installed with DESTDIR under a neutral prefix: OpenSSL compiles its
# prefix and OPENSSLDIR into the library, and a workstation path must not
# end up in an AROS binary (nor be a place a config file is looked for).
NEUTRAL_PREFIX=/aros-syncthing-openssl
PREFIX="$ST_DEPS/openssl"
LOGS="$ST_BUILD/logs"
mkdir -p "$ST_DEPS/src" "$LOGS"

case "$ST_TARGET" in
*-aros-v11) JENT_DIR_EXPECTED="" ;;
*) JENT_DIR_EXPECTED="$JENT/JitterEntropy-3.7.0-local" ;;
esac
if [ -n "$JENT_DIR_EXPECTED" ]; then
    WANT=$(st_dep_inputs "$JENT_DIR_EXPECTED")
elif [ -d "$JENT" ]; then
    WANT=$(st_dep_inputs "$(ls -d "$JENT"/JitterEntropy-* | head -1)")
else
    WANT=""
fi
if [ -f "$PREFIX/lib/libssl.a" ] && [ -f "$PREFIX/INPUTS.txt" ] && [ -n "$WANT" ] \
    && [ "$WANT" = "$(cat "$PREFIX/INPUTS.txt")" ]; then
    echo "OpenSSL in $PREFIX was built from the current inputs; reused"
    exit 0
fi
if [ -d "$PREFIX" ]; then
    echo "OpenSSL in $PREFIX does not match the current inputs; rebuilding. Differences:"
    if [ -f "$PREFIX/INPUTS.txt" ]; then
        diff <(cat "$PREFIX/INPUTS.txt") <(echo "$WANT") | sed -n 's/^[<>] /  /p' | head -20 || true
    else
        echo "  (no INPUTS.txt: built by an earlier version of this script)"
    fi
fi

# Fresh copies every time: a half-configured tree from an earlier attempt
# would silently keep its old settings.
rm -rf "$SRC" "$PREFIX.new" "$ST_DEPS/openssl-destdir"
tar -xzf "$OPENSSL_TARBALL" -C "$ST_DEPS/src"
for p in "$ST_ROOT/patches/openssl-$OPENSSL_VERSION"/*.patch; do
    patch -d "$SRC" -p1 --quiet --forward < "$p" \
        || { echo "patch failed: $p" >&2; exit 1; }
done
if [ -n "$JENT_DIR_EXPECTED" ]; then
    # v1 has no qualified prebuilt JitterEntropy package. Build its own ABI.
    bash "$ST_ROOT/scripts/build-jitterentropy.sh"
    JENT_DIR="$JENT/JitterEntropy-3.7.0-local"
    JENT_INPUT="$JENT_TARBALL"
else
    st_require JENT_PACKAGE
    case "$(basename "$JENT_PACKAGE")" in
        *.x86_64-aros-v11.zip) ;;
        *) echo "JitterEntropy package must identify x86_64-aros-v11" >&2; exit 2 ;;
    esac
    rm -rf "$JENT"
    mkdir -p "$JENT"
    unzip -q "$JENT_PACKAGE" -d "$JENT"
    JENT_DIR=$(ls -d "$JENT"/JitterEntropy-*)
    JENT_INPUT="$JENT_PACKAGE"
fi

export ST_CC="${ST_CROSS}gcc --sysroot=$AROS_SDK"
export ST_AR="${ST_CROSS}ar"
export ST_RANLIB="${ST_CROSS}ranlib"
[ -x "$ST_RANLIB" ] || ST_RANLIB="${ST_CROSS}ar s"
# The AROS headers guard POSIX declarations by these macros and, unlike
# glibc, do not derive _POSIX_C_SOURCE from _XOPEN_SOURCE: without it
# posix_memalign() is undeclared. -Werror=implicit-function-declaration
# because an undeclared function returning a pointer is silently truncated
# to int on x86_64.
export ST_CFLAGS="-O2 -D_XOPEN_SOURCE=600 -D_POSIX_C_SOURCE=200112L -D__BSD_VISIBLE=1 -DOPENSSL_NO_SECURE_MEMORY -Werror=implicit-function-declaration"

OPTIONS=$(st_openssl_options "$JENT_DIR")
WANT=$(st_dep_inputs "$JENT_DIR")

cd "$SRC"
env -i PATH="$PATH" HOME="$HOME" ST_CC="$ST_CC" ST_AR="$ST_AR" ST_RANLIB="$ST_RANLIB" ST_CFLAGS="$ST_CFLAGS" \
    ./Configure --config="$ST_ROOT/config/openssl/50-aros-syncthing.conf" "$OSSL_TARGET" $OPTIONS \
    --prefix="$NEUTRAL_PREFIX" --openssldir="$NEUTRAL_PREFIX/ssl" > "$LOGS/openssl-configure.log" 2>&1 \
    || { echo "Configure failed: $LOGS/openssl-configure.log" >&2; exit 1; }
# OpenSSL_version(OPENSSL_CFLAGS) would carry the full compile command,
# with this workstation's paths, into every binary. The build information
# is written here instead; make does not regenerate it (its only
# dependency is the generator script). The real command is in BUILD.txt.
perl util/mkbuildinf.pl "cross build for $ST_TARGET, see aros-syncthing BUILD.txt" "$OSSL_TARGET" \
    > crypto/buildinf.h
make -j8 build_libs > "$LOGS/openssl-build.log" 2>&1 \
    || { echo "build failed: $LOGS/openssl-build.log" >&2; exit 1; }
rm -rf "$ST_DEPS/openssl-destdir"
make install_dev DESTDIR="$ST_DEPS/openssl-destdir" > "$LOGS/openssl-install.log" 2>&1 \
    || { echo "install failed: $LOGS/openssl-install.log" >&2; exit 1; }

mv "$ST_DEPS/openssl-destdir$NEUTRAL_PREFIX" "$PREFIX.new"
rm -rf "$ST_DEPS/openssl-destdir"
echo "$WANT" > "$PREFIX.new/INPUTS.txt"
for lib in "$PREFIX.new/lib/libcrypto.a" "$PREFIX.new/lib/libssl.a"; do
    if strings -a "$lib" | grep -q -F -e "$ST_ROOT" -e "$AROS_SDK" -e "$HOME"; then
        echo "$lib contains a workstation path; not installed" >&2
        exit 1
    fi
done
{
    echo "openssl     $OPENSSL_VERSION, tarball SHA-256 $OPENSSL_SHA256"
    echo "target      $ST_TARGET ($OSSL_TARGET)"
    echo "compiler    $("${ST_CROSS}gcc" --version | head -1), ${ST_CROSS}gcc"
    echo "sdk         $AROS_SDK, system.h SHA-256 $(shasum -a 256 "$AROS_SDK/include/aros/system.h" | cut -c1-64)"
    echo "jitter      $(basename "$JENT_INPUT"), SHA-256 $(shasum -a 256 "$JENT_INPUT" | cut -c1-64)"
    echo "options     $(echo $OPTIONS | sed "s|$ST_ROOT/||g")"
    echo "patches     $(cd "$ST_ROOT/patches/openssl-$OPENSSL_VERSION" && shasum -a 256 *.patch | awk '{printf "%s %s; ", $2, substr($1,1,16)}')"
    echo "built       $(date -u +%Y-%m-%dT%H:%MZ)"
    echo "inputs      INPUTS.txt SHA-256 $(shasum -a 256 "$PREFIX.new/INPUTS.txt" | cut -c1-64)"
} > "$PREFIX.new/BUILD.txt"
# Only a complete build replaces the previous one.
rm -rf "$PREFIX"
mv "$PREFIX.new" "$PREFIX"
echo "OpenSSL $OPENSSL_VERSION installed in $PREFIX"
