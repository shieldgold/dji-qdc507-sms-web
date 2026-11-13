#!/bin/sh
# Computer only: network-disabled containers and host CPU tests, no USB access.
set -eu
test "$#" -eq 1 || { echo "usage: $0 /absolute/ike-validation-local" >&2; exit 2; }
LOCAL_DIR=$(cd "$1" && pwd)
SOURCE_DIR=$(CDPATH='' cd -- "$(dirname "$0")" && pwd)
TOOLS_DIR=$(CDPATH='' cd -- "$SOURCE_DIR/../.." && pwd)
mkdir -p "$LOCAL_DIR/runtime-build"
docker run --rm --network none --platform linux/amd64 \
  -v "$TOOLS_DIR:/src:ro" -v "$LOCAL_DIR/runtime-build:/out" \
  -v qdc507-ike-build-v1:/work qdc507-ike-builder:local sh -ec '
  cd /src/ike-validation/runtime
  gcc -std=gnu99 -O2 -Wall -Wextra -Werror -DQDC_MEM_TEST \
      proc.c ../lab_mem.c test_proc.c -o /out/test-ike-proc
  /out/test-ike-proc
  gcc -std=gnu99 -O2 -Wall -Wextra -Werror -DQDC_MEM_TEST \
      guard.c proc.c ../lab_mem.c ../../tun-validation/runtime/log_watch.c \
      -o /out/qdc-ike-guard-native
  QDC_IKE_GUARD=/out/qdc-ike-guard-native python3 -m unittest discover -s . -p "test_*.py"
  /work/toolchain/bin/musl-gcc -std=gnu99 -Os -static -s -march=armv7-a -mfloat-abi=soft \
      -ffunction-sections -fdata-sections -Wl,--gc-sections,-z,noexecstack \
      -Wall -Wextra -Werror -DQDC_MEM_TEST \
      guard.c proc.c ../lab_mem.c ../../tun-validation/runtime/log_watch.c \
      -o /out/qdc-ike-guard
  '
(
  cd "$TOOLS_DIR/tun-validation/runtime/host"
  env -u GOROOT GOTOOLCHAIN=local go test -race -count=1 ./...
  env -u GOROOT GOTOOLCHAIN=local go vet ./...
  env -u GOROOT GOTOOLCHAIN=local go build -o "$LOCAL_DIR/runtime-build/qdc507-ike-control" .
)
python3 - "$TOOLS_DIR" "$LOCAL_DIR/runtime-build" <<'PY'
import hashlib
import json
from pathlib import Path
import sys
tools, out = map(Path, sys.argv[1:])
sys.path.insert(0, str(tools/'ike-validation'))
from inspect_artifact import inspect_elf
artifact = inspect_elf((out/'qdc-ike-guard').read_bytes())
paths = ['ike-validation/runtime/guard.c', 'ike-validation/runtime/proc.c',
         'ike-validation/runtime/proc.h', 'ike-validation/runtime/build.sh',
         'ike-validation/lab_mem.c', 'ike-validation/lab_mem.h',
         'tun-validation/runtime/log_watch.c', 'tun-validation/runtime/log_watch.h']
manifest = {
    'guard_sha256': artifact['sha256'], 'guard_bytes': artifact['bytes'], 'elf': artifact,
    'host_controller_sha256': hashlib.sha256((out/'qdc507-ike-control').read_bytes()).hexdigest(),
    'source_sha256': {p: hashlib.sha256((tools/p).read_bytes()).hexdigest() for p in paths},
    'device_executed': False, 'compiler': 'GCC 10.2.1, patched musl 1.2.6; ARM userspace only',
}
(out/'build.json').write_text(json.dumps(manifest, indent=2)+'\n')
print(json.dumps(manifest))
PY
