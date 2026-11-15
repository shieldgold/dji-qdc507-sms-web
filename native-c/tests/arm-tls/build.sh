#!/bin/sh
# Invoked only in the network-disabled local builder, using pinned ARM deps.
set -eu
QDC_TLS_ROOT=$(mktemp -d /work/native-c-tls-root.XXXXXX)
trap 'rm -rf "$QDC_TLS_ROOT"' EXIT
QDC_TLS_SOURCE=/repo/native-c/tests/arm-tls
QDC_TLS_CC=/work/toolchain/bin/musl-gcc
QDC_TLS_DEPS=/work/native-c-deps-v1/prefix
test -d "$QDC_TLS_DEPS/include"
sha256sum "$QDC_TLS_DEPS"/lib/*.a > /out/dependency-libraries.sha256
gcc -std=c11 -O2 -Wall -Wextra -Werror "$QDC_TLS_SOURCE/test_guard.c" -o /out/test-guard
/out/test-guard
gcc -std=c11 -O2 -Wall -Wextra -Werror "$QDC_TLS_SOURCE/test_tls_record.c" -o /out/test-tls-record
/out/test-tls-record
"$QDC_TLS_CC" -std=c11 -Os -static -s -march=armv7-a -mfloat-abi=soft \
  -Wall -Wextra -Werror "$QDC_TLS_SOURCE/fixture.c" -o /out/lab-only-tls-fixture
"$QDC_TLS_CC" -std=c11 -Os -static -s -march=armv7-a -mfloat-abi=soft \
  -ffunction-sections -fdata-sections -Wl,--gc-sections,-z,noexecstack \
  -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -DQDC_TESTING \
  -I/repo/native-c/include -I"$QDC_TLS_DEPS/include" -Wall -Wextra -Werror -pthread \
  /repo/native-c/tests/test_tls.c /repo/native-c/src/tls.c /repo/native-c/src/crypto.c \
  /repo/native-c/src/clock.c /repo/native-c/src/common.c \
  -L"$QDC_TLS_DEPS/lib" -lmbedtls -lmbedx509 -lmbedcrypto -lnghttp2 -lyyjson -pthread -lm -o /out/test_tls
"$QDC_TLS_CC" -std=c11 -Os -static -s -march=armv7-a -mfloat-abi=soft \
  -ffunction-sections -fdata-sections -Wl,--gc-sections,-z,noexecstack \
  -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
  -I/repo/native-c/include -I"$QDC_TLS_DEPS/include" -Wall -Wextra -Werror -pthread \
  "$QDC_TLS_SOURCE/peer.c" /repo/native-c/src/crypto.c /repo/native-c/src/common.c \
  -L"$QDC_TLS_DEPS/lib" -lmbedtls -lmbedx509 -lmbedcrypto -lnghttp2 -lyyjson -pthread -lm -o /out/lab-only-tls-peer
"$QDC_TLS_CC" -std=gnu99 -Os -static -s -march=armv7-a -mfloat-abi=soft \
  -Wall -Wextra -Werror /repo/tools/native-c-validation/entropy_wait.c -o /out/lab-only-entropy-wait
for binary in lab-only-tls-fixture lab-only-tls-peer test_tls lab-only-entropy-wait; do
  arm-linux-gnueabi-readelf -h -l "/out/$binary" > "/out/$binary.elf.txt"
done
mkdir -p "$QDC_TLS_ROOT/bin" "$QDC_TLS_ROOT/dev" "$QDC_TLS_ROOT/proc" "$QDC_TLS_ROOT/run" "$QDC_TLS_ROOT/fixtures"
cp /work/output/lab-only-runtime-busybox "$QDC_TLS_ROOT/bin/busybox"
for name in sh cat mkdir mount ip poweroff; do ln -s busybox "$QDC_TLS_ROOT/bin/$name"; done
cp /out/lab-only-tls-fixture "$QDC_TLS_ROOT/bin/tls-fixture"
cp /out/lab-only-tls-peer "$QDC_TLS_ROOT/bin/tls-peer"
cp /out/lab-only-entropy-wait "$QDC_TLS_ROOT/bin/entropy-wait"
cp /out/test_tls "$QDC_TLS_ROOT/fixtures/test_tls"
for name in ca.pem wrong.pem server.pem server.key epoch; do cp "/certs/$name" "$QDC_TLS_ROOT/fixtures/$name"; done
cp "$QDC_TLS_SOURCE/init.sh" "$QDC_TLS_ROOT/init"
chmod 755 "$QDC_TLS_ROOT/init"
chmod 600 "$QDC_TLS_ROOT/fixtures/server.key"
mknod "$QDC_TLS_ROOT/dev/console" c 5 1
cd "$QDC_TLS_ROOT"
find . -print0 | cpio --null -o --format=newc --owner=0:0 | gzip -1 > /out/lab-only-tls-rootfs.cpio.gz
