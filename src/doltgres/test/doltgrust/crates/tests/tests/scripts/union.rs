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
use harness::pgx::Time;
use harness::plan::PlanFact;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, BindVar, Column, Diagnostic, E, Expected, Flow, N, S, ScriptTest, ScriptTestAssertion, USER_DEFINED, run_scripts, run_scripts_repeated};
use harness::wire::{Datum, F, Field, Fields, PGX_STARTUP, Receive, Send, Step, W, WireTest, run_wire_tests};

#[test]
fn test_except() {
    run_scripts(&[
        ScriptTest {
            name: "except tests",
            set_up_script: &[
                "CREATE TABLE t1 (i INT PRIMARY KEY);",
                "CREATE TABLE t2 (j INT PRIMARY KEY);",
                "INSERT INTO t1 VALUES (1), (2), (3);",
                "INSERT INTO t2 VALUES (2), (3), (4);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 EXCEPT SELECT * FROM t2;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 123 EXCEPT SELECT 456;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("123")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (VALUES (123), (456)) a EXCEPT SELECT * FROM (VALUES (456), (789)) b;",
                    expected: Expected::Rows {
                        columns: &[Column("column1", INT4)],
                        rows: &[
                            &[T("123")],
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
fn test_intersect() {
    run_scripts(&[
        ScriptTest {
            name: "intersect tests",
            set_up_script: &[
                "CREATE TABLE t1 (i INT PRIMARY KEY);",
                "CREATE TABLE t2 (j INT PRIMARY KEY);",
                "INSERT INTO t1 VALUES (1), (2), (3);",
                "INSERT INTO t2 VALUES (2), (3), (4);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 INTERSECT SELECT * FROM t2;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 123 INTERSECT SELECT 456;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (VALUES (123), (456)) a INTERSECT SELECT * FROM (VALUES (456), (789)) b;",
                    expected: Expected::Rows {
                        columns: &[Column("column1", INT4)],
                        rows: &[
                            &[T("456")],
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
fn test_union() {
    run_scripts(&[
        ScriptTest {
            name: "union tests",
            set_up_script: &[
                "CREATE TABLE t1 (i INT PRIMARY KEY);",
                "CREATE TABLE t2 (j INT PRIMARY KEY);",
                "INSERT INTO t1 VALUES (1), (2), (3);",
                "INSERT INTO t2 VALUES (2), (3), (4);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 UNION SELECT * FROM t2;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("4")],
                            &[T("1")],
                            &[T("3")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 123 UNION SELECT 456;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("123")],
                            &[T("456")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (VALUES (123), (456)) a UNION SELECT * FROM (VALUES (456), (789)) b;",
                    expected: Expected::Rows {
                        columns: &[Column("column1", INT4)],
                        rows: &[
                            &[T("456")],
                            &[T("123")],
                            &[T("789")],
                        ],
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
fn test_union_planning() {
    run_scripts(&[
        ScriptTest {
            name: "UNION and UNION ALL over tables, views, and subqueries",
            set_up_script: &[
                "CREATE TABLE u1 (a int PRIMARY KEY, b text, c varchar(3)) WITH (autovacuum_enabled = false);",
                "CREATE TABLE u2 (a int PRIMARY KEY, b text, c varchar(5)) WITH (autovacuum_enabled = false);",
                "INSERT INTO u1 VALUES (1, 'x', 'aa'), (2, 'y', 'bb'), (3, 'z', NULL);",
                "INSERT INTO u2 VALUES (2, 'y', 'bb'), (3, 'w', 'ccccc'), (4, NULL, 'dd');",
                "CREATE VIEW uv AS SELECT a, b FROM u1 UNION ALL SELECT a, b FROM u2;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT a, b FROM u1 UNION SELECT a, b FROM u2 ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                            &[T("2"), T("y")],
                            &[T("3"), T("w")],
                            &[T("3"), T("z")],
                            &[T("4"), Null],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM u1 UNION ALL SELECT a, b FROM u2 ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                            &[T("2"), T("y")],
                            &[T("2"), T("y")],
                            &[T("3"), T("w")],
                            &[T("3"), T("z")],
                            &[T("4"), Null],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT b FROM u1 UNION SELECT b FROM u2 ORDER BY b NULLS FIRST;",
                    expected: Expected::Rows {
                        columns: &[Column("b", TEXT)],
                        rows: &[
                            &[Null],
                            &[T("w")],
                            &[T("x")],
                            &[T("y")],
                            &[T("z")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM u1 UNION ALL SELECT a FROM u2 UNION SELECT a + 10 FROM u1 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("4")],
                            &[T("11")],
                            &[T("12")],
                            &[T("13")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM u1 UNION SELECT a FROM u2 UNION ALL SELECT a FROM u1 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("1")],
                            &[T("2")],
                            &[T("2")],
                            &[T("3")],
                            &[T("3")],
                            &[T("4")],
                        ],
                        tag: "SELECT 7",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM u1 UNION SELECT a FROM u2 ORDER BY 1 DESC LIMIT 2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("4")],
                            &[T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM u1 UNION SELECT a FROM u2 ORDER BY 1 LIMIT 2 OFFSET 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c FROM u1 UNION SELECT c FROM u2 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("c", VARCHAR)],
                        rows: &[
                            &[T("aa")],
                            &[T("bb")],
                            &[T("ccccc")],
                            &[T("dd")],
                            &[Null],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pg_typeof(c), count(*) FROM (SELECT c FROM u1 UNION ALL SELECT c FROM u2) s GROUP BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("pg_typeof", REGTYPE), Column("count", INT8)],
                        rows: &[
                            &[T("character varying"), T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1 AS x UNION SELECT 2.5 ORDER BY x;",
                    expected: Expected::Rows {
                        columns: &[Column("x", NUMERIC)],
                        rows: &[
                            &[T("1")],
                            &[T("2.5")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1 UNION SELECT NULL ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("1")],
                            &[Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 'abc' UNION SELECT 'abd' ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", TEXT)],
                        rows: &[
                            &[T("abc")],
                            &[T("abd")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM uv WHERE a > 2 ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("3"), T("w")],
                            &[T("3"), T("z")],
                            &[T("4"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM uv;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("6")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT a FROM u1 UNION ALL SELECT a * 10 FROM u2) s WHERE a < 25 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("20")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT s.a, u1.b FROM (SELECT a FROM u1 UNION ALL SELECT a FROM u2) s JOIN u1 USING (a) ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                            &[T("2"), T("y")],
                            &[T("2"), T("y")],
                            &[T("3"), T("z")],
                            &[T("3"), T("z")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM u1 WHERE a IN (SELECT a FROM u2 UNION SELECT 1) ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT), Column("c", VARCHAR)],
                        rows: &[
                            &[T("1"), T("x"), T("aa")],
                            &[T("2"), T("y"), T("bb")],
                            &[T("3"), T("z"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (SELECT max(a) FROM (SELECT a FROM u1 UNION SELECT a FROM u2) s);",
                    expected: Expected::Rows {
                        columns: &[Column("max", INT4)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, (SELECT b FROM u2 WHERE u2.a = u1.a UNION ALL SELECT 'none' LIMIT 1) FROM u1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("none")],
                            &[T("2"), T("y")],
                            &[T("3"), T("w")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM u1 UNION SELECT a FROM u2 FOR UPDATE;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "FOR UPDATE is not allowed with UNION/INTERSECT/EXCEPT", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM u1 UNION SELECT a FROM u2;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "each UNION query must have the same number of columns", position: 34, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM u1 UNION SELECT b FROM u2;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "UNION types integer and text cannot be matched", position: 31, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a, b FROM uv WHERE a > 2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Append")],
                            &[T("  ->  Bitmap Heap Scan on u1")],
                            &[T("        Recheck Cond: (a > 2)")],
                            &[T("        ->  Bitmap Index Scan on u1_pkey")],
                            &[T("              Index Cond: (a > 2)")],
                            &[T("  ->  Bitmap Heap Scan on u2")],
                            &[T("        Recheck Cond: (a > 2)")],
                            &[T("        ->  Bitmap Index Scan on u2_pkey")],
                            &[T("              Index Cond: (a > 2)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    skip: Some("a table without statistics gets an Index Scan where PG18 plans a Bitmap Heap Scan, as a plain SELECT of it does"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a FROM u1 UNION ALL SELECT a FROM u2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Append")],
                            &[T("  ->  Seq Scan on u1")],
                            &[T("  ->  Seq Scan on u2")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a, b FROM u1 UNION SELECT a, b FROM u2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate")],
                            &[T("  Group Key: u1.a, u1.b")],
                            &[T("  ->  Append")],
                            &[T("        ->  Seq Scan on u1")],
                            &[T("        ->  Seq Scan on u2")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT 1 UNION SELECT 2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Unique")],
                            &[T("  ->  Sort")],
                            &[T("        Sort Key: (1)")],
                            &[T("        ->  Append")],
                            &[T("              ->  Result")],
                            &[T("              ->  Result")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a FROM u1 UNION ALL SELECT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Append")],
                            &[T("  ->  Seq Scan on u1")],
                            &[T("  ->  Result")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM u1 UNION ALL SELECT 1 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a, b FROM uv WHERE b = 'y';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Append")],
                            &[T("  ->  Seq Scan on u1")],
                            &[T("        Filter: (b = 'y'::text)")],
                            &[T("  ->  Seq Scan on u2")],
                            &[T("        Filter: (b = 'y'::text)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_intersect_except_planning() {
    run_scripts(&[
        ScriptTest {
            name: "INTERSECT and EXCEPT, with and without ALL, hashed and sorted",
            set_up_script: &[
                "CREATE TABLE s1 (a int, b text) WITH (autovacuum_enabled = false);",
                "CREATE TABLE s2 (a int, b text) WITH (autovacuum_enabled = false);",
                "INSERT INTO s1 VALUES (1, 'x'), (2, 'y'), (2, 'y'), (2, 'y'), (3, 'z'), (NULL, 'n'), (NULL, 'n');",
                "INSERT INTO s2 VALUES (2, 'y'), (2, 'y'), (3, 'w'), (4, 'v'), (NULL, 'n');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT a, b FROM s1 INTERSECT SELECT a, b FROM s2 ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("2"), T("y")],
                            &[Null, T("n")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM s1 INTERSECT ALL SELECT a, b FROM s2 ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("2"), T("y")],
                            &[T("2"), T("y")],
                            &[Null, T("n")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM s1 EXCEPT SELECT a, b FROM s2 ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                            &[T("3"), T("z")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM s1 EXCEPT ALL SELECT a, b FROM s2 ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                            &[T("2"), T("y")],
                            &[T("3"), T("z")],
                            &[Null, T("n")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM s2 EXCEPT SELECT a FROM s1 ORDER BY 1;",
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
                    query: "SELECT a FROM s1 INTERSECT SELECT a FROM s2 UNION SELECT 10 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("3")],
                            &[T("10")],
                            &[Null],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM s1 UNION ALL SELECT a FROM s2 INTERSECT SELECT 3 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("2")],
                            &[T("2")],
                            &[T("3")],
                            &[T("3")],
                            &[Null],
                            &[Null],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM s1 EXCEPT ALL SELECT a FROM s2 EXCEPT ALL SELECT 2 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[Null],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1.5 INTERSECT SELECT 1.50;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", NUMERIC)],
                        rows: &[
                            &[T("1.5")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a::numeric FROM s1 INTERSECT SELECT 2.0 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", NUMERIC)],
                        rows: &[
                            &[T("2.0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM (SELECT a FROM s1 INTERSECT ALL SELECT a FROM s2) s;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("4")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM s1 WHERE a IN (SELECT a FROM s1 EXCEPT SELECT 1) ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("2"), T("y")],
                            &[T("2"), T("y")],
                            &[T("2"), T("y")],
                            &[T("3"), T("z")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM s1 INTERSECT SELECT a FROM s2 ORDER BY 1 DESC LIMIT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[Null],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM s1 EXCEPT SELECT b FROM s2;",
                    expected: Expected::Error(Diagnostic { code: "42804", message: "EXCEPT types integer and text cannot be matched", position: 32, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM s1 INTERSECT SELECT a FROM s2;",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "each INTERSECT query must have the same number of columns", position: 38, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM s1 INTERSECT SELECT a FROM s2 FOR SHARE;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "FOR SHARE is not allowed with UNION/INTERSECT/EXCEPT", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT '{1}'::int[] INTERSECT SELECT '{1}'::int[];",
                    expected: Expected::Rows {
                        columns: &[Column("int4", INT4_ARRAY)],
                        rows: &[
                            &[T("{1}")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT x FROM (VALUES ('(1,2)'::point)) v(x) INTERSECT SELECT '(1,2)'::point;",
                    expected: Expected::Error(Diagnostic { code: "42883", message: "could not identify an equality operator for type point", position: 8, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a, b FROM s1 INTERSECT SELECT a, b FROM s2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashSetOp Intersect")],
                            &[T("  ->  Seq Scan on s1")],
                            &[T("  ->  Seq Scan on s2")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a, b FROM s1 EXCEPT ALL SELECT a, b FROM s2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashSetOp Except All")],
                            &[T("  ->  Seq Scan on s1")],
                            &[T("  ->  Seq Scan on s2")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a FROM s1 INTERSECT ALL SELECT a FROM s2 INTERSECT SELECT 3;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashSetOp Intersect")],
                            &[T("  ->  Result")],
                            &[T("  ->  HashSetOp Intersect All")],
                            &[T("        ->  Seq Scan on s1")],
                            &[T("        ->  Seq Scan on s2")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_hashagg = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a, b FROM s1 INTERSECT SELECT a, b FROM s2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("SetOp Intersect")],
                            &[T("  ->  Sort")],
                            &[T("        Sort Key: s1.a, s1.b")],
                            &[T("        ->  Seq Scan on s1")],
                            &[T("  ->  Sort")],
                            &[T("        Sort Key: s2.a, s2.b")],
                            &[T("        ->  Seq Scan on s2")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a, b FROM s1 EXCEPT SELECT a, b FROM s2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("SetOp Except")],
                            &[T("  ->  Sort")],
                            &[T("        Sort Key: s1.a, s1.b")],
                            &[T("        ->  Seq Scan on s1")],
                            &[T("  ->  Sort")],
                            &[T("        Sort Key: s2.a, s2.b")],
                            &[T("        ->  Seq Scan on s2")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM s1 INTERSECT ALL SELECT a, b FROM s2 ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("2"), T("y")],
                            &[T("2"), T("y")],
                            &[Null, T("n")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM s1 EXCEPT ALL SELECT a, b FROM s2 ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                            &[T("2"), T("y")],
                            &[T("3"), T("z")],
                            &[Null, T("n")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM s1 EXCEPT SELECT a, b FROM s2 ORDER BY a, b;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("x")],
                            &[T("3"), T("z")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a FROM s2 INTERSECT SELECT a FROM s1 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("3")],
                            &[Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a FROM s1 INTERSECT SELECT a FROM s2 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("SetOp Intersect")],
                            &[T("  ->  Sort")],
                            &[T("        Sort Key: s1.a")],
                            &[T("        ->  Seq Scan on s1")],
                            &[T("  ->  Sort")],
                            &[T("        Sort Key: s2.a")],
                            &[T("        ->  Seq Scan on s2")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_hashagg;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "INTERSECT and EXCEPT run again for each outer row, and EXPLAIN ANALYZE and JSON of them",
            set_up_script: &[
                "CREATE TABLE s1 (a int, b text) WITH (autovacuum_enabled = false);",
                "CREATE TABLE s2 (a int, b text) WITH (autovacuum_enabled = false);",
                "INSERT INTO s1 VALUES (1, 'x'), (2, 'y'), (2, 'y'), (3, 'z'), (NULL, 'n');",
                "INSERT INTO s2 VALUES (2, 'y'), (3, 'w'), (4, 'v'), (NULL, 'n');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT a, (SELECT count(*) FROM (SELECT a FROM s2 WHERE a >= s1.a INTERSECT SELECT 3) q) FROM s1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("count", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("1")],
                            &[T("2"), T("1")],
                            &[T("3"), T("1")],
                            &[Null, T("0")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, (SELECT count(*) FROM (SELECT 3 INTERSECT SELECT a FROM s2) q) FROM s1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("count", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("1")],
                            &[T("2"), T("1")],
                            &[T("3"), T("1")],
                            &[Null, T("1")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_hashagg = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, (SELECT count(*) FROM (SELECT a FROM s2 WHERE a >= s1.a EXCEPT SELECT 3) q) FROM s1 ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("count", INT8)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("2"), T("2")],
                            &[T("2"), T("2")],
                            &[T("3"), T("1")],
                            &[Null, T("0")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_hashagg;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT a FROM s1 INTERSECT SELECT a FROM s2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashSetOp Intersect (actual rows=3.00 loops=1)")],
                            &[T("  ->  Seq Scan on s1 (actual rows=5.00 loops=1)")],
                            &[T("  ->  Seq Scan on s2 (actual rows=4.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (FORMAT JSON, COSTS OFF) SELECT a FROM s1 EXCEPT SELECT a FROM s2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", JSON)],
                        rows: &[
                            &[T("[\n  {\n    \"Plan\": {\n      \"Node Type\": \"SetOp\",\n      \"Strategy\": \"Hashed\",\n      \"Parallel Aware\": false,\n      \"Async Capable\": false,\n      \"Command\": \"Except\",\n      \"Disabled\": false,\n      \"Plans\": [\n        {\n          \"Node Type\": \"Seq Scan\",\n          \"Parent Relationship\": \"Outer\",\n          \"Parallel Aware\": false,\n          \"Async Capable\": false,\n          \"Relation Name\": \"s1\",\n          \"Alias\": \"s1\",\n          \"Disabled\": false\n        },\n        {\n          \"Node Type\": \"Seq Scan\",\n          \"Parent Relationship\": \"Inner\",\n          \"Parallel Aware\": false,\n          \"Async Capable\": false,\n          \"Relation Name\": \"s2\",\n          \"Alias\": \"s2\",\n          \"Disabled\": false\n        }\n      ]\n    }\n  }\n]")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}
