#!/bin/sh
# Disposable loopback-only ARM VM. Never install on a device.
export PATH=/bin
set -eu
trap 'rc=$?; test "$rc" = 0 || echo QDC_TLS_INIT_FAILED rc=$rc; poweroff -f' EXIT
mount -t proc proc /proc
mount -t devtmpfs devtmpfs /dev
/bin/tls-fixture --prepare "$(cat /fixtures/epoch)"
/bin/entropy-wait
mkdir -p /run
mount -t tmpfs -o mode=700,size=1m tmpfs /run
ip link set lo up
/bin/tls-fixture --run
