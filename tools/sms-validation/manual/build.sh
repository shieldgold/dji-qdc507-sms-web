#!/bin/sh
# Computer-only builds. Inputs/outputs remain private; no device tool is used.
set -eu
test "$#" = 3 || { echo 'usage: build.sh /absolute/ike-local /absolute/new-output /absolute/validated-short-guard' >&2; exit 64; }
MANUAL_LOCAL=$(cd "$1" && pwd)
MANUAL_GUARD=$3
case "$2" in /*) ;; *) exit 64;; esac
test ! -e "$2"
mkdir -m 700 "$2"
MANUAL_OUT=$(cd "$2" && pwd)
MANUAL_TOOLS=$(CDPATH='' cd -- "$(dirname "$0")/../.." && pwd)
test -f "$MANUAL_GUARD"
python3 -m unittest discover -s "$MANUAL_TOOLS/sms-validation/manual" -p 'test_*.py' -v > "$MANUAL_OUT/host-evidence-tests.log" 2>&1
docker run --rm --network none --platform linux/amd64 \
 -v "$MANUAL_TOOLS:/src:ro" -v "$MANUAL_OUT:/out" -v "$MANUAL_LOCAL:/export:ro" \
 -v "$MANUAL_GUARD:/input/short-guard:ro" -v qdc507-ike-build-v1:/work qdc507-ike-builder:local sh -ec '
 w=$(mktemp -d /work/manual-build.XXXXXX)
 trap '\''rm -rf "$w"'\'' EXIT
 s=/src/sms-validation/manual
 common="/src/sms-validation/swap/swap_core.c /src/ike-validation/runtime/proc.c"
 flags="-std=gnu99 -Os -static -s -D_FILE_OFFSET_BITS=64 -march=armv7-a -mfloat-abi=soft -Wall -Wextra -Werror -ffunction-sections -fdata-sections -Wl,--gc-sections,-z,noexecstack"
 gcc -std=gnu99 -O2 -Wall -Wextra -Werror "$s/manual_core.c" "$s/test_core.c" /src/sms-validation/swap/swap_core.c -o "$w/core"
 "$w/core" > /out/core-tests.log
 gcc -std=gnu99 -O2 -Wall -Wextra -Werror "$s/manual_core.c" "$s/test_soak_core.c" /src/sms-validation/swap/swap_core.c -o "$w/soak-core"
 "$w/soak-core" > /out/soak-core-tests.log
 /work/toolchain/bin/musl-gcc $flags "$s/manual_swap.c" "$s/manual_core.c" $common /src/tun-validation/runtime/log_watch.c -o /out/qdc-sms-manual-arm
 /work/toolchain/bin/musl-gcc $flags -Wl,--wrap=memory_available "$s/manual_swap.c" "$s/manual_core.c" "$s/test_memory_wrap.c" $common /src/tun-validation/runtime/log_watch.c -o "$w/helper-memory-test"
 soak_shared="$s/manual_core.c $common /src/ike-validation/runtime/coexist.c /src/ike-validation/runtime/http_probe.c /src/ike-validation/runtime/icmp_probe.c /src/ike-validation/lab_mem.c /src/tun-validation/runtime/log_watch.c"
 /work/toolchain/bin/musl-gcc $flags -DQDC_MEM_TEST "$s/soak_guard.c" $soak_shared -o /out/qdc-ike-soak-arm
 /work/toolchain/bin/musl-gcc $flags -DQDC_MEM_TEST -DSMS_SOAK_TEST_CLOCK_RATE=30 "$s/soak_guard.c" $soak_shared -o /out/lab-only-soak-clock30
 /work/toolchain/bin/musl-gcc $flags "$s/test_vm.c" /src/ike-validation/runtime/proc.c -o "$w/manual-vm-tests"
 /work/toolchain/bin/musl-gcc $flags -DBUILD_ID='\''"original"'\'' /src/sms-validation/swap/fixture.c -o "$w/original"
 /work/toolchain/bin/musl-gcc $flags -DBUILD_ID='\''"optimized"'\'' /src/sms-validation/swap/fixture.c -o "$w/optimized"
 /work/toolchain/bin/musl-gcc $flags "$s/lab_fixture.c" -o "$w/lab-fixture"
 r="$w/manual-root"
 mkdir -p "$r/bin" "$r/dev" "$r/proc" "$r/run" "$r/data" "$r/fixtures"
 cp /work/output/lab-only-runtime-busybox "$r/bin/busybox"
 for name in sh mount mkdir cat poweroff sleep grep; do ln -s busybox "$r/bin/$name"; done
 cp "$w/manual-vm-tests" "$r/bin/swap-vm-tests"
 cp "$w/original" "$r/fixtures/original"
 cp "$w/original" "$r/fixtures/guardian"
 cp "$w/optimized" "$r/fixtures/optimized"
 cp /out/qdc-sms-manual-arm "$r/fixtures/helper"
 cp "$w/helper-memory-test" "$r/fixtures/helper-memory-test"
 printf unknown-resource > "$r/fixtures/foreign"
 cp /src/sms-validation/swap/init.sh "$r/init"
 chmod 755 "$r/init"
 mknod "$r/dev/console" c 5 1
 (cd "$r"; find . -print0 | cpio --null -o --format=newc --owner=0:0 | gzip -1) > /out/lab-only-manual-rootfs.cpio.gz
 r="$w/soak-root"
 mkdir -p "$r/bin" "$r/dev" "$r/proc" "$r/sys" "$r/run" "$r/fixtures" "$r/data/smsweb"
 cp /work/output/lab-only-runtime-busybox "$r/bin/busybox"
 for name in sh cat mkdir cp mv rm sleep grep awk sed stat date printf echo test mount ip ping poweroff chmod kill mknod readlink; do ln -s busybox "$r/bin/$name"; done
 cp /export/output/charon /export/output/swanctl "$r/fixtures/"
 cp /src/ike-validation/runtime/real.conf "$r/fixtures/"
 cp /input/short-guard "$r/bin/qdc-ike-guard"
 cp /out/qdc-sms-manual-arm "$r/bin/qdc-sms-manual"
 cp /out/lab-only-soak-clock30 "$r/bin/qdc-ike-soak-test-only"
 cp "$w/lab-fixture" "$r/bin/lab-fixture"
 cp "$w/lab-fixture" "$r/data/smsweb/smsweb"
 for name in ca.pem client.pem client.key server.pem server.key; do cp "/export/lab-fixtures/$name" "$r/fixtures/"; done
 sed '"'"'s/sleep 120/sleep 240/'"'"' /src/ike-validation/runtime/coexist-vm-init.sh > "$r/fixtures/server-init.sh"
 cp "$s/soak-vm-init.sh" "$r/init"
 chmod 755 "$r/init"
 mknod "$r/dev/console" c 5 1
 (cd "$r"; find . -print0 | cpio --null -o --format=newc --owner=0:0 | gzip -1) > /out/lab-only-soak-rootfs.cpio.gz
 '
python3 - "$MANUAL_TOOLS" "$MANUAL_OUT" "$MANUAL_GUARD" "$MANUAL_LOCAL" <<'PY'
from pathlib import Path
import sys,json,hashlib
tools,out,guard,local=map(Path,sys.argv[1:])
sys.path.insert(0,str(tools/'ike-validation'))
from inspect_artifact import inspect_elf
sys.path.insert(0,str(tools/'sms-validation/manual'))
from lab_certificate_time import certificate_epoch
artifacts={name:inspect_elf((out/name).read_bytes()) for name in ['qdc-sms-manual-arm','qdc-ike-soak-arm']}
assert all(a['bytes']<=192*1024 for a in artifacts.values())
assert b'manual-injected-memory' not in (out/'qdc-sms-manual-arm').read_bytes()
assert b'soak.test-result' not in (out/'qdc-ike-soak-arm').read_bytes()
paths=[p for p in (tools/'sms-validation/manual').rglob('*') if p.is_file() and '__pycache__' not in p.parts]
paths += [tools/'sms-validation/swap'/n for n in ['swap.c','swap_core.c','swap_core.h','test_vm.c','fixture.c','init.sh']]
paths += [tools/'ike-validation/runtime'/n for n in ['guard.c','proc.c','proc.h','coexist.c','coexist.h','http_probe.c','icmp_probe.c','coexist-vm-fixture.c','coexist-vm-init.sh','real.conf']]
paths += [tools/'ike-validation/lab_mem.c',tools/'ike-validation/lab_mem.h',tools/'tun-validation/runtime/log_watch.c',tools/'tun-validation/runtime/log_watch.h']
meta={'schema':1,'status':'built_not_runtime_accepted','device_executed':False,'production_clock_rate':1,'isolated_soak_clock_rate':30,
 'lab_epoch':certificate_epoch([local/'lab-fixtures'/name for name in ['ca.pem','client.pem','server.pem']]),
 'artifacts':artifacts,'short_guard_sha256':hashlib.sha256(guard.read_bytes()).hexdigest(),
 'source_sha256':{str(p.relative_to(tools)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths},
 'initrd_sha256':{name:hashlib.sha256((out/name).read_bytes()).hexdigest() for name in ['lab-only-manual-rootfs.cpio.gz','lab-only-soak-rootfs.cpio.gz']},
 'lab_clock30_sha256':hashlib.sha256((out/'lab-only-soak-clock30').read_bytes()).hexdigest(),
 'seconds':{'manual':5400,'preparation':120,'soak':3600},'thresholds_kib':{'admission':12288,'runtime_start':8192,'floor':6144,'drop':3072,'restore_go':10240}}
(out/'build.json').write_text(json.dumps(meta,indent=2)+'\n')
print(json.dumps({'built':str(out),'artifacts':{n:{'bytes':v['bytes'],'sha256':v['sha256']} for n,v in artifacts.items()},'device_executed':False}))
PY
