#!/bin/sh
# Computer-only: verifies pinned inputs then compiles, with no USB access.
set -eu
test "$#" -eq 1 || { echo 'usage: build.sh /absolute/path/to/ike-validation-local' >&2; exit 2; }
LOCAL_DIR=$(cd "$1" && pwd)
TOOLS_DIR=$(CDPATH='' cd -- "$(dirname "$0")" && pwd)
python3 - "$TOOLS_DIR/inputs.json" "$LOCAL_DIR/sources" <<'PY'
import hashlib,json,sys
from pathlib import Path
for item in json.loads(Path(sys.argv[1]).read_text()):
    path=Path(sys.argv[2])/item['file']
    data=path.read_bytes()
    assert len(data)==item['bytes'] and hashlib.sha256(data).hexdigest()==item['sha256'], path
print('All pinned source/build inputs verified')
PY
mkdir -p "$LOCAL_DIR/logs" "$LOCAL_DIR/output"
docker build --pull=false --platform linux/amd64 -t qdc507-ike-builder:local \
    -f "$TOOLS_DIR/Dockerfile" "$LOCAL_DIR/sources"
# Linux filesystem is required: Linux headers contain names that differ only
# by case, and cannot be safely copied into a default macOS filesystem.
docker volume create qdc507-ike-build-v1 >/dev/null
docker run --rm --pull=never --platform linux/amd64 --network none \
    -v qdc507-ike-build-v1:/work -v "$LOCAL_DIR:/export" \
    -v "$LOCAL_DIR/sources:/inputs:ro" -v "$TOOLS_DIR:/tools:ro" \
    qdc507-ike-builder:local sh /tools/build-inside.sh
python3 "$TOOLS_DIR/inspect_artifact.py" "$LOCAL_DIR/output/charon" "$LOCAL_DIR/output/swanctl" \
    > "$LOCAL_DIR/output/artifacts.json"
