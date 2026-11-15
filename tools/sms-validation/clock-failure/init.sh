#!/bin/sh
# Disposable ARM VM only; this rootfs has no devices or external networking.
export PATH=/bin
mount -t proc proc /proc
grep -q ' qdc.clockfailure.lab=1' /proc/cmdline || exit 65
set -eu
suppid=
guardpid=
cleanup() {
  rc=$?
  if test -n "$guardpid"; then kill -KILL "$guardpid" 2>/dev/null || true; wait "$guardpid" 2>/dev/null || true; fi
  if test -n "$suppid"; then kill -KILL "-$suppid" 2>/dev/null || true; wait "$suppid" 2>/dev/null || true; fi
  if test "$rc" -ne 0; then echo CLOCK_FAILURE_FAILED rc="$rc"; fi
  poweroff -f
}
trap cleanup EXIT
mount -t devtmpfs devtmpfs /dev
mount -t tmpfs -o mode=700 tmpfs /run
test "$(uname -r)" = 3.18.44
echo CLOCK_FAILURE_KERNEL release=3.18.44
case_name=failure
for arg in $(cat /proc/cmdline); do case "$arg" in qdc.clockfailure.case=*) case_name=${arg#*=};; esac; done
case "$case_name" in failure|delegate-success|delegate-failure) ;; *) exit 64;; esac
(set -C; printf 'while :; do :; done\n' > /data/smsweb/start.sh)
clock-failure-launcher supervise &
suppid=$!
sleep 1
start=$(awk '{print $22}' /proc/"$suppid"/stat)
/fixtures/qdc-ike-guard identity "$suppid" "$start" /bin/busybox
echo CLOCK_FAILURE_SUPERVISOR identity=exact_busybox_argv
kill -STOP "$suppid"
count=0
while test "$(awk '{print $3}' /proc/"$suppid"/stat)" != T; do count=$((count+1)); test "$count" -lt 20; sleep 1; done
: > /run/clock-failure.log
rc=0
if test "$case_name" = failure; then
  clock-failure-launcher guard "$suppid" "$start" 1 > /run/clock-failure.log 2>&1 || rc=$?
else
  date -u -s '@315532800' > /dev/null
  mode=guard
  test "$case_name" != delegate-success || mode=full-guard
  clock-failure-launcher "$mode" "$suppid" "$start" 30 > /run/clock-failure.log 2>&1 &
  guardpid=$!
  count=0
  while ! grep -q RESUME_GUARD_READY /run/clock-failure.log; do
    count=$((count+1)); test "$count" -lt 20; kill -0 "$guardpid"; sleep 1
  done
  date -u -s '@1794700800' > /dev/null
  kill -USR1 "$guardpid"
  wait "$guardpid" || rc=$?
  guardpid=
fi
cat /run/clock-failure.log
state=$(awk '{print $3}' /proc/"$suppid"/stat)
resumed=0
delegated=0
if grep -q RESUME_SENT /run/clock-failure.log; then resumed=1; fi
if grep -q RESUME_DELEGATED /run/clock-failure.log; then delegated=1; fi
now=$(date +%s)
echo CLOCK_FAILURE_OBSERVED case="$case_name" rc="$rc" supervisor_state="$state" resume_sent="$resumed" delegated="$delegated" epoch="$now"
grep -q 'RESUME_GUARD_READY.*restore_clock=1' /run/clock-failure.log
test "$resumed" = 0
test "$state" = T
if test "$case_name" = delegate-success; then
  test "$rc" = 0
  test "$delegated" = 1
  test "$now" -ge 315532800
  test "$now" -lt 315532900
  awk '/CLOCK_RESTORED/ {restored=NR} /RESUME_DELEGATED/ {delegated=NR} END {exit !(restored && delegated && restored < delegated)}' /run/clock-failure.log
else
  grep -q 'CLOCK_FAILURE_CAPS uid=0 effective=0 permitted=0 inheritable=0 bounding=0 cap_sys_time=25' /run/clock-failure.log
  grep -q 'RECOVERY_REQUIRED clock_restore.*Operation not permitted' /run/clock-failure.log
  test "$rc" = 1
  test "$delegated" = 0
fi
kill -KILL "-$suppid"
wait "$suppid" 2>/dev/null || true
suppid=
echo CLOCK_FAILURE_CLEANUP owned_supervisor_group_removed=1
echo CLOCK_FAILURE_PASS
