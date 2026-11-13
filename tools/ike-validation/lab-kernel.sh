#!/bin/sh
# Generic computer-only ARM VM kernel. This is not firmware for QDC507.
set -eu
mkdir -p /work/lab-source
if [ ! -d /work/lab-source/linux-3.18.44 ]; then tar -xf /inputs/linux-3.18.44.tar.xz -C /work/lab-source; fi
SRC=/work/lab-source/linux-3.18.44
OUT=/work/lab-mainline-kernel
mkdir -p "$OUT"
make -s -C "$SRC" O="$OUT" ARCH=arm vexpress_defconfig
"$SRC/scripts/config" --file "$OUT/.config" \
    --enable ARCH_VIRT --enable NET_NS --enable TUN --enable IPV6 \
    --enable PROC_PAGE_MONITOR --enable DEVTMPFS_MOUNT \
    --enable HW_RANDOM --enable HW_RANDOM_VIRTIO \
    --disable ARCH_VEXPRESS --disable MCPM --disable PM_SLEEP --disable CPU_IDLE \
    --disable XFRM --disable XFRM_USER --disable NET_KEY \
    --disable INET_ESP --disable INET_AH --disable IP_MULTIPLE_TABLES \
    --disable DEBUG_INFO --disable MODULES
make -s -C "$SRC" O="$OUT" ARCH=arm olddefconfig
make -j4 -C "$SRC" O="$OUT" ARCH=arm \
    CROSS_COMPILE=/usr/bin/arm-linux-gnueabi- \
    CC=/opt/gcc493/bin/arm-linux-gnueabi-gcc HOSTCFLAGS='-O2 -fcommon' zImage
cp "$OUT/arch/arm/boot/zImage" /work/output/lab-only-zImage
cp "$OUT/.config" /work/output/lab-kernel.config
if [ -d /export/output ]; then cp /work/output/lab-only-zImage /work/output/lab-kernel.config /export/output/; fi
