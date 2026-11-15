#!/bin/sh
# Container-only builder. /work is read-only; rootfs lives in a private /tmp dir.
set -eu
test -f /.dockerenv
ROOT=$(mktemp -d /tmp/qdc-clock-failure-root.XXXXXX)
trap 'rm -rf "$ROOT"' EXIT
/work/toolchain/bin/musl-gcc -std=gnu99 -Os -static -s -march=armv7-a -mfloat-abi=soft \
  -ffunction-sections -fdata-sections -Wl,--gc-sections,-z,noexecstack \
  -Wall -Wextra -Werror /src/launcher.c -o /out/lab-only-clock-failure-launcher
mkdir -p "$ROOT/bin" "$ROOT/dev" "$ROOT/proc" "$ROOT/run" "$ROOT/fixtures" "$ROOT/data/smsweb"
cp /work/output/lab-only-runtime-busybox "$ROOT/bin/busybox"
for name in sh cat grep mount mkdir uname sleep awk kill date poweroff; do ln -s busybox "$ROOT/bin/$name"; done
cp /out/lab-only-clock-failure-launcher "$ROOT/bin/clock-failure-launcher"
cp /input/qdc-ike-guard "$ROOT/fixtures/qdc-ike-guard"
cp /src/init.sh "$ROOT/init"
chmod 755 "$ROOT/init" "$ROOT/fixtures/qdc-ike-guard"
mknod "$ROOT/dev/console" c 5 1
cd "$ROOT"
find . -print0 | cpio --null -o --format=newc --owner=0:0 | gzip -1 > /out/lab-only-clock-failure-rootfs.cpio.gz
