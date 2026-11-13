#!/bin/sh
# Temporary QEMU initramfs on the computer; contains disposable lab keys.
set -eu
TAG=${1:-v1}
case "$TAG" in *[!A-Za-z0-9_-]*|'') echo 'invalid lab rootfs tag' >&2; exit 2;; esac
ROOT=/work/lab-root-$TAG
test ! -e "$ROOT"
mkdir -p "$ROOT/bin" "$ROOT/dev" "$ROOT/proc" "$ROOT/sys" "$ROOT/run" "$ROOT/tmp" "$ROOT/fixtures"
cp /work/output/lab-only-busybox "$ROOT/bin/busybox"
cp /work/output/lab-only-mem "$ROOT/bin/lab-mem"
cp /work/output/charon /work/output/swanctl "$ROOT/bin/"
for name in sh cat mkdir cp rm sleep grep awk sed head tail tr date printf echo test true false mount ip ping poweroff chmod kill; do ln -s busybox "$ROOT/bin/$name"; done
cp /tools/vm-init.sh "$ROOT/init"
chmod 755 "$ROOT/init"
cp /export/lab-fixtures/ca.pem /export/lab-fixtures/client.pem /export/lab-fixtures/client.key /export/lab-fixtures/server.pem /export/lab-fixtures/server.key "$ROOT/fixtures/"
mknod "$ROOT/dev/console" c 5 1
cd "$ROOT"
find . -print0 | cpio --null -o --format=newc --owner=0:0 | gzip -1 > /work/output/lab-only-rootfs.cpio.gz
cp /work/output/lab-only-rootfs.cpio.gz /export/output/
