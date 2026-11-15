#!/bin/sh
# build.sh — cross-compile smsweb for the DJI QDC507 module (Linux / ARMv7).
#
# Use Go 1.21-1.23. The deployed reference uses Go 1.23.12.
# Go 1.24+ links in the FIPS 140-3 module whose init segfaults on this device's
# kernel 3.18.44; see the README section "Why Go 1.21".
set -eu

cd "$(dirname "$0")"

GO_TOOL=${QDC_GO:-go}
OUTPUT=${QDC_OUTPUT:-smsweb-armv7}
ELF_ROUND=${QDC_ELF_ROUND:-4096}
BUILD_PROFILE=${QDC_BUILD_PROFILE:-balanced}
if ! command -v "$GO_TOOL" >/dev/null 2>&1; then
    echo "error: compatible Go not found; set QDC_GO to its executable" >&2
    exit 1
fi

# Do not let a persistent Go env or another SDK's GOROOT mix compiler/stdlib
# versions, and never fetch a newer toolchain while preparing this old target.
unset GOROOT
GOENV=off
GOTOOLCHAIN=local
GOPROXY=off
GOSUMDB=off
export GOENV GOTOOLCHAIN GOPROXY GOSUMDB
GOVER=$("$GO_TOOL" env GOVERSION)
GOVER=${GOVER#go}
echo "Using Go $GOVER"

case "$GOVER" in
    1.21.*|1.22.*|1.23.*) ;;
    *) echo "error: kernel 3.18.44 target requires Go 1.21-1.23; set QDC_GO" >&2; exit 2 ;;
esac
case "$ELF_ROUND" in
    4096|65536) ;;
    *) echo "error: QDC_ELF_ROUND must be 4096 or 65536" >&2; exit 2 ;;
esac
case "$BUILD_PROFILE" in
    balanced) set -- ;;
    # Smaller code footprint, with additional call overhead. Keep this explicit
    # so ordinary builds do not silently trade CPU time for memory.
    memory) set -- '-gcflags=all=-l' ;;
    *) echo "error: QDC_BUILD_PROFILE must be balanced or memory" >&2; exit 2 ;;
esac
echo "Build profile: $BUILD_PROFILE"

# QDC507 has 4 KiB kernel pages. Preserve the 64 KiB layout as an explicit
# comparison option; neither choice changes application/crypto functionality.
CGO_ENABLED=0 GOOS=linux GOARCH=arm GOARM=7 \
    "$GO_TOOL" build -trimpath -buildvcs=false "$@" -ldflags="-s -w -R $ELF_ROUND" -o "$OUTPUT" .

echo
echo "Built: $(ls -lh "$OUTPUT" | awk '{print $5}')  $OUTPUT"
