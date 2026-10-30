#!/bin/bash
set -uo pipefail

PASS=0
TOTAL=10
DB="/tmp/self_check_$$.db"

cleanup() { rm -f "$DB"; }
trap cleanup EXIT

echo "=== Building minidb ==="
cd /workspace
make clean >/dev/null 2>&1
if make 2>&1; then
    echo "PASS: build"
    PASS=$((PASS + 1))
else
    echo "FAIL: build"
    echo "Self-check: $PASS/$TOTAL tests passed"
    exit 0
fi

run_sql() {
    echo "$1" | timeout 10 /workspace/minidb "$DB" 2>/dev/null
}

echo "=== Test: CREATE TABLE ==="
OUT=$(run_sql "CREATE TABLE test1 (id INTEGER, name TEXT, value REAL);")
if [ $? -eq 0 ]; then
    echo "PASS: CREATE TABLE"
    PASS=$((PASS + 1))
else
    echo "FAIL: CREATE TABLE"
fi

echo "=== Test: INSERT ==="
OUT=$(run_sql "INSERT INTO test1 VALUES (1, 'Alice', 3.14);")
if [ $? -eq 0 ]; then
    echo "PASS: INSERT"
    PASS=$((PASS + 1))
else
    echo "FAIL: INSERT"
fi

echo "=== Test: SELECT * ==="
OUT=$(run_sql "SELECT * FROM test1;")
if echo "$OUT" | grep -q "Alice"; then
    echo "PASS: SELECT *"
    PASS=$((PASS + 1))
else
    echo "FAIL: SELECT *"
fi

echo "=== Test: Multiple rows ==="
run_sql "INSERT INTO test1 VALUES (2, 'Bob', 2.72);"
run_sql "INSERT INTO test1 VALUES (3, 'Charlie', 1.41);"
OUT=$(run_sql "SELECT * FROM test1;")
LINES=$(echo "$OUT" | grep -c $'\t' || true)
if [ "$LINES" -ge 3 ]; then
    echo "PASS: Multiple rows"
    PASS=$((PASS + 1))
else
    echo "FAIL: Multiple rows (got $LINES lines)"
fi

echo "=== Test: WHERE clause ==="
OUT=$(run_sql "SELECT * FROM test1 WHERE id = 2;")
if echo "$OUT" | grep -q "Bob" && ! echo "$OUT" | grep -q "Alice"; then
    echo "PASS: WHERE clause"
    PASS=$((PASS + 1))
else
    echo "FAIL: WHERE clause"
fi

echo "=== Test: ORDER BY ==="
OUT=$(run_sql "SELECT * FROM test1 ORDER BY name ASC;")
FIRST=$(echo "$OUT" | head -2 | tail -1)
if echo "$FIRST" | grep -q "Alice"; then
    echo "PASS: ORDER BY"
    PASS=$((PASS + 1))
else
    echo "FAIL: ORDER BY"
fi

echo "=== Test: COUNT ==="
OUT=$(run_sql "SELECT COUNT(*) FROM test1;")
if echo "$OUT" | grep -q "3"; then
    echo "PASS: COUNT"
    PASS=$((PASS + 1))
else
    echo "FAIL: COUNT"
fi

echo "=== Test: DELETE ==="
run_sql "DELETE FROM test1 WHERE id = 2;"
OUT=$(run_sql "SELECT * FROM test1;")
if ! echo "$OUT" | grep -q "Bob"; then
    echo "PASS: DELETE"
    PASS=$((PASS + 1))
else
    echo "FAIL: DELETE"
fi

echo "=== Test: DROP TABLE ==="
run_sql "DROP TABLE test1;"
OUT=$(run_sql "SELECT * FROM test1;" 2>&1)
if [ $? -ne 0 ] || echo "$OUT" | grep -qi "error\|no such"; then
    echo "PASS: DROP TABLE"
    PASS=$((PASS + 1))
else
    echo "FAIL: DROP TABLE"
fi

echo ""
echo "==========================================="
echo "Self-check: $PASS/$TOTAL tests passed"
echo "==========================================="
