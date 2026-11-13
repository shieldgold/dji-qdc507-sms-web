#!/bin/sh
# Build on the computer only; this script contains no device access or install.
set -eu
if [ "$#" -ne 1 ]; then
    echo "usage: $0 /absolute/path/to/tun-validation-local" >&2
    exit 2
fi
LOCAL_DIR=$(cd "$1" && pwd)
SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname "$0")" && pwd)
test -f "$LOCAL_DIR/capture/config.gz"
test -f "$LOCAL_DIR/sources/quectel-kernel/Makefile"
python3 "$SCRIPT_DIR/inspect_tun.py" "$LOCAL_DIR/capture" --output "$LOCAL_DIR/inspection"
docker run --rm --network none --platform linux/amd64 \
    -v "$LOCAL_DIR:/work" \
    -v "$LOCAL_DIR/capture:/work/capture:ro" \
    -v "$LOCAL_DIR/inspection:/work/inspection:ro" \
    -v "$LOCAL_DIR/sources:/work/sources:ro" qdc507-tun-builder:local sh -ec '
    CROSS=/work/sources/gcc-linaro-4.9-2016.02-x86_64_arm-linux-gnueabi/bin/arm-linux-gnueabi-
    mkdir -p /work/build /work/output
    gzip -dc /work/capture/config.gz > /work/build/.config
    /work/sources/quectel-kernel/scripts/config --file /work/build/.config --module TUN
    export ARCH=arm CROSS_COMPILE="$CROSS"
    export KBUILD_BUILD_TIMESTAMP="2026-11-13 00:00:00 UTC"
    export KBUILD_BUILD_USER=offline-validation KBUILD_BUILD_HOST=local
    make -j4 -C /work/sources/quectel-kernel O=/work/build CC="${CROSS}gcc" \
        HOSTCFLAGS="-O2 -fcommon" olddefconfig > /work/output/configure.log 2>&1
    make -j4 -C /work/sources/quectel-kernel O=/work/build CC="${CROSS}gcc" \
        HOSTCFLAGS="-O2 -fcommon" modules_prepare > /work/output/prepare.log 2>&1
    cp /work/inspection/Module.symvers /work/build/Module.symvers
    make -j4 -C /work/sources/quectel-kernel O=/work/build CC="${CROSS}gcc" \
        HOSTCFLAGS="-O2 -fcommon" drivers/net/tun.ko > /work/output/build.log 2>&1
    cp /work/build/drivers/net/tun.ko /work/output/tun.ko
    "${CROSS}strip" --strip-debug /work/output/tun.ko
    modinfo /work/output/tun.ko > /work/output/modinfo.txt
    "${CROSS}readelf" -h /work/output/tun.ko > /work/output/elf-header.txt
    "${CROSS}gcc" --version > /work/output/compiler.txt
    '
python3 "$SCRIPT_DIR/inspect_tun.py" "$LOCAL_DIR/capture" \
    --candidate "$LOCAL_DIR/output/tun.ko" --build-config "$LOCAL_DIR/build/.config" \
    --output "$LOCAL_DIR/inspection"
