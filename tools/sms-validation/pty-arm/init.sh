#!/bin/sh
export PATH=/bin
mount -t proc proc /proc
grep -q qdc.sms.pty=1 /proc/cmdline || exit 1
set -eu
trap 'rc=$?; test "$rc" = 0 || echo SMS_PTY_INIT_FAILED code=$rc; poweroff -f' EXIT
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mkdir -p /dev/pts
mount -t devpts devpts /dev/pts
mount -t tmpfs -o mode=700 tmpfs /run
echo SMS_PTY_KERNEL release=$(cat /proc/sys/kernel/osrelease)
export GOMAXPROCS=1 GOMEMLIMIT=12MiB
if /bin/pty-tests -test.v -test.count=1 -test.timeout=20s -test.run '^Test(OpenModemCloseReleasesBlockedPTY|ModemSetupFailureReleasesPTYDescriptor)$'; then
  rc=0
else
  rc=$?
fi
echo SMS_PTY_EXIT code=$rc
echo SMS_PTY_COMPLETE
