#!/bin/sh
# Computer-only container. Source downloads and signature verification precede
# this step; run the build with --network none and no USB devices mounted.
set -eu
mkdir -p /work/build /work/output /work/logs
if [ -d /export/logs ]; then trap 'cp -a /work/logs/. /export/logs/' EXIT; fi
cd /work/build
if [ ! -f musl-installed ]; then
    tar -xf /inputs/musl-1.2.6.tar.gz
    cd musl-1.2.6
    patch -p1 < /inputs/musl-iconv-fix.patch
    patch -p1 < /inputs/musl-qsort-fixes.patch
    CC=arm-linux-gnueabi-gcc CFLAGS='-Os -march=armv7-a -mfloat-abi=soft' \
      ./configure --target=arm-linux-gnueabi --prefix=/work/toolchain --disable-shared > /work/logs/musl-configure.log 2>&1
    make -j4 > /work/logs/musl-build.log 2>&1
    make install >> /work/logs/musl-build.log 2>&1
    for h in linux asm asm-generic; do mkdir -p /work/toolchain/include/$h; cp -a /usr/arm-linux-gnueabi/include/$h/. /work/toolchain/include/$h/; done
    cd /work/build
    touch musl-installed
fi
export CC=/work/toolchain/bin/musl-gcc
export AR=arm-linux-gnueabi-ar RANLIB=arm-linux-gnueabi-ranlib STRIP=arm-linux-gnueabi-strip
export CFLAGS='-Os -march=armv7-a -mfloat-abi=soft -ffunction-sections -fdata-sections'
export LDFLAGS='-static -Wl,--gc-sections,-z,noexecstack'
export PKG_CONFIG_LIBDIR=/work/toolchain/lib/pkgconfig
if [ ! -f gmp-installed ]; then
    tar -xf /inputs/gmp-6.3.0.tar.xz
    cd gmp-6.3.0
    ./configure --host=arm-linux-gnueabi --prefix=/work/toolchain --disable-shared --enable-static --disable-cxx > /work/logs/gmp-configure.log 2>&1
    make -j4 > /work/logs/gmp-build.log 2>&1
    make install >> /work/logs/gmp-build.log 2>&1
    cd /work/build
    touch gmp-installed
fi
if [ ! -d strongswan-6.1.0 ]; then tar -xf /inputs/strongswan-6.1.0.tar.bz2; fi
cd strongswan-6.1.0
CPPFLAGS=-I/work/toolchain/include LDFLAGS="$LDFLAGS -L/work/toolchain/lib" \
./configure --host=arm-linux-gnueabi --prefix=/opt/qdc-ike \
    --sysconfdir=/run/qdc-ike --with-piddir=/run/qdc-ike \
    --with-swanctldir=/run/qdc-ike/swanctl \
    --disable-defaults --disable-shared --enable-static --enable-monolithic \
    --enable-charon --enable-ikev2 --enable-swanctl --enable-vici \
    --enable-kernel-libipsec --enable-kernel-netlink --enable-socket-default \
    --enable-aes --enable-sha1 --enable-sha2 --enable-hmac --enable-kdf \
    --enable-random --enable-nonce --enable-gmp --enable-curve25519 \
    --enable-gcm --enable-ctr --enable-drbg \
    --enable-x509 --enable-pubkey --enable-pkcs1 --enable-pkcs8 --enable-pem \
    --enable-constraints --enable-revocation --enable-eap-identity \
    --enable-eap-mschapv2 --enable-md4 --enable-des \
    > /work/logs/strongswan-configure.log 2>&1
make -j4 > /work/logs/strongswan-build.log 2>&1
# libtool's -static only prefers archive libraries. -all-static is required
# on program targets to remove PT_INTERP and all runtime loader dependencies.
rm -f src/charon/charon src/swanctl/swanctl
make -C src/charon LDFLAGS='-all-static -Wl,--gc-sections,-z,noexecstack -L/work/toolchain/lib' >> /work/logs/strongswan-build.log 2>&1
make -C src/swanctl LDFLAGS='-all-static -Wl,--gc-sections,-z,noexecstack -L/work/toolchain/lib' >> /work/logs/strongswan-build.log 2>&1
cp src/charon/charon /work/output/charon
cp src/swanctl/swanctl /work/output/swanctl
$STRIP --strip-unneeded /work/output/charon /work/output/swanctl
arm-linux-gnueabi-readelf -h -l -A /work/output/charon > /work/output/charon-elf.txt
arm-linux-gnueabi-size /work/output/charon /work/output/swanctl > /work/output/size.txt
cp config.h /work/output/strongswan-config.h
if [ -d /export/output ]; then cp -a /work/output/. /export/output/; fi
printf 'OFFLINE_BUILD_COMPLETE\n'
