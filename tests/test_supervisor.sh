#!/bin/bash
# test_supervisor.sh — end-to-end integration test.
#
# Boots the full process set under vgsupervisor in a scratch runtime
# dir, then proves the three behaviours the demo is built on:
#   1. vitals flow (DSP log shows live HR/SpO2),
#   2. the supervisor restarts vitaldsp after `faultctl kill dsp`,
#   3. an injected VFib reaches the alarm audit log.
#
# Prints PASS/FAIL lines; exits non-zero on any failure.

set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/bin"
export VG_RUNTIME_DIR=/tmp/vg_integration
STATUS="$VG_RUNTIME_DIR/supervisor.status"
AUDIT="$VG_RUNTIME_DIR/alarm_events.log"
FAIL=0

say_fail() { echo "  FAIL: $1"; FAIL=1; }

rm -rf "$VG_RUNTIME_DIR"
mkdir -p "$VG_RUNTIME_DIR"

"$BIN/vgsupervisor" &
SUP_PID=$!

cleanup() {
    kill "$SUP_PID" 2>/dev/null
    sleep 1
    pkill -f "$BIN/vitalgen"  2>/dev/null
    pkill -f "$BIN/vitaldsp"  2>/dev/null
    pkill -f "$BIN/alarmgr"   2>/dev/null
    pkill -f "$BIN/vghog"     2>/dev/null
    true
}
trap cleanup EXIT

echo "test_supervisor: waiting for all children to run"
ok=0
for _ in $(seq 1 60); do
    if [ -f "$STATUS" ] && [ "$(grep -c 'state=running' "$STATUS")" = "3" ]; then
        ok=1; break
    fi
    sleep 0.25
done
[ "$ok" = "1" ] || say_fail "children did not all reach state=running"
cat "$STATUS" | sed 's/^/  /'

dsp_field() { awk -v k="$1" '$1=="vitaldsp"{for(i=1;i<=NF;i++) if($i ~ "^"k"=") {split($i,a,"="); print a[2]}}' "$STATUS"; }

DSP_PID1="$(dsp_field pid)"
echo "test_supervisor: vitaldsp pid=$DSP_PID1; waiting for vitals"
ok=0
for _ in $(seq 1 40); do
    if [ -f "$VG_RUNTIME_DIR/vitaldsp.log" ] && \
       grep -q "HR=" "$VG_RUNTIME_DIR/vitaldsp.log"; then
        ok=1; break
    fi
    sleep 0.25
done
[ "$ok" = "1" ] || say_fail "no vitals in vitaldsp log"
grep "HR=" "$VG_RUNTIME_DIR/vitaldsp.log" | tail -1 | sed 's/^/  /'

echo "test_supervisor: faultctl kill dsp"
"$BIN/faultctl" kill dsp || say_fail "faultctl kill dsp failed"
DSP_PID2=""
for _ in $(seq 1 40); do
    sleep 0.25
    DSP_PID2="$(dsp_field pid)"
    if [ -n "$DSP_PID2" ] && [ "$DSP_PID2" != "-1" ] && [ "$DSP_PID2" != "$DSP_PID1" ]; then
        break
    fi
done
RESTARTS="$(dsp_field restarts)"
echo "  vitaldsp pid $DSP_PID1 -> $DSP_PID2, restarts=$RESTARTS"
[ -n "$DSP_PID2" ] && [ "$DSP_PID2" != "$DSP_PID1" ] \
    || say_fail "vitaldsp was not restarted with a new pid"
[ "${RESTARTS:-0}" -ge 1 ] || say_fail "restart counter did not increase"

echo "test_supervisor: letting the restarted DSP settle"
sleep 3

echo "test_supervisor: injecting VFib for 25 s"
"$BIN/faultctl" vfib 25 || say_fail "faultctl vfib failed"
ok=0
for _ in $(seq 1 48); do
    if [ -f "$AUDIT" ] && grep -q "cond=VFIB" "$AUDIT"; then
        ok=1; break
    fi
    sleep 0.25
done
[ "$ok" = "1" ] || say_fail "no VFIB entry in alarm audit log"
grep "cond=VFIB" "$AUDIT" | head -2 | sed 's/^/  /'

if [ "$FAIL" = "0" ]; then
    echo "test_supervisor: ALL PASSED"
else
    echo "test_supervisor: FAILED"
    echo "--- vitaldsp log tail ---"
    tail -10 "$VG_RUNTIME_DIR/vitaldsp.log" 2>/dev/null || true
    echo "--- alarmgr log tail ---"
    tail -10 "$VG_RUNTIME_DIR/alarmgr.log" 2>/dev/null || true
fi
exit "$FAIL"
