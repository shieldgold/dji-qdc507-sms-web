#!/bin/sh
# Computer-only builder, invoked in a network-disabled container.
set -eu
ROOT=$(mktemp -d /work/sms-runtime-root.XXXXXX)
trap 'rm -rf "$ROOT"' EXIT
gcc -std=gnu99 -O2 -Wall -Wextra -Werror /src/fake_modem.c /src/test_fake_modem.c -o /out/test-fake-modem
/out/test-fake-modem
gcc -std=gnu99 -O2 -Wall -Wextra -Werror /src/http_metadata.c /src/test_http_metadata.c -o /out/test-http-metadata
/out/test-http-metadata
/work/toolchain/bin/musl-gcc -std=gnu99 -Os -static -s -march=armv7-a -mfloat-abi=soft \
  -ffunction-sections -fdata-sections -Wl,--gc-sections,-z,noexecstack -Wall -Wextra -Werror \
  /src/fake_modem.c /src/http_metadata.c /src/fixture.c -o /out/lab-only-sms-fixture
mkdir -p "$ROOT/bin" "$ROOT/dev" "$ROOT/proc" "$ROOT/sys" "$ROOT/run" "$ROOT/fixtures"
cp /work/output/lab-only-runtime-busybox "$ROOT/bin/busybox"
for name in sh cat mkdir grep mount ip poweroff; do ln -s busybox "$ROOT/bin/$name"; done
cp /out/lab-only-sms-fixture "$ROOT/bin/sms-lab-fixture"
cp /input/smsweb "$ROOT/fixtures/smsweb"
cp /src/init.sh "$ROOT/init"
chmod 755 "$ROOT/init" "$ROOT/fixtures/smsweb"
mknod "$ROOT/dev/console" c 5 1
cd "$ROOT"
find . -print0 | cpio --null -o --format=newc --owner=0:0 | gzip -1 > /out/lab-only-sms-rootfs.cpio.gz
