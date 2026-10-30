#!/bin/bash
# Verifier entrypoint: build the agent's code, run the hidden suite, write the reward.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SUITE="$HERE/test_suite.py"
[ -f "$SUITE" ] || SUITE=/root/tests/test_suite.py

echo "=== SQL Engine Verification Suite ==="

mkdir -p /logs/verifier
cd /workspace
make clean 2>/dev/null || true
if ! make 2>&1; then
    echo "Build failed - score 0"
    echo "0" > /logs/verifier/reward.txt
    exit 0
fi

python3 "$SUITE"
EXIT_CODE=$?
if [ "$EXIT_CODE" -ne 0 ]; then
    echo "0" > /logs/verifier/reward.txt
fi
exit "$EXIT_CODE"
