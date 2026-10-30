#!/usr/bin/env python3
"""Hidden test suite for sql-engine-from-scratch."""
import subprocess
import tempfile
import time
import os
import json

MINIDB = "/workspace/minidb"
RESULTS = {"correctness": {}, "performance": {}, "robustness": {}}


def run_sql(db_path, sql, timeout=30):
    proc = subprocess.run(
        [MINIDB, db_path],
        input=sql,
        capture_output=True,
        text=True,
        timeout=timeout
    )
    return proc.stdout, proc.stderr, proc.returncode


def parse_tsv(output):
    lines = [l for l in output.strip().split('\n') if l.strip()]
    if not lines:
        return []
    headers = lines[0].split('\t')
    rows = []
    for line in lines[1:]:
        vals = line.split('\t')
        rows.append(dict(zip(headers, vals)))
    return rows


def test_correctness():
    passed = 0
    total = 0

    def check(name, db, sql, validator):
        nonlocal passed, total
        total += 1
        try:
            stdout, stderr, rc = run_sql(db, sql)
            if validator(stdout, stderr, rc):
                passed += 1
                RESULTS["correctness"][name] = "pass"
            else:
                RESULTS["correctness"][name] = "fail"
        except Exception as e:
            RESULTS["correctness"][name] = f"error: {e}"

    db = tempfile.mktemp(suffix='.db')

    check("create_table", db,
          "CREATE TABLE users (id INTEGER, name TEXT, age INTEGER);",
          lambda o, e, r: r == 0)

    check("insert_single", db,
          "INSERT INTO users VALUES (1, 'Alice', 30);",
          lambda o, e, r: r == 0)

    sql = "\n".join([
        "INSERT INTO users VALUES (2, 'Bob', 25);",
        "INSERT INTO users VALUES (3, 'Charlie', 35);",
        "INSERT INTO users VALUES (4, 'Diana', 28);",
        "INSERT INTO users VALUES (5, 'Eve', 32);"
    ])
    check("insert_multiple", db, sql, lambda o, e, r: r == 0)

    check("select_all", db,
          "SELECT * FROM users;",
          lambda o, e, r: r == 0 and len(parse_tsv(o)) == 5)

    check("select_columns", db,
          "SELECT name, age FROM users;",
          lambda o, e, r: r == 0 and 'Alice' in o)

    check("where_eq", db,
          "SELECT * FROM users WHERE name = 'Bob';",
          lambda o, e, r: r == 0 and len(parse_tsv(o)) == 1)

    check("where_lt", db,
          "SELECT * FROM users WHERE age < 30;",
          lambda o, e, r: r == 0 and len(parse_tsv(o)) == 2)

    check("where_and", db,
          "SELECT * FROM users WHERE age > 25 AND age < 33;",
          lambda o, e, r: r == 0 and len(parse_tsv(o)) == 3)

    check("where_or", db,
          "SELECT * FROM users WHERE name = 'Alice' OR name = 'Eve';",
          lambda o, e, r: r == 0 and len(parse_tsv(o)) == 2)

    check("order_by_asc", db,
          "SELECT * FROM users ORDER BY age ASC;",
          lambda o, e, r: r == 0 and parse_tsv(o) and parse_tsv(o)[0].get('name') == 'Bob')

    check("order_by_desc", db,
          "SELECT * FROM users ORDER BY age DESC;",
          lambda o, e, r: r == 0 and parse_tsv(o) and parse_tsv(o)[0].get('name') == 'Charlie')

    check("limit", db,
          "SELECT * FROM users ORDER BY id ASC LIMIT 3;",
          lambda o, e, r: r == 0 and len(parse_tsv(o)) == 3)

    check("limit_offset", db,
          "SELECT * FROM users ORDER BY id ASC LIMIT 2 OFFSET 2;",
          lambda o, e, r: r == 0 and len(parse_tsv(o)) == 2)

    check("count", db,
          "SELECT COUNT(*) FROM users;",
          lambda o, e, r: r == 0 and '5' in o)

    check("sum", db,
          "SELECT SUM(age) FROM users;",
          lambda o, e, r: r == 0 and '150' in o)

    check("avg", db,
          "SELECT AVG(age) FROM users;",
          lambda o, e, r: r == 0 and '30' in o)

    check("min", db,
          "SELECT MIN(age) FROM users;",
          lambda o, e, r: r == 0 and '25' in o)

    check("max", db,
          "SELECT MAX(age) FROM users;",
          lambda o, e, r: r == 0 and '35' in o)

    try:
        os.remove(db)
    except OSError:
        pass

    db = tempfile.mktemp(suffix='.db')
    run_sql(db, "\n".join([
        "CREATE TABLE sales (id INTEGER, region TEXT, amount REAL);",
        "INSERT INTO sales VALUES (1, 'East', 100.0);",
        "INSERT INTO sales VALUES (2, 'West', 200.0);",
        "INSERT INTO sales VALUES (3, 'East', 150.0);",
        "INSERT INTO sales VALUES (4, 'West', 250.0);",
        "INSERT INTO sales VALUES (5, 'East', 120.0);"
    ]))

    check("group_by", db,
          "SELECT region, COUNT(*), SUM(amount) FROM sales GROUP BY region;",
          lambda o, e, r: r == 0 and len(parse_tsv(o)) == 2)

    check("having", db,
          "SELECT region, SUM(amount) FROM sales GROUP BY region HAVING SUM(amount) > 400;",
          lambda o, e, r: r == 0 and len(parse_tsv(o)) == 1)

    try:
        os.remove(db)
    except OSError:
        pass

    db = tempfile.mktemp(suffix='.db')
    run_sql(db, "\n".join([
        "CREATE TABLE items (id INTEGER, name TEXT, price REAL);",
        "INSERT INTO items VALUES (1, 'Apple', 1.50);",
        "INSERT INTO items VALUES (2, 'Banana', 0.75);",
        "INSERT INTO items VALUES (3, 'Cherry', 3.00);"
    ]))
    run_sql(db, "UPDATE items SET price = 2.00 WHERE name = 'Apple';")

    check("update", db,
          "SELECT price FROM items WHERE name = 'Apple';",
          lambda o, e, r: r == 0 and '2' in o and '1.5' not in o)

    run_sql(db, "DELETE FROM items WHERE name = 'Banana';")
    check("delete", db,
          "SELECT * FROM items;",
          lambda o, e, r: r == 0 and len(parse_tsv(o)) == 2 and 'Banana' not in o)

    try:
        os.remove(db)
    except OSError:
        pass

    db = tempfile.mktemp(suffix='.db')
    run_sql(db, "\n".join([
        "CREATE TABLE departments (dept_id INTEGER, dept_name TEXT);",
        "INSERT INTO departments VALUES (1, 'Engineering');",
        "INSERT INTO departments VALUES (2, 'Marketing');",
        "CREATE TABLE employees (emp_id INTEGER, emp_name TEXT, dept_id INTEGER);",
        "INSERT INTO employees VALUES (101, 'Alice', 1);",
        "INSERT INTO employees VALUES (102, 'Bob', 2);",
        "INSERT INTO employees VALUES (103, 'Charlie', 1);"
    ]))

    check("inner_join", db,
          "SELECT emp_name, dept_name FROM employees INNER JOIN departments ON employees.dept_id = departments.dept_id;",
          lambda o, e, r: r == 0 and len(parse_tsv(o)) == 3)

    check("drop_table", db,
          "DROP TABLE departments;",
          lambda o, e, r: r == 0)

    check("dot_tables", db,
          ".tables",
          lambda o, e, r: r == 0 and 'employees' in o)

    try:
        os.remove(db)
    except OSError:
        pass

    db = tempfile.mktemp(suffix='.db')
    run_sql(db, "CREATE TABLE persist (id INTEGER, val TEXT);\nINSERT INTO persist VALUES (1, 'hello');")
    check("persistence", db,
          "SELECT * FROM persist;",
          lambda o, e, r: r == 0 and 'hello' in o)

    try:
        os.remove(db)
    except OSError:
        pass

    return passed / max(total, 1)


def test_performance():
    passed = 0
    total = 5

    db = tempfile.mktemp(suffix='.db')

    # Bulk insert 100k rows
    lines = ["CREATE TABLE large (id INTEGER, name TEXT, value REAL);"]
    for i in range(100000):
        lines.append(f"INSERT INTO large VALUES ({i}, 'name_{i}', {i * 1.5});")
    sql = "\n".join(lines)

    start = time.time()
    try:
        run_sql(db, sql, timeout=60)
        elapsed = time.time() - start
        if elapsed < 30:
            passed += 1
            RESULTS["performance"]["bulk_insert_100k"] = f"pass ({elapsed:.1f}s)"
        else:
            RESULTS["performance"]["bulk_insert_100k"] = f"fail ({elapsed:.1f}s)"
    except Exception as e:
        RESULTS["performance"]["bulk_insert_100k"] = f"error: {e}"

    # Point query
    start = time.time()
    try:
        run_sql(db, "SELECT * FROM large WHERE id = 50000;", timeout=5)
        elapsed = time.time() - start
        if elapsed < 0.05:
            passed += 1
            RESULTS["performance"]["point_query"] = f"pass ({elapsed*1000:.1f}ms)"
        else:
            RESULTS["performance"]["point_query"] = f"fail ({elapsed*1000:.1f}ms)"
    except Exception as e:
        RESULTS["performance"]["point_query"] = f"error: {e}"

    # Range scan
    start = time.time()
    try:
        run_sql(db, "SELECT * FROM large WHERE id >= 10000 AND id < 11000;", timeout=5)
        elapsed = time.time() - start
        if elapsed < 0.2:
            passed += 1
            RESULTS["performance"]["range_scan"] = f"pass ({elapsed*1000:.1f}ms)"
        else:
            RESULTS["performance"]["range_scan"] = f"fail ({elapsed*1000:.1f}ms)"
    except Exception as e:
        RESULTS["performance"]["range_scan"] = f"error: {e}"

    # Aggregation
    start = time.time()
    try:
        run_sql(db, "SELECT COUNT(*), AVG(value) FROM large;", timeout=10)
        elapsed = time.time() - start
        if elapsed < 2:
            passed += 1
            RESULTS["performance"]["aggregation"] = f"pass ({elapsed:.2f}s)"
        else:
            RESULTS["performance"]["aggregation"] = f"fail ({elapsed:.2f}s)"
    except Exception as e:
        RESULTS["performance"]["aggregation"] = f"error: {e}"

    try:
        os.remove(db)
    except OSError:
        pass

    # Join benchmark
    db = tempfile.mktemp(suffix='.db')
    lines = ["CREATE TABLE t1 (id INTEGER, val TEXT);", "CREATE TABLE t2 (id INTEGER, ref_id INTEGER, data TEXT);"]
    for i in range(10000):
        lines.append(f"INSERT INTO t1 VALUES ({i}, 'val_{i}');")
    for i in range(10000):
        lines.append(f"INSERT INTO t2 VALUES ({i}, {i % 1000}, 'data_{i}');")
    try:
        run_sql(db, "\n".join(lines), timeout=120)
    except Exception:
        pass

    start = time.time()
    try:
        run_sql(db, "SELECT t1.val, t2.data FROM t1 INNER JOIN t2 ON t1.id = t2.ref_id WHERE t1.id < 100;", timeout=30)
        elapsed = time.time() - start
        if elapsed < 5:
            passed += 1
            RESULTS["performance"]["join_10k"] = f"pass ({elapsed:.2f}s)"
        else:
            RESULTS["performance"]["join_10k"] = f"fail ({elapsed:.2f}s)"
    except Exception as e:
        RESULTS["performance"]["join_10k"] = f"error: {e}"

    try:
        os.remove(db)
    except OSError:
        pass

    return passed / max(total, 1)


def test_robustness():
    passed = 0
    total = 5

    db = tempfile.mktemp(suffix='.db')
    run_sql(db, "CREATE TABLE empty_t (id INTEGER, name TEXT);")

    try:
        stdout, _, rc = run_sql(db, "SELECT * FROM empty_t;")
        rows = parse_tsv(stdout)
        if rc == 0 and len(rows) == 0:
            passed += 1
            RESULTS["robustness"]["empty_table"] = "pass"
        else:
            RESULTS["robustness"]["empty_table"] = "fail"
    except Exception as e:
        RESULTS["robustness"]["empty_table"] = f"error: {e}"

    try:
        _, stderr, rc = run_sql(db, "SELEC * FORM users;")
        if rc != 0 or 'error' in stderr.lower():
            passed += 1
            RESULTS["robustness"]["syntax_error"] = "pass"
        else:
            RESULTS["robustness"]["syntax_error"] = "fail"
    except Exception as e:
        RESULTS["robustness"]["syntax_error"] = f"error: {e}"

    try:
        _, stderr, rc = run_sql(db, "SELECT * FROM nonexistent;")
        if rc != 0 or 'error' in stderr.lower():
            passed += 1
            RESULTS["robustness"]["no_such_table"] = "pass"
        else:
            RESULTS["robustness"]["no_such_table"] = "fail"
    except Exception as e:
        RESULTS["robustness"]["no_such_table"] = f"error: {e}"

    try:
        os.remove(db)
    except OSError:
        pass

    db = tempfile.mktemp(suffix='.db')
    try:
        run_sql(db, "CREATE TABLE strings (id INTEGER, val TEXT);")
        run_sql(db, "INSERT INTO strings VALUES (1, 'it''s a test');")
        stdout, _, rc = run_sql(db, "SELECT val FROM strings WHERE id = 1;")
        if rc == 0 and "it's a test" in stdout:
            passed += 1
            RESULTS["robustness"]["special_chars"] = "pass"
        else:
            RESULTS["robustness"]["special_chars"] = "fail"
    except Exception as e:
        RESULTS["robustness"]["special_chars"] = f"error: {e}"

    try:
        os.remove(db)
    except OSError:
        pass

    db = tempfile.mktemp(suffix='.db')
    try:
        run_sql(db, "CREATE TABLE reals (id INTEGER, val REAL);")
        run_sql(db, "INSERT INTO reals VALUES (1, 3.14159);")
        stdout, _, rc = run_sql(db, "SELECT val FROM reals WHERE id = 1;")
        if rc == 0 and '3.14' in stdout:
            passed += 1
            RESULTS["robustness"]["real_precision"] = "pass"
        else:
            RESULTS["robustness"]["real_precision"] = "fail"
    except Exception as e:
        RESULTS["robustness"]["real_precision"] = f"error: {e}"

    try:
        os.remove(db)
    except OSError:
        pass

    return passed / max(total, 1)


def main():
    if not os.path.exists(MINIDB):
        result = subprocess.run(["make"], cwd="/workspace", capture_output=True, text=True)
        if result.returncode != 0:
            os.makedirs("/logs/verifier", exist_ok=True)
            with open("/logs/verifier/reward.txt", "w") as f:
                f.write("0")
            return

    c_score = test_correctness()
    p_score = test_performance()
    r_score = test_robustness()

    final_score = 0.6 * c_score + 0.2 * p_score + 0.2 * r_score

    print(f"\n{'='*50}")
    print(f"Correctness: {c_score:.3f} (weight: 60%)")
    print(f"Performance: {p_score:.3f} (weight: 20%)")
    print(f"Robustness:  {r_score:.3f} (weight: 20%)")
    print(f"{'='*50}")
    print(f"Final Score: {final_score:.3f}")

    os.makedirs("/logs/verifier", exist_ok=True)
    with open("/logs/verifier/reward.txt", "w") as f:
        f.write(f"{final_score:.6f}")

    with open("/logs/verifier/reward.json", "w") as f:
        json.dump({
            "score": final_score,
            "correctness": c_score,
            "performance": p_score,
            "robustness": r_score,
            "details": RESULTS
        }, f, indent=2)


if __name__ == "__main__":
    main()
