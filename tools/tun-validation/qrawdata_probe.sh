#!/bin/sh
# Creates a compiler layout probe on the computer; no device access.
set -eu
if [ "$#" -ne 1 ]; then
    echo "usage: $0 /absolute/path/to/tun-validation-local" >&2
    exit 2
fi
LOCAL_DIR=$(cd "$1" && pwd)
mkdir -p "$LOCAL_DIR/qrawdata-analysis"
cat > "$LOCAL_DIR/qrawdata-analysis/fs-layout-probe.c" <<'EOF'
#include <linux/fs.h>
#include <linux/stddef.h>
const unsigned int qraw_fs_layout[] = {
    sizeof(struct file_operations),
    offsetof(struct file_operations, unlocked_ioctl),
    offsetof(struct file_operations, open),
    offsetof(struct file_operations, release)
};
EOF
docker run --rm -i --network none --platform linux/amd64 \
    -v "$LOCAL_DIR:/work" \
    -v "$LOCAL_DIR/capture:/work/capture:ro" \
    -v "$LOCAL_DIR/sources:/work/sources:ro" qdc507-tun-gcc493:local python3 - <<'PY'
from pathlib import Path
import shlex
import subprocess
command = Path('/work/build/drivers/net/.tun.o.cmd').read_text().splitlines()[0].split(':=', 1)[1].strip()
args = [value for value in shlex.split(command) if not value.startswith('-Wp,-MD,')]
if not args[0].startswith('/opt/gcc493/bin/'):
    raise ValueError('Build with GCC 4.9.3 first')
args[args.index('-o') + 1] = '/work/qrawdata-analysis/fs-layout-probe.o'
args[-1] = '/work/qrawdata-analysis/fs-layout-probe.c'
subprocess.run(args, cwd='/work/build', check=True)
PY
