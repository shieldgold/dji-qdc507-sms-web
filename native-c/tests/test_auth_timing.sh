#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DEPS=${QDC_DEPS:-$HOME/.cache/qdc507-native-c/host/prefix}
TASK_SAN_FLAGS=""
if [ "${QDC_SANITIZE:-0}" = 1 ]; then TASK_SAN_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"; fi
WORK=$(mktemp -d "/tmp/qdc-auth-timing-XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM
${CC:-cc} $TASK_SAN_FLAGS -std=c11 -Wall -Wextra -Wpedantic -g -O1 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE \
  -Dqdc_system_ms=qdc_real_system_ms -Dqdc_mono_ms=qdc_real_mono_ms -Dqdc_save_json=qdc_real_save_json \
  -I"$ROOT/include" -I"$DEPS/include" -c "$ROOT/src/common.c" -o "$WORK/common.o"
${CC:-cc} $TASK_SAN_FLAGS -std=c11 -Wall -Wextra -Wpedantic -g -O1 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE \
  -I"$ROOT/include" -I"$DEPS/include" "$ROOT/tests/test_auth_timing.c" "$ROOT/src/auth.c" "$ROOT/src/crypto.c" "$WORK/common.o" \
  -L"$DEPS/lib" -lmbedtls -lmbedx509 -lmbedcrypto -lyyjson -pthread -lm -o "$WORK/test_auth_timing"
"$WORK/test_auth_timing" "$WORK"
