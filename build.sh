#!/bin/sh
# build.sh — cross-compile smsweb for the DJI QDC507 module (Linux / ARMv7).
#
# IMPORTANT: use Go 1.21 (or any Go < 1.24).
# Go 1.24+ links in the FIPS 140-3 module whose init segfaults on this device's
# kernel 3.18.44; see the README section "Why Go 1.21".
set -e

cd "$(dirname "$0")"

if ! command -v go >/dev/null 2>&1; then
    echo "error: go not found in PATH" >&2
    exit 1
fi

GOVER=$(go env GOVERSION 2>/dev/null | sed 's/^go//')
echo "Using Go $GOVER"

# Never let go.mod fetch a newer toolchain behind our back
GOTOOLCHAIN=local
export GOTOOLCHAIN

case "$GOVER" in
    1.2[4-9]*|1.[3-9][0-9]*)
        echo "warning: Go $GOVER is known to produce binaries that segfault on kernel 3.18.44."
        echo "         Please build with Go 1.21-1.23 instead."
        ;;
esac

CGO_ENABLED=0 GOOS=linux GOARCH=arm GOARM=7 \
    go build -trimpath -ldflags="-s -w" -o smsweb-armv7 .

echo
echo "Built: $(ls -lh smsweb-armv7 | awk '{print $5}')  smsweb-armv7"
echo "Next:  ./deploy/install.sh"
