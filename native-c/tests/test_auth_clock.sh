#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
DEPS=${QDC_DEPS:-$HOME/.cache/qdc507-native-c/host/prefix}
TASK_SAN_FLAGS=""
if [ "${QDC_SANITIZE:-0}" = 1 ]; then TASK_SAN_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"; fi
WORK=$(mktemp -d "/tmp/qdc-clock-test-XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM
${CC:-cc} $TASK_SAN_FLAGS -std=c11 -Wall -Wextra -Wpedantic -g -O1 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE \
  -I"$ROOT/include" -I"$DEPS/include" "$ROOT/tests/test_auth_clock.c" "$ROOT/src/clock.c" "$ROOT/src/common.c" \
  -L"$DEPS/lib" -lyyjson -pthread -lm -o "$WORK/test_clock"
"$WORK/test_clock"
