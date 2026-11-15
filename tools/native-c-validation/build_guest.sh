#!/bin/sh
# Network-disabled wrapper around the unchanged historical SMS builders.
set -eu
case "${1:-}" in
  sms)
    sh /src/build_guest.sh
    QDC_PREFLIGHT_IMAGE=/out/lab-only-sms-rootfs.cpio.gz;;
  coexist)
    sh /sms/combined/build.sh
    QDC_PREFLIGHT_IMAGE=/out/lab-only-sms-combo-rootfs.cpio.gz;;
  *) exit 64;;
esac
gcc -std=c11 -O2 -Wall -Wextra -Werror -DQDC_ENTROPY_NO_MAIN \
  /native/tests/test_entropy_wait.c /native/entropy_wait.c -o /out/test-entropy-wait
/out/test-entropy-wait
/work/toolchain/bin/musl-gcc -std=gnu99 -Os -static -s -march=armv7-a -mfloat-abi=soft \
  -ffunction-sections -fdata-sections -Wl,--gc-sections,-z,noexecstack -Wall -Wextra -Werror \
  /native/entropy_wait.c -o /out/lab-only-entropy-wait
QDC_PREFLIGHT_ROOT=$(mktemp -d /work/native-preflight-root.XXXXXX)
trap 'rm -rf "$QDC_PREFLIGHT_ROOT"' EXIT
sha256sum "$QDC_PREFLIGHT_IMAGE" > /out/fixture-rootfs-before-preflight.sha256
cd "$QDC_PREFLIGHT_ROOT"
gzip -dc "$QDC_PREFLIGHT_IMAGE" | cpio -id --quiet --no-absolute-filenames
cp init fixtures/original-init
cp /native/init_wrapper.sh init
cp /out/lab-only-entropy-wait bin/entropy-wait
chmod 755 init fixtures/original-init bin/entropy-wait
find . -print0 | cpio --null -o --format=newc --owner=0:0 | gzip -1 > /out/native-preflight-rootfs.tmp
mv /out/native-preflight-rootfs.tmp "$QDC_PREFLIGHT_IMAGE"
