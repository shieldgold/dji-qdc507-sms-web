#!/bin/sh
# Run on the module: remove this service's boot hook, stop it, keep all data.
set -eu
D=/data/smsweb
HOOK=/etc/rcS.d/S99smsweb
if [ -L "$HOOK" ]; then
  [ "$(readlink "$HOOK")" = "$D/start.sh" ] || { echo 'Unexpected boot hook; left unchanged' >&2; exit 1; }
  mount -o remount,rw /
  trap 'mount -o remount,ro /' EXIT
  rm "$HOOK"
  sync
  mount -o remount,ro /
  trap - EXIT
elif [ -e "$HOOK" ]; then
  echo 'Unexpected boot hook; left unchanged' >&2
  exit 1
fi
if [ -f "$D/start.sh" ]; then sh "$D/start.sh" stop; fi
if pidof smsweb >/dev/null; then echo 'smsweb still running; inspect before proceeding' >&2; exit 1; fi
echo 'Service stopped; boot hook removed. SMS and settings remain in /data/smsweb.'
