#!/bin/sh
# Runtime-only initialization; preserve modem firmware and USB composition.
set -eu
umask 077
D=/data/smsweb/voice-runtime
READY=/tmp/smsweb-voice-ready
PIDFILE=/tmp/smsweb-alsaucm.pid
LOG=/tmp/smsweb-alsaucm.log
FIFO=/run/alsaucm_test
[ "$(uname -r)" = 3.18.44 ] || { echo 'unsupported kernel' >&2; exit 1; }
cd "$D"
printf '%s\n' \
 '3d82d3dec4f1e323201bba87156df9d41438e08314097353f2607f9117211d4a  qdc507_aprv3.ko' \
 'ed3821682d5309969a01c764192c83feff9669c61ef237c69475cd1619cf296c  qdc507_voice.ko' \
 'e55995d9d2a5b8233f3e720005de9548a8b7e55507dc07ab8a2555b4ea57c37e  /usr/bin/alsaucm_test' \
 '8cba40f3682661a0133d3cc4f0e281b1a4133fe3f9d953ec952d5b492f399302  /usr/lib/libacdbloader.so' | sha256sum -c -
[ -d /sys/module/qdc507_aprv3 ] || insmod "$D/qdc507_aprv3.ko"
[ -d /sys/module/qdc507_voice ] || insmod "$D/qdc507_voice.ko"
[ -c /dev/snd/pcmC0D4p ] && [ -c /dev/snd/pcmC0D4c ] && [ -c /dev/snd/pcmC0D5p ] && [ -c /dev/snd/pcmC0D6c ]
"$D/network-pcm-bridge" --check
owned=0
if [ -s "$PIDFILE" ]; then
 read pid expected_start < "$PIDFILE"
 case "$pid" in *[!0-9]*|'') exit 1;; esac
 current_start=$(cut -d ' ' -f 22 "/proc/$pid/stat" 2>/dev/null || true)
 argv0=$(tr '\000' '\n' < "/proc/$pid/cmdline" 2>/dev/null | sed -n '1p' || true)
 if [ "$current_start" = "$expected_start" ] && [ "$argv0" = /usr/bin/alsaucm_test ]; then owned=1; fi
fi
if [ "$owned" -eq 1 ] && [ -s "$READY" ] && cmp -s "$READY" "$PIDFILE"; then exit 0; fi
rm -f "$READY"
cleanup() {
 result=$?
 if [ "$result" -ne 0 ]; then
  rm -f "$READY"
  if [ "${started:-0}" -eq 1 ]; then
   kill "$pid" 2>/dev/null || true
   rm -f "$PIDFILE" "$FIFO"
  fi
 fi
}
trap cleanup EXIT
if [ "$owned" -eq 0 ]; then
 # A stale FIFO from our previous process is safe to remove; never take over
 # an unknown calibrator. PID/start identity avoids killing reused PIDs.
 if [ -p "$FIFO" ]; then
  [ -s "$PIDFILE" ] || { echo 'another calibration service owns the FIFO' >&2; exit 1; }
  [ -z "$(pidof alsaucm_test || true)" ] || { echo 'unowned calibration process' >&2; exit 1; }
  rm -f "$FIFO"
 fi
 setsid env LD_PRELOAD="$D/offline-calibration.so" /usr/bin/alsaucm_test </dev/null > "$LOG" 2>&1 &
 pid=$!
 started=1
 start=$(cut -d ' ' -f 22 "/proc/$pid/stat")
 printf '%s %s\n' "$pid" "$start" > "$PIDFILE"
 i=0
 while [ ! -p "$FIFO" ] && [ "$i" -lt 40 ]; do kill -0 "$pid"; sleep 0.1; i=$((i+1)); done
 [ -p "$FIFO" ]
fi
chmod 600 "$FIFO"
if ! grep -q 'ACDB -> Sent VocProc Cal!' "$LOG"; then
 # The vendor parser consumes one command per FIFO write. A combined write
 # silently loses commands. Keep the entire sequence bounded.
 busybox timeout -t 8 /bin/sh -c '
  printf "open snd_soc_msm_9x07_Tomtom_I2S\n" > /run/alsaucm_test
  sleep 0.1
  printf "set _verb VoLTE\n" > /run/alsaucm_test
  sleep 0.1
  printf "set _enadev Auxpcm Rx\n" > /run/alsaucm_test
  sleep 0.1
  printf "set _enadev Auxpcm Tx\n" > /run/alsaucm_test
 '
 i=0
 while ! grep -q 'ACDB -> Sent VocProc Cal!' "$LOG" && [ "$i" -lt 60 ]; do kill -0 "$pid"; sleep 0.1; i=$((i+1)); done
fi
grep -q 'ACDB -> Sent VocProc Cal!' "$LOG"
kill -0 "$pid"
cp "$PIDFILE" "$READY"
echo 'voice calibration ready'
