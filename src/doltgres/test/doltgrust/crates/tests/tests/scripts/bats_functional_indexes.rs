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

//! Dolt's integration-tests/bats/functional-indexes.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_functional_indexes() {
    run_scripts(&[
        ScriptTest {
            name: r#"functional-indexes: create index with multiple expressions mixed with a plain column"#,
            intent: r#"An index of expressions and a plain column answers queries on all of them and is what a lookup by them uses."#,
            set_up_script: &[
                r#"CREATE TABLE t (pk INT PRIMARY KEY, name VARCHAR(100), age INT, c1 INT, c2 INT);"#,
                r#"INSERT INTO t VALUES (1, 'alice', 30, 1, 2), (2, 'bob', 40, 3, 4);"#,
                r#"CREATE INDEX idx1 ON t ((UPPER(name)), age, (c1 + c2));"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT pk FROM t WHERE UPPER(name) = 'ALICE' AND age = 30 AND c1 + c2 = 3;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("1")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pk FROM t WHERE UPPER(name) = 'BOB' AND age = 40 AND c1 + c2 = 7;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("2")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SET enable_seqscan = off;"#,
                    expected: Expected::Tag("SET"),
                    dolt: Some(Dolt::MySqlOnly("Postgres reads a table this small sequentially unless told not to, so the port turns that off to show the index's use")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"EXPLAIN (COSTS OFF) SELECT pk FROM t WHERE UPPER(name) = 'ALICE' AND age = 30 AND c1 + c2 = 3;"#,
                    expected: Expected::Rows {
                        columns: &[Column("QUERY PLAN", TEXT)],
                        rows: &[
                            &[T("Index Scan using idx1 on t")],
                            &[T("  Index Cond: ((upper((name)::text) = 'ALICE'::text) AND (age = 30) AND ((c1 + c2) = 3))")],
                        ],
                        tag: "EXPLAIN",
                    },
                    dolt: Some(Dolt::MySqlOnly("the plan is Postgres', which shows the index by name instead of go-mysql-server's hidden columns")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"functional-indexes: hidden generated columns are not visible in schema output"#,
            intent: r#"An index of expressions shows them in its definition, and the table shows only its own columns."#,
            set_up_script: &[
                r#"CREATE TABLE t (pk INT PRIMARY KEY, c1 INT, c2 INT, c3 INT);"#,
                r#"CREATE UNIQUE INDEX idx1 ON t ((c1 * 10), c2, (c3 * 10));"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT pg_get_indexdef('idx1'::regclass);"#,
                    expected: Expected::Rows {
                        columns: &[Column("pg_get_indexdef", TEXT)],
                        rows: &[
                            &[T("CREATE UNIQUE INDEX idx1 ON public.t USING btree (((c1 * 10)), c2, ((c3 * 10)))")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt schema show is a CLI command, so the port reads the index's definition from pg_get_indexdef")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT attname FROM pg_attribute WHERE attrelid = 't'::regclass AND attnum > 0 ORDER BY attnum;"#,
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME)],
                        rows: &[
                            &[T("pk")],
                            &[T("c1")],
                            &[T("c2")],
                            &[T("c3")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL keeps an index's expressions in hidden columns, and Postgres keeps them in the index, so the table's columns stay its own")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"functional-indexes: UNIQUE constraint is enforced across all expressions"#,
            intent: r#"A unique index of expressions refuses a row whose values for all of them repeat another row's, and takes one that differs in any."#,
            set_up_script: &[
                r#"CREATE TABLE t (pk INT PRIMARY KEY, c1 INT, c2 INT, c3 INT);"#,
                r#"CREATE UNIQUE INDEX idx1 ON t ((c1 * 10), c2, (c3 * 10));"#,
                r#"INSERT INTO t VALUES (1, 1, 2, 3);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (2, 1, 2, 3);"#,
                    expected: Expected::Error(Diagnostic { code: "23505", message: r#"duplicate key value violates unique constraint "idx1""#, detail: "Key ((c1 * 10), c2, (c3 * 10))=(10, 2, 30) already exists.", schema: "public", table: "t", constraint: "idx1", ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"INSERT INTO t VALUES (3, 1, 3, 3);"#,
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"functional-indexes: DROP INDEX removes only its own hidden columns"#,
            intent: r#"Dropping one index of expressions leaves the other index and the table's columns as they were."#,
            set_up_script: &[
                r#"CREATE TABLE t (pk INT PRIMARY KEY, c1 INT, c2 INT, c3 INT);"#,
                r#"INSERT INTO t VALUES (1, 10, 20, 30);"#,
                r#"CREATE INDEX idx2 ON t ((c2 * 100));"#,
                r#"CREATE INDEX idx1 ON t ((c1 * 10), c2, (c3 * 10));"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT attname FROM pg_attribute WHERE attrelid = 't'::regclass AND attnum > 0 ORDER BY attnum;"#,
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME)],
                        rows: &[
                            &[T("pk")],
                            &[T("c1")],
                            &[T("c2")],
                            &[T("c3")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL keeps an index's expressions in hidden columns, and Postgres keeps them in the index, so the table's columns stay its own")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP INDEX idx1;"#,
                    expected: Expected::Tag("DROP INDEX"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's DROP INDEX idx1 ON t is Postgres' DROP INDEX idx1")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT indexname FROM pg_indexes WHERE tablename = 't' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("indexname", NAME)],
                        rows: &[
                            &[T("idx2")],
                            &[T("t_pkey")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL keeps an index's expressions in hidden columns, and Postgres keeps them in the index, so the table's columns stay its own")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT attname FROM pg_attribute WHERE attrelid = 't'::regclass AND attnum > 0 ORDER BY attnum;"#,
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME)],
                        rows: &[
                            &[T("pk")],
                            &[T("c1")],
                            &[T("c2")],
                            &[T("c3")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL keeps an index's expressions in hidden columns, and Postgres keeps them in the index, so the table's columns stay its own")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"DROP INDEX idx2;"#,
                    expected: Expected::Tag("DROP INDEX"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT attname FROM pg_attribute WHERE attrelid = 't'::regclass AND attnum > 0 ORDER BY attnum;"#,
                    expected: Expected::Rows {
                        columns: &[Column("attname", NAME)],
                        rows: &[
                            &[T("pk")],
                            &[T("c1")],
                            &[T("c2")],
                            &[T("c3")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::MySqlOnly("MySQL keeps an index's expressions in hidden columns, and Postgres keeps them in the index, so the table's columns stay its own")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"functional-indexes: DROP COLUMN blocked for columns referenced by an expression"#,
            intent: r#"Dropping a column an index of expressions uses drops the index with it, as Postgres does."#,
            set_up_script: &[
                r#"CREATE TABLE t (pk INT PRIMARY KEY, c1 INT, c2 INT, c3 INT);"#,
                r#"CREATE INDEX idx1 ON t ((c1 * 10), c2, (c3 * 10));"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"ALTER TABLE t DROP COLUMN c1;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::MySqlOnly("Postgres drops an index along with a column it uses, where MySQL refuses to drop the column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT count(*) FROM pg_indexes WHERE indexname = 'idx1';"#,
                    expected: Expected::Rows {
                        columns: &[Column("count", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("Postgres drops an index along with a column it uses, where MySQL refuses to drop the column")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE t DROP COLUMN c3;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::MySqlOnly("Postgres drops an index along with a column it uses, where MySQL refuses to drop the column")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"functional-indexes: survives dolt commit and diff"#,
            intent: r#"A table with an index of expressions commits, diffs its new rows, and answers lookups through the index after commits."#,
            set_up_script: &[
                r#"CREATE TABLE t (pk INT PRIMARY KEY, name VARCHAR(100), age INT, c1 INT, c2 INT);"#,
                r#"CREATE INDEX idx1 ON t ((UPPER(name)), age, (c1 + c2));"#,
                r#"SELECT dolt_add('.');"#,
                r#"SELECT dolt_commit('-m', 'create table with multi-expression functional index');"#,
                r#"INSERT INTO t VALUES (1, 'alice', 30, 1, 2);"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT to_name, dolt_diff_type FROM dolt_diff('HEAD', 'WORKING', 't');"#,
                    expected: Expected::Rows {
                        columns: &[Column("to_name", VARCHAR), Column("dolt_diff_type", TEXT)],
                        rows: &[
                            &[T("alice"), T("added")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt diff is a CLI command, so the port reads dolt_diff")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('.');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'insert row');"#,
                    expected: Expected::Ok,
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT pk FROM t WHERE UPPER(name) = 'ALICE' AND age = 30 AND c1 + c2 = 3;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
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
