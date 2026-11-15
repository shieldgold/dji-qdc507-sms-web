#!/bin/sh
# Disposable computer guest; no network or host devices.
set -eu
SMS_PTY_ROOT=$(mktemp -d /work/sms-pty-root.XXXXXX)
trap 'rm -rf "$SMS_PTY_ROOT"' EXIT
mkdir -p "$SMS_PTY_ROOT/bin" "$SMS_PTY_ROOT/dev" "$SMS_PTY_ROOT/proc" "$SMS_PTY_ROOT/sys" "$SMS_PTY_ROOT/run" "$SMS_PTY_ROOT/tmp"
chmod 1777 "$SMS_PTY_ROOT/tmp"
cp /work/output/lab-only-runtime-busybox "$SMS_PTY_ROOT/bin/busybox"
for name in sh cat mkdir grep mount poweroff; do ln -s busybox "$SMS_PTY_ROOT/bin/$name"; done
cp /input/pty-tests "$SMS_PTY_ROOT/bin/pty-tests"
cp /src/init.sh "$SMS_PTY_ROOT/init"
chmod 755 "$SMS_PTY_ROOT/bin/pty-tests" "$SMS_PTY_ROOT/init"
mknod "$SMS_PTY_ROOT/dev/console" c 5 1
cd "$SMS_PTY_ROOT"
find . -print0 | cpio --null -o --format=newc --owner=0:0 | gzip -1 > /out/lab-only-pty-rootfs.cpio.gz
