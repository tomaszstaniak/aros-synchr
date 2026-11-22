#!/bin/bash
# Host unit tests (Device ID, LZ4 block decoder, protobuf/BEP codecs,
# local discovery announcements, relay messages, HTTPS/JSON helpers,
# framing, the destination store, version vectors against Syncthing's own
# cases, the decision table, state format 2, the two-way engine), built with AddressSanitizer and UBSan. Needs HOST_LZ4 (liblz4,
# used only as the reference compressor in tests).
set -e
. "$(dirname "$0")/env.sh"
st_require HOST_LZ4 HOST_OPENSSL

OUT="$ST_ROOT/build/host/tests"
mkdir -p "$OUT"
CFLAGS="-std=c99 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all"
S="$ST_ROOT/src"
T="$ST_ROOT/tests"

cc $CFLAGS "$T/test_devid.c" "$S/devid.c" -o "$OUT/test_devid"
cc $CFLAGS "$T/test_idxprog.c" "$S/idxprog.c" -o "$OUT/test_idxprog"
cc $CFLAGS -I"$HOST_LZ4/include" "$T/test_lz4blk.c" "$S/lz4blk.c" \
    -L"$HOST_LZ4/lib" -llz4 -o "$OUT/test_lz4blk"
cc $CFLAGS -I"$HOST_LZ4/include" "$T/test_bep.c" "$S/bep.c" "$S/pb.c" "$S/frame.c" \
    "$S/lz4blk.c" "$S/devid.c" -L"$HOST_LZ4/lib" -llz4 -o "$OUT/test_bep"

cc $CFLAGS "$T/test_conf.c" "$S/conf.c" "$S/devid.c" -o "$OUT/test_conf"
cc $CFLAGS "$T/test_disco.c" "$S/disco.c" "$S/net.c" "$S/pb.c" -o "$OUT/test_disco"
cc $CFLAGS -D_POSIX_C_SOURCE=200809L -I"$HOST_OPENSSL/include" "$T/test_relay.c" "$S/relay.c" "$S/https.c" \
    "$S/tls.c" "$S/net.c" "$S/devid.c" -L"$HOST_OPENSSL/lib" -lssl -lcrypto -o "$OUT/test_relay"
cc $CFLAGS -D_POSIX_C_SOURCE=200809L -DSTORE_TEST_HOOKS "$T/test_store.c" "$S/store.c" -o "$OUT/test_store"
cc $CFLAGS -D_POSIX_C_SOURCE=200809L -I"$HOST_OPENSSL/include" "$T/test_state.c" "$S/state.c" "$S/stfile.c" "$S/pb.c" \
    "$S/devid.c" -L"$HOST_OPENSSL/lib" -lcrypto -o "$OUT/test_state"
cc $CFLAGS -D_POSIX_C_SOURCE=200809L -DFETCH_TEST_HOOKS -I"$HOST_OPENSSL/include" "$T/test_fetch.c" "$S/fetch.c" "$S/state.c" "$S/stfile.c" \
    "$S/store.c" "$S/bep.c" "$S/pb.c" "$S/devid.c" "$S/tls.c" "$S/net.c" "$S/prof.c" \
    -L"$HOST_OPENSSL/lib" -lssl -lcrypto -o "$OUT/test_fetch"
cc $CFLAGS -D_DEFAULT_SOURCE -I"$HOST_OPENSSL/include" "$T/test_vv.c" "$S/vv.c" "$S/finfo.c" "$S/pb.c" \
    "$S/devid.c" -L"$HOST_OPENSSL/lib" -lcrypto -o "$OUT/test_vv"
cc $CFLAGS -D_POSIX_C_SOURCE=200809L -I"$HOST_OPENSSL/include" "$T/test_decide.c" "$S/decide.c" "$S/vv.c" \
    "$S/finfo.c" "$S/pb.c" "$S/devid.c" -L"$HOST_OPENSSL/lib" -lcrypto -o "$OUT/test_decide"
cc $CFLAGS -D_POSIX_C_SOURCE=200809L -I"$HOST_OPENSSL/include" "$T/test_db.c" "$S/db.c" "$S/stfile.c" "$S/vv.c" \
    "$S/finfo.c" "$S/pb.c" "$S/devid.c" -L"$HOST_OPENSSL/lib" -lcrypto -o "$OUT/test_db"
cc $CFLAGS -D_POSIX_C_SOURCE=200809L -DTW_TEST_HOOKS -I"$HOST_OPENSSL/include" "$T/test_tw.c" "$S/tw.c" "$S/db.c" \
    "$S/stfile.c" "$S/state.c" "$S/scan.c" "$S/store.c" "$S/decide.c" "$S/vv.c" "$S/finfo.c" "$S/bep.c" "$S/pb.c" \
    "$S/devid.c" "$S/net.c" "$S/tls.c" -L"$HOST_OPENSSL/lib" -lssl -lcrypto -o "$OUT/test_tw"

fail=0
for t in test_devid test_idxprog test_lz4blk test_bep test_conf test_disco test_relay; do
    "$OUT/$t" || fail=1
done
"$OUT/test_store" "$OUT" || fail=1
"$OUT/test_state" "$OUT" || fail=1
"$OUT/test_fetch" "$OUT" || fail=1
"$OUT/test_vv" "$T/compat/syncthing-cases.txt" || fail=1
"$OUT/test_decide" || fail=1
"$OUT/test_db" "$OUT" || fail=1
"$OUT/test_tw" "$OUT" || fail=1
python3 "$ST_ROOT/tests/test_peer_isolation.py" || fail=1
python3 "$ST_ROOT/tests/test_jitter_build.py" || fail=1
python3 "$ST_ROOT/tests/test_dep_inputs.py" || fail=1
exit $fail
