#!/bin/bash
set -euo pipefail
echo "=== Preflight Checks ==="

[ -f "/workspace/Makefile" ] && echo "PASS: Makefile exists" || { echo "FAIL: Makefile missing"; exit 1; }
[ -f "/workspace/self_check.sh" ] && echo "PASS: self_check.sh exists" || { echo "FAIL: self_check.sh missing"; exit 1; }

# Accept any C source file (main.c, minidb.c, or other .c files)
C_FILES=$(find /workspace -maxdepth 1 -name "*.c" | wc -l)
if [ "$C_FILES" -gt 0 ]; then
    echo "PASS: C source file(s) found ($C_FILES file(s))"
else
    echo "FAIL: No C source files found in /workspace"
    exit 1
fi

echo "All preflight checks passed."
