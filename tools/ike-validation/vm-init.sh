#!/bin/sh
# Only for the isolated QEMU guests on the computer. Never deploy to a module.
export PATH=/bin
mount -t proc proc /proc
if ! grep -q 'qdc.lab=1' /proc/cmdline; then echo REFUSE_NON_LAB; exit 1; fi
set -eu
trap 'rc=$?; if test "$rc" -ne 0; then echo LAB_CLIENT_FAILED rc=$rc; fi; poweroff -f' EXIT
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mkdir -p /run/qdc-ike/swanctl/private /run/qdc-ike/swanctl/x509 /run/qdc-ike/swanctl/x509ca
role=; threads=16; auth=pubkey; epoch=
for arg in $(cat /proc/cmdline); do
  case "$arg" in qdc.role=*) role=${arg#*=};; qdc.threads=*) threads=${arg#*=};; qdc.auth=*) auth=${arg#*=};; qdc.epoch=*) epoch=${arg#*=};; esac
done
case "$threads" in 8|16) :;; *) exit 2;; esac
case "$auth" in pubkey|psk) :;; *) exit 2;; esac
date -u -s "@$epoch"
case "$role" in
  server) own=10.77.0.1; peer=10.77.0.2; inner=10.99.0.1; remote_inner=10.88.0.1; remote_role=client;;
  client) own=10.77.0.2; peer=10.77.0.1; inner=10.88.0.1; remote_inner=10.99.0.1; remote_role=server;;
  *) exit 2;;
esac
echo LAB_GUEST_BEGIN role=$role threads=$threads auth=$auth
cat /proc/version
printf 'ONLINE_CPUS '; cat /sys/devices/system/cpu/online
grep -E 'MemTotal:|MemAvailable:' /proc/meminfo
ip link set lo up
ip link set eth0 up
ip addr add "$own/24" dev eth0
ip addr add "$inner/32" dev lo
for item in rmem_default wmem_default rmem_max wmem_max; do echo 1048576 > /proc/sys/net/core/$item; done
if test "$auth" = pubkey; then
  cp /fixtures/ca.pem /run/qdc-ike/swanctl/x509ca/
  cp /fixtures/$role.pem /run/qdc-ike/swanctl/x509/
  cp /fixtures/$role.key /run/qdc-ike/swanctl/private/
  chmod 600 /run/qdc-ike/swanctl/private/$role.key
fi
cat > /run/qdc-ike/strongswan.conf <<EOF
charon {
  threads = $threads
  load_modular = no
  load = random nonce aes des sha1 sha2 md4 mgf1 hmac kdf gmp curve25519 gcm ctr drbg x509 pubkey pkcs1 pkcs8 pem constraints revocation kernel-libipsec kernel-netlink socket-default vici eap-identity eap-mschapv2 counters
  routing_table = 0
  install_routes = no
  install_virtual_ip = no
  plugins {
    kernel-libipsec {
      raw_esp = no
    }
  }
  filelog {
    stdout {
      default = 1
      flush_line = yes
    }
  }
}
EOF
certline=
if test "$auth" = pubkey; then
  certline="certs = $role.pem"
  secretblock="rsa-lab {
    file = $role.key
  }"
else
  secretblock="ike-lab {
    id-1 = client.lab
    id-2 = server.lab
    secret = lab-only-public-test-value
  }"
fi
cat > /run/qdc-ike/swanctl/swanctl.conf <<EOF
connections {
  lab {
    version = 2
    local_addrs = $own
    remote_addrs = $peer
    proposals = aes128gcm16-prfsha256-modp2048
    encap = yes
    mobike = no
    rekey_time = 0
    reauth_time = 0
    local {
      auth = $auth
      id = $role.lab
      $certline
    }
    remote {
      auth = $auth
      id = $remote_role.lab
    }
    children {
      lab {
        local_ts = $inner/32
        remote_ts = $remote_inner/32
        esp_proposals = aes128gcm16-modp2048
        rekey_time = 0
        start_action = none
      }
    }
  }
}
secrets {
  $secretblock
}
EOF
echo LAB_BEFORE_DAEMON
grep -E 'MemAvailable:|Slab:|KernelStack:|PageTables:' /proc/meminfo
charon & daemon=$!
sleep 2
kill -0 "$daemon"
lab-mem run load_all /bin/swanctl --load-all
ip link set ipsec0 up
ip route add "$remote_inner/32" dev ipsec0 src "$inner"
echo LAB_READY role=$role
if test "$role" = server; then
  sleep 180
  kill -TERM "$daemon"
  wait "$daemon"
  exit 0
fi
lab-mem watch "$daemon" 3000 IDLE
lab-mem watch "$daemon" 15000 NEGOTIATION & monitor=$!
lab-mem run initiate /bin/swanctl --initiate --child lab --timeout 30
wait "$monitor"
lab-mem run list_established /bin/swanctl --list-sas
lab-mem watch "$daemon" 5000 TRAFFIC & monitor=$!
ping -c 100 -A -s 1024 -I "$inner" "$remote_inner" > /run/ping.log
cat /run/ping.log
wait "$monitor"
lab-mem watch "$daemon" 8000 REKEY & monitor=$!
lab-mem run rekey /bin/swanctl --rekey --child lab
wait "$monitor"
lab-mem run list_rekeyed /bin/swanctl --list-sas
lab-mem watch "$daemon" 3000 ESTABLISHED
lab-mem run terminate /bin/swanctl --terminate --ike lab
kill -TERM "$daemon"
wait "$daemon"
echo LAB_AFTER_DAEMON
grep -E 'MemAvailable:|Slab:|KernelStack:|PageTables:' /proc/meminfo
echo LAB_CLIENT_COMPLETE auth=$auth threads=$threads
