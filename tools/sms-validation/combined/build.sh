#!/bin/sh
# Network-disabled computer-only builder. Reads only disposable lab inputs.
set -eu
SMS_COMBO_ROOT=$(mktemp -d /work/sms-combo-root.XXXXXX)
trap 'rm -rf "$SMS_COMBO_ROOT"' EXIT
SMS_COMBO_CC=/work/toolchain/bin/musl-gcc
SMS_COMBO_FLAGS='-std=gnu99 -Os -static -s -march=armv7-a -mfloat-abi=soft -ffunction-sections -fdata-sections -Wl,--gc-sections,-z,noexecstack -Wall -Wextra -Werror'
$SMS_COMBO_CC $SMS_COMBO_FLAGS /sms/fake_modem.c /sms/combined/modem.c -o /out/lab-only-combo-modem
$SMS_COMBO_CC $SMS_COMBO_FLAGS /sms/http_metadata.c /sms/combined/http.c -o /out/lab-only-combo-http
$SMS_COMBO_CC $SMS_COMBO_FLAGS /tools/ike-validation/runtime/coexist-vm-fixture.c -o /out/lab-only-combo-meta
mkdir -p "$SMS_COMBO_ROOT/bin" "$SMS_COMBO_ROOT/dev" "$SMS_COMBO_ROOT/proc" "$SMS_COMBO_ROOT/sys" "$SMS_COMBO_ROOT/run" "$SMS_COMBO_ROOT/fixtures" "$SMS_COMBO_ROOT/data/smsweb"
cp /work/output/lab-only-runtime-busybox "$SMS_COMBO_ROOT/bin/busybox"
for name in sh cat mkdir cp mv rm sleep grep awk date printf echo test mount ip ping poweroff chmod kill mknod readlink; do ln -s busybox "$SMS_COMBO_ROOT/bin/$name"; done
cp /out/lab-only-combo-modem "$SMS_COMBO_ROOT/bin/sms-combo-modem"
cp /out/lab-only-combo-http "$SMS_COMBO_ROOT/bin/sms-combo-http"
cp /out/lab-only-combo-meta "$SMS_COMBO_ROOT/bin/lab-fixture"
cp /input/smsweb "$SMS_COMBO_ROOT/data/smsweb/smsweb"
cp /input/guard "$SMS_COMBO_ROOT/bin/qdc-ike-guard"
cp /ike/charon /ike/swanctl "$SMS_COMBO_ROOT/fixtures/"
cp /tools/ike-validation/runtime/real.conf "$SMS_COMBO_ROOT/fixtures/"
for name in ca.pem client.pem client.key server.pem server.key; do cp /pki/$name "$SMS_COMBO_ROOT/fixtures/"; done
cp /sms/combined/init.sh "$SMS_COMBO_ROOT/init"
chmod 755 "$SMS_COMBO_ROOT/init" "$SMS_COMBO_ROOT/data/smsweb/smsweb"
mknod "$SMS_COMBO_ROOT/dev/console" c 5 1
cd "$SMS_COMBO_ROOT"
find . -print0 | cpio --null -o --format=newc --owner=0:0 | gzip -1 > /out/lab-only-sms-combo-rootfs.cpio.gz
