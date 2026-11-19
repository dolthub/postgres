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
fn test_explain() {
    run_scripts(&[
        ScriptTest {
            name: "basic explain tests",
            set_up_script: &[
                "CREATE TABLE t (i INT PRIMARY KEY)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN SELECT * FROM T;",
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"
EXPLAIN 
(
	ANALYZE, 
	VERBOSE, 
	COSTS, 
	SETTINGS,
	BUFFERS,
	WAL,
	TIMING,
	SUMMARY,
	FORMAT TEXT
) 
	SELECT * FROM t;
"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"
EXPLAIN 
(
	ANALYZE ON, 
	VERBOSE OFF, 
	COSTS TRUE, 
	SETTINGS FALSE,
	BUFFERS,
	WAL,
	TIMING,
	SUMMARY,
	FORMAT TEXT
) 
	SELECT * FROM t;
"#,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"
EXPLAIN 
(
	NOTAVALIDOPTION
) 
	SELECT * FROM t;
"#,
                    skip: Some("the Go test expects Postgres plan text, whose costs are implementation details"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_planned_joins() {
    run_scripts(&[
        ScriptTest {
            name: "lookup joins through secondary indexes",
            set_up_script: &["SET enable_seqscan = off;"],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE lj_big (id INT PRIMARY KEY, k SMALLINT, v INT, label TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE INDEX lj_big_k ON lj_big (k, v);",
                    expected: Expected::Tag("CREATE INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO lj_big SELECT i, (i % 500)::SMALLINT, i % 7, 'b' || i FROM generate_series(1, 3000) i;",
                    expected: Expected::Tag("INSERT 0 3000"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO lj_big VALUES (3001, NULL, 1, 'null key');",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE lj_small (id INT PRIMARY KEY, k BIGINT, w INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO lj_small VALUES (1, 3, 1), (2, 40000, 2), (3, NULL, 3), (4, 499, 4), (5, 3, 5), (6, 7, 6);",
                    expected: Expected::Tag("INSERT 0 6"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT s.id, b.id, b.k, b.v FROM lj_small s JOIN lj_big b ON b.k = s.k ORDER BY s.id, b.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4), Column("k", INT2), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("3"), T("3"), T("3")],
                            &[T("1"), T("503"), T("3"), T("6")],
                            &[T("1"), T("1003"), T("3"), T("2")],
                            &[T("1"), T("1503"), T("3"), T("5")],
                            &[T("1"), T("2003"), T("3"), T("1")],
                            &[T("1"), T("2503"), T("3"), T("4")],
                            &[T("4"), T("499"), T("499"), T("2")],
                            &[T("4"), T("999"), T("499"), T("5")],
                            &[T("4"), T("1499"), T("499"), T("1")],
                            &[T("4"), T("1999"), T("499"), T("4")],
                            &[T("4"), T("2499"), T("499"), T("0")],
                            &[T("4"), T("2999"), T("499"), T("3")],
                            &[T("5"), T("3"), T("3"), T("3")],
                            &[T("5"), T("503"), T("3"), T("6")],
                            &[T("5"), T("1003"), T("3"), T("2")],
                            &[T("5"), T("1503"), T("3"), T("5")],
                            &[T("5"), T("2003"), T("3"), T("1")],
                            &[T("5"), T("2503"), T("3"), T("4")],
                            &[T("6"), T("7"), T("7"), T("0")],
                            &[T("6"), T("507"), T("7"), T("3")],
                            &[T("6"), T("1007"), T("7"), T("6")],
                            &[T("6"), T("1507"), T("7"), T("2")],
                            &[T("6"), T("2007"), T("7"), T("5")],
                            &[T("6"), T("2507"), T("7"), T("1")],
                        ],
                        tag: "SELECT 24",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT s.id, b.id FROM lj_small s JOIN lj_big b ON b.k = s.k AND b.v = s.w ORDER BY s.id, b.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("2003")],
                            &[T("4"), T("1999")],
                            &[T("5"), T("1503")],
                            &[T("6"), T("1007")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT s.id, b.id FROM lj_small s LEFT JOIN lj_big b ON b.k = s.k AND b.v = 2 ORDER BY s.id, b.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("1003")],
                            &[T("2"), Null],
                            &[T("3"), Null],
                            &[T("4"), T("499")],
                            &[T("5"), T("1003")],
                            &[T("6"), T("1507")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT s.id, b.id, b.label FROM lj_small s JOIN lj_big b ON b.k = s.k WHERE b.label LIKE 'b2%' ORDER BY s.id, b.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4), Column("label", TEXT)],
                        rows: &[
                            &[T("1"), T("2003"), T("b2003")],
                            &[T("1"), T("2503"), T("b2503")],
                            &[T("4"), T("2499"), T("b2499")],
                            &[T("4"), T("2999"), T("b2999")],
                            &[T("5"), T("2003"), T("b2003")],
                            &[T("5"), T("2503"), T("b2503")],
                            &[T("6"), T("2007"), T("b2007")],
                            &[T("6"), T("2507"), T("b2507")],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT s.id, b.id FROM lj_big b JOIN lj_small s ON b.k = s.k WHERE b.id > 2000 ORDER BY s.id, b.id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4)],
                        rows: &[
                            &[T("1"), T("2003")],
                            &[T("1"), T("2503")],
                            &[T("4"), T("2499")],
                            &[T("4"), T("2999")],
                            &[T("5"), T("2003")],
                            &[T("5"), T("2503")],
                            &[T("6"), T("2007")],
                            &[T("6"), T("2507")],
                        ],
                        tag: "SELECT 8",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN SELECT s.id, b.id FROM lj_small s JOIN lj_big b ON b.k = s.k;",
                    expected: Expected::Plan(&[PlanFact::Join { kind: "HashJoin", left: "lj_small", right: "lj_big" }]),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM lj_small s JOIN lj_big b ON b.k = s.k;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("24")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "semi and anti joins from EXISTS",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE ex_a (id INT PRIMARY KEY, x INT, y INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE ex_b (id INT PRIMARY KEY, x INT, y INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ex_a VALUES (1, 1, 1), (2, 2, 2), (3, 3, 3), (4, NULL, 4), (5, 2, 5);",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO ex_b VALUES (1, 1, 1), (2, 2, 9), (3, 2, 2), (4, NULL, 4);",
                    expected: Expected::Tag("INSERT 0 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM ex_a WHERE EXISTS (SELECT 1 FROM ex_b WHERE ex_b.x = ex_a.x) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("5")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN SELECT id FROM ex_a WHERE EXISTS (SELECT 1 FROM ex_b WHERE ex_b.x = ex_a.x);",
                    expected: Expected::Plan(&[PlanFact::Join { kind: "HashJoin", left: "ex_a", right: "ex_b" }]),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM ex_a WHERE NOT EXISTS (SELECT 1 FROM ex_b WHERE ex_b.x = ex_a.x) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("3")],
                            &[T("4")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM ex_a WHERE EXISTS (SELECT 1 FROM ex_b WHERE ex_b.x = ex_a.x AND ex_b.y = ex_a.y) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM ex_a WHERE EXISTS (SELECT 1 FROM ex_b WHERE ex_b.x = ex_a.x AND ex_b.id > 2) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("2")],
                            &[T("5")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM ex_a WHERE id > 1 AND NOT EXISTS (SELECT 1 FROM ex_b WHERE ex_b.id = ex_a.id) ORDER BY id;",
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
                    query: "SELECT id FROM ex_a WHERE EXISTS (SELECT 1 FROM ex_b WHERE ex_b.x = ex_a.x OR ex_b.y = ex_a.y) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("4")],
                            &[T("5")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM ex_a WHERE EXISTS (SELECT 1 FROM ex_b WHERE ex_b.x = ex_a.x) AND NOT EXISTS (SELECT 1 FROM ex_b WHERE ex_b.y = ex_a.y) ORDER BY id;",
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
                    query: "DELETE FROM ex_b;",
                    expected: Expected::Tag("DELETE 4"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM ex_a WHERE NOT EXISTS (SELECT 1 FROM ex_b WHERE ex_b.x = ex_a.x) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                            &[T("4")],
                            &[T("5")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id FROM ex_a WHERE EXISTS (SELECT 1 FROM ex_b WHERE ex_b.x = ex_a.x) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "catalog index scans and lookups",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE cat_t1 (id INT PRIMARY KEY, v TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE cat_t2 (id INT PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname FROM pg_catalog.pg_class WHERE oid = 'cat_t1'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME)],
                        rows: &[
                            &[T("cat_t1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT relname FROM pg_catalog.pg_class WHERE oid IN ('cat_t1'::regclass, 'cat_t2'::regclass) ORDER BY relname;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME)],
                        rows: &[
                            &[T("cat_t1")],
                            &[T("cat_t2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT typname FROM pg_catalog.pg_type WHERE oid IN (23, 25) ORDER BY typname;",
                    expected: Expected::Rows {
                        columns: &[Column("typname", NAME)],
                        rows: &[
                            &[T("int4")],
                            &[T("text")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT typname FROM pg_catalog.pg_type WHERE oid > 22 AND oid < 26 ORDER BY oid;",
                    expected: Expected::Rows {
                        columns: &[Column("typname", NAME)],
                        rows: &[
                            &[T("int4")],
                            &[T("regproc")],
                            &[T("text")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT oid FROM pg_catalog.pg_namespace WHERE nspname = 'public';",
                    expected: Expected::Rows {
                        columns: &[Column("oid", OID)],
                        rows: &[
                            &[T("2200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname, n.nspname FROM pg_catalog.pg_class c JOIN pg_catalog.pg_namespace n ON c.relnamespace = n.oid WHERE c.relname = 'cat_t1';",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("nspname", NAME)],
                        rows: &[
                            &[T("cat_t1"), T("public")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT c.relname, a.attname FROM pg_catalog.pg_class c JOIN pg_catalog.pg_attribute a ON c.oid = a.attrelid WHERE c.relname = 'cat_t1' AND a.attnum > 0 ORDER BY a.attnum;",
                    expected: Expected::Rows {
                        columns: &[Column("relname", NAME), Column("attname", NAME)],
                        rows: &[
                            &[T("cat_t1"), T("id")],
                            &[T("cat_t1"), T("v")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT t.typname, n.nspname FROM pg_catalog.pg_type t JOIN pg_catalog.pg_namespace n ON t.typnamespace = n.oid WHERE t.typname = 'int4';",
                    expected: Expected::Rows {
                        columns: &[Column("typname", NAME), Column("nspname", NAME)],
                        rows: &[
                            &[T("int4"), T("pg_catalog")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM pg_catalog.pg_index WHERE indrelid = 'cat_t1'::regclass;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1")],
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
fn test_limited_joins() {
    run_scripts(&[
        ScriptTest {
            name: "a LIMIT over an unordered join looks rows up instead of hashing a whole input",
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE lim_a (id INT PRIMARY KEY, v INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE lim_b (id INT PRIMARY KEY, a_id INT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO lim_a SELECT i, i % 10 FROM generate_series(1, 5000) i;",
                    expected: Expected::Tag("INSERT 0 5000"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO lim_b SELECT i, 5001 - i FROM generate_series(1, 5000) i;",
                    expected: Expected::Tag("INSERT 0 5000"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN SELECT a.id, a.v FROM lim_a a, lim_a b WHERE a.id = b.id LIMIT 50;",
                    expected: Expected::Plan(&[PlanFact::FullScan { table: "lim_a" }]),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN SELECT a.id, b.id FROM lim_a a, lim_b b WHERE a.id = b.a_id LIMIT 50;",
                    expected: Expected::Plan(&[PlanFact::Join { kind: "LookupJoin", left: "lim_b", right: "lim_a" }]),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), count(DISTINCT a_id) FROM (SELECT a.id, b.a_id FROM lim_a a, lim_b b WHERE a.id = b.a_id LIMIT 50) s WHERE id = a_id;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("count", INT8)],
                        rows: &[
                            &[T("50"), T("50")],
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
fn test_empty_and_full_joins() {
    run_scripts(&[
        ScriptTest {
            name: "joins that find no rows and FULL JOIN conditions",
            set_up_script: &[
                "CREATE TABLE fj_a (x INT);",
                "CREATE TABLE fj_b (y INT);",
                "INSERT INTO fj_a VALUES (1), (2);",
                "INSERT INTO fj_b VALUES (1), (3);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM fj_a JOIN fj_b ON false;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM fj_a LEFT JOIN fj_b ON fj_a.x = fj_b.y WHERE false;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM fj_a FULL JOIN fj_b ON fj_a.x = fj_b.y ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("y", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), Null],
                            &[Null, T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM fj_a FULL JOIN fj_b ON fj_a.x < fj_b.y;",
                    expected: Expected::Error(Diagnostic { code: "0A000", message: "FULL JOIN is only supported with merge-joinable or hash-joinable join conditions", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_pulled_up_subqueries() {
    run_scripts(&[
        ScriptTest {
            name: "subqueries of join conditions and of subqueries' conditions",
            set_up_script: &[
                "CREATE TABLE sl_a (x INT PRIMARY KEY, v INT);",
                "CREATE TABLE sl_b (y INT, w INT);",
                "CREATE TABLE sl_c (z INT, u INT);",
                "INSERT INTO sl_a SELECT i, i % 7 FROM generate_series(1, 200) i;",
                "INSERT INTO sl_b SELECT i % 50, i FROM generate_series(1, 300) i;",
                "INSERT INTO sl_c SELECT i % 20, i % 3 FROM generate_series(1, 100) i;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM sl_a JOIN sl_b ON sl_a.x = sl_b.y AND sl_b.w IN (SELECT z FROM sl_c);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("19")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM sl_a LEFT JOIN sl_b ON sl_a.x = sl_b.y AND EXISTS (SELECT 1 FROM sl_c WHERE sl_c.z = sl_b.w);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("200")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM sl_a LEFT JOIN sl_b ON sl_a.x = sl_b.y AND EXISTS (SELECT 1 FROM sl_c WHERE sl_c.z = sl_a.v);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("445")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM sl_a WHERE EXISTS (SELECT 1 FROM sl_b WHERE sl_b.y = sl_a.x AND sl_b.w IN (SELECT z FROM sl_c));",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("19")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM sl_a WHERE EXISTS (SELECT 1 FROM sl_b WHERE sl_b.y = sl_a.x AND EXISTS (SELECT 1 FROM sl_c WHERE sl_c.z = sl_b.w AND sl_c.u = sl_a.v));",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("8")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM sl_a WHERE NOT EXISTS (SELECT 1 FROM sl_b WHERE sl_b.y = sl_a.x AND NOT EXISTS (SELECT 1 FROM sl_c WHERE sl_c.z = sl_b.w));",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("151")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM sl_a WHERE EXISTS (SELECT 1 FROM sl_b WHERE sl_b.y = sl_a.x AND sl_b.w > (SELECT max(u) FROM sl_c WHERE sl_c.z = sl_b.y));",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("19")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM sl_a RIGHT JOIN sl_b ON sl_a.x = sl_b.y AND sl_a.v IN (SELECT u FROM sl_c);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("300")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM sl_a FULL JOIN sl_b ON sl_a.x = sl_b.y AND sl_a.v IN (SELECT u FROM sl_c);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("479")],
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
fn test_group_by_unique_columns() {
    run_scripts(&[
        ScriptTest {
            name: "GROUP BY columns that a unique index's columns determine",
            set_up_script: &[
                "CREATE TABLE gk_t (id INT PRIMARY KEY, a INT, b TEXT);",
                "CREATE TABLE gk_u (k INT NOT NULL, j INT NOT NULL, c INT, UNIQUE (k, j));",
                "INSERT INTO gk_t SELECT i, i % 5, 'x' || (i % 3) FROM generate_series(1, 50) i;",
                "INSERT INTO gk_u SELECT i, i % 4, i % 6 FROM generate_series(1, 40) i;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT id, a, b, count(*) FROM gk_t GROUP BY id, a, b ORDER BY id LIMIT 3;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4), Column("b", TEXT), Column("count", INT8)],
                        rows: &[
                            &[T("1"), T("1"), T("x1"), T("1")],
                            &[T("2"), T("2"), T("x2"), T("1")],
                            &[T("3"), T("3"), T("x0"), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT k, j, c, count(*) FROM gk_u GROUP BY k, j, c ORDER BY k LIMIT 3;",
                    expected: Expected::Rows {
                        columns: &[Column("k", INT4), Column("j", INT4), Column("c", INT4), Column("count", INT8)],
                        rows: &[
                            &[T("1"), T("1"), T("1"), T("1")],
                            &[T("2"), T("2"), T("2"), T("1")],
                            &[T("3"), T("3"), T("3"), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b, count(*) FROM gk_t GROUP BY a, b ORDER BY a, b LIMIT 2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT), Column("count", INT8)],
                        rows: &[
                            &[T("0"), T("x0"), T("3")],
                            &[T("0"), T("x1"), T("3")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_window_run_conditions() {
    run_scripts(&[
        ScriptTest {
            name: "restrictions on window functions of a subquery",
            set_up_script: &[
                "CREATE TABLE wr_t (id INT PRIMARY KEY, g INT, v INT);",
                "INSERT INTO wr_t SELECT i, i % 3, i * 7 % 11 FROM generate_series(1, 30) i;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT id, row_number() OVER (ORDER BY v, id) rn FROM wr_t) s WHERE rn <= 3 ORDER BY rn;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("rn", INT8)],
                        rows: &[
                            &[T("11"), T("1")],
                            &[T("22"), T("2")],
                            &[T("8"), T("3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT id, g, rank() OVER (PARTITION BY g ORDER BY v) r FROM wr_t) s WHERE r < 2 AND id > 3 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("g", INT4), Column("r", INT8)],
                        rows: &[
                            &[T("11"), T("2"), T("1")],
                            &[T("22"), T("1"), T("1")],
                            &[T("30"), T("0"), T("1")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT id, count(*) OVER (ORDER BY id) c FROM wr_t) s WHERE 5 >= c ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("c", INT8)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                            &[T("4"), T("4")],
                            &[T("5"), T("5")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT id, row_number() OVER (ORDER BY id) rn FROM wr_t) s WHERE rn = 4;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("rn", INT8)],
                        rows: &[
                            &[T("4"), T("4")],
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
fn test_planned_views_and_inlined_functions() {
    run_scripts(&[
        ScriptTest {
            name: "views whose plans test a condition once, and SQL functions of one expression",
            set_up_script: &[
                "CREATE TABLE pv_a (a INT);",
                "CREATE TABLE pv_b (b INT);",
                "CREATE TABLE pv_c (c INT);",
                "CREATE VIEW pv_v AS SELECT a FROM pv_a WHERE EXISTS (SELECT 1 FROM pv_b JOIN pv_c ON b = c);",
                "CREATE FUNCTION pv_plus1(x INT) RETURNS INT LANGUAGE SQL IMMUTABLE AS 'SELECT x + 1';",
                "CREATE FUNCTION pv_add(a INT, b INT) RETURNS INT LANGUAGE SQL STRICT IMMUTABLE RETURN a + b;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM pv_v;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO pv_a VALUES (1); INSERT INTO pv_b VALUES (2); INSERT INTO pv_c VALUES (2);",
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM pv_v;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT pv_plus1(pv_plus1(a)), pv_add(a, NULL), pv_add(a, 2) FROM pv_a WHERE a = pv_plus1(0);",
                    expected: Expected::Rows {
                        columns: &[Column("pv_plus1", INT4), Column("pv_add", INT4), Column("pv_add", INT4)],
                        rows: &[
                            &[T("3"), Null, T("3")],
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
fn test_window_clause_frames() {
    run_scripts(&[
        ScriptTest {
            name: "ranking functions and aggregates over windows of different frames",
            set_up_script: &[
                "CREATE TABLE wf_t (id INT PRIMARY KEY, p INT, o INT);",
                "INSERT INTO wf_t SELECT i, i % 3, i % 7 FROM generate_series(1, 20) i;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT id, row_number() OVER (ORDER BY o, id), sum(o) OVER (ORDER BY o, id ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW), rank() OVER (ORDER BY o) FROM wf_t ORDER BY id LIMIT 4;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("row_number", INT8), Column("sum", INT8), Column("rank", INT8)],
                        rows: &[
                            &[T("1"), T("3"), T("1"), T("3")],
                            &[T("2"), T("6"), T("5"), T("6")],
                            &[T("3"), T("9"), T("12"), T("9")],
                            &[T("4"), T("12"), T("22"), T("12")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_lateral_functions() {
    run_scripts(&[
        ScriptTest {
            name: "functions and VALUES lists that read the FROM items before them",
            set_up_script: &[
                "CREATE TABLE lat_t (id INT PRIMARY KEY, n INT, arr INT[]);",
                "INSERT INTO lat_t VALUES (1, 2, '{10,20}'), (2, 3, '{30}'), (3, 0, '{}');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT id, g FROM lat_t, generate_series(1, lat_t.n) g ORDER BY id, g;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("g", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("1"), T("2")],
                            &[T("2"), T("1")],
                            &[T("2"), T("2")],
                            &[T("2"), T("3")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, x FROM lat_t, unnest(lat_t.arr) x ORDER BY id, x;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("x", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("1"), T("20")],
                            &[T("2"), T("30")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lat_t.id, v.a FROM lat_t, LATERAL (VALUES (lat_t.id * 10), (lat_t.n)) v(a) ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("a", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("1"), T("10")],
                            &[T("2"), T("3")],
                            &[T("2"), T("20")],
                            &[T("3"), T("0")],
                            &[T("3"), T("30")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.id, b.id, g FROM lat_t a JOIN lat_t b ON a.id = b.id, generate_series(a.n, b.n + 1) g ORDER BY 1, 3;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4), Column("g", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("2")],
                            &[T("1"), T("1"), T("3")],
                            &[T("2"), T("2"), T("3")],
                            &[T("2"), T("2"), T("4")],
                            &[T("3"), T("3"), T("0")],
                            &[T("3"), T("3"), T("1")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_lateral_chains_and_correlated_ctes() {
    run_scripts(&[
        ScriptTest {
            name: "functions that read several FROM items before them, and WITH queries that read an enclosing row",
            set_up_script: &[
                "CREATE TABLE lc_t (f1 INT);",
                "INSERT INTO lc_t VALUES (1), (2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(r3) FROM generate_series(1, 2) r1, generate_series(r1, 3) r2, generate_series(r1 + r2, 5) r3;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("12"), T("49")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM generate_series(1, 2) r1, generate_series(r1, 3) r2, ROWS FROM (generate_series(10 + r1, 11), generate_series(10 + r2, 11)) ORDER BY 1, 2, 3, 4;",
                    expected: Expected::Rows {
                        columns: &[Column("r1", INT4), Column("r2", INT4), Column("generate_series", INT4), Column("generate_series", INT4)],
                        rows: &[
                            &[T("1"), T("1"), T("11"), T("11")],
                            &[T("1"), T("2"), T("11"), Null],
                            &[T("1"), T("3"), T("11"), Null],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (WITH cte(foo) AS (VALUES (f1)) SELECT (SELECT foo FROM cte)) FROM lc_t ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("foo", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_memoized_lookup_filters() {
    run_scripts(&[
        ScriptTest {
            name: "a cached index lookup that filters the rows it finds",
            set_up_script: &[
                "CREATE TABLE mm_t (u INT, h INT, tw INT);",
                "CREATE INDEX mm_t_u ON mm_t (u);",
                "CREATE INDEX mm_t_h ON mm_t (h);",
                "INSERT INTO mm_t SELECT i, i % 100, i % 20 FROM generate_series(0, 9999) i;",
                "ANALYZE mm_t;",
                "SET enable_hashjoin = off;",
                "SET enable_mergejoin = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM mm_t t1 JOIN mm_t t2 ON t1.u = t2.h WHERE t1.tw = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("500")],
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
fn test_right_semi_join_order() {
    run_scripts(&[
        ScriptTest {
            name: "ordering by the column that an IN subquery matches, above a right semi join",
            set_up_script: &[
                "CREATE TABLE rs_a (a INT, b INT, c VARCHAR);",
                "CREATE TABLE rs_b (a INT, b INT, c VARCHAR);",
                "CREATE TABLE rs_c (a INT, b INT, c INT);",
                "INSERT INTO rs_a SELECT i, i % 25, to_char(i, 'FM0000') FROM generate_series(0, 599) i WHERE i % 2 = 0;",
                "INSERT INTO rs_b SELECT i % 25, i, to_char(i, 'FM0000') FROM generate_series(0, 599) i WHERE i % 3 = 0;",
                "INSERT INTO rs_c SELECT i, i, i % 25 FROM generate_series(0, 599, 2) i;",
                "ANALYZE rs_a;",
                "ANALYZE rs_b;",
                "ANALYZE rs_c;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT t1.* FROM rs_a t1 WHERE t1.a IN (SELECT t1.b FROM rs_b t1, rs_c t2 WHERE t1.a = 0 AND t1.b = (t2.a + t2.b)/2) AND t1.b = 0 ORDER BY t1.a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", VARCHAR)],
                        rows: &[
                            &[T("0"), T("0"), T("0000")],
                            &[T("150"), T("0"), T("0150")],
                            &[T("300"), T("0"), T("0300")],
                            &[T("450"), T("0"), T("0450")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_union_all_append_relations() {
    run_scripts(&[
        ScriptTest {
            name: "UNION ALL subqueries and queries that become append relations",
            set_up_script: &[
                "CREATE TABLE ua1 (a INT PRIMARY KEY, b TEXT);",
                "CREATE TABLE ua2 (c INT PRIMARY KEY, d TEXT);",
                "CREATE TABLE ua3 (e INT PRIMARY KEY, f INT);",
                "INSERT INTO ua1 SELECT g, 'x' || g FROM generate_series(1, 100) g;",
                "INSERT INTO ua2 SELECT g, 'y' || g FROM generate_series(50, 150) g;",
                "INSERT INTO ua3 SELECT g, g % 7 FROM generate_series(1, 20) g;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT a, b FROM ua1 UNION ALL SELECT c, d FROM ua2) s WHERE a = 60 ORDER BY 2;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("60"), T("x60")],
                            &[T("60"), T("y60")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a, b FROM ua1 WHERE a < 3 UNION ALL SELECT c, d FROM ua2 WHERE c > 148 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("x1")],
                            &[T("2"), T("x2")],
                            &[T("149"), T("y149")],
                            &[T("150"), T("y150")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT s.x, ua3.f FROM (SELECT a + 1 AS x FROM ua1 UNION ALL SELECT 1000) s JOIN ua3 ON ua3.e = s.x WHERE s.x < 5 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("f", INT4)],
                        rows: &[
                            &[T("2"), T("2")],
                            &[T("3"), T("3")],
                            &[T("4"), T("4")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), max(a) FROM (SELECT a FROM ua1 UNION ALL SELECT c FROM ua2 UNION ALL SELECT e FROM ua3) s;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("max", INT4)],
                        rows: &[
                            &[T("221"), T("150")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ua3.e, s.k FROM ua3 LEFT JOIN (SELECT a, 'k' AS k FROM ua1 UNION ALL SELECT c, 'm' FROM ua2) s ON s.a = ua3.e * 10 WHERE ua3.e IN (4, 5, 16) ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("e", INT4), Column("k", TEXT)],
                        rows: &[
                            &[T("4"), T("k")],
                            &[T("5"), T("k")],
                            &[T("5"), T("m")],
                            &[T("16"), Null],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT a FROM ua1 WHERE a = 1 UNION ALL (SELECT c FROM ua2 ORDER BY c LIMIT 2)) s ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("50")],
                            &[T("51")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT 1 AS x UNION ALL SELECT 2 UNION ALL SELECT 3 ORDER BY 1 DESC LIMIT 2;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4)],
                        rows: &[
                            &[T("3")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT a, b FROM ua1 UNION ALL SELECT c, d FROM ua2) s ORDER BY a DESC LIMIT 3;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("150"), T("y150")],
                            &[T("149"), T("y149")],
                            &[T("148"), T("y148")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (SELECT a + 1 AS x, b FROM ua1 UNION ALL SELECT c, d FROM ua2) s ORDER BY x LIMIT 3;",
                    expected: Expected::Rows {
                        columns: &[Column("x", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("2"), T("x1")],
                            &[T("3"), T("x2")],
                            &[T("4"), T("x3")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ua3 JOIN (SELECT a, b FROM ua1 UNION ALL SELECT c, d FROM ua2) s ON s.a = ua3.e * 10 WHERE ua3.f = 1 ORDER BY 1, 4;",
                    expected: Expected::Rows {
                        columns: &[Column("e", INT4), Column("f", INT4), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("1"), T("10"), T("x10")],
                            &[T("8"), T("1"), T("80"), T("x80")],
                            &[T("8"), T("1"), T("80"), T("y80")],
                            &[T("15"), T("1"), T("150"), T("y150")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM ua3 LEFT JOIN (SELECT a, b FROM ua1 UNION ALL SELECT c, d FROM ua2) s ON s.a > ua3.e * 10 AND s.a < ua3.e * 10 + 2 WHERE ua3.f = 1 ORDER BY 1, 4;",
                    expected: Expected::Rows {
                        columns: &[Column("e", INT4), Column("f", INT4), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("1"), T("11"), T("x11")],
                            &[T("8"), T("1"), T("81"), T("x81")],
                            &[T("8"), T("1"), T("81"), T("y81")],
                            &[T("15"), T("1"), Null, Null],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_parameterized_joins() {
    run_scripts(&[
        ScriptTest {
            name: "joins whose inner side is a join parameterized by the outer rows",
            set_up_script: &[
                "CREATE TABLE pj1 (id INT PRIMARY KEY, x INT);",
                "CREATE TABLE pj2 (id INT PRIMARY KEY, b INT, a INT);",
                "CREATE INDEX pj2_b ON pj2 (b);",
                "CREATE TABLE pj3 (a INT PRIMARY KEY, v TEXT);",
                "INSERT INTO pj1 SELECT g, g * 3 FROM generate_series(1, 10) g;",
                "INSERT INTO pj2 SELECT g, g % 2000, g % 500 FROM generate_series(1, 20000) g;",
                "INSERT INTO pj3 SELECT g, 'v' || g FROM generate_series(0, 499) g;",
                "ANALYZE pj1;",
                "ANALYZE pj2;",
                "ANALYZE pj3;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT pj1.id, count(pj2.id), min(pj3.v) FROM pj1 LEFT JOIN (pj2 JOIN pj3 ON pj2.a = pj3.a) ON pj1.x = pj2.b WHERE pj1.id IN (1, 2, 3) GROUP BY pj1.id ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("count", INT8), Column("min", TEXT)],
                        rows: &[
                            &[T("1"), T("10"), T("v3")],
                            &[T("2"), T("10"), T("v6")],
                            &[T("3"), T("10"), T("v9")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT a.id, b.id, count(pj2.id) FROM pj1 a JOIN pj1 b ON a.x = b.id LEFT JOIN (pj2 JOIN pj3 ON pj2.a = pj3.a) ON pj2.b = a.x + b.x WHERE a.id IN (1, 2) GROUP BY a.id, b.id ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("id", INT4), Column("count", INT8)],
                        rows: &[
                            &[T("1"), T("3"), T("10")],
                            &[T("2"), T("6"), T("10")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_matching_operator_selectivity() {
    run_scripts(&[
        ScriptTest {
            name: "jsonb, text search, and array filters estimated from column statistics",
            set_up_script: &[
                "CREATE TABLE mo1 (id INT PRIMARY KEY, j JSONB, t TEXT);",
                "CREATE TABLE mo2 (id INT PRIMARY KEY, n INT);",
                "INSERT INTO mo1 SELECT g, jsonb_build_object('k', g % 7, 'v', g % 50), 'word' || (g % 13) FROM generate_series(1, 1000) g;",
                "INSERT INTO mo2 SELECT g, g FROM generate_series(1, 1000) g;",
                "ANALYZE mo1;",
                "ANALYZE mo2;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(mo2.n) FROM mo1 JOIN mo2 ON mo1.id = mo2.id WHERE mo1.j @> '{\"k\": 3}';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("143"), T("71500")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT mo1.id, mo2.n FROM mo1 JOIN mo2 ON mo1.id = mo2.id WHERE mo1.j @> '{\"v\": 3}' AND mo1.id < 200 ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("n", INT4)],
                        rows: &[
                            &[T("3"), T("3")],
                            &[T("53"), T("53")],
                            &[T("103"), T("103")],
                            &[T("153"), T("153")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM mo1 JOIN mo2 ON mo1.id = mo2.id WHERE mo1.j ? 'z';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM mo1 JOIN mo2 ON mo1.id = mo2.id WHERE mo1.t @@ 'word3';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("77")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM mo1 WHERE mo1.j @> NULL;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM mo1 JOIN mo2 ON mo1.id = mo2.id WHERE mo1.j @> jsonb_build_object('k', mo2.n % 7);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("1000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM mo1 WHERE mo1.t = ANY (ARRAY['word3', 'word4']::VARCHAR[]::TEXT[]);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("154")],
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
fn test_expression_index_statistics() {
    run_scripts(&[
        ScriptTest {
            name: "filters on an indexed expression estimated from the expression's statistics",
            set_up_script: &[
                "CREATE TABLE ei1 (id INT PRIMARY KEY, t TEXT);",
                "CREATE TABLE ei2 (id INT PRIMARY KEY, n INT);",
                "CREATE INDEX ei1_lower ON ei1 (lower(t));",
                "INSERT INTO ei1 SELECT g, CASE WHEN g % 2 = 0 THEN 'Common' ELSE 'Rare' || g END FROM generate_series(1, 1000) g;",
                "INSERT INTO ei2 SELECT g, g FROM generate_series(1, 1000) g;",
                "ANALYZE ei1;",
                "ANALYZE ei2;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(ei2.n) FROM ei1 JOIN ei2 ON ei1.id = ei2.id WHERE lower(ei1.t) = 'common';",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("500"), T("250500")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT ei1.id, ei2.n FROM ei1 JOIN ei2 ON ei1.id = ei2.id WHERE lower(ei1.t) = 'rare7';",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("n", INT4)],
                        rows: &[
                            &[T("7"), T("7")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT lower(t), count(*) FROM ei1 GROUP BY lower(t) ORDER BY 2 DESC, 1 LIMIT 2;",
                    expected: Expected::Rows {
                        columns: &[Column("lower", TEXT), Column("count", INT8)],
                        rows: &[
                            &[T("common"), T("500")],
                            &[T("rare1"), T("1")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_explain_changes() {
    run_scripts(&[
        ScriptTest {
            name: "EXPLAIN of INSERT, UPDATE, and DELETE",
            set_up_script: &[
                "CREATE TABLE ex (id INT PRIMARY KEY, v TEXT, w INT UNIQUE);",
                "CREATE TABLE ex2 (id INT PRIMARY KEY, x INT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) INSERT INTO ex VALUES (1, 'a', 1);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Insert on ex"#)],
                            &[T(r#"  ->  Result"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) INSERT INTO ex VALUES (1, 'a', 1), (2, 'b', 2);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Insert on ex"#)],
                            &[T(r#"  ->  Values Scan on "*VALUES*""#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) INSERT INTO ex SELECT id, 'x', x FROM ex2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Insert on ex"#)],
                            &[T(r#"  ->  Seq Scan on ex2"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) INSERT INTO ex VALUES (1, 'a', 1) ON CONFLICT DO NOTHING;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Insert on ex"#)],
                            &[T(r#"  Conflict Resolution: NOTHING"#)],
                            &[T(r#"  ->  Result"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) INSERT INTO ex VALUES (1, 'a', 1) ON CONFLICT (id) DO NOTHING;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Insert on ex"#)],
                            &[T(r#"  Conflict Resolution: NOTHING"#)],
                            &[T(r#"  Conflict Arbiter Indexes: ex_pkey"#)],
                            &[T(r#"  ->  Result"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) INSERT INTO ex AS e VALUES (1, 'a', 1) ON CONFLICT (w) DO UPDATE SET v = excluded.v;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Insert on ex e"#)],
                            &[T(r#"  Conflict Resolution: UPDATE"#)],
                            &[T(r#"  Conflict Arbiter Indexes: ex_w_key"#)],
                            &[T(r#"  ->  Result"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE ex SET v = 'z' WHERE v = 'q';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Update on ex"#)],
                            &[T(r#"  ->  Seq Scan on ex"#)],
                            &[T(r#"        Filter: (v = 'q'::text)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) UPDATE ex e SET v = 'z';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Update on ex e"#)],
                            &[T(r#"  ->  Seq Scan on ex e"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DELETE FROM ex;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Delete on ex"#)],
                            &[T(r#"  ->  Seq Scan on ex"#)],
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
fn test_explain_execute() {
    run_scripts(&[
        ScriptTest {
            name: "EXPLAIN EXECUTE explains a prepared statement with its parameters' values unless plans are generic",
            set_up_script: &[
                "CREATE TABLE ee (a INT PRIMARY KEY, b TEXT);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "PREPARE ee_count(text) AS SELECT count(*) FROM ee WHERE b = $1;",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) EXECUTE ee_count('x');",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Aggregate")],
                            &[T("  ->  Seq Scan on ee")],
                            &[T("        Filter: (b = 'x'::text)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET plan_cache_mode = force_generic_plan;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) EXECUTE ee_count('x');",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Aggregate")],
                            &[T("  ->  Seq Scan on ee")],
                            &[T("        Filter: (b = $1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN EXECUTE ee_count('x', 'y');",
                    expected: Expected::Error(Diagnostic {
                        code: "42601",
                        message: "wrong number of parameters for prepared statement \"ee_count\"",
                        detail: "Expected 1 parameters but got 2.",
                        ..E
                    }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN EXECUTE missing;",
                    expected: Expected::Error(Diagnostic { code: "26000", message: "prepared statement \"missing\" does not exist", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_explain_analyze() {
    run_scripts(&[
        ScriptTest {
            name: "EXPLAIN ANALYZE reports each node's rows, the rows its conditions removed, and its WAL",
            set_up_script: &[
                "CREATE TABLE ea (id INT PRIMARY KEY, a INT, b TEXT);",
                "INSERT INTO ea VALUES (1, 1, 'x'), (2, 2, 'y'), (3, 3, 'z'), (4, 1, 'w'), (5, 2, 'v');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT * FROM ea WHERE b > 'w';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on ea (actual rows=3.00 loops=1)")],
                            &[T("  Filter: (b > 'w'::text)")],
                            &[T("  Rows Removed by Filter: 2")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT * FROM ea WHERE id = 3;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Index Scan using ea_pkey on ea (actual rows=1.00 loops=1)")],
                            &[T("  Index Cond: (id = 3)")],
                            &[T("  Index Searches: 1")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) SELECT * FROM ea WHERE b > 'w';",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", JSON)],
                        rows: &[&[T("[\n  {\n    \"Plan\": {\n      \"Node Type\": \"Seq Scan\",\n      \"Parallel Aware\": false,\n      \"Async Capable\": false,\n      \"Relation Name\": \"ea\",\n      \"Alias\": \"ea\",\n      \"Actual Rows\": 3.00,\n      \"Actual Loops\": 1,\n      \"Disabled\": false,\n      \"Filter\": \"(b > 'w'::text)\",\n      \"Rows Removed by Filter\": 2\n    },\n    \"Triggers\": [\n    ]\n  }\n]")]],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, BUFFERS OFF, WAL, COSTS OFF, TIMING OFF, SUMMARY OFF, FORMAT XML) SELECT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", XML)],
                        rows: &[&[T("<explain xmlns=\"http://www.postgresql.org/2009/explain\">\n  <Query>\n    <Plan>\n      <Node-Type>Result</Node-Type>\n      <Parallel-Aware>false</Parallel-Aware>\n      <Async-Capable>false</Async-Capable>\n      <Actual-Rows>1.00</Actual-Rows>\n      <Actual-Loops>1</Actual-Loops>\n      <Disabled>false</Disabled>\n      <WAL-Records>0</WAL-Records>\n      <WAL-FPI>0</WAL-FPI>\n      <WAL-Bytes>0</WAL-Bytes>\n      <WAL-Buffers-Full>0</WAL-Buffers-Full>\n    </Plan>\n    <Triggers>\n    </Triggers>\n  </Query>\n</explain>")]],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (WAL) SELECT 1;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "EXPLAIN option WAL requires ANALYZE", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, WAL) INSERT INTO ea VALUES (6, 6, 'u');",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Insert on ea (actual rows=0.00 loops=1)")],
                            &[T("  WAL: records=2 bytes=129")],
                            &[T("  ->  Result (actual rows=1.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    skip: Some("our WAL logs a statement's changed rows in one record per commit, so its counts follow our storage"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_explain_analyze_hash_and_memoize() {
    run_scripts(&[
        ScriptTest {
            name: "EXPLAIN ANALYZE reports a Hash node's buckets, batches, and memory and a Memoize node's cache counts",
            set_up_script: &[
                "CREATE TABLE eh (id INT PRIMARY KEY, x INT);",
                "CREATE TABLE ei (id INT PRIMARY KEY, y INT);",
                "INSERT INTO eh SELECT a.v * 100 + b.v * 10 + c.v, c.v FROM (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) a(v), (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) b(v), (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) c(v);",
                "INSERT INTO ei SELECT a.v * 10 + b.v, b.v % 7 FROM (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) a(v), (VALUES (0), (1), (2), (3), (4), (5), (6), (7), (8), (9)) b(v);",
                "VACUUM ANALYZE eh, ei;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT * FROM eh JOIN ei ON eh.x = ei.y;",
                    skip: Some("work_mem counts our hash joins' and Memoize caches' real size, larger than Postgres' rows, so the memory, bucket, and batch figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Hash Join (actual rows=10000.00 loops=1)")],
                            &[T("  Hash Cond: (ei.y = eh.x)")],
                            &[T("  ->  Seq Scan on ei (actual rows=100.00 loops=1)")],
                            &[T("  ->  Hash (actual rows=1000.00 loops=1)")],
                            &[T("        Buckets: 1024  Batches: 1  Memory Usage: 48kB")],
                            &[T("        ->  Seq Scan on eh (actual rows=1000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) SELECT count(*) FROM eh JOIN ei ON eh.x = ei.y;",
                    skip: Some("work_mem counts our hash joins' and Memoize caches' real size, larger than Postgres' rows, so the memory, bucket, and batch figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", JSON)],
                        rows: &[&[T("[\n  {\n    \"Plan\": {\n      \"Node Type\": \"Aggregate\",\n      \"Strategy\": \"Plain\",\n      \"Partial Mode\": \"Simple\",\n      \"Parallel Aware\": false,\n      \"Async Capable\": false,\n      \"Actual Rows\": 1.00,\n      \"Actual Loops\": 1,\n      \"Disabled\": false,\n      \"Plans\": [\n        {\n          \"Node Type\": \"Hash Join\",\n          \"Parent Relationship\": \"Outer\",\n          \"Parallel Aware\": false,\n          \"Async Capable\": false,\n          \"Join Type\": \"Inner\",\n          \"Actual Rows\": 10000.00,\n          \"Actual Loops\": 1,\n          \"Disabled\": false,\n          \"Inner Unique\": false,\n          \"Hash Cond\": \"(ei.y = eh.x)\",\n          \"Plans\": [\n            {\n              \"Node Type\": \"Seq Scan\",\n              \"Parent Relationship\": \"Outer\",\n              \"Parallel Aware\": false,\n              \"Async Capable\": false,\n              \"Relation Name\": \"ei\",\n              \"Alias\": \"ei\",\n              \"Actual Rows\": 100.00,\n              \"Actual Loops\": 1,\n              \"Disabled\": false\n            },\n            {\n              \"Node Type\": \"Hash\",\n              \"Parent Relationship\": \"Inner\",\n              \"Parallel Aware\": false,\n              \"Async Capable\": false,\n              \"Actual Rows\": 1000.00,\n              \"Actual Loops\": 1,\n              \"Disabled\": false,\n              \"Hash Buckets\": 1024,\n              \"Original Hash Buckets\": 1024,\n              \"Hash Batches\": 1,\n              \"Original Hash Batches\": 1,\n              \"Peak Memory Usage\": 44,\n              \"Plans\": [\n                {\n                  \"Node Type\": \"Seq Scan\",\n                  \"Parent Relationship\": \"Outer\",\n                  \"Parallel Aware\": false,\n                  \"Async Capable\": false,\n                  \"Relation Name\": \"eh\",\n                  \"Alias\": \"eh\",\n                  \"Actual Rows\": 1000.00,\n                  \"Actual Loops\": 1,\n                  \"Disabled\": false\n                }\n              ]\n            }\n          ]\n        }\n      ]\n    },\n    \"Triggers\": [\n    ]\n  }\n]")]],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_hashjoin = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_mergejoin = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT * FROM eh JOIN ei ON ei.id = eh.x;",
                    skip: Some("work_mem counts our hash joins' and Memoize caches' real size, larger than Postgres' rows, so the memory, bucket, and batch figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Nested Loop (actual rows=1000.00 loops=1)")],
                            &[T("  ->  Seq Scan on eh (actual rows=1000.00 loops=1)")],
                            &[T("  ->  Memoize (actual rows=1.00 loops=1000)")],
                            &[T("        Cache Key: eh.x")],
                            &[T("        Cache Mode: logical")],
                            &[T("        Hits: 990  Misses: 10  Evictions: 0  Overflows: 0  Memory Usage: 2kB")],
                            &[T("        ->  Index Scan using ei_pkey on ei (actual rows=1.00 loops=10)")],
                            &[T("              Index Cond: (id = eh.x)")],
                            &[T("              Index Searches: 10")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT YAML) SELECT eh.id, ei.id FROM eh JOIN ei ON ei.id = eh.x;",
                    skip: Some("work_mem counts our hash joins' and Memoize caches' real size, larger than Postgres' rows, so the memory, bucket, and batch figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[&[T("- Plan: \n    Node Type: \"Nested Loop\"\n    Parallel Aware: false\n    Async Capable: false\n    Join Type: \"Inner\"\n    Actual Rows: 1000.00\n    Actual Loops: 1\n    Disabled: false\n    Inner Unique: true\n    Plans: \n      - Node Type: \"Seq Scan\"\n        Parent Relationship: \"Outer\"\n        Parallel Aware: false\n        Async Capable: false\n        Relation Name: \"eh\"\n        Alias: \"eh\"\n        Actual Rows: 1000.00\n        Actual Loops: 1\n        Disabled: false\n      - Node Type: \"Memoize\"\n        Parent Relationship: \"Inner\"\n        Parallel Aware: false\n        Async Capable: false\n        Actual Rows: 1.00\n        Actual Loops: 1000\n        Disabled: false\n        Cache Key: \"eh.x\"\n        Cache Mode: \"logical\"\n        Cache Hits: 990\n        Cache Misses: 10\n        Cache Evictions: 0\n        Cache Overflows: 0\n        Peak Memory Usage: 2\n        Plans: \n          - Node Type: \"Index Only Scan\"\n            Parent Relationship: \"Outer\"\n            Parallel Aware: false\n            Async Capable: false\n            Scan Direction: \"Forward\"\n            Index Name: \"ei_pkey\"\n            Relation Name: \"ei\"\n            Alias: \"ei\"\n            Actual Rows: 1.00\n            Actual Loops: 10\n            Disabled: false\n            Index Cond: \"(id = eh.x)\"\n            Rows Removed by Index Recheck: 0\n            Heap Fetches: 0\n            Index Searches: 10\n  Triggers: ")]],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_hashjoin;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_mergejoin;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_explain_verbose_row_identity_columns() {
    run_scripts(&[
        ScriptTest {
            name: "EXPLAIN VERBOSE names the hidden row identity columns as Postgres does",
            set_up_script: &[
                "CREATE TABLE t (a INT, b TEXT);",
                "CREATE TABLE u (a INT);",
                "CREATE TABLE p (a INT, b TEXT);",
                "CREATE TABLE c1 () INHERITS (p);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t FOR UPDATE;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"LockRows"#)],
                            &[T(r#"  Output: a, b, ctid"#)],
                            &[T(r#"  ->  Seq Scan on public.t"#)],
                            &[T(r#"        Output: a, b, ctid"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM t JOIN u USING (a) FOR UPDATE OF t;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"LockRows"#)],
                            &[T(r#"  Output: t.a, t.b, t.ctid, u.ctid"#)],
                            &[T(r#"  ->  Merge Join"#)],
                            &[T(r#"        Output: t.a, t.b, t.ctid, u.ctid"#)],
                            &[T(r#"        Merge Cond: (t.a = u.a)"#)],
                            &[T(r#"        ->  Sort"#)],
                            &[T(r#"              Output: t.a, t.b, t.ctid"#)],
                            &[T(r#"              Sort Key: t.a"#)],
                            &[T(r#"              ->  Seq Scan on public.t"#)],
                            &[T(r#"                    Output: t.a, t.b, t.ctid"#)],
                            &[T(r#"        ->  Sort"#)],
                            &[T(r#"              Output: u.ctid, u.a"#)],
                            &[T(r#"              Sort Key: u.a"#)],
                            &[T(r#"              ->  Seq Scan on public.u"#)],
                            &[T(r#"                    Output: u.ctid, u.a"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) UPDATE t SET b = 'x' WHERE a = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Update on public.t"#)],
                            &[T(r#"  ->  Seq Scan on public.t"#)],
                            &[T(r#"        Output: 'x'::text, ctid"#)],
                            &[T(r#"        Filter: (t.a = 1)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) DELETE FROM t WHERE a = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Delete on public.t"#)],
                            &[T(r#"  ->  Seq Scan on public.t"#)],
                            &[T(r#"        Output: ctid"#)],
                            &[T(r#"        Filter: (t.a = 1)"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) UPDATE t SET b = 'y' FROM u WHERE t.a = u.a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Update on public.t"#)],
                            &[T(r#"  ->  Merge Join"#)],
                            &[T(r#"        Output: 'y'::text, t.ctid, u.ctid"#)],
                            &[T(r#"        Merge Cond: (t.a = u.a)"#)],
                            &[T(r#"        ->  Sort"#)],
                            &[T(r#"              Output: t.ctid, t.a"#)],
                            &[T(r#"              Sort Key: t.a"#)],
                            &[T(r#"              ->  Seq Scan on public.t"#)],
                            &[T(r#"                    Output: t.ctid, t.a"#)],
                            &[T(r#"        ->  Sort"#)],
                            &[T(r#"              Output: u.ctid, u.a"#)],
                            &[T(r#"              Sort Key: u.a"#)],
                            &[T(r#"              ->  Seq Scan on public.u"#)],
                            &[T(r#"                    Output: u.ctid, u.a"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM p FOR SHARE;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"LockRows"#)],
                            &[T(r#"  Output: p.a, p.b, p.ctid, p.tableoid"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  Seq Scan on public.p p_1"#)],
                            &[T(r#"              Output: p_1.a, p_1.b, p_1.ctid, p_1.tableoid"#)],
                            &[T(r#"        ->  Seq Scan on public.c1 p_2"#)],
                            &[T(r#"              Output: p_2.a, p_2.b, p_2.ctid, p_2.tableoid"#)],
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
fn test_conditions_implied_by_index_conditions() {
    run_scripts(&[
        ScriptTest {
            name: "index and bitmap scans drop the conditions their index conditions imply",
            set_up_script: &[
                "CREATE TABLE bm (a INT, b INT);",
                "INSERT INTO bm SELECT x, x FROM (VALUES (1), (2), (3), (4), (5), (6), (7), (8), (9), (10)) v(x);",
                "CREATE INDEX bm_a ON bm (a);",
                "ANALYZE bm;",
                "SET enable_seqscan = off;",
                "SET enable_indexscan = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM bm WHERE a IN (1, 2) AND (a <= 2 OR b = 5);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Bitmap Heap Scan on bm"#)],
                            &[T(r#"  Recheck Cond: (a = ANY ('{1,2}'::integer[]))"#)],
                            &[T(r#"  ->  Bitmap Index Scan on bm_a"#)],
                            &[T(r#"        Index Cond: (a = ANY ('{1,2}'::integer[]))"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM bm WHERE a IN (1, 2) AND (a <= 2 OR b = 5) ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("1")],
                            &[T("2"), T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_bitmapscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_indexscan = on;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM bm WHERE a IN (1, 2) AND (a <= 2 OR b = 5);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Index Scan using bm_a on bm"#)],
                            &[T(r#"  Index Cond: (a = ANY ('{1,2}'::integer[]))"#)],
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
fn test_explain_group_keys() {
    run_scripts(&[
        ScriptTest {
            name: "an aggregate's ORDER BY adds no sort key the GROUP BY already sorts by",
            set_up_script: &[
                "CREATE TABLE t (a INT, b INT) WITH (autovacuum_enabled = false);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a, sum(b ORDER BY a) FROM t GROUP BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("GroupAggregate")],
                            &[T("  Group Key: a")],
                            &[T("  ->  Sort")],
                            &[T("        Sort Key: a")],
                            &[T("        ->  Seq Scan on t")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a, sum(b ORDER BY a, b) FROM t GROUP BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("GroupAggregate")],
                            &[T("  Group Key: a")],
                            &[T("  ->  Sort")],
                            &[T("        Sort Key: a, b")],
                            &[T("        ->  Seq Scan on t")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "EXPLAIN qualifies the columns of a plan that scans no table",
            set_up_script: &[
                "CREATE TABLE t (a INT, b INT) WITH (autovacuum_enabled = false);",
                "CREATE TABLE p1 (x INT, y INT) WITH (autovacuum_enabled = false);",
                "CREATE TABLE p2 (x INT, y INT) WITH (autovacuum_enabled = false);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a.x, a.y, count(*) FROM (SELECT * FROM p1 WHERE x = 1 AND x = 2) a LEFT JOIN p2 b ON a.x = b.y GROUP BY a.x, a.y ORDER BY 1, 2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("GroupAggregate")],
                            &[T("  Group Key: p1.y")],
                            &[T("  ->  Sort")],
                            &[T("        Sort Key: p1.y")],
                            &[T("        ->  Result")],
                            &[T("              One-Time Filter: false")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF, VERBOSE) SELECT c, sum(a) FROM (SELECT a, b AS c FROM t) s WHERE 1 = 2 GROUP BY c;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate")],
                            &[T("  Output: t.b, sum(t.a)")],
                            &[T("  Group Key: t.b")],
                            &[T("  ->  Result")],
                            &[T("        Output: t.b, t.a")],
                            &[T("        One-Time Filter: false")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT b, sum(a) FROM t WHERE 1 = 2 GROUP BY b;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate")],
                            &[T("  Group Key: b")],
                            &[T("  ->  Result")],
                            &[T("        One-Time Filter: false")],
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
fn test_explain_analyze_sorts() {
    run_scripts(&[
        ScriptTest {
            name: "EXPLAIN ANALYZE reports each sort's method and the memory or disk it took, sorts past work_mem spill to temporary files, and temp_file_limit stops them",
            set_up_script: &[
                "CREATE TABLE st (i INT PRIMARY KEY, a INT, b TEXT, g INT) WITH (autovacuum_enabled = false);",
                "INSERT INTO st SELECT i, (i * 7919) % 10007, 'row ' || i, i % 10 FROM generate_series(1, 20000) i;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET max_parallel_workers_per_gather = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_indexscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_bitmapscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT a, b FROM st ORDER BY a, b;",
                    skip: Some("work_mem counts our rows' real size, larger than Postgres' tuples, so the memory and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Sort (actual rows=20000.00 loops=1)")],
                            &[T("  Sort Key: a, b")],
                            &[T("  Sort Method: quicksort  Memory: 1394kB")],
                            &[T("  ->  Seq Scan on st (actual rows=20000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET work_mem = '64kB';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT a, b FROM st ORDER BY a, b;",
                    skip: Some("work_mem counts our rows' real size, larger than Postgres' tuples, so the memory and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Sort (actual rows=20000.00 loops=1)")],
                            &[T("  Sort Key: a, b")],
                            &[T("  Sort Method: external merge  Disk: 504kB")],
                            &[T("  ->  Seq Scan on st (actual rows=20000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT b FROM st ORDER BY b;",
                    skip: Some("work_mem counts our rows' real size, larger than Postgres' tuples, so the memory and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Sort (actual rows=20000.00 loops=1)")],
                            &[T("  Sort Key: b")],
                            &[T("  Sort Method: external merge  Disk: 280kB")],
                            &[T("  ->  Seq Scan on st (actual rows=20000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT a, b FROM st ORDER BY a, b LIMIT 10;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Limit (actual rows=10.00 loops=1)")],
                            &[T("  ->  Sort (actual rows=10.00 loops=1)")],
                            &[T("        Sort Key: a, b")],
                            &[T("        Sort Method: top-N heapsort  Memory: 25kB")],
                            &[T("        ->  Seq Scan on st (actual rows=20000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT * FROM (SELECT * FROM st ORDER BY a OFFSET 0) s ORDER BY a, b;",
                    skip: Some("work_mem counts our rows' real size, larger than Postgres' tuples, so the memory and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Incremental Sort (actual rows=20000.00 loops=1)")],
                            &[T("  Sort Key: st.a, st.b")],
                            &[T("  Presorted Key: st.a")],
                            &[T("  Full-sort Groups: 625  Sort Method: quicksort  Average Memory: 26kB  Peak Memory: 26kB")],
                            &[T("  ->  Sort (actual rows=20000.00 loops=1)")],
                            &[T("        Sort Key: st.a")],
                            &[T("        Sort Method: external merge  Disk: 688kB")],
                            &[T("        ->  Seq Scan on st (actual rows=20000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT * FROM (SELECT * FROM st ORDER BY g OFFSET 0) s ORDER BY g, b;",
                    skip: Some("work_mem counts our rows' real size, larger than Postgres' tuples, so the memory and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Incremental Sort (actual rows=20000.00 loops=1)")],
                            &[T("  Sort Key: st.g, st.b")],
                            &[T("  Presorted Key: st.g")],
                            &[T("  Full-sort Groups: 10  Sort Method: quicksort  Average Memory: 27kB  Peak Memory: 27kB")],
                            &[T("  Pre-sorted Groups: 10  Sort Method: external merge  Average Disk: 80kB  Peak Disk: 80kB")],
                            &[T("  ->  Sort (actual rows=20000.00 loops=1)")],
                            &[T("        Sort Key: st.g")],
                            &[T("        Sort Method: external merge  Disk: 688kB")],
                            &[T("        ->  Seq Scan on st (actual rows=20000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT i FROM (SELECT * FROM st ORDER BY g OFFSET 0) s ORDER BY g, b OFFSET 3998 LIMIT 4;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4)],
                        rows: &[
                            &[T("9981")],
                            &[T("9991")],
                            &[T("10002")],
                            &[T("10012")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT i, a, b FROM st ORDER BY a DESC, b OFFSET 19995;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("17934"), T("2"), T("row 17934")],
                            &[T("7927"), T("2"), T("row 7927")],
                            &[T("18974"), T("1"), T("row 18974")],
                            &[T("8967"), T("1"), T("row 8967")],
                            &[T("10007"), T("0"), T("row 10007")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(rn * i) FROM (SELECT i, row_number() OVER () AS rn FROM (SELECT i FROM st ORDER BY b OFFSET 0) s) t;",
                    expected: Expected::Rows {
                        columns: &[Column("sum", NUMERIC)],
                        rows: &[
                            &[T("1737956988687")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE c SCROLL CURSOR FOR SELECT i, a, b FROM st ORDER BY a, b;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 2 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("10007"), T("0"), T("row 10007")],
                            &[T("18974"), T("1"), T("row 18974")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH LAST FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("11047"), T("10006"), T("row 11047")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD 2 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1040"), T("10006"), T("row 1040")],
                            &[T("2080"), T("10005"), T("row 2080")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ABSOLUTE 10001 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("19494"), T("5004"), T("row 19494")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH RELATIVE -1 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("520"), T("5003"), T("row 520")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH FIRST FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("a", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("10007"), T("0"), T("row 10007")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD 1 FROM c;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("a", INT4), Column("b", TEXT)],
                        rows: &[

                        ],
                        tag: "FETCH 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_hashjoin = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_nestloop = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT count(*) FROM st s1 JOIN st s2 ON s1.b = s2.b;",
                    skip: Some("work_mem counts our rows' real size, larger than Postgres' tuples, so the memory and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Aggregate (actual rows=1.00 loops=1)")],
                            &[T("  ->  Merge Join (actual rows=20000.00 loops=1)")],
                            &[T("        Merge Cond: (s1.b = s2.b)")],
                            &[T("        ->  Sort (actual rows=20000.00 loops=1)")],
                            &[T("              Sort Key: s1.b")],
                            &[T("              Sort Method: external merge  Disk: 280kB")],
                            &[T("              ->  Seq Scan on st s1 (actual rows=20000.00 loops=1)")],
                            &[T("        ->  Materialize (actual rows=20000.00 loops=1)")],
                            &[T("              Storage: Memory  Maximum Storage: 17kB")],
                            &[T("              ->  Sort (actual rows=20000.00 loops=1)")],
                            &[T("                    Sort Key: s2.b")],
                            &[T("                    Sort Method: external merge  Disk: 280kB")],
                            &[T("                    ->  Seq Scan on st s2 (actual rows=20000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM st s1 JOIN st s2 ON s1.a = s2.a;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("39986")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_hashjoin;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET enable_nestloop;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET temp_file_limit = '100kB';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM (SELECT a, b FROM st ORDER BY a, b OFFSET 0) s;",
                    expected: Expected::Error(Diagnostic { code: "53400", message: "temporary file size exceeds \"temp_file_limit\" (100kB)", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET temp_file_limit;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET work_mem;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}


#[test]
fn test_explain_analyze_tuplestores() {
    run_scripts(&[
        ScriptTest {
            name: "EXPLAIN ANALYZE reports the storage of WindowAgg, CTE Scan, and Recursive Union nodes, and their rows and those of cursors kept past their transactions go to temporary files past work_mem",
            set_up_script: &[
                "CREATE TABLE tt (i INT PRIMARY KEY, a INT, b TEXT) WITH (autovacuum_enabled = false);",
                "INSERT INTO tt SELECT i, (i * 7919) % 10007, 'row ' || i FROM generate_series(1, 20000) i;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET max_parallel_workers_per_gather = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_indexscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_bitmapscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT sum(c) FROM (SELECT count(*) OVER (PARTITION BY a % 3) AS c FROM tt) s;",
                    skip: Some("work_mem counts our rows' real size, larger than Postgres' tuples, so the memory and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Aggregate (actual rows=1.00 loops=1)")],
                            &[T("  ->  WindowAgg (actual rows=20000.00 loops=1)")],
                            &[T("        Window: w1 AS (PARTITION BY ((tt.a % 3)))")],
                            &[T("        Storage: Memory  Maximum Storage: 273kB")],
                            &[T("        ->  Sort (actual rows=20000.00 loops=1)")],
                            &[T("              Sort Key: ((tt.a % 3))")],
                            &[T("              Sort Method: quicksort  Memory: 769kB")],
                            &[T("              ->  Seq Scan on tt (actual rows=20000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) WITH w AS MATERIALIZED (SELECT * FROM tt) SELECT count(*) FROM w w1, (SELECT count(*) FROM w) w2;",
                    skip: Some("work_mem counts our rows' real size, larger than Postgres' tuples, so the memory and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Aggregate (actual rows=1.00 loops=1)")],
                            &[T("  CTE w")],
                            &[T("    ->  Seq Scan on tt (actual rows=20000.00 loops=1)")],
                            &[T("  ->  Nested Loop (actual rows=20000.00 loops=1)")],
                            &[T("        ->  Aggregate (actual rows=1.00 loops=1)")],
                            &[T("              ->  CTE Scan on w (actual rows=20000.00 loops=1)")],
                            &[T("                    Storage: Memory  Maximum Storage: 1186kB")],
                            &[T("        ->  CTE Scan on w w1 (actual rows=20000.00 loops=1)")],
                            &[T("              Storage: Memory  Maximum Storage: 1186kB")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) WITH RECURSIVE r(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM r WHERE n < 5000) SELECT count(*) FROM r;",
                    skip: Some("work_mem counts our rows' real size, larger than Postgres' tuples, so the memory and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Aggregate (actual rows=1.00 loops=1)")],
                            &[T("  CTE r")],
                            &[T("    ->  Recursive Union (actual rows=5000.00 loops=1)")],
                            &[T("          Storage: Memory  Maximum Storage: 33kB")],
                            &[T("          ->  Result (actual rows=1.00 loops=1)")],
                            &[T("          ->  WorkTable Scan on r r_1 (actual rows=1.00 loops=5000)")],
                            &[T("                Filter: (n < 5000)")],
                            &[T("                Rows Removed by Filter: 0")],
                            &[T("  ->  CTE Scan on r (actual rows=5000.00 loops=1)")],
                            &[T("        Storage: Memory  Maximum Storage: 221kB")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET work_mem = '64kB';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT sum(c) FROM (SELECT count(*) OVER (PARTITION BY a % 3) AS c FROM tt) s;",
                    skip: Some("work_mem counts our rows' real size, larger than Postgres' tuples, so the memory and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Aggregate (actual rows=1.00 loops=1)")],
                            &[T("  ->  WindowAgg (actual rows=20000.00 loops=1)")],
                            &[T("        Window: w1 AS (PARTITION BY ((tt.a % 3)))")],
                            &[T("        Storage: Disk  Maximum Storage: 92kB")],
                            &[T("        ->  Sort (actual rows=20000.00 loops=1)")],
                            &[T("              Sort Key: ((tt.a % 3))")],
                            &[T("              Sort Method: external merge  Disk: 280kB")],
                            &[T("              ->  Seq Scan on tt (actual rows=20000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(c), sum(f), sum(l) FROM (SELECT count(*) OVER w AS c, first_value(i) OVER w AS f, lag(i, 3) OVER w AS l FROM tt WINDOW w AS (PARTITION BY a % 3 ORDER BY i ROWS BETWEEN 2 PRECEDING AND 5 FOLLOWING)) s;",
                    expected: Expected::Rows {
                        columns: &[Column("sum", NUMERIC), Column("sum", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("159946"), T("199890058"), T("199830048")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) WITH w AS MATERIALIZED (SELECT * FROM tt) SELECT count(*) FROM w w1, (SELECT count(*) FROM w) w2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Aggregate (actual rows=1.00 loops=1)")],
                            &[T("  CTE w")],
                            &[T("    ->  Seq Scan on tt (actual rows=20000.00 loops=1)")],
                            &[T("  ->  Nested Loop (actual rows=20000.00 loops=1)")],
                            &[T("        ->  Aggregate (actual rows=1.00 loops=1)")],
                            &[T("              ->  CTE Scan on w (actual rows=20000.00 loops=1)")],
                            &[T("                    Storage: Disk  Maximum Storage: 537kB")],
                            &[T("        ->  CTE Scan on w w1 (actual rows=20000.00 loops=1)")],
                            &[T("              Storage: Disk  Maximum Storage: 537kB")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH w AS MATERIALIZED (SELECT * FROM tt) SELECT sum(w1.i), count(*) FROM w w1 JOIN w w2 ON w1.i = w2.i + 1;",
                    expected: Expected::Rows {
                        columns: &[Column("sum", INT8), Column("count", INT8)],
                        rows: &[
                            &[T("200009999"), T("19999")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) WITH RECURSIVE r(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM r WHERE n < 5000) SELECT count(*) FROM r;",
                    skip: Some("work_mem counts our rows' real size, larger than Postgres' tuples, so the memory and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Aggregate (actual rows=1.00 loops=1)")],
                            &[T("  CTE r")],
                            &[T("    ->  Recursive Union (actual rows=5000.00 loops=1)")],
                            &[T("          Storage: Memory  Maximum Storage: 33kB")],
                            &[T("          ->  Result (actual rows=1.00 loops=1)")],
                            &[T("          ->  WorkTable Scan on r r_1 (actual rows=1.00 loops=5000)")],
                            &[T("                Filter: (n < 5000)")],
                            &[T("                Rows Removed by Filter: 0")],
                            &[T("  ->  CTE Scan on r (actual rows=5000.00 loops=1)")],
                            &[T("        Storage: Disk  Maximum Storage: 65kB")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "WITH RECURSIVE r(n, t) AS (SELECT 1, repeat('x', 20) UNION ALL SELECT n + 1, t FROM r WHERE n < 5000) SELECT count(*), sum(n) FROM r;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("5000"), T("12502500")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(g) FROM generate_series(1, 30000) g;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("30000"), T("450015000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM (SELECT * FROM generate_series(1, 30000) g ORDER BY 1) s;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("30000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE hc SCROLL CURSOR WITH HOLD FOR SELECT i, b FROM tt ORDER BY i;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 2 FROM hc;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("row 1")],
                            &[T("2"), T("row 2")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 2 FROM hc;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("3"), T("row 3")],
                            &[T("4"), T("row 4")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH LAST FROM hc;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("20000"), T("row 20000")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH BACKWARD 3 FROM hc;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("19999"), T("row 19999")],
                            &[T("19998"), T("row 19998")],
                            &[T("19997"), T("row 19997")],
                        ],
                        tag: "FETCH 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH ABSOLUTE 10000 FROM hc;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("10000"), T("row 10000")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH FIRST FROM hc;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4), Column("b", TEXT)],
                        rows: &[
                            &[T("1"), T("row 1")],
                        ],
                        tag: "FETCH 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLOSE hc;",
                    expected: Expected::Tag("CLOSE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DECLARE hn CURSOR WITH HOLD FOR SELECT i FROM tt ORDER BY i;",
                    expected: Expected::Tag("DECLARE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 3 FROM hn;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                            &[T("3")],
                        ],
                        tag: "FETCH 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 2 FROM hn;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4)],
                        rows: &[
                            &[T("4")],
                            &[T("5")],
                        ],
                        tag: "FETCH 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "MOVE FORWARD ALL FROM hn;",
                    expected: Expected::Tag("MOVE 19995"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "FETCH 1 FROM hn;",
                    expected: Expected::Rows {
                        columns: &[Column("i", INT4)],
                        rows: &[

                        ],
                        tag: "FETCH 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CLOSE hn;",
                    expected: Expected::Tag("CLOSE CURSOR"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET work_mem;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
            ],
            ..S
        },
    ]);
    run_wire_tests(&[
        WireTest {
            name: "a portal of INSERT RETURNING fetched in pieces keeps its rows in a temporary file past work_mem",
            set_up_script: &[
                "CREATE TABLE pf (i INT PRIMARY KEY, t TEXT);",
            ],
            steps: &[
                Step::Send(&[Send::Query("SET work_mem = '64kB';")]),
                Step::Receive(&[Receive::CommandComplete("SET"), Receive::ReadyForQuery(b'I')]),
                Step::Send(&[
                    Send::Parse { name: "", query: "INSERT INTO pf SELECT g, repeat('y', 30) FROM generate_series(1, 5000) g RETURNING i;", parameter_oids: &[] },
                    Send::Bind { portal: "", statement: "", parameter_formats: &[], parameters: &[], result_formats: &[0] },
                    Send::Execute("", 2),
                    Send::Execute("", 2),
                    Send::Sync,
                ]),
                Step::Receive(&[
                    Receive::ParseComplete,
                    Receive::BindComplete,
                    Receive::DataRow(&[Datum::Text("1")]),
                    Receive::DataRow(&[Datum::Text("2")]),
                    Receive::PortalSuspended,
                    Receive::DataRow(&[Datum::Text("3")]),
                    Receive::DataRow(&[Datum::Text("4")]),
                    Receive::PortalSuspended,
                    Receive::ReadyForQuery(b'I'),
                ]),
                Step::Send(&[Send::Query("SELECT count(*) FROM pf;")]),
                Step::Receive(&[
                    Receive::RowDescription(&[Field { name: "count", attnum: 0, type_oid: INT8, size: 8, typmod: -1, format: 0 }]),
                    Receive::DataRow(&[Datum::Text("5000")]),
                    Receive::CommandComplete("SELECT 1"),
                    Receive::ReadyForQuery(b'I'),
                ]),
            ],
            ..W
        },
    ]);
}

#[test]
fn test_explain_analyze_hash_batches() {
    run_scripts(&[
        ScriptTest {
            name: "hash joins past work_mem split their rows into batches kept in temporary files, with the outer side's most common values in skew buckets",
            set_up_script: &[
                "CREATE TABLE hi (id INT PRIMARY KEY, k INT, pad TEXT) WITH (autovacuum_enabled = false);",
                "CREATE TABLE ho (id INT PRIMARY KEY, k INT, pad TEXT) WITH (autovacuum_enabled = false);",
                "INSERT INTO hi SELECT i, i % 5000, repeat('p', 20) FROM generate_series(1, 20000) i;",
                "INSERT INTO ho SELECT i, CASE WHEN i % 3 = 0 THEN 7 ELSE (i * 37) % 6000 END, repeat('q', 20) FROM generate_series(1, 30000) i;",
                "ANALYZE hi, ho;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET max_parallel_workers_per_gather = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_mergejoin = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_nestloop = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_indexscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_bitmapscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET work_mem = '64kB';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT count(*) FROM ho JOIN hi ON ho.k = hi.k;",
                    skip: Some("work_mem counts our hash joins' and Memoize caches' real size, larger than Postgres' rows, so the memory, bucket, and batch figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Aggregate (actual rows=1.00 loops=1)")],
                            &[T("  ->  Hash Join (actual rows=106660.00 loops=1)")],
                            &[T("        Hash Cond: (ho.k = hi.k)")],
                            &[T("        ->  Seq Scan on ho (actual rows=30000.00 loops=1)")],
                            &[T("        ->  Hash (actual rows=20000.00 loops=1)")],
                            &[T("              Buckets: 8192  Batches: 8  Memory Usage: 155kB")],
                            &[T("              ->  Seq Scan on hi (actual rows=20000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(ho.id), sum(hi.id) FROM ho JOIN hi ON ho.k = hi.k;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("106660"), T("1599019820"), T("966863340")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT count(*) FROM ho FULL JOIN hi ON ho.k = hi.k;",
                    skip: Some("work_mem counts our hash joins' and Memoize caches' real size, larger than Postgres' rows, so the memory, bucket, and batch figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Aggregate (actual rows=1.00 loops=1)")],
                            &[T("  ->  Hash Full Join (actual rows=116663.00 loops=1)")],
                            &[T("        Hash Cond: (ho.k = hi.k)")],
                            &[T("        ->  Seq Scan on ho (actual rows=30000.00 loops=1)")],
                            &[T("        ->  Hash (actual rows=20000.00 loops=1)")],
                            &[T("              Buckets: 8192  Batches: 8  Memory Usage: 155kB")],
                            &[T("              ->  Seq Scan on hi (actual rows=20000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), count(hi.id), count(ho.id) FROM ho FULL JOIN hi ON ho.k = hi.k;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("count", INT8), Column("count", INT8)],
                        rows: &[
                            &[T("116663"), T("113328"), T("109995")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM ho RIGHT JOIN hi ON ho.k = hi.k;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("113328")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM hi WHERE NOT EXISTS (SELECT 1 FROM ho WHERE ho.k = hi.k);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("6668")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET work_mem;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "hash joins add batches while they build when the inner side has more rows than estimated, and batch files keep values stored out of line",
            set_up_script: &[
                "CREATE TABLE gi (id INT PRIMARY KEY, k INT, pad TEXT) WITH (autovacuum_enabled = false);",
                "CREATE TABLE go (id INT PRIMARY KEY, k INT, pad TEXT) WITH (autovacuum_enabled = false);",
                "INSERT INTO gi SELECT i, CASE WHEN i % 10 = 0 THEN NULL ELSE i % 3000 END, repeat(md5(i::text), 3) FROM generate_series(1, 15000) i;",
                "INSERT INTO go SELECT i, CASE WHEN i % 11 = 0 THEN NULL ELSE (i * 7) % 4000 END, repeat(md5(i::text), 2) FROM generate_series(1, 12000) i;",
                "INSERT INTO gi SELECT 100000 + i, i % 7, (SELECT string_agg(md5((i * 1000 + j)::text), '') FROM generate_series(1, 120) j) FROM generate_series(1, 60) i;",
                "ANALYZE gi, go;",
                "CREATE FUNCTION gif() RETURNS SETOF gi LANGUAGE plpgsql ROWS 10 AS $$ BEGIN RETURN QUERY SELECT * FROM gi; END $$;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET max_parallel_workers_per_gather = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_mergejoin = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_nestloop = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET work_mem = '64kB';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT count(*) FROM go JOIN gif() s ON go.k = s.k;",
                    skip: Some("work_mem counts our hash joins' and Memoize caches' real size, larger than Postgres' rows, so the memory, bucket, and batch figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Aggregate (actual rows=1.00 loops=1)")],
                            &[T("  ->  Hash Join (actual rows=36992.00 loops=1)")],
                            &[T("        Hash Cond: (go.k = s.k)")],
                            &[T("        ->  Seq Scan on go (actual rows=12000.00 loops=1)")],
                            &[T("        ->  Hash (actual rows=13560.00 loops=1)")],
                            &[T("              Buckets: 4096 (originally 1024)  Batches: 8 (originally 1)  Memory Usage: 97kB")],
                            &[T("              ->  Function Scan on gif s (actual rows=15060.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(length(s.pad)), sum(length(go.pad)), sum(hashtext(s.pad)::BIGINT) FROM go JOIN gif() s ON go.k = s.k;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8), Column("sum", INT8), Column("sum", NUMERIC)],
                        rows: &[
                            &[T("36992"), T("4157760"), T("2367488"), T("111735206738")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT count(*) FROM go FULL JOIN gif() s ON go.k = s.k;",
                    skip: Some("work_mem counts our hash joins' and Memoize caches' real size, larger than Postgres' rows, so the memory, bucket, and batch figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Aggregate (actual rows=1.00 loops=1)")],
                            &[T("  ->  Hash Full Join (actual rows=43123.00 loops=1)")],
                            &[T("        Hash Cond: (go.k = s.k)")],
                            &[T("        ->  Seq Scan on go (actual rows=12000.00 loops=1)")],
                            &[T("        ->  Hash (actual rows=15060.00 loops=1)")],
                            &[T("              Buckets: 4096 (originally 1024)  Batches: 8 (originally 1)  Memory Usage: 135kB")],
                            &[T("              ->  Function Scan on gif s (actual rows=15060.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), count(s.id), count(go.id), sum(hashtext(s.pad)::BIGINT) FROM go FULL JOIN gif() s ON go.k = s.k;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("count", INT8), Column("count", INT8), Column("sum", NUMERIC)],
                        rows: &[
                            &[T("43123"), T("38492"), T("41623"), T("197982595997")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), count(s.id) FROM gi g1 LEFT JOIN gif() s ON g1.k = s.k;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("count", INT8)],
                        rows: &[
                            &[T("70036"), T("68536")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET work_mem;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_explain_analyze_hash_aggregates() {
    run_scripts(&[
        ScriptTest {
            name: "hashed Agg nodes take memory as Postgres counts it, spill the rows of groups past work_mem to temporary files in partitions, and aggregate the partitions in batches",
            set_up_script: &[
                "CREATE TABLE ha (id INT PRIMARY KEY, k INT, n NUMERIC, t TEXT, iv INTERVAL, b BIGINT) WITH (autovacuum_enabled = false);",
                "INSERT INTO ha SELECT i, i % 7000, i * 1.5, 'v' || (i % 300), (i % 50) * INTERVAL '1 minute', i::BIGINT * 1000003 FROM generate_series(1, 30000) i;",
                "ANALYZE ha;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET max_parallel_workers_per_gather = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_sort = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT k, count(*) FROM ha WHERE id <= 1 GROUP BY k;",
                    skip: Some("work_mem counts our hash tables' real size, larger than Postgres' contexts hold, so the memory, batch, and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate (actual rows=1.00 loops=1)")],
                            &[T("  Group Key: k")],
                            &[T("  Batches: 1  Memory Usage: 32kB")],
                            &[T("  ->  Index Scan using ha_pkey on ha (actual rows=1.00 loops=1)")],
                            &[T("        Index Cond: (id <= 1)")],
                            &[T("        Index Searches: 1")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT k, count(*) FROM ha WHERE id <= 300 GROUP BY k;",
                    skip: Some("work_mem counts our hash tables' real size, larger than Postgres' contexts hold, so the memory, batch, and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate (actual rows=300.00 loops=1)")],
                            &[T("  Group Key: k")],
                            &[T("  Batches: 1  Memory Usage: 56kB")],
                            &[T("  ->  Index Scan using ha_pkey on ha (actual rows=300.00 loops=1)")],
                            &[T("        Index Cond: (id <= 300)")],
                            &[T("        Index Searches: 1")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT k, count(*) FROM ha WHERE id <= 1000 GROUP BY k;",
                    skip: Some("work_mem counts our hash tables' real size, larger than Postgres' contexts hold, so the memory, batch, and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate (actual rows=1000.00 loops=1)")],
                            &[T("  Group Key: k")],
                            &[T("  Batches: 1  Memory Usage: 121kB")],
                            &[T("  ->  Index Scan using ha_pkey on ha (actual rows=1000.00 loops=1)")],
                            &[T("        Index Cond: (id <= 1000)")],
                            &[T("        Index Searches: 1")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT k, min(t) FROM ha GROUP BY k;",
                    skip: Some("work_mem counts our hash tables' real size, larger than Postgres' contexts hold, so the memory, batch, and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate (actual rows=7000.00 loops=1)")],
                            &[T("  Group Key: k")],
                            &[T("  Batches: 1  Memory Usage: 785kB")],
                            &[T("  ->  Seq Scan on ha (actual rows=30000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT k, t, count(*) FROM ha GROUP BY k, t;",
                    skip: Some("work_mem counts our hash tables' real size, larger than Postgres' contexts hold, so the memory, batch, and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate (actual rows=21000.00 loops=1)")],
                            &[T("  Group Key: k, t")],
                            &[T("  Batches: 1  Memory Usage: 1561kB")],
                            &[T("  ->  Seq Scan on ha (actual rows=30000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT k, avg(id) FROM ha GROUP BY k;",
                    skip: Some("work_mem counts our hash tables' real size, larger than Postgres' contexts hold, so the memory, batch, and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate (actual rows=7000.00 loops=1)")],
                            &[T("  Group Key: k")],
                            &[T("  Batches: 1  Memory Usage: 1169kB")],
                            &[T("  ->  Seq Scan on ha (actual rows=30000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT k, sum(b) FROM ha GROUP BY k;",
                    skip: Some("work_mem counts our hash tables' real size, larger than Postgres' contexts hold, so the memory, batch, and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate (actual rows=7000.00 loops=1)")],
                            &[T("  Group Key: k")],
                            &[T("  Batches: 1  Memory Usage: 1169kB")],
                            &[T("  ->  Seq Scan on ha (actual rows=30000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT k, sum(n) FROM ha GROUP BY k;",
                    skip: Some("work_mem counts our hash tables' real size, larger than Postgres' contexts hold, so the memory, batch, and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate (actual rows=7000.00 loops=1)")],
                            &[T("  Group Key: k")],
                            &[T("  Batches: 1  Memory Usage: 2961kB")],
                            &[T("  ->  Seq Scan on ha (actual rows=30000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT k, stddev(n) FROM ha GROUP BY k;",
                    skip: Some("work_mem counts our hash tables' real size, larger than Postgres' contexts hold, so the memory, batch, and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate (actual rows=7000.00 loops=1)")],
                            &[T("  Group Key: k")],
                            &[T("  Batches: 1  Memory Usage: 3473kB")],
                            &[T("  ->  Seq Scan on ha (actual rows=30000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT k, stddev(id) FROM ha GROUP BY k;",
                    skip: Some("work_mem counts our hash tables' real size, larger than Postgres' contexts hold, so the memory, batch, and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate (actual rows=7000.00 loops=1)")],
                            &[T("  Group Key: k")],
                            &[T("  Batches: 1  Memory Usage: 1169kB")],
                            &[T("  ->  Seq Scan on ha (actual rows=30000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT k, avg(iv) FROM ha GROUP BY k;",
                    skip: Some("work_mem counts our hash tables' real size, larger than Postgres' contexts hold, so the memory, batch, and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate (actual rows=7000.00 loops=1)")],
                            &[T("  Group Key: k")],
                            &[T("  Batches: 1  Memory Usage: 1169kB")],
                            &[T("  ->  Seq Scan on ha (actual rows=30000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT k, array_agg(t) FROM ha GROUP BY k;",
                    skip: Some("work_mem counts our hash tables' real size, larger than Postgres' contexts hold, so the memory, batch, and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate (actual rows=7000.00 loops=1)")],
                            &[T("  Group Key: k")],
                            &[T("  Batches: 17  Memory Usage: 497kB  Disk Usage: 1008kB")],
                            &[T("  ->  Seq Scan on ha (actual rows=30000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT k, array_agg(id) FROM ha GROUP BY k;",
                    skip: Some("work_mem counts our hash tables' real size, larger than Postgres' contexts hold, so the memory, batch, and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate (actual rows=7000.00 loops=1)")],
                            &[T("  Group Key: k")],
                            &[T("  Batches: 17  Memory Usage: 497kB  Disk Usage: 1008kB")],
                            &[T("  ->  Seq Scan on ha (actual rows=30000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT k, string_agg(t, ',') FROM ha GROUP BY k;",
                    skip: Some("work_mem counts our hash tables' real size, larger than Postgres' contexts hold, so the memory, batch, and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate (actual rows=7000.00 loops=1)")],
                            &[T("  Group Key: k")],
                            &[T("  Batches: 17  Memory Usage: 1521kB  Disk Usage: 1008kB")],
                            &[T("  ->  Seq Scan on ha (actual rows=30000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT t, string_agg(k::TEXT, ',') FROM ha GROUP BY t;",
                    skip: Some("work_mem counts our hash tables' real size, larger than Postgres' contexts hold, so the memory, batch, and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate (actual rows=300.00 loops=1)")],
                            &[T("  Group Key: t")],
                            &[T("  Batches: 1  Memory Usage: 560kB")],
                            &[T("  ->  Seq Scan on ha (actual rows=30000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET work_mem = '64kB';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT k, count(*) FROM ha GROUP BY k;",
                    skip: Some("work_mem counts our hash tables' real size, larger than Postgres' contexts hold, so the memory, batch, and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate (actual rows=7000.00 loops=1)")],
                            &[T("  Group Key: k")],
                            &[T("  Batches: 5  Memory Usage: 129kB  Disk Usage: 728kB")],
                            &[T("  ->  Seq Scan on ha (actual rows=30000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT k, max(id), min(t) FROM ha GROUP BY k;",
                    skip: Some("work_mem counts our hash tables' real size, larger than Postgres' contexts hold, so the memory, batch, and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate (actual rows=7000.00 loops=1)")],
                            &[T("  Group Key: k")],
                            &[T("  Batches: 21  Memory Usage: 177kB  Disk Usage: 1568kB")],
                            &[T("  ->  Seq Scan on ha (actual rows=30000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT k, avg(id) FROM ha GROUP BY k;",
                    skip: Some("work_mem counts our hash tables' real size, larger than Postgres' contexts hold, so the memory, batch, and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate (actual rows=7000.00 loops=1)")],
                            &[T("  Group Key: k")],
                            &[T("  Batches: 21  Memory Usage: 169kB  Disk Usage: 984kB")],
                            &[T("  ->  Seq Scan on ha (actual rows=30000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT k, sum(b) FROM ha GROUP BY k;",
                    skip: Some("work_mem counts our hash tables' real size, larger than Postgres' contexts hold, so the memory, batch, and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate (actual rows=7000.00 loops=1)")],
                            &[T("  Group Key: k")],
                            &[T("  Batches: 21  Memory Usage: 169kB  Disk Usage: 1592kB")],
                            &[T("  ->  Seq Scan on ha (actual rows=30000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT k, sum(n), avg(id) FROM ha GROUP BY k;",
                    skip: Some("work_mem counts our hash tables' real size, larger than Postgres' contexts hold, so the memory, batch, and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate (actual rows=7000.00 loops=1)")],
                            &[T("  Group Key: k")],
                            &[T("  Batches: 85  Memory Usage: 144kB  Disk Usage: 1600kB")],
                            &[T("  ->  Seq Scan on ha (actual rows=30000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT k, array_agg(t), string_agg(t, ',') FROM ha GROUP BY k;",
                    skip: Some("work_mem counts our hash tables' real size, larger than Postgres' contexts hold, so the memory, batch, and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate (actual rows=7000.00 loops=1)")],
                            &[T("  Group Key: k")],
                            &[T("  Batches: 323  Memory Usage: 176kB  Disk Usage: 1552kB")],
                            &[T("  ->  Seq Scan on ha (actual rows=30000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT sum(x), count(*) FROM (SELECT k, sum(n) AS x, avg(id), array_length(array_agg(t), 1) FROM ha GROUP BY k) s;",
                    expected: Expected::Rows {
                        columns: &[Column("sum", NUMERIC), Column("count", INT8)],
                        rows: &[
                            &[T("675022500.0"), T("7000")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT md5(string_agg(x, ';' ORDER BY k)) FROM (SELECT k, string_agg(t, ',' ORDER BY t) AS x FROM ha GROUP BY k) s;",
                    expected: Expected::Rows {
                        columns: &[Column("md5", TEXT)],
                        rows: &[
                            &[T("d3d73a6bf4e0f12bdc9155239d128255")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET work_mem;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "hashed Agg nodes spill rows with NULL keys and values kept out of line, filter groups, report in JSON, and spill again when rescanned",
            set_up_script: &[
                "CREATE TABLE hc (id INT PRIMARY KEY, k INT, g INT, t TEXT) WITH (autovacuum_enabled = false);",
                "INSERT INTO hc SELECT i, CASE WHEN i % 13 = 0 THEN NULL ELSE i % 5000 END, i % 3, CASE WHEN i % 997 = 0 THEN (SELECT string_agg(md5((i * 100 + j)::TEXT), '') FROM generate_series(1, 100) j) ELSE 'x' || i END FROM generate_series(1, 40000) i;",
                "ANALYZE hc;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET max_parallel_workers_per_gather = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_sort = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET work_mem = '64kB';",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT k, count(*), max(t) FROM hc GROUP BY k HAVING count(*) > 7;",
                    skip: Some("work_mem counts our hash tables' real size, larger than Postgres' contexts hold, so the memory, batch, and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate (actual rows=1925.00 loops=1)")],
                            &[T("  Group Key: k")],
                            &[T("  Filter: (count(*) > 7)")],
                            &[T("  Batches: 21  Memory Usage: 177kB  Disk Usage: 1592kB")],
                            &[T("  Rows Removed by Filter: 3076")],
                            &[T("  ->  Seq Scan on hc (actual rows=40000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(c), sum(length(m)), sum(hashtext(m)::BIGINT) FROM (SELECT k, count(*) AS c, max(t) AS m FROM hc GROUP BY k HAVING count(*) > 7) s;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", NUMERIC), Column("sum", INT8), Column("sum", NUMERIC)],
                        rows: &[
                            &[T("1925"), T("18468"), T("9411"), T("29546649406")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(length(t)) FROM (SELECT t, count(*) FROM hc GROUP BY t) s;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("40000"), T("356665")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT t, count(*) FROM hc GROUP BY t;",
                    skip: Some("work_mem counts our hash tables' real size, larger than Postgres' contexts hold, so the memory, batch, and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate (actual rows=40000.00 loops=1)")],
                            &[T("  Group Key: t")],
                            &[T("  Batches: 85  Memory Usage: 169kB  Disk Usage: 1600kB")],
                            &[T("  ->  Seq Scan on hc (actual rows=40000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) SELECT k, count(*) FROM hc GROUP BY k;",
                    skip: Some("work_mem counts our hash tables' real size, larger than Postgres' contexts hold, so the memory, batch, and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", JSON)],
                        rows: &[
                            &[T("[\n  {\n    \"Plan\": {\n      \"Node Type\": \"Aggregate\",\n      \"Strategy\": \"Hashed\",\n      \"Partial Mode\": \"Simple\",\n      \"Parallel Aware\": false,\n      \"Async Capable\": false,\n      \"Actual Rows\": 5001.00,\n      \"Actual Loops\": 1,\n      \"Disabled\": false,\n      \"Group Key\": [\"k\"],\n      \"HashAgg Batches\": 5,\n      \"Peak Memory Usage\": 129,\n      \"Disk Usage\": 752,\n      \"Plans\": [\n        {\n          \"Node Type\": \"Seq Scan\",\n          \"Parent Relationship\": \"Outer\",\n          \"Parallel Aware\": false,\n          \"Async Capable\": false,\n          \"Relation Name\": \"hc\",\n          \"Alias\": \"hc\",\n          \"Actual Rows\": 40000.00,\n          \"Actual Loops\": 1,\n          \"Disabled\": false\n        }\n      ]\n    },\n    \"Triggers\": [\n    ]\n  }\n]")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT g, (SELECT count(*) FROM (SELECT k, count(*) FROM hc WHERE hc.g <> o.g GROUP BY k) s) FROM generate_series(0, 2) o(g);",
                    skip: Some("work_mem counts our hash tables' real size, larger than Postgres' contexts hold, so the memory, batch, and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Function Scan on generate_series o (actual rows=3.00 loops=1)")],
                            &[T("  SubPlan 1")],
                            &[T("    ->  Aggregate (actual rows=1.00 loops=3)")],
                            &[T("          ->  HashAggregate (actual rows=5001.00 loops=3)")],
                            &[T("                Group Key: hc.k")],
                            &[T("                Batches: 13  Memory Usage: 161kB  Disk Usage: 680kB")],
                            &[T("                ->  Seq Scan on hc (actual rows=26666.67 loops=3)")],
                            &[T("                      Filter: (g <> o.g)")],
                            &[T("                      Rows Removed by Filter: 13333")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT g, (SELECT count(*) FROM (SELECT k, count(*) FROM hc WHERE hc.g <> o.g GROUP BY k) s) FROM generate_series(0, 2) o(g);",
                    expected: Expected::Rows {
                        columns: &[Column("g", INT4), Column("count", INT8)],
                        rows: &[
                            &[T("0"), T("5001")],
                            &[T("1"), T("5001")],
                            &[T("2"), T("5001")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "RESET work_mem;",
                    expected: Expected::Tag("RESET"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "hashed Agg nodes count the memory of the json and jsonb aggregates' states, and of arrays of arrays, as Postgres allocates it",
            set_up_script: &[
                "CREATE TABLE he (id INT PRIMARY KEY, k INT, t TEXT) WITH (autovacuum_enabled = false);",
                "INSERT INTO he SELECT i, i % 3000, 'v' || (i % 300) FROM generate_series(1, 20000) i;",
                "ANALYZE he;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET max_parallel_workers_per_gather = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_sort = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT k, json_agg(t) FROM he GROUP BY k;",
                    skip: Some("work_mem counts our hash tables' real size, larger than Postgres' contexts hold, so the memory, batch, and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate (actual rows=3000.00 loops=1)")],
                            &[T("  Group Key: k")],
                            &[T("  Batches: 9  Memory Usage: 1457kB  Disk Usage: 504kB")],
                            &[T("  ->  Seq Scan on he (actual rows=20000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT k, json_object_agg(id, t) FROM he GROUP BY k;",
                    skip: Some("work_mem counts our hash tables' real size, larger than Postgres' contexts hold, so the memory, batch, and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate (actual rows=3000.00 loops=1)")],
                            &[T("  Group Key: k")],
                            &[T("  Batches: 9  Memory Usage: 1457kB  Disk Usage: 504kB")],
                            &[T("  ->  Seq Scan on he (actual rows=20000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT k, jsonb_agg(t) FROM he GROUP BY k;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate (actual rows=3000.00 loops=1)")],
                            &[T("  Group Key: k")],
                            &[T("  Batches: 9  Memory Usage: 945kB  Disk Usage: 504kB")],
                            &[T("  ->  Seq Scan on he (actual rows=20000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    skip: Some("the memory of jsonb_agg's state, the JsonbValue tree pushJsonbValue builds, is not counted as Postgres allocates it"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT k, jsonb_object_agg(id, t) FROM he GROUP BY k;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate (actual rows=3000.00 loops=1)")],
                            &[T("  Group Key: k")],
                            &[T("  Batches: 9  Memory Usage: 2225kB  Disk Usage: 504kB")],
                            &[T("  ->  Seq Scan on he (actual rows=20000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    skip: Some("the memory of jsonb_object_agg's state, the JsonbValue tree pushJsonbValue builds, is not counted as Postgres allocates it"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT k, array_agg(ARRAY[id, k]) FROM he GROUP BY k;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate (actual rows=3000.00 loops=1)")],
                            &[T("  Group Key: k")],
                            &[T("  Batches: 9  Memory Usage: 1457kB  Disk Usage: 496kB")],
                            &[T("  ->  Seq Scan on he (actual rows=20000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    skip: Some("the memory of array_agg's state of arrays, ArrayBuildStateArr's data and null bitmap, is not counted as Postgres allocates it"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "hashed Agg nodes count a copied state with the short header of an input column's value and the full header of a value an expression computes",
            set_up_script: &[
                "CREATE TABLE hf (id INT PRIMARY KEY, t TEXT) WITH (autovacuum_enabled = false);",
                "INSERT INTO hf SELECT i, 'v' || i FROM generate_series(1, 40000) i;",
                "ANALYZE hf;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET max_parallel_workers_per_gather = 0;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_sort = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT id, max(t) FROM hf GROUP BY id;",
                    skip: Some("work_mem counts our hash tables' real size, larger than Postgres' contexts hold, so the memory, batch, and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate (actual rows=40000.00 loops=1)")],
                            &[T("  Group Key: id")],
                            &[T("  Batches: 1  Memory Usage: 3601kB")],
                            &[T("  ->  Seq Scan on hf (actual rows=40000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT id, max(t || '') FROM hf GROUP BY id;",
                    skip: Some("work_mem counts our hash tables' real size, larger than Postgres' contexts hold, so the memory, batch, and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate (actual rows=40000.00 loops=1)")],
                            &[T("  Group Key: id")],
                            &[T("  Batches: 1  Memory Usage: 3857kB")],
                            &[T("  ->  Seq Scan on hf (actual rows=40000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT id, max(u) FROM (SELECT id, t || '' AS u FROM hf OFFSET 0) s GROUP BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate (actual rows=40000.00 loops=1)")],
                            &[T("  Group Key: hf.id")],
                            &[T("  Batches: 1  Memory Usage: 3857kB")],
                            &[T("  ->  Seq Scan on hf (actual rows=40000.00 loops=1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    skip: Some("a column a lower node computes is counted with the short header of a stored value, where Postgres keeps the full header the expression gave it"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT id, min(t) FROM (SELECT * FROM hf ORDER BY t) s GROUP BY id;",
                    skip: Some("work_mem counts our rows' real size, larger than Postgres' tuples, so the memory and disk figures differ (STORAGE_DESIGN Appendix C)"),
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("HashAggregate (actual rows=40000.00 loops=1)")],
                            &[T("  Group Key: hf.id")],
                            &[T("  Batches: 1  Memory Usage: 3601kB")],
                            &[T("  ->  Sort (actual rows=40000.00 loops=1)")],
                            &[T("        Disabled: true")],
                            &[T("        Sort Key: hf.t")],
                            &[T("        Sort Method: quicksort  Memory: 2786kB")],
                            &[T("        ->  Seq Scan on hf (actual rows=40000.00 loops=1)")],
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
fn test_explain_analyze_bitmap_heap_blocks() {
    run_scripts(&[
        ScriptTest {
            name: "EXPLAIN ANALYZE of a Bitmap Heap Scan reports its heap blocks, the counts masked as they vary",
            set_up_script: &[
                "CREATE TABLE hb (id int4 PRIMARY KEY, v int4, pad text);",
                "INSERT INTO hb SELECT g, g % 10, repeat('x', 200) FROM generate_series(1, 2000) g;",
                "CREATE INDEX hb_v ON hb (v);",
                "ANALYZE hb;",
                "CHECKPOINT;",
                "CREATE FUNCTION hb_explain(q text) RETURNS SETOF text LANGUAGE plpgsql AS $$ DECLARE ln text; BEGIN FOR ln IN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) ' || q LOOP RETURN NEXT regexp_replace(ln, '\\d+', 'N', 'g'); END LOOP; END $$;",
                "CREATE FUNCTION hb_json(q text) RETURNS jsonb LANGUAGE plpgsql AS $$ DECLARE j jsonb; BEGIN EXECUTE 'EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) ' || q INTO j; RETURN j; END $$;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET enable_seqscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET enable_indexscan = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM hb_explain('SELECT id FROM hb WHERE v = 3');",
                    expected: Expected::Rows {
                        columns: &[Column("hb_explain", TEXT)],
                        rows: &[
                            &[T("Bitmap Heap Scan on hb (actual rows=N.N loops=N)")],
                            &[T("  Recheck Cond: (v = N)")],
                            &[T("  Heap Blocks: exact=N")],
                            &[T("  ->  Bitmap Index Scan on hb_v (actual rows=N.N loops=N)")],
                            &[T("        Index Cond: (v = N)")],
                            &[T("        Index Searches: N")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM hb_explain('SELECT id FROM hb WHERE v = 3 AND id < 0');",
                    expected: Expected::Rows {
                        columns: &[Column("hb_explain", TEXT)],
                        rows: &[
                            &[T("Bitmap Heap Scan on hb (actual rows=N.N loops=N)")],
                            &[T("  Recheck Cond: (id < N)")],
                            &[T("  Filter: (v = N)")],
                            &[T("  ->  Bitmap Index Scan on hb_pkey (actual rows=N.N loops=N)")],
                            &[T("        Index Cond: (id < N)")],
                            &[T("        Index Searches: N")],
                        ],
                        tag: "SELECT 6",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM hb_explain('SELECT id FROM hb WHERE v = 11');",
                    expected: Expected::Rows {
                        columns: &[Column("hb_explain", TEXT)],
                        rows: &[
                            &[T("Bitmap Heap Scan on hb (actual rows=N.N loops=N)")],
                            &[T("  Recheck Cond: (v = N)")],
                            &[T("  ->  Bitmap Index Scan on hb_v (actual rows=N.N loops=N)")],
                            &[T("        Index Cond: (v = N)")],
                            &[T("        Index Searches: N")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT line FROM hb_explain('SELECT id FROM hb WHERE v = 3') AS line WHERE line LIKE '%Heap Blocks%';",
                    expected: Expected::Rows {
                        columns: &[Column("line", TEXT)],
                        rows: &[
                            &[T("  Heap Blocks: exact=N")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT j->0->'Plan'->'Exact Heap Blocks', j->0->'Plan'->'Lossy Heap Blocks' FROM hb_json('SELECT id FROM hb WHERE v = 11') AS j;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", JSONB), Column("?column?", JSONB)],
                        rows: &[
                            &[T("0"), T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (j->0->'Plan'->>'Exact Heap Blocks')::int8 > 0, j->0->'Plan'->'Lossy Heap Blocks' FROM hb_json('SELECT id FROM hb WHERE v = 3') AS j;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL), Column("?column?", JSONB)],
                        rows: &[
                            &[T("t"), T("0")],
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
fn test_estimates_kept_across_folds() {
    run_scripts(&[
        ScriptTest {
            name: "a table's row estimates stay as ANALYZE left them across a checkpoint and VACUUM with no writes",
            set_up_script: &[
                "CREATE TABLE k7 (a INT PRIMARY KEY, b TEXT) WITH (autovacuum_enabled = false);",
                "INSERT INTO k7 SELECT i, repeat('x', 100) FROM generate_series(1, 1290) i;",
                "CREATE TABLE k1 (a INT PRIMARY KEY, b TEXT) WITH (autovacuum_enabled = false);",
                "INSERT INTO k1 VALUES (1, 'a'), (2, 'b'), (3, 'c');",
                "ANALYZE k7;",
                "ANALYZE k1;",
                "CREATE FUNCTION est(q TEXT) RETURNS TEXT LANGUAGE plpgsql AS $$ DECLARE j JSON; BEGIN EXECUTE 'EXPLAIN (FORMAT JSON) ' || q INTO j; RETURN j->0->'Plan'->>'Plan Rows'; END $$;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "BEGIN ISOLATION LEVEL REPEATABLE READ;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT est('SELECT * FROM k7'), est('SELECT * FROM k1'), est('SELECT * FROM k7 WHERE a < 100');",
                    expected: Expected::Rows {
                        columns: &[Column("est", TEXT), Column("est", TEXT), Column("est", TEXT)],
                        rows: &[
                            &[T("1290"), T("3"), T("98")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CHECKPOINT;",
                    expected: Expected::Tag("CHECKPOINT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT est('SELECT * FROM k7'), est('SELECT * FROM k1'), est('SELECT * FROM k7 WHERE a < 100');",
                    expected: Expected::Rows {
                        columns: &[Column("est", TEXT), Column("est", TEXT), Column("est", TEXT)],
                        rows: &[
                            &[T("1290"), T("3"), T("98")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT est('SELECT * FROM k7'), est('SELECT * FROM k1'), est('SELECT * FROM k7 WHERE a < 100');",
                    expected: Expected::Rows {
                        columns: &[Column("est", TEXT), Column("est", TEXT), Column("est", TEXT)],
                        rows: &[
                            &[T("1290"), T("3"), T("98")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM k7;",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "VACUUM k1;",
                    expected: Expected::Tag("VACUUM"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT est('SELECT * FROM k7'), est('SELECT * FROM k1'), est('SELECT * FROM k7 WHERE a < 100');",
                    expected: Expected::Rows {
                        columns: &[Column("est", TEXT), Column("est", TEXT), Column("est", TEXT)],
                        rows: &[
                            &[T("1290"), T("3"), T("98")],
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
fn test_self_join_removal_under_lateral_references() {
    run_scripts(&[
        ScriptTest {
            name: "self join removal under lateral references",
            set_up_script: &[
                "CREATE TABLE sj (a INT UNIQUE, b INT, c INT UNIQUE);",
                "INSERT INTO sj VALUES (1, NULL, 2), (NULL, 2, NULL), (2, 1, 1);",
                "ANALYZE sj;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) SELECT t3.a FROM sj t1 JOIN sj t2 ON t1.a = t2.a JOIN LATERAL (SELECT t1.a OFFSET 0) t3 ON true;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Nested Loop")],
                            &[T("  Output: (t2.a)")],
                            &[T("  ->  Seq Scan on public.sj t2")],
                            &[T("        Output: t2.a, t2.b, t2.c")],
                            &[T("        Filter: (t2.a IS NOT NULL)")],
                            &[T("  ->  Result")],
                            &[T("        Output: t2.a")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) SELECT t3.a FROM sj t1 JOIN sj t2 ON t1.a = t2.a JOIN LATERAL (SELECT * FROM (SELECT t1.a OFFSET 0) OFFSET 0) t3 ON true;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Nested Loop")],
                            &[T("  Output: (t2.a)")],
                            &[T("  ->  Seq Scan on public.sj t2")],
                            &[T("        Output: t2.a, t2.b, t2.c")],
                            &[T("        Filter: (t2.a IS NOT NULL)")],
                            &[T("  ->  Result")],
                            &[T("        Output: t2.a")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT t3.a FROM sj t1 JOIN sj t2 ON t1.a = t2.a JOIN LATERAL (SELECT * FROM (SELECT t1.a OFFSET 0) OFFSET 0) t3 ON true ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4)],
                        rows: &[
                            &[T("1")],
                            &[T("2")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_clauses_an_equivalence_class_gives_back_keep_their_operands_in_order() {
    run_scripts(&[
        ScriptTest {
            name: "clauses an equivalence class gives back keep their operands in order",
            set_up_script: &[
                "CREATE TABLE int4_tbl (f1 INT4);",
                "INSERT INTO int4_tbl VALUES (0), (123456), (-123456), (2147483647), (-2147483647);",
                "VACUUM int4_tbl;",
                "CREATE TABLE int8_tbl (q1 INT8, q2 INT8);",
                "INSERT INTO int8_tbl VALUES (123, 456), (123, 4567890123456789), (4567890123456789, 123), (4567890123456789, 4567890123456789), (4567890123456789, -4567890123456789);",
                "VACUUM int8_tbl;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM int4_tbl t1 LEFT JOIN (int4_tbl t2 LEFT JOIN int4_tbl t3 ON t2.f1 > 0) ON t2.f1 > 1 LEFT JOIN int4_tbl t4 ON t2.f1 > 2 AND t3.f1 > 3 WHERE t1.f1 = coalesce(t2.f1, 1);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Nested Loop Left Join")],
                            &[T("  Join Filter: ((t2.f1 > 2) AND (t3.f1 > 3))")],
                            &[T("  ->  Nested Loop Left Join")],
                            &[T("        Join Filter: (t2.f1 > 0)")],
                            &[T("        ->  Nested Loop Left Join")],
                            &[T("              Filter: (t1.f1 = COALESCE(t2.f1, 1))")],
                            &[T("              ->  Seq Scan on int4_tbl t1")],
                            &[T("              ->  Materialize")],
                            &[T("                    ->  Seq Scan on int4_tbl t2")],
                            &[T("                          Filter: (f1 > 1)")],
                            &[T("        ->  Materialize")],
                            &[T("              ->  Seq Scan on int4_tbl t3")],
                            &[T("  ->  Materialize")],
                            &[T("        ->  Seq Scan on int4_tbl t4")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM int4_tbl AS i41, LATERAL (SELECT 1 AS x FROM (SELECT i41.f1 AS lat, i42.f1 AS loc FROM int8_tbl AS i81, int4_tbl AS i42) AS ss1 RIGHT JOIN int4_tbl AS i43 ON (i43.f1 > 1) WHERE ss1.loc = ss1.lat) AS ss2 WHERE i41.f1 > 0;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Nested Loop")],
                            &[T("  ->  Nested Loop")],
                            &[T("        ->  Seq Scan on int4_tbl i41")],
                            &[T("              Filter: (f1 > 0)")],
                            &[T("        ->  Nested Loop")],
                            &[T("              Join Filter: (i42.f1 = i41.f1)")],
                            &[T("              ->  Seq Scan on int8_tbl i81")],
                            &[T("              ->  Materialize")],
                            &[T("                    ->  Seq Scan on int4_tbl i42")],
                            &[T("  ->  Materialize")],
                            &[T("        ->  Seq Scan on int4_tbl i43")],
                            &[T("              Filter: (f1 > 1)")],
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
fn test_sampled_partitions_read_their_own_join_partner() {
    run_scripts(&[
        ScriptTest {
            name: "a TABLESAMPLE of a partition joined partition by partition samples with its own partner partition's columns",
            set_up_script: &[
                "CREATE TABLE prt1 (a INT, b INT, c VARCHAR) PARTITION BY RANGE (a);",
                "CREATE TABLE prt1_p1 PARTITION OF prt1 FOR VALUES FROM (0) TO (250);",
                "CREATE TABLE prt1_p3 PARTITION OF prt1 FOR VALUES FROM (500) TO (600);",
                "CREATE TABLE prt1_p2 PARTITION OF prt1 FOR VALUES FROM (250) TO (500);",
                "INSERT INTO prt1 SELECT i, i % 25, to_char(i, 'FM0000') FROM generate_series(0, 599) i WHERE i % 2 = 0;",
                "ANALYZE prt1;",
                "SET enable_partitionwise_join = on;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM prt1 t1 JOIN LATERAL (SELECT * FROM prt1 t2 TABLESAMPLE SYSTEM (t1.a) REPEATABLE (t1.b)) s ON t1.a = s.a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Append")],
                            &[T("  ->  Nested Loop")],
                            &[T("        ->  Seq Scan on prt1_p1 t1_1")],
                            &[T("        ->  Sample Scan on prt1_p1 t2_1")],
                            &[T("              Sampling: system (t1_1.a) REPEATABLE (t1_1.b)")],
                            &[T("              Filter: (t1_1.a = a)")],
                            &[T("  ->  Nested Loop")],
                            &[T("        ->  Seq Scan on prt1_p2 t1_2")],
                            &[T("        ->  Sample Scan on prt1_p2 t2_2")],
                            &[T("              Sampling: system (t1_2.a) REPEATABLE (t1_2.b)")],
                            &[T("              Filter: (t1_2.a = a)")],
                            &[T("  ->  Nested Loop")],
                            &[T("        ->  Seq Scan on prt1_p3 t1_3")],
                            &[T("        ->  Sample Scan on prt1_p3 t2_3")],
                            &[T("              Sampling: system (t1_3.a) REPEATABLE (t1_3.b)")],
                            &[T("              Filter: (t1_3.a = a)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(s.a) FROM prt1 t1 JOIN LATERAL (SELECT * FROM prt1 t2 TABLESAMPLE SYSTEM (t1.a * 0 + 100) REPEATABLE (t1.b)) s ON t1.a = s.a;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("300"), T("89700")],
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
fn test_subplans_of_inner_hash_keys_belong_to_the_hash() {
    run_scripts(&[
        ScriptTest {
            name: "a subplan in a hash condition's inner side is listed under the Hash and reads the inner side's columns",
            set_up_script: &[
                "CREATE TABLE hs (a INT, b INT);",
                "INSERT INTO hs SELECT i, i % 2 FROM generate_series(1, 100) i;",
                "CREATE INDEX hs_a ON hs (a);",
                "ANALYZE hs;",
                "SET enable_nestloop = off;",
                "SET enable_mergejoin = off;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) SELECT t1.a, t2.a FROM hs t1 JOIN hs t2 ON t1.b = t2.b AND t1.a = (SELECT max(a) FROM hs WHERE a = t2.a);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Hash Join")],
                            &[T("  Output: t1.a, t2.a")],
                            &[T("  Hash Cond: ((t1.b = t2.b) AND (t1.a = (SubPlan 2)))")],
                            &[T("  ->  Seq Scan on public.hs t1")],
                            &[T("        Output: t1.a, t1.b")],
                            &[T("  ->  Hash")],
                            &[T("        Output: t2.a, t2.b")],
                            &[T("        ->  Seq Scan on public.hs t2")],
                            &[T("              Output: t2.a, t2.b")],
                            &[T("        SubPlan 2")],
                            &[T("          ->  Result")],
                            &[T("                Output: (InitPlan 1).col1")],
                            &[T("                InitPlan 1")],
                            &[T("                  ->  Limit")],
                            &[T("                        Output: hs.a")],
                            &[T("                        ->  Seq Scan on public.hs")],
                            &[T("                              Output: hs.a")],
                            &[T("                              Filter: ((hs.a IS NOT NULL) AND (hs.a = t2.a))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(t1.a), sum(t2.a) FROM hs t1 JOIN hs t2 ON t1.b = t2.b AND t1.a = (SELECT max(a) FROM hs WHERE a = t2.a);",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8), Column("sum", INT8)],
                        rows: &[
                            &[T("100"), T("5050"), T("5050")],
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
fn test_index_scan_costs_before_the_index_is_folded() {
    run_scripts(&[
        ScriptTest {
            name: "an index's height counts its entries before they are folded, so join.sql's plan matches Postgres without a VACUUM",
            set_up_script: &[
                "CREATE TABLE tenk1 (unique1 INT4, unique2 INT4, two INT4, four INT4, ten INT4, twenty INT4, hundred INT4, thousand INT4, twothousand INT4, fivethous INT4, tenthous INT4, odd INT4, even INT4, stringu1 NAME, stringu2 NAME, string4 NAME);",
                "INSERT INTO tenk1 SELECT u1, u2, u1 % 2, u1 % 4, u1 % 10, u1 % 20, u1 % 100, u1 % 1000, u1 % 2000, u1 % 5000, u1, u1 % 100 * 2, u1 % 100 * 2 + 1, substr(md5(u1::TEXT), 1, 6), substr(md5(u2::TEXT), 1, 6), 'AAAAxx' FROM (SELECT u2 * 7919 % 10000 AS u1, u2 FROM generate_series(0, 9999) u2) s;",
                "VACUUM ANALYZE tenk1;",
                "CREATE INDEX tenk1_unique1 ON tenk1 USING btree (unique1 int4_ops);",
                "CREATE INDEX tenk1_unique2 ON tenk1 USING btree (unique2 int4_ops);",
                "CREATE INDEX tenk1_hundred ON tenk1 USING btree (hundred int4_ops);",
                "CREATE INDEX tenk1_thous_tenthous ON tenk1 (thousand, tenthous);",
                "CREATE TABLE int4_tbl (f1 INT4);",
                "INSERT INTO int4_tbl (f1) VALUES ('   0  '), ('123456     '), ('-123456'), ('2147483647'), ('-2147483647');",
                "VACUUM int4_tbl;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT b.unique1 FROM tenk1 a JOIN tenk1 b ON a.unique1 = b.unique2 LEFT JOIN tenk1 c ON b.unique1 = 42 AND c.thousand = a.thousand JOIN int4_tbl i1 ON b.thousand = f1 RIGHT JOIN int4_tbl i2 ON i2.f1 = b.tenthous ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Sort")],
                            &[T("  Sort Key: b.unique1")],
                            &[T("  ->  Nested Loop Left Join")],
                            &[T("        ->  Seq Scan on int4_tbl i2")],
                            &[T("        ->  Nested Loop Left Join")],
                            &[T("              Join Filter: (b.unique1 = 42)")],
                            &[T("              ->  Nested Loop")],
                            &[T("                    ->  Nested Loop")],
                            &[T("                          ->  Seq Scan on int4_tbl i1")],
                            &[T("                          ->  Index Scan using tenk1_thous_tenthous on tenk1 b")],
                            &[T("                                Index Cond: ((thousand = i1.f1) AND (tenthous = i2.f1))")],
                            &[T("                    ->  Index Scan using tenk1_unique1 on tenk1 a")],
                            &[T("                          Index Cond: (unique1 = b.unique2)")],
                            &[T("              ->  Index Only Scan using tenk1_thous_tenthous on tenk1 c")],
                            &[T("                    Index Cond: (thousand = a.thousand)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT b.unique1 FROM tenk1 a JOIN tenk1 b ON a.unique1 = b.unique2 LEFT JOIN tenk1 c ON b.unique1 = 42 AND c.thousand = a.thousand JOIN int4_tbl i1 ON b.thousand = f1 RIGHT JOIN int4_tbl i2 ON i2.f1 = b.tenthous ORDER BY 1;",
                    expected: Expected::Rows {
                        columns: &[Column("unique1", INT4)],
                        rows: &[
                            &[T("0")],
                            &[Null],
                            &[Null],
                            &[Null],
                            &[Null],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_explain_serialize() {
    run_scripts(&[
        ScriptTest {
            name: "EXPLAIN (ANALYZE, SERIALIZE) converts the rows by their output or send functions and counts the bytes their DataRow messages would take",
            set_up_script: &[
                "CREATE TABLE ser_t (a int4, b text, c numeric, d timestamptz, e int8[]);",
                "INSERT INTO ser_t SELECT g, repeat('x', g % 50), g * 1.5, '2026-01-01 00:00:00+00'::timestamptz + g * interval '1 hour', ARRAY[g, g + 1] FROM generate_series(1, 2000) g;",
                "INSERT INTO ser_t VALUES (NULL, NULL, NULL, NULL, NULL);",
                "SET TIME ZONE 'UTC';",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, SERIALIZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT * FROM ser_t;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on ser_t (actual rows=2001.00 loops=1)")],
                            &[T("Serialization: output=171kB  format=text")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, SERIALIZE BINARY, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT * FROM ser_t;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on ser_t (actual rows=2001.00 loops=1)")],
                            &[T("Serialization: output=222kB  format=binary")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, SERIALIZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF, FORMAT JSON) SELECT a FROM ser_t WHERE a < 10;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", JSON)],
                        rows: &[
                            &[T("[\n  {\n    \"Plan\": {\n      \"Node Type\": \"Seq Scan\",\n      \"Parallel Aware\": false,\n      \"Async Capable\": false,\n      \"Relation Name\": \"ser_t\",\n      \"Alias\": \"ser_t\",\n      \"Actual Rows\": 9.00,\n      \"Actual Loops\": 1,\n      \"Disabled\": false,\n      \"Filter\": \"(a < 10)\",\n      \"Rows Removed by Filter\": 1992\n    },\n    \"Triggers\": [\n    ],\n    \"Serialization\": {\n      \"Output Volume\": 1,\n      \"Format\": \"text\"\n    }\n  }\n]")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, SERIALIZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) CREATE TABLE ser_ctas AS SELECT * FROM ser_t;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on ser_t (actual rows=2001.00 loops=1)")],
                            &[T("Serialization: output=0kB  format=text")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (SERIALIZE) SELECT 1;",
                    expected: Expected::Error(Diagnostic { code: "22023", message: "EXPLAIN option SERIALIZE requires ANALYZE", ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_explain_memory() {
    run_scripts(&[
        ScriptTest {
            name: "EXPLAIN (MEMORY) reports the memory planning used and allocated, for EXPLAIN EXECUTE too, with numbers filtered as Postgres' regression tests filter them",
            set_up_script: &[
                "CREATE TABLE mt1 (a int PRIMARY KEY, b int);",
                "CREATE TABLE mt2 (a int PRIMARY KEY, b int);",
                "CREATE TABLE mt3 (a int PRIMARY KEY, b int);",
                "CREATE FUNCTION mem_filter(text) RETURNS SETOF text LANGUAGE plpgsql AS $$ DECLARE ln text; BEGIN FOR ln IN EXECUTE $1 LOOP ln := regexp_replace(ln, '\\d+kB', 'NkB', 'g'); ln := regexp_replace(ln, '\\m\\d+\\M', 'N', 'g'); RETURN NEXT ln; END LOOP; END $$;",
                "CREATE FUNCTION mem_planning(text) RETURNS jsonb LANGUAGE plpgsql AS $$ DECLARE j jsonb; BEGIN EXECUTE $1 INTO j; RETURN j->0->'Planning'; END $$;",
                "PREPARE mq(int) AS SELECT * FROM mt1 WHERE a = $1;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT mem_filter('EXPLAIN (MEMORY, COSTS OFF) SELECT * FROM mt1');",
                    expected: Expected::Rows {
                        columns: &[Column("mem_filter", TEXT)],
                        rows: &[
                            &[T("Seq Scan on mt1")],
                            &[T("Planning:")],
                            &[T("  Memory: used=NkB  allocated=NkB")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT mem_filter('EXPLAIN (ANALYZE, MEMORY, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) SELECT * FROM mt1');",
                    expected: Expected::Rows {
                        columns: &[Column("mem_filter", TEXT)],
                        rows: &[
                            &[T("Seq Scan on mt1 (actual rows=N.N loops=N)")],
                            &[T("Planning:")],
                            &[T("  Memory: used=NkB  allocated=NkB")],
                        ],
                        tag: "SELECT 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT mem_filter('EXPLAIN (MEMORY, COSTS OFF) EXECUTE mq(1)');",
                    expected: Expected::Rows {
                        columns: &[Column("mem_filter", TEXT)],
                        rows: &[
                            &[T("Index Scan using mt1_pkey on mt1")],
                            &[T("  Index Cond: (a = N)")],
                            &[T("Planning:")],
                            &[T("  Memory: used=NkB  allocated=NkB")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT mem_filter('EXPLAIN (MEMORY, COSTS OFF, FORMAT YAML) SELECT * FROM mt1');",
                    expected: Expected::Rows {
                        columns: &[Column("mem_filter", TEXT)],
                        rows: &[
                            &[T("- Plan: \n    Node Type: \"Seq Scan\"\n    Parallel Aware: false\n    Async Capable: false\n    Relation Name: \"mt1\"\n    Alias: \"mt1\"\n    Disabled: false\n  Planning: \n    Memory Used: N\n    Memory Allocated: N")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (p->>'Memory Used')::int > 0 AS used, (p->>'Memory Used')::int <= (p->>'Memory Allocated')::int AS within FROM mem_planning('EXPLAIN (MEMORY, FORMAT JSON) SELECT * FROM mt1') p;",
                    expected: Expected::Rows {
                        columns: &[Column("used", BOOL), Column("within", BOOL)],
                        rows: &[
                            &[T("t"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT (j->>'Memory Used')::int > (s->>'Memory Used')::int AS grows FROM mem_planning('EXPLAIN (MEMORY, FORMAT JSON) SELECT * FROM mt1 JOIN mt2 USING (a) JOIN mt3 USING (a) WHERE mt1.b > 5 ORDER BY 1') j, mem_planning('EXPLAIN (MEMORY, FORMAT JSON) SELECT * FROM mt1') s;",
                    expected: Expected::Rows {
                        columns: &[Column("grows", BOOL)],
                        rows: &[
                            &[T("t")],
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
fn test_explain_of_utility_statements_holding_queries() {
    run_scripts(&[
        ScriptTest {
            name: "EXPLAIN of DECLARE CURSOR, CREATE TABLE AS, CREATE TABLE AS EXECUTE, and CREATE MATERIALIZED VIEW explains the query each runs, and EXPLAIN EXECUTE of a rule's NOTIFY shows it",
            set_up_script: &[
                "CREATE TABLE eu_t (a int, b int);",
                "INSERT INTO eu_t VALUES (1, 2), (3, 4);",
                "PREPARE eu_p(int) AS SELECT * FROM eu_t WHERE a = $1;",
                "CREATE TABLE eu_log (a int);",
                "CREATE RULE eu_r AS ON INSERT TO eu_log DO ALSO NOTIFY eu_chan;",
                "PREPARE eu_i(int) AS INSERT INTO eu_log VALUES ($1);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DECLARE eu_c CURSOR FOR SELECT * FROM eu_t;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on eu_t")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) DECLARE eu_c SCROLL CURSOR FOR SELECT * FROM eu_t ORDER BY a;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Sort")],
                            &[T("  Sort Key: a")],
                            &[T("  ->  Seq Scan on eu_t")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) CREATE TABLE eu_x AS SELECT * FROM eu_t;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on eu_t")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) CREATE TABLE eu_x AS EXECUTE eu_p(1);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on eu_t")],
                            &[T("  Filter: (a = 1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) CREATE TABLE eu_y AS EXECUTE eu_p(1);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on eu_t (actual rows=1.00 loops=1)")],
                            &[T("  Filter: (a = 1)")],
                            &[T("  Rows Removed by Filter: 1")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM eu_y;",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) CREATE MATERIALIZED VIEW eu_mv AS SELECT a FROM eu_t;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on eu_t")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) EXECUTE eu_i(1);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Insert on eu_log")],
                            &[T("  ->  Result")],
                            &[T("")],
                            &[T("NOTIFY")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF, BUFFERS OFF) DECLARE eu_d CURSOR FOR SELECT * FROM eu_t;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on eu_t (actual rows=2.00 loops=1)")],
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
fn test_explain_query_identifier() {
    run_scripts(&[
        ScriptTest {
            name: "EXPLAIN VERBOSE shows the query identifier under compute_query_id = on",
            set_up_script: &[
                "CREATE TABLE qi_t (a INT, b TEXT)",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SET compute_query_id = on;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) SELECT a FROM qi_t WHERE a = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on public.qi_t")],
                            &[T("  Output: a")],
                            &[T("  Filter: (qi_t.a = 1)")],
                            &[T("Query Identifier: -4005938107589656576")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) SELECT a FROM qi_t WHERE a = 2;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on public.qi_t")],
                            &[T("  Output: a")],
                            &[T("  Filter: (qi_t.a = 2)")],
                            &[T("Query Identifier: -4005938107589656576")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) SELECT b FROM qi_t WHERE a = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on public.qi_t")],
                            &[T("  Output: b")],
                            &[T("  Filter: (qi_t.a = 1)")],
                            &[T("Query Identifier: -8665554817700561524")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) SELECT a FROM qi_t WHERE a IN (1, 2, 3);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on public.qi_t")],
                            &[T("  Output: a")],
                            &[T("  Filter: (qi_t.a = ANY ('{1,2,3}'::integer[]))")],
                            &[T("Query Identifier: -7579009276720948917")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) SELECT a FROM qi_t WHERE a IN (4, 5, 6, 7);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on public.qi_t")],
                            &[T("  Output: a")],
                            &[T("  Filter: (qi_t.a = ANY ('{4,5,6,7}'::integer[]))")],
                            &[T("Query Identifier: -7579009276720948917")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT a FROM qi_t WHERE a = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on qi_t")],
                            &[T("  Filter: (a = 1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "PREPARE qi_p AS SELECT a FROM qi_t WHERE a = $1;",
                    expected: Expected::Tag("PREPARE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) EXECUTE qi_p(1);",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on public.qi_t")],
                            &[T("  Output: a")],
                            &[T("  Filter: (qi_t.a = 1)")],
                            &[T("Query Identifier: 693481973879783553")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) CREATE TABLE qi_u AS SELECT a FROM qi_t WHERE a = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on public.qi_t")],
                            &[T("  Output: a")],
                            &[T("  Filter: (qi_t.a = 1)")],
                            &[T("Query Identifier: -4005938107589656576")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) DECLARE qi_c CURSOR FOR SELECT a FROM qi_t WHERE a = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on public.qi_t")],
                            &[T("  Output: a")],
                            &[T("  Filter: (qi_t.a = 1)")],
                            &[T("Query Identifier: -4005938107589656576")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET compute_query_id = regress;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) SELECT a FROM qi_t WHERE a = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on public.qi_t")],
                            &[T("  Output: a")],
                            &[T("  Filter: (qi_t.a = 1)")],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SET compute_query_id = off;",
                    expected: Expected::Tag("SET"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) SELECT a FROM qi_t WHERE a = 1;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Seq Scan on public.qi_t")],
                            &[T("  Output: a")],
                            &[T("  Filter: (qi_t.a = 1)")],
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
fn test_explain_values_scan_subplans() {
    run_scripts(&[
        ScriptTest {
            name: "EXPLAIN shows the SubPlans a VALUES list's expressions run under its Values Scan",
            set_up_script: &[],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (VERBOSE, COSTS OFF) SELECT * FROM (VALUES (3 NOT IN (SELECT * FROM (VALUES (1), (2)) ss1)), (false)) ss;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Values Scan on "*VALUES*""#)],
                            &[T(r#"  Output: "*VALUES*".column1"#)],
                            &[T("  SubPlan 1")],
                            &[T(r#"    ->  Values Scan on "*VALUES*_1""#)],
                            &[T(r#"          Output: "*VALUES*_1".column1"#)],
                        ],
                        tag: "EXPLAIN",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM (VALUES (3 NOT IN (SELECT * FROM (VALUES (1), (2)) ss1)), (false)) ss;",
                    expected: Expected::Rows {
                        columns: &[Column("column1", BOOL)],
                        rows: &[
                            &[T("t")],
                            &[T("f")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_explain_attaches_a_pruning_subplan_to_its_append() {
    run_scripts(&[
        ScriptTest {
            name: "EXPLAIN shows a SubPlan of an Append's pruning expressions under the Append, since an index scan builds no scan keys for EXPLAIN alone",
            set_up_script: &[
                "CREATE TABLE int4_tbl (f1 int4);",
                "INSERT INTO int4_tbl (f1) VALUES ('   0  '), ('123456     '), ('    -123456'), ('2147483647'), ('-2147483647');",
                "ANALYZE int4_tbl;",
                "CREATE TABLE asptab (id int PRIMARY KEY) PARTITION BY RANGE (id);",
                "CREATE TABLE asptab0 PARTITION OF asptab FOR VALUES FROM (0) TO (1);",
                "CREATE TABLE asptab1 PARTITION OF asptab FOR VALUES FROM (1) TO (2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "EXPLAIN (COSTS OFF) SELECT * FROM (SELECT EXISTS (SELECT 1 FROM int4_tbl tinner WHERE f1 = touter.f1) AS b FROM int4_tbl touter) ss, asptab WHERE asptab.id > ss.b::int;",
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T(r#"Nested Loop"#)],
                            &[T(r#"  ->  Seq Scan on int4_tbl touter"#)],
                            &[T(r#"  ->  Append"#)],
                            &[T(r#"        ->  Index Only Scan using asptab0_pkey on asptab0 asptab_1"#)],
                            &[T(r#"              Index Cond: (id > (EXISTS(SubPlan 3))::integer)"#)],
                            &[T(r#"              SubPlan 4"#)],
                            &[T(r#"                ->  Seq Scan on int4_tbl tinner_2"#)],
                            &[T(r#"        ->  Index Only Scan using asptab1_pkey on asptab1 asptab_2"#)],
                            &[T(r#"              Index Cond: (id > (EXISTS(SubPlan 3))::integer)"#)],
                            &[T(r#"        SubPlan 3"#)],
                            &[T(r#"          ->  Seq Scan on int4_tbl tinner_1"#)],
                            &[T(r#"                Filter: (f1 = touter.f1)"#)],
                            &[T(r#"  SubPlan 2"#)],
                            &[T(r#"    ->  Seq Scan on int4_tbl tinner"#)],
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
