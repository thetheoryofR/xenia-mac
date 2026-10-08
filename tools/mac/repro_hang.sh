#!/usr/bin/env bash
# Launch a title headless, wait for it to freeze (log stops growing), capture a
# stack sample of the still-running process, then kill it. Produces a deterministic
# snapshot of a hang for debugging. See docs/mac_port.md.
#
# Usage: tools/mac/repro_hang.sh "/path/to/Game.iso" [quiet_seconds] [out_prefix]
set -euo pipefail

ISO="${1:?usage: repro_hang.sh <iso> [quiet_seconds] [out_prefix]}"
QUIET="${2:-8}"          # consider it hung after this many seconds with no log growth
PREFIX="${3:-/tmp/xenia_hang}"
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$REPO/build/bin/Mac-ARM64/Debug/Xenia-Canary.app/Contents/MacOS/xenia"

LOG="${PREFIX}.log"
SAMPLE="${PREFIX}.sample.txt"
rm -f "$LOG" "$SAMPLE"

echo "[repro] launching: $ISO"
"$BIN" "$ISO" > "$LOG" 2>&1 &
PID=$!
echo "[repro] pid=$PID  log=$LOG"

# Wait until the log size is stable for $QUIET seconds (i.e. the guest went quiet).
last=-1; stable=0
while kill -0 "$PID" 2>/dev/null; do
  sz=$(wc -c < "$LOG" | tr -d ' ')
  if [ "$sz" = "$last" ]; then
    stable=$((stable + 1))
    [ "$stable" -ge "$QUIET" ] && break
  else
    stable=0; last="$sz"
  fi
  sleep 1
done

if ! kill -0 "$PID" 2>/dev/null; then
  echo "[repro] process exited on its own (not a hang). See $LOG"
  exit 0
fi

echo "[repro] log quiet for ${QUIET}s -> sampling pid $PID"
sample "$PID" 3 -file "$SAMPLE" >/dev/null 2>&1 || true
kill -9 "$PID" 2>/dev/null || true

echo "[repro] sample -> $SAMPLE"
echo "[repro] last NtWaitSingle per guest thread (deadlock culprit):"
grep "NtWaitSingle" "$LOG" | tail -20 || true
