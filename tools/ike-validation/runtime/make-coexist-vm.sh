#!/bin/sh
# Computer-only, network-disabled builder. Never copy this fixture to hardware.
set -eu
ROOT=/work/coexist-runtime-root
test ! -e "$ROOT"
/work/toolchain/bin/musl-gcc -std=gnu99 -Os -static -s -march=armv7-a -mfloat-abi=soft \
  -ffunction-sections -fdata-sections -Wl,--gc-sections,-z,noexecstack -Wall -Wextra -Werror \
  /src/ike-validation/runtime/coexist-vm-fixture.c -o /out/lab-only-coexist-fixture
mkdir -p "$ROOT/bin" "$ROOT/dev" "$ROOT/proc" "$ROOT/sys" "$ROOT/run" "$ROOT/fixtures" "$ROOT/data/smsweb"
cp /work/output/lab-only-runtime-busybox "$ROOT/bin/busybox"
for name in sh cat mkdir cp mv rm sleep grep awk date printf echo test mount ip ping poweroff chmod kill mknod readlink; do
  ln -s busybox "$ROOT/bin/$name"
done
cp /export/output/charon /export/output/swanctl "$ROOT/fixtures/"
cp /src/ike-validation/runtime/real.conf "$ROOT/fixtures/"
cp /out/qdc-ike-guard "$ROOT/bin/"
cp /out/lab-only-coexist-fixture "$ROOT/bin/lab-fixture"
cp /out/lab-only-coexist-fixture "$ROOT/data/smsweb/smsweb"
cp /src/ike-validation/runtime/coexist-vm-init.sh "$ROOT/init"
chmod 755 "$ROOT/init"
for name in ca.pem client.pem client.key server.pem server.key; do
  cp /export/lab-fixtures/$name "$ROOT/fixtures/"
done
mknod "$ROOT/dev/console" c 5 1
cd "$ROOT"
find . -print0 | cpio --null -o --format=newc --owner=0:0 | gzip -1 > /out/lab-only-coexist-rootfs.cpio.gz
cd /work
rm -rf "$ROOT"
