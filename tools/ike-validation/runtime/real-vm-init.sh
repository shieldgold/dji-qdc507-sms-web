#!/bin/sh
# Disposable ARM VMs only. No real credentials, Internet or USB.
export PATH=/bin
mount -t proc proc /proc
grep -q 'qdc.real.lab=1' /proc/cmdline || exit 1
set -eu
trap 'rc=$?; test "$rc" = 0 || echo REAL_VM_FAILED rc=$rc; poweroff -f' EXIT
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mount -t tmpfs -o mode=755 tmpfs /run
role=; epoch=
for arg in $(cat /proc/cmdline); do
  case "$arg" in qdc.role=*) role=${arg#*=};; qdc.epoch=*) epoch=${arg#*=};; esac
done
date -u -s "@$epoch"
mkdir -p /dev/net
test -c /dev/net/tun || mknod /dev/net/tun c 10 200
ip link set lo up
ip link set eth0 up
case "$role" in
 server) own=10.77.0.1; peer=10.77.0.2;;
 client) own=10.77.0.2; peer=10.77.0.1;;
 *) exit 2;;
esac
ip addr add "$own/24" dev eth0
root=/run/qdc-tun-REALVM
mkdir "$root"
chmod 700 "$root"
cp /fixtures/charon /fixtures/swanctl "$root/"
cp /fixtures/real.conf "$root/strongswan.conf"
cp /fixtures/ca.pem "$root/ca.pem"
cp /fixtures/$role.pem "$root/client.pem"
cp /fixtures/$role.key "$root/client.key"
chmod 500 "$root/charon" "$root/swanctl"
chmod 400 "$root/client.key" "$root/client.pem" "$root/ca.pem" "$root/strongswan.conf"
if test "$role" = server; then
  mkdir -p /run/qdc-ike/swanctl/private /run/qdc-ike/swanctl/x509 /run/qdc-ike/swanctl/x509ca
  cp "$root/client.key" /run/qdc-ike/swanctl/private/client.key
  cp "$root/client.pem" /run/qdc-ike/swanctl/x509/client.pem
  cp "$root/ca.pem" /run/qdc-ike/swanctl/x509ca/ca.pem
  # A custom NAT-T listener tests strongSwan's nonstandard-port behavior.
  awk '{print} /threads =/ {print "  port_nat_t = 45000"}' /fixtures/real.conf > "$root/strongswan.conf"
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
  echo REAL_VM_SERVER_READY
  sleep 150
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
  local_port = 4500
  remote_port = 45000
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
# Let initial IPv6 DAD settle before taking the original-interface baseline.
sleep 3
ip addr show > /run/net-before
ip route show table all > /run/routes-before
printf 'GO\n' | /bin/qdc-ike-guard connect "$root" VM_REAL 60 10.99.0.1 10.88.0.2 > /run/connect.log 2>&1 || {
  cat /run/connect.log; exit 2;
}
cat /run/connect.log
grep -q 'CONNECT_COMPLETE rc=0 children_reaped=1' /run/connect.log
grep -q 'PROTECTED_TRAFFIC_COMPLETE ping_pairs=10 rekey_requested=1' /run/connect.log
test ! -e /run/qdc-ike
test ! -e /sys/class/net/ipsec0
ip addr show > /run/net-after
ip route show table all > /run/routes-after
if test "$(cat /run/net-before)" != "$(cat /run/net-after)"; then
  echo ADDRESS_BASELINE_MISMATCH
  cat /run/net-before /run/net-after
  exit 1
fi
if test "$(cat /run/routes-before)" != "$(cat /run/routes-after)"; then
  echo ROUTE_BASELINE_MISMATCH
  cat /run/routes-before /run/routes-after
  exit 1
fi
echo REAL_VM_CLIENT_COMPLETE
