// Copyright 2026 Dolthub, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

use harness::oid::*;
use harness::script::Cell::{Null, Text as T};
use harness::script::{A, Column, Diagnostic, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_index_expressions() {
    run_scripts(&[
        ScriptTest {
            name: "index expressions",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, name TEXT, a INT, b INT);",
                "INSERT INTO t VALUES (1, 'Alice', 1, 2), (2, 'bob', 3, 4), (3, 'CAROL', 5, 6);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX t_lower ON t (lower(name));",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX t_sum ON t ((a + b), id);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX t_upper ON t (upper(name));",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT attname, atttypid FROM pg_attribute WHERE attrelid = 't_sum'::regclass ORDER BY attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME), Column("atttypid", OID)],
                        rows: &[
                            &[T("expr"), T("23")],
                            &[T("id"), T("23")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indkey, indexprs IS NOT NULL FROM pg_index WHERE indexrelid = 't_lower'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("indkey", INT2VECTOR), Column("?column?", BOOL)],
                        rows: &[
                            &[T("0"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (4, 'alice', 7, 8);",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "t_upper""#, detail: "Key (upper(name))=(ALICE) already exists.", schema: "public", table: "t", constraint: "t_upper", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (5, 'Dave', 9, 10);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET name = 'BOB' WHERE id = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE t SET name = 'Dave' WHERE id = 1;",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "t_upper""#, detail: "Key (upper(name))=(DAVE) already exists.", schema: "public", table: "t", constraint: "t_upper", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM t WHERE id = 3;",
                    expected: Expected::Tag("DELETE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (6, 'carol', 0, 0);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", TEXT), Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("Alice"), T("1"), T("2")],
                            &[T("2"), T("BOB"), T("3"), T("4")],
                            &[T("5"), T("Dave"), T("9"), T("10")],
                            &[T("6"), T("carol"), T("0"), T("0")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM t WHERE lower(name) = 'alice';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Index Scan using t_lower on t"#)],
                            &[T(r#"  Index Cond: (lower(name) = 'alice'::text)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t WHERE lower(name) = 'alice';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", TEXT), Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("Alice"), T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT id FROM t WHERE a + b = 19;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Index Scan using t_sum on t"#)],
                            &[T(r#"  Index Cond: ((a + b) = 19)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM t WHERE a + b = 19;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT lower(name) FROM t WHERE lower(name) > 'b' ORDER BY lower(name);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Index Scan using t_lower on t"#)],
                            &[T(r#"  Index Cond: (lower(name) > 'b'::text)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lower(name) FROM t WHERE lower(name) > 'b' ORDER BY lower(name);",
                    expected: Expected::Rows {
                        columns: &[Column("lower", TEXT)],
                        rows: &[
                            &[T("bob")],
                            &[T("carol")],
                            &[T("dave")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM t ORDER BY a + b, id;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Index Scan using t_sum on t"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM t ORDER BY a + b, id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("name", TEXT), Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("6"), T("carol"), T("0"), T("0")],
                            &[T("1"), T("Alice"), T("1"), T("2")],
                            &[T("2"), T("BOB"), T("3"), T("4")],
                            &[T("5"), T("Dave"), T("9"), T("10")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_seqscan;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX t_bad ON t (random());",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: "functions in index expression must be marked IMMUTABLE", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX t_bad2 ON t ((now() > '2000-01-01'));",
                    expected: Expected::Error(Diagnostic { code: "42P17", message: "functions in index expression must be marked IMMUTABLE", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX t_ctid ON t ((ctid::text));",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "index creation on system columns is not supported", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE g (a INT, b INT GENERATED ALWAYS AS (a * 2) VIRTUAL);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX g_b ON g ((b + 1));",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "indexes on virtual generated columns are not supported", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX t_rec ON t ((ROW(a, b)));",
                    expected: Expected::Error(Diagnostic { code: "42P16", message: r#"column "row" has pseudo-type record"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP INDEX t_upper;",
                    expected: Expected::Tag("DROP INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE p (a INT, b TEXT) PARTITION BY RANGE (a);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE p1 PARTITION OF p FOR VALUES FROM (0) TO (10);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX p_lower ON p (lower(b));",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE p2 (b TEXT, a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE p ATTACH PARTITION p2 FOR VALUES FROM (10) TO (20);",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname, i.indkey FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid WHERE i.indrelid IN ('p'::regclass, 'p1'::regclass, 'p2'::regclass) ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("indkey", INT2VECTOR)],
                        rows: &[
                            &[T("p1_lower_idx"), T("0")],
                            &[T("p2_lower_idx"), T("0")],
                            &[T("p_lower"), T("0")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO p VALUES (1, 'X'), (15, 'Y');",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM p ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("X")],
                            &[T("15"), T("Y")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t DROP COLUMN name;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexrelid::regclass FROM pg_index WHERE indrelid = 't'::regclass ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexrelid", REGCLASS)],
                        rows: &[
                            &[T("t_pkey")],
                            &[T("t_sum")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "the statistics of index expressions",
            set_up_script: &[
                "CREATE TABLE n (a INT);",
                "INSERT INTO n VALUES (1);",
                "INSERT INTO n SELECT a + m FROM n, (SELECT max(a) AS m FROM n) x;",
                "INSERT INTO n SELECT a + m FROM n, (SELECT max(a) AS m FROM n) x;",
                "INSERT INTO n SELECT a + m FROM n, (SELECT max(a) AS m FROM n) x;",
                "INSERT INTO n SELECT a + m FROM n, (SELECT max(a) AS m FROM n) x;",
                "INSERT INTO n SELECT a + m FROM n, (SELECT max(a) AS m FROM n) x;",
                "INSERT INTO n SELECT a + m FROM n, (SELECT max(a) AS m FROM n) x;",
                "INSERT INTO n SELECT a + m FROM n, (SELECT max(a) AS m FROM n) x;",
                "INSERT INTO n SELECT a + m FROM n, (SELECT max(a) AS m FROM n) x;",
                "INSERT INTO n SELECT a + m FROM n, (SELECT max(a) AS m FROM n) x;",
                "INSERT INTO n SELECT a + m FROM n, (SELECT max(a) AS m FROM n) x;",
                "CREATE TABLE t (id INT PRIMARY KEY, a INT, b INT);",
                "INSERT INTO t SELECT a, a % 10, a % 7 FROM n;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX t_mod ON t ((a * 100 + b));",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX t_abs ON t (abs(b - 3), id);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ANALYZE t;",
                    expected: Expected::Tag("ANALYZE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT starelid::regclass::text, staattnum, stanullfrac, stadistinct, stakind1, stakind2 FROM pg_statistic WHERE starelid IN ('t_mod'::regclass, 't_abs'::regclass) ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("starelid", TEXT), Column("staattnum", INT2), Column("stanullfrac", FLOAT4), Column("stadistinct", FLOAT4), Column("stakind1", INT2), Column("stakind2", INT2)],
                        rows: &[
                            &[T("t_abs"), T("1"), T("0"), T("4"), T("1"), T("3")],
                            &[T("t_mod"), T("1"), T("0"), T("70"), T("1"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t WHERE a * 100 + b = 305;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("15")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "index expressions over functions, renamed columns, and unique expressions",
            set_up_script: &[
                "CREATE FUNCTION twice(x INT) RETURNS INT LANGUAGE sql IMMUTABLE AS 'SELECT x * 2';",
                "CREATE FUNCTION vol(x INT) RETURNS INT LANGUAGE sql VOLATILE AS 'SELECT x * 2';",
                "CREATE TABLE t (id INT PRIMARY KEY, a INT, b TEXT);",
                "INSERT INTO t VALUES (1, 1, 'x'), (2, 2, 'Y'), (3, 3, 'z');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX t_twice ON t (twice(a));",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX t_vol ON t (vol(a));",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT id FROM t WHERE twice(a) = 4;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Index Scan using t_vol on t"#)],
                            &[T(r#"  Index Cond: ((a * 2) = 4)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM t WHERE twice(a) = 4;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FUNCTION twice(INT);",
                    expected: Expected::Error(Diagnostic { code: "2BP01", message: "cannot drop function twice(integer) because other objects depend on it", detail: "index t_twice depends on function twice(integer)", hint: "Use DROP ... CASCADE to drop the dependent objects too.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP FUNCTION twice(INT) CASCADE;",
                    expected: Expected::Tag("DROP FUNCTION"),
                    notices: &[Diagnostic { code: "00000", message: "drop cascades to index t_twice", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT indexrelid::regclass FROM pg_index WHERE indrelid = 't'::regclass ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("indexrelid", REGCLASS)],
                        rows: &[
                            &[T("t_pkey")],
                            &[T("t_vol")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX t_lower ON t (lower(b));",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t RENAME COLUMN b TO bb;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT id FROM t WHERE lower(bb) = 'y';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Index Scan using t_lower on t"#)],
                            &[T(r#"  Index Cond: (lower(bb) = 'y'::text)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM t WHERE lower(bb) = 'y';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ALTER COLUMN a TYPE BIGINT;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function vol(bigint) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", position: 45, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX t_neg ON t ((-a));",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "ALTER TABLE t ALTER COLUMN a TYPE INT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT id FROM t WHERE -a = -3;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Index Scan using t_neg on t"#)],
                            &[T(r#"  Index Cond: ((- a) = '-3'::integer)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM t WHERE -a = -3;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "REINDEX TABLE t;",
                    expected: Expected::Tag("REINDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM t WHERE lower(bb) = 'z';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("3")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE UNIQUE INDEX t_ulower ON t (lower(bb));",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (4, 4, 'X');",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "t_ulower""#, detail: "Key (lower(bb))=(x) already exists.", schema: "public", table: "t", constraint: "t_ulower", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO t VALUES (5, 5, NULL), (6, 6, NULL);",
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE INDEX t_coll ON t ((bb || 'a') COLLATE "C");"#,
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX t_sub ON t ((SELECT 1));",
                    expected: Expected::Error(Diagnostic { code: "42601", message: r#"syntax error at or near "SELECT""#, position: 27, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX t_agg ON t ((count(*)));",
                    expected: Expected::Error(Diagnostic { code: "42803", message: "aggregate functions are not allowed in index expressions", position: 27, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX t_include ON t (id) INCLUDE ((a + 1));",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "expressions are not supported in included columns", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE u (a INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX u_whole ON u ((u));",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX u_whole2 ON u (((u).a));",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a merge of both branches' inserts into a table with an expression index",
            set_up_script: &[
                "CREATE TABLE t (id INT PRIMARY KEY, name TEXT);",
                "CREATE INDEX t_lower ON t (lower(name));",
                "INSERT INTO t VALUES (1, 'Alice');",
                "SELECT DOLT_COMMIT('-Am', 'base');",
                "SELECT DOLT_CHECKOUT('-b', 'other');",
                "INSERT INTO t VALUES (2, 'BOB'), (3, 'alice');",
                "SELECT DOLT_COMMIT('-am', 'other rows');",
                "SELECT DOLT_CHECKOUT('main');",
                "INSERT INTO t VALUES (4, 'Bob'), (5, 'CAROL');",
                "SELECT DOLT_COMMIT('-am', 'main rows');",
                "SELECT DOLT_MERGE('other');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM t WHERE lower(name) = 'alice' ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM t WHERE lower(name) = 'bob' ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM t WHERE lower(name) > '';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "a query AS OF an older commit evaluates an indexed expression with the function's current body, through a scan or the index",
            set_up_script: &[
                "CREATE FUNCTION f(x INT) RETURNS INT LANGUAGE sql IMMUTABLE AS 'SELECT x * 2';",
                "CREATE TABLE t (id INT PRIMARY KEY, a INT);",
                "CREATE INDEX t_f ON t (f(a));",
                "INSERT INTO t VALUES (1, 1), (2, 2), (3, 3);",
                "SELECT DOLT_COMMIT('-Am', 'doubled');",
                "CREATE OR REPLACE FUNCTION f(x INT) RETURNS INT LANGUAGE sql IMMUTABLE AS 'SELECT x * 3';",
                "REINDEX INDEX t_f;",
                "SELECT DOLT_COMMIT('-am', 'tripled');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT id FROM t AS OF 'HEAD~1' WHERE f(a) = 6;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_bitmapscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM t WHERE f(a) = 6;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM t AS OF 'HEAD~1' WHERE f(a) = 6;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_session_function_index_expressions() {
    run_scripts(&[
        ScriptTest {
            name: "indexes on functions that run in a session, SQL ones that cannot be inlined and PL/pgSQL ones, form their entries read-only with a fixed search path",
            set_up_script: &[
                "CREATE TABLE fi (a INT PRIMARY KEY, b TEXT)",
                "CREATE TABLE fi_log (a INT)",
                "INSERT INTO fi VALUES (1, 'x'), (2, 'y'), (3, 'z')",
                "CREATE FUNCTION fi_sql(INT) RETURNS INT LANGUAGE sql IMMUTABLE AS 'SELECT $1 * 2 LIMIT 1'",
                "CREATE FUNCTION fi_pl(x TEXT) RETURNS TEXT LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN RETURN upper(x) || '!'; END $$",
                "CREATE FUNCTION fi_big(x INT) RETURNS INT LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN IF x > 5 THEN RAISE EXCEPTION 'too big: %', x; END IF; RETURN x; END $$",
                "CREATE FUNCTION fi_wsql(x INT) RETURNS INT LANGUAGE sql IMMUTABLE AS 'INSERT INTO public.fi_log VALUES (1); SELECT $1'",
                "CREATE FUNCTION fi_wpl(x INT) RETURNS INT LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN INSERT INTO public.fi_log VALUES (x); RETURN x; END $$",
                "CREATE FUNCTION fi_helper(INT) RETURNS INT LANGUAGE sql IMMUTABLE AS 'SELECT $1 + 1 LIMIT 1'",
                "CREATE FUNCTION fi_uses_helper(x INT) RETURNS INT LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN RETURN fi_helper(x); END $$",
                "CREATE TABLE fi_late (a INT)",
                "CREATE INDEX fi_late_h ON fi_late (fi_uses_helper(a))",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE INDEX fi_s ON fi (fi_sql(a))",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX fi_p ON fi (fi_pl(b))",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX fi_part ON fi (a) WHERE fi_sql(a) > 2",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO fi VALUES (4, 'w')",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE fi SET b = 'v' WHERE a = 1",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_bitmapscan = off",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM fi WHERE fi_sql(a) = 8",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM fi WHERE fi_pl(b) = 'V!'",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("v")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM fi WHERE fi_sql(a) > 2 AND a < 3 ORDER BY a",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_seqscan",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_bitmapscan",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX fi_b ON fi (fi_big(a))",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO fi VALUES (7, 'u')",
                    expected: Expected::Error(Diagnostic { code: "P0001", message: "too big: 7", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ON fi (fi_wsql(a))",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "INSERT is not allowed in a non-volatile function", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ON fi (fi_wpl(a))",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "INSERT is not allowed in a non-volatile function", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX ON fi (fi_uses_helper(a))",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "function fi_helper(integer) does not exist", hint: "No function matches the given name and argument types. You might need to add explicit type casts.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO fi_late VALUES (1)",
                    expected: Expected::Tag("INSERT 0 1"),
                    skip: Some("index functions always run with the search path pg_catalog, pg_temp, as CREATE INDEX and REINDEX run them, so that every path forms the same entries"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM fi_log",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "notices an index function raises reach the session forming the entries, each time a pooled session runs it",
            set_up_script: &[
                "CREATE FUNCTION nf(x int) RETURNS int LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN RAISE NOTICE 'forming %', x; RETURN x; END $$;",
                "CREATE TABLE nt (id int PRIMARY KEY, a int);",
                "CREATE INDEX nt_f ON nt (nf(a));",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO nt VALUES (1, 10);",
                    notices: &[Diagnostic { code: "00000", message: "forming 10", ..N }],
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO nt VALUES (2, 20), (3, 30);",
                    notices: &[Diagnostic { code: "00000", message: "forming 20", ..N }, Diagnostic { code: "00000", message: "forming 30", ..N }],
                    expected: Expected::Tag("INSERT 0 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE nt SET a = 40 WHERE id = 1;",
                    notices: &[Diagnostic { code: "00000", message: "forming 40", ..N }],
                    expected: Expected::Tag("UPDATE 1"),
                    skip: Some("an UPDATE evaluates the old row's index expressions too, to remove the old entry, where Postgres leaves it for VACUUM (Appendix C)"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_index_functions_replaced_in_a_transaction() {
    run_scripts(&[
        ScriptTest {
            name: "an index expression's function replaced in a transaction that already wrote the table runs its new body",
            set_up_script: &[
                "CREATE FUNCTION rf(x int) RETURNS int LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN RAISE NOTICE 'old %', x; RETURN x; END $$;",
                "CREATE TABLE rt (id int PRIMARY KEY, a int);",
                "CREATE INDEX rt_f ON rt (rf(a));",
            ],
            assertions: &[
                ScriptTestAssertion { query: "BEGIN;", expected: Expected::Tag("BEGIN"), ..A },
                ScriptTestAssertion {
                    query: "INSERT INTO rt VALUES (1, 10);",
                    notices: &[Diagnostic { code: "00000", message: "old 10", ..N }],
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION rf(x int) RETURNS int LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN RAISE NOTICE 'new %', x; RETURN x; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rt VALUES (2, 20);",
                    notices: &[Diagnostic { code: "00000", message: "new 20", ..N }],
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion { query: "COMMIT;", expected: Expected::Tag("COMMIT"), ..A },
            ],
            ..S
        },
        ScriptTest {
            name: "a partial index's predicate function replaced in a transaction that already wrote the table runs its new body",
            set_up_script: &[
                "CREATE FUNCTION rp(x int) RETURNS boolean LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN RAISE NOTICE 'old %', x; RETURN x > 0; END $$;",
                "CREATE TABLE rq (id int PRIMARY KEY, a int);",
                "CREATE INDEX rq_p ON rq (a) WHERE rp(a);",
            ],
            assertions: &[
                ScriptTestAssertion { query: "BEGIN;", expected: Expected::Tag("BEGIN"), ..A },
                ScriptTestAssertion {
                    query: "INSERT INTO rq VALUES (1, 10);",
                    notices: &[Diagnostic { code: "00000", message: "old 10", ..N }],
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE OR REPLACE FUNCTION rp(x int) RETURNS boolean LANGUAGE plpgsql IMMUTABLE AS $$ BEGIN RAISE NOTICE 'new %', x; RETURN x > 0; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO rq VALUES (2, 20);",
                    notices: &[Diagnostic { code: "00000", message: "new 20", ..N }],
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion { query: "COMMIT;", expected: Expected::Tag("COMMIT"), ..A },
            ],
            ..S
        },
        ScriptTest {
            name: "a table written before and after unrelated DDL in one transaction keeps its expression index, partial index, and rows of an earlier layout",
            set_up_script: &[
                "CREATE FUNCTION uf(x int) RETURNS int LANGUAGE sql IMMUTABLE AS 'SELECT x * 2';",
                "CREATE TABLE ut (id int PRIMARY KEY, a int);",
                "CREATE INDEX ut_f ON ut (uf(a));",
                "CREATE INDEX ut_p ON ut (a) WHERE a > 5;",
                "INSERT INTO ut VALUES (1, 1);",
                "ALTER TABLE ut ADD COLUMN b int DEFAULT 7;",
            ],
            assertions: &[
                ScriptTestAssertion { query: "BEGIN;", expected: Expected::Tag("BEGIN"), ..A },
                ScriptTestAssertion {
                    query: "INSERT INTO ut VALUES (2, 10, 3);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE uo (id int);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION ug() RETURNS int LANGUAGE sql AS 'SELECT 1';",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ut VALUES (3, 20);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE ut SET a = a + 6 WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion { query: "SET LOCAL enable_seqscan = off;", expected: Expected::Tag("SET"), ..A },
                ScriptTestAssertion {
                    query: "SELECT id, b FROM ut WHERE uf(a) = 14;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("b", INT4)],
                        rows: &[&[T("1"), T("7")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, a, b FROM ut WHERE a > 5 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", INT4)],
                        rows: &[&[T("1"), T("7"), T("7")], &[T("2"), T("10"), T("3")], &[T("3"), T("20"), T("7")]],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion { query: "COMMIT;", expected: Expected::Tag("COMMIT"), ..A },
                ScriptTestAssertion {
                    query: "SELECT id, a, b FROM ut WHERE uf(a) > 10 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", INT4)],
                        rows: &[&[T("1"), T("7"), T("7")], &[T("2"), T("10"), T("3")], &[T("3"), T("20"), T("7")]],
                        tag: "SELECT 3",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_catalog_changes_rolled_back_to_a_savepoint() {
    run_scripts(&[
        ScriptTest {
            name: "a table made after a savepoint is gone once the transaction rolls back to it",
            assertions: &[
                ScriptTestAssertion { query: "BEGIN;", expected: Expected::Tag("BEGIN"), ..A },
                ScriptTestAssertion { query: "CREATE TABLE sa (x INT);", expected: Expected::Tag("CREATE TABLE"), ..A },
                ScriptTestAssertion {
                    query: "SELECT * FROM sa;",
                    expected: Expected::Rows { columns: &[Column("x", INT4)], rows: &[], tag: "SELECT 0" },
                    ..A
                },
                ScriptTestAssertion { query: "SAVEPOINT s;", expected: Expected::Tag("SAVEPOINT"), ..A },
                ScriptTestAssertion { query: "CREATE TABLE sb (x INT);", expected: Expected::Tag("CREATE TABLE"), ..A },
                ScriptTestAssertion {
                    query: "SELECT * FROM sb;",
                    expected: Expected::Rows { columns: &[Column("x", INT4)], rows: &[], tag: "SELECT 0" },
                    ..A
                },
                ScriptTestAssertion { query: "ROLLBACK TO s;", expected: Expected::Tag("ROLLBACK"), ..A },
                ScriptTestAssertion {
                    query: "SELECT * FROM sb;",
                    expected: Expected::Error(Diagnostic { code: "42P01", message: r#"relation "sb" does not exist"#, position: 15, ..E }),
                    ..A
                },
                ScriptTestAssertion { query: "ROLLBACK;", expected: Expected::Tag("ROLLBACK"), ..A },
            ],
            ..S
        },
        ScriptTest {
            name: "a column added after a savepoint is gone once the transaction rolls back to it",
            set_up_script: &["CREATE TABLE sc (x INT);"],
            assertions: &[
                ScriptTestAssertion { query: "BEGIN;", expected: Expected::Tag("BEGIN"), ..A },
                ScriptTestAssertion { query: "CREATE TABLE sd (x INT);", expected: Expected::Tag("CREATE TABLE"), ..A },
                ScriptTestAssertion {
                    query: "SELECT * FROM sc;",
                    expected: Expected::Rows { columns: &[Column("x", INT4)], rows: &[], tag: "SELECT 0" },
                    ..A
                },
                ScriptTestAssertion { query: "SAVEPOINT s;", expected: Expected::Tag("SAVEPOINT"), ..A },
                ScriptTestAssertion {
                    query: "ALTER TABLE sc ADD COLUMN y INT;",
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM sc;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion { query: "ROLLBACK TO s;", expected: Expected::Tag("ROLLBACK"), ..A },
                ScriptTestAssertion {
                    query: "SELECT * FROM sc;",
                    expected: Expected::Rows { columns: &[Column("x", INT4)], rows: &[], tag: "SELECT 0" },
                    ..A
                },
                ScriptTestAssertion { query: "ROLLBACK;", expected: Expected::Tag("ROLLBACK"), ..A },
            ],
            ..S
        },
        ScriptTest {
            name: "a database's comment set after a savepoint is gone once the transaction rolls back to it",
            assertions: &[
                ScriptTestAssertion { query: "BEGIN;", expected: Expected::Tag("BEGIN"), ..A },
                ScriptTestAssertion { query: "CREATE TABLE se (x INT);", expected: Expected::Tag("CREATE TABLE"), ..A },
                ScriptTestAssertion {
                    query: "SELECT * FROM se;",
                    expected: Expected::Rows { columns: &[Column("x", INT4)], rows: &[], tag: "SELECT 0" },
                    ..A
                },
                ScriptTestAssertion { query: "SAVEPOINT s;", expected: Expected::Tag("SAVEPOINT"), ..A },
                ScriptTestAssertion {
                    query: "COMMENT ON DATABASE postgres IS 'rolled back';",
                    expected: Expected::Tag("COMMENT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT shobj_description(oid, 'pg_database') FROM pg_database WHERE datname = 'postgres';",
                    expected: Expected::Rows {
                        columns: &[Column("shobj_description", TEXT)],
                        rows: &[&[T("rolled back")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion { query: "ROLLBACK TO s;", expected: Expected::Tag("ROLLBACK"), ..A },
                ScriptTestAssertion {
                    query: "SELECT shobj_description(oid, 'pg_database') FROM pg_database WHERE datname = 'postgres';",
                    expected: Expected::Rows {
                        columns: &[Column("shobj_description", TEXT)],
                        rows: &[&[T("default administrative connection database")]],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion { query: "ROLLBACK;", expected: Expected::Tag("ROLLBACK"), ..A },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_catalog_changes_undone_in_a_long_transaction() {
    run_scripts(&[
        ScriptTest {
            name: "a table made before an error under a savepoint is gone once the transaction rolls back to it",
            assertions: &[
                ScriptTestAssertion { query: "BEGIN;", expected: Expected::Tag("BEGIN"), ..A },
                ScriptTestAssertion { query: "CREATE TABLE ua (x INT);", expected: Expected::Tag("CREATE TABLE"), ..A },
                ScriptTestAssertion { query: "SAVEPOINT s;", expected: Expected::Tag("SAVEPOINT"), ..A },
                ScriptTestAssertion { query: "CREATE TABLE ub (x INT);", expected: Expected::Tag("CREATE TABLE"), ..A },
                ScriptTestAssertion {
                    query: "CREATE TABLE ub (x INT);",
                    expected: Expected::Error(Diagnostic { code: "42P07", message: r#"relation "ub" already exists"#, ..E }),
                    ..A
                },
                ScriptTestAssertion { query: "ROLLBACK TO s;", expected: Expected::Tag("ROLLBACK"), ..A },
                ScriptTestAssertion {
                    query: "SELECT to_regclass('ub');",
                    expected: Expected::Rows { columns: &[Column("to_regclass", REGCLASS)], rows: &[&[Null]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion { query: "CREATE TABLE uc (x INT);", expected: Expected::Tag("CREATE TABLE"), ..A },
                ScriptTestAssertion {
                    query: "SELECT relname FROM pg_class WHERE relname IN ('ua', 'ub', 'uc') ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME)],
                        rows: &[&[T("ua")], &[T("uc")]],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion { query: "ROLLBACK;", expected: Expected::Tag("ROLLBACK"), ..A },
            ],
            ..S
        },
        ScriptTest {
            name: "a table a function's exception block undid is gone from the statements after it",
            assertions: &[
                ScriptTestAssertion { query: "BEGIN;", expected: Expected::Tag("BEGIN"), ..A },
                ScriptTestAssertion { query: "CREATE TABLE ud (x INT);", expected: Expected::Tag("CREATE TABLE"), ..A },
                ScriptTestAssertion {
                    query: "DO $$ BEGIN CREATE TABLE ue (x INT); ALTER TABLE ud ADD COLUMN y INT; RAISE EXCEPTION 'undo'; EXCEPTION WHEN OTHERS THEN NULL; END $$;",
                    expected: Expected::Tag("DO"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT to_regclass('ue');",
                    expected: Expected::Rows { columns: &[Column("to_regclass", REGCLASS)], rows: &[&[Null]], tag: "SELECT 1" },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ud;",
                    expected: Expected::Rows { columns: &[Column("x", INT4)], rows: &[], tag: "SELECT 0" },
                    ..A
                },
                ScriptTestAssertion { query: "CREATE TABLE uf (x INT);", expected: Expected::Tag("CREATE TABLE"), ..A },
                ScriptTestAssertion {
                    query: "SELECT relname FROM pg_class WHERE relname IN ('ud', 'ue', 'uf') ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME)],
                        rows: &[&[T("ud")], &[T("uf")]],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion { query: "ROLLBACK;", expected: Expected::Tag("ROLLBACK"), ..A },
            ],
            ..S
        },
    ]);
}
