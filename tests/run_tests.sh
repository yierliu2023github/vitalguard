#!/bin/bash
# run_tests.sh — build and run the full VitalGuard host test suite.
set -u
cd "$(dirname "$0")/.."

make -s all || { echo "BUILD FAILED"; exit 1; }

FAIL=0
for t in tests/test_dsp tests/test_alarm tests/test_ipc; do
    echo "===== $t ====="
    "./$t" || FAIL=1
done

echo "===== tests/test_supervisor.sh ====="
bash tests/test_supervisor.sh || FAIL=1

echo "=================================="
if [ "$FAIL" = "0" ]; then
    echo "ALL TESTS PASSED"
else
    echo "SOME TESTS FAILED"
fi
exit "$FAIL"
