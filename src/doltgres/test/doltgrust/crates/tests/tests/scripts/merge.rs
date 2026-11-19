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
fn test_merge() {
    run_scripts(&[
        ScriptTest {
            name: "simple merge",
            set_up_script: &[
                "CREATE TABLE t1 (a INT, b INT, c INT, PRIMARY KEY (a))",
                "CREATE TABLE t2 (a INT, b INT, c INT, PRIMARY KEY (a))",
                "INSERT INTO t1 VALUES (1, 2, 3), (4, 5, 6), (7, 8, 9)",
                "INSERT INTO t2 VALUES (1, 2, 3), (4, 5, 6), (7, 8, 9)",
                "SELECT DOLT_COMMIT('-Am', 'intial commit')",
                "SELECT DOLT_BRANCH('branch1')",
                "SELECT DOLT_BRANCH('branch2')",
                "SELECT DOLT_CHECKOUT('branch1')",
                "INSERT INTO t1 VALUES (10, 11, 12)",
                "INSERT INTO t2 VALUES (10, 11, 12)",
                "SELECT DOLT_COMMIT('-Am', 'added 10')",
                "SELECT DOLT_CHECKOUT('branch2')",
                "INSERT INTO t1 VALUES (20, 21, 22)",
                "INSERT INTO t2 VALUES (20, 21, 22)",
                "SELECT DOLT_COMMIT('-Am', 'added 20')",
                "SELECT DOLT_CHECKOUT('main')",
                "SELECT DOLT_MERGE('branch1')",
                "SELECT DOLT_MERGE('branch2')",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM t1",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("2"), T("3")],
                            &[T("4"), T("5"), T("6")],
                            &[T("7"), T("8"), T("9")],
                            &[T("10"), T("11"), T("12")],
                            &[T("20"), T("21"), T("22")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM t2",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4), Column("c", INT4)],
                        rows: &[
                            &[T("1"), T("2"), T("3")],
                            &[T("4"), T("5"), T("6")],
                            &[T("7"), T("8"), T("9")],
                            &[T("10"), T("11"), T("12")],
                            &[T("20"), T("21"), T("22")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge independent JSON document changes",
            set_up_script: &[
                "CREATE TABLE documents_json (id INT PRIMARY KEY, doc JSON)",
                "CREATE TABLE documents_jsonb (id INT PRIMARY KEY, doc JSONB)",
                r#"INSERT INTO documents_json VALUES (1, '{"a": 1, "b": 2}')"#,
                r#"INSERT INTO documents_jsonb VALUES (1, '{"a": 1, "b": 2}')"#,
                "SELECT DOLT_COMMIT('-Am', 'base')",
                "SELECT DOLT_BRANCH('left')",
                "SELECT DOLT_BRANCH('right')",
                "SELECT DOLT_CHECKOUT('left')",
                r#"UPDATE documents_json SET doc = '{"a": 100, "b": 2}' WHERE id = 1"#,
                r#"UPDATE documents_jsonb SET doc = doc || '{"a": 100}' WHERE id = 1"#,
                "SELECT DOLT_COMMIT('-am', 'left')",
                "SELECT DOLT_CHECKOUT('right')",
                r#"UPDATE documents_json SET doc = '{"a": 1, "b": 200}' WHERE id = 1"#,
                r#"UPDATE documents_jsonb SET doc = doc || '{"b": 200}' WHERE id = 1"#,
                "SELECT DOLT_COMMIT('-am', 'right')",
                "SELECT DOLT_CHECKOUT('left')",
                "SELECT DOLT_MERGE('right')",
            ],
            assertions: &[
                // Doltgrust-specific: a merged json document is written as jsonb writes it, Appendix C.
                ScriptTestAssertion {
                    query: "SELECT doc::TEXT FROM documents_json",
                    expected: Expected::Rows {
                        columns: &[Column("doc", TEXT)],
                        rows: &[
                            &[T(r#"{"a": 100, "b": 200}"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT doc::TEXT FROM documents_jsonb",
                    expected: Expected::Rows {
                        columns: &[Column("doc", TEXT)],
                        rows: &[
                            &[T(r#"{"a": 100, "b": 200}"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT COUNT(*) FROM dolt_conflicts",
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
            name: "merge with check expressions and column defaults",
            set_up_script: &[
                "SET timezone TO 'UTC';",
                "CREATE TABLE t1 (a INT, b timestamptz default '2020-01-01 00:00:00'::timestamptz, PRIMARY KEY (a))",
                "ALTER TABLE t1 ADD CONSTRAINT check_b CHECK (b >= '2020-01-01 00:00:00'::timestamptz)",
                "INSERT INTO t1 VALUES (1, '2020-01-02 00:00:00'), (2, '2020-01-03 00:00:00')",
                "SELECT DOLT_COMMIT('-Am', 'intial commit')",
                "SELECT DOLT_BRANCH('branch1')",
                "SELECT DOLT_BRANCH('branch2')",
                "SELECT DOLT_CHECKOUT('branch1')",
                "INSERT INTO t1 VALUES (3, '2020-01-04 00:00:00')",
                "SELECT DOLT_COMMIT('-Am', 'added 3')",
                "SELECT DOLT_CHECKOUT('branch2')",
                "INSERT INTO t1 VALUES (4, '2020-01-05 00:00:00')",
                "SELECT DOLT_COMMIT('-Am', 'added 4')",
                "SELECT DOLT_CHECKOUT('main')",
                "SELECT DOLT_MERGE('branch1')",
                "SELECT DOLT_MERGE('branch2')",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 order by a",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", TIMESTAMPTZ)],
                        rows: &[
                            &[T("1"), T("2020-01-02 00:00:00+00")],
                            &[T("2"), T("2020-01-03 00:00:00+00")],
                            &[T("3"), T("2020-01-04 00:00:00+00")],
                            &[T("4"), T("2020-01-05 00:00:00+00")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                // Postgres reports the violated constraint as it does outside a merge.
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (5, '2019-12-31 00:00:00')",
                    expected: Expected::Error(Diagnostic { code: "23514", message: r#"new row for relation "t1" violates check constraint "check_b""#, detail: "Failing row contains (5, 2019-12-31 00:00:00+00).", schema: "public", table: "t1", constraint: "check_b", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge with unique constraints and foreign keys",
            set_up_script: &[
                "CREATE TABLE t1 (a INT, b INT, PRIMARY KEY (a), unique (b))",
                "CREATE TABLE t2 (a INT, b INT, PRIMARY KEY (a), foreign key (b) references t1(b))",
                "INSERT INTO t1 VALUES (1, 2), (4, 5), (7, 8)",
                "INSERT INTO t2 VALUES (1, 2), (4, 5), (7, 8)",
                "SELECT DOLT_COMMIT('-Am', 'intial commit')",
                "SELECT DOLT_BRANCH('branch1')",
                "SELECT DOLT_BRANCH('branch2')",
                "SELECT DOLT_CHECKOUT('branch1')",
                "INSERT INTO t1 VALUES (10, 11)",
                "INSERT INTO t2 VALUES (10, 11)",
                "SELECT DOLT_COMMIT('-Am', 'added 10')",
                "SELECT DOLT_CHECKOUT('branch2')",
                "INSERT INTO t1 VALUES (20, 21)",
                "INSERT INTO t2 VALUES (20, 21)",
                "SELECT DOLT_COMMIT('-Am', 'added 20')",
                "SELECT DOLT_CHECKOUT('main')",
                "SELECT DOLT_MERGE('branch1')",
                "SELECT DOLT_MERGE('branch2')",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM t1 order by a",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", INT4)],
                        rows: &[
                            &[T("1"), T("2")],
                            &[T("4"), T("5")],
                            &[T("7"), T("8")],
                            &[T("10"), T("11")],
                            &[T("20"), T("21")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                // Postgres reports the violated constraint as it does outside a merge.
                ScriptTestAssertion {
                    query: "INSERT INTO t1 VALUES (100, 2)",
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "t1_b_key""#, detail: "Key (b)=(2) already exists.", schema: "public", table: "t1", constraint: "t1_b_key", ..E }),
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "INSERT INTO t2 VALUES (100, 200)",
                    expected: Expected::Error(Diagnostic { code: "23503", message: r#"insert or update on table "t2" violates foreign key constraint "t2_b_fkey""#, detail: r#"Key (b)=(200) is not present in table "t1"."#, schema: "public", table: "t2", constraint: "t2_b_fkey", ..E }),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge a branch that created a type",
            set_up_script: &[
                "CREATE TABLE t1 (a INT PRIMARY KEY)",
                "SELECT DOLT_COMMIT('-Am', 'initial commit')",
                "SELECT DOLT_CHECKOUT('-b', 'branch1')",
                "CREATE TYPE type1 AS ENUM ('a', 'b')",
                "CREATE TABLE t2 (a INT PRIMARY KEY, b type1)",
                "INSERT INTO t2 VALUES (1, 'a')",
                "SELECT DOLT_COMMIT('-Am', 'added type1')",
                "SELECT DOLT_CHECKOUT('main')",
                "INSERT INTO t1 VALUES (1)",
                "SELECT DOLT_COMMIT('-Am', 'added 1')",
                "SELECT DOLT_MERGE('branch1')",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM t2",
                    expected: Expected::Rows {
                        columns: &[Column("a", INT4), Column("b", USER_DEFINED)],
                        rows: &[
                            &[T("1"), T("a")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT 'b'::type1",
                    expected: Expected::Rows {
                        columns: &[Column("type1", USER_DEFINED)],
                        rows: &[
                            &[T("b")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "merge columns that each side added",
            set_up_script: &[
                "CREATE TABLE a (pk INT PRIMARY KEY, x INT)",
                "INSERT INTO a VALUES (1, 1)",
                "SELECT DOLT_COMMIT('-Am', 'a')",
                "SELECT DOLT_BRANCH('r2')",
                "ALTER TABLE a ADD COLUMN l TEXT",
                "INSERT INTO a VALUES (2, 2, 'left')",
                "SELECT DOLT_COMMIT('-am', 'left a')",
                "SELECT DOLT_CHECKOUT('r2')",
                "ALTER TABLE a ADD COLUMN r INT",
                "UPDATE a SET x = 10, r = 5 WHERE pk = 1",
                "SELECT DOLT_COMMIT('-am', 'right a')",
                "SELECT DOLT_CHECKOUT('main')",
                "SELECT DOLT_MERGE('r2')",
            ],
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "SELECT * FROM a ORDER BY pk",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4), Column("x", INT4), Column("l", TEXT), Column("r", INT4)],
                        rows: &[
                            &[T("1"), T("10"), Null, T("5")],
                            &[T("2"), T("2"), T("left"), Null],
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
fn test_merge_large_tables() {
    run_scripts(&[
        ScriptTest {
            name: "merging tables of several tree levels",
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "CREATE TABLE mt (id INT PRIMARY KEY, v INT, t TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mt SELECT i, i, 'row ' || i FROM generate_series(1, 20000) i;",
                    expected: Expected::Tag("INSERT 0 20000"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DOLT_COMMIT('-Am', 'base');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DOLT_BRANCH('other');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE mt SET v = v + 1 WHERE id % 1000 = 1;",
                    expected: Expected::Tag("UPDATE 20"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM mt WHERE id BETWEEN 5000 AND 5010;",
                    expected: Expected::Tag("DELETE 11"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mt SELECT i, i, 'main ' || i FROM generate_series(30001, 30005) i;",
                    expected: Expected::Tag("INSERT 0 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DOLT_COMMIT('-Am', 'main changes');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DOLT_CHECKOUT('other');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T(r#"(0,"Switched to branch 'other'")"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE mt SET v = -id WHERE id % 1500 = 2;",
                    expected: Expected::Tag("UPDATE 14"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DELETE FROM mt WHERE id BETWEEN 15002 AND 15006;",
                    expected: Expected::Tag("DELETE 5"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mt SELECT i, i, 'other ' || i FROM generate_series(40001, 40003) i;",
                    expected: Expected::Tag("INSERT 0 3"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DOLT_COMMIT('-Am', 'other changes');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DOLT_CHECKOUT('main');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T(r#"(0,"Switched to branch 'main'")"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DOLT_MERGE('other');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_merge", RECORD)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*), sum(v), count(DISTINCT t) FROM mt;",
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8), Column("sum", INT8), Column("count", INT8)],
                        rows: &[
                            &[T("19992"), T("199906913"), T("19992")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v, t FROM mt WHERE id % 1000 = 1 AND id < 6000 OR id % 1500 = 2 AND id < 7000 OR id > 30000 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4), Column("t", TEXT)],
                        rows: &[
                            &[T("1"), T("2"), T("row 1")],
                            &[T("2"), T("-2"), T("row 2")],
                            &[T("1001"), T("1002"), T("row 1001")],
                            &[T("1502"), T("-1502"), T("row 1502")],
                            &[T("2001"), T("2002"), T("row 2001")],
                            &[T("3001"), T("3002"), T("row 3001")],
                            &[T("3002"), T("-3002"), T("row 3002")],
                            &[T("4001"), T("4002"), T("row 4001")],
                            &[T("4502"), T("-4502"), T("row 4502")],
                            &[T("6002"), T("-6002"), T("row 6002")],
                            &[T("30001"), T("30001"), T("main 30001")],
                            &[T("30002"), T("30002"), T("main 30002")],
                            &[T("30003"), T("30003"), T("main 30003")],
                            &[T("30004"), T("30004"), T("main 30004")],
                            &[T("30005"), T("30005"), T("main 30005")],
                            &[T("40001"), T("40001"), T("other 40001")],
                            &[T("40002"), T("40002"), T("other 40002")],
                            &[T("40003"), T("40003"), T("other 40003")],
                        ],
                        tag: "SELECT 18",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) FROM mt WHERE id BETWEEN 5000 AND 5010 OR id BETWEEN 15002 AND 15006;",
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
            name: "merge conflicts in tables of several tree levels",
            assertions: &[
                // Doltgrust-specific: Postgres cannot run this, so the Go server's output is expected.
                ScriptTestAssertion {
                    query: "CREATE TABLE mc (id INT PRIMARY KEY, v INT, t TEXT);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO mc SELECT i, i, 'row ' || i FROM generate_series(1, 20000) i;",
                    expected: Expected::Tag("INSERT 0 20000"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DOLT_COMMIT('-Am', 'base');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DOLT_BRANCH('other');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_branch", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE mc SET t = 'main' WHERE id IN (7777, 12345);",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE mc SET v = 0 WHERE id = 100;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DOLT_COMMIT('-Am', 'main changes');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DOLT_CHECKOUT('other');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T(r#"(0,"Switched to branch 'other'")"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE mc SET t = 'other' WHERE id IN (7777, 19999);",
                    expected: Expected::Tag("UPDATE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "UPDATE mc SET v = 1 WHERE id = 200;",
                    expected: Expected::Tag("UPDATE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DOLT_COMMIT('-Am', 'other changes');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_commit", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT DOLT_CHECKOUT('main');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_checkout", RECORD)],
                        rows: &[
                            &[T(r#"(0,"Switched to branch 'main'")"#)],
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
                    query: "SELECT DOLT_MERGE('other');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_merge", RECORD)],
                        rows: &[
                            &[T(r#"("",0,1,"conflicts found")"#)],
                        ],
                        tag: "SELECT 1",
                    },
                    notices: &[Diagnostic { severity: "WARNING", code: "01000", message: "merge has unresolved conflicts or constraint violations", ..N }],
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT "table", num_conflicts FROM dolt_conflicts;"#,
                    expected: Expected::Rows {
                        columns: &[Column("table", TEXT), Column("num_conflicts", NUMERIC)],
                        rows: &[
                            &[T("mc"), T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT our_id, our_t, their_t, base_t FROM dolt_conflicts_mc;",
                    expected: Expected::Rows {
                        columns: &[Column("our_id", INT4), Column("our_t", TEXT), Column("their_t", TEXT), Column("base_t", TEXT)],
                        rows: &[
                            &[T("7777"), T("main"), T("other"), T("row 7777")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT id, v, t FROM mc WHERE id IN (100, 200, 7777, 12345, 19999) ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4), Column("t", TEXT)],
                        rows: &[
                            &[T("100"), T("0"), T("row 100")],
                            &[T("200"), T("1"), T("row 200")],
                            &[T("7777"), T("7777"), T("main")],
                            &[T("12345"), T("12345"), T("main")],
                            &[T("19999"), T("19999"), T("other")],
                        ],
                        tag: "SELECT 5",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_merge_joins() {
    run_scripts(&[
        ScriptTest {
            name: "MERGE rows found by joining the target to the source",
            set_up_script: &[
                "CREATE TABLE m1 (id INT PRIMARY KEY, v INT);",
                "CREATE TABLE m2 (id INT PRIMARY KEY, w INT);",
                "INSERT INTO m1 SELECT g, g FROM generate_series(1, 20) g;",
                "INSERT INTO m2 SELECT g, g * 10 FROM generate_series(15, 25) g;",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "MERGE INTO m1 USING m2 ON m1.id = m2.id WHEN MATCHED AND m2.w > 180 THEN DELETE WHEN MATCHED THEN UPDATE SET v = m2.w WHEN NOT MATCHED THEN INSERT VALUES (m2.id, m2.w + 1);",
                    expected: Expected::Tag("MERGE 11"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM m1 WHERE id > 12 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("13"), T("13")],
                            &[T("14"), T("14")],
                            &[T("15"), T("150")],
                            &[T("16"), T("160")],
                            &[T("17"), T("170")],
                            &[T("18"), T("180")],
                            &[T("21"), T("211")],
                            &[T("22"), T("221")],
                            &[T("23"), T("231")],
                            &[T("24"), T("241")],
                            &[T("25"), T("251")],
                        ],
                        tag: "SELECT 11",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO m1 USING (SELECT id FROM m2 WHERE id < 17) s ON m1.id = s.id WHEN MATCHED THEN UPDATE SET v = 0;",
                    expected: Expected::Tag("MERGE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM m1 WHERE v = 0 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("15"), T("0")],
                            &[T("16"), T("0")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO m1 USING (VALUES (1, 5), (1, 6)) s(a, b) ON m1.id = s.a WHEN MATCHED THEN UPDATE SET v = s.b;",
                    expected: Expected::Error(Diagnostic { code: "21000", message: "MERGE command cannot affect row a second time", hint: "Ensure that not more than one source row matches any one target row.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO m1 t USING m2 s ON t.id = s.id + 100 WHEN NOT MATCHED AND s.id = 25 THEN INSERT VALUES (s.id + 100, 7) WHEN MATCHED THEN DO NOTHING;",
                    expected: Expected::Tag("MERGE 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM m1 WHERE id > 100 ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("125"), T("7")],
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
fn test_merge_returning() {
    run_scripts(&[
        ScriptTest {
            name: "MERGE RETURNING with MERGE_ACTION() and the rows before and after the change",
            set_up_script: &[
                "CREATE TABLE mt (id INT PRIMARY KEY, v INT);",
                "CREATE TABLE ms2 (id INT PRIMARY KEY, w INT);",
                "INSERT INTO mt VALUES (1, 10), (2, 20), (3, 30);",
                "INSERT INTO ms2 VALUES (2, 200), (3, 300), (4, 400);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "MERGE INTO mt USING ms2 ON mt.id = ms2.id WHEN MATCHED AND ms2.w = 300 THEN DELETE WHEN MATCHED THEN UPDATE SET v = ms2.w WHEN NOT MATCHED THEN INSERT VALUES (ms2.id, ms2.w) RETURNING merge_action(), mt.*, ms2.w, old.v, new.v, (SELECT merge_action() || '!');",
                    expected: Expected::Rows {
                        columns: &[Column("merge_action", TEXT), Column("id", INT4), Column("v", INT4), Column("w", INT4), Column("v", INT4), Column("v", INT4), Column("?column?", TEXT)],
                        rows: &[
                            &[T("UPDATE"), T("2"), T("200"), T("200"), T("20"), T("200"), T("UPDATE!")],
                            &[T("DELETE"), T("3"), T("30"), T("300"), T("30"), Null, T("DELETE!")],
                            &[T("INSERT"), T("4"), T("400"), T("400"), Null, T("400"), T("INSERT!")],
                        ],
                        tag: "MERGE 3",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO mt t USING ms2 s ON t.id = s.id WHEN MATCHED THEN DO NOTHING RETURNING *;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("w", INT4), Column("id", INT4), Column("v", INT4)],
                        rows: &[],
                        tag: "MERGE 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT merge_action();",
                    expected: Expected::Error(Diagnostic { code: "42601", message: "MERGE_ACTION() can only be used in the RETURNING list of a MERGE command", position: 8, ..E }),
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_merge_changed_rows() {
    run_scripts(&[
        ScriptTest {
            name: "MERGE fails on a target row it already changed",
            set_up_script: &[
                "CREATE TABLE mt (id INT PRIMARY KEY, v INT);",
                "CREATE TABLE ms (id INT, w INT);",
                "INSERT INTO mt VALUES (1, 10), (2, 20);",
                "INSERT INTO ms VALUES (1, 1), (1, 2);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "MERGE INTO mt USING ms ON mt.id = ms.id WHEN MATCHED THEN UPDATE SET v = ms.w;",
                    expected: Expected::Error(Diagnostic { code: "21000", message: "MERGE command cannot affect row a second time", hint: "Ensure that not more than one source row matches any one target row.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO mt USING ms ON mt.id = ms.id WHEN MATCHED THEN DELETE;",
                    expected: Expected::Error(Diagnostic { code: "21000", message: "MERGE command cannot affect row a second time", hint: "Ensure that not more than one source row matches any one target row.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION mt_touch() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN IF pg_trigger_depth() = 1 THEN UPDATE mt SET v = v + 100 WHERE id = OLD.id; END IF; RETURN NEW; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER mt_touch BEFORE UPDATE ON mt FOR EACH ROW EXECUTE FUNCTION mt_touch();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO mt USING (VALUES (2, 5)) AS s(id, w) ON mt.id = s.id WHEN MATCHED THEN UPDATE SET v = s.w;",
                    expected: Expected::Error(Diagnostic { code: "27000", message: "tuple to be updated or deleted was already modified by an operation triggered by the current command", hint: "Consider using an AFTER trigger instead of a BEFORE trigger to propagate changes to other rows.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "DROP TRIGGER mt_touch ON mt;",
                    expected: Expected::Tag("DROP TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE FUNCTION mt_keep() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN IF TG_OP = 'DELETE' THEN RETURN OLD; END IF; RETURN NEW; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TRIGGER mt_keep BEFORE UPDATE OR DELETE ON mt FOR EACH ROW EXECUTE FUNCTION mt_keep();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO mt USING ms ON mt.id = ms.id WHEN MATCHED THEN UPDATE SET v = ms.w;",
                    expected: Expected::Error(Diagnostic { code: "21000", message: "MERGE command cannot affect row a second time", hint: "Ensure that not more than one source row matches any one target row.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO mt USING ms ON mt.id = ms.id WHEN MATCHED AND ms.w = 1 THEN UPDATE SET v = 0 WHEN MATCHED THEN DELETE;",
                    expected: Expected::Error(Diagnostic { code: "21000", message: "MERGE command cannot affect row a second time", hint: "Ensure that not more than one source row matches any one target row.", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM mt ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("10")],
                            &[T("2"), T("20")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "READ COMMITTED MERGE acts on the newest version of a target row another transaction changed",
            set_up_script: &[
                "CREATE TABLE ct (id INT PRIMARY KEY, v INT);",
                "CREATE TABLE cs (id INT PRIMARY KEY, w INT);",
                "INSERT INTO ct VALUES (1, 0), (2, 0), (3, 0), (4, 0);",
                "INSERT INTO cs VALUES (1, 10), (2, 20), (3, 30), (5, 50);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE ct SET v = v + 1 WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ MERGE INTO ct USING cs ON ct.id = cs.id WHEN MATCHED THEN UPDATE SET v = ct.v + cs.w;",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT * FROM ct ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("11")],
                            &[T("2"), T("20")],
                            &[T("3"), T("30")],
                            &[T("4"), T("0")],
                        ],
                        tag: "SELECT 4",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE ct SET v = 100 WHERE id = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ MERGE INTO ct USING (SELECT * FROM cs WHERE id = 2) s ON ct.id = s.id AND ct.v < 100 WHEN MATCHED THEN UPDATE SET v = -1 WHEN NOT MATCHED THEN INSERT VALUES (s.id + 100, s.w);",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT * FROM ct ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("11")],
                            &[T("2"), T("100")],
                            &[T("3"), T("30")],
                            &[T("4"), T("0")],
                            &[T("102"), T("20")],
                        ],
                        tag: "SELECT 5",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ DELETE FROM ct WHERE id = 3;",
                    expected: Expected::Tag("DELETE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ MERGE INTO ct USING (SELECT * FROM cs WHERE id = 3) s ON ct.id = s.id WHEN MATCHED THEN UPDATE SET v = s.w WHEN NOT MATCHED THEN INSERT VALUES (s.id, s.w + 1);",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT * FROM ct ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("11")],
                            &[T("2"), T("100")],
                            &[T("3"), T("31")],
                            &[T("4"), T("0")],
                            &[T("102"), T("20")],
                        ],
                        tag: "SELECT 5",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE ct SET v = 7 WHERE id = 4;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ MERGE INTO ct USING (SELECT * FROM cs WHERE id = 1) s ON ct.id = s.id WHEN NOT MATCHED BY SOURCE AND ct.id = 4 THEN UPDATE SET v = ct.v * 10;",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT * FROM ct ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("11")],
                            &[T("2"), T("100")],
                            &[T("3"), T("31")],
                            &[T("4"), T("70")],
                            &[T("102"), T("20")],
                        ],
                        tag: "SELECT 5",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE ct SET v = 500 WHERE id = 1;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ MERGE INTO ct USING (SELECT * FROM cs WHERE id = 1) s ON ct.id = s.id AND ct.v < 100 WHEN MATCHED THEN UPDATE SET v = 0 WHEN NOT MATCHED BY SOURCE AND ct.id = 1 THEN UPDATE SET v = ct.v + 1 WHEN NOT MATCHED THEN INSERT VALUES (s.id + 200, s.w) RETURNING merge_action(), ct.*;",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT * FROM ct ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("501")],
                            &[T("2"), T("100")],
                            &[T("3"), T("31")],
                            &[T("4"), T("70")],
                            &[T("102"), T("20")],
                            &[T("201"), T("10")],
                        ],
                        tag: "SELECT 6",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE ct SET v = 9 WHERE id = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ BEGIN ISOLATION LEVEL REPEATABLE READ;",
                    expected: Expected::Tag("BEGIN"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT 1;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ MERGE INTO ct USING (SELECT * FROM cs WHERE id = 2) s ON ct.id = s.id WHEN MATCHED THEN UPDATE SET v = s.w;",
                    expected: Expected::Error(Diagnostic { code: "40001", message: "could not serialize access due to concurrent update", ..E }),
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ ROLLBACK;",
                    expected: Expected::Tag("ROLLBACK"),
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT * FROM ct ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("501")],
                            &[T("2"), T("9")],
                            &[T("3"), T("31")],
                            &[T("4"), T("70")],
                            &[T("102"), T("20")],
                            &[T("201"), T("10")],
                        ],
                        tag: "SELECT 6",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ CREATE FUNCTION ct_keep() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RETURN NEW; END $$;",
                    expected: Expected::Tag("CREATE FUNCTION"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ CREATE TRIGGER ct_keep BEFORE UPDATE ON ct FOR EACH ROW EXECUTE FUNCTION ct_keep();",
                    expected: Expected::Tag("CREATE TRIGGER"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE ct SET v = 1000 WHERE id = 2;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ MERGE INTO ct USING (SELECT * FROM cs WHERE id = 2) s ON ct.id = s.id AND ct.v < 1000 WHEN MATCHED THEN UPDATE SET v = -2 WHEN NOT MATCHED THEN INSERT VALUES (s.id + 300, s.w);",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT * FROM ct ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("501")],
                            &[T("2"), T("1000")],
                            &[T("3"), T("31")],
                            &[T("4"), T("70")],
                            &[T("102"), T("20")],
                            &[T("201"), T("10")],
                            &[T("302"), T("20")],
                        ],
                        tag: "SELECT 7",
                    },
                    client: "B",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ BEGIN;",
                    expected: Expected::Tag("BEGIN"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ UPDATE ct SET v = v + 1 WHERE id = 4;",
                    expected: Expected::Tag("UPDATE 1"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ MERGE INTO ct USING (SELECT 4 AS id) s ON ct.id = s.id WHEN MATCHED THEN UPDATE SET v = ct.v * 2;",
                    flow: Flow::Exec,
                    client: "B",
                    expected_blocking: true,
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client A */ COMMIT;",
                    expected: Expected::Tag("COMMIT"),
                    client: "A",
                    ..A
                },
                ScriptTestAssertion {
                    query: "/* client B */ SELECT * FROM ct ORDER BY id;",
                    expected: Expected::Rows {
                        columns: &[Column("id", INT4), Column("v", INT4)],
                        rows: &[
                            &[T("1"), T("501")],
                            &[T("2"), T("1000")],
                            &[T("3"), T("31")],
                            &[T("4"), T("142")],
                            &[T("102"), T("20")],
                            &[T("201"), T("10")],
                            &[T("302"), T("20")],
                        ],
                        tag: "SELECT 7",
                    },
                    client: "B",
                    ..A
                },
            ],
            ..S
        },
    ]);
}

#[test]
fn test_merge_into_partitions_with_other_column_orders() {
    run_scripts(&[
        ScriptTest {
            name: "MERGE into partitions whose columns are in other orders, moving and routing rows",
            set_up_script: &[
                "CREATE TABLE mp (k int4, b text, a int4) PARTITION BY RANGE (k);",
                "CREATE TABLE mp1 (a int4, b text, k int4);",
                "ALTER TABLE mp ATTACH PARTITION mp1 FOR VALUES FROM (0) TO (10);",
                "CREATE TABLE mp2 (b text, extra int4, k int4, a int4);",
                "ALTER TABLE mp2 DROP COLUMN extra;",
                "ALTER TABLE mp ATTACH PARTITION mp2 FOR VALUES FROM (10) TO (20);",
                "INSERT INTO mp VALUES (1, 'one', 100), (11, 'eleven', 1100);",
                "CREATE TABLE ms (k int4, b text, a int4);",
                "INSERT INTO ms VALUES (1, 'uno', 1), (2, 'dos', 2), (11, 'once', 11), (12, 'doce', 12);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "MERGE INTO mp t USING ms s ON t.k = s.k WHEN MATCHED AND t.k = 1 THEN UPDATE SET k = t.k + 14, b = s.b WHEN MATCHED THEN UPDATE SET b = s.b || '!', a = t.a + s.a WHEN NOT MATCHED THEN INSERT (a, k, b) VALUES (s.a * 10, s.k, s.b) RETURNING merge_action(), t.tableoid::regclass, t.*;",
                    expected: Expected::Rows {
                        columns: &[Column("merge_action", TEXT), Column("tableoid", REGCLASS), Column("k", INT4), Column("b", TEXT), Column("a", INT4)],
                        rows: &[
                            &[T("UPDATE"), T("mp2"), T("15"), T("uno"), T("100")],
                            &[T("INSERT"), T("mp1"), T("2"), T("dos"), T("20")],
                            &[T("UPDATE"), T("mp2"), T("11"), T("once!"), T("1111")],
                            &[T("INSERT"), T("mp2"), T("12"), T("doce"), T("120")],
                        ],
                        tag: "MERGE 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM mp ORDER BY k, b;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("k", INT4), Column("b", TEXT), Column("a", INT4)],
                        rows: &[
                            &[T("mp1"), T("2"), T("dos"), T("20")],
                            &[T("mp2"), T("11"), T("once!"), T("1111")],
                            &[T("mp2"), T("12"), T("doce"), T("120")],
                            &[T("mp2"), T("15"), T("uno"), T("100")],
                        ],
                        tag: "SELECT 4",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "MERGE INTO mp t USING (VALUES (12, 'x', 5), (3, 'y', 6)) AS s(k, b, a) ON t.k = s.k WHEN MATCHED THEN UPDATE SET k = s.k - 10 WHEN NOT MATCHED THEN INSERT VALUES (s.k, s.b, s.a);",
                    expected: Expected::Tag("MERGE 2"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT tableoid::regclass, * FROM mp ORDER BY k, b;",
                    expected: Expected::Rows {
                        columns: &[Column("tableoid", REGCLASS), Column("k", INT4), Column("b", TEXT), Column("a", INT4)],
                        rows: &[
                            &[T("mp1"), T("2"), T("doce"), T("120")],
                            &[T("mp1"), T("2"), T("dos"), T("20")],
                            &[T("mp1"), T("3"), T("y"), T("6")],
                            &[T("mp2"), T("11"), T("once!"), T("1111")],
                            &[T("mp2"), T("15"), T("uno"), T("100")],
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
