# sql-engine-from-scratch

> **Harbor Task** · `very_hard` · Systems Programming  
> Author: Ajay Mishra

Build a working SQL database engine **from scratch in C** — no external database libraries allowed. You implement everything: storage, parsing, query planning, and execution.

---

## Quick Start

```bash
# Run with the reference (oracle) solution — should score 1.000
harbor run -p tasks/sql-engine-from-scratch -a oracle

# Run with a no-op agent — should score 0.000
harbor run -p tasks/sql-engine-from-scratch -a nop
```

---

## Task Overview

Your engine must compile to an executable called `minidb` that accepts SQL commands via stdin and writes results to stdout in **tab-separated value (TSV)** format:

```bash
echo "SELECT * FROM users WHERE age > 25;" | ./minidb mydata.db
```

```
id      name    age
1       Alice   30
2       Bob     35
```

---

## Directory Structure

```
sql-engine-from-scratch/
├── README.md                        ← you are here
├── task.toml                        ← Harbor task config
├── instruction.md                   ← full task spec shown to the agent
├── environment/
│   ├── Dockerfile                   ← container image (gcc, Python 3)
│   ├── workspace/
│   │   ├── main.c                   ← skeleton stub (agent starts here)
│   │   ├── Makefile                 ← builds main.c → ./minidb
│   │   ├── self_check.sh            ← 10-test local validator
│   │   └── testdata/
│   │       └── basic.sql            ← sample SQL for manual testing
│   └── tests/
│       └── test_suite.py            ← hidden verifier (runs inside container)
├── solution/
│   └── solve.sh                     ← oracle: complete minidb implementation
├── preflight/
│   └── preflight_checks.sh          ← checks workspace files exist
└── tests/
    └── test.sh                      ← verifier entrypoint → runs test_suite.py
```

---

## What the Agent Must Build

### Binary Interface

- Executable: `./minidb <database-file>`
- Input: SQL statements via **stdin** (semicolon-delimited, can span multiple lines)
- Output: **TSV with header row** to stdout
- Errors: to stderr

### Required SQL Features

| Feature | Example |
|---|---|
| `CREATE TABLE` | `CREATE TABLE users (id INTEGER, name TEXT, age INTEGER);` |
| `DROP TABLE` | `DROP TABLE users;` |
| `INSERT` | `INSERT INTO users VALUES (1, 'Alice', 30);` |
| `SELECT *` | `SELECT * FROM users;` |
| `SELECT columns` | `SELECT name, age FROM users;` |
| `WHERE` | `WHERE age > 25 AND age < 40` |
| `ORDER BY` | `ORDER BY age DESC` |
| `LIMIT` / `OFFSET` | `LIMIT 10 OFFSET 5` |
| Aggregations | `COUNT(*)`, `SUM()`, `AVG()`, `MIN()`, `MAX()` |
| `GROUP BY` + `HAVING` | `GROUP BY dept HAVING COUNT(*) > 2` |
| `INNER JOIN` | `INNER JOIN dept ON emp.dept_id = dept.id` |
| `UPDATE` | `UPDATE users SET age = 31 WHERE name = 'Alice';` |
| `DELETE` | `DELETE FROM users WHERE age < 18;` |
| `.tables` | lists all table names |
| `.schema <table>` | shows CREATE TABLE statement |

### Storage Requirements

- Data persisted to a **single binary file** (path is first CLI argument)
- Must survive across separate process invocations (load on start, save on exit)
- Must handle **≥ 100,000 rows** per table without excessive memory usage
- Recommended page size: 4096 bytes

### Performance Gates

| Benchmark | Limit |
|---|---|
| Bulk INSERT 100,000 rows | < 30 seconds |
| Point query `WHERE id = X` on 100k rows | < 50 ms |
| Range scan returning ~1,000 rows from 100k | < 200 ms |
| `COUNT(*), AVG(val)` over 100k rows | < 2 seconds |
| JOIN 10k × 10k rows (~1,000 matches) | < 5 seconds |

---

## Scoring

| Category | Weight | Description |
|---|---|---|
| **Correctness** | 60% | 21 tests: DDL, DML, SELECT, WHERE, ORDER BY, aggregations, GROUP BY, HAVING, JOIN, UPDATE, DELETE, persistence |
| **Performance** | 20% | 5 benchmarks (bulk insert, point query, range scan, aggregation, join) |
| **Robustness** | 20% | 5 tests: empty tables, syntax errors, nonexistent tables, special characters (`'it''s a test'`), REAL precision |

```
Final Score = 0.6 × correctness + 0.2 × performance + 0.2 × robustness
```

---

## Self-Check

A self-check script is available at `/workspace/self_check.sh` inside the container. Run it any time to see your progress across 10 basic tests:

```bash
cd /workspace && bash self_check.sh
```

---

## Build

Your Makefile must produce `./minidb` in `/workspace`:

```bash
cd /workspace && make
```

The default skeleton compiles `main.c` with:

```makefile
gcc -Wall -Wextra -O2 -std=c11 -o minidb main.c -lm
```

No third-party libraries — only the standard C library.

---

## Sample SQL

See [`environment/workspace/testdata/basic.sql`](environment/workspace/testdata/basic.sql) for a working example:

```sql
CREATE TABLE employees (id INTEGER, name TEXT, department TEXT, salary REAL);
INSERT INTO employees VALUES (1, 'Alice Smith', 'Engineering', 95000.00);
INSERT INTO employees VALUES (2, 'Bob Jones',   'Marketing',  72000.00);
SELECT department, COUNT(*), AVG(salary)
  FROM employees
 GROUP BY department
 ORDER BY department;
```

---

## Implementation Tips

1. **Start with storage** — implement page I/O, then a flat row store before attempting B-tree.
2. **Then the parser** — tokenize first, then parse statement by statement.
3. **Then the executor** — get `CREATE TABLE` + `INSERT` + `SELECT *` working before WHERE/JOIN.
4. **Use `self_check.sh` often** — fixing its failures directly raises your final score.
5. **Performance** — the 50 ms point-query gate requires small row sizes and/or an in-memory index. Keep `sizeof(Row)` in the low KB range so 100k rows fit comfortably in memory.

---

## Column Types

| SQL Type | Aliases accepted | Storage |
|---|---|---|
| `INTEGER` | `INT` | 64-bit signed integer |
| `REAL` | `FLOAT`, `DOUBLE` | IEEE 754 double |
| `TEXT` | anything else | UTF-8 string |

---

## Task Config

| Field | Value |
|---|---|
| Agent timeout | 20 hours |
| Verifier timeout | 10 minutes |
| CPUs | 4 |
| Memory | 8 GB |
| Disk | 20 GB |
| Network | none (offline) |
