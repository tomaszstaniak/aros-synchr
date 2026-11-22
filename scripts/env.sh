# Sourced by the build scripts. One place for target, toolchain, SDK and
# dependency locations.
#
# Precedence: variables already in the environment, then local.env in the
# repository root (ignored by Git; copy local.env.example), then the
# defaults below. Relative paths (values containing a slash) in local.env
# are resolved against the repository root.

ST_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)

if [ -f "$ST_ROOT/local.env" ]; then
    # Read KEY=VALUE lines without letting them override the environment.
    while IFS='=' read -r key value; do
        case "$key" in ''|\#*) continue ;; esac
        if [ -z "${!key+x}" ]; then
            # Only path-like values (containing a slash) are made absolute.
            case "$value" in /*|'') ;; */*) value="$ST_ROOT/$value" ;; esac
            export "$key=$value"
        fi
    done < "$ST_ROOT/local.env"
fi

# Target: architecture and ABI, as used in build/<target> and deps/<target>.
: "${ST_TARGET:=x86_64-aros-v11}"
export ST_TARGET

st_require() {
    for v in "$@"; do
        if [ -z "${!v}" ]; then
            echo "missing setting $v: set it in the environment or in local.env (see local.env.example)" >&2
            exit 2
        fi
    done
}

# The AROS build profile of a target: architecture, cross tools and the
# OpenSSL Configure target. AROS_TC and AROS_SDK must belong to the same
# ABI as the target; the dependency manifest (scripts/dep-inputs.py)
# records exactly which ones were used.
st_profile() {
    st_require AROS_TC AROS_SDK
    case "$ST_TARGET" in
    x86_64-aros-v1|x86_64-aros-v11)
        ST_ARCH=x86_64
        ST_CROSS="$AROS_TC/x86_64-aros-"
        ST_OSSL_TARGET=aros-syncthing-x86_64
        ;;
    aarch64-aros-v1)
        # Native Raspberry Pi (raspi-aarch64), Clang toolchain with a
        # compiler wrapper; the wrapper's SDK is set to AROS_SDK as well.
        ST_ARCH=aarch64
        ST_CROSS="$AROS_TC/bin/aarch64-aros-"
        ST_OSSL_TARGET=aros-syncthing-aarch64
        RPI_SDK="$AROS_SDK"
        export RPI_SDK
        ;;
    *)
        echo "unknown AROS target $ST_TARGET (x86_64-aros-v1, x86_64-aros-v11, aarch64-aros-v1)" >&2
        exit 2
        ;;
    esac
    [ -x "${ST_CROSS}gcc" ] || { echo "no compiler ${ST_CROSS}gcc for $ST_TARGET" >&2; exit 2; }
    [ -d "$AROS_SDK/include" ] && [ -d "$AROS_SDK/lib" ] || { echo "AROS_SDK $AROS_SDK has no include/ and lib/" >&2; exit 2; }
    export ST_ARCH ST_CROSS ST_OSSL_TARGET
}

# The jitterentropy input of a target: the upstream tarball (built here)
# for v1 targets, an existing developer package for ABIv11.
st_jent_input() {
    case "$ST_TARGET" in
    *-aros-v11) st_require JENT_PACKAGE; echo "$JENT_PACKAGE" ;;
    *) st_require JENT_TARBALL; echo "$JENT_TARBALL" ;;
    esac
}

# OpenSSL Configure options; $1 is the jitterentropy package directory.
# no-dgram: drops DTLS, QUIC and SCTP together with the datagram BIO,
# whose sendmsg/recvmsg code does not compile against AROS headers.
# --with-rand-seed=none: OpenSSL gets no built-in seed source at all, so
# the program must select JITTER explicitly and fails if it is unusable;
# there is nothing to fall back to.
st_openssl_options() {
    echo "no-shared no-asm no-threads no-apps no-tests no-docs no-dso" \
        "no-engine no-ui-console no-ssl3 no-dgram no-comp" \
        "no-secure-memory no-async no-afalgeng no-ktls" \
        "--with-rand-seed=none enable-jitter" \
        "--with-jitter-include=$1/include --with-jitter-lib=$1/lib"
}

# The dependency manifest for the current target and inputs ($1: the
# jitterentropy package directory).
st_dep_inputs() {
    python3 "$ST_ROOT/scripts/dep-inputs.py" "$ST_ROOT" "$ST_TARGET" "${ST_CROSS}gcc" "${ST_CROSS}ar" "$AROS_SDK" \
        "$OPENSSL_TARBALL" "$(st_jent_input)" $(st_openssl_options "$1")
}

ST_DEPS="$ST_ROOT/deps/$ST_TARGET"
ST_BUILD="$ST_ROOT/build/$ST_TARGET"
export ST_DEPS ST_BUILD
