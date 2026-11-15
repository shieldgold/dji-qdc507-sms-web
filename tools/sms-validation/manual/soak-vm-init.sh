#!/bin/sh
# Isolated two-guest socket lab. No physical USB, WAN or real SIM.
export PATH=/bin
mount -t proc proc /proc
grep -q 'qdc.coexist.lab=1' /proc/cmdline || exit 77
role=; epoch=; scenario=normal
for arg in $(cat /proc/cmdline); do
 case "$arg" in qdc.role=*) role=${arg#*=};; qdc.epoch=*) epoch=${arg#*=};; qdc.soak.case=*) scenario=${arg#*=};; esac
done
if test "$role" = server; then exec /bin/sh /fixtures/server-init.sh; fi
test "$role" = client || exit 77
set -eu
trap 'rc=$?; test "$rc" = 0 || echo SMS_SOAK_VM_FAILED rc=$rc; poweroff -f' EXIT
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mount -t tmpfs -o mode=755 tmpfs /run
date -u -s "@$epoch"
mkdir -p /dev/net
test -c /dev/net/tun || mknod /dev/net/tun c 10 200
ip link set lo up
ip link set eth0 up
ip addr add 10.77.0.2/24 dev eth0
root=/run/qdc-tun-LAB01
ram=/run/qdc-sms-LAB01
mkdir "$root" "$ram" /data/smsweb-c
chmod 700 "$root" "$ram" /data/smsweb /data/smsweb-c
cp /fixtures/charon /fixtures/swanctl "$root/"
cp /fixtures/real.conf "$root/strongswan.conf"
cp /fixtures/client.pem "$root/client.pem"
cp /fixtures/client.key "$root/client.key"
cp /fixtures/ca.pem "$root/ca.pem"
cp /bin/qdc-ike-guard "$root/qdc-ike-guard"
cp /bin/qdc-ike-soak-test-only "$root/qdc-ike-soak"
cp /bin/qdc-sms-manual "$ram/qdc-sms-swap"
cp /bin/lab-fixture /data/smsweb-c/smsweb
chmod 500 "$root/charon" "$root/swanctl" "$root/qdc-ike-guard" "$root/qdc-ike-soak" "$ram/qdc-sms-swap" /data/smsweb-c/smsweb
chmod 400 "$root/strongswan.conf" "$root/client.key" "$root/client.pem" "$root/ca.pem"
printf 'LAB01\n' > "$root/RUN_ID"
cat > "$root/swanctl.conf" <<'EOF'
connections {
 qdc-test {
  version = 2
  local_addrs = 10.77.0.2
  remote_addrs = 10.77.0.1
  local_port = 500
  remote_port = 500
  proposals = aes256-sha256-modp2048
  encap = yes
  mobike = no
  rekey_time = 0
  reauth_time = 0
  vips = 0.0.0.0
  local {
   auth = pubkey
   id = client.lab
   certs = client.pem
  }
  remote {
   auth = pubkey
   id = server.lab
  }
  children {
   qdc-test {
    local_ts = dynamic
    remote_ts = 10.99.0.1/32
    esp_proposals = aes256-sha256
    rekey_time = 0
    start_action = none
   }
  }
 }
}
secrets {
 rsa-lab {
  file = client.key
 }
}
EOF
chmod 400 "$root/swanctl.conf"
# Let the lab NIC finish IPv6 duplicate-address detection before taking its
# exact baseline. A tentative->ready transition is unrelated to the VPN.
i=0
while :; do
 ip addr show dev eth0 > /run/lab-if-ready
 if ! grep -q tentative /run/lab-if-ready; then break; fi
 i=$((i+1))
 test "$i" -le 5
 sleep 1
done
ip addr show > /run/net-before
ip route show table all > /run/routes-before
cat > /data/smsweb/start.sh <<'EOF'
#!/bin/sh
while :; do
 /data/smsweb/smsweb &
 sms=$!
 echo "$sms" > /run/sms-child.pid
 wait "$sms" || true
 sleep 1
done
EOF
chmod 500 /data/smsweb/start.sh
original=$(/bin/lab-fixture fileinfo /data/smsweb/smsweb)
source=$(/bin/lab-fixture fileinfo /data/smsweb-c/smsweb)
/bin/busybox /bin/sh /data/smsweb/start.sh supervise & supervisor=$!
sleep 1
sms=$(cat /run/sms-child.pid)
sup_start=$(/bin/lab-fixture pidinfo "$supervisor")
umask 077
"$root/qdc-ike-guard" resume-clock "$supervisor" "$sup_start" 300 > "$root/sms-guard.log" 2>&1 & guardian=$!
/bin/lab-fixture wait-text "$root/sms-guard.log" RESUME_GUARD_READY 3
guardian_start=$(/bin/lab-fixture pidinfo "$guardian")
kill -STOP "$supervisor"
kill -TERM "$sms"
sleep 1
cat > "$ram/lease.control" <<EOF
QDC_SMS_MANUAL_V1
RUN LAB01
BOOT $(cat /proc/sys/kernel/random/boot_id)
SUP $supervisor $sup_start
ORIGINAL $original
SOURCE $source
CLOCK $guardian $guardian_start
SECONDS 5400
EOF
"$ram/qdc-sms-swap" manual-hold LAB01 > "$ram/helper.stdout" 2>&1 & holder=$!
/bin/lab-fixture wait-text "$ram/swap.log" MANUAL_PREP_READY 5
wait "$guardian"
test "$(/bin/lab-fixture state "$supervisor")" = T
printf 'QDC_SMS_MANUAL_ARM_V1\nRUN LAB01\n' > "$ram/arm.manual"
/bin/lab-fixture wait-text "$ram/swap.log" MANUAL_ARMED 5
sleep 2
c_sms=$(cat /run/sms-child.pid)
c_start=$(/bin/lab-fixture pidinfo "$c_sms")
baseline=$(/bin/lab-fixture baseline "$ram/swap.log")
test -n "$baseline"
if test "$scenario" = wrong_baseline; then baseline=$((baseline+1)); fi
cat > "$root/soak.control" <<EOF
QDC_SMS_SOAK_V1
RUN LAB01
BOOT $(cat /proc/sys/kernel/random/boot_id)
SUP $supervisor $sup_start
SMS $c_sms $c_start
SOURCE $source
BASELINE $baseline
HTTP 127.0.0.1
SECONDS 3600
EOF
chmod 400 "$root/soak.control"
"$root/qdc-ike-soak" soak "$root" LAB01_SOAK 3600 10.99.0.1 10.88.0.2 & soak=$!
if test "$scenario" != wrong_baseline; then
 /bin/lab-fixture wait-text "$root/soak.log" SOAK_LOGGER_READY 5
 if test "$scenario" != no_go; then printf 'SOAK_GO LAB01\n' > "$root/soak.go"; fi
 case "$scenario" in
  http_failure|signal_abort)
   /bin/lab-fixture wait-text "$root/soak.log" SOAK_READY 30
   if test "$scenario" = http_failure; then printf '503\n' > /run/sms-status; else kill -TERM "$soak"; fi;;
 esac
fi
if wait "$soak"; then result=0; else result=$?; fi
cat "$root/soak.log"
case "$scenario" in
 normal)
  test "$result" = 0
  grep -q 'SOAK_BUILD production=0 clock_rate=30' "$root/soak.log"
  grep -q 'SOAK_RECONNECTED index=3 ' "$root/soak.log"
  grep -q 'SOAK_REKEY_COMPLETE inbound_and_outbound_spi_changed=1' "$root/soak.log"
  test -f "$root/soak.test-result";;
 wrong_baseline) test "$result" = 65; grep -q 'SOAK_REFUSED stage=pre_c_baseline_proof' "$root/soak.log";;
 no_go) test "$result" != 0; grep -q LOGGER_NOT_ARMED "$root/soak.log";;
 http_failure|signal_abort) test "$result" != 0; grep -q 'SOAK_COMPLETE rc=1 children_reaped=1' "$root/soak.log";;
 *) exit 77;;
esac
test ! -e "$root/soak.result"
printf '200\n' > /run/sms-status
# Scaled results cannot authorize a production manual session to stay mounted.
wait "$holder"
cat "$ram/swap.log"
grep -q 'MANUAL_ROLLBACK_COMPLETE source_retained=1' "$ram/swap.log"
test "$(/bin/lab-fixture fileinfo /data/smsweb/smsweb)" = "$original"
test "$(/bin/lab-fixture fileinfo /data/smsweb-c/smsweb)" = "$source"
test ! -e /run/qdc-ike
test ! -e /sys/class/net/ipsec0
ip addr show > /run/net-after
ip route show table all > /run/routes-after
if test "$(cat /run/net-before)" != "$(cat /run/net-after)"; then
 echo SMS_SOAK_VM_NETWORK_DIFFERENCE
 cat /run/net-before /run/net-after
 exit 1
fi
if test "$(cat /run/routes-before)" != "$(cat /run/routes-after)"; then
 echo SMS_SOAK_VM_ROUTE_DIFFERENCE
 cat /run/routes-before /run/routes-after
 exit 1
fi
echo SMS_SOAK_VM_NETWORK_UNCHANGED
echo SMS_SOAK_VM_PRODUCTION_PROOF_NOT_FAKED
echo SMS_SOAK_VM_PASS case=$scenario scaled_functional_only=1 clock_rate=30 install_retained=1
