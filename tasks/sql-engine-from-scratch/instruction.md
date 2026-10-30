# Build a SQL Database Engine from Scratch

## Overview

You must build a working SQL database engine from scratch in **C** (C11 or later). No external database libraries (sqlite, leveldb, etc.) are permitted — you must implement everything yourself, including storage, parsing, and query execution.

Your engine will be compiled and tested against a comprehensive test suite that checks correctness and performance.

## Requirements

### Binary Interface

Your engine must compile to an executable called `minidb` that accepts SQL commands via standard input and writes results to standard output:

```bash
echo "SELECT * FROM users WHERE age > 25;" | ./minidb mydata.db
```

The output format is **tab-separated values (TSV)** with a header row:

```
id	name	age
1	Alice	30
2	Bob	35
```

The engine should also support a batch mode where multiple statements are separated by semicolons and newlines.

### SQL Features — Required

Your engine must support these SQL operations:

1. **DDL**: `CREATE TABLE` with typed columns (`INTEGER`, `TEXT`, `REAL`), `DROP TABLE`
2. **DML**: `INSERT INTO ... VALUES (...)`, `UPDATE ... SET ... WHERE ...`, `DELETE FROM ... WHERE ...`
3. **Queries**: `SELECT` with:
   - Column selection and `*`
   - `WHERE` clauses with `=`, `!=`, `<`, `>`, `<=`, `>=`, `AND`, `OR`, `NOT`
   - `ORDER BY` (ASC/DESC)
   - `LIMIT` and `OFFSET`
   - `COUNT(*)`, `SUM()`, `AVG()`, `MIN()`, `MAX()` aggregations
   - `GROUP BY` with `HAVING`
4. **Joins**: `INNER JOIN ... ON ...` between two tables
5. **Schema**: `.tables` command lists all tables, `.schema <table>` shows CREATE statement

### Storage Engine

- Data must be **persisted to a single database file** on disk (the file path is the first CLI argument)
- Use a **B-tree** (or B+ tree) for table storage and indexing
- The engine must handle **at least 100,000 rows per table** without excessive memory usage
- Page size should be 4096 bytes

### Performance Gates

The test suite includes performance benchmarks. Your engine must:

- Insert 100,000 rows in under **30 seconds**
- Run a point query (`WHERE id = X`) on 100k rows in under **50 milliseconds**
- Run a range scan (`WHERE id BETWEEN X AND Y`) returning ~1000 rows in under **200 milliseconds**
- Run an aggregation (`SELECT COUNT(*), AVG(value) FROM large_table`) in under **2 seconds**
- Complete a two-table join (10k × 10k rows, ~1000 matches) in under **5 seconds**

### Build

Your code must compile with:

```bash
cd /workspace && make
```

The `Makefile` must produce `./minidb` in the `/workspace` directory. You may use any standard C library functions. No third-party libraries.

## Self-Check

A self-check script is available at `/workspace/self_check.sh`. Run it at any time to see how your implementation is progressing. The score correlates with the hidden final test suite: fixing failures in the self-check will improve your final score.

```bash
cd /workspace && bash self_check.sh
```

## Getting Started

The workspace at `/workspace` contains:
- A skeleton `Makefile`
- A `self_check.sh` script for local validation
- A `testdata/` directory with sample SQL scripts

Start by implementing the storage layer (pages, B-tree), then the SQL parser, then the executor. Get basic `CREATE TABLE` and `INSERT` working first before tackling queries.

## Scoring

Your score is based on the fraction of test cases passed (correctness) weighted with performance benchmarks:
- **60%** — Correctness: DDL, DML, queries, joins, aggregations all produce correct results
- **20%** — Performance: meeting the timing gates above
- **20%** — Robustness: handling edge cases (empty tables, NULL values, syntax errors, large datasets)
