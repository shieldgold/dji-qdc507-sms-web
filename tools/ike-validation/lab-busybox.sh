#!/bin/sh
# Isolated VM tools, not part of the client package.
set -eu
mkdir -p /work/lab-tools
cd /work/lab-tools
if [ ! -d busybox-1.37.0 ]; then tar -xf /inputs/busybox-1.37.0.tar.bz2; fi
cd busybox-1.37.0
make allnoconfig
CFG=/work/lab-source/linux-3.18.44/scripts/config
for item in STATIC LFS LONG_OPTS SH_IS_ASH ASH ASH_PRINTF ASH_ECHO ASH_TEST FEATURE_SH_MATH FEATURE_SH_MATH_64 \
    CAT MKDIR CP RM SLEEP GREP AWK SED HEAD TAIL FEATURE_FANCY_TAIL TR DATE FEATURE_DATE_ISOFMT \
    PRINTF ECHO TEST TRUE FALSE MOUNT FEATURE_MOUNT_FLAGS IP FEATURE_IP_ADDRESS FEATURE_IP_LINK FEATURE_IP_ROUTE \
    PING FEATURE_FANCY_PING HALT POWEROFF CHMOD KILL; do "$CFG" --file .config --enable "$item"; done
yes '' | make oldconfig
make -j4 CC=/work/toolchain/bin/musl-gcc CROSS_COMPILE=arm-linux-gnueabi- \
    EXTRA_CFLAGS='-Os -march=armv7-a -mfloat-abi=soft' \
    EXTRA_LDFLAGS='-static -Wl,-z,noexecstack'
cp busybox /work/output/lab-only-busybox
/work/toolchain/bin/musl-gcc -Os -static -march=armv7-a -mfloat-abi=soft \
    -Wall -Wextra -Werror /tools/lab_mem.c -o /work/output/lab-only-mem
if [ -d /export/output ]; then cp /work/output/lab-only-busybox /work/output/lab-only-mem /export/output/; fi
