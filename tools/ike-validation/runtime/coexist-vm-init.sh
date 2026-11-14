#!/bin/sh
# Disposable guest only. Fixed lab addresses and disposable identities.
export PATH=/bin
mount -t proc proc /proc
qdc_lab=0
for arg in $(cat /proc/cmdline); do
  if test "$arg" = 'qdc.coexist.lab=1'; then qdc_lab=1; fi
done
test "$qdc_lab" = 1 || exit 1
set -eu
trap 'rc=$?; test "$rc" = 0 || echo COEXIST_VM_FAILED rc=$rc; poweroff -f' EXIT
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mount -t tmpfs -o mode=755 tmpfs /run
role=; epoch=; scenario=
for arg in $(cat /proc/cmdline); do
  case "$arg" in qdc.role=*) role=${arg#*=};; qdc.epoch=*) epoch=${arg#*=};; qdc.case=*) scenario=${arg#*=};; esac
done
case "$scenario" in normal_30s|wrong_guardian|no_go|http_failure|sms_restart|memory_abort|signal_abort) ;; *) exit 2;; esac
date -u -s "@$epoch"
mkdir -p /dev/net
test -c /dev/net/tun || mknod /dev/net/tun c 10 200
ip link set lo up
ip link set eth0 up
case "$role" in server) own=10.77.0.1; peer=10.77.0.2;; client) own=10.77.0.2; peer=10.77.0.1;; *) exit 2;; esac
ip addr add "$own/24" dev eth0
root=/run/qdc-tun-LAB01
mkdir "$root"
chmod 700 "$root"
cp /fixtures/charon /fixtures/swanctl "$root/"
cp /fixtures/real.conf "$root/strongswan.conf"
cp /fixtures/ca.pem "$root/ca.pem"
cp /fixtures/$role.pem "$root/client.pem"
cp /fixtures/$role.key "$root/client.key"
cp /bin/qdc-ike-guard "$root/qdc-ike-guard"
chmod 500 "$root/charon" "$root/swanctl" "$root/qdc-ike-guard"
chmod 400 "$root/client.key" "$root/client.pem" "$root/ca.pem" "$root/strongswan.conf"
if test "$role" = server; then
  mkdir -p /run/qdc-ike/swanctl/private /run/qdc-ike/swanctl/x509 /run/qdc-ike/swanctl/x509ca
  cp "$root/client.key" /run/qdc-ike/swanctl/private/client.key
  cp "$root/client.pem" /run/qdc-ike/swanctl/x509/client.pem
  cp "$root/ca.pem" /run/qdc-ike/swanctl/x509ca/ca.pem
  ip addr add 10.99.0.1/32 dev lo
  export STRONGSWAN_CONF=$root/strongswan.conf
  "$root/charon" > /run/server.log 2>&1 & daemon=$!
  sleep 2
  cat > /run/qdc-ike/swanctl/swanctl.conf <<EOF
connections {
 qdc-test {
  version = 2
  local_addrs = $own
  proposals = aes256-sha256-modp2048
  mobike = no
  encap = yes
  pools = qdc-pool
  local {
   auth = pubkey
   id = server.lab
   certs = client.pem
  }
  remote {
   auth = pubkey
   id = client.lab
  }
  children {
   qdc-test {
    local_ts = 10.99.0.1/32
    remote_ts = dynamic
    esp_proposals = aes256-sha256
    start_action = none
   }
  }
 }
}
pools {
 qdc-pool {
  addrs = 10.88.0.2/32
 }
}
secrets {
 rsa-lab {
  file = client.key
 }
}
EOF
  "$root/swanctl" --load-all --noprompt
  ip route add 10.88.0.2/32 dev ipsec0
  echo COEXIST_VM_SERVER_READY
  sleep 120
  cat /run/server.log
  kill -TERM "$daemon"
  wait "$daemon"
  exit 0
fi
cat > "$root/swanctl.conf" <<EOF
connections {
 qdc-test {
  version = 2
  local_addrs = $own
  remote_addrs = $peer
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
sleep 3
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
date -u -s '@315532800'
/bin/busybox /bin/sh /data/smsweb/start.sh supervise & supervisor=$!
sleep 1
sms=$(cat /run/sms-child.pid)
sup_start=$(/bin/lab-fixture pidinfo "$supervisor")
sms_start=$(/bin/lab-fixture pidinfo "$sms")
umask 077
"$root/qdc-ike-guard" resume-clock "$supervisor" "$sup_start" 300 > "$root/sms-guard.log" 2>&1 & guardian=$!
/bin/lab-fixture wait-text "$root/sms-guard.log" 'RESUME_GUARD_READY' 3
guardian_start=$(/bin/lab-fixture pidinfo "$guardian")
kill -STOP "$supervisor"
kill -TERM "$sms"
sleep 1
date -u -s "@$epoch"
control_start=$guardian_start
if test "$scenario" = wrong_guardian; then control_start=$((guardian_start+1)); fi
cat > "$root/coexist.control" <<EOF
QDC_COEXIST_V1
RUN LAB01
BOOT $(cat /proc/sys/kernel/random/boot_id)
TOKEN LAB01_COEX
SUP $supervisor $sup_start
SMS $sms $sms_start
RESUME $guardian $control_start
HTTP 10.77.0.2
SECONDS 30
EOF
chmod 400 "$root/coexist.control"
printf 'GO\n' > /run/go
input=/run/go
if test "$scenario" = no_go; then input=/dev/null; fi
"$root/qdc-ike-guard" coexist "$root" LAB01_COEX 30 10.99.0.1 10.88.0.2 < "$input" > /run/coexist.log 2>&1 & guard=$!
injector=
case "$scenario" in http_failure|sms_restart|memory_abort|signal_abort)
  guard_start=$(/bin/lab-fixture pidinfo "$guard")
  /bin/lab-fixture inject "$scenario" /run/coexist.log "$guard" "$guard_start" > /run/inject.log 2>&1 & injector=$!;;
esac
if wait "$guard"; then rc=0; else rc=$?; fi
cat /run/coexist.log
if test -n "$injector"; then wait "$injector"; cat /run/inject.log; fi
case "$scenario" in
 normal_30s)
  test "$rc" = 0
  grep -q 'COEXIST_TRAFFIC_COMPLETE seconds=' /run/coexist.log
  test ! -e "$root/swanctl";;
 wrong_guardian) test "$rc" = 65; grep -q 'PREFLIGHT_COEXIST_REFUSED' /run/coexist.log;;
 no_go) test "$rc" = 66; grep -q 'LOGGER_NOT_ARMED' /run/coexist.log;;
 *) test "$rc" != 0; grep -q 'COEXIST_COMPLETE rc=1 children_reaped=1' /run/coexist.log;;
esac
# Refused/unarmed runs leave the independent guardian untouched; recover it exactly.
if /bin/lab-fixture matches "$guardian" "$guardian_start" "$root/qdc-ike-guard"; then
  kill -TERM "$guardian"
fi
wait "$guardian"
/bin/lab-fixture wait-text "$root/sms-guard.log" 'RESUME_SENT' 3
cat "$root/sms-guard.log"
grep -q '^CLOCK_RESTORED epoch=.* before_supervisor_cont=1$' "$root/sms-guard.log"
test "$(awk '/^CLOCK_RESTORED/{print NR}' "$root/sms-guard.log")" -lt "$(awk '/^RESUME_SENT/{print NR}' "$root/sms-guard.log")"
echo COEXIST_VM_CLOCK_ORDER_OK
printf '200\n' > /run/sms-status
sleep 2
new_sms=$(cat /run/sms-child.pid)
new_sms_start=$(/bin/lab-fixture pidinfo "$new_sms")
test "$new_sms" != "$sms"
/bin/lab-fixture matches "$supervisor" "$sup_start" /bin/busybox
/bin/lab-fixture matches "$new_sms" "$new_sms_start" /data/smsweb/smsweb
"$root/qdc-ike-guard" http-once 10.77.0.2
echo COEXIST_VM_SMS_ALIVE
first_http=$(cat /run/first-http-epoch)
echo COEXIST_VM_FIRST_HTTP epoch=$first_http
test "$first_http" -ge 315532800
test "$first_http" -lt 315533000
test ! -e /run/qdc-ike
test ! -e /sys/class/net/ipsec0
echo COEXIST_VM_VPN_CLEANED
ip addr show > /run/net-after
ip route show table all > /run/routes-after
test "$(cat /run/net-before)" = "$(cat /run/net-after)" || {
  echo COEXIST_VM_ADDRESS_BASELINE_MISMATCH; cat /run/net-before /run/net-after; exit 1;
}
test "$(cat /run/routes-before)" = "$(cat /run/routes-after)" || {
  echo COEXIST_VM_ROUTE_BASELINE_MISMATCH; cat /run/routes-before /run/routes-after; exit 1;
}
echo COEXIST_VM_NETWORK_UNCHANGED
echo COEXIST_VM_CASE_PASS case=$scenario
