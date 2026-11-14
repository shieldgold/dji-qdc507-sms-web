#!/bin/sh
# Computer-only init. No physical devices, external peers or Internet.
export PATH=/bin
mount -t proc proc /proc
grep -q 'qdc.runtime.lab=1' /proc/cmdline || exit 1
set -eu
trap 'rc=$?; if test "$rc" -ne 0; then echo RUNTIME_VM_FAILED rc=$rc; fi; poweroff -f' EXIT
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mount -t tmpfs -o mode=755 tmpfs /run
mkdir -p /dev/net
test -c /dev/net/tun || mknod /dev/net/tun c 10 200
root=/run/qdc-tun-VM01
mkdir "$root"
chmod 700 "$root"
cp /fixtures/charon /fixtures/swanctl "$root/"
cp /fixtures/idle.conf "$root/strongswan.conf"
chmod 500 "$root/charon" "$root/swanctl"
chmod 400 "$root/strongswan.conf"
ip addr show > /run/net-before
cat /proc/sys/net/core/xfrm_acq_expires > /run/sysctl-before
cat /proc/net/udp /proc/net/udp6 > /run/udp-before
qdc-tun-probe-musl selftest
echo CASE_PASS probe_selftest
if qdc-tun-probe-musl watch VM_PROBE_NOGO 1 5 /bin/sh -c 'echo executed > /run/probe-executed' </dev/null > /run/probe-nogo.log 2>&1; then exit 2; fi
cat /run/probe-nogo.log
grep -q LOGGER_NOT_ARMED /run/probe-nogo.log
test ! -e /run/probe-executed
echo CASE_PASS probe_no_go
printf 'GO\n' | qdc-tun-probe-musl watch VM_PROBE_LOG 1 5 /bin/sh -c 'echo executed > /run/probe-executed' > /run/probe-gate.log 2>&1
cat /run/probe-gate.log
test -e /run/probe-executed
grep -q LOGGER_COMPLETE /run/probe-gate.log
echo CASE_PASS probe_log_gate
mknod /dev/qdc-tun-VM_PROBE c 10 200
chmod 600 /dev/qdc-tun-VM_PROBE
qdc-tun-probe-musl features /dev/qdc-tun-VM_PROBE
echo CASE_PASS probe_features
qdc-tun-probe-musl create /dev/qdc-tun-VM_PROBE VM_PROBE_CREATE
echo CASE_PASS probe_create
qdc-tun-probe-musl traffic /dev/qdc-tun-VM_PROBE VM_PROBE_TRAFFIC
echo CASE_PASS probe_traffic
rm /dev/qdc-tun-VM_PROBE
qdc-ike-guard selftest
if qdc-ike-guard idle "$root" VM_NOGO 1 </dev/null > /run/nogo.log 2>&1; then exit 2; fi
cat /run/nogo.log
grep -q LOGGER_NOT_ARMED /run/nogo.log
test ! -e /run/qdc-ike
test ! -e "$root/VM_NOGO.state"
echo CASE_PASS no_go
normal_rc=0
printf 'GO\n' | qdc-ike-guard idle "$root" VM_OK 3 > /run/normal.log 2>&1 || normal_rc=$?
cat /run/normal.log
test "$normal_rc" = 0
grep -q 'IDLE_COMPLETE rc=0 children_reaped=1' /run/normal.log
grep -q 'IDLE_READY no_connections_configured=1' /run/normal.log
grep -q 'NS_INTERFACE name=ipsec0' /run/normal.log
test ! -e /run/qdc-ike
echo CASE_PASS normal_cleanup
awk '/created TUN device: ipsec0/ && !created {created=NR} /IDLE_READY/ && !ready {ready=NR} END {exit !(created && ready && created < ready)}' /run/normal.log
echo CASE_PASS streamed_app_logs
# Replace only a VM-local copy to exercise early process exit cleanup.
cp "$root/charon" /run/saved-charon
rm "$root/charon"
printf '#!/bin/sh\nexit 17\n' > "$root/charon"
chmod 500 "$root/charon"
if printf 'GO\n' | qdc-ike-guard idle "$root" VM_EARLY 1 > /run/early.log 2>&1; then exit 2; fi
cat /run/early.log
grep -q 'IDLE_COMPLETE rc=1 children_reaped=1' /run/early.log
test ! -e /run/qdc-ike
rm "$root/charon"
cp /run/saved-charon "$root/charon"
chmod 500 "$root/charon"
echo CASE_PASS early_exit_cleanup
# Exercise watchdog reaction to a synthetic kmsg warning (no kernel fault).
(sleep 2; printf '<3>Oops: QDC_RUNTIME_VM_SYNTHETIC_WARNING\n' > /dev/kmsg) &
if printf 'GO\n' | qdc-ike-guard idle "$root" VM_LOGSTOP 10 > /run/logstop.log 2>&1; then exit 2; fi
wait
cat /run/logstop.log
grep -q 'STOP kmsg_problem_or_gap' /run/logstop.log
grep -q 'IDLE_COMPLETE rc=1 children_reaped=1' /run/logstop.log
test ! -e /run/qdc-ike
echo CASE_PASS kmsg_abort_cleanup
# CLI ignores TERM in this fault case; its entire owned group must be reaped.
cp "$root/swanctl" /run/saved-swanctl
rm "$root/swanctl"
printf '#!/bin/sh\ntrap "" TERM\nwhile true; do sleep 1; done\n' > "$root/swanctl"
chmod 500 "$root/swanctl"
if printf 'GO\n' | qdc-ike-guard idle "$root" VM_CLI_TIMEOUT 1 > /run/cli-timeout.log 2>&1; then exit 2; fi
cat /run/cli-timeout.log
grep -q 'VICI_EXIT command=--stats success=0' /run/cli-timeout.log
grep -q 'IDLE_COMPLETE rc=1 children_reaped=1' /run/cli-timeout.log
test ! -e /run/qdc-ike
rm "$root/swanctl"
cp /run/saved-swanctl "$root/swanctl"
chmod 500 "$root/swanctl"
echo CASE_PASS cli_timeout_cleanup
qdc-ike-guard idle "$root" VM_RUNTIME_MEMORY 10 > /run/runtime-memory.log 2>&1 <<'GO_INPUT' &
GO
GO_INPUT
runpid=$!
qdc_count=0
while ! grep -q IDLE_READY /run/runtime-memory.log; do
  qdc_count=$((qdc_count+1)); test "$qdc_count" -lt 15; sleep 1
done
vm-fixture fill &
fillpid=$!
while test ! -e /run/fill.ready; do sleep 1; done
if wait "$runpid"; then cat /run/runtime-memory.log; exit 2; fi
kill -TERM "$fillpid"
wait "$fillpid" || true
rm /run/fill.ready
cat /run/runtime-memory.log
grep -q 'STOP memory' /run/runtime-memory.log
grep -q 'IDLE_COMPLETE rc=1 children_reaped=1' /run/runtime-memory.log
test ! -e /run/qdc-ike
echo CASE_PASS runtime_memory_abort
vm-fixture block "$root"
test -d /run/qdc-ike
# Exact, manual-recovery equivalent, exclusively created VM_BLOCKED directory.
test "$(cat /run/qdc-ike/.owner)" = VM_BLOCKED
rm -f /run/qdc-ike/charon.pid /run/qdc-ike/charon.vici /run/qdc-ike/.owner
rmdir /run/qdc-ike
echo CASE_PASS blocked_output_deadline
vm-fixture fill &
fillpid=$!
while test ! -e /run/fill.ready; do sleep 1; done
if printf 'GO\n' | qdc-ike-guard idle "$root" VM_MEMORY 1 > /run/memory.log 2>&1; then exit 2; fi
kill -TERM "$fillpid"
wait "$fillpid" || true
cat /run/memory.log
grep -q PREFLIGHT_MEMORY_REFUSED /run/memory.log
test ! -e /run/qdc-ike
test ! -e "$root/VM_MEMORY.state"
echo CASE_PASS low_memory_refusal
# Match the exact production supervisor argv in a private, disposable VM.
mkdir -p /data/smsweb
printf 'while true; do sleep 1; done\n' > /data/smsweb/start.sh
/bin/busybox /bin/sh /data/smsweb/start.sh supervise &
suppid=$!
sleep 1
start=$(vm-fixture pidinfo "$suppid")
qdc-ike-guard resume "$suppid" "$start" 2 > /run/resume.log 2>&1 &
guardpid=$!
sleep 1
grep -q RESUME_GUARD_READY /run/resume.log
kill -STOP "$suppid"
wait "$guardpid"
cat /run/resume.log
grep -q RESUME_SENT /run/resume.log
case "$(cat /proc/"$suppid"/stat)" in *') T '*) exit 2;; esac
echo CASE_PASS supervisor_auto_resume
qdc-ike-guard resume "$suppid" "$start" 60 > /run/resume-manual.log 2>&1 &
guardpid=$!
sleep 1
grep -q RESUME_GUARD_READY /run/resume-manual.log
kill -STOP "$suppid"
kill -TERM "$guardpid"
wait "$guardpid"
cat /run/resume-manual.log
grep -q RESUME_SENT /run/resume-manual.log
case "$(cat /proc/"$suppid"/stat)" in *') T '*) exit 2;; esac
for mode in auto manual; do
  date -u -s '@315532800'
  ttl=3
  test "$mode" != manual || ttl=60
  qdc-ike-guard resume-clock "$suppid" "$start" "$ttl" > /run/clock-$mode.log 2>&1 &
  guardpid=$!
  sleep 1
  grep -q 'restore_clock=1' /run/clock-$mode.log
  kill -STOP "$suppid"
  date -u -s '@1794630000'
  test "$mode" != manual || kill -TERM "$guardpid"
  wait "$guardpid"
  cat /run/clock-$mode.log
  now=$(date +%s)
  test "$now" -ge 315532800
  test "$now" -lt 315532815
  awk '/CLOCK_RESTORED/ {restored=NR} /RESUME_SENT/ {resumed=NR} END {exit !(restored && resumed && restored < resumed)}' /run/clock-$mode.log
  case "$(cat /proc/"$suppid"/stat)" in *') T '*) exit 2;; esac
  echo CASE_PASS supervisor_clock_${mode}_restore
done
kill -TERM "$suppid"
wait "$suppid" || true
echo CASE_PASS supervisor_manual_resume
# Verify the original namespace's addresses, UDP sockets and sysctl.
ip addr show > /run/net-after
cat /proc/sys/net/core/xfrm_acq_expires > /run/sysctl-after
cat /proc/net/udp /proc/net/udp6 > /run/udp-after
test "$(cat /run/net-before)" = "$(cat /run/net-after)"
test "$(cat /run/sysctl-before)" = "$(cat /run/sysctl-after)"
test "$(cat /run/udp-before)" = "$(cat /run/udp-after)"
echo CASE_PASS original_network_unchanged
echo RUNTIME_VM_COMPLETE
