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

//! Dolt's integration-tests/bats/rename-tables.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_rename_tables() {
    run_scripts(&[
        ScriptTest {
            name: r#"rename-tables: rename a table with sql"#,
            intent: r#"A renamed table goes by its new name only, its old name is free for a new table, and the new name is taken."#,
            set_up_script: &[
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test (pk int PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"ALTER TABLE test RENAME TO quiz;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    dolt: Some(Dolt::MySqlOnly("MySQL's ALTER TABLE ... RENAME quiz is Postgres' RENAME TO quiz")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tablename", NAME)],
                        rows: &[
                            &[T("quiz")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls is a CLI command, so the port lists the tables from pg_tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE test (pk int PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE quiz (pk int PRIMARY KEY);"#,
                    expected: Expected::Error(Diagnostic { code: "42P07", message: r#"relation "quiz" already exists"#, ..E }),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tablename", NAME)],
                        rows: &[
                            &[T("quiz")],
                            &[T("test")],
                        ],
                        tag: "SELECT 2",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls is a CLI command, so the port lists the tables from pg_tables")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"rename-tables: diff a renamed table"#,
            intent: r#"The diff of a committed table renamed in the working set shows one table renamed from its old name to its new one."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk int PRIMARY KEY);"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'added table test');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"ALTER TABLE test RENAME TO quiz;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT from_table_name, to_table_name, diff_type FROM dolt_diff_summary('HEAD', 'WORKING');"#,
                    expected: Expected::Rows {
                        columns: &[Column("from_table_name", TEXT), Column("to_table_name", TEXT), Column("diff_type", TEXT)],
                        rows: &[
                            &[T("public.test"), T("public.quiz"), T("renamed")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt diff is a CLI command, so the port reads dolt_diff_summary")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"rename-tables: sql diff a renamed table"#,
            intent: r#"The SQL diff of a renamed table is a statement that renames it."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk int PRIMARY KEY);"#,
                r#"SELECT dolt_add('test');"#,
                r#"SELECT dolt_commit('-m', 'added table test');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"ALTER TABLE test RENAME TO quiz;"#,
                    expected: Expected::Tag("ALTER TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT statement FROM dolt_patch('HEAD', 'WORKING');"#,
                    expected: Expected::Rows {
                        columns: &[Column("statement", TEXT)],
                        rows: &[
                            &[T("ALTER TABLE public.test RENAME TO quiz;")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt diff -r sql is a CLI command, so the port reads dolt_patch, whose statement is Postgres' ALTER TABLE ... RENAME TO")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"rename-tables: merge a renamed table"#,
            intent: r#"A merge applies the other branch's rows to a table this branch renamed."#,
            set_up_script: &[
                r#"CREATE TABLE test (pk int PRIMARY KEY);"#,
                r#"INSERT INTO test VALUES (0),(1);"#,
                r#"SELECT dolt_add('-A');"#,
                r#"SELECT dolt_commit('-m', 'created table test');"#,
                r#"SELECT * FROM dolt_checkout('-b', 'other');"#,
                r#"INSERT INTO test VALUES (8);"#,
                r#"SELECT dolt_add('-A');"#,
                r#"SELECT dolt_commit('-m', 'inserted some values on branch other');"#,
                r#"SELECT * FROM dolt_checkout('main');"#,
                r#"ALTER TABLE test RENAME TO quiz;"#,
                r#"INSERT INTO quiz VALUES (9);"#,
                r#"SELECT dolt_add('-A');"#,
                r#"SELECT dolt_commit('-m', 'renamed test to quiz, added values');"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"SELECT * FROM dolt_merge('other', '-m', 'merge');"#,
                    expected: Expected::Rows {
                        columns: &[Column("hash", TEXT), Column("fast_forward", INT8), Column("conflicts", INT8), Column("message", TEXT)],
                        rows: &[
                            &[Any, T("0"), T("0"), T("merge successful")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Bug("Dolt skips this test because its merge panics on a renamed table, and a merge should follow the rename")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT * FROM quiz ORDER BY pk;"#,
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("0")],
                            &[T("1")],
                            &[T("8")],
                            &[T("9")],
                        ],
                        tag: "SELECT 4",
                    },
                    dolt: Some(Dolt::Bug("Dolt skips this test because its merge panics on a renamed table, and a merge should follow the rename")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: r#"rename-tables: Reusing a table name should NOT fail"#,
            intent: r#"Names of dropped tables, committed or not, can be used again for new tables, which commit as usual."#,
            set_up_script: &[
                r#"CREATE TABLE one (pk int PRIMARY KEY);"#,
                r#"CREATE TABLE two (pk int PRIMARY KEY);"#,
                r#"SELECT dolt_add('-A');"#,
                r#"SELECT dolt_commit('-m', 'create test tables');"#,
                r#"DROP TABLE one;"#,
                r#"SELECT dolt_add('-A');"#,
                r#"SELECT dolt_commit('-m', 'dropped table one');"#,
                r#"DROP TABLE two;"#,
                r#"CREATE TABLE three (pk int PRIMARY KEY);"#,
                r#"DROP TABLE three;"#,
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: r#"CREATE TABLE one (pk int PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE two (pk int PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"CREATE TABLE three (pk int PRIMARY KEY);"#,
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT tablename FROM pg_tables WHERE schemaname = 'public' ORDER BY 1;"#,
                    expected: Expected::Rows {
                        columns: &[Column("tablename", NAME)],
                        rows: &[
                            &[T("one")],
                            &[T("three")],
                            &[T("two")],
                        ],
                        tag: "SELECT 3",
                    },
                    dolt: Some(Dolt::MySqlOnly("dolt ls is a CLI command, so the port lists the tables from pg_tables")),
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_add('-A');"#,
                    expected: Expected::Rows {
                        columns: &[Column("dolt_add", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: r#"SELECT dolt_commit('-m', 'commit recreated tables');"#,
                    expected: Expected::Ok,
                    ..A
                },
            ],
            ..S
        },
    ]);
}
