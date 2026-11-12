#!/bin/sh
# A small supervisor; all state except the boot symlink lives under /data/smsweb.
set -eu
umask 077
D=/data/smsweb
LOCK=/tmp/smsweb-supervisor.lock
case "${1:-start}" in
  supervise)
    if ! mkdir "$LOCK" 2>/dev/null; then
      old=$(cat "$LOCK/pid" 2>/dev/null || true)
      if [ -n "$old" ] && kill -0 "$old" 2>/dev/null; then exit 0; fi
      rm -f "$LOCK/pid"
      rmdir "$LOCK" 2>/dev/null || exit 1
      mkdir "$LOCK"
    fi
    echo $$ > "$LOCK/pid"
    child=
    trap '[ -z "$child" ] || kill "$child" 2>/dev/null || true; rm -f "$D/service.pid" "$LOCK/pid"; rmdir "$LOCK" 2>/dev/null || true; exit 0' TERM INT EXIT
    while [ -x "$D/smsweb" ]; do
      for log in "$D/smsweb.log" "$D/run.log"; do
        if [ -f "$log" ] && [ "$(wc -c < "$log")" -gt 262144 ]; then mv "$log" "$log.1"; fi
      done
      "$D/smsweb" -at /dev/smd9 -listen 192.168.225.1 -port 8787 -data "$D" >> "$D/run.log" 2>&1 &
      child=$!
      echo "$child" > "$D/service.pid"
      wait "$child" || true
      child=
      sleep 5
    done
    ;;
  start)
    [ -x "$D/smsweb" ] || exit 1
    start-stop-daemon -S -b -x /bin/sh -- "$D/start.sh" supervise
    ;;
  stop)
    if [ -f "$LOCK/pid" ]; then
      pid=$(cat "$LOCK/pid")
      case "$pid" in *[!0-9]*|'') exit 1;; esac
      if [ -r "/proc/$pid/cmdline" ] && tr '\000' ' ' < "/proc/$pid/cmdline" | grep -q '/data/smsweb/start.sh supervise'; then
        kill "$pid"
        i=0
        while kill -0 "$pid" 2>/dev/null && [ "$i" -lt 10 ]; do sleep 1; i=$((i + 1)); done
        if kill -0 "$pid" 2>/dev/null; then echo 'supervisor did not stop' >&2; exit 1; fi
      fi
    fi
    ;;
  *) echo 'usage: start.sh [start|stop|supervise]' >&2; exit 2;;
esac
