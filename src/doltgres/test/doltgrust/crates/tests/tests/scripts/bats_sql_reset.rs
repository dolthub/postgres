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

//! Dolt's integration-tests/bats/sql-reset.bats, ported at Dolt 614771d742.

use harness::oid::*;
use harness::script::Cell::{Any, Null, Text as T};
use harness::script::{A, Column, Diagnostic, Dolt, E, Expected, N, S, ScriptTest, ScriptTestAssertion, run_scripts};

#[test]
fn test_sql_reset() {
    run_scripts(&[
        ScriptTest {
            name: "sql-reset: DOLT_RESET --hard works on unstaged and staged table changes",
            intent: "A hard reset discards unstaged and staged changes, and so does one to head.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'Add a table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
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
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard', 'head');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-reset: CALL DOLT_RESET --hard does not ignore staged docs",
            intent: "A hard reset discards staged new and changed docs.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'Add a table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_docs VALUES ('LICENSE.md', '~license~');",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::MySqlOnly("dolt docs upload writes the file into dolt_docs")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
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
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_docs VALUES ('LICENSE.md', '~license~');",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::MySqlOnly("dolt docs upload writes the file into dolt_docs")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
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
                    query: "SELECT dolt_commit('-a', '-m', 'Add a the license file');",
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
                    query: "UPDATE dolt_docs SET doc_text = '~edited-license~' WHERE doc_name = 'LICENSE.md';",
                    expected: Expected::Tag("UPDATE 1"),
                    dolt: Some(Dolt::MySqlOnly("dolt docs upload writes the file into dolt_docs")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
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
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-reset: CALL DOLT_RESET --soft works on unstaged and staged table changes",
            intent: "A soft reset leaves unstaged changes unstaged and staged changes staged.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'Add a table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--soft');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
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
                    query: "SELECT dolt_reset('--soft');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("t"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-reset: CALL DOLT_RESET --soft ignores staged docs",
            intent: "A soft reset leaves staged new docs staged.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'Add a table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO dolt_docs VALUES ('LICENSE.md', '~license~');",
                    expected: Expected::Tag("INSERT 0 1"),
                    dolt: Some(Dolt::MySqlOnly("dolt docs upload writes the file into dolt_docs")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
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
                    query: "SELECT dolt_reset('--soft');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("dolt_docs"), T("t"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-reset: CALL DOLT_RESET works on specific tables",
            intent: "A reset of named tables unstages them.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'Add a table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('test');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "CREATE TABLE test2 (pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
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
                    query: "SELECT dolt_reset('test', 'test2');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("f"), T("modified")],
                            &[T("public.test2"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-reset: CALL DOLT_RESET --soft and --hard on the same table",
            intent: "A table reset unstages a change and keeps its rows, and a hard reset then discards them.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'Add a table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('test');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
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
                    query: "SELECT dolt_reset('test');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
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
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-reset: CALL DOLT_RESET('--hard') doesn't remove newly created table.",
            intent: "A hard reset keeps an untracked new table, and drops it once it was staged.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'Add a table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "CREATE TABLE test2 (pk int PRIMARY KEY);",
                    expected: Expected::Tag("CREATE TABLE"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test2"), T("f"), T("new table")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
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
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT active_branch();",
                    expected: Expected::Rows {
                        columns: &[Column("active_branch", TEXT)],
                        rows: &[
                            &[T("main")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                        ],
                        tag: "SELECT 0",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-reset: No rows in dolt_diff table after DOLT_RESET('--hard') on committed table.",
            intent: "After a hard reset dolt_diff_<t> shows no working change.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'Add a table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) = 0 FROM dolt_diff_test;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
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
        ScriptTest {
            name: "sql-reset: No rows in dolt_status table after DOLT_RESET('--hard') on committed table.",
            intent: "After a hard reset dolt_status is empty.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'Add a table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT count(*) = 0 FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
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
        ScriptTest {
            name: "sql-reset: CALL DOLT_RESET --hard properly maintains session variables.",
            intent: "A hard reset leaves the session's head on the same commit.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'Add a table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('test.head', dolt_hashof('HEAD'), FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof('HEAD') = current_setting('test.head');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Postgres has no system variables named after a database, so dolt_hashof and dolt_hashof_db read the hashes of @@<db>_head and @@<db>_working, Appendix C \"per-database session variables\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-reset: dolt_status still has the same information in the face of a DOLT_RESET",
            intent: "A table reset of an unstaged change leaves it unstaged.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'Add a table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('test');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("public.test"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-reset: CALL DOLT_RESET soft maintains staged session variable",
            intent: "Resetting a staged change moves the working root, and a hard reset brings it back.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'Add a table');",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT set_config('test.working', dolt_hashof_db('WORKING'), FALSE);",
                    expected: Expected::Rows {
                        columns: &[Column("set_config", TEXT)],
                        rows: &[
                            &[Any],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "INSERT INTO test VALUES (1);",
                    expected: Expected::Tag("INSERT 0 1"),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_add('.');",
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
                    query: "SELECT dolt_reset('test');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof_db('WORKING') = current_setting('test.working');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("f")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Postgres has no system variables named after a database, so dolt_hashof and dolt_hashof_db read the hashes of @@<db>_head and @@<db>_working, Appendix C \"per-database session variables\"")),
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_hashof_db('WORKING') = current_setting('test.working');",
                    expected: Expected::Rows {
                        columns: &[Column("?column?", BOOL)],
                        rows: &[
                            &[T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    dolt: Some(Dolt::Differs("Postgres has no system variables named after a database, so dolt_hashof and dolt_hashof_db read the hashes of @@<db>_head and @@<db>_working, Appendix C \"per-database session variables\"")),
                    ..A
                },
            ],
            ..S
        },
        ScriptTest {
            name: "sql-reset: reset handles ignored tables",
            intent: "A hard reset keeps an ignored table and its rows, which dolt_status_ignored lists.",
            set_up_script: &[
                "CREATE TABLE test (pk int PRIMARY KEY);",
                "SELECT dolt_add('.');",
                "SELECT dolt_commit('-a', '-m', 'Add a table');",
                "CREATE TABLE test2 (pk int PRIMARY KEY);",
                "INSERT INTO test2 VALUES (9);",
                "INSERT INTO test VALUES (1);",
                "INSERT INTO dolt_ignore VALUES ('test2', TRUE);",
            ],
            assertions: &[
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status;",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT)],
                        rows: &[
                            &[T("dolt_ignore"), T("f"), T("new table")],
                            &[T("public.test"), T("f"), T("modified")],
                        ],
                        tag: "SELECT 2",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT dolt_reset('--hard');",
                    expected: Expected::Rows {
                        columns: &[Column("dolt_reset", INT8)],
                        rows: &[
                            &[T("0")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM dolt_status_ignored WHERE table_name = 'public.test2';",
                    expected: Expected::Rows {
                        columns: &[Column("table_name", TEXT), Column("staged", BOOL), Column("status", TEXT), Column("ignored", BOOL)],
                        rows: &[
                            &[T("public.test2"), T("f"), T("new table"), T("t")],
                        ],
                        tag: "SELECT 1",
                    },
                    ..A
                },
                ScriptTestAssertion {
                    query: "SELECT * FROM test2;",
                    expected: Expected::Rows {
                        columns: &[Column("pk", INT4)],
                        rows: &[
                            &[T("9")],
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
