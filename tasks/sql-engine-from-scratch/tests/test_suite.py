#!/usr/bin/env python3
"""Hidden test suite for sql-engine-from-scratch.

Scoring: 60% correctness + 20% performance + 20% robustness (fraction of
tests passed in each category).  Cell values are compared numerically when
both sides parse as numbers, so "30", "30.0" and "3e1" are all accepted for
the same numeric result; text must match exactly.
"""
import json
import os
import subprocess
import tempfile
import time

MINIDB = "/workspace/minidb"
RESULTS = {"correctness": {}, "performance": {}, "robustness": {}}


def run_sql(db_path, sql, timeout=30):
    proc = subprocess.run(
        [MINIDB, db_path], input=sql, capture_output=True, text=True, timeout=timeout,
        encoding="utf-8", errors="replace",
    )
    return proc.stdout, proc.stderr, proc.returncode


def parse_tsv(output):
    """Returns (header, rows) where rows are lists of strings."""
    lines = [l for l in output.split("\n") if l.strip() != ""]
    if not lines:
        return [], []
    return lines[0].split("\t"), [l.split("\t") for l in lines[1:]]


def cell_eq(got, want):
    try:
        return abs(float(got) - float(want)) < 1e-9 * max(1.0, abs(float(want)))
    except (TypeError, ValueError):
        return got == str(want)


def rows_eq(rows, expected, ordered=True):
    expected = [[str(c) for c in r] for r in expected]
    if len(rows) != len(expected):
        return False
    if not ordered:
        rows = sorted(rows)
        expected = sorted(expected)
    return all(
        len(r) == len(e) and all(cell_eq(a, b) for a, b in zip(r, e))
        for r, e in zip(rows, expected)
    )


class Db:
    """A throw-away database file."""

    def __init__(self):
        fd, self.path = tempfile.mkstemp(suffix=".db")
        os.close(fd)
        os.remove(self.path)

    def sql(self, text, timeout=30):
        return run_sql(self.path, text, timeout)

    def close(self):
        try:
            os.remove(self.path)
        except OSError:
            pass


def record(category, name, ok, detail=""):
    RESULTS[category][name] = "pass" if ok else ("fail" + (f" ({detail})" if detail else ""))
    return 1 if ok else 0


class Suite:
    def __init__(self, category):
        self.category = category
        self.passed = 0
        self.total = 0

    def check(self, name, fn):
        self.total += 1
        try:
            ok = bool(fn())
            self.passed += record(self.category, name, ok)
        except Exception as e:  # timeouts, crashes, decoding errors
            RESULTS[self.category][name] = f"error: {e}"

    @property
    def score(self):
        return self.passed / max(self.total, 1)


def q_rows(db, query):
    out, _, rc = db.sql(query)
    return rc, parse_tsv(out)


USERS = [
    (1, "Alice", 30), (2, "Bob", 25), (3, "Charlie", 35), (4, "Diana", 28), (5, "Eve", 32),
]


def users_db():
    db = Db()
    db.sql("CREATE TABLE users (id INTEGER, name TEXT, age INTEGER);\n" + "\n".join(
        f"INSERT INTO users VALUES ({i}, '{n}', {a});" for i, n, a in USERS))
    return db


def test_correctness():
    s = Suite("correctness")

    # ---- DDL / DML basics ----
    db = Db()
    s.check("create_table", lambda: db.sql("CREATE TABLE users (id INTEGER, name TEXT, age INTEGER);")[2] == 0)
    s.check("insert_single", lambda: db.sql("INSERT INTO users VALUES (1, 'Alice', 30);")[2] == 0)
    s.check("insert_multiple", lambda: db.sql("\n".join(
        f"INSERT INTO users VALUES ({i}, '{n}', {a});" for i, n, a in USERS[1:]))[2] == 0)
    s.check("insert_column_list", lambda: (
        db.sql("INSERT INTO users (name, id) VALUES ('Zed', 6);")[2] == 0
        and rows_eq(q_rows(db, "SELECT id, name FROM users WHERE id = 6;")[1][1], [[6, "Zed"]])))
    db.close()

    # ---- SELECT / WHERE / ORDER / LIMIT ----
    db = users_db()
    s.check("select_all", lambda: (lambda r: r[1][0] == ["id", "name", "age"]
            and rows_eq(r[1][1], [list(u) for u in USERS]))(q_rows(db, "SELECT * FROM users ORDER BY id;")))
    s.check("select_columns", lambda: (lambda r: r[1][0] == ["name", "age"]
            and rows_eq(r[1][1], [[n, a] for _, n, a in USERS], ordered=False))(
        q_rows(db, "SELECT name, age FROM users;")))
    s.check("where_eq", lambda: rows_eq(q_rows(db, "SELECT name FROM users WHERE name = 'Bob';")[1][1], [["Bob"]]))
    s.check("where_ne", lambda: rows_eq(q_rows(db, "SELECT id FROM users WHERE id != 3 AND id <= 4 ORDER BY id;")[1][1],
                                        [[1], [2], [4]]))
    s.check("where_lt", lambda: rows_eq(q_rows(db, "SELECT name FROM users WHERE age < 30;")[1][1],
                                        [["Bob"], ["Diana"]], ordered=False))
    s.check("where_ge", lambda: rows_eq(q_rows(db, "SELECT name FROM users WHERE age >= 32 ORDER BY name;")[1][1],
                                        [["Charlie"], ["Eve"]]))
    s.check("where_and", lambda: rows_eq(q_rows(db, "SELECT name FROM users WHERE age > 25 AND age < 33 ORDER BY age;")[1][1],
                                         [["Diana"], ["Alice"], ["Eve"]]))
    s.check("where_or", lambda: rows_eq(q_rows(db, "SELECT name FROM users WHERE name = 'Alice' OR name = 'Eve' ORDER BY name;")[1][1],
                                        [["Alice"], ["Eve"]]))
    s.check("where_not", lambda: rows_eq(q_rows(db, "SELECT id FROM users WHERE NOT age > 28 ORDER BY id;")[1][1],
                                         [[2], [4]]))
    s.check("where_precedence", lambda: rows_eq(
        q_rows(db, "SELECT id FROM users WHERE id = 1 OR id = 2 AND age > 100 ORDER BY id;")[1][1], [[1]]))
    s.check("where_parentheses", lambda: rows_eq(
        q_rows(db, "SELECT id FROM users WHERE (id = 1 OR id = 2) AND age < 30;")[1][1], [[2]]))
    s.check("where_between", lambda: rows_eq(
        q_rows(db, "SELECT id FROM users WHERE id BETWEEN 2 AND 4 ORDER BY id;")[1][1], [[2], [3], [4]]))
    s.check("order_by_asc", lambda: [r[0] for r in q_rows(db, "SELECT name FROM users ORDER BY age ASC;")[1][1]]
            == ["Bob", "Diana", "Alice", "Eve", "Charlie"])
    s.check("order_by_desc", lambda: [r[0] for r in q_rows(db, "SELECT name FROM users ORDER BY age DESC;")[1][1]]
            == ["Charlie", "Eve", "Alice", "Diana", "Bob"])
    s.check("order_by_text", lambda: [r[0] for r in q_rows(db, "SELECT name FROM users ORDER BY name DESC;")[1][1]]
            == ["Eve", "Diana", "Charlie", "Bob", "Alice"])
    s.check("limit", lambda: [r[0] for r in q_rows(db, "SELECT id FROM users ORDER BY id ASC LIMIT 3;")[1][1]] == ["1", "2", "3"])
    s.check("limit_offset", lambda: [r[0] for r in q_rows(db, "SELECT id FROM users ORDER BY id ASC LIMIT 2 OFFSET 2;")[1][1]]
            == ["3", "4"])
    s.check("expression_in_select", lambda: rows_eq(
        q_rows(db, "SELECT age + 1 FROM users WHERE id = 1;")[1][1], [[31]]))

    # ---- aggregates ----
    s.check("count", lambda: rows_eq(q_rows(db, "SELECT COUNT(*) FROM users;")[1][1], [[5]]))
    s.check("count_where", lambda: rows_eq(q_rows(db, "SELECT COUNT(*) FROM users WHERE age > 28;")[1][1], [[3]]))
    s.check("sum", lambda: rows_eq(q_rows(db, "SELECT SUM(age) FROM users;")[1][1], [[150]]))
    s.check("avg", lambda: rows_eq(q_rows(db, "SELECT AVG(age) FROM users;")[1][1], [[30]]))
    s.check("min", lambda: rows_eq(q_rows(db, "SELECT MIN(age) FROM users;")[1][1], [[25]]))
    s.check("max", lambda: rows_eq(q_rows(db, "SELECT MAX(age) FROM users;")[1][1], [[35]]))
    s.check("multiple_aggregates", lambda: rows_eq(
        q_rows(db, "SELECT COUNT(*), MIN(age), MAX(age) FROM users;")[1][1], [[5, 25, 35]]))
    db.close()

    # ---- GROUP BY / HAVING ----
    db = Db()
    db.sql("\n".join([
        "CREATE TABLE sales (id INTEGER, region TEXT, amount REAL);",
        "INSERT INTO sales VALUES (1, 'East', 100.0);",
        "INSERT INTO sales VALUES (2, 'West', 200.0);",
        "INSERT INTO sales VALUES (3, 'East', 150.0);",
        "INSERT INTO sales VALUES (4, 'West', 250.0);",
        "INSERT INTO sales VALUES (5, 'East', 120.0);",
    ]))
    s.check("group_by", lambda: rows_eq(
        q_rows(db, "SELECT region, COUNT(*), SUM(amount) FROM sales GROUP BY region ORDER BY region;")[1][1],
        [["East", 3, 370], ["West", 2, 450]]))
    s.check("having", lambda: rows_eq(
        q_rows(db, "SELECT region, SUM(amount) FROM sales GROUP BY region HAVING SUM(amount) > 400;")[1][1],
        [["West", 450]]))
    s.check("having_count", lambda: rows_eq(
        q_rows(db, "SELECT region FROM sales GROUP BY region HAVING COUNT(*) >= 3;")[1][1], [["East"]]))
    s.check("order_by_aggregate", lambda: [r[0] for r in q_rows(
        db, "SELECT region, AVG(amount) FROM sales GROUP BY region ORDER BY AVG(amount) DESC;")[1][1]]
        == ["West", "East"])
    db.close()

    # ---- UPDATE / DELETE ----
    db = Db()
    db.sql("\n".join([
        "CREATE TABLE items (id INTEGER, name TEXT, price REAL);",
        "INSERT INTO items VALUES (1, 'Apple', 1.50);",
        "INSERT INTO items VALUES (2, 'Banana', 0.75);",
        "INSERT INTO items VALUES (3, 'Cherry', 3.00);",
    ]))
    db.sql("UPDATE items SET price = 2.00 WHERE name = 'Apple';")
    s.check("update", lambda: rows_eq(q_rows(db, "SELECT name, price FROM items ORDER BY id;")[1][1],
                                      [["Apple", 2.0], ["Banana", 0.75], ["Cherry", 3.0]]))
    db.sql("UPDATE items SET price = price * 2, name = 'Fruit' WHERE id > 1;")
    s.check("update_multiple_rows_and_columns", lambda: rows_eq(
        q_rows(db, "SELECT name, price FROM items ORDER BY id;")[1][1],
        [["Apple", 2.0], ["Fruit", 1.5], ["Fruit", 6.0]]))
    db.sql("DELETE FROM items WHERE name = 'Fruit' AND price > 5;")
    s.check("delete", lambda: rows_eq(q_rows(db, "SELECT id FROM items ORDER BY id;")[1][1], [[1], [2]]))
    db.sql("DELETE FROM items;")
    s.check("delete_all", lambda: rows_eq(q_rows(db, "SELECT * FROM items;")[1][1], []))
    db.close()

    # ---- JOIN ----
    db = Db()
    db.sql("\n".join([
        "CREATE TABLE departments (dept_id INTEGER, dept_name TEXT);",
        "INSERT INTO departments VALUES (1, 'Engineering');",
        "INSERT INTO departments VALUES (2, 'Marketing');",
        "INSERT INTO departments VALUES (3, 'Empty');",
        "CREATE TABLE employees (emp_id INTEGER, emp_name TEXT, dept_id INTEGER);",
        "INSERT INTO employees VALUES (101, 'Alice', 1);",
        "INSERT INTO employees VALUES (102, 'Bob', 2);",
        "INSERT INTO employees VALUES (103, 'Charlie', 1);",
        "INSERT INTO employees VALUES (104, 'Dave', 9);",
    ]))
    s.check("inner_join", lambda: rows_eq(q_rows(
        db, "SELECT emp_name, dept_name FROM employees INNER JOIN departments "
            "ON employees.dept_id = departments.dept_id ORDER BY emp_id;")[1][1],
        [["Alice", "Engineering"], ["Bob", "Marketing"], ["Charlie", "Engineering"]]))
    s.check("join_with_where", lambda: rows_eq(q_rows(
        db, "SELECT e.emp_name FROM employees e INNER JOIN departments d ON e.dept_id = d.dept_id "
            "WHERE d.dept_name = 'Engineering' ORDER BY e.emp_id;")[1][1], [["Alice"], ["Charlie"]]))
    s.check("join_aggregate", lambda: rows_eq(q_rows(
        db, "SELECT dept_name, COUNT(*) FROM employees INNER JOIN departments "
            "ON employees.dept_id = departments.dept_id GROUP BY dept_name ORDER BY dept_name;")[1][1],
        [["Engineering", 2], ["Marketing", 1]]))
    s.check("join_select_star_header", lambda: q_rows(
        db, "SELECT * FROM employees INNER JOIN departments ON employees.dept_id = departments.dept_id;"
    )[1][0] == ["emp_id", "emp_name", "dept_id", "dept_id", "dept_name"])

    # ---- schema commands ----
    s.check("dot_tables", lambda: sorted(l.strip() for l in db.sql(".tables")[0].split() if l.strip())
            == ["departments", "employees"])
    s.check("dot_schema", lambda: (lambda o: "CREATE TABLE" in o and "dept_name" in o and "employees" not in o)(
        db.sql(".schema departments")[0]))
    s.check("drop_table", lambda: db.sql("DROP TABLE departments;")[2] == 0
            and "departments" not in db.sql(".tables")[0]
            and db.sql("SELECT * FROM departments;")[2] != 0)
    db.close()

    # ---- persistence & batch mode ----
    db = Db()
    db.sql("CREATE TABLE persist (id INTEGER, val TEXT);\nINSERT INTO persist VALUES (1, 'hello');")
    db.sql("INSERT INTO persist VALUES (2, 'world');")
    s.check("persistence_across_processes", lambda: rows_eq(
        q_rows(db, "SELECT val FROM persist ORDER BY id;")[1][1], [["hello"], ["world"]]))
    out, _, rc = db.sql("CREATE TABLE b (x INTEGER);\nINSERT INTO b VALUES (1); INSERT INTO b VALUES (2);\n"
                        "SELECT SUM(x) FROM b;\nSELECT COUNT(*) FROM b;")
    s.check("batch_mode_multiple_statements", lambda: rc == 0 and out.split() == ["SUM(x)", "3", "COUNT(*)", "2"])
    s.check("multiline_statement", lambda: rows_eq(q_rows(
        db, "SELECT\n  val\nFROM\n  persist\nWHERE id = 1\n;")[1][1], [["hello"]]))
    db.close()

    # ---- B-tree: many rows force page splits; order and content must survive ----
    db = Db()
    n = 3000
    db.sql("CREATE TABLE big (id INTEGER, label TEXT);\n" + "\n".join(
        f"INSERT INTO big VALUES ({i}, 'row-{i}-{'x' * (i % 40)}');" for i in range(n)))
    s.check("many_rows_count", lambda: rows_eq(q_rows(db, "SELECT COUNT(*), MIN(id), MAX(id) FROM big;")[1][1],
                                               [[n, 0, n - 1]]))
    s.check("many_rows_point_lookup", lambda: rows_eq(
        q_rows(db, "SELECT label FROM big WHERE id = 1234;")[1][1], [[f"row-1234-{'x' * (1234 % 40)}"]]))
    db.sql("DELETE FROM big WHERE id % 2 = 0;")
    db.sql("INSERT INTO big VALUES (-1, 'late');")
    s.check("many_rows_delete_then_insert", lambda: rows_eq(
        q_rows(db, "SELECT COUNT(*), SUM(id) FROM big;")[1][1], [[n // 2 + 1, sum(range(1, n, 2)) - 1]]))
    db.close()

    # ---- text longer than a typical fixed buffer ----
    db = Db()
    long_text = "L" * 500
    db.sql(f"CREATE TABLE t (id INTEGER, body TEXT);\nINSERT INTO t VALUES (1, '{long_text}');")
    s.check("long_text_roundtrip", lambda: rows_eq(q_rows(db, "SELECT body FROM t;")[1][1], [[long_text]]))
    db.close()

    return s.score


def test_performance():
    passed = 0
    total = 5
    db = Db()

    lines = ["CREATE TABLE large (id INTEGER, name TEXT, value REAL);"]
    for i in range(100000):
        lines.append(f"INSERT INTO large VALUES ({i}, 'name_{i}', {i * 1.5});")
    sql = "\n".join(lines)

    def timed(name, query_or_sql, limit, timeout, to_seconds=1.0):
        nonlocal passed
        start = time.time()
        try:
            out, _, rc = db.sql(query_or_sql, timeout=timeout)
            elapsed = time.time() - start
            ok = rc == 0 and elapsed < limit
            RESULTS["performance"][name] = f"{'pass' if ok else 'fail'} ({elapsed * to_seconds:.2f}s)"
            passed += 1 if ok else 0
            return out
        except Exception as e:
            RESULTS["performance"][name] = f"error: {e}"
            return ""

    timed("bulk_insert_100k", sql, 30, 60)

    out = timed("point_query", "SELECT * FROM large WHERE id = 50000;", 0.05, 5)
    if "name_50000" not in out and RESULTS["performance"].get("point_query", "").startswith("pass"):
        RESULTS["performance"]["point_query"] = "fail (wrong result)"
        passed -= 1

    timed("range_scan", "SELECT * FROM large WHERE id >= 10000 AND id < 11000;", 0.2, 5)
    timed("aggregation", "SELECT COUNT(*), AVG(value) FROM large;", 2, 10)
    db.close()

    db = Db()
    lines = ["CREATE TABLE t1 (id INTEGER, val TEXT);",
             "CREATE TABLE t2 (id INTEGER, ref_id INTEGER, data TEXT);"]
    lines += [f"INSERT INTO t1 VALUES ({i}, 'val_{i}');" for i in range(10000)]
    lines += [f"INSERT INTO t2 VALUES ({i}, {i % 1000}, 'data_{i}');" for i in range(10000)]
    try:
        db.sql("\n".join(lines), timeout=120)
    except Exception:
        pass
    out = timed("join_10k",
                "SELECT t1.val, t2.data FROM t1 INNER JOIN t2 ON t1.id = t2.ref_id WHERE t1.id < 100;", 5, 30)
    if len([l for l in out.split("\n") if l.strip()]) != 1001 \
            and RESULTS["performance"].get("join_10k", "").startswith("pass"):
        RESULTS["performance"]["join_10k"] = "fail (wrong row count)"
        passed -= 1
    db.close()

    return passed / total


def test_robustness():
    s = Suite("robustness")

    db = Db()
    db.sql("CREATE TABLE empty_t (id INTEGER, name TEXT);")
    s.check("empty_table_select", lambda: (lambda r: r[0] == 0 and len(r[1][1]) == 0)(q_rows(db, "SELECT * FROM empty_t;")))
    s.check("empty_table_aggregates", lambda: (lambda r: r[0] == 0 and r[1][1][0][0] == "0" if r[1][1] else False)(
        q_rows(db, "SELECT COUNT(*) FROM empty_t;")))

    def syntax_error():
        _, err, rc = db.sql("SELEC * FORM users;")
        return rc != 0 and "error" in err.lower()
    s.check("syntax_error", syntax_error)

    def no_such_table():
        _, err, rc = db.sql("SELECT * FROM nonexistent;")
        return rc != 0 and "error" in err.lower()
    s.check("no_such_table", no_such_table)

    def no_such_column():
        _, err, rc = db.sql("SELECT nope FROM empty_t;")
        return rc != 0 and "error" in err.lower()
    s.check("no_such_column", no_such_column)

    def duplicate_table():
        _, err, rc = db.sql("CREATE TABLE empty_t (a INTEGER);")
        return rc != 0 and "error" in err.lower()
    s.check("duplicate_table", duplicate_table)

    def error_does_not_stop_batch():
        out, err, rc = db.sql("SELECT * FROM missing;\nINSERT INTO empty_t VALUES (1, 'ok');\nSELECT name FROM empty_t;")
        return rc != 0 and "error" in err.lower() and out.split() == ["name", "ok"]
    s.check("error_does_not_stop_batch", error_does_not_stop_batch)

    def wrong_value_count():
        _, err, rc = db.sql("INSERT INTO empty_t VALUES (1);")
        return rc != 0 and "error" in err.lower()
    s.check("wrong_value_count", wrong_value_count)
    db.close()

    db = Db()
    db.sql("CREATE TABLE strings (id INTEGER, val TEXT);")
    db.sql("INSERT INTO strings VALUES (1, 'it''s a test');")
    s.check("special_chars_quote", lambda: rows_eq(q_rows(db, "SELECT val FROM strings WHERE id = 1;")[1][1], [["it's a test"]]))
    db.sql("INSERT INTO strings VALUES (2, 'semi;colon -- not a comment');")
    s.check("special_chars_semicolon", lambda: rows_eq(
        q_rows(db, "SELECT val FROM strings WHERE id = 2;")[1][1], [["semi;colon -- not a comment"]]))
    db.sql("INSERT INTO strings VALUES (3, 'h\u00e9llo w\u00f6rld \u2713');")
    s.check("utf8_text", lambda: rows_eq(q_rows(db, "SELECT val FROM strings WHERE id = 3;")[1][1],
                                         [["h\u00e9llo w\u00f6rld \u2713"]]))
    db.close()

    db = Db()
    db.sql("CREATE TABLE reals (id INTEGER, val REAL);")
    db.sql("INSERT INTO reals VALUES (1, 3.14159);")
    s.check("real_precision", lambda: q_rows(db, "SELECT val FROM reals WHERE id = 1;")[1][1][0][0].startswith("3.14159"))
    db.sql("INSERT INTO reals VALUES (2, -0.5);\nINSERT INTO reals VALUES (3, 1e3);")
    s.check("real_negative_and_exponent", lambda: rows_eq(
        q_rows(db, "SELECT val FROM reals WHERE id > 1 ORDER BY id;")[1][1], [[-0.5], [1000.0]]))
    db.close()

    db = Db()
    db.sql("CREATE TABLE n (id INTEGER, v INTEGER);\nINSERT INTO n VALUES (1, 10);\n"
           "INSERT INTO n VALUES (2, NULL);\nINSERT INTO n VALUES (3, 30);")
    s.check("null_is_null", lambda: rows_eq(q_rows(db, "SELECT id FROM n WHERE v IS NULL;")[1][1], [[2]]))
    s.check("null_is_not_null", lambda: rows_eq(q_rows(db, "SELECT id FROM n WHERE v IS NOT NULL ORDER BY id;")[1][1],
                                                [[1], [3]]))
    s.check("null_comparison_is_unknown", lambda: rows_eq(
        q_rows(db, "SELECT id FROM n WHERE v != 10 ORDER BY id;")[1][1], [[3]]))
    s.check("null_ignored_by_aggregates", lambda: rows_eq(
        q_rows(db, "SELECT COUNT(*), COUNT(v), SUM(v), AVG(v) FROM n;")[1][1], [[3, 2, 40, 20]]))
    db.close()

    db = Db()
    out, _, rc = db.sql("create table T (Id integer, Name text); -- comment\n"
                        "INSERT into t VALUES (1, 'a');  /* block */  select NAME from T where ID = 1;")
    s.check("case_insensitive_and_comments", lambda: rc == 0 and out.split() == ["Name", "a"] or out.split() == ["NAME", "a"])
    db.close()

    db = Db()
    path = db.path
    with open(path, "wb") as f:
        f.write(b"this is definitely not a database file" * 200)
    out, err, rc = run_sql(path, "SELECT 1;")
    s.check("rejects_non_database_file", lambda: rc != 0)
    db.close()

    return s.score


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

    print(f"\n{'=' * 50}")
    print(f"Correctness: {c_score:.3f} (weight: 60%)")
    print(f"Performance: {p_score:.3f} (weight: 20%)")
    print(f"Robustness:  {r_score:.3f} (weight: 20%)")
    print(f"{'=' * 50}")
    print(f"Final Score: {final_score:.3f}")
    for category, items in RESULTS.items():
        bad = {k: v for k, v in items.items() if v != "pass" and not v.startswith("pass")}
        if bad:
            print(f"\n{category} failures:")
            for k, v in bad.items():
                print(f"  {k}: {v}")

    os.makedirs("/logs/verifier", exist_ok=True)
    with open("/logs/verifier/reward.txt", "w") as f:
        f.write(f"{final_score:.6f}")
    with open("/logs/verifier/reward.json", "w") as f:
        json.dump({
            "score": final_score,
            "correctness": c_score,
            "performance": p_score,
            "robustness": r_score,
            "details": RESULTS,
        }, f, indent=2)


if __name__ == "__main__":
    main()
