#!/bin/sh
# VM only: prove entropy readiness before executing the byte-identical fixture.
export PATH=/bin
set -eu
trap 'rc=$?; echo QDC_ENTROPY_PREFLIGHT_FAILED rc=$rc; poweroff -f' EXIT
mount -t proc proc /proc
mount -t devtmpfs devtmpfs /dev
# The helper uses umount(2), since the historical busybox omits that applet.
/bin/entropy-wait --release-lab-mounts
exec /fixtures/original-init
