#!/bin/sh
# Network-disabled computer builder. Only disposable lab identities.
set -eu
ROOT=/work/real-runtime-root
test ! -e "$ROOT"
mkdir -p "$ROOT/bin" "$ROOT/dev" "$ROOT/proc" "$ROOT/sys" "$ROOT/run" "$ROOT/fixtures"
cp /work/output/lab-only-runtime-busybox "$ROOT/bin/busybox"
for name in sh cat mkdir cp mv rm sleep grep awk date printf echo test mount ip ping poweroff chmod kill mknod; do
  ln -s busybox "$ROOT/bin/$name"
done
cp /export/output/charon /export/output/swanctl "$ROOT/fixtures/"
cp /src/ike-validation/runtime/real.conf "$ROOT/fixtures/"
cp /out/qdc-ike-guard "$ROOT/bin/"
cp /src/ike-validation/runtime/real-vm-init.sh "$ROOT/init"
chmod 755 "$ROOT/init"
for name in ca.pem client.pem client.key server.pem server.key; do
  cp /export/lab-fixtures/$name "$ROOT/fixtures/"
done
mknod "$ROOT/dev/console" c 5 1
cd "$ROOT"
find . -print0 | cpio --null -o --format=newc --owner=0:0 | gzip -1 > /out/lab-only-real-rootfs.cpio.gz
cd /work
rm -rf "$ROOT"
