#!/bin/sh
# Runs only inside the network-disabled builder, without USB.
set -eu
cd /work
test ! -e runtime-root
mkdir runtime-root
if [ ! -x /work/output/lab-only-runtime-busybox ]; then
cp -a /work/lab-tools/busybox-1.37.0 /work/runtime-busybox
cd /work/runtime-busybox
for item in READLINK RMDIR MKFIFO MKNOD UNAME; do
  /work/lab-source/linux-3.18.44/scripts/config --file .config --enable "$item"
done
yes '' | make oldconfig >/out/busybox-runtime.log 2>&1
make -j4 CC=/work/toolchain/bin/musl-gcc CROSS_COMPILE=arm-linux-gnueabi- \
    EXTRA_CFLAGS='-Os -march=armv7-a -mfloat-abi=soft' \
    EXTRA_LDFLAGS='-static -Wl,-z,noexecstack' >>/out/busybox-runtime.log 2>&1
cp busybox /work/output/lab-only-runtime-busybox
cd /work
rm -rf /work/runtime-busybox
fi
# The real module has IPV6_SIT=n. Generic vexpress enables a fallback sit0;
# match this relevant option instead of weakening the namespace allowlist.
if [ ! -e /out/lab-only-runtime-zImage ]; then
  SRC=/work/lab-source/linux-3.18.44
  KOUT=/work/lab-mainline-kernel
  cp "$KOUT/.config" /out/lab-kernel-before-runtime.config
  "$SRC/scripts/config" --file "$KOUT/.config" --disable IPV6_SIT
  make -s -C "$SRC" O="$KOUT" ARCH=arm olddefconfig >/out/kernel-runtime.log 2>&1
  make -j4 -C "$SRC" O="$KOUT" ARCH=arm \
    CROSS_COMPILE=/usr/bin/arm-linux-gnueabi- \
    CC=/opt/gcc493/bin/arm-linux-gnueabi-gcc HOSTCFLAGS='-O2 -fcommon' zImage >>/out/kernel-runtime.log 2>&1
  cp "$KOUT/arch/arm/boot/zImage" /out/lab-only-runtime-zImage
  cp "$KOUT/.config" /out/lab-only-runtime-kernel.config
fi
/work/toolchain/bin/musl-gcc -std=gnu99 -Os -static -march=armv7-a -mfloat-abi=soft \
    -Wall -Wextra -Werror -DQDC_MEM_TEST \
    /src/ike-validation/runtime/vm-fixture.c /src/ike-validation/runtime/proc.c \
    /src/ike-validation/lab_mem.c -o /out/lab-only-vm-fixture
ROOT=/work/runtime-root
mkdir -p "$ROOT/bin" "$ROOT/dev" "$ROOT/proc" "$ROOT/sys" "$ROOT/run" "$ROOT/fixtures"
cp /work/output/lab-only-runtime-busybox "$ROOT/bin/busybox"
for name in sh cat mkdir cp rm sleep grep awk sed head tail tr date printf echo test true false mount ip ping poweroff chmod kill readlink rmdir mkfifo mknod uname; do
  ln -s busybox "$ROOT/bin/$name"
done
cp /export/output/charon /export/output/swanctl "$ROOT/fixtures/"
cp /src/ike-validation/runtime/idle.conf "$ROOT/fixtures/"
cp /out/qdc-ike-guard "$ROOT/bin/"
cp /out/lab-only-vm-fixture "$ROOT/bin/vm-fixture"
cp /src/ike-validation/runtime/vm-init.sh "$ROOT/init"
chmod 755 "$ROOT/init"
mknod "$ROOT/dev/console" c 5 1
cd "$ROOT"
find . -print0 | cpio --null -o --format=newc --owner=0:0 | gzip -1 > /out/lab-only-runtime-rootfs.cpio.gz
# Only temporary computer-side VM build directories.
cd /work
rm -rf /work/runtime-root
