#!/bin/sh
# Computer-only reference objects. Does not rebuild or deploy tun.ko.
set -eu
if [ "$#" -ne 1 ]; then
    echo "usage: $0 /absolute/path/to/tun-validation-local" >&2
    exit 2
fi
LOCAL_DIR=$(cd "$1" && pwd)
SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname "$0")" && pwd)
mkdir -p "$LOCAL_DIR/abi-analysis"
docker run --rm -i --network none --platform linux/amd64 \
    -v "$LOCAL_DIR:/work" \
    -v "$LOCAL_DIR/capture:/work/capture:ro" \
    -v "$LOCAL_DIR/sources:/work/sources:ro" \
    -v "$SCRIPT_DIR:/analysis-tools:ro" qdc507-tun-gcc493:local python3 - <<'PY'
from pathlib import Path
import json
import shlex
import subprocess

root = Path('/work/abi-analysis')
args = shlex.split(Path('/work/build/drivers/net/.tun.o.cmd').read_text().splitlines()[0].split(':=', 1)[1])
if not args[0].startswith('/opt/gcc493/bin/'):
    raise ValueError('Build GCC 4.9.3 candidate first')
version = subprocess.check_output([args[0], '-dumpversion'], text=True).strip()
if version != '4.9.3':
    raise ValueError('Expected GCC 4.9.3')
args = [arg for arg in args if not arg.startswith('-Wp,-MD,')]
args[args.index('-o') + 1] = '/work/abi-analysis/tun-debug.o'
args.extend(['-g', '-fno-eliminate-unused-debug-types', '-fdump-tree-optimized'])
with (root / 'debug-build.log').open('w') as log:
    subprocess.run(args, cwd='/work/build', stdout=log, stderr=subprocess.STDOUT, check=True)
targets = json.loads(Path('/analysis-tools/abi-targets.json').read_text())
command = ['make', '-j4', '-C', '/work/sources/quectel-kernel', 'O=/work/build',
           'ARCH=arm', 'CROSS_COMPILE=/opt/gcc493/bin/arm-linux-gnueabi-',
           'CC=/opt/gcc493/bin/arm-linux-gnueabi-gcc', 'HOSTCFLAGS=-O2 -fcommon'] + targets['objects']
with (root / 'core-build.log').open('w') as log:
    subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
print('Built %d reference translation units; no device access.' % len(targets['objects']))
PY
