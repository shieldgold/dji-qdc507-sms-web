#!/bin/sh
export PATH=/bin
mount -t proc proc /proc
grep -q 'qdc.sms.swap.lab=1' /proc/cmdline || exit 77
set -eu
trap 'rc=$?; test "$rc" = 0 || echo SMS_SWAP_VM_FAILED rc=$rc; poweroff -f' EXIT
mount -t devtmpfs devtmpfs /dev
mount -t tmpfs -o mode=755 tmpfs /run
mount -t tmpfs -o mode=755,size=24m tmpfs /data
/bin/swap-vm-tests
