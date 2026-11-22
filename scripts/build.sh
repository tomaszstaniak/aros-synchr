#!/bin/bash
# Build a program for the host or for the AROS target.
#
#   scripts/build.sh host|aros [stpoc|gui|tlsprobe|holdopen|tcpburst|dirbench|stgui]     (default stpoc)
#
# stpoc is the BEP client (src/main.c); tlsprobe the TLS probe
# (experiments/tls-probe). The AROS build needs scripts/build-openssl.sh
# first. Output: build/<target>/<program>
set -e
. "$(dirname "$0")/env.sh"

PROG=${2:-stpoc}
case "$PROG" in
stpoc)
    WRAP=""
    SRC="$ST_ROOT/src/devid.c $ST_ROOT/src/net.c $ST_ROOT/src/tls.c $ST_ROOT/src/pb.c
 $ST_ROOT/src/bep.c $ST_ROOT/src/lz4blk.c $ST_ROOT/src/frame.c $ST_ROOT/src/session.c
 $ST_ROOT/src/store.c $ST_ROOT/src/fetch.c $ST_ROOT/src/state.c $ST_ROOT/src/stfile.c $ST_ROOT/src/idxprog.c $ST_ROOT/src/prof.c
 $ST_ROOT/src/vv.c $ST_ROOT/src/finfo.c $ST_ROOT/src/decide.c $ST_ROOT/src/db.c $ST_ROOT/src/scan.c
 $ST_ROOT/src/tw.c $ST_ROOT/src/engine.c $ST_ROOT/src/link.c $ST_ROOT/src/conf.c $ST_ROOT/src/ctl.c $ST_ROOT/src/ctlport.c $ST_ROOT/src/disco.c $ST_ROOT/src/relay.c $ST_ROOT/src/https.c $ST_ROOT/src/reach.c
 $ST_ROOT/src/main.c" ;;
tlsprobe)
    # The probe counts and can fail the jitterentropy calls (tlsprobe.c).
    WRAP="-Wl,--wrap=jent_entropy_init_ex -Wl,--wrap=jent_read_entropy"
    SRC="$ST_ROOT/src/devid.c $ST_ROOT/src/net.c $ST_ROOT/src/tls.c
 $ST_ROOT/experiments/tls-probe/tlsprobe.c" ;;
holdopen)
    # Test helper for tests/aros/dataprotect.scr; no OpenSSL.
    WRAP=""
    SRC="$ST_ROOT/tests/aros/holdopen.c" ;;
tcpburst)
    # Reproducer for a TCP receive stall (tests/aros/tcpburst.c); no OpenSSL.
    WRAP=""
    SRC="$ST_ROOT/tests/aros/tcpburst.c" ;;
dirbench)
    # Directory lookup timing (tests/aros/dirbench.c); no OpenSSL.
    WRAP=""
    SRC="$ST_ROOT/tests/aros/dirbench.c" ;;
gui)
    # The Zune window (gui/stgui.c): AROS only, no OpenSSL; output stgui-app.
    WRAP=""
    SRC="$ST_ROOT/gui/stgui.c $ST_ROOT/src/conf.c $ST_ROOT/src/devid.c" ;;
stgui)
    # Zune preview (experiments/zune-preview); AROS only, no OpenSSL.
    WRAP=""
    SRC="$ST_ROOT/experiments/zune-preview/stgui.c" ;;
*) echo "unknown program $PROG" >&2; exit 2 ;;
esac
WARN="-Wall -Wextra -Werror -Werror=implicit-function-declaration"
# __FILE__ (used by OpenSSL's allocation macros) must not carry the
# checkout's absolute path into the binary.
PREFIXMAP="-ffile-prefix-map=$ST_ROOT/=./"

case "${1:-}" in
host)
    st_require HOST_OPENSSL
    OUT="$ST_ROOT/build/host"
    mkdir -p "$OUT"
    cc -std=c99 -O2 $WARN $PREFIXMAP -D_POSIX_C_SOURCE=200809L -I"$HOST_OPENSSL/include" $SRC \
        -L"$HOST_OPENSSL/lib" -lssl -lcrypto -o "$OUT/$PROG"
    ;;
aros)
    st_require OPENSSL_TARBALL
    st_profile
    OSSL="$ST_DEPS/openssl"
    JENT=$(ls -d "$ST_DEPS"/jitterentropy/JitterEntropy-* 2>/dev/null | head -1)
    if [ ! -f "$OSSL/lib/libssl.a" ] || [ -z "$JENT" ]; then
        echo "OpenSSL for $ST_TARGET is not built; run scripts/build-openssl.sh first" >&2
        exit 2
    fi
    # Never link a dependency tree built from other inputs (another SDK or
    # compiler at the same path, changed patches or options).
    if [ "$(st_dep_inputs "$JENT")" != "$(cat "$OSSL/INPUTS.txt" 2>/dev/null)" ]; then
        echo "OpenSSL in $OSSL was not built from the current inputs; run scripts/build-openssl.sh" >&2
        exit 2
    fi
    OUT="$ST_BUILD"
    mkdir -p "$OUT"
    CC="${ST_CROSS}gcc"
    # Plain char is unsigned on aarch64; the code does not depend on it,
    # -fsigned-char keeps both architectures alike anyway.
    case "$ST_ARCH" in aarch64) ARCHFLAGS="-fsigned-char" ;; *) ARCHFLAGS="" ;; esac
    # -fno-pic: a PIC build gets GOT relocations that AROS refuses to load
    # (seen in aros-xterm's SSH port). -lnet: the SDK's autoinit that opens
    # bsdsocket.library as SocketBase before main().
    "$CC" --sysroot="$AROS_SDK" -std=gnu99 -O2 -fno-pic $ARCHFLAGS $WARN $PREFIXMAP \
        -D_XOPEN_SOURCE=600 -D_POSIX_C_SOURCE=200112L -D__BSD_VISIBLE=1 \
        -DST_USE_JITTER -DST_TARGET="\"$ST_TARGET\"" -I"$OSSL/include" $SRC \
        $WRAP \
        "$OSSL/lib/libssl.a" "$OSSL/lib/libcrypto.a" "$JENT/lib/libjitterentropy.a" \
        -lnet -o "$OUT/$PROG"
    # collect-aros does not fail on every undefined symbol (see the scar on
    # paths with spaces), so check the result ourselves.
    undef=$("${ST_CROSS}nm" -u "$OUT/$PROG")
    if [ -n "$undef" ]; then
        echo "undefined symbols in $OUT/$PROG:" >&2
        echo "$undef" >&2
        exit 1
    fi
    if "${ST_CROSS}objdump" -r "$OUT/$PROG" | grep -q -E 'R_X86_64_(GOTPCREL|REX_GOTPCRELX|GOTPCRELX)|R_AARCH64_(ADR_GOT_PAGE|LD64_GOT)'; then
        echo "GOT relocations in $OUT/$PROG; AROS will not load it" >&2
        exit 1
    fi
    ;;
*)
    echo "usage: $0 host|aros [stpoc|tlsprobe]" >&2
    exit 2
    ;;
esac
ls -l "$OUT/$PROG"
shasum -a 256 "$OUT/$PROG"
