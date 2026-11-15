#!/bin/sh
# Disposable computer-only VM. Never install on hardware.
export PATH=/bin
mount -t proc proc /proc
grep -q 'qdc.sms.lab=1' /proc/cmdline || exit 1
set -eu
trap 'rc=$?; test "$rc" = 0 || echo SMS_LAB_FAILED init_rc=$rc; poweroff -f' EXIT
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mkdir -p /dev/pts /run
mount -t devpts devpts /dev/pts
mount -t tmpfs -o mode=700 tmpfs /run
ip link set lo up
scenario=
for arg in $(cat /proc/cmdline); do
  case "$arg" in qdc.sms.case=*) scenario=${arg#*=};; esac
done
case "$scenario" in ready|pin) ;; *) exit 2;; esac
/bin/sms-lab-fixture /fixtures/smsweb "$scenario"
