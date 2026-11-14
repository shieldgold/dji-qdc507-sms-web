#!/bin/sh
# Computer only. No devices are mounted; the builder has --network none.
set -eu
test "$#" -eq 1 || { echo "usage: $0 /absolute/ike-validation-local" >&2; exit 2; }
LOCAL_DIR=$(cd "$1" && pwd)
SOURCE_DIR=$(CDPATH='' cd -- "$(dirname "$0")" && pwd)
TOOLS_DIR=$(CDPATH='' cd -- "$SOURCE_DIR/../.." && pwd)
mkdir -p "$LOCAL_DIR/coexist-build"
chmod 700 "$LOCAL_DIR/coexist-build"
docker run --rm --network none --platform linux/amd64 \
  -v "$TOOLS_DIR:/src:ro" -v "$LOCAL_DIR/coexist-build:/out" \
  -v qdc507-ike-build-v1:/work qdc507-ike-builder:local sh -ec '
  cd /src/ike-validation/runtime
  gcc -std=gnu99 -O2 -Wall -Wextra -Werror -DQDC_MEM_TEST \
      proc.c ../lab_mem.c test_proc.c -o /out/test-ike-proc
  /out/test-ike-proc
  gcc -std=gnu99 -O2 -Wall -Wextra -Werror -DQDC_MEM_TEST \
      coexist.c http_probe.c proc.c ../lab_mem.c test_coexist.c -o /out/test-coexist
  /out/test-coexist
  gcc -std=gnu99 -O2 -Wall -Wextra -Werror \
      http_probe.c test_http_probe_main.c -o /out/test-http-probe
  gcc -std=gnu99 -O2 -Wall -Wextra -Werror -DQDC_MEM_TEST \
      guard.c proc.c coexist.c http_probe.c ../lab_mem.c ../../tun-validation/runtime/log_watch.c \
      -o /out/qdc-ike-guard-native
  QDC_IKE_GUARD=/out/qdc-ike-guard-native QDC_HTTP_PROBE=/out/test-http-probe \
      python3 -m unittest discover -s . -p "test_*.py"
  /work/toolchain/bin/musl-gcc -std=gnu99 -Os -static -s -march=armv7-a -mfloat-abi=soft \
      -ffunction-sections -fdata-sections -Wl,--gc-sections,-z,noexecstack \
      -Wall -Wextra -Werror -DQDC_MEM_TEST \
      guard.c proc.c coexist.c http_probe.c ../lab_mem.c ../../tun-validation/runtime/log_watch.c \
      -o /out/qdc-ike-guard
  '
python3 - "$TOOLS_DIR" "$LOCAL_DIR/coexist-build" <<'PY'
import hashlib
import json
from pathlib import Path
import sys
tools, out = map(Path, sys.argv[1:])
sys.path.insert(0, str(tools/'ike-validation'))
from inspect_artifact import inspect_elf
artifact = inspect_elf((out/'qdc-ike-guard').read_bytes())
assert artifact['bytes'] <= 192*1024, 'helper exceeds reviewed bootstrap budget'
paths = ['ike-validation/runtime/'+n for n in ('guard.c', 'proc.c', 'proc.h', 'coexist.c',
         'coexist.h', 'http_probe.c', 'http_probe.h', 'build-coexist.sh')]
paths += ['ike-validation/lab_mem.c', 'ike-validation/lab_mem.h',
          'tun-validation/runtime/log_watch.c', 'tun-validation/runtime/log_watch.h']
manifest = {'guard_sha256': artifact['sha256'], 'guard_bytes': artifact['bytes'], 'elf': artifact,
            'source_sha256': {p: hashlib.sha256((tools/p).read_bytes()).hexdigest() for p in paths},
            'device_executed': False, 'compiler': 'GCC 10.2.1, patched musl 1.2.6; ARM userspace only',
            'coexist_seconds': 30, 'default_admission_kib': 12288,
            'runtime_floor_kib': 6144, 'runtime_max_drop_kib': 3072}
(out/'build.json').write_text(json.dumps(manifest, indent=2)+'\n')
print(json.dumps({'guard_sha256': artifact['sha256'], 'guard_bytes': artifact['bytes'], 'device_executed': False}))
PY
