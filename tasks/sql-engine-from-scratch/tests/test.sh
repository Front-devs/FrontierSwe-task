#!/bin/bash
set -euo pipefail

echo "=== SQL Engine Verification Suite ==="

cd /workspace
make clean 2>/dev/null || true
if ! make 2>&1; then
    echo "Build failed — score 0"
    mkdir -p /logs/verifier
    echo "0" > /logs/verifier/reward.txt
    exit 0
fi

python3 /root/tests/test_suite.py
EXIT_CODE=$?
if [ $EXIT_CODE -ne 0 ]; then
    mkdir -p /logs/verifier
    echo "0" > /logs/verifier/reward.txt
fi
exit $EXIT_CODE
