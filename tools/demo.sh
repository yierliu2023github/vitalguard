#!/bin/bash
# demo.sh — guided VitalGuard host demo (the QEMU demo's rehearsal).
#
# Timeline (~75 s):
#   0:00  system boots under the supervisor; calm sinus rhythm
#   0:10  SpO2 desaturation to 84% — watch the alarm manager WAIT
#         out the 10 s persistence delay before firing (alarm-fatigue
#         design), then SPO2_LOW annunciates
#   0:40  VFib injection — HIGH alarm within a few seconds
#   0:55  CPU hog starts; VFib again — the pipeline keeps up
#   1:05  vitaldsp is killed; the supervisor restarts it and the
#         monitor recovers (watch the status output)
#
# Everything is driven through faultctl, exactly as on target.
# Press Ctrl-C to stop; the supervisor takes the children down.

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/bin"
export VG_RUNTIME_DIR="${VG_RUNTIME_DIR:-/tmp/vitalguard-demo}"
AUDIT="$VG_RUNTIME_DIR/alarm_events.log"

rm -rf "$VG_RUNTIME_DIR"
mkdir -p "$VG_RUNTIME_DIR"

"$BIN/vgsupervisor" &
SUP_PID=$!
trap 'kill $SUP_PID 2>/dev/null; sleep 1; "$BIN/faultctl" hog stop >/dev/null 2>&1; true' EXIT

sleep 4
echo "=== VitalGuard running (normal sinus rhythm) ==="
"$BIN/faultctl" status

sleep 6
echo "=== Injecting SpO2 desaturation to 84% for 30 s ==="
"$BIN/faultctl" desat 84 30
sleep 30
echo "--- alarm audit so far ---"
cat "$AUDIT" 2>/dev/null || echo "(none)"

echo "=== Injecting VFib for 15 s ==="
"$BIN/faultctl" vfib 15
sleep 15
tail -3 "$AUDIT"

echo "=== Starting CPU hog; injecting VFib again ==="
"$BIN/faultctl" hog start
"$BIN/faultctl" vfib 12
sleep 12
"$BIN/faultctl" hog stop

echo "=== Killing vitaldsp (supervisor will restart it) ==="
"$BIN/faultctl" kill dsp
sleep 5
"$BIN/faultctl" status
echo "--- final alarm audit ---"
cat "$AUDIT"
echo "=== Demo done. Ctrl-C to stop the system. ==="
wait "$SUP_PID"
