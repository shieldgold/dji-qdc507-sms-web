#!/bin/sh
# Local build and CPU-only tests. Never opens USB or executes module commands.
set -eu
if [ "$#" -ne 1 ]; then
    echo "usage: $0 /absolute/path/to/tun-validation-local" >&2
    exit 2
fi
LOCAL_DIR=$(cd "$1" && pwd)
SOURCE_DIR=$(CDPATH='' cd -- "$(dirname "$0")" && pwd)
mkdir -p "$LOCAL_DIR/runtime-build"
docker run --rm --network none --platform linux/amd64 \
    -v "$SOURCE_DIR:/src:ro" \
    -v "$LOCAL_DIR/runtime-build:/out" qdc507-tun-gcc493:local sh -ec '
    gcc -std=gnu99 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
        /src/packet.c /src/test_packet.c -o /out/test-packet
    ASAN_OPTIONS=detect_leaks=0 /out/test-packet
    gcc -Wall -Wextra -Werror /src/log_watch.c /src/test_log.c -o /out/test-log
    /out/test-log
    gcc -Wall -Wextra -Werror /src/log_watch.c /src/test_watch_main.c -o /out/test-watch
    python3 /src/test_watch.py
    gcc -std=gnu99 -O2 -Wall -Wextra -Werror \
        /src/probe.c /src/packet.c /src/log_watch.c -o /out/probe-native
    python3 /src/test_probe.py
    arm-linux-gnueabi-gcc -std=gnu99 -Os -static -s -march=armv7-a -mfloat-abi=soft \
        -Wall -Wextra -Werror /src/probe.c /src/packet.c /src/log_watch.c \
        -o /out/qdc-tun-probe
    arm-linux-gnueabi-readelf -h -l /out/qdc-tun-probe > /out/probe-elf.txt
    '
(
    cc -Wall -Wextra -Werror "$SOURCE_DIR/test_signal_guard.c" -o "$LOCAL_DIR/runtime-build/test-signal-guard"
    "$LOCAL_DIR/runtime-build/test-signal-guard"
    cd "$SOURCE_DIR/host"
    env -u GOROOT GOTOOLCHAIN=local go test -race ./...
    env -u GOROOT GOTOOLCHAIN=local go vet ./...
    env -u GOROOT GOTOOLCHAIN=local go build -o "$LOCAL_DIR/runtime-build/qdc-tun-control" .
)
python3 - "$LOCAL_DIR/runtime-build/qdc-tun-probe" <<'PY'
import hashlib
from pathlib import Path
import struct
import sys
path = Path(sys.argv[1])
data = path.read_bytes()
assert data[:6] == b'\x7fELF\x01\x01', 'expected little-endian ELF32'
assert struct.unpack_from('<H', data, 18)[0] == 40, 'expected ARM'
offset = struct.unpack_from('<I', data, 28)[0]
size, count = struct.unpack_from('<HH', data, 42)
types = [struct.unpack_from('<I', data, offset + n * size)[0] for n in range(count)]
assert 2 not in types and 3 not in types, 'helper must be statically linked'
print('ARM static helper:', len(data), 'bytes;', hashlib.sha256(data).hexdigest())
PY
