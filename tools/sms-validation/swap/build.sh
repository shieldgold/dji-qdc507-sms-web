#!/bin/sh
# Computer only. Disposable Linux namespace; no device tools or device mounts.
set -eu
test "$#" = 1 || { echo "usage: $0 /absolute/private-output-directory" >&2; exit 64; }
case "$1" in /*) ;; *) echo 'absolute output required' >&2; exit 64;; esac
SMS_SWAP_SRC=$(CDPATH='' cd -- "$(dirname "$0")/../../.." && pwd)
mkdir -p "$1"
SMS_SWAP_OUT=$(cd "$1" && pwd)
chmod 700 "$SMS_SWAP_OUT"
docker run --rm --network none --platform linux/amd64 --cap-add SYS_ADMIN \
  --tmpfs /data:exec,mode=0755,size=64m --tmpfs /run:exec,mode=0755,size=16m \
  -v "$SMS_SWAP_SRC:/src:ro" -v "$SMS_SWAP_OUT:/out" \
  -v qdc507-ike-build-v1:/work qdc507-ike-builder:local sh -ec '
  SMS_SWAP_WORK=$(mktemp -d /work/sms-swap-build.XXXXXX)
  trap '\''rm -rf "$SMS_SWAP_WORK"'\'' EXIT
  S=/src/tools/sms-validation/swap
  CFLAGS="-std=gnu99 -O2 -Wall -Wextra -Werror -D_FILE_OFFSET_BITS=64 -ffunction-sections -fdata-sections -Wl,--gc-sections"
  gcc $CFLAGS "$S/swap_core.c" "$S/test_core.c" -o "$SMS_SWAP_WORK/test-core"
  "$SMS_SWAP_WORK/test-core" > /out/core-tests.log
  gcc $CFLAGS "$S/swap.c" "$S/swap_core.c" /src/tools/ike-validation/runtime/proc.c -o "$SMS_SWAP_WORK/helper"
  cp "$SMS_SWAP_WORK/helper" /out/qdc-sms-swap-native-test-only
  gcc $CFLAGS -DBUILD_ID='\''"original"'\'' "$S/fixture.c" -o "$SMS_SWAP_WORK/original"
  gcc $CFLAGS -DBUILD_ID='\''"optimized"'\'' "$S/fixture.c" -o "$SMS_SWAP_WORK/optimized"
  cp "$SMS_SWAP_WORK/original" "$SMS_SWAP_WORK/supervisor"
  cp "$SMS_SWAP_WORK/original" "$SMS_SWAP_WORK/guardian"
  cp "$SMS_SWAP_WORK/original" /out/original-test-only
  cp "$SMS_SWAP_WORK/optimized" /out/optimized-test-only
  SMS_SWAP_HELPER="$SMS_SWAP_WORK/helper" SMS_SWAP_FIXTURES="$SMS_SWAP_WORK" \
      python3 "$S/test_native.py" \
        NativeSwapTests.test_copy_syncs_private_file_and_refuses_overwrite \
        NativeSwapTests.test_copy_rejects_short_input_and_removes_only_owned_partial \
        NativeSwapTests.test_copy_failure_never_removes_replaced_directory \
        NativeSwapTests.test_large_copy_exercises_bounded_writeback \
        NativeSwapTests.test_idle_stdin_is_interruptible_without_host_eof \
        NativeSwapTests.test_idle_stdin_expires_at_real_monotonic_deadline > /out/native-tests.log 2>&1
  gcc $CFLAGS -Wl,--wrap=memory_available "$S/swap.c" "$S/swap_core.c" "$S/test_low_memory.c" \
      /src/tools/ike-validation/runtime/proc.c -o "$SMS_SWAP_WORK/low-memory-helper"
  SMS_SWAP_HELPER="$SMS_SWAP_WORK/low-memory-helper" SMS_SWAP_FIXTURES="$SMS_SWAP_WORK" \
      python3 "$S/test_native.py" NativeSwapTests.test_memory_floor_before_and_after_private_sync > /out/copy-memory-tests.log 2>&1
  /work/toolchain/bin/musl-gcc -std=gnu99 -Os -static -s -D_FILE_OFFSET_BITS=64 \
      -march=armv7-a -mfloat-abi=soft -ffunction-sections -fdata-sections \
      -Wl,--gc-sections,-z,noexecstack -Wall -Wextra -Werror \
      "$S/swap.c" "$S/swap_core.c" /src/tools/ike-validation/runtime/proc.c -o /out/qdc-sms-swap-arm
  SMS_SWAP_ARM_FLAGS="-std=gnu99 -Os -static -s -D_FILE_OFFSET_BITS=64 -march=armv7-a -mfloat-abi=soft -ffunction-sections -fdata-sections -Wl,--gc-sections,-z,noexecstack -Wall -Wextra -Werror"
  /work/toolchain/bin/musl-gcc $SMS_SWAP_ARM_FLAGS "$S/test_vm.c" /src/tools/ike-validation/runtime/proc.c -o "$SMS_SWAP_WORK/swap-vm-tests"
  /work/toolchain/bin/musl-gcc $SMS_SWAP_ARM_FLAGS -DBUILD_ID='\''"original"'\'' "$S/fixture.c" -o "$SMS_SWAP_WORK/original-arm"
  /work/toolchain/bin/musl-gcc $SMS_SWAP_ARM_FLAGS -DBUILD_ID='\''"optimized"'\'' "$S/fixture.c" -o "$SMS_SWAP_WORK/optimized-arm"
  SMS_SWAP_ROOT="$SMS_SWAP_WORK/root"
  mkdir -p "$SMS_SWAP_ROOT/bin" "$SMS_SWAP_ROOT/dev" "$SMS_SWAP_ROOT/proc" "$SMS_SWAP_ROOT/run" "$SMS_SWAP_ROOT/data" "$SMS_SWAP_ROOT/fixtures"
  cp /work/output/lab-only-runtime-busybox "$SMS_SWAP_ROOT/bin/busybox"
  for name in sh mount grep mkdir cat poweroff sleep; do ln -s busybox "$SMS_SWAP_ROOT/bin/$name"; done
  cp "$SMS_SWAP_WORK/swap-vm-tests" "$SMS_SWAP_ROOT/bin/swap-vm-tests"
  cp /out/qdc-sms-swap-arm "$SMS_SWAP_ROOT/fixtures/helper"
  cp "$SMS_SWAP_WORK/original-arm" "$SMS_SWAP_ROOT/fixtures/original"
  cp "$SMS_SWAP_WORK/optimized-arm" "$SMS_SWAP_ROOT/fixtures/optimized"
  cp "$SMS_SWAP_WORK/original-arm" "$SMS_SWAP_ROOT/fixtures/guardian"
  printf unknown-resource > "$SMS_SWAP_ROOT/fixtures/foreign"
  cp "$S/init.sh" "$SMS_SWAP_ROOT/init"
  chmod 755 "$SMS_SWAP_ROOT/init"
  mknod "$SMS_SWAP_ROOT/dev/console" c 5 1
  cd "$SMS_SWAP_ROOT"
  find . -print0 | cpio --null -o --format=newc --owner=0:0 | gzip -1 > /out/lab-only-sms-swap-rootfs.cpio.gz
  '
docker run --rm --network none --platform linux/amd64 --cap-add SYS_ADMIN \
  --tmpfs /data:exec,mode=0755,size=3m --tmpfs /run:exec,mode=0755,size=16m \
  -v "$SMS_SWAP_SRC:/src:ro" -v "$SMS_SWAP_OUT:/out:ro" qdc507-ike-builder:local sh -ec '
  mkdir /run/fixtures
  cp /out/original-test-only /run/fixtures/original
  cp /out/optimized-test-only /run/fixtures/optimized
  SMS_SWAP_HELPER=/out/qdc-sms-swap-native-test-only SMS_SWAP_FIXTURES=/run/fixtures \
    python3 /src/tools/sms-validation/swap/test_native.py NativeSwapTests.test_storage_refusal_creates_no_files
  ' > "$SMS_SWAP_OUT/storage-tests.log" 2>&1
python3 - "$SMS_SWAP_SRC" "$SMS_SWAP_OUT" <<'PY'
import hashlib,json,sys
from pathlib import Path
source,out=map(Path,sys.argv[1:])
sys.path.insert(0,str(source/'tools/ike-validation'))
from inspect_artifact import inspect_elf
artifact=inspect_elf((out/'qdc-sms-swap-arm').read_bytes())
assert artifact['bytes']<=192*1024, 'helper exceeds reviewed small-helper budget'
paths=[*sorted((source/'tools/sms-validation/swap').glob('*')),
       source/'tools/ike-validation/runtime/proc.c',source/'tools/ike-validation/runtime/proc.h']
manifest={'device_executed':False,'clock_modified':False,'tests':'native stream-copy/storage tests; ARM VM proc/mount tests require run_vm.py',
          'arm_elf':artifact,'source_sha256':{str(p.relative_to(source)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths if p.is_file()},
          'initrd_sha256':hashlib.sha256((out/'lab-only-sms-swap-rootfs.cpio.gz').read_bytes()).hexdigest(),
          'seconds':300,'clock_guard_deadline_margin_seconds':30,'storage_reserve_bytes':1024*1024}
(out/'build.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps({'helper_bytes':artifact['bytes'],'sha256':artifact['sha256'],'device_executed':False}))
PY
