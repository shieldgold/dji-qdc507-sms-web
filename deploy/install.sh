#!/bin/sh
# deploy/install.sh — push smsweb into the DJI QDC507 module and start it.
#
#   ./deploy/install.sh                 push + start (touches nothing outside /data)
#   ./deploy/install.sh --autostart     also install the boot hook
#
# The boot hook is a single symlink in the read-only root filesystem:
#   /etc/rcS.d/S99smsweb -> /data/smsweb/start.sh
# No firmware is flashed and nothing in the boot chain is modified.
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN="$ROOT/smsweb-armv7"
REMOTE_DIR=/data/smsweb
PORT=8787
AUTOSTART=0

for arg in "$@"; do
    case "$arg" in
        --autostart) AUTOSTART=1 ;;
        -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
        *) echo "unknown option: $arg" >&2; exit 1 ;;
    esac
done

# ---------- locate adb ----------
if [ -n "$ADB" ]; then
    ADB_BIN="$ADB"
elif command -v adb >/dev/null 2>&1; then
    ADB_BIN=adb
elif [ -x "$ROOT/../platform-tools/adb" ]; then
    ADB_BIN="$ROOT/../platform-tools/adb"
else
    echo "error: adb not found. Install Android platform-tools or set \$ADB." >&2
    exit 1
fi

# ---------- locate device ----------
ADB_PORT=${ADB_PORT:-5037}
if ! "$ADB_BIN" -P "$ADB_PORT" devices | grep -q "device$"; then
    echo "error: no adb device. Plug the module in and make sure ADB is enabled (see README)." >&2
    "$ADB_BIN" -P "$ADB_PORT" devices >&2
    exit 1
fi
T=$("$ADB_BIN" -P "$ADB_PORT" devices -l | grep 'device usb:' | grep -o 'transport_id:[0-9]*' | head -1 | cut -d: -f2)
if [ -n "$T" ]; then
    A="$ADB_BIN -P $ADB_PORT -t $T"
else
    A="$ADB_BIN -P $ADB_PORT"
fi

echo "==> device check"
$A shell 'id' | grep -q 'uid=0' || { echo "error: no root shell on the module" >&2; exit 1; }
$A shell 'test -c /dev/smd9' || { echo "error: /dev/smd9 (free AT channel) not found" >&2; exit 1; }

if [ ! -f "$BIN" ]; then
    echo "==> building (smsweb-armv7 not found)"
    sh "$ROOT/build.sh"
fi

echo "==> replacing any running instance"
$A shell "kill \$(pidof smsweb) 2>/dev/null; sleep 1; mkdir -p $REMOTE_DIR"

echo "==> pushing binary and boot script"
$A push "$BIN" "$REMOTE_DIR/smsweb"
$A push "$ROOT/deploy/start.sh" "$REMOTE_DIR/start.sh"
$A shell "chmod +x $REMOTE_DIR/smsweb $REMOTE_DIR/start.sh; sync"

if [ "$AUTOSTART" = "1" ]; then
    echo "==> installing boot hook: /etc/rcS.d/S99smsweb"
    $A shell "mount -o remount,rw / && ln -sf $REMOTE_DIR/start.sh /etc/rcS.d/S99smsweb && sync && mount -o remount,ro / && ls -l /etc/rcS.d/S99smsweb"
else
    echo "==> skipping boot hook (pass --autostart to install it)"
fi

echo "==> starting service"
$A shell "sh $REMOTE_DIR/start.sh"
sleep 4

echo
echo "==> status"
$A shell "ps | grep -v grep | grep smsweb || { echo 'service is NOT running; log:'; tail -20 $REMOTE_DIR/smsweb.log; }"
$A shell "tail -4 $REMOTE_DIR/smsweb.log" || true

echo
echo "Done. On the device the module is plugged into, open:"
echo
echo "    http://192.168.225.1:$PORT"
echo
echo "Notes:"
echo "  * The module needs ~40s after power-up before the page responds."
echo "  * First visit asks you to set a per-SIM password."
echo "  * Uninstall: adb shell \"kill \\\$(pidof smsweb); mount -o remount,rw /;"
echo "      rm -f /etc/rcS.d/S99smsweb; sync; mount -o remount,ro /; rm -rf $REMOTE_DIR\""
